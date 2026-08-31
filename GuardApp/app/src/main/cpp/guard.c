/*
 * Guard 守护进程 — 前台检测
 * 架构：logcat 事件驱动 + oom_score_adj 兜底（poll 超时触发）
 * 关键：所有 fork() 在堆分配前执行；所有 /proc 操作用纯 syscall
 */
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
#include <sys/syscall.h>
#include <dirent.h>
#include <stdarg.h>

/* ============ 常量 ============ */
#define MAX_PATH    512
#define MAX_LINE    4096
#define MAX_PKG     256
#define MAX_CONFIG  8192
#define MAX_TARGET  64
#define MAX_FREEZE  64
#define MAX_LOGCAT  3   /* events + main + system */
#define OOM_SCAN_INTERVAL_MS 1500  /* oom_score_adj 兜底扫描间隔 */

/* ============ 全局 ============ */
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t cleanup_pending = 0;
static char config_path[MAX_PATH];
static char log_path[MAX_PATH];
static char pid_path[MAX_PATH];
static int  log_fd = -1;

/* logcat pipe 表（全局，fork_logcat 和 restart_logcat_pipe 需要访问） */
static struct { int fd; pid_t pid; } g_pipes[MAX_LOGCAT];
static int g_pipe_count = 0;

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
static long long last_oom_scan_ms = 0;

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

/* 统一前台事件输出（logcat 和 oom 扫描都调这里，去重） */
static void emit_fg_event(const char *comp, const char *src) {
    if (!comp || !*comp || strcmp(comp, current_fg) == 0) return;
    reload_if_changed();
    bool target = comp_matches(comp, cfg.target, cfg.target_count);
    strncpy(current_fg, comp, sizeof(current_fg) - 1);
    log_msg("[前台事件] %s %s（来源:%s）\n", comp,
            target ? "【目标应用】" : "【普通应用】", src);
}

/* ============ logcat 包名提取 ============ */
static bool extract_pkg(const char *line, char *out, size_t out_size) {
    /* 格式 1: events buffer 花括号格式 */
    const char *brace = strchr(line, '{');
    const char *close = brace ? strchr(brace + 1, '}') : NULL;
    if (brace && close) {
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
    /* 格式 2: "Displayed com.example/.MainActivity" */
    const char *d = strstr(line, "Displayed");
    if (d) {
        d += 9;
        if (*d == ':') d++;
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
    /* 格式 3: "TopResumedActivity" */
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
    /* 格式 4: "activity com.example/.MainActivity" */
    const char *a = strstr(line, " activity ");
    if (!a) a = strstr(line, " activity=");
    if (a) {
        const char *last = NULL;
        const char *scan = line;
        while ((a = strstr(scan, "activity ")) != NULL) { last = a; scan = a + 9; }
        if (last) {
            last += 9;
            while (*last == ' ') last++;
            size_t len = 0;
            while (last[len] && last[len] != ' ' && last[len] != '\n' &&
                   last[len] != '/' && last[len] != ')') len++;
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
    bool is_fg =
        strstr(line, "wm_on_resume_called") ||
        strstr(line, "wm_on_top_resumed_gained_called") ||
        strstr(line, "wm_on_paused_called") ||
        (strstr(line, "ActivityTaskManager") &&
         (strstr(line, "Displayed") || strstr(line, "TopResumedActivity"))) ||
        (strstr(line, "ActivityManager") && strstr(line, "Displayed"));

    if (!is_fg) return;

    char comp[MAX_PKG] = {0};
    if (!extract_pkg(line, comp, sizeof(comp))) return;
    emit_fg_event(comp, "logcat");
}

/* ============ oom_score_adj 兜底扫描（纯 syscall，不碰堆） ============ */
/* Linux dirent64 结构（与内核 ABI 对齐） */
struct dirent64_r {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char     d_name[256];
};

/* 从 /proc/<pid>/cmdline 或 /proc/<pid>/comm 提取包名
   Android 进程名可能是 "com.example" 或 "com.example:remote"
   也可能 cmdline 是空的（system_server 用 comm） */
static bool read_pkg_from_proc(pid_t pid, char *out, size_t out_size) {
    char path[MAX_PATH];

    /* 优先读 cmdline（Android app 主进程的 cmdline 是包名） */
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[MAX_PKG];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            /* cmdline 第一个 \0 是 argv[0] 结束 */
            char *nul = memchr(buf, '\0', n);
            if (nul) *nul = '\0';
            /* 排除系统进程：app_process64 / zygote / kswapd 等 */
            if (strncmp(buf, "app_process", 11) != 0 &&
                strncmp(buf, "zygote", 6) != 0 &&
                strncmp(buf, "cameraserver", 12) != 0 &&
                strncmp(buf, "surfaceflinger", 14) != 0 &&
                strncmp(buf, "mediaserver", 11) != 0 &&
                strncmp(buf, "dumpsys", 7) != 0 &&
                strncmp(buf, "servicemanager", 14) != 0 &&
                buf[0] >= 'a' && buf[0] <= 'z') {
                /* 去掉 :remote 等后缀 */
                char *colon = strchr(buf, ':');
                if (colon) *colon = '\0';
                /* 去掉 .MainActivity 这种后缀？不，cmdline 里没有 */
                strncpy(out, buf, out_size - 1);
                return true;
            }
        }
    }

    /* 备选：读 comm（/proc/<pid>/comm） */
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[MAX_PKG];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            /* 去掉末尾换行 */
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
            /* comm 可能是 "com.example" 或 "com.example:push" */
            if (buf[0] >= 'a' && buf[0] <= 'z' &&
                strncmp(buf, "app_process", 11) != 0 &&
                strncmp(buf, "zygote", 6) != 0 &&
                strncmp(buf, "dumpsys", 7) != 0) {
                char *colon = strchr(buf, ':');
                if (colon) *colon = '\0';
                strncpy(out, buf, out_size - 1);
                return true;
            }
        }
    }
    return false;
}

/* 扫描 /proc，找 oom_score_adj <= 50 的包名（最可能是前台） */
static void run_oom_scan(long long now_ms) {
    int dir = open("/proc", O_RDONLY | O_DIRECTORY);
    if (dir < 0) return;

    char ent_buf[8192];   /* 栈上，不碰堆 */
    int best_score = 10000;
    char best_pkg[MAX_PKG] = {0};

    while (1) {
        ssize_t n = syscall(SYS_getdents64, dir, ent_buf, sizeof(ent_buf));
        if (n <= 0) break;

        for (ssize_t off = 0; off < n; ) {
            struct dirent64_r *d = (struct dirent64_r*)(ent_buf + off);
            if (d->d_reclen == 0) break;
            off += d->d_reclen;

            /* 只处理数字目录（进程 PID） */
            if (d->d_type != DT_DIR) continue;
            if (d->d_name[0] < '0' || d->d_name[0] > '9') continue;

            pid_t pid = atoi(d->d_name);
            if (pid <= 100) continue;  /* 跳过 init/kthreadd 等 */

            /* 读 oom_score_adj */
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
            int ofd = open(path, O_RDONLY);
            if (ofd < 0) continue;
            char obuf[16];
            ssize_t on = read(ofd, obuf, sizeof(obuf) - 1);
            close(ofd);
            if (on <= 0) continue;
            obuf[on] = '\0';
            int score = atoi(obuf);

            /* 前台进程：score = 0；可见进程：score <= 50 */
            if (score < best_score) {
                char pkg[MAX_PKG] = {0};
                if (read_pkg_from_proc(pid, pkg, sizeof(pkg))) {
                    best_score = score;
                    strncpy(best_pkg, pkg, sizeof(best_pkg) - 1);
                }
            }
        }
    }
    close(dir);
    last_oom_scan_ms = now_ms;

    if (best_score <= 50 && best_pkg[0]) {
        emit_fg_event(best_pkg, "oom_scan");
    }
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

/* 重启一个死掉的 logcat pipe（含 waitpid + fork 新的） */
static void restart_logcat_pipe(int idx) {
    close(g_pipes[idx].fd);
    waitpid(g_pipes[idx].pid, NULL, WNOHANG);
    int new_pfd[2];
    if (pipe(new_pfd) >= 0) {
        pid_t lp = fork_logcat(new_pfd[1], "events");
        close(new_pfd[1]);
        if (lp >= 0) {
            g_pipes[idx].fd = new_pfd[0];
            g_pipes[idx].pid = lp;
            log_msg("[信息] logcat #%d 已重启 PID=%d\n", idx, (int)lp);
        } else {
            close(new_pfd[0]);
            for (int j = idx; j < g_pipe_count - 1; j++) g_pipes[j] = g_pipes[j+1];
            g_pipe_count--;
        }
    } else {
        for (int j = idx; j < g_pipe_count - 1; j++) g_pipes[j] = g_pipes[j+1];
        g_pipe_count--;
    }
}

/* ============ main ============ */
int main(int argc, char **argv) {
    /* ===== 步骤 1: 重定向 stdin/stdout/stderr 到 /dev/null ===== */
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }

    /* ===== 步骤 2: 确定路径 ===== */
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

    /* ===== 步骤 3: 设置进程名为 "Guard" ===== */
    prctl(PR_SET_NAME, "Guard", 0, 0, 0);
    if (argv && argv[0]) {
        argv[0][0]='G'; argv[0][1]='u'; argv[0][2]='a';
        argv[0][3]='r'; argv[0][4]='d'; argv[0][5]='\0';
    }

    /* ===== 步骤 4: fork 所有 logcat（堆分配前！）===== */

    static const char *buffers[] = { "events", "main", "system", NULL };
    for (int i = 0; buffers[i] != NULL && g_pipe_count < MAX_LOGCAT; i++) {
        int pfd[2];
        if (pipe(pfd) < 0) continue;
        pid_t lp = fork_logcat(pfd[1], buffers[i]);
        close(pfd[1]);
        if (lp < 0) { close(pfd[0]); continue; }
        g_pipes[g_pipe_count].fd = pfd[0];
        g_pipes[g_pipe_count].pid = lp;
        g_pipe_count++;
    }

    /* ===== 步骤 5: 堆分配现在可以碰了 ===== */
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

    log_msg("[Guard] 启动 PID=%d，logcat 进程 %d 个\n", (int)getpid(), g_pipe_count);

    /* ===== 主循环 ===== */
    char line[MAX_LINE];
    size_t used = 0;
    long long start_ms = 0;
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      start_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

    while (running) {
        reload_if_changed();
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，Java 端应执行清理\n");
        }

        /* 计算 poll 超时：如果 g_pipe_count==0 就跑 oom 扫描兜底 */
        int poll_timeout = (g_pipe_count == 0) ? OOM_SCAN_INTERVAL_MS : 1500;

        struct pollfd pfds[MAX_LOGCAT];
        for (int i = 0; i < g_pipe_count; i++) {
            pfds[i].fd = g_pipes[i].fd;
            pfds[i].events = POLLIN;
        }
        int pr = poll(pfds, g_pipe_count, poll_timeout);

        if (pr < 0) { if (errno == EINTR) continue; break; }

        if (pr > 0) {
            for (int i = 0; i < g_pipe_count; i++) {
                /* ★ 关键修复：同时检查 POLLIN 和 POLLHUP/POLLERR/POLLNVAL
                   之前只检查 POLLIN 导致 logcat 退出后死 pipe 永远挂着
                   → poll 每次立刻返回 → 忙等死循环 → CPU 100% */
                short rev = pfds[i].revents;
                if (rev & (POLLHUP | POLLERR | POLLNVAL)) {
                    /* pipe 断开 → 重启这个 logcat，continue 会 ++i
                       但 restart_logcat_pipe 会把它替换/移除 */
                    restart_logcat_pipe(i);
                    i--;  /* 处理后留在当前位置继续检查 */
                    continue;
                }
                if (!(rev & POLLIN)) continue;

                char buf[512];
                ssize_t n = read(g_pipes[i].fd, buf, sizeof(buf));
                if (n <= 0) {
                    restart_logcat_pipe(i);
                    i--;
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

        /* poll 超时 或 有输出但没命中事件 → oom_score_adj 兜底扫描
           这个扫描能抓到 logcat 漏掉的前台切换 */
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now_ms - last_oom_scan_ms >= OOM_SCAN_INTERVAL_MS) {
                run_oom_scan(now_ms);
            }
        }
    }

    log_msg("[Guard] 退出\n");
    remove_pidfile();
    for (int i = 0; i < g_pipe_count; i++) close(g_pipes[i].fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
