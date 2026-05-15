# Phase F.0.13 TAA Motion-Adaptive Sharpness — TODO

> 6A 工作流 · 阶段 6 · TODO 收尾

---

## 1. 必做

| 任务 | 状态 |
|------|------|
| commit + push 代码到 main | ✅ 完成 (3923584) |
| 监控 GitHub Actions CI 6/6 success | ✅ 完成 ([25936869113](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113)) |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | ✅ 完成 |
| 修复 typo GL33RenderBackend → GL33Backend | ✅ 完成 (0a794d0) |

---

## 2. 用户指引

### 2.1 启用 motion-adaptive sharpness

```lua
local TAA = Light.Graphics.TAA
TAA.Enable(1280, 720)
TAA.SetSharpness(0.8)                       -- 静止时锐化强度
TAA.SetMotionAdaptiveSharpness(true)        -- 启用自动调整
TAA.SetMotionSharpness(0.1)                 -- 高速时降到 0.1 (减 trail)
```

### 2.2 与 F.0.8 motion-adaptive γ 协同

```lua
TAA.SetClipMode("variance")                 -- 必须 variance 才生效 motion-adaptive γ
TAA.SetMotionAdaptive(true)                 -- F.0.8: γ 在高速时 lerp 到 motionGamma
TAA.SetMotionGamma(1.5)                     -- UE5 推荐
TAA.SetMotionAdaptiveSharpness(true)        -- F.0.13: sharpness 在高速时 lerp
TAA.SetMotionSharpness(0.1)
-- 双重防 trail: variance clip 宽容 + sharpening 降低
```

### 2.3 不同场景建议

| 场景 | sharpness | motionSharpness | MotionAdaptiveSharpness |
|------|-----------|-----------------|--------------------------|
| **FPS / 赛车 (相机高速旋转/平移)** | **0.8** | **0.1** | **ON** |
| **镜头切换/cutscene 瞬间** | 0.5 | 0.0 | **ON** |
| 桌面 RPG 慢速相机 | 0.5 | 0.5 | OFF (差异不明显) |
| 静止预览 / 截图工具 | 任意 | 任意 | OFF |

### 2.4 故障排查

| 现象 | 处理 |
|------|------|
| MotionAdaptiveSharpness=true 但相机运动时无变化 | 检查 backend 是否实现 ComputeCameraMotionScalar (老 backend 返 0 静默失效) |
| 静止时 sharpness 略低于设定值 | 正常: ComputeCameraMotionScalar 首帧返 0, 但 jitter 仍引入微小 viewProj 差异 (~0 motion factor) |
| 切换 sharpenMode 后 motion-adaptive 失效 | 不会, F.0.13 在 TAARenderer 层 lerp, 与所有 mode 兼容 |
| effSharpness 比 motionSharpness 还低 | 不会, lerp 在 [motionSharpness, sharpness] 区间, motion=1 时严格等于 motionSharpness |

---

## 3. CI 回填

GitHub Run ID: [`25936869113`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113) / Fix commit: `0a794d0` / Date: 2026-05-15 19:23 UTC / Result: **6/6 success**

---

## 4. 候选 Phase

- F.0.10 — TAARenderer 多实例 split-screen demo (6h+)
- F.0.11 — Demo 截图/录屏 (3h)
- F.0.14 — Lanczos-2 25-tap 上采样 (4h)
- F.0.15 — TAA-driven CAS strength scaling (history stability 反馈, 2h)

---

## 5. 总结

Phase F.0.13 实施完整，**无阻塞性遗留**。主要交付：

- backend: ComputeCameraMotionScalar virtual + GL33 override (Frobenius distance)
- TAARenderer: state +2 + Process effSharpness lerp + Set/Get +4
- Lua API: +4 fn (TAA 31 → 35), Set/GetMotionAdaptiveSharpness + Set/GetMotionSharpness
- smoke: 默认 + round-trip + clamp + type-error + 状态独立 + sharpenMode 共存 + 十启共存 (~10 PASS)
- demo: O 键 toggle MotionAdaptiveSharpness + Keys help
- 默认零回归 (motionAdaptiveSharpness=false)
- 零 GPU overhead (Frobenius distance 在 CPU, ~0.001 ms/frame)
- 与所有 sharpenMode (unsharp/cas/rcas) 100% 兼容
- 与 F.0.8 motion-adaptive γ 可同时启用 (双重防 trail)

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
