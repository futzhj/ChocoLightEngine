# Phase3 Runtime Smoke 增强验证设计

## 背景

ChocoLight Phase3 已在 `ENGINE_EVALUATION.md` 中标记完成，覆盖用户 Shader API、精灵动画、UI 控件库、资源热重载、场景管理器与加密模块。当前 `scripts/smoke/core_runtime.lua` 已验证模块可 `require` 并挂载到 `Light` 表，但缺少 Phase3 行为级 smoke。

本设计以 CI 绿色为第一优先级，同时增加窗口、Shader、UI 绘制路径的增强验证。

## 目标

- 新增 Phase3 行为级 smoke，覆盖无窗口环境可稳定验证的核心行为。
- 新增窗口/GL 增强 smoke，验证窗口、Shader 与 UI 绘制路径在可用环境中不崩溃。
- 将新增 smoke 接入 Windows runtime CI，并保持全平台 `lightc -p` 语法检查。
- 窗口/GL 不可用时，增强 smoke 打印 skip 并正常退出，避免 CI 假失败。
- 最终验证以 GitHub Actions 为准，代码只推送到 `origin`。

## 非目标

- 不新增 Phase3 功能。
- 不重构 Phase3 模块实现。
- 不要求 Linux/macOS CI 运行 runtime smoke。
- 不把窗口/Shader 创建失败作为 CI 强制失败条件。
- 不验证真实用户交互、像素输出或 GPU 渲染结果。

## 方案

### 1. 强制行为 smoke

新增 `scripts/smoke/phase3_runtime.lua`，作为 Windows runtime CI 的强制脚本。

覆盖范围：

- `Light.Crypto`
  - `SHA256("hello")`、`MD5("hello")` 与已知摘要对比。
  - `Base64Encode` / `Base64Decode` 往返。
  - `AES256_Encrypt` / `AES256_Decrypt` 固定 key/iv 往返。
  - `RandomBytes` / `RandomHex` 长度校验。
  - `KeyFromPassword` 长度与确定性校验。
- `Light.Scene`
  - `Push` / `Pop` / `Replace` / `Clear` / `Top` / `Count`。
  - `OnEnter` / `OnExit` / `OnPause` / `OnResume` 调用顺序。
  - `Update` / `Dispatch` 转发到栈顶场景。
- `Light.Graphics.SpriteAnimation`
  - `New`、`Play`、`Pause`、`Resume`、`Stop`。
  - 帧推进、循环回调、非循环完成回调。
  - `SetFrame` 钳位、`GetFrame`、`GetFrameIndex`、`GetFrameCount`。
- `Light.UI.Widget`
  - `Container` / `Label` / `Button` / `CheckBox` 构造。
  - 子节点增删、绝对坐标、命中测试。
  - 鼠标事件分发、Button 点击、CheckBox 状态切换。
- `Light.HotReload`
  - 监视一个临时文件。
  - `Watch` / `List` / `Unwatch` / `Clear`。
  - 文件时间戳检测只做基础可用性验证，不依赖亚秒级 mtime。
- `Light.Graphics.Shader`
  - `IsSupported`、`New`、`UseDefault` 函数存在。
  - 不在无窗口脚本中创建 shader。

失败策略：任何断言失败，脚本退出失败。

### 2. 窗口/GL 增强 smoke

新增 `scripts/smoke/phase3_window_runtime.lua`，作为 Windows runtime CI 的增强脚本。

流程：

1. `require("Light.UI.Window")`、`require("Light.Graphics")`、`require("Light.Graphics.Shader")`、`require("Light.UI.Widget")`。
2. 创建 `Light(Light.UI.Window):New()` 实例。
3. 调用 `Window:Open(160, 120, "Phase3 Smoke")`。
4. 如果返回失败或抛错，打印 `phase3_window_runtime smoke skipped` 并退出成功。
5. 如果窗口成功打开：
   - 调用基础 `Light.Graphics` 绘制函数。
   - 创建 `Light.UI.Widget` 控件树并执行 `Draw()`。
   - 当 `Shader.IsSupported()` 为 true 时，尝试创建最小 shader，调用 `Use`、`SetFloat`、`SetVec2`、`SetVec3`、`SetVec4`、`SetInt`、`SetMat4`、`UseDefault`、`Delete`。
   - 调用窗口步进一次，然后关闭窗口。

失败策略：

- 窗口/GL 创建失败：skip，退出成功。
- 窗口已创建后，绘制、Shader 或关闭流程异常：脚本失败。

### 3. CI 集成

更新 `.github/workflows/build-templates.yml`：

- 保持 Windows/Linux/macOS 对 `scripts/smoke/*.lua` 的 `lightc -p` 语法检查。
- Windows runtime smoke 在 `core_runtime.lua` 与 `physics_p0_p1.lua` 后追加：
  - `phase3_runtime.lua`
  - `phase3_window_runtime.lua`

最终验证流程：

1. 本地至少运行 `git status` 与相关脚本语法检查。
2. 提交并推送分支到 `origin`。
3. 以 GitHub Actions 全平台结果作为最终验收依据。

## 风险与处理

- **CI 无显示/无 GL**：窗口增强 smoke 可 skip，不影响 CI 绿色。
- **文件 mtime 精度差异**：HotReload smoke 不依赖立即修改后触发回调，只验证 watch/list/unwatch/clear 的稳定行为。
- **Shader 后端差异**：只有 `Shader.IsSupported()` 为 true 时才创建 shader。
- **Phase3 纯 Lua 状态污染**：每个测试段在结束时调用对应清理逻辑，例如 `Scene.Clear()` 与 `HotReload.Clear()`。

## 验收标准

- 新增 smoke 脚本通过 `lightc -p` 语法检查。
- Windows runtime smoke 能运行 `phase3_runtime.lua`。
- `phase3_window_runtime.lua` 在无窗口/无 GL 环境下可 skip，在可用环境下完成窗口、UI 与 Shader 基础路径验证。
- GitHub Actions 在推送到 `origin` 后通过。
