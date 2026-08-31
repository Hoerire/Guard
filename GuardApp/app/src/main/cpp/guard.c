#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <grp.h>

/* ============ 常量 ============ */
#define MAX_PATH         512
#define MAX_LINE         2048
#define MAX_PKG          256
#define MAX_CONFIG       8192
#define MAX_OUTPUT       (64 * 1024)
#define MAX_TARGET       64
#define MAX_FREEZE       64

/* ============ 全局状态 ============ */
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t cleanup_pending = 0;
static char config_path[MAX_PATH];
static char log_path[MAX_PATH];
static int  log_fd = -1;

/* ============ 工具函数（全栈缓冲，零堆分配）============ */
static void raw_write(int fd, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        s += w; n -= w;
    }
}

static void log_msg(const char *fmt, ...) {
    char buf[MAX_OUTPUT];
    int off = 0;
    /* 时间戳（手动格式化，不走 strftime）*/
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    off += snprintf(buf + off, sizeof(buf) - off,
        "[%02d-%02d %02d:%02d:%02d.%03ld] ",
        tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        ts.tv_nsec / 1000000);
    /* 消息体 */
    va_list ap; va_start(ap, fmt);
    int want = vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    if (want > 0 && (size_t)want >= sizeof(buf) - off) want = sizeof(buf) - off - 1;
    off += want;
    if (off > 0 && buf[off-1] != '\n') buf[off++] = '\n';
    /* 先写 stderr（调试用），再写日志文件 */
    write(2, buf, off);
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
    char pid_path[MAX_PATH];
    snprintf(pid_path, sizeof(pid_path), "%s.guard_pid", config_path);
    int fd = open(pid_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        write(fd, buf, n);
        close(fd);
    }
}

static void remove_pidfile(void) {
    char pid_path[MAX_PATH];
    snprintf(pid_path, sizeof(pid_path), "%s.guard_pid", config_path);
    unlink(pid_path);
}

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
        char *eq = strchr(line, '=');
        if (eq) {
            char *key = line;  char *val = eq + 1;  *eq = '\0';
            while (*key == ' ' || *key == '\t') key++;
            while (*val == ' ' || *val == '\t' || *val == '\r') val++;
            char *end = val + strlen(val) - 1;
            while (end > val && (*end == ' ' || *end == '\t' || *end == '\r')) *end-- = '\0';

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

static bool extract_component(const char *line, char *out, size_t out_sz) {
    /* 从 logcat 事件中提取包名：wm_on_resume_called: ActivityRecord{... com.xxx/.yyy} */
    const char *tag = "wm_on_resume_called:";
    const char *tag2 = "wm_on_top_resumed_gained_called:";
    const char *p = strstr(line, tag);
    if (!p) p = strstr(line, tag2);
    if (!p) return false;
    p = strchr(p, '{');
    if (!p) return false;
    const char *end = strchr(p + 1, '}');
    if (!end) return false;
    /* 找最后一个空格后面的 "com.xxx/.yyy" */
    const char *last_space = NULL;
    for (const char *q = p + 1; q < end; q++) if (*q == ' ') last_space = q;
    if (!last_space) return false;
    const char *pkg = last_space + 1;
    size_t len = end - pkg;
    if (len >= out_sz) len = out_sz - 1;
    strncpy(out, pkg, len);
    out[len] = '\0';
    /* 去掉 Activity 后缀，只留包名 */
    char *slash = strchr(out, '/');
    if (slash) *slash = '\0';
    return len > 0;
}

/* ============ 前台事件处理（只记日志 + 信号，不执行任何命令）============ */
static void handle_event(const char *line) {
    if (!line) return;
    if (!strstr(line, "wm_on_resume_called") && !strstr(line, "wm_on_top_resumed_gained_called"))
        return;

    char comp[MAX_PKG];
    if (!extract_component(line, comp, sizeof(comp))) return;
    reload_if_changed();

    if (strcmp(current_fg, comp) == 0 && !reload_if_changed()) return;
    strncpy(current_fg, comp, sizeof(current_fg) - 1);

    bool target = comp_matches(comp, cfg.target, cfg.target_count);
    log_msg("[前台事件] %s %s\n", comp, target ? "【目标应用】" : "【普通应用】");
    /* 到此为止！pm disable/enable 和 cleanup 全部由 Java 端自己处理 */
}

/* ============ logcat 事件读取 ============ */
static pid_t start_logcat(int *read_fd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        close(pipefd[1]);
        const char *argv[] = { "logcat", "-b", "events", "-v", "brief", "-T", "1",
            "wm_on_resume_called:V", "wm_on_top_resumed_gained_called:V", "*:S", NULL };
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    *read_fd = pipefd[0];
    return pid;
}

/* ============ main ============ */
int main(int argc, char **argv) {
    /* exec self 切断父进程污染 */
    const char *tag = "GUARD_DEMONIZED";
    if (!getenv(tag) || strcmp(getenv(tag), "1") != 0) {
        pid_t p = fork();
        if (p < 0) { write(2, "[Guard] fork fail\n", 18); return 1; }
        if (p > 0) _exit(0);
        setenv("SCUDO_OPTIONS", "strictness=0", 1);
        setenv(tag, "1", 1);
        setsid();
        execve("/proc/self/exe", argv, environ);
        /* exec 失败兜底：double-fork */
        pid_t p2 = fork();
        if (p2 < 0) _exit(1);
        if (p2 > 0) _exit(0);
    }

    /* 重定向 stdin/stdout 到 /dev/null，stderr 保留给 crash_handler */
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); close(dn); }

    /* 初始化路径 */
    const char *pkg = getenv("GUARD_PKG");
    if (!pkg) pkg = "com.example.guard";
    snprintf(config_path, sizeof(config_path), "/data/user/0/%s/files/config.txt", pkg);
    snprintf(log_path, sizeof(log_path), "/data/user/0/%s/files/guard.log", pkg);

    /* 打开日志文件（O_APPEND 保证原子追加）*/
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    /* 安装信号 */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);

    /* 读配置（首次）*/
    reload_if_changed();

    /* 写 pidfile */
    write_pidfile();
    atexit(remove_pidfile);

    log_msg("[Guard] 启动 PID=%d\n", (int)getpid());

    /* 启动 logcat */
    int lc_fd = -1;
    pid_t lc_pid = start_logcat(&lc_fd);
    if (lc_pid < 0) {
        log_msg("[错误] 无法启动 logcat\n");
        return 1;
    }
    log_msg("[信息] logcat events 监听已启动 PID=%d\n", (int)lc_pid);

    /* 主循环：单线程 poll，零 fork，零堆分配 */
    char line[MAX_LINE];
    size_t used = 0;

    while (running) {
        struct pollfd pfd = { .fd = lc_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);

        /* 周期性检查配置变化 */
        reload_if_changed();

        /* SIGUSR1 触发的清理（守护只记日志，实际清理由 Java 端做）*/
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，Java 端应执行清理\n");
        }

        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;  /* 超时，继续 */

        if (!(pfd.revents & POLLIN)) continue;

        /* 读 logcat 输出 */
        char buf[512];
        ssize_t n = read(lc_fd, buf, sizeof(buf));
        if (n <= 0) {
            /* logcat 退出，重启 */
            log_msg("[警告] logcat 退出，重启中…\n");
            close(lc_fd);
            waitpid(lc_pid, NULL, WNOHANG);
            lc_pid = start_logcat(&lc_fd);
            if (lc_pid < 0) {
                log_msg("[错误] 无法重启 logcat\n");
                break;
            }
            continue;
        }

        /* 拼到行缓冲 */
        if (used + n >= sizeof(line)) { used = 0; }  /* 防溢出 */
        memcpy(line + used, buf, n);
        used += n;

        /* 按 \n 切行处理 */
        char *nl;
        while ((nl = memchr(line, '\n', used)) != NULL) {
            *nl = '\0';
            handle_event(line);
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
