# GuardApp · 工程 README

> Android 前台应用触发式禁用工具（ARM64 原生版）。
> 两段式架构：**Java App 负责配置部署 UI，原生 C 程序 Guard 专职前台监听与系统级禁用/恢复。**

本文档面向开发者/贡献者。**用户使用文档见仓库根 `../README.md`。**

---

## 一、工程结构总览

```
GuardApp/
├── build.gradle               # 项目级 Gradle（仅声明 AGP 8.6.1，不实际执行）
├── settings.gradle            # 仓库源 + 模块 include
│
└── app/
    ├── build.gradle           # 模块级：namespace / compileSdk 35 / minSdk 26 / abiFilters arm64-v8a
    │
    └── src/main/
        ├── AndroidManifest.xml
        │    · package: com.example.guard
        │    · permission: QUERY_ALL_PACKAGES
        │    · launcher activity: .MainActivity
        │    · theme: @style/AppTheme (Material Light NoActionBar)
        │
        ├── assets/
        │   └── guard_arm64-v8a      # ⭐ 预编译的 Guard 二进制（ARM64 PIE，随 APK 发布）
        │                                  App 启动服务时会从 assets 强制覆盖释放到 files/guard
        │
        ├── cpp/
        │   ├── CMakeLists.txt        # add_executable(guard guard.c) + -O2 -fPIE -pie
        │   └── guard.c               # ⭐ Guard 原生程序（独立 main()，非 JNI！）
        │
        ├── java/com/example/guard/
        │   └── MainActivity.java     # ⭐ App 全部逻辑（单 Activity，纯代码构建 UI，无 layout XML）
        │
        └── res/
            ├── drawable/             # 背景 + 图标（均为矢量 XML，无 PNG 资源）
            ├── mipmap-anydpi-v26/    # 自适应启动器
            └── values/               # strings.xml + themes.xml
```

**三个关键源文件**：

| 文件 | 代码量 | 职责 |
|------|--------|------|
| `app/src/main/java/.../MainActivity.java` | ~1600 行 | **Java App 全部逻辑**：UI（4 个页面纯代码构建）、应用列表加载、Root 管理、Guard 服务拉起/停止、config 读写、日志系统、脚本管理（路径浏览+交互式运行+进程清理）、轻量终端 |
| `app/src/main/cpp/guard.c` | ~1120 行 | **Guard 守护进程**：logcat events 订阅 + poll() 消费、组件名前缀匹配、pm disable/enable 状态机、配置热重载（原子生效）、信号处理 + cleanup |
| `app/src/main/assets/guard_arm64-v8a` | 二进制 | **预编译 Guard 产物**，构建时从 cpp 产出复制到此处，随 APK 打包 |

---

## 二、构建指南

### 2.1 前置条件

| 依赖 | 版本要求 | 环境变量/路径（脚本默认） |
|------|----------|--------------------------|
| Android SDK Platform | `android-35` | `$SDK/platforms/android-35/android.jar` |
| Android Build Tools | `35.0.0` | `$SDK/build-tools/35.0.0`（含 aapt2/d8/zipalign/apksigner） |
| JDK | 11+（`javac --release 11`） | PATH 中可用 `javac`/`jarsigner` |
| Android NDK clang | 支持 aarch64-linux-android26 | 编译 Guard 二进制时使用（PATH 中可用 `aarch64-linux-android26-clang`） |
| keytool | 随 JDK 提供 | 自动生成签名密钥 |

**默认路径**（`build_arm64.sh` 顶部）：
```bash
SDK=/data/user/work/android-sdk
ROOT=/data/user/work/GuardApp
BUILD=/data/user/work/build
KEYSTORE=/workspace/guard.keystore   # 不存在会自动创建（storepass/android）
```
在你的环境中请先修改这四个变量。

### 2.2 Guard 二进制单独编译

CMake 方式（Android Studio 项目默认）：
```cmake
cmake_minimum_required(VERSION 3.22.1)
project("guard")
add_executable(guard guard.c)
target_compile_options(guard PRIVATE -O2 -fPIE -Wextra -Wno-unused-parameter)
target_link_options(guard PRIVATE -pie)
```

NDK clang 直接编译（用于更新 `assets/guard_arm64-v8a`）：
```bash
aarch64-linux-android26-clang \
    -O2 -fPIE -pie -Wextra -Wno-unused-parameter \
    -o GuardApp/app/src/main/assets/guard_arm64-v8a \
    GuardApp/app/src/main/cpp/guard.c
```

> 📌 **PIE 强制**：Android 5.0+ 要求所有可执行必须是 position-independent，`-fPIE -pie` 不可省略。

### 2.3 完整 APK 构建（推荐）

```bash
cd /workspace
bash build_arm64.sh
```

脚本**七步手工构建**（不依赖 Gradle/Android Studio，零守护进程开销）：

| 步骤 | 命令 | 产物 |
|------|------|------|
| 1 | 准备目录：`rm -rf $BUILD; mkdir` | 干净构建目录 |
| 2 | `aapt2 compile --dir res` | 资源编译 `res.zip` |
| 3 | `aapt2 link -I android.jar + 注入包名/版本 + assets` | `app.unaligned.apk`（未含 dex）+ `R.java` |
| 4 | `javac -source 11 -target 11 MainActivity.java R.java` | `classes/*.class` |
| 5 | `d8 --release --min-api 26 class_files` | `classes.dex` |
| 6 | `zip -q app.unaligned.apk classes.dex` | 追加 dex 到 APK |
| 7 | `zipalign 4` + `apksigner sign`（自动生成 keystore） | `$BUILD/GuardApp-arm64-v8a.apk` |

构建成功时末尾输出：
```
== 校验 ==
Verifies
Verified using v1 scheme (JAR signing): true
Verified using v2 scheme (APK Signature Scheme v2): true
...
== DONE ==
-rw-r--r-- 1 user user 2XXXXXXX 日期 GuardApp-arm64-v8a.apk
```

### 2.4 安装

```bash
adb install -r /data/user/work/build/GuardApp-arm64-v8a.apk
```

---

## 三、开发与调试

### 3.1 快速调试循环

```bash
# 1. 构建 + 安装
bash build_arm64.sh && adb install -r /data/user/work/build/GuardApp-arm64-v8a.apk

# 2. 打开 App（触发 Logcat 输出）
adb shell monkey -p com.example.guard -c android.intent.category.LAUNCHER 1

# 3. 观察 Guard 日志（App 内日志框实时显示，也可独立通过 stdout 抓）
#    Guard 自己的日志走 stderr 但被 App 的 ProcessBuilder.redirectErrorStream(true) 合并抓
adb shell su -c "grep -a '' /proc/$(pgrep -x Guard)/fd/2"  # 不推荐，直接看 App 界面

# 4. 抓原生崩溃（如果 native crash）
adb logcat -b crash | grep -i guard
adb tombstoned
```

### 3.2 关键运行时路径

| 路径 | 归属 | 说明 |
|------|------|------|
| `/data/user/0/com.example.guard/files/config.txt` | App 私有目录 + 0666 | 配置文件，App/Guard 双向读写 |
| `/data/user/0/com.example.guard/files/guard` | App 私有目录 + 0755 | 每次 `startService()` 从 assets 覆盖释放的 Guard 二进制 |
| `/data/user/0/com.example.guard/files/script_path.txt` | App 私有目录 | 脚本管理上次浏览的目录（一行文本） |
| `/sdcard/Guard日志_*.txt` | 公共存储 | 用户点「导出」时保存的日志 |
| `/proc/<pid>/cgroup` | procfs | `cleanupLeftovers()` 扫描进程归属的依据 |

### 3.3 手动调用 Guard（不经过 App）

用于快速验证原生守护的行为是否正常：

```bash
# 推到临时目录
adb push GuardApp/app/src/main/assets/guard_arm64-v8a /data/local/tmp/Guard

# 写一份临时配置
adb shell su -c "cat > /data/local/tmp/Guard/config.txt <<EOF
interval:2
target:com.tencent.tmgp.sgame
freeze:me.weishu.kernelsu
EOF
chmod 666 /data/local/tmp/Guard/config.txt"

# 后台运行（前台切到王者荣耀观察效果）
adb shell su -c "chmod 755 /data/local/tmp/Guard; /data/local/tmp/Guard /data/local/tmp/Guard/config.txt"
```

正常日志（前几行）：
```
[2026-08-30 10:00:00.123] [Guard] 启动，配置：/data/local/tmp/Guard/config.txt
[2026-08-30 10:00:00.140] [配置加载完成] 目标应用 1 个，禁用应用 1 个
[2026-08-30 10:00:00.141] [自检] uid=0 pm=可用 首行=「package:com.android...」
[2026-08-30 10:00:00.142] [Guard] 就绪 PID=12345（logcat events 监听）
```

---

## 四、模块接口协议

### 4.1 App → Guard 启动协议

```
su -c "chmod 755 <exe_path> ; <exe_path> <config_file_path>"
```

- **`argv[1]`**：配置文件绝对路径（不填则 Guard 用内置默认 `/data/local/tmp/Guard/config.txt`）
- **进程名**：Guard 启动时立即 `prctl(PR_SET_NAME, "Guard")`，因此 `pgrep -x Guard` 可精确找到

### 4.2 Guard → App 日志管道协议

Guard 日志输出到 **stderr**，App 用 `ProcessBuilder.redirectErrorStream(true)` 合并到 stdout 读取。

**单条日志格式**：
```
[2026-08-30 HH:mm:ss.SSS] [标签] 消息内容\n
```

App 端会：
1. **剥去 Guard 自带的时间戳前缀**（`[20... ]` 到 `] ` 之间），统一由 App 打新的毫秒级前缀（避免双时间戳）
2. **跳过空行**
3. 追加到 `logBuffer`（超过 30000 字符裁前半段，防内存膨胀）

> 🔍 标签速查：`[系统]` `[配置]` `[脚本]` `[错误]`（App 产生）；`[Guard]` `[进入目标应用]` `[离开目标应用]` `[成功]` `[失败]` `[警告]` `[pm]` `[前台事件]` `[配置已更新]`（Guard 产生）

### 4.3 config.txt 双向协议

```
# 注释
interval:<1..60>         # 保留字段（事件驱动不用）
target:<合法包名>         # 多行，MAX_TARGET=64
freeze:<合法包名>         # 多行，MAX_FREEZE=128
```

**包名校验规则**（Guard 的 `valid_pkg()`）：
- 长度 1~255，不以 `.` 开头或结尾
- 点分段，每段非空
- 段内字符：`a-zA-Z0-9_`

**互斥规则**（两边同时校验）：
- App 端：勾选时目标优先，同包从另一侧移除
- Guard 端：`load_config()` 解析完成后若 freeze 中出现 target 包，从 freeze 剔除并告警

**热重载触发条件**（Guard 每次事件消费前比对）：
```c
changed = !cfg_stat_valid ||
          st.st_mtime != cfg_stat.st_mtime ||
          st.st_size  != cfg_stat.st_size;
```
修改时间**或**文件大小任一变化 → 立即热重载。**原子生效**：解析全程到临时 `Config nc`，全部校验通过 `cfg = nc` 赋值。

---

## 五、常见开发陷阱

| 陷阱 | 现象 | 规避 |
|------|------|------|
| 修改了 guard.c 但 APK 行为没变 | 忘记更新 `assets/guard_arm64-v8a`，APK 里还是旧二进制 | 每次改 guard.c → 重新编译覆盖 assets → 构建 APK |
| aapt2 link 失败「resource not found」 | 删除 drawable 时某处代码还在引用 | 删除前 `grep -r R.drawable.xxx app/src/main` 确认 0 命中 |
| 配置保存偶发 EACCES | Guard 以 root 创建 config.txt，App 普通身份写入被拒 | 三层兜底写入（见 README.md §配置格式），Root 写入后务必 `chmod 666` |
| `pm disable` 成功但状态没变 | 厂商系统（MIUI/ColorOS）hook pm，表面成功实际未生效 | Guard 已经 try `disable` → `disable-user` 双命令，仍失败会输出 `[失败]` |
| 脚本执行后遗留 shell 进程 | 只 `destroy()` Java Process，没清理 su→sh→子进程链 | `cleanupLeftovers()` cgroup 扫描 + SIGTERM/SIGKILL 两段清理 |
| Root 对话框中 `proc.pid()` 抛异常 | 部分旧 ROM 的 Process 类未实现 `pid()` | `pidOf()` 已兜底反射读 `pid` 字段 |
