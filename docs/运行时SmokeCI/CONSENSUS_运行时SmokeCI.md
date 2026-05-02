# 运行时 Smoke CI 共识

## 需求描述

将 smoke 脚本纳入 CI，覆盖 Lua 语法检查与 Windows 运行时模块加载检查，补上“只编译不运行”的验证缺口。

## 验收标准

- `scripts/smoke/core_runtime.lua` 可由 Windows `light.exe` 运行通过。
- `scripts/smoke/physics_p0_p1.lua` 可由 Windows `light.exe` 运行通过。
- Windows/Linux/macOS CI 都会执行 `lightc -p scripts/smoke/*.lua`。
- GitHub Actions 在 `origin` 仓库完成全平台构建并成功。

## 技术约束

- 不引入新的测试框架。
- 不改变 smoke 脚本的业务 API。
- 不推送到 `iosndesign` 远程。
- 最终验证以 GitHub Actions 为准。
