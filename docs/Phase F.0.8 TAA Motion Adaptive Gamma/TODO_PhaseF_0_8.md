# Phase F.0.8 TAA Motion-Adaptive γ — TODO

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档

---

## 1. 必做

| 任务 | 状态 |
|------|------|
| commit + push 代码到 main | 🔴 待办 |
| 监控 GitHub Actions CI 6/6 平台 success | 🔴 待办 |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 🔴 待办 |

---

## 2. 推荐 Phase F.0.x 候选

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| F.0.9 — Custom upsampler | bicubic / Lanczos 替代 F.0.5 bilinear | 4h | 🟡 中 |
| F.0.10 — TAARenderer 多实例化 + 真 split-screen | 真 left/right 双 TAA 实例 | 6h+ | 🟢 低 |
| F.0.11 — Demo 截图 / 录屏 | frame-by-frame 截图 + GIF/MP4 | 3h | 🟢 低 |
| F.0.12 — RCAS (FSR2 增强) | CAS 增强版处理 deringing | 4h | 🟢 低 |
| F.0.13 — Motion-adaptive sharpness | 高速时降 sharpness 减 trail | 2h | 🟢 低 |

---

## 4. 用户指引

### 4.1 启用 motion-adaptive

```lua
local TAA = Light.Graphics.TAA
TAA.Enable(1280, 720)
TAA.SetClipMode("variance")     -- 必须 variance 才生效
TAA.SetVarianceGamma(1.0)       -- 静止 γ
TAA.SetMotionGamma(1.5)         -- 高速 γ (UE5 默认)
TAA.SetMotionAdaptive(true)
```

### 4.2 不同场景建议

| 场景 | mot γ | MotionAdaptive |
|------|-------|----------------|
| 静态 (回合制 / 解谜) | - | OFF |
| **中等运动 (RPG / TPS)** | `1.5` | **ON** (UE5 默认) |
| 快节奏 (FPS / 赛车) | `2.0` | **ON** |
| HDR + 高速 | `1.5` | **ON + AntiFlicker** |

### 4.3 故障排查

| 现象 | 处理 |
|------|------|
| 切 motion-adaptive 后无变化 | 检查 ClipMode 是否为 "variance" (其他模式不受影响) |
| 高速运动 trail 仍多 | 提高 motionGamma 到 2.0~3.0 |
| 静止画面 ghost | 降低 varianceGamma 到 0.75 |

---

## 5. CI 回填（待 T5 完成后填）

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 6. 总结

Phase F.0.8 实施完整，**无阻塞性遗留**。主要交付：

- shader: dynGamma lerp 替代 uVarianceGamma (variance clip 路径)
- TAARenderer: motionGamma + motionAdaptive state + Set/Get
- Lua API: +4 fn (TAA 25 → 29)
- smoke: 10 个 Phase F.0.8 PASS + 七启共存
- demo: Q 键切换 + HUD sγ/mγ 双显
- 默认零回归 (motionAdaptive=false, motionGamma=1.5)

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
