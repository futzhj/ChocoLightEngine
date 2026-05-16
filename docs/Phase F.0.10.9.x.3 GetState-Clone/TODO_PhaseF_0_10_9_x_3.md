# Phase F.0.10.9.x.3 — GetState/Clone TODO

> 6A · 后续接力. 本 phase 已 ✅ 完成.

---

## ✅ 已完成

- 5 renderer × 2 fn = 10 Lua API (95 → 105)
- 5 smoke 加 Clone/GetState 测试段 (CS.1~5 / 9.17.1~4 / 27.1~4)
- 8 smoke 零回归 + demo_quad_split 真 GL 启动验证
- PLAN + FINAL + TODO

## 🟡 待办 — CI 验证

- [ ] commit + push → CI 6 平台
- [ ] 6/6 全绿确认

## 🔵 后续接力

### 1. demo_quad_split 改 Clone setup (低优, ~30min)

利用本 phase 新加的 `CloneInstance(0)` 简化 split-screen 4 player setup:

```lua
-- 旧 (F.0.10.9.x.2 风格): 每 instance 手动 Set 全部字段
for i = 1, 3 do
    local id = Bloom.CreateInstance()
    Bloom.SetActiveInstance(id)
    Bloom.SetThreshold(profiles[i].threshold)
    Bloom.SetIntensity(profiles[i].intensity)
    Bloom.SetRadius(profiles[i].radius)
    Bloom.SetLevels(profiles[i].levels)
end

-- 新 (F.0.10.9.x.3 风格): 1 行 clone + 微调
for i = 1, 3 do
    local id = Bloom.CloneInstance(0)         -- ✨ 复制 default 全部字段
    Bloom.SetActiveInstance(id)
    Bloom.SetIntensity(profiles[i].intensity) -- 仅微调差异字段
end
```

收益: demo setup 减少 ~50 行, 代码更清晰. 不在本 phase 做以保持 demo 提交独立性.

### 2. F.0.10.9.x.4 SetState (反向 API, 中优, ~1h)

为 save/load profile 加反向 API:

```cpp
// 5 renderer × SetState(table) → bool
bool BloomRenderer::SetState(/* lua_State* L */) {
    // 解析 lua table, 调 Set* 还原所有字段
}
```

用户体验:
```lua
-- save
local snap = HDR.GetState()
File.WriteJson("profile.json", snap)

-- load
local snap = File.ReadJson("profile.json")
HDR.SetState(snap)   -- ✨ 一键还原 (vs 手动 SetExposure/SetGamma/SetTonemapper/...)
```

不在本 phase 做因为:
1. 用户可手动 `Set*` 还原, SetState 仅是糖语法
2. 字段名/类型校验复杂 (table 错误时静默 fallback vs 抛 error?)
3. 需要再加 5 wrap fn + 5 smoke section

### 3. demo 截图/录屏 (F.0.11, 高优, ~3h)

实现 `Light.Graphics.Screenshot(path)` + `Light.Graphics.RecordVideo(path,fps,dur)`.
demo_quad_split 启动后自动截 30 帧 stabilize 后画面 → PNG 直接附 README/wiki.

可与本 phase 的 GetState 结合做对比图: 同帧不同 instance 各自 screenshot, 横排展示 4 player profile 效果.

### 4. 其他 renderer multi-instance (低优)

`SSAORenderer`, `AutoExposureRenderer`, `LensFlareRenderer`, `LensDirtRenderer`,
`StreakRenderer` 也是全局单例. 但实际 demo 场景中 SSAO/AE 全局共享一份就够 (split-screen
不需要 per-quad SSAO/AE), 边际价值低. 留待用户提需求再做.

## 📚 文档导航

- `PLAN_PhaseF_0_10_9_x_3.md` — 6A 设计 (5 renderer × 2 fn 模板)
- `FINAL_PhaseF_0_10_9_x_3.md` — 实现总结 + lessons (字段命名陷阱) + 累计里程碑
- `TODO_PhaseF_0_10_9_x_3.md` — 本文
- `../Phase F.0.10.9.x.2 Bloom-SSR-MB multi-instance/` — 上一 phase (基础多实例)
- `../Phase F.0.10.9 multi-instance HDR/` — 多实例母 phase (模板出处)
