# 自由启停（Guard）工程

Android 前台应用触发式禁用工具（ARM64 原生版）。进入指定「目标应用」时自动批量禁用「禁用应用」，离开后自动恢复。App（Java）负责配置与部署，原生 Guard（C 可执行程序）专职前台监听与禁用/恢复。

## 工程结构

- `app/src/main/java/com/example/guard/MainActivity.java`：App 全部逻辑（UI、root、服务拉起、脚本/终端、日志）
- `app/src/main/cpp/guard.c`：原生 Guard 程序（独立 `main()`，非 JNI）
- `app/src/main/cpp/CMakeLists.txt`：CMake 可执行目标配置
- `app/src/main/assets/guard_arm64-v8a`：已编译的 Guard 二进制，打包时随 APK 发布
- `app/src/main/AndroidManifest.xml`：minSdk 26 / targetSdk 35，启用预测性返回

## 构建

- 原生部分：NDK clang 交叉编译 `guard.c` 为 aarch64 PIE 可执行文件，放入 `assets/guard_arm64-v8a`
- APK 部分：完整构建脚本见仓库根 `build_arm64.sh`（aapt2 + javac + d8 + zipalign + apksigner）

## 功能

目标/禁用双列表（互斥、勾选即存）、配置热重载、实时毫秒日志与导出到 `/sdcard`、脚本管理（路径浏览、SU/普通运行、交互输入、ELF 识别、进程清理）、轻量 root 终端、毛玻璃 UI。

完整使用文档见仓库根 `README.md`。
