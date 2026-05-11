# ACCEPTANCE — Phase E.3.3 · `Light.Graphics.HDR.*` Lua API + smoke + demo

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.3.3**：10 个 Lua 绑定函数 + `scripts/smoke/hdr.lua` + `samples/demo_hdr/main.lua` + CI 注册。

---

## 1. 改动摘要

| 文件 | 改动量 | 类型 | 关键点 |
|------|--------|------|--------|
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~115 行 | 修改 | 10 个 `l_HDR_*` 静态函数 + `hdr_funcs[]` 注册表 + `luaopen_Light_Graphics` 里挂 `HDR` 子表（line 1683-1686） |
| `@e:\jinyiNew\Light\scripts\smoke\hdr.lua` | +~175 行 | 新建 | API surface (10 函数) + Headless guard + Exposure/Gamma 往返 + Disable/Resize 边界 + 错误参数测试 |
| `@e:\jinyiNew\Light\samples\demo_hdr\main.lua` | +~225 行 | 新建 | 亮度梯度条（灰+RGB）+ 交互式 Exposure/Gamma/Toggle + OSD + Headless 降级 |
| `@e:\jinyiNew\Light\samples\demo_hdr\README.md` | +~65 行 | 新建 | 运行说明 + 操作表 + 预期视觉 + 后端兼容性 |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 行 | 修改 | 注册 `scripts\smoke\hdr.lua` 到 Windows runtime smoke 序列末尾 |

---

## 2. Lua API 设计（`Light.Graphics.HDR.*`）

| 函数 | 签名 | 说明 |
|------|------|------|
| `Enable` | `(w:int, h:int) -> bool` | 创建 HDR RT（RGBA16F + Depth24）。必须在 Window.Open 后调用 |
| `Disable` | `() -> void` | 释放 HDR RT，退回 LDR 管线 |
| `IsEnabled` | `() -> bool` | 当前 HDR 状态 |
| `IsSupported` | `() -> bool` | 后端是否支持（GL33=true，Legacy=false） |
| `Resize` | `(w:int, h:int) -> bool` | 变更 HDR RT 尺寸（窗口 resize 回调手动触发） |
| `SetExposure` | `(v:number) -> void` | 线性曝光倍率（默认 1.0） |
| `GetExposure` | `() -> number` | |
| `SetGamma` | `(v:number) -> void` | sRGB encode gamma（默认 2.2） |
| `GetGamma` | `() -> number` | |
| `GetSceneTexture` | `() -> int` | HDR RT 的 GL texture id，高级用户可自定义 shader 采样 |

### 子表挂载方式

`luaopen_Light_Graphics` 里在 Graphics 表创建完成后，创建 `HDR` 子表并用 `luaL_setfuncs` 注册 10 个函数：

```cpp
// light_graphics.cpp:1683-1686
lua_createtable(L, 0, 0);
luaL_setfuncs(L, hdr_funcs, 0);
lua_setfield(L, -2, "HDR");
```

这种挂载模式避开了 `luaL_register(L, "Light.Graphics.HDR", ...)` 对 `LUA_GLOBALSINDEX` 的多层查表触发 Light 全局 OOP 守护（参见 `Phase AV` 经验教训）。

---

## 3. smoke 覆盖（`scripts/smoke/hdr.lua`）

### 设计原则

- **Headless 友好**：smoke 运行时无 GL context，`HDR.Enable(256, 256)` 允许：
  - (a) 返回 `false`（典型场景，后端检测到无上下文）
  - (b) 返回 `true`（极少数 host 已有 ctx）
  无论哪种，API 表面 + 后续 Disable/Resize 必须不崩
- **纯 API 契约验证**：不测试渲染结果（那是 demo 的工作）

### 6 个测试段

1. **子表存在 + 10 函数注册**
2. **初始状态**：`IsSupported` 返回 bool；`IsEnabled` = false
3. **Exposure/Gamma 默认值 + 往返**：默认 1.0/2.2；SetN→GetN 值一致
4. **GetSceneTexture 未启用 = 0**
5. **Enable/Disable robustness**：Enable 不崩（允许返回 false）；Disable 幂等；Resize on disabled 返回 false 不崩
6. **错误参数**：`Enable(0, 0)` / `Enable(-1, 256)` 返回 false；失败后 GetSceneTexture 仍为 0

### 预期输出

```
PASS: Light.Graphics.HDR subtable present
PASS: Light.Graphics.HDR module surface ok (10 functions)
PASS: IsSupported() returns boolean (value=true)
PASS: IsEnabled() = false initially
PASS: GetExposure() default = 1.0
PASS: SetExposure / GetExposure round-trip ok
PASS: GetGamma() default = 2.2
PASS: SetGamma / GetGamma round-trip ok
PASS: GetSceneTexture() = 0 when disabled
PASS: HDR.Enable(256, 256) returned false cleanly (no throw)
PASS: IsEnabled() tracks Enable() result
PASS: HDR.Disable() always safe; IsEnabled() = false after
PASS: Double Disable() idempotent
PASS: HDR.Resize on disabled returned false cleanly
PASS: Enable bad params rejected (w<=0 or h<=0)
PASS: GetSceneTexture() stays 0 after failed Enable
[Phase E.3] Light.Graphics.HDR smoke PASS (10 functions)
```

---

## 4. demo 设计（`samples/demo_hdr/main.lua`）

### 视觉验收点

| 场景 | LDR（HDR OFF） | HDR ON |
|------|----------------|--------|
| 10 个灰度条 `b ∈ [0.2, 3.8]` | `b > 1.0` 的 6 个条全部为白（clip） | 10 个条呈连续渐变（ACES 压缩） |
| 3 组 RGB 梯度条 | `b > 1.0` 后色相丢失 → 白 | 色相保留，随亮度渐进压缩 |
| Exposure 下调 | 无视觉效果（已 clip） | 高亮区域"恢复"细节 |
| Gamma 提高 | 轻微变化 | 画面变暗（符合预期） |

### 交互

- `H` 切换 HDR（运行时动态切换，可对比同一画面的 LDR vs HDR）
- `Z/X` Exposure -/+ 0.1（0.1 ~ 5.0）
- `C/V` Gamma -/+ 0.1（1.0 ~ 3.0）
- `R` 重置
- `ESC` 退出

### Headless 降级

无 `UI.Window` 时仅打印 API 探测结果后 `return`（与其他 demo 一致）。

---

## 5. 验收清单

| 标准 | 状态 | 证据 |
|------|------|------|
| 10 个 Lua 绑定函数完整 | ✅ | `light_graphics.cpp:1504-1585` |
| `hdr_funcs[]` 注册表 10 条 | ✅ | `light_graphics.cpp:1587-1599` |
| HDR 子表挂载到 `Light.Graphics` | ✅ | `light_graphics.cpp:1683-1686` |
| `scripts/smoke/hdr.lua` 纯 API 测试 | ✅ | 6 段，16 个 PASS 点 |
| `samples/demo_hdr/main.lua` 视觉验收 | ✅ | 亮度梯度 + Exposure/Gamma 交互 |
| `samples/demo_hdr/README.md` 说明文档 | ✅ | 运行方式、操作表、预期视觉 |
| CI `build-templates.yml` 注册 hdr.lua smoke | ✅ | line 97 + line 197 |
| Light.dll 编译通过（6 平台） | ⏳ | 等 CI |
| hdr.lua smoke runtime 通过 | ⏳ | 等 CI |
| 既有 45+ smoke 零回归 | ⏳ | 等 CI |

---

## 6. 已知限制 / 留给未来

| 限制 | 原因 | 解决路径 |
|------|------|----------|
| 无 Bloom 通道 | E.3 scope 之外 | Phase E.4 候选 |
| 不支持自定义 tonemap 曲线（仅 ACES） | E.3 scope 之外 | 后续 `HDR.SetTonemapper("reinhard"/"aces"/"uncharted2")` |
| 窗口 resize 需手动调 `HDR.Resize` | 引擎未暴露 window resize callback | 与 `Light.UI.Window` 事件系统对接 |
| smoke 仅验证 API 表面，不验证渲染结果 | headless 无 GL ctx | demo_hdr 承担视觉验收（人工） |
| demo 未捕截屏用于自动对比 | 需要离屏 readback + pixel diff 基础设施 | 后续基础设施任务 |

---

## 7. 下一步

**Phase E.3 全部完成后**：
- `FINAL_PhaseE_3.md` — 汇总 E.3.1 + E.3.2 + E.3.3 的交付物、CI 证据、视觉验收记录。
