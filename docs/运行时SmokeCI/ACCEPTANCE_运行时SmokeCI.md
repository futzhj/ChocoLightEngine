# 运行时 Smoke CI 验收记录

## 验证结果

- [x] 本地 `lightc -p scripts/smoke/core_runtime.lua`
- [x] 本地 `lightc -p scripts/smoke/physics_p0_p1.lua`
- [x] GitHub Actions Windows runtime smoke
- [x] GitHub Actions 全平台构建

## Actions 记录

- 分支: `feature/runtime-smoke-ci`
- 成功 Run: `25242428905`
- 结果: Windows/Linux/macOS/Android/iOS/Web 全部成功

## 发现并修复的问题

- Windows `light.exe` 的 `Light.dll` 预加载表缺少 Phase2/Phase3 新模块。
- `luaopen_Light_Input` 在 `LT::RegisterModule` 返回模块表后又错误读取 `Input` 子字段，导致运行时预加载崩溃。
