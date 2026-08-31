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
#define MAX_PATH    512
#define MAX_LINE    4096
#define MAX_PKG     256
#define MAX_CONFIG  8192
#define MAX_TARGET  64
#define MAX_FREEZE  64
#define MAX_LOGCAT  12  /* events + events_0..events_6 + main + system */

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

/* 从一行 logcat 中提取包名，支持多种格式：
   - events buffer: "wm_on_resume_called: [0xabc, com.example/.MainActivity, ...]" → {} 内最后一个字段
   - main/system:    "Displayed com.example/.MainActivity" → "Displayed " 后的第一个字符串
                    "Start proc com.example for activity com.example/.MainActivity" → activity 后的组件
*/
static bool extract_pkg(const char *line, char *out, size_t out_size) {
    /* 格式 1: events buffer 花括号格式 — wm_on_resume_called / wm_on_top_resumed_gained_called
       事件参数都在 [...] 或 {...} 中，最后一个字段是组件名 */
    const char *brace = strchr(line, '{');
    const char *close = brace ? strchr(brace + 1, '}') : NULL;
    if (brace && close) {
        /* 跳过中间空格，取花括号内最后一个 token */
        const char *p = close - 1;
        while (p > brace && (*p == ' ' || *p == ',' || *p == ']')) p--;
        const char *end = p + 1;
        while (p > brace && *p != ' ' && *p != ',' && *p != '[') p--;
        if (*p == ' ' || *p == ',' || *p == '[') p++;
        size_t len = end - p;
        if (len > 0 && len < out_size) {
            memcpy(out, p, len);
            out[len] = '\0';
            char *slash = strchr(out, '/');
            if (slash) *slash = '\0';
            return out[0] != '\0';
        }
    }

    /* 格式 2: "Displayed com.example/.MainActivity ..." (ActivityTaskManager) */
    const char *d = strstr(line, "Displayed ");
    if (!d) d = strstr(line, "Displayed: ");
    if (d) {
        d += 10; /* 跳过 "Displayed " */
        while (*d == ' ') d++;
        size_t len = 0;
        while (d[len] && d[len] != ' ' && d[len] != '\n' && d[len] != '\r') len++;
        if (len > 0 && len < out_size) {
            memcpy(out, d, len);
            out[len] = '\0';
            char *slash = strchr(out, '/');
            if (slash) *slash = '\0';
            return out[0] != '\0';
        }
    }

    /* 格式 3: "TopResumedActivity com.example/.MainActivity" (dumpsys/trace) */
    const char *t = strstr(line, "TopResumedActivity");
    if (t) {
        t += 18;
        while (*t == ' ' || *t == '=') t++;
        size_t len = 0;
        while (t[len] && t[len] != ' ' && t[len] != '\n') len++;
        if (len > 0 && len < out_size) {
            memcpy(out, t, len);
            out[len] = '\0';
            char *slash = strchr(out, '/');
            if (slash) *slash = '\0';
            return out[0] != '\0';
        }
    }

    /* 格式 4: "activity com.example/.MainActivity" (Start proc ... for activity ...) */
    const char *a = strstr(line, " activity ");
    if (!a) a = strstr(line, " activity=");
    if (a) {
        /* 找最后一个 "activity "（Start proc 行里可能有两个 com.example） */
        const char *last = NULL;
        const char *scan = line;
        while ((a = strstr(scan, "activity ")) != NULL) { last = a; scan = a + 9; }
        if (!last) scan = strstr(line, "activity=");
        if (last) {
            last += 9;
            while (*last == ' ') last++;
            size_t len = 0;
            while (last[len] && last[len] != ' ' && last[len] != '\n' && last[len] != '/' && last[len] != ')') len++;
            if (len > 0 && len < out_size) {
                memcpy(out, last, len);
                out[len] = '\0';
                return out[0] != '\0';
            }
        }
    }

    return false;
}

static void process_line(char *line) {
    /* 事件名匹配：覆盖 events buffer 和 main/system buffer 的常见事件 */
    bool is_fg_event =
        strstr(line, "wm_on_resume_called") ||
        strstr(line, "wm_on_top_resumed_gained_called") ||
        strstr(line, "wm_on_paused_called") ||
        (strstr(line, "ActivityTaskManager") && (strstr(line, "Displayed") || strstr(line, "TopResumedActivity"))) ||
        (strstr(line, "ActivityManager") && strstr(line, "Displayed"));

    if (!is_fg_event) return;

    char comp[MAX_PKG] = {0};
    if (!extract_pkg(line, comp, sizeof(comp))) return;
    if (!comp[0] || strcmp(comp, current_fg) == 0) return;

    reload_if_changed();

    bool target = comp_matches(comp, cfg.target, cfg.target_count);
    strncpy(current_fg, comp, sizeof(current_fg) - 1);
    log_msg("[前台事件] %s %s\n", comp, target ? "【目标应用】" : "【普通应用】");
}

/* ============ Fork+exec logcat (fork 前零堆分配) ============ */
static pid_t fork_logcat(int out_fd, const char *buffer) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        /* 动态构造 argv：logcat -b <buffer> -v raw -T 1 -s ActivityManager:D *:S
           - events/main/system buffer 里 ActivityManager 事件覆盖前台切换
           - D (Debug) 级别覆盖更广，兼容不同 ROM 的日志级别设置
           - -v raw 输出原始格式，便于代码 strstr 过滤事件名
           - -T 1 跳过历史，只看新事件 */
        const char *argv[12];
        int a = 0;
        argv[a++] = "logcat";
        argv[a++] = "-b";
        argv[a++] = buffer;
        argv[a++] = "-v";
        argv[a++] = "raw";
        argv[a++] = "-T";
        argv[a++] = "1";
        argv[a++] = "-s";
        argv[a++] = "ActivityManager:D";
        argv[a++] = "ActivityTaskManager:I";
        argv[a++] = "*:S";
        argv[a] = NULL;
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

    /* ===== 步骤 2.5: 设置进程名为 "Guard"（纯 syscall + 栈操作） ===== */
    prctl(PR_SET_NAME, "Guard", 0, 0, 0);
    if (argv && argv[0]) {
        argv[0][0]='G'; argv[0][1]='u'; argv[0][2]='a';
        argv[0][3]='r'; argv[0][4]='d'; argv[0][5]='\0';
    }

    /* ===== 步骤 3: 创建多个 pipe + FORK 多个 logcat =====
       所有 fork() 必须在堆分配器被触碰之前！
       events buffer 在多核设备上被分成 events + events_0..events_N
    */
    struct { int fd; pid_t pid; } pipes[MAX_LOGCAT];
    int pipe_count = 0;

    /* 尝试的 buffer 名列表
       - events / events_0..events_6：Android events buffer（前台事件主要来源）
       - main / system：部分 ROM 把 ActivityManager 事件放这里 */
    static const char *buffers[] = {
        "events", "events_0", "events_1", "events_2", "events_3",
        "events_4", "events_5", "events_6",
        "main", "system", NULL
    };

    for (int i = 0; buffers[i] != NULL && pipe_count < MAX_LOGCAT; i++) {
        int pfd[2];
        if (pipe(pfd) < 0) continue;
        pid_t lp = fork_logcat(pfd[1], buffers[i]);
        close(pfd[1]);
        if (lp < 0) { close(pfd[0]); continue; }
        pipes[pipe_count].fd = pfd[0];
        pipes[pipe_count].pid = lp;
        pipe_count++;
    }

    /* ===== 步骤 4: 以下可以安全碰堆了（所有 fork 已完成） ===== */

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

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

    log_msg("[Guard] 启动 PID=%d，logcat 进程 %d 个\n", (int)getpid(), pipe_count);

    /* ===== 主循环: poll 所有 logcat pipe ===== */
    char line[MAX_LINE];
    size_t used = 0;

    while (running) {
        reload_if_changed();
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，Java 端应执行清理\n");
        }

        if (pipe_count == 0) {
            usleep(500 * 1000);
            /* 下次重试 fork 一个 */
            if (pipe_count == 0) {
                int pfd[2];
                if (pipe(pfd) >= 0) {
                    pid_t lp = fork_logcat(pfd[1], "events");
                    close(pfd[1]);
                    if (lp >= 0) {
                        pipes[0].fd = pfd[0];
                        pipes[0].pid = lp;
                        pipe_count = 1;
                    } else close(pfd[0]);
                }
            }
            continue;
        }

        struct pollfd pfds[MAX_LOGCAT];
        for (int i = 0; i < pipe_count; i++) {
            pfds[i].fd = pipes[i].fd;
            pfds[i].events = POLLIN;
        }
        int pr = poll(pfds, pipe_count, 500);

        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;

        for (int i = 0; i < pipe_count; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;

            char buf[512];
            ssize_t n = read(pipes[i].fd, buf, sizeof(buf));
            if (n <= 0) {
                /* 这个 logcat 退出了 → 重启 */
                close(pipes[i].fd);
                waitpid(pipes[i].pid, NULL, WNOHANG);
                int new_pfd[2];
                if (pipe(new_pfd) >= 0) {
                    /* 找出刚才的 buffer 名 — 简化：重新 fork 用 "events" */
                    pid_t lp = fork_logcat(new_pfd[1], "events");
                    close(new_pfd[1]);
                    if (lp >= 0) {
                        pipes[i].fd = new_pfd[0];
                        pipes[i].pid = lp;
                        log_msg("[信息] logcat #%d 已重启 PID=%d\n", i, (int)lp);
                    } else {
                        close(new_pfd[0]);
                        /* 移走这个位置 */
                        for (int j = i; j < pipe_count - 1; j++) pipes[j] = pipes[j+1];
                        pipe_count--;
                        i--;
                    }
                } else {
                    for (int j = i; j < pipe_count - 1; j++) pipes[j] = pipes[j+1];
                    pipe_count--;
                    i--;
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
    }

    log_msg("[Guard] 退出\n");
    remove_pidfile();
    for (int i = 0; i < pipe_count; i++) close(pipes[i].fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
