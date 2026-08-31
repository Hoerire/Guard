#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <stdarg.h>

/* ============ 常量 ============ */
#define MAX_PATH   512
#define MAX_LINE   4096
#define MAX_PKG    256
#define MAX_CONFIG 8192
#define MAX_TARGET 64
#define MAX_FREEZE 64

/* ============ 全局 ============ */
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t cleanup_pending = 0;
static char config_path[MAX_PATH];
static char log_path[MAX_PATH];
static char pid_path[MAX_PATH];
static int  log_fd = -1;

/* ============ 日志 ============ */
static void log_msg(const char *fmt, ...) {
    char buf[8192];
    int off = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    off += snprintf(buf + off, sizeof(buf) - off,
        "[%02d-%02d %02d:%02d:%02d.%03ld] ",
        tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        ts.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt);
    int want = vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    if (want > 0 && (size_t)want >= sizeof(buf) - off) want = sizeof(buf) - off - 1;
    off += want;
    if (off > 0 && buf[off-1] != '\n') buf[off++] = '\n';
    if (log_fd >= 0) {
        size_t n = off, p = 0;
        while (n > 0) {
            ssize_t w = write(log_fd, buf + p, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            p += w; n -= w;
        }
    }
}

static void write_pidfile(void) {
    int fd = open(pid_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        write(fd, buf, n);
        close(fd);
    }
}
static void remove_pidfile(void) { unlink(pid_path); }

static void signal_handler(int sig) {
    if (sig == SIGUSR1) cleanup_pending = 1;
    else if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP) running = 0;
}

/* ============ 配置 ============ */
typedef struct { char name[MAX_PKG]; } Package;
typedef struct {
    Package target[MAX_TARGET];  size_t target_count;
    Package freeze[MAX_FREEZE];  size_t freeze_count;
    int  appuid;
} Config;

static Config cfg;
static char current_fg[MAX_PKG] = {0};
static time_t config_mtime = 0;

static char *find_sep(char *line) {
    char *c1 = strchr(line, ':');
    char *c2 = strchr(line, '=');
    if (c1 && c2) return (c1 < c2) ? c1 : c2;
    return c1 ? c1 : c2;
}

static bool reload_if_changed(void) {
    struct stat st;
    if (stat(config_path, &st) < 0) return false;
    if (st.st_mtime == config_mtime) return false;
    config_mtime = st.st_mtime;

    int fd = open(config_path, O_RDONLY);
    if (fd < 0) return false;
    char buf[MAX_CONFIG];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    Config new_cfg = {0};
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') { line = strtok_r(NULL, "\n", &saveptr); continue; }

        char *sep = find_sep(line);
        if (sep) {
            char *key = line;  char *val = sep + 1;  *sep = '\0';
            while (*key == ' ' || *key == '\t') key++;
            char *ke = key + strlen(key) - 1;
            while (ke > key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
            while (*val == ' ' || *val == '\t' || *val == '\r') val++;
            char *ve = val + strlen(val) - 1;
            while (ve > val && (*ve == ' ' || *ve == '\t' || *ve == '\r')) *ve-- = '\0';

            if (strcmp(key, "appuid") == 0) {
                new_cfg.appuid = atoi(val);
            } else if (strcmp(key, "target") == 0 && new_cfg.target_count < MAX_TARGET) {
                strncpy(new_cfg.target[new_cfg.target_count].name, val, MAX_PKG - 1);
                new_cfg.target_count++;
            } else if (strcmp(key, "freeze") == 0 && new_cfg.freeze_count < MAX_FREEZE) {
                strncpy(new_cfg.freeze[new_cfg.freeze_count].name, val, MAX_PKG - 1);
                new_cfg.freeze_count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    cfg = new_cfg;
    log_msg("[配置变更] 热重载：目标 %zu 个，禁用 %zu 个\n",
            cfg.target_count, cfg.freeze_count);
    return true;
}

static bool comp_matches(const char *comp, const Package *list, size_t count) {
    if (!comp) return false;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(comp, list[i].name, strlen(list[i].name)) == 0)
            return true;
    }
    return false;
}

static void process_line(char *line) {
    if (!strstr(line, "wm_on_resume_called") && !strstr(line, "wm_on_top_resumed_gained_called"))
        return;

    reload_if_changed();

    const char *brace = strchr(line, '{');
    const char *end = brace ? strchr(brace + 1, '}') : NULL;
    const char *pkg_start = NULL;

    if (brace && end) {
        const char *last_space = NULL;
        for (const char *q = brace + 1; q < end; q++) if (*q == ' ') last_space = q;
        pkg_start = last_space ? last_space + 1 : brace + 1;
    }
    if (!pkg_start || !*pkg_start) return;

    char comp[MAX_PKG];
    size_t pkg_len = end ? (size_t)(end - pkg_start) : strlen(pkg_start);
    if (pkg_len >= sizeof(comp)) pkg_len = sizeof(comp) - 1;
    strncpy(comp, pkg_start, pkg_len);
    comp[pkg_len] = '\0';
    char *slash = strchr(comp, '/');
    if (slash) *slash = '\0';

    if (!comp[0] || strcmp(comp, current_fg) == 0) return;

    bool target = comp_matches(comp, cfg.target, cfg.target_count);
    strncpy(current_fg, comp, sizeof(current_fg) - 1);
    log_msg("[前台事件] %s %s\n", comp, target ? "【目标应用】" : "【普通应用】");
}

/* ============ Fork+exec logcat (fork 前零堆分配) ============ */
static pid_t fork_logcat(int out_fd) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        /* 子进程：直接 exec logcat，不做任何额外操作 */
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        const char *argv[] = { "logcat", "-b", "events", "-v", "brief", "-T", "1",
            "wm_on_resume_called:V", "wm_on_top_resumed_gained_called:V", "*:S", NULL };
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    return p;
}

/* ============ main ============ */
int main(int argc, char **argv) {
    /* ===== 步骤 1: 重定向 stdin/stdout/stderr 到 /dev/null (syscalls only) ===== */
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }

    /* ===== 步骤 2: 确定路径 (snprintf 写栈，不碰堆) ===== */
    if (argc >= 2 && argv[1][0] == '/') {
        strncpy(config_path, argv[1], sizeof(config_path) - 1);
    } else {
        const char *pkg = getenv("GUARD_PKG");
        if (!pkg) pkg = "com.example.guard";
        snprintf(config_path, sizeof(config_path),
                 "/data/user/0/%s/files/config.txt", pkg);
    }
    /* 手动取 config.txt 所在目录，拼出 log_path */
    {
        char *last_slash = strrchr(config_path, '/');
        if (last_slash) {
            size_t dlen = last_slash - config_path;
            if (dlen >= MAX_PATH) dlen = MAX_PATH - 1;
            char dir[MAX_PATH];
            memcpy(dir, config_path, dlen);
            dir[dlen] = '\0';
            snprintf(log_path, sizeof(log_path), "%s/guard.log", dir);
        }
    }
    snprintf(pid_path, sizeof(pid_path), "%s.pid", config_path);

    /* ===== 步骤 2.5: 设置进程名为 "Guard"，必须在 fork 前 =====
       prctl 改 comm 名 (/proc/pid/status Name)
       argv[0] 改命令名 (ps / top)
       这样 Java 端 aliveByPid() 用 "Guard" 就能匹配到 */
    prctl(PR_SET_NAME, "Guard", 0, 0, 0);
    if (argv && argv[0]) {
        /* argv[0] 可安全覆写：原字符串 > 6 字节，"Guard" 只有 6 字节 */
        argv[0][0] = 'G'; argv[0][1] = 'u'; argv[0][2] = 'a';
        argv[0][3] = 'r'; argv[0][4] = 'd'; argv[0][5] = '\0';
    }

    /* ===== 步骤 3: 创建 pipe (syscall) ===== */
    int pipefd[2];
    if (pipe(pipefd) < 0) return 1;

    /* ===== 步骤 4: FORK (此时 guard 只执行了 syscalls + 栈变量，零堆分配) ===== */
    pid_t lc_pid = fork_logcat(pipefd[1]);
    close(pipefd[1]);  /* 父关写端 */
    int lc_fd = pipefd[0];
    if (lc_pid < 0) { close(lc_fd); return 1; }

    /* ===== 步骤 5: 以下可以安全碰堆了（fork 已完成，子进程已 exec） ===== */

    /* 打开日志 */
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    /* 安装信号 */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);

    reload_if_changed();
    write_pidfile();
    atexit(remove_pidfile);

    log_msg("[Guard] 启动 PID=%d\n", (int)getpid());
    log_msg("[信息] logcat 监听已启动 PID=%d\n", (int)lc_pid);

    /* 主循环 */
    char line[MAX_LINE];
    size_t used = 0;

    while (running) {
        reload_if_changed();
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，Java 端应执行清理\n");
        }

        struct pollfd pfd = { .fd = lc_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);

        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        char buf[512];
        ssize_t n = read(lc_fd, buf, sizeof(buf));
        if (n <= 0) {
            /* logcat 退出 → 重启 */
            log_msg("[警告] logcat 退出，重启中…\n");
            close(lc_fd);
            int new_pipe[2];
            if (pipe(new_pipe) < 0) break;
            waitpid(lc_pid, NULL, WNOHANG);
            lc_pid = fork_logcat(new_pipe[1]);
            close(new_pipe[1]);
            lc_fd = new_pipe[0];
            if (lc_pid < 0) {
                log_msg("[错误] 无法重启 logcat\n");
                break;
            }
            continue;
        }

        if (used + n >= sizeof(line)) { used = 0; }
        memcpy(line + used, buf, n);
        used += n;

        char *nl;
        while ((nl = memchr(line, '\n', used)) != NULL) {
            *nl = '\0';
            process_line(line);
            size_t consumed = nl - line + 1;
            used -= consumed;
            if (used > 0) memmove(line, line + consumed, used);
        }
    }

    log_msg("[Guard] 退出\n");
    remove_pidfile();
    if (lc_fd >= 0) close(lc_fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
