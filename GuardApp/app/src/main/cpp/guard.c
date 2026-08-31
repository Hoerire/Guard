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
#include <sys/stat.h>
#include <sys/poll.h>
#include <stdarg.h>

/* ============ 常量 ============ */
#define MAX_PATH         512
#define MAX_LINE         4096
#define MAX_PKG          256
#define MAX_CONFIG       8192
#define MAX_TARGET       64
#define MAX_FREEZE       64
#define EVENT_DEV        "/dev/log/events"
#define EVENT_DEV_2      "/dev/log/events_0"

/* ============ 全局状态 ============ */
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t cleanup_pending = 0;
static char config_path[MAX_PATH];
static char log_path[MAX_PATH];
static char pid_path[MAX_PATH];
static int  log_fd = -1;
static int  event_fd = -1;

/* ============ 日志（只写 log_fd）============ */
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

static void remove_pidfile(void) {
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

/* 找到行的分隔符（先找 ':' 再找 '='） */
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

/* ============ 包匹配 ============ */
static bool comp_matches(const char *comp, const Package *list, size_t count) {
    if (!comp) return false;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(comp, list[i].name, strlen(list[i].name)) == 0)
            return true;
    }
    return false;
}

/*
 * 解析 logcat events buffer 中的 ActivityManager resumed 事件
 * 格式 (briefer/brief):
 *   I/ActivityManager( 123): wm_on_resume_called(999): ActivityRecord{... com.tencent.tmgp.cf/.MainActivity}
 *   I/ActivityManager( 123): wm_on_top_resumed_gained_called(999): ActivityRecord{... com.android.launcher/.Launcher}
 */
static void handle_events_data(const char *data, size_t len) {
    static char line_buf[MAX_LINE];
    static size_t line_used = 0;

    /* 按字节追加 */
    if (line_used + len >= sizeof(line_buf)) { line_used = 0; }
    memcpy(line_buf + line_used, data, len);
    line_used += len;

    /* 按 \n 切行 */
    char *start = line_buf;
    char *nl;
    while ((nl = memchr(start, '\n', line_used - (start - line_buf))) != NULL) {
        *nl = '\0';
        const char *tag = "wm_on_resume_called";
        const char *tag2 = "wm_on_top_resumed_gained_called";
        const char *p = strstr(start, tag);
        bool handled = false;
        if (!p) { p = strstr(start, tag2); handled = true; }
        if (p) {
            /* 找 { pkg/activity } 或最后一个空格+包名 */
            const char *brace = strchr(p, '{');
            const char *end = brace ? strchr(brace + 1, '}') : NULL;
            const char *pkg_start = NULL;
            if (brace && end) {
                /* 从 { 到 } 之间找最后一个空格后的包名 */
                const char *last_space = NULL;
                for (const char *q = brace + 1; q < end; q++) if (*q == ' ') last_space = q;
                if (last_space) pkg_start = last_space + 1;
                else pkg_start = brace + 1;
            } else {
                /* fallback: 找最后的空格串 */
                const char *last_space = NULL;
                for (const char *q = p; q < start + line_used; q++) if (*q == ' ') last_space = q;
                if (last_space) pkg_start = last_space + 1;
            }
            if (pkg_start && *pkg_start) {
                char comp[MAX_PKG];
                size_t pkg_len = end ? (size_t)(end - pkg_start) : strlen(pkg_start);
                if (pkg_len >= sizeof(comp)) pkg_len = sizeof(comp) - 1;
                strncpy(comp, pkg_start, pkg_len);
                comp[pkg_len] = '\0';
                char *slash = strchr(comp, '/');
                if (slash) *slash = '\0';
                if (comp[0] && strcmp(comp, current_fg) != 0) {
                    reload_if_changed();
                    bool target = comp_matches(comp, cfg.target, cfg.target_count);
                    strncpy(current_fg, comp, sizeof(current_fg) - 1);
                    log_msg("[前台事件] %s %s\n", comp, target ? "【目标应用】" : "【普通应用】");
                }
            }
        }
        start = nl + 1;
    }
    /* 未消费的尾部移到开头 */
    if (start != line_buf) {
        size_t remaining = line_used - (start - line_buf);
        if (remaining > 0) memmove(line_buf, start, remaining);
        line_used = remaining;
    } else {
        line_used = 0;
    }
}

/* 打开 event 设备 */
static int open_event_dev(void) {
    /* 尝试 /dev/log/events 再 fallback /dev/log/events_0 */
    int fd = open(EVENT_DEV, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        log_msg("[事件源] %s (fd=%d)\n", EVENT_DEV, fd);
        return fd;
    }
    fd = open(EVENT_DEV_2, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        log_msg("[事件源] %s (fd=%d)\n", EVENT_DEV_2, fd);
        return fd;
    }
    log_msg("[警告] 无法打开事件源：%s 和 %s 都失败（errno=%d）\n",
            EVENT_DEV, EVENT_DEV_2, errno);
    return -1;
}

/* ============ main ============ */
int main(int argc, char **argv) {
    /* 重定向 stdin/stdout/stderr 全部到 /dev/null */
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }

    /* 初始化路径 */
    const char *pkg = getenv("GUARD_PKG");
    if (!pkg) pkg = "com.example.guard";
    snprintf(config_path, sizeof(config_path), "/data/user/0/%s/files/config.txt", pkg);
    snprintf(log_path, sizeof(log_path), "/data/user/0/%s/files/guard.log", pkg);
    snprintf(pid_path, sizeof(pid_path), "%s.pid", config_path);

    /* 打开日志文件 */
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    /* 安装信号 */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    /* 首次读配置 */
    reload_if_changed();

    /* 写 pidfile */
    write_pidfile();
    atexit(remove_pidfile);

    log_msg("[Guard] 启动 PID=%d\n", (int)getpid());

    /* 打开事件设备 */
    event_fd = open_event_dev();

    /* 主循环：poll 事件设备 + 定期 reload 配置 */
    while (running) {
        /* 定期 reload 配置 */
        reload_if_changed();

        /* 处理 SIGUSR1 */
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，Java 端应执行清理\n");
        }

        if (event_fd >= 0) {
            struct pollfd pfd = { .fd = event_fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 500);
            if (pr < 0) {
                if (errno == EINTR) continue;
                /* poll 出错，尝试重新打开 */
                log_msg("[警告] poll 事件源失败 errno=%d，尝试重开\n", errno);
                close(event_fd);
                event_fd = -1;
                usleep(500 * 1000);
                event_fd = open_event_dev();
                continue;
            }
            if (pr > 0 && (pfd.revents & POLLIN)) {
                /* 一次性读完所有可用数据 */
                char buf[4096];
                while (1) {
                    ssize_t n = read(event_fd, buf, sizeof(buf));
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        /* 设备可能断开 */
                        log_msg("[警告] read 事件源失败 errno=%d，重开\n", errno);
                        close(event_fd); event_fd = -1;
                        event_fd = open_event_dev();
                        break;
                    }
                    if (n == 0) break;
                    handle_events_data(buf, n);
                }
            }
        } else {
            /* 事件源不可用，简单 sleep 等下次重试 */
            usleep(500 * 1000);
            event_fd = open_event_dev();
        }
    }

    log_msg("[Guard] 退出\n");
    remove_pidfile();
    if (event_fd >= 0) close(event_fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
