# Phase F.0.9 TAA Custom Upsampler — TODO

> 6A 工作流 · 阶段 6 · TODO 收尾

---

## 1. 必做

| 任务 | 状态 |
|------|------|
| commit + push 代码到 main | 🔴 待办 |
| 监控 GitHub Actions CI 6/6 success | 🔴 待办 |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 🔴 待办 |

---

## 2. 用户指引

### 2.1 启用 bicubic 上采样

```lua
local TAA = Light.Graphics.TAA
TAA.Enable(1280, 720)
TAA.SetSharpness(0)              -- 零 ALU 路径 (必需)
TAA.SetHalfResHistory(true)      -- VRAM -75% (必需)
TAA.SetUpscaleMode("bicubic")    -- -50% blur vs bilinear
```

### 2.2 不同场景建议

| 场景 | sharp | halfRes | upscale | 说明 |
|------|-------|---------|---------|------|
| 桌面 1080p 纯质量 | 0.5+ | OFF | bilinear (不生效) | 用 sharpening 弥补 |
| 高画质 4K 桌面 | 0.5+ | OFF | bilinear | full-res 无需上采样 |
| **低开销移动 4K** | **0** | **ON** | **bicubic** | 推荐 |
| **低开销桌面 4K** | **0** | **ON** | **bicubic** | 推荐 |
| 调试/对比 | 0 | ON | demo_ssr P 键 | 实时切换 |

### 2.3 故障排查

| 现象 | 处理 |
|------|------|
| 切 bicubic 后画面无变化 | 检查 sharpness 是否 = 0 + halfRes 是否 = true |
| 看到黑斑 ringing | bicubic 负权重 ringing, max(result, 0) 已防护; 若仍出现请提 issue |
| 性能下降明显 | 切回 "bilinear" (-0.025 ms @ 1080p) |
| 高速运动 trail 增多 | 与 F.0.8 motion-adaptive γ 无关; 检查 blendAlpha 是否过高 |

---

## 3. CI 回填（待 T5 完成后填）

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 4. 候选 Phase

- F.0.10 — TAARenderer 多实例 split-screen demo
- F.0.11 — Demo 截图/录屏
- F.0.12 — RCAS (FSR2 增强)
- F.0.13 — Motion-adaptive sharpness (与 F.0.8 motion-adaptive γ 成对)
- F.0.14 — Lanczos-2 25-tap (画质再 ↑5%, +0.07 ms)

---

## 5. 总结

Phase F.0.9 实施完整，**无阻塞性遗留**。主要交付：

- shader: FS_BICUBIC_UPSCALE Catmull-Rom 9-tap (Sigggraph 2018 Filmic SMAA)
- backend: programBicubicUpscale + DrawTAAUpscalePass override
- TAARenderer: upscaleMode state + sharpness=0 路径切分支
- Lua API: +2 fn (TAA 29 → 31)
- smoke: 10 个 Phase F.0.9 PASS + 八启共存
- demo: P 键切换 + HUD sharp=0 路径 blit-bil/blit-bic 后缀
- 默认零回归 (upscaleMode="bilinear", 仅 sharpness=0+halfRes 时生效)
- HDR safe: max(result, 0) 防 ringing 黑斑

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
