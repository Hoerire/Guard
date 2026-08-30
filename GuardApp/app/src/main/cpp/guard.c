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

#define MAX_PATH     512
#define DEFAULT_CONFIG "/data/local/tmp/Guard/config.txt"

#define MAX_TARGET  64
#define MAX_FREEZE  128
#define MAX_PKG     256
#define MAX_LINE    4096
#define MAX_OUTPUT  65536

static char config_file[MAX_PATH];

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
static bool active;
static char current_fg[MAX_PKG];
static struct stat cfg_stat;
static bool cfg_stat_valid;

static bool reload_if_changed(void);

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void log_msg(const char *fmt, ...)
{
    char timebuf[32];
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        struct tm tmv;

        localtime_r(&ts.tv_sec, &tmv);

        strftime(timebuf, sizeof(timebuf),
                 "%Y-%m-%d %H:%M:%S",
                 &tmv);

        fprintf(stderr, "[%s.%03ld] ",
                timebuf,
                ts.tv_nsec / 1000000L);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fflush(stderr);
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
    char  cmdline0[MAX_PKG];  // cmdline 首段（常等于包名或 "包名:service"）
    char  name[MAX_NAME];
} ProcRecord;

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
        "logcat", NULL
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

    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return false;

    buf[n] = '\0';

    // environ 是 '\0' 分隔的多个 "K=V" 字符串；逐段比对前缀
    const char *p = buf;
    const char *end = buf + n;

    while (p < end) {
        size_t seg_len = strnlen(p, (size_t)(end - p));
        if (seg_len == 0)
            break;
        if (strncmp(p, "GUARD_TASK=", 11) == 0)
            return true;
        p += seg_len + 1;
    }

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
    if (appuid < 0)
        return false;
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

static bool read_proc_status(pid_t pid, char *name_out, size_t name_sz,
                             pid_t *ppid_out, uid_t *uid_out, bool *is_kernel_out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);

    FILE *fp = fopen(path, "re");
    if (!fp)
        return false;

    char line[512];
    char name[MAX_NAME] = {0};
    pid_t ppid = -1;
    uid_t uid = (uid_t)-1;
    bool kernel = false;

    while (fgets(line, sizeof(line), fp)) {
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
            const char *v = line + 6;
            ppid = (pid_t)atoi(v);
        } else if (strncmp(line, "Uid:\t", 5) == 0) {
            const char *v = line + 5;
            /* Uid: <RealUid> <EffectiveUid> <SavedSetUid> <FilesystemUid> */
            unsigned long ru = strtoul(v, NULL, 10);
            uid = (uid_t)ru;
        }
    }

    fclose(fp);

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

    /* ---- 1) 遍历 /proc，收集数字 PID 的快照 ---- */
    DIR *dp = opendir("/proc");
    if (!dp) {
        log_msg("[脚本清理] 无法打开 /proc errno=%d，跳过\n", errno);
        free(procs);
        return;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        if (count >= MAX_PROC)
            break;

        int pidi = atoi(de->d_name);
        if (pidi <= 0)
            continue;

        pid_t pid = (pid_t)pidi;
        ProcRecord *r = &procs[count];
        memset(r, 0, sizeof(*r));
        r->pid = pid;

        if (!read_proc_status(pid, r->name, sizeof(r->name),
                              &r->ppid, &r->uid, &r->is_kernel))
            continue;

        /* 绝对不能碰的白名单 */
        if (pid == 1 || pid == 2 || pid == me || pid == my_pp) {
            r->critical = true;
        } else if (r->is_kernel) {
            r->critical = true;
        } else if (is_critical_by_name(r->name)) {
            r->critical = true;
        }

        /* 主判据：GUARD_TASK 环境变量 */
        if (!r->critical)
            r->has_guard_task = read_environ_has_guard_task(pid);

        /* 次判据：Guard 应用自身的 UI / Service 进程（包名或 app_process 主进程）
         * 用户明确要求：触发目标应用时允许连自身 App 一起关掉，只留守护 */
        if (!r->critical) {
            (void)read_cmdline_first(pid, r->cmdline0, sizeof(r->cmdline0));
            r->is_guard_self = is_guard_app_process(cfg.appuid,
                                                    r->uid,
                                                    r->name,
                                                    r->cmdline0[0] ? r->cmdline0 : NULL);
        }

        count++;
    }
    closedir(dp);

    if (count == 0) {
        free(procs);
        return;
    }

    /* ---- 2) 标记要清理的 PID 集合：种子 GUARD_TASK + 所有后代 ---- */
    bool *kill_flag = calloc(count, sizeof(bool));
    if (!kill_flag) {
        log_msg("[脚本清理] 内存不足，跳过\n");
        free(procs);
        return;
    }

    /* 先标记种子：GUARD_TASK=1 进程 + Guard 应用自身（App UI/Service，只留守护） */
    for (size_t i = 0; i < count; i++) {
        ProcRecord *r = &procs[i];
        if (!r->critical && (r->has_guard_task || r->is_guard_self)) {
            kill_flag[i] = true;
        }
    }

    /* 再传递性标记后代（BFS 轮询式收敛，最多 N 轮，N<=count）
     * 每一轮：若某个进程父被标记且自己不是 critical，则自己也标记 */
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            ProcRecord *r = &procs[i];
            if (kill_flag[i] || r->critical)
                continue;
            pid_t ppid = r->ppid;
            if (ppid <= 0)
                continue;
            for (size_t j = 0; j < count; j++) {
                if (procs[j].pid == ppid && kill_flag[j]) {
                    kill_flag[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);

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

    /* ---- 4) 收集 kill 列表 ---- */
    pid_t *kill_list = calloc(MAX_PROC, sizeof(pid_t));
    size_t kill_n = 0;
    if (!kill_list) {
        log_msg("[脚本清理] 内存不足，跳过\n");
        free(kill_list); free(depth); free(order); free(kill_flag); free(procs);
        return;
    }

    for (size_t oi = 0; oi < count; oi++) {
        size_t idx = (size_t)order[oi];
        if (!kill_flag[idx])
            continue;
        if (kill_n < MAX_PROC)
            kill_list[kill_n++] = procs[idx].pid;
    }

    if (kill_n == 0) {
        free(kill_list); free(depth); free(order); free(kill_flag); free(procs);
        return;
    }

    log_msg("[脚本清理] 命中 %zu 个残留进程，开始清理…\n", kill_n);

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
    if (argc >= 2 && argv[1] && argv[1][0])
        copy_str(config_file, sizeof(config_file), argv[1]);
    else
        copy_str(config_file, sizeof(config_file), DEFAULT_CONFIG);

    prctl(PR_SET_NAME, "Guard", 0, 0, 0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    memset(states, 0, sizeof(states));
    state_count = 0;
    active = false;
    current_fg[0] = '\0';

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