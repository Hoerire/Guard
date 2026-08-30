# 弹窗风格统一 + 代码精简 + 文档重写 实现计划

## Repository Research

### 一、弹窗现状（5 处 AlertDialog 风格不一致）

| 弹窗 | 位置（行号） | 问题 |
|------|-------------|------|
| `promptNoRoot()` | 710–715 | 纯原生 Builder，无自定义内容，OK |
| `showPrinciple()` | 954–957 | 自定义 ScrollView + LinearLayout，内容 padding `22,14,22,18`，无标题栏 |
| `runScriptDialog()` | 1140–1161 | 自定义 LinearLayout，padding 仅 `6,2,6,2`（偏小，紧贴边框），标题系统默认蓝 |
| `runScript()` 交互式窗口 | 1223–1230 | 自定义输出 + 输入区，外层 padding `4,2,4,2`（最挤），PositiveButton「完成」无作用语义，无负按钮 |
| `showScriptResult()` | 1346–1359 | **死代码**（定义未调用），ScrollView 内 padding `18,12,18,12` |

核心不统一表现：
1. 自定义容器 padding 从 `4/2` 到 `22/14` 无规律，内部文字贴边程度各异
2. 主按钮颜色走系统默认 Material Light 蓝，与 App 的 `PRIMARY=0xFF5B8DEF` 不统一
3. 部分弹窗无标题（工作原理）、部分有但风格乱；交互式运行窗口「完成」按钮无实际功能只是关闭
4. 脚本运行确认框的 SU 复选框绿 `0xFF2FA66F` 与按钮主题不一致

### 二、死代码/冗余资产

| 项目 | 状态 | 处理 |
|------|------|------|
| `showScriptResult(String name,int code,String out)` (L1346–1359) | 全工程只有定义，无调用点 | 直接删除 |
| `res/drawable/ic_stop.xml` | 全工程 0 引用，MainActivity 中无 `R.drawable.ic_stop` 使用 | 删除 |
| `res/drawable/ic_play.xml` | 全工程 0 引用 | 删除 |
| `FsEntry.size` 字段 | `rootList()` 不赋值 size，仅 Java File API 路径赋值；但实际展示仅用到 name/isDir | 保留（无害，Java 路径偶尔显示文件大小） |

### 三、文档现状

- `/workspace/README.md`：用户使用文档，内容详细但排版偏密、段落冗长，「6. 注意事项」与「5. 常见问题」结构可优化，缺少「快速上手指南」和「界面功能地图」
- `/workspace/GuardApp/README.md`：工程说明仅三段话，缺少模块职责、构建前置条件、调试技巧

---

## Files and Modules

- **`/workspace/GuardApp/app/src/main/java/com/example/guard/MainActivity.java`**
  - 新增 2 个小工具：`buildDialogContent(int outerPad)` 统一外层容器；`buildAlertDialog()` 统一 Builder 风格
  - 修改 4 处 AlertDialog 调用点（去除死代码后剩 4 处），使用新封装
  - 删除死代码 `showScriptResult()`
  - 清理 `runScript()` 中「完成」按钮 → 改为语义正确的「关闭运行窗口」并允许通过取消键关闭
- **`/workspace/GuardApp/app/src/main/res/drawable/`**
  - 删除 `ic_stop.xml`、`ic_play.xml`
- **`/workspace/README.md`**：全量重写
- **`/workspace/GuardApp/README.md`**：全量重写

---

## Implementation Steps

### Step 1：弹窗风格统一封装（MainActivity.java）
1. 新增对话框内容容器统一工厂方法：
   ```java
   LinearLayout dialogBox(int padDp) { ... } // padding=padDp，VERTICAL
   ```
2. 新增 AlertDialog.Builder 统一入口（与主题 PRIMARY 色协调）：
   - 不强制自定义标题栏（最小改动原则），只统一内容 padding
3. 重构 `runScriptDialog()`：外层 padding 从 `6,2,6,2` → `18,6,18,6`（与工作原理同量级）；Hint 文字行距统一，SU 复选框颜色改用 PRIMARY 色 `5B8DEF` 而非绿色
4. 重构 `runScript()` 交互式窗口：
   - 外层 padding 从 `4,2,4,2` → `14,4,14,4`
   - 标题后加「（可随时关闭窗口取消运行）」提示
   - 「完成」按钮 → 改为「关闭窗口」，同时移除 `setCancelable(false)`，允许用户按返回键（已存在 `onDismissListener` 会 destroy 进程，行为安全）
5. `showPrinciple()` 弹窗：补一个标题「工作原理」(之前无标题)，视觉上与其他弹窗一致
6. `promptNoRoot()`：无需改动（纯系统原生 message 对话框，无自定义内容，已足够清晰）

### Step 2：清理死代码与冗余资源
1. 删除 `MainActivity.java` 中 `showScriptResult()` 方法（~14 行）
2. 删除 `res/drawable/ic_stop.xml` 和 `res/drawable/ic_play.xml`（两个 0 引用的 XML 资源）

### Step 3：重写 README 文档
1. **根 `/workspace/README.md`** 重写为以下结构（面向最终用户）：
   - 顶部产品名 + 一句话标语 + 产物
   - **快速开始**（3 步搞定：安装→授权→勾选）
   - **核心功能**：6 模块分节（服务控制/目标&禁用/日志/脚本/终端/界面），每节一屏可读，要点加粗
   - **配置格式**：保留但精简示例+字段说明
   - **工作原理**：2 段式架构图（ASCII 或表格）+ 状态机一句话说明
   - **常见问题 FAQ**：用 Q/A 格式替换表格
   - **安全与注意**：独立一节
2. **`/workspace/GuardApp/README.md`** 重写为工程 README（面向开发者）：
   - 项目简介 + 架构一句话
   - **工程结构**：按目录 + 源文件分条列出，指明职责
   - **构建指南**：前置（SDK+NDK+路径）、脚本说明、手动构建步骤
   - **开发调试**：如何手动测试（adb install + logcat -s Guard）、关键文件路径
   - **模块接口**：App↔Guard 启动参数、日志管道协议、config.txt 格式

### Step 4：构建验证
- 执行 `bash build_arm64.sh`，确保：
  - aapt2 compile/link 通过（删除 drawable 无残留引用）
  - javac 编译通过（删除 showScriptResult 无调用点）
  - 最终 APK 签名成功
- 若出现 aapt2 `resource not found` 报错，立即回退删除 drawable 的动作（极小风险，已 grep 确认无引用）

---

## Dependencies and Considerations

1. **最小改动原则**：不引入新的 AndroidX / Material 依赖，不新增 style.xml 主题（避免 aapt2 link 风险，参考 Experience 300805 的教训）
2. **AlertDialog 兼容性**：不自定义 CustomTitle（需要额外 drawable），通过统一内容 padding + 统一颜色方案即可达到 90% 风格一致，同时保持零新增资源
3. **setCancelable(false) 去除风险**：`runScript()` 原本禁用返回键是为了防止误关导致进程残留；但现有 `setOnDismissListener` 已经 `proc[0].destroy()` + finally 会 `cleanupLeftovers()`，安全无虞，可开放返回键
4. **drawable 删除风险**：已 `grep -r ic_stop\|ic_play /workspace/GuardApp/app/src/main/` 确认 0 匹配，删除安全
5. **README 风格**：保留中文，不改变已有核心事实描述，仅做结构重组 + 精简

---

## Validation

1. **编译验证**：`bash /workspace/build_arm64.sh` 执行完毕，末尾 `apksigner verify` 成功 + APK 文件生成
2. **功能回归点**（静态检查）：
   - 5 处 AlertDialog → 删除 1 处死代码后剩 4 处：每处均保留原有功能逻辑（仅改 UI 包装）
   - `runScript()` onDismissListener 保留（`userClosed[0]=true` + proc destroy），与修改前行为等价
   - `showPrinciple()` + 标题不影响内容展示
3. **代码精简验证**：grep `showScriptResult` / `ic_stop` / `ic_play` 返回 0 匹配
4. **文档检查**：README.md 有标题/目录结构，无内容事实错误（与 guard.c/MainActivity 对照）

---

## Risks

| 风险 | 概率 | 影响 | 处理方案 |
|------|------|------|----------|
| aapt2 link 失败（删除 drawable 但某处引用） | 极低（grep 0 命中） | 编译阻断 | 立即回退删除，恢复 drawable 文件 |
| javac 失败（删除 `showScriptResult` 有隐藏调用） | 极低（grep 0 命中） | 编译阻断 | 恢复方法定义 |
| `runScript()` 开放返回键后用户误操作体验下降 | 低 | 体验 | 标题后附说明「可随时关闭窗口取消运行」，同时保留关闭按钮 |
| `showPrinciple()` 加标题后内容滚动增加一屏 | 低 | 视觉 | 内容 padding 稍减补偿（从 22/14 → 18/10） |
