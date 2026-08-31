#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>   /* SYS_getdents64 + syscall() */
/* linux_dirent64 结构 —— NDK 没提供 <linux/dirent.h>，自己定义一份。
 * 对应内核的 struct linux_dirent64，d_reclen 是内核实际写入的记录长度。*/
struct linux_dirent64 {
    uint64_t        d_ino;
    int64_t         d_off;
    unsigned short  d_reclen;
    unsigned char   d_type;
    char            d_name[];
};

#define MAX_PATH     512
#define DEFAULT_CONFIG "/data/local/tmp/Guard/config.txt"

#define MAX_TARGET  64
#define MAX_FREEZE  128
#define MAX_PKG     256
#define MAX_LINE    4096
#define MAX_OUTPUT  65536

static char config_file[MAX_PATH];
static char pid_file[MAX_PATH];    /* ${config_file}.pid —— Java 端 kill -0 校验存活用 */
static char crash_file[MAX_PATH];   /* ${config_file}.crash —— 崩溃处理器落盘现场 */
static char log_path[MAX_PATH];     /* guard.log —— 打开 fd 后用 write() 直写，绕开 FILE* FORTIFY */
static int  log_fd = -1;            /* 直写 fd，不走 fprintf/vfprintf/fflush */

#define DEFAULT_INTERVAL 2
#define MIN_INTERVAL     1
#define MAX_INTERVAL     60

typedef struct {
    char name[MAX_PKG];
} Package;

typedef struct {
    char pkg[MAX_PKG];
    bool was_enabled;
    bool modified;
} AppState;

typedef struct {
    unsigned int interval;
    int appuid;                     // Guard 应用的 Linux UID（兜底识别脚本残留血统）
    Package target[MAX_TARGET];
    size_t target_count;
    Package freeze[MAX_FREEZE];
    size_t freeze_count;
} Config;

static Config cfg;
static AppState states[MAX_FREEZE];
static size_t state_count;
static volatile sig_atomic_t running = 1;
/* 立即清理标志：脚本执行完毕 Java 端通过 kill -USR1 <守护PID> 触发；
 * 主循环 poll 返回/中断后检查此标志，为 1 时立即跑一次 cleanup_script_leftovers()
 *，不再必须等切到目标应用才清包装层。 */
static volatile sig_atomic_t cleanup_pending = 0;
static bool active;
static char current_fg[MAX_PKG];
static struct stat cfg_stat;
static bool cfg_stat_valid;
/* 崩溃现场落盘路径：在守护 SIGSEGV / SIGBUS / SIGABRT / SIGFPE 等致命信号触发
 * 时，把信号号、errno、PID、最后已知上下文写到 ${config_file}.crash，便于下次
 * UI 启动时读取呈现给用户，定位 FORTIFY / 非法地址 等偶发崩溃。*/
static char crash_file[MAX_PATH];

static bool reload_if_changed(void);
static void remove_pidfile(void);    /* forward decl：供 signal_handler / atexit 使用 */

/* 直接用系统调用写字符串（不走 stdio FILE* / vfprintf 锁路径）。
 * 当 Bionic FORTIFY 打印 "pthread_mutex_lock called on a destroyed mutex" 前后，
 * stderr 对应的 FILE* 内部锁可能已经被同进程其它线程（通常是 libbinder/zygote
 * 继承下来的背景线程）破坏；再走 fprintf/vfprintf 会二次命中 FORTIFY 再被
 * SIGABRT。因此崩溃路径用 write() 直写 fd，完全绕开 FILE* 层。*/
static void raw_write(int fd, const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        n -= (size_t)w;
        s += (size_t)w;
    }
}
static void raw_write_i(int fd, long v)
{
    char buf[24]; int p = 0;
    unsigned long uv;
    if (v < 0) {
        raw_write(fd, "-");
        uv = (unsigned long)(-(v + 1)) + 1UL;  /* 避免 -LONG_MIN 的溢出 */
    } else {
        uv = (unsigned long)v;
    }
    if (uv == 0) buf[p++] = '0';
    while (uv > 0 && p < (int)sizeof(buf) - 1) {
        buf[p++] = (char)('0' + (uv % 10));
        uv /= 10;
    }
    for (int i = 0, j = p - 1; i < j; i++, j--) { char t = buf[i]; buf[i] = buf[j]; buf[j] = t; }
    buf[p] = 0;
    raw_write(fd, buf);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    /* 第一步：pidfile 先删掉，避免 UI 误以为服务还在。 */
    remove_pidfile();

    /* 第二步：崩溃现场写 crash_file + stderr (fd=2) 双通道 */
    char line[192];
    int cfd = -1;
    if (crash_file[0]) {
        cfd = open(crash_file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
#define W(fd, s) raw_write(fd, s)
    W(2, "\n[Guard][CRASH] signal=");
    raw_write_i(2, sig);
    W(2, " pid="); raw_write_i(2, (long)getpid());
    W(2, " code="); raw_write_i(2, si ? (long)si->si_code : -1L);
    W(2, " errno="); raw_write_i(2, (long)errno);
    W(2, "\n");
    if (cfd >= 0) {
        /* 把 fd 2 上的同一段文本再给 crash_file */
        int len = snprintf(line, sizeof(line),
                           "signal=%d code=%d errno=%d addr=%p pid=%d ppid=%d uid=%d\n",
                           sig,
                           si ? si->si_code : -1,
                           errno,
                           si ? si->si_addr : NULL,
                           (int)getpid(),
                           (int)getppid(),
                           (int)getuid());
        if (len > 0) (void)write(cfd, line, (size_t)len);
        /* 再附带一段 ASCII stack 回溯提示：通过 /proc/self/maps 前 8 行，
         * 便于将来进一步符号化；此处不强制解析，仅留线索。 */
        int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (maps >= 0) {
            char buf[2048];
            ssize_t r = read(maps, buf, sizeof(buf) - 1);
            if (r > 0) {
                (void)write(cfd, "maps:\n", 6);
                (void)write(cfd, buf, (size_t)r);
                (void)write(cfd, "\n", 1);
            }
            close(maps);
        }
        close(cfd);
    }
#undef W
    /* 致命信号不返回：恢复默认处理器然后再 raise 一次，让内核正确写入退出状态 */
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

static void install_crash_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGSYS, SIGTRAP };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        (void)sigaction(sigs[i], &sa, NULL);
    }
}

static void signal_handler(int sig)
{
    if (sig == SIGUSR1) { cleanup_pending = 1; return; }
    running = 0;
    remove_pidfile();
}

static void log_msg(const char *fmt, ...)
{
    /* ⚠️ 绝对不能用 fprintf(stderr, …) / fflush(stderr)：
     *
     *   守护启动方式是 `nohup guard cfg > guard.log 2>&1 &`，
     *   stderr FILE* 内部有 Bionic pthread_mutex_t。当 cleanup 杀完一批进程
     *   （60 个进程时必崩，9 个时也崩），Scudo 堆分配器复用到 FILE* 同一 chunk
     *   时 mutex 状态已被 destroy，Bionic FORTIFY 检测到 → raise(SIGABRT) →
     *   守护崩溃，pidfile 被删，UI 提示"服务停止了"。
     *
     *   全部替换为 write() 系统调用直写 fd（log_fd = O_APPEND 打开的 guard.log），
     *   完全绕开 FILE* / pthread_mutex_t / Scudo 堆分配器的 FILE 对象层。
     *   时间戳用 snprintf 格式化到栈缓冲区，vsnprintf 格式化日志体（都不需要
     *   FILE*，不堆分配）。write() 保证内核 O_APPEND 原子追加。
     */
    char buf[MAX_OUTPUT];   /* 64KB 栈缓冲足够容纳单次日志；vsnprintf 截断写 */
    int off = 0;

    /* 时间戳段：[YYYY-MM-DD HH:MM:SS.mmm] */
    char timebuf[32];
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        struct tm tmv;
        localtime_r(&ts.tv_sec, &tmv);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "[%s.%03ld] ", timebuf, ts.tv_nsec / 1000000L);
    }

    /* 日志体段（用户 fmt 内容）—— vsnprintf 纯格式化到缓冲，不碰 FILE* */
    va_list ap;
    va_start(ap, fmt);
    int want = vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    if (want > 0) off += want;
    if (off < 0) off = 0;
    if ((size_t)off >= sizeof(buf)) off = (int)sizeof(buf) - 1;
    buf[off] = '\0';

    /* 直写系统调用：绕过 FILE*，绕过 pthread_mutex_t，绕过 Scudo */
    if (off > 0 && log_fd >= 0) {
        size_t remain = (size_t)off;
        char *p = buf;
        while (remain > 0) {
            ssize_t w = write(log_fd, p, remain);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            remain -= (size_t)w;
            p += (size_t)w;
        }
    } else if (off > 0) {
        /* 兜底：log_fd 还没开就 raw_write 到 stderr fd=2，绝对不用 FILE* */
        raw_write(2, buf);
    }
}

/* 写当前 PID 到 pid_file（格式一行纯文本），Java 端用 kill -0 精准确认存活，
 * 避免 pgrep -x 被僵尸进程/同名/子进程 su 返回码异常误导。*/
static void write_pidfile(void)
{
    if (!pid_file[0])
        return;

    int fd = open(pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_msg("[警告] 无法写入 pidfile %s: %s\n",
                pid_file, strerror(errno));
        return;
    }

    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%d\n", (int)getpid());
    if (n > 0)
        (void)write(fd, tmp, (size_t)n);
    close(fd);
    chmod(pid_file, 0644);
}

/* 清理 pidfile。进程退出/收信号时调用，避免 Java 端误判。*/
static void remove_pidfile(void)
{
    if (pid_file[0])
        (void)unlink(pid_file);
}

static void trim(char *s)
{
    if (!s)
        return;

    char *start = s;

    while (*start == ' ' ||
           *start == '\t' ||
           *start == '\r' ||
           *start == '\n')
        start++;

    char *end = start + strlen(start);

    while (end > start &&
           (end[-1] == ' ' ||
            end[-1] == '\t' ||
            end[-1] == '\r' ||
            end[-1] == '\n'))
        end--;

    *end = '\0';

    if (start != s)
        memmove(s, start, strlen(start) + 1);
}

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0)
        return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, size, "%s", src);
}

static bool valid_pkg(const char *s)
{
    if (!s || !s[0])
        return false;

    size_t len = strlen(s);

    if (len >= MAX_PKG ||
        s[0] == '.' ||
        s[len - 1] == '.')
        return false;

    size_t segment = 0;

    for (size_t i = 0; i < len; i++) {
        char c = s[i];

        if (c == '.') {
            if (segment == 0)
                return false;

            segment = 0;
            continue;
        }

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_'))
            return false;

        segment++;
    }

    return segment != 0;
}

static bool in_list(const Package *list,
                    size_t count,
                    const char *pkg)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].name, pkg) == 0)
            return true;
    }

    return false;
}

static bool add_target(Config *c, const char *pkg)
{
    if (!valid_pkg(pkg))
        return false;

    if (in_list(c->target, c->target_count, pkg))
        return true;

    if (c->target_count >= MAX_TARGET)
        return false;

    copy_str(c->target[c->target_count].name,
             MAX_PKG,
             pkg);

    c->target_count++;
    return true;
}

static bool add_freeze(Config *c, const char *pkg)
{
    if (!valid_pkg(pkg))
        return false;

    if (in_list(c->freeze, c->freeze_count, pkg))
        return true;

    if (c->freeze_count >= MAX_FREEZE)
        return false;

    copy_str(c->freeze[c->freeze_count].name,
             MAX_PKG,
             pkg);

    c->freeze_count++;
    return true;
}

static int exec_cmd_rc(char *out,
                       size_t out_size,
                       const char *const argv[])
{
    if (out && out_size)
        out[0] = '\0';

    int pipefd[2];

    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();

    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);

        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);

        close(pipefd[1]);

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);

    char buf[4096];
    size_t pos = 0;

    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));

        if (n > 0) {
            if (out && out_size > 1 && pos < out_size - 1) {
                size_t copy = (size_t)n;

                if (copy > out_size - 1 - pos)
                    copy = out_size - 1 - pos;

                memcpy(out + pos, buf, copy);
                pos += copy;
            }

            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        break;
    }

    close(pipefd[0]);

    if (out && out_size) {
        if (pos >= out_size)
            pos = out_size - 1;

        out[pos] = '\0';
    }

    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}

static bool package_exists(const char *pkg)
{
    const char *argv[] = {
        "/system/bin/pm", "path", pkg, NULL
    };

    char out[4096];

    if (exec_cmd_rc(out, sizeof(out), argv) < 0)
        return false;

    // 某些 pm 版本对已安装包也可能返回非 0，但输出含 package: 行即视为已安装
    return strstr(out, "package:") != NULL;
}

static bool list_has_pkg(const char *flag, const char *pkg, bool *found)
{
    const char *argv[] = {
        "/system/bin/pm", "list", "packages", flag, NULL
    };

    char out[MAX_OUTPUT];

    if (exec_cmd_rc(out, sizeof(out), argv) < 0)
        return false;

    char *saveptr = NULL;
    char *line = strtok_r(out, "\n", &saveptr);

    while (line) {
        trim(line);

        if (strncmp(line, "package:", 8) == 0 &&
            strcmp(line + 8, pkg) == 0) {
            *found = true;
            return true;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    *found = false;
    return true;
}

static bool package_enabled(const char *pkg, bool *enabled)
{
    if (!enabled)
        return false;

    bool found;
    bool ok;

    // 先在禁用列表（-d）中查找
    ok = list_has_pkg("-d", pkg, &found);

    if (ok && found) {
        *enabled = false;
        return true;
    }

    // 未命中禁用列表：再在启用列表（-e）中确认
    ok = list_has_pkg("-e", pkg, &found);

    if (ok && found) {
        *enabled = true;
        return true;
    }

    // 两个命令都执行失败：无法判定状态
    if (!ok)
        return false;

    // 命令执行成功但两个列表都不含该包（异常状态）：按启用处理
    *enabled = true;
    return true;
}

static int run_pm(const char *cmd, const char *pkg)
{
    const char *argv[] = {
        "/system/bin/pm", cmd, "--user", "0", pkg, NULL
    };

    char out[4096];
    int rc = exec_cmd_rc(out, sizeof(out), argv);

    trim(out);

    log_msg("[pm] %s %s => 退出码=%d 输出：%s%s%s\n",
            cmd, pkg, rc,
            out[0] ? "「" : "",
            out[0] ? out : "（无）",
            out[0] ? "」" : "");

    return rc;
}

static ssize_t find_state(const char *pkg)
{
    for (size_t i = 0; i < state_count; i++) {
        if (strcmp(states[i].pkg, pkg) == 0)
            return (ssize_t)i;
    }

    return -1;
}

static bool disable_pkg(const char *pkg)
{
    if (!package_exists(pkg)) {
        log_msg("[跳过] %s 未安装\n", pkg);
        return true;
    }

    bool enabled = true;
    bool st_ok = package_enabled(pkg, &enabled);

    if (!st_ok)
        log_msg("[警告] 无法读取 %s 状态，尝试直接禁用\n", pkg);

    ssize_t found = find_state(pkg);
    size_t idx;

    if (found < 0) {
        if (state_count >= MAX_FREEZE) {
            log_msg("[错误] 状态缓存已满\n");
            return false;
        }

        idx = state_count++;

        memset(&states[idx], 0, sizeof(states[idx]));

        copy_str(states[idx].pkg, MAX_PKG, pkg);
        states[idx].was_enabled = st_ok ? enabled : true;
    } else {
        idx = (size_t)found;
    }

    if (st_ok && !enabled) {
        log_msg("[信息] %s 已处于禁用状态，跳过\n", pkg);
        return true;
    }

    // 依次尝试 disable、disable-user 两种命令，直到状态确认已禁用
    const char *cmds[] = { "disable", "disable-user", NULL };

    for (size_t i = 0; cmds[i]; i++) {
        int rc = run_pm(cmds[i], pkg);

        bool now = true;
        bool v_ok = package_enabled(pkg, &now);

        if (v_ok && !now) {
            states[idx].modified = true;
            log_msg("[成功] 禁用 %s（命令 %s）\n", pkg, cmds[i]);
            return true;
        }

        if (rc != 0)
            continue;

        log_msg("[警告] %s 命令退出码为 0 但状态未变，尝试备用命令\n", pkg);
    }

    log_msg("[失败] 禁用 %s：所有命令均未生效\n", pkg);
    return false;
}

static bool enable_pkg(const char *pkg)
{
    ssize_t found = find_state(pkg);

    if (found < 0)
        return true;

    size_t idx = (size_t)found;

    if (!states[idx].modified)
        return true;

    if (!package_exists(pkg)) {
        states[idx].modified = false;
        log_msg("[跳过] %s 已不存在\n", pkg);
        return true;
    }

    bool enabled;

    if (!package_enabled(pkg, &enabled)) {
        log_msg("[警告] 无法读取 %s 当前状态\n", pkg);
        return false;
    }

    if (enabled) {
        states[idx].modified = false;
        log_msg("[信息] %s 已被重新启用\n", pkg);
        return true;
    }

    if (states[idx].was_enabled) {
        if (run_pm("enable", pkg) == 0) {
            states[idx].modified = false;
            log_msg("[成功] 恢复 %s\n", pkg);
            return true;
        }

        log_msg("[失败] 恢复 %s\n", pkg);
        return false;
    }

    states[idx].modified = false;
    return true;
}

/* 前向声明：cleanup_script_leftovers 定义在 handle_event 之前，
 * 供 activate 首次进入目标应用时调用 */
static void cleanup_script_leftovers(void);

static void activate(void)
{
    if (active)
        log_msg("[配置/前台更新] 重新执行禁用\n");
    else {
        /* 触发（首次进入）目标应用时：清理脚本/终端及其子孙残留进程，
         * 保证进入禁用模式后后台无无关工作负荷。其余时机不调用。 */
        cleanup_script_leftovers();
        log_msg("[进入目标应用] 开始批量禁用\n");
    }

    for (size_t i = 0; i < cfg.freeze_count && running; i++)
        disable_pkg(cfg.freeze[i].name);

    active = true;
}

static void deactivate(void)
{
    if (!active)
        return;

    log_msg("[离开目标应用] 开始恢复\n");

    for (size_t i = 0; i < state_count && running; i++)
        enable_pkg(states[i].pkg);

    active = false;
}

static void cleanup(void)
{
    log_msg("[退出] 恢复 Guard 修改过的应用\n");

    for (size_t i = 0; i < state_count; i++) {
        if (states[i].modified)
            enable_pkg(states[i].pkg);
    }

    active = false;
}

static bool extract_component(const char *line,
                              char *comp,
                              size_t comp_size)
{
    if (!line || !comp || comp_size == 0)
        return false;

    comp[0] = '\0';

    const char *begin = strchr(line, '[');

    if (!begin)
        return false;

    begin++;

    const char *comma = strchr(begin, ',');

    if (!comma)
        return false;

    const char *field = comma + 1;
    const char *end = strchr(field, ',');

    if (!end)
        end = strchr(field, ']');

    if (!end)
        return false;

    size_t len = (size_t)(end - field);

    if (len == 0 || len >= comp_size)
        return false;

    memcpy(comp, field, len);
    comp[len] = '\0';

    trim(comp);

    return comp[0] != '\0';
}

static bool comp_matches(const char *comp,
                         const Package *list,
                         size_t count)
{
    if (!comp)
        return false;

    size_t clen = strlen(comp);

    for (size_t i = 0; i < count; i++) {
        size_t plen = strlen(list[i].name);

        if (clen < plen)
            continue;

        if (strncmp(comp, list[i].name, plen) != 0)
            continue;

        char c = comp[plen];

        if (c == '\0' || c == '/' || c == '.')
            return true;
    }

    return false;
}

/* ============================================================
 * cleanup_script_leftovers() —— 触发目标应用时清理脚本残留进程
 *
 * 识别策略（按可靠性从高到低）：
 *   1. 主判据：进程 environ 中存在 GUARD_TASK=1（Java 端在所有脚本/终端/
 *      ELF 子进程的 ProcessBuilder / export 中强制注入，后代默认继承）。
 *   2. 为了覆盖「脚本在自身内部 unset GUARD_TASK 再后台拉 daemon」的
 *      逃逸场景，额外递归寻找种子进程的全部后代（子/孙/曾孙…）。
 *   3. 白名单：系统关键进程（PID 0/1/2、守护自己、守护父进程、
 *      zygote64/zygote/app_process/system_server 等）一律跳过，
 *      避免触发目标应用时把系统/应用 UI 带挂。
 *
 * 杀伤流程：
 *   - 先按「后代 → 祖先」顺序对每个 pid 发送 SIGTERM（15）
 *   - usleep(400ms)，给脚本/进程处理退出机会
 *   - 仍存活的 pid 再发送 SIGKILL（9）强制清理
 *
 * 说明：只在进入目标应用的瞬间调用一次，其余时机不清理，
 *       既不误伤也不影响脚本正常运行时的持续进程。
 * ============================================================ */
#define MAX_PROC       32768
#define MAX_NAME       64

typedef struct {
    pid_t pid;
    pid_t ppid;
    uid_t uid;
    bool  is_kernel;          // kernel 线程（Name 含 '['）
    bool  has_guard_task;     // environ 中 GUARD_TASK=1
    bool  critical;           // 系统关键进程（绝不杀）
    bool  is_guard_self;      // Guard 应用自身的 UI/Service 进程（允许杀，只留守护）
    bool  protect;            // 用户"管理类二进制"白名单（绝不杀），GUARD_TASK=1 且 非包装层且确认为真实原生二进制
    bool  is_script_wrapper;  // 进程名属于 shell/su/工具包装层（可作为清理种子）
    bool  is_jvm_app;         // cmdline/exe 表明是 zygote 派生的 Android App（app_process/java/dalvikvm），不计入"用户二进制"
    char  cmdline0[MAX_PKG];  // cmdline 首段（常等于包名或 "包名:service"）
    char  exe_link[MAX_PATH]; // /proc/<pid>/exe 符号链接目的（能读到则填；读不到空串）
    char  name[MAX_NAME];
} ProcRecord;

/* 判定一个进程"更像 Android 应用（Java 虚拟机）进程"，而不是用户自己通过
 * 脚本 exec 替换加载的原生 ELF。
 * 脚本里 `export GUARD_TASK=1` 之后如果 run-as、am、或脚本里通过 app_process
 * 拉起了某个 App 的组件，那么这些 App 进程会继承 GUARD_TASK=1，之前只按
 * 「GUARD_TASK=1 且 !wrapper」算 protect 会导致 protect_n 虚高（用户反馈 50/52）。
 * 现在在 Phase B protect 之前先通过 exe_link 和 cmdline0 排除：
 *   - exe 链接到 system 分区的 app_process32 / app_process64 / dalvikvm /
 *     dalvikvm32 / art 等 JVM 启动器；
 *   - cmdline0 典型形态「com.xxx.yyy」或「process:acra」「process:xxxservice」
 *     这种典型包名+冒号子进程名，且 exe 指向 app_process 系列。 */
static bool looks_like_android_app_process(ProcRecord *r)
{
    if (!r) return false;

    const char *ex = r->exe_link;

    /* 1) 通过 /proc/<pid>/exe 识别：只要链接到 app_process* / dalvikvm* / art */
    if (ex && ex[0]) {
        const char *base = strrchr(ex, '/');
        base = base ? base + 1 : ex;
        if (strncmp(base, "app_process", 11) == 0) return true;
        if (strncmp(base, "dalvikvm", 8) == 0) return true;
        if (strncmp(base, "art", 3) == 0 && (base[3] == '\0' || base[3] == '6' || base[3] == '3')) return true;
        if (strcmp(base, "java") == 0) return true;
    }

    /* 2) cmdline0 特征匹配：「包名:子进程」或者「形如 com.xxx 的包名」且短名
     *    就是 app_process*，双重避免误伤。*/
    if (r->cmdline0[0]) {
        /* 含有冒号子进程后缀且 cmdline0 形如 pkg:xxx => Android 系统特征 */
        const char *colon = strchr(r->cmdline0, ':');
        if (colon && colon != r->cmdline0) return true;
        /* 典型包名：至少包含两个点，且以字母/下划线开头，如 com.example.guard */
        int dots = 0;
        for (const char *p = r->cmdline0; *p; p++) if (*p == '.') dots++;
        if (dots >= 2) {
            bool ok_start = ((*r->cmdline0 >= 'a' && *r->cmdline0 <= 'z') ||
                             (*r->cmdline0 >= 'A' && *r->cmdline0 <= 'Z') ||
                             (*r->cmdline0 == '_'));
            if (ok_start) {
                /* 再配合 name 匹配更稳：/proc/status Name 就是 app_process 被截断
                 * 的短名 "app_process"（11 字节，刚好没超过 TASK_COMM_LEN=15）*/
                if (strncmp(r->name, "app_process", 11) == 0) return true;
            }
        }
    }
    return false;
}

static bool is_critical_by_name(const char *name)
{
    if (!name || !name[0])
        return false;

    static const char * const list[] = {
        "init", "kthreadd", "migration", "ksoftirqd",
        "kworker", "rcu", "watchdog", "oom_reaper",
        "writeback", "kblockd", "cryptd", "netd",
        "ueventd", "vold", "lmkd", "hwservicemanager",
        "servicemanager", "healthd", "zygote", "zygote64",
        "zygote32", "system_server", "surfaceflinger",
        "audioserver", "cameraserver", "drm",
        "installd", "keystore", "storaged", "logd",
        "logcat",
        /* Android 核心应用/系统 UI：进程名截断到 15 字节时也尽量按短名匹配 */
        "systemui", "com.android.systemui", /* 大部分 ROM 上 /proc/<pid>/status Name: 是 "com.android.sy"，见 cmdline 判定 */
        "android.process.acore",
        "android.process.media",
        NULL
    };

    for (size_t i = 0; list[i]; i++) {
        if (strcmp(name, list[i]) == 0)
            return true;
    }

    return false;
}

/* 基于 cmdline0（包名）判断"绝对不能碰"的核心系统进程。
 * 因为 /proc/<pid>/status Name 最长 15 字节（TASK_COMM_LEN），SystemUI / 系统 App
 * 的长包名会被截断（如 com.android.systemui → com.android.sy），单靠 name 匹配
 * 是漏网的；之前误引入 "uid>=10000 全部送 kill" 就把 SystemUI 一起杀掉了，
 * 导致用户反馈"系统界面直接被重启"。这里按完整 cmdline 前缀兜底。 */
static bool is_critical_by_cmdline(const char *cmd)
{
    if (!cmd || !cmd[0]) return false;

    /* 前缀式匹配（包含主进程与 :xxx 子进程，例如 com.android.systemui:screenshot） */
    static const char * const prefix_list[] = {
        "com.android.systemui",          /* SystemUI（状态栏/导航栏/锁屏），杀了会重启并报 FORTIFY */
        "com.android.settings",          /* 系统设置 */
        "com.android.launcher",          /* 桌面（AOSP 原生 Launcher3 常见前缀） */
        "com.oplus.launcher",            /* Oplus/ColorOS 桌面 */
        "com.coloros.launcher",          /* 旧 ColorOS 桌面 */
        "android.process.acore",         /* 联系人/电话核心 */
        "android.process.media",         /* 媒体扫描/存储提供器 */
        "com.android.providers.",        /* 所有系统 Provider */
        "com.android.phone",             /* 电话进程 */
        "com.android.server.telecom",    /* 电信服务 */
        NULL
    };
    for (size_t i = 0; prefix_list[i]; i++) {
        size_t pl = strlen(prefix_list[i]);
        if (strncmp(cmd, prefix_list[i], pl) == 0) {
            /* 要么正好等于前缀长度，要么下一个字符是子进程分隔符 ':' 或 '/' */
            char next = cmd[pl];
            if (next == '\0' || next == ':') return true;
        }
    }
    return false;
}

/* 进程名是否属于"脚本包装层/Android 工具包"。仅这一类 GUARD_TASK=1 进程会被
 * 当作"脚本残留种子"清理；其余 GUARD_TASK=1 进程（用户自定义二进制、
 * 被管理员脚本后台的长期程序）一律进白名单，连祖先一起保护不杀，
 * 避免把"通过脚本管理的二进制程序"被误清。*/
static bool is_script_wrapper_name(const char *name)
{
    if (!name || !name[0])
        return false;

    static const char * const list[] = {
        "sh","su","toybox","toolbox","busybox","bash","ash","dash",
        "ksh","mksh","zsh","csh","tcsh",NULL
    };

    for (size_t i = 0; list[i]; i++) {
        if (strcmp(name, list[i]) == 0)
            return true;
    }
    return false;
}

static bool read_environ_has_guard_task(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    /* 16KB 缓冲改用堆分配，避免 NDK stack protector 对大栈数组的误触发 */
    enum { ENV_BUF_SZ = 16384 };
    char *buf = (char *)calloc(1, ENV_BUF_SZ);
    if (!buf) {
        close(fd);
        return false;
    }

    ssize_t n = read(fd, buf, ENV_BUF_SZ - 1);
    close(fd);

    if (n <= 0) {
        free(buf);
        return false;
    }

    buf[n] = '\0';

    // environ 是 '\0' 分隔的多个 "K=V" 字符串；逐段比对前缀
    const char *p = buf;
    const char *end = buf + n;

    while (p < end) {
        size_t seg_len = strnlen(p, (size_t)(end - p));
        if (seg_len == 0)
            break;
        if (strncmp(p, "GUARD_TASK=", 11) == 0) {
            free(buf);
            return true;
        }
        p += seg_len + 1;
    }

    free(buf);
    return false;
}

/* 读取 /proc/<pid>/cmdline 的 argv[0]（NUL 分隔首段），最多写入 out_sz-1 字节，
 * 用于区分同一个 app_process 可执行名下的不同应用（包名就是 cmdline[0]） */
static bool read_cmdline_first(pid_t pid, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return false;
    out[0] = '\0';

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return false;

    buf[n] = '\0';
    size_t fl = strnlen(buf, (size_t)n);
    if (fl == 0)
        return false; /* 内核线程 cmdline 为空 */
    size_t cp = fl < (out_sz - 1) ? fl : (out_sz - 1);
    memcpy(out, buf, cp);
    out[cp] = '\0';
    return true;
}

/* 判断是否是 Guard 应用自身的进程（包名或子进程服务名）：
 *   - uid == cfg.appuid（同 Linux UID，应用沙箱识别）
 *   - 并且 cmdline 前缀匹配 Guard 包名（com.example.guard 或 com.example.guard:xxx）
 *   - 或者 Name 是 app_process / app_process32 / app_process64 且 uid 吻合（多
 *     种 ROM 上 zygote fork 的 app 主进程名可能没改到 app_process，只能靠 uid）
 *  仅用于触发目标应用时「杀自身应用 UI，只留守护」。 */
static bool is_guard_app_process(int appuid, uid_t uid,
                                 const char *name, const char *cmdline0)
{
    if (appuid < 10000)
        return false; /* Android 应用 UID >= FIRST_APPLICATION_UID (10000)；root/系统账号勿误判 */
    if ((int)uid != appuid)
        return false;

    /* cmdline 明确是 Guard 包名（含 :service 多进程） */
    if (cmdline0 && cmdline0[0]) {
        if (strcmp(cmdline0, "com.example.guard") == 0)
            return true;
        if (strncmp(cmdline0, "com.example.guard:", 18) == 0)
            return true;
    }

    /* cmdline 读不到时兜底：app_process 主进程名 + 同 UID，基本上就是 Guard 自己 */
    if (name && name[0]) {
        if (strcmp(name, "app_process") == 0 ||
            strcmp(name, "app_process32") == 0 ||
            strcmp(name, "app_process64") == 0)
            return true;
    }

    return false;
}

/* ---- 纯系统调用 /proc 目录遍历器（零 FILE* 依赖） ----
 *
 * 用 open("/proc", O_RDONLY|O_DIRECTORY|O_CLOEXEC) 拿到裸 fd，
 * 然后用 syscall(SYS_getdents64, fd, buf, sizeof(buf)) 逐条取 linux_dirent64。
 * 完全绕开 opendir/readdir/closedir/rewinddir（它们在 Bionic 内部用 FILE*），
 * 从根源上消除 Bionic FORTIFY "pthread_mutex_lock on destroyed mutex" 触发点。
 *
 * callback 原型：bool cb(const char *d_name, void *ctx)
 *   - 仅对纯数字 d_name 调用（跳过 ., .., sys, self, timer_list 等）
 *   - 返回 true：继续；返回 false：立即终止遍历
 *
 * 返回值：成功遍历完整条链路 = true；中途被 cb 终止或错误 = false
 *
 * 为什么不直接 read() /proc？read 也能读到目录项但不保证对齐且容易
 * 漏掉内核新条目；getdents64 是 Linux 内核官方目录遍历接口。*/
typedef bool (*proc_dir_cb)(const char *d_name, void *ctx);

static bool proc_iterate(proc_dir_cb cb, void *ctx)
{
    int fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;

    char buf[4096];
    bool ok = true;

    while (1) {
        ssize_t n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (n <= 0) break;

        size_t pos = 0;
        while (pos < (size_t)n) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + pos);
            const char *name = de->d_name;

            if (name[0] >= '0' && name[0] <= '9') {
                if (!cb(name, ctx)) { ok = false; goto done; }
            }
            pos += de->d_reclen;
        }
    }

done:
    close(fd);
    return ok;
}

/* ---- 前向声明：ScanCtx / CtxCheck / cb_* 在 read_proc_status 之前引用它 ---- */
static bool read_proc_status(pid_t pid, char *name_out, size_t name_sz,
                             pid_t *ppid_out, uid_t *uid_out, bool *is_kernel_out);

/* ---- /proc 主扫描 context：cleanup_script_leftovers 第一次遍历 ---- */
typedef struct {
    ProcRecord *procs;
    size_t     *count;
    size_t      capacity;
    pid_t       me;
    pid_t       my_pp;
} ScanCtx;

static bool cb_scan_main(const char *d_name, void *pctx)
{
    ScanCtx *c = (ScanCtx *)pctx;
    if (*c->count >= c->capacity) return false;

    int pidi = atoi(d_name);
    if (pidi <= 0) return true;
    pid_t pid = (pid_t)pidi;

    ProcRecord *r = &c->procs[*c->count];
    memset(r, 0, sizeof(*r));
    r->pid = pid;

    if (!read_proc_status(pid, r->name, sizeof(r->name),
                          &r->ppid, &r->uid, &r->is_kernel))
        return true;

    r->is_script_wrapper = is_script_wrapper_name(r->name);

    if (pid == 1 || pid == 2 || pid == c->me || pid == c->my_pp) {
        r->critical = true;
    } else if (r->is_kernel) {
        r->critical = true;
    } else if (is_critical_by_name(r->name)) {
        r->critical = true;
    }

    if (!r->critical) {
        (void)read_cmdline_first(pid, r->cmdline0, sizeof(r->cmdline0));
        if (is_critical_by_cmdline(r->cmdline0)) r->critical = true;
    }

    if (!r->critical && !r->is_kernel) {
        char exepath[48];
        snprintf(exepath, sizeof(exepath), "/proc/%d/exe", (int)pid);
        ssize_t rl = readlink(exepath, r->exe_link, sizeof(r->exe_link) - 1);
        if (rl < 0) rl = 0;
        r->exe_link[rl] = 0;
    }
    if (!r->critical) {
        r->is_jvm_app = looks_like_android_app_process(r);
    }

    if (!r->critical)
        r->has_guard_task = read_environ_has_guard_task(pid);

    if (!r->critical) {
        r->is_guard_self = is_guard_app_process(cfg.appuid,
                                                r->uid,
                                                r->name,
                                                r->cmdline0[0] ? r->cmdline0 : NULL);
    }

    (*c->count)++;
    return true;
}

/* ---- 复核阶段 context + 三个回调 ---- */
typedef struct {
    pid_t me, my_pp;

    /* 第一次遍历：统计 */
    size_t lw, pa;
    size_t sn;
    pid_t  sp_pid[12];
    char   sp_name[12][MAX_NAME];

    /* 第二次遍历：收集残留 PID 用于 SIGKILL */
    enum { MAX_LK2 = 256 };
    size_t lk2n;
    pid_t  lk2[MAX_LK2];

    /* 第三次遍历：最终复核 */
    size_t fl, fp;
    size_t sn2;
    pid_t  sn2_pid[12];
    char   sn2_name[12][MAX_NAME];
} CtxCheck;

static bool cb_check_stat(const char *d_name, void *pctx)
{
    CtxCheck *c = (CtxCheck *)pctx;
    int pidi = atoi(d_name);
    if (pidi <= 1) return true;
    pid_t pid2 = (pid_t)pidi;

    char nm2[MAX_NAME] = {0}; pid_t pp2 = -1; uid_t ui2 = (uid_t)-1; bool is_k2 = false;
    if (!read_proc_status(pid2, nm2, sizeof(nm2), &pp2, &ui2, &is_k2)) return true;
    if (is_k2 || pidi == (int)c->me || pidi == (int)c->my_pp) return true;
    if (is_critical_by_name(nm2)) return true;

    bool wr2 = is_script_wrapper_name(nm2);
    bool t2  = read_environ_has_guard_task(pid2);

    if (t2 && wr2) {
        c->lw++;
        if (c->sn < 12) {
            c->sp_pid[c->sn] = pid2;
            snprintf(c->sp_name[c->sn], MAX_NAME, "%s", nm2);
            c->sn++;
        }
    } else if (t2 && !wr2) {
        c->pa++;
    }
    return true;
}

static bool cb_check_gather(const char *d_name, void *pctx)
{
    CtxCheck *c = (CtxCheck *)pctx;
    if (c->lk2n >= MAX_LK2) return false;
    int pidi3 = atoi(d_name);
    if (pidi3 <= 1) return true;
    pid_t pid3 = (pid_t)pidi3;
    char nm3[MAX_NAME] = {0}; pid_t pp3 = -1; uid_t ui3 = (uid_t)-1; bool is_k3 = false;
    if (!read_proc_status(pid3, nm3, sizeof(nm3), &pp3, &ui3, &is_k3)) return true;
    if (is_k3 || pidi3 == (int)c->me || pidi3 == (int)c->my_pp) return true;
    if (is_critical_by_name(nm3)) return true;
    if (is_script_wrapper_name(nm3) && read_environ_has_guard_task(pid3)) {
        c->lk2[c->lk2n++] = pid3;
    }
    return true;
}

static bool cb_check_final(const char *d_name, void *pctx)
{
    CtxCheck *c = (CtxCheck *)pctx;
    int pidi4 = atoi(d_name);
    if (pidi4 <= 1) return true;
    pid_t pid4 = (pid_t)pidi4;
    char nm4[MAX_NAME] = {0}; pid_t pp4 = -1; uid_t ui4 = (uid_t)-1; bool is_k4 = false;
    if (!read_proc_status(pid4, nm4, sizeof(nm4), &pp4, &ui4, &is_k4)) return true;
    if (is_k4 || pidi4 == (int)c->me || pidi4 == (int)c->my_pp) return true;
    if (is_critical_by_name(nm4)) return true;
    bool wr4 = is_script_wrapper_name(nm4);
    bool t4  = read_environ_has_guard_task(pid4);
    if (t4 && wr4) {
        c->fl++;
        if (c->sn2 < 12) {
            c->sn2_pid[c->sn2] = pid4;
            snprintf(c->sn2_name[c->sn2], MAX_NAME, "%s", nm4);
            c->sn2++;
        }
    } else if (t4 && !wr4) {
        c->fp++;
    }
    return true;
}

static bool read_proc_status(pid_t pid, char *name_out, size_t name_sz,
                             pid_t *ppid_out, uid_t *uid_out, bool *is_kernel_out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);

    /* 不使用 fopen/fgets/fclose：在 cleanup 复核扫描热路径上，刚被杀的进程
     * /proc/<pid>/status 高速消失，fopen→失败→fclose→fopen 循环会让 Bionic
     * FILE* 内部 pthread_mutex_t 经历 destroy→free→malloc→reuse，命中 FORTIFY
     * "pthread_mutex_lock called on a destroyed mutex" → SIGABRT → 守护崩溃。
     * 改用 open/read/close 系统调用 + 手动行解析，彻底绕开 FILE*。*/
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return false;

    buf[n] = '\0';

    char name[MAX_NAME] = {0};
    pid_t ppid = -1;
    uid_t uid = (uid_t)-1;
    bool kernel = false;

    /* 手动按 \n 切行，找 Name:/PPid:/Uid: 三个字段 */
    char *line = buf;
    char *lend;
    while (line < buf + n && (lend = memchr(line, '\n', (size_t)(buf + n - line))) != NULL) {
        *lend = '\0';

        if (strncmp(line, "Name:\t", 6) == 0) {
            const char *v = line + 6;
            size_t vl = strlen(v);
            while (vl > 0 &&
                   (v[vl - 1] == '\n' || v[vl - 1] == '\r' ||
                    v[vl - 1] == ' '  || v[vl - 1] == '\t'))
                vl--;
            size_t cp = vl < (name_sz - 1) ? vl : (name_sz - 1);
            memcpy(name, v, cp);
            name[cp] = '\0';
        } else if (strncmp(line, "PPid:\t", 6) == 0) {
            ppid = (pid_t)atoi(line + 6);
        } else if (strncmp(line, "Uid:\t", 5) == 0) {
            unsigned long ru = strtoul(line + 5, NULL, 10);
            uid = (uid_t)ru;
        }

        line = lend + 1;
    }

    /* 处理最后一行（无 \n 结尾的情况）*/
    if (line < buf + n) {
        if (strncmp(line, "Name:\t", 6) == 0) {
            const char *v = line + 6;
            size_t vl = strlen(v);
            while (vl > 0 &&
                   (v[vl - 1] == '\n' || v[vl - 1] == '\r' ||
                    v[vl - 1] == ' '  || v[vl - 1] == '\t'))
                vl--;
            size_t cp = vl < (name_sz - 1) ? vl : (name_sz - 1);
            memcpy(name, v, cp);
            name[cp] = '\0';
        } else if (strncmp(line, "PPid:\t", 6) == 0) {
            ppid = (pid_t)atoi(line + 6);
        } else if (strncmp(line, "Uid:\t", 5) == 0) {
            unsigned long ru = strtoul(line + 5, NULL, 10);
            uid = (uid_t)ru;
        }
    }

    if (name[0] == '[')
        kernel = true;

    if (name_out) snprintf(name_out, name_sz, "%s", name);
    if (ppid_out) *ppid_out = ppid;
    if (uid_out)  *uid_out  = uid;
    if (is_kernel_out) *is_kernel_out = kernel;

    return true;
}

static void cleanup_script_leftovers(void)
{
    pid_t me     = getpid();
    pid_t my_pp  = getppid();

    ProcRecord *procs = calloc(MAX_PROC, sizeof(ProcRecord));
    if (!procs) {
        log_msg("[脚本清理] 内存不足，跳过\n");
        return;
    }

    size_t count = 0;

    /* ---- 1) 遍历 /proc，收集数字 PID 的快照 ----
     * 使用 proc_iterate（open + SYS_getdents64）纯系统调用遍历，
     * 完全不碰 opendir/readdir/closedir（它们在 Bionic 内部用 FILE*，
     * FILE* 内部 pthread_mutex_t 会在 cleanup 杀完进程后被 Scudo 堆分配
     * 器复用到已 destroy 的 mutex chunk 上触发 FORTIFY 崩溃）。*/
    ScanCtx sctx = { procs, &count, MAX_PROC, me, my_pp };
    (void)proc_iterate(cb_scan_main, &sctx);

    if (count == 0) {
        free(procs);
        return;
    }

    /* ---- 2) 保护白名单 + 清理种子 + 递归后代 ----
     *   白名单保护（Phase B）：如果一个进程 GUARD_TASK=1 且它不是包装层，
     *     说明它是用户通过脚本主动启动的"管理类二进制"（后台常驻程序），
     *     这类绝不能杀；同时将其**祖先链**（沿 ppid 往上直到 init/找不到）
     *     也一并打白标，避免杀父 sh 触发 SIGHUP 误伤。
     *   清理种子（Phase C）：只有「GUARD_TASK=1 且 is_script_wrapper=1」的
     *     进程 + 「is_guard_self=1」的 App UI 进程会被标记 kill_flag（守护自己
     *     本来就是 critical=1，已经排除，不会被杀）。
     *   后代扩散（Phase D）：BFS 方式：父被 kill 且子 !critical && !protect，
     *     则子也被 kill，用来连带清包装层启动的短生命工具（cat/grep/sed 等），
     *     但不会穿透到被 protect 的用户二进制后代。
     * ---------------------------------------------------- */
    bool *kill_flag = calloc(count, sizeof(bool));
    if (!kill_flag) {
        log_msg("[脚本清理] 内存不足，跳过\n");
        free(procs);
        return;
    }

    /* ---- Phase B：白名单最小集合（仅"真实用户原生二进制正本"）----
     * 用户明确口径：除了 Guard 守护程序本身 + 用户执行的二进制程序，其余全杀。
     * protect 条件（必须全部满足）：
     *   ① !critical  —— 不跟系统底线白名单冲突
     *   ② has_guard_task = 1 —— 脚本血统（环境变量由 Java 端 export 给脚本及其后代）
     *   ③ !is_script_wrapper —— 不是 sh/su/toybox/toolbox…等脚本包装层
     *   ④ !is_jvm_app —— 不是 app_process / dalvikvm / art 等 JVM 启动的 Android App
     *                     （脚本里通过 am/app_process 拉起 App 组件时会继承 GUARD_TASK=1，
     *                      这一类不算"用户二进制"，否则 protect_n 会虚高到 50/52，
     *                      同时误让一堆 App 进程逃脱清理）
     *   ⑤ 可选兜底：/proc/<pid>/exe 能读到且不是 /system/bin /system/xbin /vendor/bin
     *                分区里的标准系统工具；这一步用来进一步把 toybox 多工具名、
     *                am/pm/cmd/wm/settings 等 Java wrapper 的 shell 形态排除。
     *   不再做「子被保护 -> 父也保护」的祖先链传播。父 sh / 中间 su / 过渡 exec
     *   全部属于"其余"，严格模式下必须清理。 */
    for (size_t i = 0; i < count; i++) {
        ProcRecord *r = &procs[i];
        if (r->critical) continue;
        if (!r->has_guard_task) continue;
        if (r->is_script_wrapper) continue;
        if (r->is_jvm_app) continue;
        /* 兜底：如果 exe 链接指向系统分区 (/system|/vendor|/product|/apex) 里的
         * 已知工具路径，说明它其实是"被脚本继承了 GUARD_TASK 的系统工具短命
         * 令"，不应当算用户二进制。典型：/system/bin/toybox、/system/bin/cmd、
         * /system/bin/sh、/system/bin/app_process64 等。 */
        if (r->exe_link[0]) {
            static const char *const sys_prefixes[] = {
                "/system/bin/", "/system/xbin/", "/vendor/bin/",
                "/product/bin/", "/apex/com.android.", NULL
            };
            bool in_sys = false;
            for (size_t k = 0; sys_prefixes[k]; k++) {
                size_t pl = strlen(sys_prefixes[k]);
                if (strncmp(r->exe_link, sys_prefixes[k], pl) == 0) {
                    in_sys = true; break;
                }
            }
            if (in_sys) continue;
        }
        r->protect = true;
    }

    /* ---- Phase C：打 kill_flag 种子（严格模式 = 除了 critical + protect 之外，
     *   只杀"明确属于脚本/App 血统"的进程）。
     *
     *  ⚠️ 已移除旧方案 (d)「uid>=AID_APP (10000) 全部作兜底种子」：
     *  /proc/<pid>/status 的 Name 字段最长 15 字节，像 com.android.systemui 会被
     *  截断成 "com.android.sy"，旧的 is_critical_by_name() 仅按短名匹配，导致
     *  SystemUI/Settings/Launcher 等长包名系统 App 从 critical 网里漏掉，再叠加
     *  10000 兜底就会连带一起 kill，出现用户反馈"系统界面直接被重启"，且 Bionic
     *  FORTIFY 会在 SystemUI 被杀时打印 pthread_mutex_lock on destroyed mutex。
     *
     *  现在种子只保留血统明确的 3 类：
     *   (a) is_guard_self = 1              → Guard 应用 UI/Service/子进程（只留守护二进制）
     *   (b) GUARD_TASK=1 && wrapper = 1    → 脚本血统下的 su/sh/toybox/toolbox…包装层
     *   (c) GUARD_TASK=1 && !wrapper && !protect → Phase B 异常兜底（理论不会发生）
     *
     *  SystemUI 等关键系统进程现在通过 is_critical_by_cmdline() 读完整 cmdline[0]
     *  前缀来兜底标记 critical，不会再进入种子或扩散下游。 */
    size_t seeds_wrapper = 0, seeds_app = 0, seeds_extra = 0;
    for (size_t i = 0; i < count; i++) {
        ProcRecord *r = &procs[i];
        if (r->critical) continue;
        if (r->protect)  continue;

        if (r->is_guard_self) {
            kill_flag[i] = true;
            seeds_app++;
            continue;
        }

        bool wrapper_seed = r->has_guard_task && r->is_script_wrapper;
        if (wrapper_seed) {
            kill_flag[i] = true;
            seeds_wrapper++;
            continue;
        }

        if (r->has_guard_task) {
            kill_flag[i] = true;
            seeds_extra++;
            continue;
        }

        /* 不再按 uid>=10000 范围兜底，避免误伤 SystemUI/Launcher 等系统 App */
    }
    (void)seeds_extra;
    /* ---- Phase D：后代传递 kill_flag（父杀 -> 子杀，除非子被 protect/critical）。
     *   重要边界：如果父进程是 critical（例如守护自己的父进程 su、或是 zygote /
     *   system_server 等系统进程），父即便被某个 seed 误命中 kill_flag 也不会真
     *   的执行 kill，所以这里"父 kill_flag=true → 子继承"时要额外验证父并不是
     *   critical，避免以 critical 作"扩散锚点"把整个系统树吞进来。*/
    bool kill_changed;
    do {
        kill_changed = false;
        for (size_t i = 0; i < count; i++) {
            ProcRecord *r = &procs[i];
            if (kill_flag[i] || r->critical || r->protect)
                continue;
            pid_t ppid = r->ppid;
            if (ppid <= 0)
                continue;
            for (size_t j = 0; j < count; j++) {
                if (procs[j].pid == ppid && kill_flag[j] && !procs[j].critical) {
                    kill_flag[i] = true;
                    kill_changed = true;
                    break;
                }
            }
        }
    } while (kill_changed);

    /* ---- 3) 深度层级，按深度从大到小排序，保证先杀子再杀父 ---- */
    int  *depth  = calloc(count, sizeof(int));
    int  *order  = calloc(count, sizeof(int));
    if (!depth || !order) {
        log_msg("[脚本清理] 内存不足，跳过\n");
        free(depth); free(order); free(kill_flag); free(procs);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        order[i] = (int)i;
        if (!kill_flag[i]) { depth[i] = -1; continue; }
        int d = 0;
        size_t cur = i;
        /* 防止循环（极端异常），限制最多 255 层 */
        while (d < 255) {
            pid_t ppid = procs[cur].ppid;
            if (ppid <= 0) break;
            size_t j;
            for (j = 0; j < count; j++) {
                if (procs[j].pid == ppid) break;
            }
            if (j == count) break;
            if (!kill_flag[j]) break;
            cur = j;
            d++;
        }
        depth[i] = d;
    }

    /* 按深度降序（插入排序；进程数通常小） */
    for (size_t i = 1; i < count; i++) {
        int t = order[i];
        ssize_t j = (ssize_t)i - 1;
        while (j >= 0 && depth[order[j]] < depth[t]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = t;
    }

    /* ---- 4) 收集 kill 列表，顺便统计保护数量 ----
     * 注意 protect_n 口径：只数"真正的用户二进制白名单正本"（GUARD_TASK=1 且
     * 非脚本包装层）。祖先链传播得到的 protect 副本/App UI/critical 不算在内，
     * 否则会把同一次清理的保护数虚高，并随调用次数/脚本累积显得"越来越多"，
     * 对用户判断自己后台二进制是否都被保护造成误导。 */
    pid_t *kill_list = calloc(MAX_PROC, sizeof(pid_t));
    size_t kill_n = 0;
    size_t protect_n = 0;
    size_t protect_raw_n = 0;
    if (!kill_list) {
        log_msg("[脚本清理] 内存不足，跳过\n");
        free(kill_list); free(depth); free(order); free(kill_flag); free(procs);
        return;
    }

    for (size_t oi = 0; oi < count; oi++) {
        size_t idx = (size_t)order[oi];
        if (procs[idx].critical) continue;         /* 绝对底线：守护/系统/关键进程即便误命中 flag 也不入 kill_list */
        if (procs[idx].pid == me) continue;        /* 保险：守护 PID 不杀 */
        if (procs[idx].pid == my_pp) continue;     /* 保险：父进程（su） 不杀 */
        if (procs[idx].protect) {
            protect_raw_n++;
            /* 严格口径：只有 Phase B 的"真实原生二进制正本"才算白名单 */
            if (procs[idx].has_guard_task && !procs[idx].is_script_wrapper && !procs[idx].is_jvm_app)
                protect_n++;
            continue;
        }
        if (!kill_flag[idx])
            continue;
        if (kill_n < MAX_PROC)
            kill_list[kill_n++] = procs[idx].pid;
    }

    (void)protect_raw_n;

    /* 调试辅助：将前 10 个被白名单保护的进程 (pid,name,exe) 追加到日志里，
     * 方便用户/我们目测 protect_n 虚高时"到底保护了谁"。*/
    if (protect_n > 0) {
        size_t shown = 0;
        char line[2048];
        int l = 0;
        l += snprintf(line + l, sizeof(line) - (size_t)l, "[脚本清理] 白名单样本(至多10)：");
        for (size_t i = 0; i < count && shown < 10 && (size_t)l < sizeof(line) - 32; i++) {
            ProcRecord *pr = &procs[i];
            if (!pr->protect) continue;
            if (!(pr->has_guard_task && !pr->is_script_wrapper && !pr->is_jvm_app)) continue;
            const char *ex = pr->exe_link[0] ? pr->exe_link : (pr->cmdline0[0] ? pr->cmdline0 : "-");
            l += snprintf(line + l, sizeof(line) - (size_t)l,
                          "%spid=%d name=%s exe=%s",
                          shown ? " | " : "",
                          (int)pr->pid, pr->name, ex);
            shown++;
        }
        if (l > 0) {
            l += snprintf(line + l, sizeof(line) - (size_t)l, "\n");
            log_msg("%s", line);
        }
    }

    if (kill_n == 0) {
        log_msg("[脚本清理] 无可清理残留（白名单保护 %zu 个用户管理进程），跳过\n", protect_n);
        free(kill_list); free(depth); free(order); free(kill_flag); free(procs);
        return;
    }

    log_msg("[脚本清理] 清理种子：包装层 %zu / App UI %zu；白名单保护 %zu 个用户进程；将清理 %zu 个进程\n",
            seeds_wrapper, seeds_app, protect_n, kill_n);

    /* ---- 5) SIGTERM（第一轮，温柔退出） ---- */
    for (size_t i = 0; i < kill_n; i++) {
        (void)kill(kill_list[i], SIGTERM);
    }

    /* 给脚本 400ms 响应时间 */
    struct timespec ts1;
    ts1.tv_sec  = 0;
    ts1.tv_nsec = 400L * 1000L * 1000L;
    nanosleep(&ts1, NULL);

    /* ---- 6) 再发 SIGKILL 处理未退出者 ---- */
    for (size_t i = 0; i < kill_n; i++) {
        pid_t pid = kill_list[i];
        /* 若已退出（kill errno ESRCH）则忽略 */
        if (kill(pid, SIGKILL) != 0 && errno == ESRCH)
            continue;
    }

    /* 回收子进程，避免僵尸（su/logcat/sh 的短生命 wrapper） */
    int ws = 0;
    while (waitpid((pid_t)-1, &ws, WNOHANG) > 0) {}

    log_msg("[脚本清理] 清理完毕，已结束 %zu 个进程（TERM→KILL）\n", kill_n);

    /* ---- 7) 清理后复核：再扫一次 /proc，确认残留与保护效果 ----
     * 统计仍存活的：
     *   a) GUARD_TASK=1 且 is_script_wrapper=1 且 !critical 且 !protect 的进程
     *      = 本该被清但没清掉的"残留包装层"（应该为 0 才算干净）
     *   b) GUARD_TASK=1 且 !is_script_wrapper 的用户二进制保护数
     *      = 仍存活的被管理后台程序（应>0，否则用户后台程序可能被杀了）
     * 同时打印残留前 12 个样本 PID/Name，便于用户判断问题。
     * ------------------------------------------------------------- */
    {
        CtxCheck cc;
        memset(&cc, 0, sizeof(cc));
        cc.me = me; cc.my_pp = my_pp;

        /* ---- 7) 清理后复核：第一次 /proc 遍历 ----
         * 统计包装层残留和用户二进制存活数。*/
        proc_iterate(cb_check_stat, &cc);
        size_t leftover_wrapper = cc.lw;
        size_t protected_alive  = cc.pa;

        /* ---- 8) 兜底再清理：若仍有残留包装层，本次复核直接逐个 SIGKILL ---- */
        if (leftover_wrapper > 0) {
            size_t kill2 = 0;

            /* 第二次 /proc 遍历：收集残留包装层 PID */
            cc.lk2n = 0;
            proc_iterate(cb_check_gather, &cc);

            for (size_t i = 0; i < cc.lk2n; i++) {
                if (kill(cc.lk2[i], SIGKILL) == 0) kill2++;
            }
            struct timespec ts2;
            ts2.tv_sec = 0; ts2.tv_nsec = 200L * 1000L * 1000L;
            nanosleep(&ts2, NULL);
            int ws2 = 0;
            while (waitpid((pid_t)-1, &ws2, WNOHANG) > 0) {}

            /* 第三次 /proc 遍历：最终复核 */
            cc.fl = 0; cc.fp = 0; cc.sn2 = 0;
            proc_iterate(cb_check_final, &cc);
            size_t final_leftover = cc.fl;
            size_t final_protect  = cc.fp;

            if (final_leftover == 0) {
                log_msg("[脚本清理] 复核残留 %zu 个，兜底 SIGKILL %zu 个后最终残留=0，用户二进制仍存活 %zu 个\n",
                        leftover_wrapper, kill2, final_protect);
            } else {
                char sb2[512]; char *p2 = sb2; size_t left2 = sizeof(sb2); *p2 = '\0';
                for (size_t i = 0; i < cc.sn2; i++) {
                    int n = snprintf(p2, left2, "%s pid=%d%s",
                                     cc.sn2_name[i], (int)cc.sn2_pid[i],
                                     (i + 1 < cc.sn2) ? ", " : "");
                    if (n <= 0 || (size_t)n >= left2) break;
                    p2 += n; left2 -= (size_t)n;
                }
                log_msg("[脚本清理] 复核残留 %zu 个，兜底 SIGKILL %zu 个后仍剩 %zu 个，用户二进制仍存活 %zu 个。样本：%s\n",
                        leftover_wrapper, kill2, final_leftover, final_protect, sb2);
            }
        } else {
            log_msg("[脚本清理] 清理后复核：包装层残留 0 个（已清理干净），用户管理二进制仍存活 %zu 个\n",
                    protected_alive);
        }
    }

    free(kill_list);
    free(depth);
    free(order);
    free(kill_flag);
    free(procs);
}

static void handle_event(const char *line)
{
    if (!line)
        return;

    bool resume =
        strstr(line, "wm_on_resume_called") != NULL;

    bool top_resumed =
        strstr(line, "wm_on_top_resumed_gained_called") != NULL;

    if (!resume && !top_resumed)
        return;

    char comp[MAX_PKG];

    if (!extract_component(line, comp, sizeof(comp)))
        return;

    // 每次事件先检查配置文件是否被修改，修改则热重载，
    // 保证服务运行中重新勾选应用也能即时生效
    bool cfg_changed = reload_if_changed();

    bool same_fg = strcmp(current_fg, comp) == 0;

    if (same_fg && !cfg_changed)
        return;

    // 配置变更且前台未变时，也按新配置重新判定
    // （可能解除目标身份，或新增了需要禁用的应用）
    if (!same_fg)
        copy_str(current_fg, sizeof(current_fg), comp);

    bool target =
        comp_matches(comp, cfg.target, cfg.target_count);

    char display[MAX_PKG];
    copy_str(display, sizeof(display), comp);

    if (target) {
        for (size_t i = 0; i < cfg.target_count; i++) {
            if (strncmp(comp, cfg.target[i].name,
                        strlen(cfg.target[i].name)) == 0) {
                copy_str(display, sizeof(display),
                         cfg.target[i].name);
                break;
            }
        }
    } else {
        char *slash = strchr(display, '/');

        if (slash)
            *slash = '\0';
    }

    log_msg("[前台事件] %s %s\n",
            display,
            target ? "【目标应用】" : "【普通应用】");

    if (target)
        activate();
    else
        deactivate();
}

static pid_t start_logcat(int *read_fd)
{
    if (!read_fd)
        return -1;

    int pipefd[2];

    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();

    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);

        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        close(pipefd[1]);

        const char *argv[] = {
            "logcat",
            "-b", "events",
            "-v", "brief",
            "-T", "1",
            "wm_on_resume_called:V",
            "wm_on_top_resumed_gained_called:V",
            "*:S",
            NULL
        };

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);

    *read_fd = pipefd[0];

    return pid;
}

static bool monitor_logcat(void)
{
    int fd;
    pid_t logcat_pid = start_logcat(&fd);

    if (logcat_pid < 0) {
        log_msg("[错误] 无法启动 logcat events 监听\n");
        return false;
    }

    log_msg("[信息] logcat events 监听已启动 PID=%d\n",
            (int)logcat_pid);

    char line[MAX_LINE];
    size_t used = 0;
    bool ok = true;

    while (running) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0
        };

        int ret = poll(&pfd, 1, -1);

        if (ret < 0) {
            if (errno == EINTR)
                continue;

            log_msg("[错误] poll() 失败 errno=%d\n", errno);
            ok = false;
            break;
        }

        if (pfd.revents & (POLLERR | POLLNVAL)) {
            log_msg("[警告] logcat events 通道异常\n");
            ok = false;
            break;
        }

        if (pfd.revents & POLLHUP) {
            log_msg("[警告] logcat events 通道关闭\n");
            ok = false;
            break;
        }

        if (!(pfd.revents & POLLIN))
            continue;

        char buf[4096];

        ssize_t n = read(fd, buf, sizeof(buf));

        if (n == 0) {
            log_msg("[警告] logcat 已退出\n");
            ok = false;
            break;
        }

        if (n < 0) {
            if (errno == EINTR)
                continue;

            log_msg("[错误] 读取 logcat 失败 errno=%d\n", errno);
            ok = false;
            break;
        }

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[used] = '\0';
                handle_event(line);
                used = 0;
                continue;
            }

            if (used < sizeof(line) - 1) {
                line[used++] = buf[i];
            } else {
                used = 0;
            }
        }

        /* 每次读一批事件后，检查是否有"Java 端请求立即清理"（SIGUSR1）：
         * 脚本执行完毕需要立刻清包装层，不再必须等到切到目标应用触发。 */
        if (cleanup_pending) {
            cleanup_pending = 0;
            log_msg("[脚本清理] 收到外部清理请求，立即清理脚本包装层\n");
            cleanup_script_leftovers();
        }
    }

    close(fd);

    if (logcat_pid > 0) {
        kill(logcat_pid, SIGTERM);

        while (waitpid(logcat_pid, NULL, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    }

    return ok;
}

static int create_config(void)
{
    FILE *fp = fopen(config_file, "w");

    if (!fp) {
        log_msg("[错误] 无法创建 %s errno=%d\n",
                config_file, errno);
        return -1;
    }

    fprintf(fp,
        "# ==================================================\n"
        "# Guard 配置\n"
        "# ==================================================\n"
        "# interval：保留配置字段，当前事件监听模式不使用\n"
        "# target：触发应用\n"
        "# freeze：进入目标后禁用\n"
        "# ==================================================\n"
        "\n"
        "interval:2\n"
        "\n"
        "# ====== 目标应用 ======\n"
        "target:com.tencent.tmgp.sgame\n"
        "target:com.tencent.ig\n"
        "target:com.tencent.tmgp.cf\n"
        "target:com.tencent.tmgp.dfm\n"
        "\n"
        "# ====== 禁用列表 ======\n"
        "freeze:me.weishu.kernelsu\n"
        "freeze:me.bmax.apatch\n"
        "freeze:bin.mt.plus\n"
        "freeze:bin.mt.plus.canary\n"
    );

    fclose(fp);

    // 以 root 身份创建的文件必须放开权限（0666），否则应用进程无法覆写（EACCES）
    if (chmod(config_file, 0666) != 0)
        log_msg("[警告] 无法修改 %s 权限 errno=%d\n",
                config_file, errno);

    log_msg("[信息] 已生成默认配置 %s\n", config_file);

    return 0;
}

static int ensure_config(void)
{
    struct stat st;

    if (stat(config_file, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            log_msg("[错误] %s 不是普通文件\n", config_file);
            return -1;
        }

        return 0;
    }

    if (errno != ENOENT) {
        log_msg("[错误] 无法访问 %s errno=%d\n",
                config_file, errno);
        return -1;
    }

    return create_config();
}

static int load_config(void)
{
    FILE *fp = fopen(config_file, "r");

    if (!fp) {
        log_msg("[错误] 无法打开 %s errno=%d\n",
                config_file, errno);
        return -1;
    }

    Config nc;
    memset(&nc, 0, sizeof(nc));
    nc.interval = DEFAULT_INTERVAL;
    nc.appuid = -1;

    char line[MAX_LINE];

    while (fgets(line, sizeof(line), fp)) {
        trim(line);

        if (!line[0] || line[0] == '#')
            continue;

        char *sep = strchr(line, ':');

        if (!sep)
            continue;

        *sep = '\0';

        char *key = line;
        char *value = sep + 1;

        trim(key);
        trim(value);

        if (!value[0])
            continue;

        if (strcmp(key, "interval") == 0) {
            char *end = NULL;

            unsigned long n =
                strtoul(value, &end, 10);

            if (end == value || *end != '\0')
                n = DEFAULT_INTERVAL;

            if (n < MIN_INTERVAL)
                n = MIN_INTERVAL;

            if (n > MAX_INTERVAL)
                n = MAX_INTERVAL;

            nc.interval = (unsigned int)n;
        }
        else if (strcmp(key, "appuid") == 0) {
            char *end = NULL;
            long n = strtol(value, &end, 10);
            if (end == value || *end != '\0')
                n = -1;
            nc.appuid = (int)n;
        }
        else if (strcmp(key, "target") == 0) {
            if (!add_target(&nc, value))
                log_msg("[警告] 忽略非法 target：%s\n", value);
        }
        else if (strcmp(key, "freeze") == 0) {
            if (!add_freeze(&nc, value))
                log_msg("[警告] 忽略非法 freeze：%s\n", value);
        }
    }

    fclose(fp);

    // 互斥保护：同一应用不能同时是 target 和 freeze（禁用正在进入的应用无意义）
    for (size_t i = 0; i < nc.freeze_count;) {
        if (in_list(nc.target, nc.target_count, nc.freeze[i].name)) {
            log_msg("[警告] %s 同时出现在 target 与 freeze，已从 freeze 移除\n",
                    nc.freeze[i].name);

            memmove(&nc.freeze[i], &nc.freeze[i + 1],
                    (nc.freeze_count - i - 1) * sizeof(nc.freeze[i]));
            nc.freeze_count--;
            continue;
        }

        i++;
    }

    if (nc.target_count == 0) {
        log_msg("[错误] target 列表为空\n");
        return -1;
    }

    if (nc.freeze_count == 0) {
        log_msg("[错误] freeze 列表为空\n");
        return -1;
    }

    // 全部解析成功后才整体生效（失败时旧配置保持不变）
    cfg = nc;

    log_msg("[配置加载完成] 目标应用 %zu 个，禁用应用 %zu 个，AppUID=%d\n",
            cfg.target_count,
            cfg.freeze_count,
            cfg.appuid);

    return 0;
}

// 检测配置文件是否被修改；修改则热重载，保证运行中重新勾选即时生效（无需重启服务）
static bool reload_if_changed(void)
{
    struct stat st;

    if (stat(config_file, &st) != 0)
        return false;

    bool changed = !cfg_stat_valid ||
                   st.st_mtime != cfg_stat.st_mtime ||
                   st.st_size != cfg_stat.st_size;

    cfg_stat = st;
    cfg_stat_valid = true;

    if (!changed)
        return false;

    log_msg("[配置变更] 正在热重载…\n");

    if (load_config() != 0) {
        log_msg("[警告] 配置重载失败，保留原配置\n");
        return false;
    }

    log_msg("[配置已更新] 目标 %zu 个，禁用 %zu 个\n",
            cfg.target_count, cfg.freeze_count);

    return true;
}

int main(int argc, char **argv)
{
    /* ==== Phase 0：fork + exec self，彻底脱离祖先 pthread 状态 ====
     *
     * 守护进程被 Java 端通过 `nohup guard cfg > guard.log 2>&1 &` 启动，
     * 链路是 Java App → su → sh → nohup → fork → exec guard。
     *
     * 根因：进程从 sh/nohup fork 出来时，**libc 内部 pthread_mutex_t 是在
     * 父进程堆里初始化过的对象被 memcpy 复制过来**。父进程里这些 mutex 已
     * 经经历过 init/lock/unlock/destroy，fork 后在守护进程里呈现"已初始化
     * 但无线程真正持有"的悬空状态。cleanup 杀完一批进程触发 Scudo 堆分配
     * 器复用同一 chunk 时，新对象的 mutex 初始化与旧 destroy 状态竞争 →
     * Bionic FORTIFY "pthread_mutex_lock called on a destroyed mutex" →
     * raise(SIGABRT) → 守护崩溃。
     *
     * 解法：先检查环境变量 GUARD_DEMONIZED=1（标记已经 exec 过一次）。
     * 若未设置 → fork → 子进程设置 GUARD_DEMONIZED=1 + execve("/proc/self/exe",
     * argv, environ)。execve 是关键：它让内核重新把我们的 ELF 加载到新地址空间，
     * 同时 Bionic libc 从零初始化，所有 pthread_mutex_t 干净无状态，不再继承
     * 任何祖先悬空锁。execve 后我们的地址空间完全是自己的，不再有 su/java/
     * sh 进程的堆/栈/环境污染。
     *
     * fork 父进程（nohup/shell 的子进程）在 exec 前就 _exit()，让 init 接
     * 管子进程（ppid=1），Java 端后续 pgrep/kill-0 能正确找到我们。
     *
     * 为什么不只用 setsid？setsid 只改变 session/group 归属，不影响内存；
     * pthread 状态还是从父进程复制过来。必须 exec() 才能让内核重新初始化
     * 整个 C 运行时。
     *
     * 为什么环境变量而不是 argv？argv 被 Java 端固定传 "config_path"，
     * 加 "--daemon" 后缀需要同步改 Java 端；环境变量更干净，不需要改调用方。
     */
    const char *already_daemon = getenv("GUARD_DEMONIZED");
    if (!already_daemon || strcmp(already_daemon, "1") != 0) {
        pid_t p = fork();
        if (p < 0) {
            /* fork 失败，原始 stderr 还可用（此时还没 exec 也没重定向），
             * 用 raw_write(fd=2) 直接写系统调用，不碰 FILE*。*/
            raw_write(2, "[Guard][FATAL] pre-daemon fork failed: ");
            raw_write_i(2, errno); raw_write(2, "\n");
            return 1;
        }
        if (p > 0) {
            /* 父进程 = Java 端 nohup/shell fork 出来的第一层。
             * 它的 ppid 就是 nohup/shell，立即退出让 init 接管子进程。
             * 不能 exit()，要用 _exit() 跳过 atexit/stdio cleanup 以免
             * 触发我们还没来得及 exec 的悬空 pthread 锁。*/
            _exit(0);
        }
        /* 子进程：setsid() 成新 session leader，断开控制终端 */
        (void)setsid();

        /* 继承环境变量 + 追加 GUARD_DEMONIZED=1，防止 exec 后无限循环 */
        setenv("GUARD_DEMONIZED", "1", 1);

        /* 立即 exec 自己：/proc/self/exe 是内核提供的自引用，
         * 不管原始可执行文件是什么都能正确找到我们自己。
         * exec 后 Bionic libc 重新加载，pthread mutex 全部零初始化。
         *
         * 环境变量保留原始 environ + GUARD_DEMONIZED=1（setenv 已修改过），
         * 确保后续 popen("logcat", "r") 能找到 PATH，pm/cmd 等命令也正常。*/
        execve("/proc/self/exe", argv, environ);

        /* execve 失败（极少见：权限/SELinux/文件被删），用 raw_write 到 fd=2
         * 告诉 Java 端，然后 _exit 不用 exit()。*/
        raw_write(2, "[Guard][FATAL] execve self failed: ");
        raw_write_i(2, errno); raw_write(2, "\n");
        _exit(1);
    }

    /* 到这里，GUARD_DEMONIZED=1 说明我们已经是 exec 后的纯净守护：
     *   - 被 init 接管（ppid=1）
     *   - 独立 session leader（setsid）
     *   - Bionic libc 刚从 exec 加载，pthread mutex 全干净
     *   - 唯一的内存来源是我们自己的 ELF + 干净的 C 运行时 */

    /* 重定向 stdin/stdout/stderr 到 /dev/null，断开 exec 前的 fd 继承。
     * 守护日志完全走 log_msg() → write(log_fd, …)，不碰 stdio。*/
    {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
            if (dn > 2) close(dn);
        } else {
            close(0); close(1); close(2);
        }
    }

    if (argc >= 2 && argv[1] && argv[1][0])
        copy_str(config_file, sizeof(config_file), argv[1]);
    else
        copy_str(config_file, sizeof(config_file), DEFAULT_CONFIG);

    /* pidfile = ${config_file}.pid，Java 端 kill -0 精准确认存活；
     * crash_file = ${config_file}.crash，崩溃信号处理器把现场写到这里；
     * log_path = ${config_dir}/guard.log，守护自己 O_APPEND 打开后用 write()
     *          直写，彻底绕开 FILE* / pthread_mutex_t（fprintf(stderr,…)
     *          在 cleanup 杀完一批进程后会触发 Bionic FORTIFY 崩溃）。 */
    {
        size_t cl = strlen(config_file);
        if (cl + strlen(".pid") + 1 <= sizeof(pid_file)) {
            memcpy(pid_file, config_file, cl);
            memcpy(pid_file + cl, ".pid", 5);
            atexit(remove_pidfile);
        }
        if (cl + strlen(".crash") + 1 <= sizeof(crash_file)) {
            memcpy(crash_file, config_file, cl);
            memcpy(crash_file + cl, ".crash", 7);
        }
        /* log_path：取 config_file 的目录部分 + "guard.log" */
        const char *slash = strrchr(config_file, '/');
        if (slash) {
            size_t prefix = (size_t)(slash - config_file) + 1;
            if (prefix + strlen("guard.log") + 1 <= sizeof(log_path)) {
                memcpy(log_path, config_file, prefix);
                memcpy(log_path + prefix, "guard.log", 10);
                log_path[prefix + 10] = '\0';
            }
        }
        if (!log_path[0]) {
            copy_str(log_path, sizeof(log_path), "guard.log");
        }
    }

    /* 立刻以 O_APPEND 打开 guard.log，拿到直写 fd。
     * 之后 log_msg() 全部走 write(log_fd, …)，不再碰 FILE*。
     * 打开失败不致命：log_msg 有 raw_write(fd=2) 兜底。*/
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    /* 致命信号先落盘再死：SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL/SIGSYS/SIGTRAP。
     * 必须装在 prctl/进入 main 逻辑之前，保证任何阶段（包括 ensure_config、
     * log_msg fprintf 内部）的 FORTIFY abort 都能被正确捕获。*/
    install_crash_handlers();

    /* 双保险改进程名：prctl 失败时退回到 argv[0] 覆盖（不依赖 libc 对 comm 传参） */
    if (prctl(PR_SET_NAME, "Guard", 0, 0, 0) < 0 && argv && argv[0]) {
        static char guard_name[] = "Guard";
        argv[0] = guard_name;
        (void)prctl(PR_SET_NAME, guard_name, 0, 0, 0);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, signal_handler);

    memset(states, 0, sizeof(states));
    state_count = 0;
    active = false;
    current_fg[0] = '\0';

    /* 写 pidfile：comm 设置完成立即落盘，让 Java 端 kill -0 立刻就能验证。
     * 若崩在 ensure_config 之前，Java 端会读到 pid 但 kill -0 失败，判为未启动。 */
    write_pidfile();

    /*
     * 确保配置文件存在。
     * 已存在的配置不会覆盖。
     */
    if (ensure_config() != 0)
        return 1;

    log_msg("[Guard] 启动，配置：%s\n", config_file);

    if (load_config() != 0) {
        log_msg("[错误] 配置无效，Guard 退出\n");
        return 1;
    }

    // 记录初始配置文件的元数据，作为后续热重载的比对基准
    if (stat(config_file, &cfg_stat) == 0)
        cfg_stat_valid = true;

    // 自检：pm 命令可用性与当前身份，便于排查禁用失败
    {
        char pmtest[2048];
        const char *pm_argv[] = {
            "/system/bin/pm", "list", "packages", "-e", NULL
        };

        int pmrc = exec_cmd_rc(pmtest, sizeof(pmtest), pm_argv);

        char *pnl = strchr(pmtest, '\n');

        if (pnl)
            *pnl = '\0';

        trim(pmtest);

        log_msg("[自检] uid=%d pm=%s 首行=%s%s%s\n",
                (int)getuid(),
                pmrc >= 0 ? "可用" : "不可用",
                pmtest[0] ? "「" : "",
                pmtest[0] ? pmtest : "（无）",
                pmtest[0] ? "」" : "");
    }

    log_msg("[Guard] 就绪 PID=%d（logcat events 监听）\n",
            (int)getpid());

    monitor_logcat();

    cleanup();

    log_msg("Guard 正常退出\n");

    return 0;
}