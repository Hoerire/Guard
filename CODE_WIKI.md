# 自由启停（Guard）项目 Code Wiki

---

## 目录

1. [项目概述](#1-项目概述)
2. [整体架构设计](#2-整体架构设计)
3. [目录结构](#3-目录结构)
4. [模块职责详解](#4-模块职责详解)
   - 4.1 [Java App 层（MainActivity）](#41-java-app-层mainactivity)
   - 4.2 [C 原生 Guard 层](#42-c-原生-guard-层)
5. [关键类与函数说明](#5-关键类与函数说明)
   - 5.1 [MainActivity 关键函数](#51-mainactivity-关键函数)
   - 5.2 [Guard C 程序关键函数](#52-guard-c-程序关键函数)
6. [数据结构与状态机](#6-数据结构与状态机)
7. [依赖关系分析](#7-依赖关系分析)
8. [配置文件说明](#8-配置文件说明)
9. [项目运行方式](#9-项目运行方式)
10. [构建流程详解](#10-构建流程详解)
11. [核心机制详解](#11-核心机制详解)
12. [常见问题排查](#12-常见问题排查)

---

## 1. 项目概述

**自由启停（Guard）** 是一个 Android 前台应用触发式禁用工具。当用户进入指定的「目标应用」时，系统自动批量禁用一组预先配置的「禁用应用」（如 Magisk、KernelSU、MT 管理器、Termux 等 Root 或调试类工具）；当用户离开目标应用后，被禁用的应用会自动恢复启用状态。

### 核心特性

- **两段式架构**：Java App 负责配置与部署，原生 C 程序专职前台监听与系统级禁用
- **事件驱动**：通过 `logcat -b events` 感知前台应用变化，空闲开销趋近于零
- **配置热重载**：服务运行中修改配置即时生效，无需重启
- **幂等状态机**：全局状态标志保证重复进入不会重复禁用，离开不误恢复
- **脚本管理**：支持路径浏览、SU/普通权限运行、交互式输入、ELF 二进制识别
- **轻量终端**：内置 root shell 终端，支持常驻会话与脚本运行

### 技术规格

| 项目 | 规格 |
|------|------|
| 包名 | `com.example.guard` |
| minSdk | 26 (Android 8.0) |
| targetSdk | 35 (Android 15) |
| 架构 | arm64-v8a 原生架构 |
| 权限 | `QUERY_ALL_PACKAGES`（查询所有已安装应用） |
| Root 依赖 | 必须（Magisk / KernelSU / APatch 等） |

---

## 2. 整体架构设计

### 2.1 两段式架构

项目采用清晰的两段式职责分离设计：

```
┌──────────────────────────────────────────────────────────────┐
│                      Android 设备                            │
│                                                              │
│  ┌────────────────────────────┐     ┌───────────────────┐   │
│  │    自由启停 App (Java)     │     │   Guard (C 可执行) │   │
│  │                            │     │                   │   │
│  │  · UI 界面 (毛玻璃风格)    │ su  │  · 前台事件监听   │   │
│  │  · 配置管理 (读写config)   │────▶│  · logcat events  │   │
│  │  · Root 授权管理           │     │  · 禁用/恢复状态机│   │
│  │  · 服务拉起与停止          │◀────│  · 配置热重载     │   │
│  │  · 日志实时回显            │stdout│  · 信号处理       │   │
│  │  · 脚本管理 & 终端         │     │                   │   │
│  └────────────────────────────┘     └───────────────────┘   │
│         │                              ▲                     │
│         │                              │ pm disable/enable    │
│         ▼                              │                     │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              Android Framework / System Server        │   │
│  │   (PackageManager · logcat · 事件系统 · cgroup)       │   │
│  └──────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 核心数据流

1. **启动流程**：App 请求 Root → 从 assets 释放 Guard 二进制 → `su -c` 拉起 Guard → 通过 stdout 管道接收日志
2. **监听流程**：Guard 启动 logcat 子进程订阅 events buffer → poll() 阻塞等待窗口事件
3. **触发流程**：前台切换事件 → 组件名前缀匹配 → 命中目标 → 批量 pm disable → 记录 AppState
4. **恢复流程**：离开目标应用 → 遍历 modified=true 的 AppState → 按需 pm enable
5. **退出流程**：收到 SIGTERM → cleanup() 恢复所有 modified 应用 → 正常退出

---

## 3. 目录结构

```
/workspace/
├── README.md                      # 用户使用文档
├── build_arm64.sh                 # 完整 APK 构建脚本
├── guard.keystore                 # APK 签名密钥（自动生成）
├── .gitignore                     # Git 忽略规则
│
└── GuardApp/                      # Android 工程根目录
    ├── README.md                  # 工程说明文档
    ├── build.gradle               # 项目级 Gradle 配置
    ├── settings.gradle            # Gradle 设置（含仓库源）
    │
    └── app/                       # 应用模块
        ├── build.gradle           # 模块级 Gradle 配置
        │
        └── src/main/
            ├── AndroidManifest.xml     # 应用清单
            │
            ├── assets/
            │   └── guard_arm64-v8a     # 预编译的 Guard 二进制（随 APK 发布）
            │
            ├── cpp/
            │   ├── CMakeLists.txt      # CMake 构建配置（可执行目标）
            │   └── guard.c             # Guard 原生 C 源码
            │
            ├── java/com/example/guard/
            │   └── MainActivity.java   # App 全部逻辑（单 Activity 架构）
            │
            └── res/                    # Android 资源
                ├── drawable/           # 图标与背景 XML
                │   ├── bg_card.xml     # 毛玻璃卡片背景
                │   ├── bg_glass.xml    # 全局渐变毛玻璃背景
                │   ├── bg_search.xml   # 搜索框背景
                │   ├── bg_topbar.xml   # 顶部栏背景
                │   ├── ic_back.xml     # 返回图标
                │   ├── ic_file.xml     # 文件图标
                │   ├── ic_folder.xml   # 文件夹图标
                │   ├── ic_play.xml     # 播放/运行图标
                │   ├── ic_search.xml   # 搜索图标
                │   ├── ic_snow.xml     # 雪花（禁用应用）图标
                │   ├── ic_stop.xml     # 停止图标
                │   ├── ic_target.xml   # 靶心（目标应用）图标
                │   ├── ic_terminal.xml # 终端图标
                │   ├── ic_up.xml       # 上一级图标
                │   ├── ic_launcher_bg.xml  # 启动器背景
                │   └── ic_launcher_fg.xml  # 启动器前景
                ├── mipmap-anydpi-v26/
                │   └── ic_launcher.xml # 自适应启动器图标
                └── values/
                    ├── strings.xml      # 字符串资源
                    └── themes.xml       # 主题样式（浅色毛玻璃）
```

---

## 4. 模块职责详解

### 4.1 Java App 层（MainActivity）

[MainActivity.java](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java) 是应用的唯一 Activity，采用纯代码构建 UI（无 XML 布局文件），承担全部用户交互与进程管理职责。

#### 子模块划分

| 子模块 | 主要职责 | 关键函数/区域 |
|--------|----------|--------------|
| **UI 构建** | 纯代码构建 4 个页面（主页/选择器/脚本/终端） | `buildUi()`, `buildHome()`, `buildPicker()`, `buildScripts()`, `buildTerminal()` |
| **导航管理** | 4 个页面的显示/隐藏切换与返回键处理 | `showHome()`, `showPicker()`, `showScripts()`, `openTerminal()`, `onBackPressed()` |
| **应用列表** | 加载已安装应用、搜索过滤、勾选管理 | `loadAppsAsync()`, `rebuildList()`, `appRow()` |
| **Root 管理** | Root 授权请求与服务状态同步 | `requestRoot()`, `syncService()`, `suExec()` |
| **服务控制** | Guard 服务启动/停止与日志管道 | `startService()`, `stopService()` |
| **配置管理** | config.txt 读写（三层兜底写入） | `writeConfig()`, `applyConfigFromFile()`, `writeConfigAsRoot()` |
| **日志系统** | 日志追加、清空、导出到 /sdcard | `appendLog()`, `clearLog()`, `exportLog()` |
| **脚本管理** | 目录浏览（含 root ls 回退）、脚本运行对话框 | `refreshFileList()`, `rootList()`, `runScriptDialog()`, `runScript()` |
| **轻量终端** | 交互式 root shell、命令发送 | `termStart()`, `termWrite()`, `termSend()`, `appendTerm()` |
| **进程清理** | 按 cgroup 扫描并清理遗留进程 | `cleanupLeftovers()`, `pidOf()` |
| **动效系统** | Apple 风格悬浮胶囊与按压弹性动画 | `showFloat()`, `spring()` |

---

### 4.2 C 原生 Guard 层

[guard.c](file:///workspace/GuardApp/app/src/main/cpp/guard.c) 是一个独立的 C 可执行程序（非 JNI 库），由 App 通过 root 部署并拉起。

#### 子模块划分

| 子模块 | 主要职责 | 关键函数 |
|--------|----------|----------|
| **日志系统** | 带毫秒时间戳的结构化日志输出 | `log_msg()` |
| **配置管理** | 配置文件加载、验证、热重载、默认生成 | `load_config()`, `reload_if_changed()`, `create_config()`, `ensure_config()` |
| **包管理接口** | 通过 pm 命令检测应用存在/启停状态 | `package_exists()`, `package_enabled()`, `list_has_pkg()`, `run_pm()` |
| **禁用引擎** | 状态机驱动的禁用/恢复逻辑（双命令兜底） | `disable_pkg()`, `enable_pkg()`, `find_state()` |
| **事件监听** | logcat events 订阅与 poll() 阻塞消费 | `start_logcat()`, `monitor_logcat()` |
| **事件处理** | 前台组件名解析、前缀匹配、状态切换 | `handle_event()`, `extract_component()`, `comp_matches()` |
| **生命周期** | 主流程、信号处理、退出清理 | `main()`, `signal_handler()`, `cleanup()`, `activate()`, `deactivate()` |
| **子进程执行** | fork+exec+pipe 封装（捕获输出+退出码） | `exec_cmd_rc()` |

---

## 5. 关键类与函数说明

### 5.1 MainActivity 关键函数

#### 内部类

| 类名 | 字段 | 说明 |
|------|------|------|
| `AppInfo` | `icon`, `label`, `pkg`, `system`, `checkedT`, `checkedF` | 已安装应用信息容器。`checkedT`=是否为目标应用，`checkedF`=是否为禁用应用 |
| `FsEntry` | `name`, `isDir`, `size` | 文件系统条目（脚本管理浏览用） |
| `Res` | `code`, `out` | `suExec()` 返回值：退出码 + 标准输出字符串 |

---

#### UI 构建函数

**`buildUi()` [Line 124-164](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L124-L164)**

初始化根 FrameLayout 与 4 个页面容器：
- `homeScroll`（主页）、`pickerContent`（选择器）、`scriptContent`（脚本管理）、`terminalContent`（终端）
- 创建全局 `floatToast` 悬浮胶囊提示组件
- 4 个页面叠加显示，通过 `setVisibility()` 切换

**`buildHome()` [Line 183-316](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L183-L316)**

构建主页内容（从上到下）：
1. 服务状态卡（状态圆点 + 状态文本 + 副行信息）
2. 启动/停止合并切换按钮
3. 三个入口菜单卡（目标应用 / 禁用应用 / 脚本管理）
4. 运行日志框（含导出、清空按钮，毫秒时间戳日志展示）
5. 底部工作原理 + Bug 反馈链接

---

#### Root 与服务控制

**`requestRoot()` [Line 683-707](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L683-L707)**

通过 `su id` 命令校验 root 权限：
- 成功：`rootGranted=true`，调用 `syncService()` 同步 Guard 运行状态
- 失败：弹出「需要 Root 权限」对话框并引导用户退出应用

**`startService()` [Line 731-777](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L731-L777)**

启动 Guard 服务流程：
1. 检查是否已获 root，未获则提示
2. `pgrep -x Guard` 检查服务是否已运行，避免重复启动
3. `ensureBinary()` 从 assets 覆盖释放 Guard 二进制到应用私有目录
4. `ProcessBuilder("su","-c","chmod 755 <exe> ; <exe> <config_path>")` 启动
5. 独立线程读取 stdout 管道，实时转发到日志框（去除 Guard 自带时间戳前缀）

**`stopService()` [Line 779-783](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L779-L783)**

发送 `pkill -TERM -x Guard`，Guard 收到 SIGTERM 后会执行 `cleanup()` 恢复所有修改过的应用后再退出。

---

#### 配置管理

**`writeConfig()` [Line 787-820](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L787-L820)**

三层兜底写入 config.txt：
1. **第一层**：普通 `FileWriter` 直接写入
2. **第二层**：失败则删除旧文件（root 创建的文件目录删除权限通常仍保留）后重建
3. **第三层**：仍失败则使用 root 的 `cat >` 写入并 `chmod 666` 保证后续可写

**`applyConfigFromFile()` [Line 511-531](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L511-L531)**

应用启动时读取 config.txt，自动将已保存的 target/freeze 包名回显到应用列表勾选状态。**互斥规则**：同一应用同时出现在两个列表时，目标优先（`checkedF=false`）。

---

#### 脚本管理

**`runScript(File f, boolean useSu)` [Line 1163-1316](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L1163-L1316)**

交互式脚本运行核心逻辑：
1. 创建对话框：顶部 ScrollView 输出区 + 底部输入框+发送按钮
2. **SU 模式**：在 root shell 中读取文件头 4 字节判断是否为 ELF 二进制
   - 二进制：`cd <dir> && chmod +x && export LD_LIBRARY_PATH && exec <path>`
   - 脚本：`cd <dir> && <环境补全> && exec sh <path>`
   - 首行输出 `__GUARD_PID__=$$` 标记脚本 PID 用于后续清理
3. **普通模式**：ProcessBuilder 直接执行，注入 PATH/HOME/LANG/TERM 环境变量
4. 读取线程逐行转发输出（ANSI 过滤）
5. finally 块：关闭所有流、destroy 进程、调用 `cleanupLeftovers()` 深度清理

**`cleanupLeftovers(long suPid, long shPid, String scriptName)` [Line 1527-1608](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L1527-L1608)**

三段式遗留进程清理（需 root）：
1. **已知执行链 kill**：对 su 进程 PID 和脚本进程 PID 发送 SIGTERM（跳过自身 PID、父 PID）
2. **cgroup 扫描**：读取 `/proc/self/cgroup` 作为应用标识，遍历 `/proc/[pid]/cgroup` 匹配同属进程
3. **强杀兜底**：sleep 1 秒后对仍存活的匹配进程发送 SIGKILL
4. 日志输出所有清理过的 PID 与进程名，无残留时输出「无残留」

---

#### 终端模块

**`termStart()` [Line 1431-1465](file:///workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java#L1431-L1465)**

启动交互式 shell：
- Root 模式：`su -c "<SCRIPT_ENV> && sh"` 进入 root shell
- 普通模式：`/system/bin/sh` + 环境变量注入
- 支持 `pendingRunScript`：从脚本管理跳转时自动执行脚本命令
- 离开终端页会话保持，App 销毁时进程终止

---

### 5.2 Guard C 程序关键函数

#### 主流程

**`main(int argc, char **argv)` [Line 1052-1123](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L1052-L1123)**

Guard 程序入口：
1. 确定配置文件路径（命令行参数优先，否则 `/data/local/tmp/Guard/config.txt`）
2. `prctl(PR_SET_NAME, "Guard")` 设置进程名便于 `pgrep -x Guard` 查找
3. 注册信号处理器：SIGINT/SIGTERM/SIGHUP → 设置 `running=0`；SIGPIPE → 忽略
4. `ensure_config()`：配置不存在则创建默认配置（含常见游戏 target + Root 工具 freeze）
5. `load_config()`：首次加载配置，校验有效性
6. 记录初始配置文件 stat 元数据用于热重载比对
7. 自检：uid、pm 命令可用性、首行输出，写入日志便于排查
8. `monitor_logcat()`：进入事件监听主循环
9. 退出：`cleanup()` 恢复所有 modified 应用

---

#### 事件监听

**`start_logcat(int *read_fd)` [Line 697-750](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L697-L750)**

fork+exec 启动 logcat 子进程订阅事件：
```
logcat -b events -v brief -T 1 \
    wm_on_resume_called:V \
    wm_on_top_resumed_gained_called:V \
    *:S
```
- `-T 1`：从最近 1 行开始，避免历史事件重放
- `*:S`：静默所有其他 tag，只保留指定事件
- stdout 通过 pipe 传回 Guard，stderr 重定向到 /dev/null

**`monitor_logcat()` [Line 752-849](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L752-L849)**

事件消费主循环：
1. `poll(&pfd, 1, -1)` 无限阻塞等待管道可读（零 CPU 空闲）
2. 收到 EINTR 重试（信号打断）
3. `POLLERR/POLLNVAL/POLLHUP` 异常时记录日志并退出
4. 读入缓冲后逐行切分，调用 `handle_event(line)`
5. 退出时 kill logcat 子进程并 waitpid 回收

---

#### 事件处理

**`handle_event(const char *line)` [Line 632-695](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L632-L695)**

窗口事件处理核心：
1. 过滤事件类型：仅处理 `wm_on_resume_called` 和 `wm_on_top_resumed_gained_called`
2. `extract_component()` 解析日志中的组件名（包名/类名）
3. **配置热重载检查**：调用 `reload_if_changed()` 比对文件 mtime+size
4. 前台应用未变且配置未变 → 直接跳过（去抖动）
5. `comp_matches()` 前缀匹配判断是否命中目标应用列表
6. 命中 → `activate()` 批量禁用；否则 → `deactivate()` 批量恢复

**`extract_component(const char *line, char *comp, size_t comp_size)` [Line 562-603](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L562-L603)**

解析 logcat events 行格式：
```
I wm_on_resume_called: [0,com.example.app/.MainActivity,TOP_APP]
                         ↑  ↑                      ↑
                       begin comma                end/ ]
```
从 `[` 后第一个 `,` 与第二个 `,`/`]` 之间提取组件名字段。

**`comp_matches(const char *comp, const Package *list, size_t count)` [Line 605-630](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L605-L630)**

前缀匹配规则（兼容三种形态）：
- 纯包名：`com.tencent.tmgp.sgame`
- 包名/类名：`com.tencent.tmgp.sgame/.GameActivity`
- 完整类名：`com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.GameActivity`

匹配条件：`comp` 前缀等于目标包名，且紧随其后字符为 `\0`（精确包名）、`/`（类名分隔）或 `.`（子包类名）。

---

#### 禁用引擎

**`disable_pkg(const char *pkg)` [Line 416-476](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L416-L476)**

禁用单个应用（状态机驱动）：
1. `package_exists()` 用 `pm path` 检查已安装，未安装跳过
2. `package_enabled()` 读取当前启用状态
3. 查找/创建 `AppState` 条目：首次遇到则记录 `was_enabled`
4. 已禁用 → 直接返回（幂等）
5. 依次尝试命令：
   - `pm disable --user 0 <pkg>`
   - `pm disable-user --user 0 <pkg>`（备用，兼容厂商限制）
6. 每次执行后 `package_enabled()` 回读验证状态，确认禁用成功才标记 `modified=true`

**`enable_pkg(const char *pkg)` [Line 478-522](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L478-L522)**

恢复单个应用（精确保守）：
1. 无 AppState 记录 → 直接跳过（没动过的不碰）
2. `modified=false` → 跳过（本次会话没真正禁用过）
3. 已卸载 → 清除 modified 标记跳过
4. 已启用 → 清除 modified 标记跳过（可能被用户手动启用了）
5. 仅当 `was_enabled=true`（进入目标前原先是启用的）才执行 `pm enable`
   - 避免错误启用原本就是禁用状态的应用

---

#### 配置热重载

**`reload_if_changed()` [Line 1022-1050](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L1022-L1050)**

每次前台事件前自动检测：
- 比对 stat 的 `st_mtime`（修改时间）与 `st_size`（文件大小）
- 任一不同 → 触发 `load_config()` 热重载
- **原子生效**：先解析到临时 `Config nc`，全部校验通过后 `cfg = nc` 整体替换
- 解析失败 → 保留旧配置，输出警告日志

**`load_config()` [Line 919-1019](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L919-L1019)**

配置解析与校验：
1. 逐行读取：`#` 开头为注释，`key:value` 格式
2. `interval`：数值范围校验 [1, 60]，当前未使用（保留字段）
3. `target:`/`freeze:`：`valid_pkg()` 校验包名合法性（点分段规则、字符合法性）
4. **互斥保护**：同一包名同时在 target 和 freeze 时，从 freeze 列表剔除
5. 空列表检查：target 或 freeze 任一为空返回失败
6. 成功后日志输出 `[配置加载完成] 目标 X 个，禁用应用 Y 个`

---

#### 子进程执行封装

**`exec_cmd_rc(char *out, size_t out_size, const char *const argv[])` [Line 223-306](file:///workspace/GuardApp/app/src/main/cpp/guard.c#L223-L306)**

通用 fork+exec+pipe 封装：
1. `pipe()` 创建管道用于捕获子进程 stdout+stderr
2. `fork()` 子进程：dup2 将 stdout/stderr 重定向到管道写端
3. 子进程 `execvp(argv[0], argv)` 执行命令
4. 父进程循环 read 管道，将输出写入 `out` 缓冲区（截断保护）
5. `waitpid()` 等待子进程，返回退出码（异常退出返回 -1）

---

## 6. 数据结构与状态机

### 6.1 Java 层数据结构

**应用列表 `ArrayList<AppInfo> allApps`**

```java
static class AppInfo {
    Drawable icon;       // 应用图标
    String label;        // 应用名称
    String pkg;          // 包名
    boolean system;      // 是否为系统应用
    boolean checkedT;    // 是否勾选为「目标应用」
    boolean checkedF;    // 是否勾选为「禁用应用」
}
```

- 加载后按 `label` 字母序稳定排序
- 勾选互斥：同一应用 `checkedT` 与 `checkedF` 不能同时为 true
- 勾选后在列表中自动排到最前（`rebuildList()` 二次稳定排序）

---

### 6.2 C 层数据结构

**配置 `Config cfg`**（全局变量）

```c
typedef struct {
    char name[MAX_PKG];  // 包名字符串 (256 字节)
} Package;

typedef struct {
    unsigned int interval;          // 轮询间隔（保留，未使用）
    Package target[MAX_TARGET];     // 目标应用数组 (64 个)
    size_t target_count;            // 目标应用实际数量
    Package freeze[MAX_FREEZE];     // 禁用应用数组 (128 个)
    size_t freeze_count;            // 禁用应用实际数量
} Config;
```

**应用状态缓存 `AppState states[MAX_FREEZE]`**

```c
typedef struct {
    char pkg[MAX_PKG];     // 包名
    bool was_enabled;      // 进入目标前该应用是否启用（原始快照）
    bool modified;         // 本次会话 Guard 是否真的修改过该应用状态
} AppState;
```

- 首次遇到禁用对象时入缓存，记录 `was_enabled`
- `modified=true` 才会在 `deactivate()`/`cleanup()` 中被恢复
- 容量 = MAX_FREEZE = 128，与 freeze 列表上限一致

---

### 6.3 全局状态标志

| 标志 | 类型 | 说明 |
|------|------|------|
| `running` | `volatile sig_atomic_t` | 信号驱动的主循环退出标志（1=运行，0=退出） |
| `active` | `bool` | 是否处于「进入目标应用」激活状态（true=已批量禁用，false=已恢复） |
| `current_fg[MAX_PKG]` | `char[]` | 当前前台组件名缓存，用于去抖动（相同前台不重复处理） |
| `cfg_stat` / `cfg_stat_valid` | `struct stat` / `bool` | 配置文件元数据快照，热重载比对基准 |
| `serviceRunning` | `volatile boolean` (Java) | App 侧 Guard 服务运行状态 |
| `rootGranted` | `volatile boolean` (Java) | 是否已获 root 权限 |

---

### 6.4 禁用/恢复状态机

```
                          ┌──────────────────┐
                          │   启动 Guard     │
                          └────────┬─────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │   active=false              │
                    │   states[] 全部 modified=0  │
                    └──────────────┬──────────────┘
                                   │
              ┌────────────────────▼────────────────────┐
              │        前台事件命中 target 应用          │
              └────────────────────┬────────────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │   activate() 批量禁用       │
                    │                              │
                    │  对每个 freeze 应用:         │
                    │    · 记录 was_enabled        │
                    │    · 执行 disable/disable-user│
                    │    · 状态确认后 modified=true │
                    │                              │
                    │   active=true               │
                    └──────────────┬──────────────┘
                                   │
         ┌─────────────────────────┼─────────────────────────┐
         │                         │                         │
         │    再次命中同 target     │  离开（前台非 target）   │  SIGTERM 信号
         │   (active=true 跳过)    │                         │
         │                         │                         │
         ▼                         ▼                         ▼
   （无操作）           ┌───────────────────────┐   ┌────────────────────┐
                        │  deactivate() 恢复   │   │  cleanup() 全恢复  │
                        │                       │   │                    │
                        │  仅恢复:              │   │  遍历 states[]     │
                        │    modified=true 且   │   │  modified=true 的  │
                        │    was_enabled=true   │   │  全部 enable       │
                        │                       │   │                    │
                        │  active=false         │   │  active=false      │
                        └───────────┬───────────┘   └─────────┬──────────┘
                                    │                         │
                                    └────────────┬────────────┘
                                                 │
                                                 ▼
                                    回到初始 active=false
```

---

## 7. 依赖关系分析

### 7.1 外部依赖（Android SDK / NDK）

| 依赖项 | 版本 | 用途 |
|--------|------|------|
| Android Gradle Plugin | 8.6.1 | 项目级构建系统 |
| compileSdk | 35 | Java 编译目标 API |
| minSdk | 26 | 最低支持 Android 8.0 |
| targetSdk | 35 | 目标 Android 15 |
| CMake | 3.22.1 | C 原生代码构建 |
| NDK clang | - | aarch64 交叉编译（构建脚本直接调用） |
| Android Build Tools | 35.0.0 | aapt2/d8/zipalign/apksigner |
| android.jar (platform-35) | - | Java 编译类路径 |

### 7.2 系统运行时依赖（设备端）

| 依赖 | 说明 |
|------|------|
| `su` 二进制 | Magisk / KernelSU / APatch 等 Root 方案提供。App 和 Guard 的所有特权操作都通过 `su -c` |
| `pm` (PackageManager CLI) | `/system/bin/pm`，Guard 调用 `pm path/list packages/disable/disable-user/enable` |
| `logcat` | Android 系统日志工具，Guard 子进程订阅 events buffer |
| `/proc` 文件系统 | `cleanupLeftovers()` 扫描进程 cgroup、进程名 |
| `cgroup` (v1/v2) | 进程归属识别，用于遗留进程清理 |

### 7.3 内部模块依赖图

```
┌────────────────────────────────────────────────────┐
│                    MainActivity                    │
├─────────┬──────────┬──────────┬──────────┬─────────┤
│  UI 构建 │ Root管理 │ 服务控制 │ 脚本管理 │  终端   │
└────┬────┴────┬─────┴────┬─────┴────┬─────┴────┬────┘
     │         │          │          │          │
     ▼         ▼          ▼          ▼          ▼
┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐
│ 资源XML│ │suExec()│ │Guard可执行│ │ su -c  │ │ su -c  │
│Drawable│ │        │ │(assets) │ │ ls/pm/ │ │ sh     │
│Themes  │ └───┬────┘ │ 部署    │ │ cp/cat │ └───┬────┘
└────────┘     │      └────┬─────┘ └───┬────┘     │
               │           │             │          │
               ▼           ▼             ▼          ▼
         ┌─────────────────────────────────────────────┐
         │              Android 系统层                  │
         │  PackageManager · logcat · /proc · cgroup   │
         └──────────────────┬──────────────────────────┘
                            │
                            ▼
                  ┌───────────────────┐
                  │  Guard (子进程)    │
                  │                   │
                  │  ┌─────────────┐  │
                  │  │ logcat 子进程│  │
                  │  │ (events buf)│  │
                  │  └──────┬──────┘  │
                  │         │ poll()  │
                  │         ▼         │
                  │  handle_event()   │
                  │         │         │
                  │    activate() /   │
                  │   deactivate()    │
                  │         │         │
                  │         ▼         │
                  │  disable/enable   │
                  │    (pm 命令)      │
                  └───────────────────┘
```

### 7.4 关键数据流接口

| 接口 | 方向 | 格式 | 说明 |
|------|------|------|------|
| App → Guard | su -c 拉起 | `Guard <config_file_path>` | 命令行参数传递配置路径 |
| Guard → App | stdout 管道 | `[时间戳] [标签] 消息` | 实时日志流，App 端会剥去时间戳前缀 |
| App ↔ config.txt | Java File/root cat | `key:value` 行格式 | 配置读写，App 写入，Guard 热重载读取 |
| Guard → pm | execvp | `pm <cmd> --user 0 <pkg>` | 禁用/恢复/状态查询 |
| Guard ← logcat | pipe | logcat events 文本行 | 前台事件输入 |

---

## 8. 配置文件说明

### 8.1 config.txt 格式

**位置**：`/data/user/0/com.example.guard/files/config.txt`（应用私有目录，App 普通读写；Guard 以 root 身份读写）

**权限**：通常为 0666（root 创建的文件会 chmod 放开，App 写入兜底也会 chmod）

```
# Guard config
interval:2

# ====== 目标应用 ======
target:com.tencent.tmgp.sgame
target:com.tencent.ig

# ====== 禁用列表 ======
freeze:me.weishu.kernelsu
freeze:bin.mt.plus.canary
```

| 字段 | 说明 | 约束 |
|------|------|------|
| `# ...` | 注释行 | 整行忽略 |
| `interval:<N>` | 保留字段 | 范围 [1,60]，当前事件驱动模式不使用 |
| `target:<pkg>` | 触发应用包名 | 可多行，最多 64 个，必须合法包名 |
| `freeze:<pkg>` | 禁用应用包名 | 可多行，最多 128 个，必须合法包名 |
| 空行 | 忽略 | - |
| 非法行 | 忽略并告警 | 格式错误或包名不合法 |

### 8.2 包名校验规则 `valid_pkg()`

合法包名必须满足：
1. 长度 1~255 字符
2. 不以 `.` 开头，不以 `.` 结尾
3. 点号 `.` 分段，每段非空
4. 段内允许字符：`a-z` `A-Z` `0-9` `_`

### 8.3 脚本路径持久化

**位置**：`/data/user/0/com.example.guard/files/script_path.txt`

纯文本单行，保存用户上次浏览的目录路径，脚本管理页面打开时自动恢复。

---

## 9. 项目运行方式

### 9.1 环境要求

| 项目 | 要求 |
|------|------|
| 开发/构建机器 | Linux（构建脚本 `build_arm64.sh` 为 bash 脚本） |
| Android SDK | 含 platform-35 + build-tools 35.0.0（`/data/user/work/android-sdk`，脚本内可修改） |
| NDK clang | C 交叉编译工具链（需在 PATH 中可用 aarch64 目标 clang） |
| 密钥库 | `/workspace/guard.keystore`（脚本自动生成，密码 `android`，别名 `guard`） |
| 目标设备 | ARM64 架构 Android 8.0+，已 Root（Magisk/KernelSU/APatch 提供 `su`） |

### 9.2 安装使用流程

```
adb install GuardApp-arm64-v8a.apk
        │
        ▼
  打开「自由启停」App
        │
        ▼
  弹出 Root 授权框 → 允许永久授权
        │
        ▼
  主页点击「目标应用」→ 勾选触发禁用的游戏/应用
        │
        ▼
  主页点击「禁用应用」→ 勾选进入目标后要禁用的 Root 工具
        │
        ▼
  主页点击「启动服务」→ 状态栏变绿，开始监听
        │
        ▼
  前台切换到目标应用 → 日志显示批量禁用完成
        │
        ▼
  退出目标应用 → 日志显示恢复完成
```

### 9.3 服务手动启停（shell）

```bash
# 启动 Guard（配置路径按需修改）
su -c "/data/user/0/com.example.guard/files/guard /data/user/0/com.example.guard/files/config.txt"

# 停止 Guard（触发 cleanup 恢复所有应用）
su -c "pkill -TERM -x Guard"

# 检查运行状态
su -c "pgrep -x Guard && echo 运行中 || echo 未运行"
```

### 9.4 日志导出

- App 内「运行日志」框 → 点「导出」按钮
- 优先 root 写入（应用缓存 → root cp → /sdcard），回退 root cat 直写，最后普通写入
- 导出路径：`/sdcard/Guard日志_yyyyMMdd_HHmmss.txt`
- 任意文件管理器可见，Android 11+ 分区存储也可正常访问

---

## 10. 构建流程详解

完整构建由仓库根 [build_arm64.sh](file:///workspace/build_arm64.sh) 脚本实现，7 步手工构建（不依赖 Android Studio/Gradle Daemon）：

### 10.1 构建步骤详解

| 步骤 | 命令 | 产物 | 说明 |
|------|------|------|------|
| **1/7 准备目录** | `rm -rf + mkdir -p` | `$BUILD/gen`, `$BUILD/classes` | 清理旧构建 |
| **2/7 资源编译** | `aapt2 compile --dir res` | `res.zip` | 编译 drawable/values 等资源为二进制 |
| **3/7 资源链接** | `aapt2 link -I android.jar` + 清单注入包名/版本 | `app.unaligned.apk` (不含 dex) | 链接资源+清单，生成 `R.java` 到 `$BUILD/gen`，注入 assets |
| **4/7 Java 编译** | `javac -source 11 -target 11` | `classes/com/example/guard/*.class` | 编译 MainActivity.java + R.java，类路径 android.jar |
| **5/7 Dex 生成** | `d8 --release --min-api 26 --lib android.jar` | `classes.dex` | class → Dalvik Executable，release 优化 |
| **6/7 Dex 打包** | `zip -q app.unaligned.apk classes.dex` | 更新后的 `app.unaligned.apk` | 将 dex 追加到 APK（ZIP 格式） |
| **7/7 对齐+签名** | `zipalign 4` + `apksigner sign` (自动生成 keystore) | `GuardApp-arm64-v8a.apk` | 4 字节对齐优化加载，v2/v3 签名，apksigner verify 校验 |

### 10.2 构建脚本关键变量

```bash
SDK=/data/user/work/android-sdk          # Android SDK 根目录
BT=$SDK/build-tools/35.0.0                # Build Tools 目录
PLATFORM=$SDK/platforms/android-35/android.jar  # 编译用 android.jar
ROOT=/data/user/work/GuardApp             # 工程根目录
BUILD=/data/user/work/build               # 构建临时输出目录
KEYSTORE=/workspace/guard.keystore        # 签名密钥（不存在则自动生成）
```

### 10.3 Guard 原生程序单独编译

Guard C 代码的 CMake 配置 [CMakeLists.txt](file:///workspace/GuardApp/app/src/main/cpp/CMakeLists.txt)：

```cmake
cmake_minimum_required(VERSION 3.22.1)
project("guard")
add_executable(guard guard.c)
target_compile_options(guard PRIVATE -O2 -fPIE -Wextra -Wno-unused-parameter)
target_link_options(guard PRIVATE -pie)
```

编译命令等价（NDK clang 直接调用）：
```bash
aarch64-linux-android26-clang -O2 -fPIE -pie -Wextra -Wno-unused-parameter \
    guard.c -o guard_arm64-v8a
```

- **`-fPIE -pie`**：位置无关可执行（Android 5.0+ 强制要求）
- **`-O2`**：标准性能优化
- **`-Wextra`**：开启额外警告，`-Wno-unused-parameter` 抑制回调函数未用参数告警
- **动态链接 bionic**：未指定 `-static`，依赖设备上的 Android libc（体积小、兼容性好）

编译产物需复制到 `app/src/main/assets/guard_arm64-v8a` 随 APK 发布。App 每次启动服务时都会从 assets **强制覆盖**释放到 `files/guard`，确保 APK 更新后新 Guard 二进制立即生效。

---

## 11. 核心机制详解

### 11.1 前台监听机制（事件驱动 vs 轮询）

传统方案通常使用 `UsageStatsManager` 轮询查询前台应用，存在 2~5 秒延迟和持续 CPU 开销。Guard 采用 `logcat events` 订阅方案：

| 对比项 | logcat events 方案 | UsageStats 轮询方案 |
|--------|-------------------|-------------------|
| 延迟 | 事件即时触发（毫秒级） | 取决于轮询间隔（2~5 秒） |
| 空闲 CPU | poll() 阻塞，0% 占用 | 周期性唤醒，持续占用 |
| 权限要求 | root（读 logcat events buffer） | PACKAGE_USAGE_STATS + 用户手动授权 |
| 实现复杂度 | 需管理 logcat 子进程 + pipe | 简单循环查询 |
| 可靠性 | logcat 可能被系统重启（Guard 检测 POLLHUP 退出） | 稳定但延迟大 |

监听的两类事件含义：
- `wm_on_resume_called`：Activity 调用 `onResume()` 时由 ActivityTaskManager 写入
- `wm_on_top_resumed_gained_called`：顶层 resumed Activity 变更（更可靠的前台切换标志）

同时监听两者确保不同 Android 版本兼容性。

---

### 11.2 幂等状态机设计

状态机的核心目标：**任何时候重复进入/离开，应用的最终状态都正确且一致**。

```
进入目标 → 批量禁用：
  · 已禁用（manual disabled） → skip（was_enabled=false）
  · 已被 Guard 禁用（modified=true） → skip（active=true 去重）
  · 正常启用 → pm disable → modified=true, was_enabled=true

离开目标 → 批量恢复：
  · modified=false → skip（没碰过，不管）
  · was_enabled=false → skip（原本就是禁用的，用户手动禁用的）
  · 两者都 true → pm enable → modified=false

SIGTERM cleanup：
  · 只看 modified=true → 逐个 enable → modified=false
  · was_enabled=false 的 modified 条目不会出现（因为 disable 时 was_enabled=false → 已禁用 → skip → modified 保持 false）
```

---

### 11.3 配置热重载原子性

热重载采用 **临时 Config + 整体替换** 模式：

```c
// load_config() 内部
Config nc;                        // 栈上临时配置
memset(&nc, 0, sizeof(nc));       // 从零开始填充
// ... 逐行解析填充 nc ...
// ... 互斥校验 + 空列表校验 ...
cfg = nc;                         // 单条赋值原子生效（结构体拷贝）
```

解析中途任何一行失败：`return -1`，nc 被丢弃，**原 cfg 保持不变**。这保证了用户保存不完整的配置时，Guard 不会进入半配置状态。

---

### 11.4 ANSI 转义码过滤

脚本和终端输出可能包含 ANSI 颜色/控制码（如 `\x1b[1;32m` 绿色加粗、`\x1b[0m` 重置），TextView 不支持，会显示为乱码。`cleanAnsi()` 两层正则过滤：

```java
// 第一层：CSI 序列（颜色、光标移动等）\x1b[ ... 终字节
s.replaceAll("\u001B\\[[0-9;?]*[ -/]*[@-~]","")

// 第二层：OSC 序列（设置标题等）\x1b] ... \u0007 或 \x1b\\
.replaceAll("\u001B\\][^\\u0007\\u001B]*(\\u0007|\\u001B\\\\)","")
```

再去除尾部 `\r`（Windows/DOS 换行残留）。

---

### 11.5 ELF 二进制自动识别

用户可能把编译后的 ARM64 二进制命名为 `.sh` 或无扩展名，若交给 `sh` 解释会出现 ELF 头部乱码报错。两层识别机制：

| 场景 | 判定方式 |
|------|----------|
| 普通权限运行脚本 | Java 侧 `isElf()` 读取文件前 4 字节：`0x7F 'E' 'L' 'F'` → 直接 ProcessBuilder 执行，不调 sh |
| Root 权限运行脚本 | App 进程可能无权读 `/data` 等目录文件头 → **在 root shell 内**用 `head -c4 | od -An -tx1` 判定 → 二进制则 `chmod +x && exec`，脚本则 `exec sh <path>` |

Root 模式下使用 `exec` 而非直接调用：`exec` 替换当前 shell 进程，保证 PID 不变（脚本 PID = 真实进程 PID），便于后续 cgroup 清理和日志追踪。

---

## 12. 常见问题排查

| 现象 | 排查方向 | 日志线索 |
|------|----------|----------|
| **禁用不生效** | 1. 确认「目标应用」列表勾选正确；2. 查看配置热重载日志；3. 检查 pm 命令是否被厂商拦截 | 查找 `[进入目标应用]` 或 `[前台事件] xxx 【目标应用】` |
| **日志出现 `[pm] ... 退出码=X`** | X≠0 表示 pm 命令执行被系统拒绝，常见于 Device Owner / 厂商限制 | 查看退出码和输出详情 |
| **`[警告] ... 命令退出码为 0 但状态未变`** | `pm disable` 输出成功但实际未禁用（厂商 hook），Guard 已自动 fallback 到 `disable-user` | 之后应看到 `[成功] 禁用 xxx（命令 disable-user）` |
| **`[自检] uid=0`** | uid=0 表示 Guard 确实以 root 身份运行；uid≠0 则 root 授权失败 | 首行自检日志 |
| **配置保存提示 EACCES** | 旧配置文件属主为 root（Guard 首次创建），App 会自动：① 删除后重建 ② root 写入 + chmod 666 | 日志显示 `[配置] 清理旧配置失败，改用 root 写入` 或 `[配置] 已保存(root)` |
| **更新 APK 后行为未变** | Guard 二进制每次 `startService()` 都从 assets 覆盖释放，确认安装的是新 APK 即可 | 启动日志 `[系统] 服务进程已启动` |
| **脚本运行出现 ELF 乱码** | 文件头判定失败（可能脚本内容恰好以 `\x7fELF` 开头？）→ 此情况极罕见，通常是二进制文件被错误地用 su 模式之外执行 | 日志 `（检测为二进制可执行文件，直接执行）` 判定结果 |
| **停止服务后应用仍禁用** | Guard 被 SIGKILL 强杀（非 TERM），无法执行 cleanup → 重新启动服务再停止，或手动 `pm enable` | 正常停止日志应有 `[退出] 恢复 Guard 修改过的应用` |
| **终端输出乱码** | cleanAnsi() 未覆盖某些罕见转义序列 → 反馈具体输出样本 | 对比原始脚本输出 |
| **日志导出失败** | Android 11+ 分区存储 + 无 root → App 应通过 root cp/cat 兜底写入 /sdcard | 日志 `[错误] 导出日志失败：...` |

---

*文档生成时间：基于当前仓库状态（guard.c ~1123 行，MainActivity.java ~1634 行）*
