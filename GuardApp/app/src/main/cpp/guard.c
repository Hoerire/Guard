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
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
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
#define MAX_POLL_FDS (MAX_LOGCAT + 1)  /* +1 for netlink connector */
#define OOM_SCAN_INTERVAL_MS 800       /* netlink 正常时兜底扫描间隔 */
#define OOM_SCAN_FALLBACK_MS 300       /* netlink 降级时加快扫描 */
#define HEALTH_LOG_INTERVAL_S 60       /* 健康日志周期 */

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

/* netlink connector fd + 事件触发标记 */
static int g_netlink_fd = -1;
static bool g_netlink_pending = false;

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

/* 判断一个进程名是否是 Android 应用（com. 开头，排除系统服务） */
static bool looks_like_app(const char *name) {
    if (!name || !*name) return false;
    if (strncmp(name, "com.", 4) != 0) return false;
    /* 排除常见系统/厂商服务包名（这些可能永久 score=0） */
    static const char *blacklist[] = {
        "com.android.systemui",
        "com.android.settings",
        "com.android.launcher",
        "com.android.launcher3",
        "com.android.packageinstaller",
        "com.android.permissioncontroller",
        "com.oplus.appplatform",        /* OPPO 系统服务 */
        "com.oplus.launcher",           /* OPPO 桌面 */
        "com.coloros.launcher",         /* ColorOS 桌面 */
        "com.heytap.market",            /* OPPO 市场 */
        "com.android.system",
        NULL
    };
    for (int i = 0; blacklist[i]; i++) {
        if (strcmp(name, blacklist[i]) == 0) return false;
    }
    return true;
}

/* 扫描 /proc，找前台应用（oom_score_adj == 0 且像应用包名）
   多个 score=0 时选 PID 最大的（最可能是最新前台）
   找不到 score=0 则 fallback 到分数最低的应用进程 */
static void run_oom_scan(long long now_ms) {
    int dir = open("/proc", O_RDONLY | O_DIRECTORY);
    if (dir < 0) return;

    char ent_buf[8192];   /* 栈上，不碰堆 */
    int best_score = 10000;
    char best_pkg[MAX_PKG] = {0};
    pid_t best_pid = 0;
    bool found_fg_zero = false;

    while (1) {
        ssize_t n = syscall(SYS_getdents64, dir, ent_buf, sizeof(ent_buf));
        if (n <= 0) break;

        for (ssize_t off = 0; off < n; ) {
            struct dirent64_r *d = (struct dirent64_r*)(ent_buf + off);
            if (d->d_reclen == 0) break;
            off += d->d_reclen;

            if (d->d_type != DT_DIR) continue;
            if (d->d_name[0] < '0' || d->d_name[0] > '9') continue;

            pid_t pid = atoi(d->d_name);
            if (pid <= 100) continue;

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

            char pkg[MAX_PKG] = {0};
            if (!read_pkg_from_proc(pid, pkg, sizeof(pkg))) continue;
            if (!looks_like_app(pkg)) continue;

            if (score == 0) {
                /* 多个 score=0：选 PID 最大的（最可能是最新 foreground） */
                if (!found_fg_zero || pid > best_pid) {
                    best_pid = pid;
                    best_score = score;
                    strncpy(best_pkg, pkg, sizeof(best_pkg) - 1);
                    found_fg_zero = true;
                }
            } else if (!found_fg_zero) {
                /* 没找到 score==0 的，fallback 到分数最低的 */
                if (score < best_score) {
                    best_pid = pid;
                    best_score = score;
                    strncpy(best_pkg, pkg, sizeof(best_pkg) - 1);
                }
            }
        }
    }
    close(dir);
    last_oom_scan_ms = now_ms;

    if (best_pkg[0]) {
        emit_fg_event(best_pkg, found_fg_zero ? "oom_fg" : "oom_fallback");
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

/* ============ Netlink Connector (CN_PROC) ============
   创建 netlink socket 订阅进程事件（fork/exec/comm 变更）。
   内核通过 socket 主动推送事件 → 阻塞等待 → 零轮询零功耗。
   失败返回 -1（CONFIG_PROC_EVENTS 未启用或 SELinux 限制），调用方应降级。 */
static int netlink_init(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if (fd < 0) {
        log_msg("[Netlink] socket(AF_NETLINK,NETLINK_CONNECTOR) 失败 errno=%d(%s)\n",
                errno, strerror(errno));
        return -1;
    }

    struct sockaddr_nl addr = {0};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("[Netlink] bind(pid=%d) 失败 errno=%d(%s)\n",
                getpid(), errno, strerror(errno));
        close(fd);
        return -1;
    }

    /* 发送订阅命令：CN_PROC + PROC_CN_MCAST_LISTEN */
    char send_buf[256];
    struct nlmsghdr *nlh = (struct nlmsghdr*)send_buf;
    nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(uint32_t));
    nlh->nlmsg_type  = NLMSG_NOOP;
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_seq   = 1;
    nlh->nlmsg_pid   = getpid();

    struct cn_msg *cn = (struct cn_msg*)NLMSG_DATA(nlh);
    cn->id.idx = CN_IDX_PROC;
    cn->id.val = CN_VAL_PROC;
    cn->seq = 0;  cn->ack = 0;
    cn->len = sizeof(uint32_t);
    cn->flags = 0;

    uint32_t op = PROC_CN_MCAST_LISTEN;
    memcpy((char*)cn + sizeof(struct cn_msg), &op, sizeof(op));

    struct sockaddr_nl dst = {0};
    dst.nl_family = AF_NETLINK;
    dst.nl_pid = 0;  /* kernel */

    ssize_t rc = sendto(fd, send_buf, nlh->nlmsg_len, 0,
                        (struct sockaddr*)&dst, sizeof(dst));
    if (rc < 0) {
        log_msg("[Netlink] sendto(PROC_CN_MCAST_LISTEN) 失败 errno=%d(%s) — "
                "内核可能未启用 CONFIG_PROC_EVENTS\n", errno, strerror(errno));
        close(fd);
        return -1;
    }

    log_msg("[Netlink] CN_PROC 订阅成功 fd=%d\n", fd);
    return fd;
}

/* 处理 netlink 上来的一批消息。
   过滤：只关心 PROC_EVENT_EXEC（app 进程启动）、PROC_EVENT_COMM（进程名变更）、
   PROC_EVENT_FORK（新进程出生）。其他系统进程 fork/uid 变更忽略。
   命中 → g_netlink_pending = true → 主循环下一次 poll 后立刻跑 oom 扫描。 */
static void handle_netlink_data(int fd) {
    char buf[4096];
    ssize_t total = recv(fd, buf, sizeof(buf), 0);
    if (total <= 0) return;

    unsigned int len = (unsigned int)total;
    struct nlmsghdr *nlh;
    for (nlh = (struct nlmsghdr*)buf; NLMSG_OK(nlh, len);
         nlh = NLMSG_NEXT(nlh, len)) {
        /* 跳过 ack 消息（cn_msg.len == 0） */
        struct cn_msg *cn = (struct cn_msg*)NLMSG_DATA(nlh);
        if (cn->len == 0) continue;

        /* proc_event 紧跟 cn_msg 后面 */
        struct proc_event *ev = (struct proc_event*)((char*)cn + sizeof(struct cn_msg));
        uint32_t what = (uint32_t)ev->what;

        switch (what) {
            case PROC_EVENT_EXEC:
            case PROC_EVENT_COMM:
            case PROC_EVENT_FORK:
                /* 任何 app 进程启动/改名/出生 → 可能换前台 → 触发 oom 扫描 */
                g_netlink_pending = true;
                break;
            default:
                break;
        }
    }
}

/* ============ main ============ */
int main(int argc, char **argv) {
    /* ===== 步骤 1: 确定路径（纯字符串操作，无 fd） ===== */
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

    /* ===== 步骤 2: 尽早打开日志文件 — netlink_init 和 fork 前就要能用 ===== */
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    /* ===== 步骤 3: 重定向 stdin/stdout/stderr 到 /dev/null ===== */
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }

    /* ===== 步骤 4: 设置进程名为 "Guard" ===== */
    prctl(PR_SET_NAME, "Guard", 0, 0, 0);
    if (argv && argv[0]) {
        argv[0][0]='G'; argv[0][1]='u'; argv[0][2]='a';
        argv[0][3]='r'; argv[0][4]='d'; argv[0][5]='\0';
    }

    /* ===== 步骤 5: fork 所有 logcat（堆分配前！）===== */
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

    /* ===== 步骤 6: 创建 netlink CN_PROC socket（现在 log_fd 已开，错误可见）===== */
    g_netlink_fd = netlink_init();

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

    int scan_interval = g_netlink_fd >= 0 ? OOM_SCAN_INTERVAL_MS : OOM_SCAN_FALLBACK_MS;
    log_msg("[Guard] 启动 PID=%d，logcat %d 个，netlink=%s，扫描间隔=%dms\n",
            (int)getpid(), g_pipe_count,
            g_netlink_fd >= 0 ? "OK" : "降级(快速)", scan_interval);

    /* ===== 主循环 ===== */
    char line[MAX_LINE];
    size_t used = 0;
    long long last_health_s = 0;

    while (running) {
        reload_if_changed();
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，Java 端应执行清理\n");
        }

        /* 健康日志：每 HEALTH_LOG_INTERVAL_S 秒输出一次状态 */
        {
            struct timespec hts; clock_gettime(CLOCK_MONOTONIC, &hts);
            long long hnow = hts.tv_sec;
            if (hnow - last_health_s >= HEALTH_LOG_INTERVAL_S) {
                last_health_s = hnow;
                log_msg("[心跳] logcat=%d netlink=%s fg=%s\n",
                        g_pipe_count,
                        g_netlink_fd >= 0 ? "OK" : "-",
                        current_fg[0] ? current_fg : "?");
            }
        }

        /* poll 数组：logcat pipes [0..g_pipe_count-1] + netlink [末尾] */
        int nfds = g_pipe_count + (g_netlink_fd >= 0 ? 1 : 0);
        if (nfds == 0) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now = (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
            run_oom_scan(now);
            usleep(scan_interval * 1000);
            continue;
        }

        struct pollfd pfds[MAX_POLL_FDS];
        for (int i = 0; i < g_pipe_count; i++) {
            pfds[i].fd = g_pipes[i].fd;
            pfds[i].events = POLLIN;
        }
        if (g_netlink_fd >= 0) {
            pfds[g_pipe_count].fd = g_netlink_fd;
            pfds[g_pipe_count].events = POLLIN;
        }

        int pr = poll(pfds, nfds, scan_interval);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        /* --- 先处理 netlink --- */
        if (g_netlink_fd >= 0 && pfds[g_pipe_count].revents & POLLIN) {
            handle_netlink_data(g_netlink_fd);
        }

        /* --- 再处理 logcat pipes --- */
        for (int i = 0; i < g_pipe_count; i++) {
            short rev = pfds[i].revents;
            if (rev & (POLLHUP | POLLERR | POLLNVAL)) {
                log_msg("[警告] logcat #%d revents=0x%x，重启\n", i, rev);
                restart_logcat_pipe(i);
                i--;
                continue;
            }
            if (!(rev & POLLIN)) continue;

            char buf[512];
            ssize_t n = read(g_pipes[i].fd, buf, sizeof(buf));
            if (n <= 0) {
                log_msg("[警告] logcat #%d read 返回 %zd，重启\n", i, n);
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

        /* --- oom_score_adj 扫描 ---
           触发条件：netlink 事件到达 → 立刻扫；或到周期了 → 兜底扫
           netlink 正常: 800ms; 降级: 300ms */
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now_ms = (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;

            bool should = g_netlink_pending ||
                          (now_ms - last_oom_scan_ms >= scan_interval);
            g_netlink_pending = false;
            if (should) run_oom_scan(now_ms);
        }
    }

    log_msg("[Guard] 退出\n");
    remove_pidfile();
    for (int i = 0; i < g_pipe_count; i++) close(g_pipes[i].fd);
    if (g_netlink_fd >= 0) close(g_netlink_fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
