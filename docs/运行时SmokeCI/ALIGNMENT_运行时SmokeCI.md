# 运行时 Smoke CI 对齐

## 原始需求

继续推进 Physics P0/P1 之后的推荐工作：补齐运行时 smoke 与 CI 验证，最终通过 GitHub Actions 验证。

## 项目上下文

- 当前仓库使用 GitHub Actions `build-templates.yml` 构建 Windows/Linux/macOS/Web/Android/iOS 模板。
- Lumen 提供 `lightc` 用于 Lua 语法/字节码检查。
- Windows `light.exe` 会从可执行文件目录加载 `Light.dll` 并预加载模块。
- 非 Windows 的 `light` 当前没有等价的 `libLight` 自动预加载流程。

## 边界

- 本阶段只做低风险 smoke/CI 加固。
- 桌面三平台执行 `scripts/smoke/*.lua` 的 `lightc -p` 语法检查。
- Windows 额外执行 `core_runtime.lua` 与 `physics_p0_p1.lua` 运行时检查。
- 不在本阶段实现 Linux/macOS 动态加载 `libLight`。

## 关键结论

Windows 运行时 smoke 需要补齐 `light.cpp` 的模块预加载表，否则新模块不会出现在 `Light` 全局表中。
