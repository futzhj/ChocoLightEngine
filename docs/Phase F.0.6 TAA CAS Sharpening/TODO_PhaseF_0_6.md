# Phase F.0.6 TAA CAS Sharpening — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_6.md` / `ACCEPTANCE_PhaseF_0_6.md` / `FINAL_PhaseF_0_6.md`

---

## 1. 必做（阻塞性，本 Phase 内）

| 任务 | 操作 | 状态 |
|------|------|------|
| commit + push 代码到 main | git add + commit + push origin main | ✅ 完成 (commit `7b14f46`) |
| 监控 GitHub Actions CI 6/6 平台 success | gh run view 25931268319 | ✅ 完成 (Run 25931268319, 11m42s) |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 编辑 3 份文档 §CI 状态 | ✅ 完成 |

---

## 2. 推荐（Phase F.0.x 候选）

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| Phase F.0.8 — Motion-adaptive γ | 基于 velocity 长度动态调 γ | 3h | 🟡 中 |
| Phase F.0.9 — Custom upsampler | bicubic / Lanczos 替代 bilinear | 4h | 🟢 低 |
| Phase F.0.10 — TAARenderer 多实例化 + 真 split-screen demo | 真 left/right 双实例 | 6h+ | 🟢 低 |
| Phase F.0.11 — Demo 截图 / 录屏 | frame-by-frame 截图 + GIF/MP4 | 3h | 🟢 低 |
| Phase F.0.12 — RCAS (Robust CAS, FSR2) | CAS 增强版处理 deringing | 4h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| FLIP / SSIM perceptual A/B | 自动量化 unsharp vs CAS 画质差异 | 集成 NVIDIA FLIP 库 |
| Mobile GPU 实测 | iOS / Android 真机数据 | CI / 真机环境 |
| Phase F.1 — DLSS-like TAAU | upscale 1/2 → 2× 输出 (FSR2 风格) | TAAU 算法 + history 灵活倍率 |

---

## 4. 用户指引

### 4.1 启用 CAS

```lua
local TAA = Light.Graphics.TAA
TAA.Enable(1280, 720)

-- 切到 CAS (HDR 强高光场景推荐)
TAA.SetSharpenMode("cas")
TAA.SetSharpness(0.6)        -- CAS 范围 [0, 1], 0.6 = 中高强度
```

### 4.2 不同场景的 sharpenMode 建议

| 场景 | 推荐 mode | sharpness | 理由 |
|------|----------|-----------|------|
| 低对比场景 (天空 / 雾 / 阴影) | **`"cas"`** | `0.5` | 不锁牐噪点 |
| HDR 强高光场景 (金属 / 灯光) | **`"cas"`** | `0.5-0.7` | HDR safe 防 firefly 加剧 |
| 高丝节贴图场景 | `"unsharp"` 又 `"cas"` 都可 | `0.5` | 视个人偏好 |
| 低端移动设备 | `"unsharp"` | `0.5` | 少 ALU |
| **桌面 / 中高端移动** | **`"cas"`** | `0.6` | 推荐生产配置 |
| 纯粉 / 2D 场景 | `"unsharp"` | `0.3` | 低对比表现不明显 |
| 调试对比 | demo_ssr Z 键 | - | 实时切换 |

### 4.3 sharpness 语义注意

**重要**：`sharpness` 字段在两个 mode 中含义不同：

| mode | 范围 | 0.5 实际效果 |
|------|------|-------------|
| `"unsharp"` | [0, 2] | 中等强度 4-tap 锐化 |
| `"cas"` | [0, 1] (内部 clamp) | peak=-1/6.5 中等 contrast-adaptive |

不可直接同值比较 (e.g. "unsharp 0.5" vs "cas 0.5" 视觉强度不同)。建议：
- unsharp 推荐 `0.5`
- CAS 推荐 `0.6`（同视觉强度感）

### 4.4 故障排查

| 现象 | 原因 / 处理 |
|------|------------|
| `TAA.SetSharpenMode: 未识别的 mode 'XXX'` | 仅接受 `"unsharp"` / `"cas"`（大小写不敏感） |
| 切到 CAS 后画面看起来无变化 | 检查 sharpness 是否 > 0; 等 ~30 帧 history 稳定 |
| HDR 高光仍闪烁 | CAS HDR safe 仅防 sharpening 不放大 firefly；本身需配 F.0.4 anti-flicker |
| 低端 GPU CAS 卡顿 | 切回 `"unsharp"` (-0.02 ms) 或减 sharpness |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（FS_CAS 直接编入 render_gl33.cpp）
- 不需要修改 CMake (taa_renderer.cpp / render_gl33.cpp 已编)
- 不需要修改 CI workflow (taa.lua smoke 已注册)
- 不需要 .env / API key

### 已有依赖

- HDR (Phase E.3)
- TAA (Phase F.0)
- Phase F.0.1 sharpness 字段（共享）
- vaoTonemap (复用全屏 quad VAO)

---

## 6. CI 回填 (✅ 全部完成)

| 字段 | 值 |
|------|---|
| GitHub Run ID | [25931268319](https://github.com/futzhj/ChocoLightEngine/actions/runs/25931268319) |
| Commit hash | `7b14f46` |
| 6/6 平台状态 | ✅ 全部 success |
| Total duration | `11m42s` (17:16:34 → 17:28:16 UTC) |
| Date | `2026-05-15` |

回填完成 ✅：
- `ACCEPTANCE_PhaseF_0_6.md` 第 7 章 已更新
- `FINAL_PhaseF_0_6.md` 第 6 章 已更新

---

## 7. 总结

Phase F.0.6 实施完整，**无阻塞性遗留**。主要交付：

- AMD FidelityFX FSR1 5-tap CAS 算法 (FS_CAS shader, GLES3 + GL33 双版本)
- TAARenderer sharpenMode state + Process 切分支 (CAS sharpness clamp [0, 1])
- Lua API SetSharpenMode / GetSharpenMode (大小写不敏感, invalid 返 nil+err)
- 与 F.0.1 unsharp 共存，默认 "unsharp" 零回归
- HDR safe clamp(0, ∞) 适配 ChocoLight HDR pipeline (与 FSR1 LDR 不同)
- 跨平台 case-insensitive 手写 (避免 strcasecmp Windows 问题)
- 性能 +0.02 ms (1080p), VRAM 0 (in-place)
- smoke 25 fn / demo Z 键切换 + HUD 字段 / docs 同步更新

**状态**: ✅ 全部交付完成，CI 6/6 平台验证通过。
