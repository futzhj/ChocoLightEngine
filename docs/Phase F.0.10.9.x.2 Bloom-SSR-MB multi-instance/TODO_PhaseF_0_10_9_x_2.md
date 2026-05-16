# Phase F.0.10.9.x.2 — Bloom/SSR/MB Multi-Instance TODO

> 6A · 后续接力. 本 phase 已 ✅ 完成.

---

## ✅ 已完成

- 3 renderer × 5 fn = 15 Lua API (80 → 95)
- 3 smoke 各自加 multi-instance § (MI.1~8 / MI.1~6)
- 8 smoke 零回归 + demo_quad_split 真 GL 启动验证
- PLAN + FINAL + TODO

## 🟡 待办 — CI 验证

- [ ] commit + push → CI 6 平台
- [ ] 6/6 全绿确认

## 🔵 后续接力

### 1. demo_quad_split 改造为 multi-instance 模式 (低优, ~30min)

当前 demo_quad_split 仍用 "每帧切 Bloom/SSR/MB 全局参数" hack. 改造后:

```lua
-- OnOpen 一次性 setup 4 instance (instance 0 default + 1/2/3 user)
for i = 1, 3 do
    g_bloom_ids[i] = Bloom.CreateInstance(); Bloom.SetActiveInstance(g_bloom_ids[i])
    Bloom.SetIntensity(profiles[i].bloom); Bloom.SetThreshold(...); Bloom.SetRadius(...)
    -- 类似 SSR / MB
end
Bloom.SetActiveInstance(0); SSR.SetActiveInstance(0); MB.SetActiveInstance(0)
-- 各自固定 profile

-- Draw: 不再 apply_postfx_profile, 仅切 active
function Demo:Draw()
    for i = 0, 3 do
        HDR.SetActiveInstance(hdr_ids[i])
        Bloom.SetActiveInstance(bloom_ids[i])   -- 自动取对应 profile
        SSR.SetActiveInstance(ssr_ids[i])
        MB.SetActiveInstance(mb_ids[i])
        TAA.SetActiveInstance(taa_ids[i])
        Bloom.Process(rgn); SSR.Process(rgn); MB.Process(rgn); TAA.Process(rgn)
        ...
    end
end
```

**收益**: demo 代码 -30 行 (apply_postfx_profile 消除), SDK 价值演示更清晰.
**代价**: 4× Bloom/SSR/MB RT VRAM (~64MB at 1080p, ~16MB at 640×360 可接受).

不在本 phase 做, 因为需要拍板 VRAM 代价是否值得 (现在 hack 也工作).

### 2. F.0.10.9.x.3 GetState/Clone (中优, ~1.5h)

为所有 5 个 multi-instance renderer (HDR/TAA/Bloom/SSR/MB) 加:
- `GetState() → table` 返回当前 instance 全部 state 快照
- `CloneInstance(srcId) → newId` 一键复制 profile 到新 instance

让 multi-instance setup 由 ~30 行 → 1 行 (clone + 微调).

### 3. F.0.11 demo 截图/录屏 (高优, ~3h)

实现 `Light.Graphics.Screenshot(path)` + `Light.Graphics.RecordVideo(path,fps,dur)`.
demo_quad_split 启动后自动截 30 帧 stabilize 后画面 → PNG, 直接附 README/wiki.

### 4. 其他 renderer multi-instance (低优, ~大工程)

`SSAORenderer`, `AutoExposureRenderer`, `LensFlareRenderer`, `LensDirtRenderer`,
`StreakRenderer` 也是全局单例. 但实际 demo 场景中 SSAO/AE 全局共享一份就够 (split-screen
不需要 per-quad SSAO/AE), 边际价值低. 留待用户提需求再做.

## 📚 文档导航

- `PLAN_PhaseF_0_10_9_x_2.md` — 6A 设计 (3 renderer + 模板 + Sub-task 拆分)
- `FINAL_PhaseF_0_10_9_x_2.md` — 实现总结 + lessons + F.0.10.x 里程碑
- `TODO_PhaseF_0_10_9_x_2.md` — 本文
- `../Phase F.0.10.10 quad-split linkage demo/` — 上一 phase (4-screen demo)
- `../Phase F.0.10.9 multi-instance HDR/` — multi-HDR 母 phase (模板出处)
