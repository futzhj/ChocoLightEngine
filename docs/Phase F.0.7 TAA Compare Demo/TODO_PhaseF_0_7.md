# Phase F.0.7 TAA Compare Demo — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_7.md` / `ACCEPTANCE_PhaseF_0_7.md` / `FINAL_PhaseF_0_7.md`

---

## 1. 必做（阻塞性，本 Phase 内）

| 任务 | 操作 | 优先级 |
|------|------|--------|
| commit + push 代码到 main | git add + commit + push origin main | 🔴 高 |
| 监控 GitHub Actions CI 6/6 平台 success | gh run view | 🔴 高 |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 编辑 3 份文档 §CI 状态 | 🔴 高 |

---

## 2. 推荐（Phase F.0.x 候选）

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| Phase F.0.6 — 5-tap CAS sharpening | 替换 F.0.1 4-tap, AMD FSR2 算法 | 4h | 🟡 中 |
| Phase F.0.8 — Motion-adaptive γ | 基于 velocity 长度动态调 γ | 3h | 🟢 低 |
| Phase F.0.9 — Custom upsampler | bicubic / Lanczos 替代 bilinear | 4h | 🟢 低 |
| Phase F.0.10 — TAARenderer 多实例化 + 真 split-screen demo | 真 left/right 双 viewport 双 TAA 实例 | 6h+ | 🟢 低 |
| Phase F.0.11 — Demo 截图 / 录屏 | frame-by-frame 截图 + GIF/MP4 导出 | 3h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| FLIP / SSIM perceptual A/B | 自动量化 preset 间画质差异 | 集成 NVIDIA FLIP 库 |
| Mobile GPU 实测 | iOS / Android 真机 demo 截图 | CI / 真机环境 |
| Phase F.1 — DLSS-like TAAU | upscale 1/2 → 2× 输出 (FSR2 风格) | TAAU 算法 + history 灵活倍率 |

---

## 4. 用户指引

### 4.1 启动 demo

```pwsh
cd samples\demo_taa_compare
..\..\Light\light.exe main.lua
```

### 4.2 推荐观察流程（教学路径）

| 步骤 | 操作 | 预期观察 |
|-----|------|---------|
| 1 | 启动 → 默认 preset 1 (OFF) | 严重 aliasing + firefly + 1px shimmer |
| 2 | 按 `2` 切到 F.0 base | aliasing 消失，但有 trail/ghosting |
| 3 | 等 30 帧 (HUD STABLE) | 公平对比 |
| 4 | 按 `3` 切到 F.0.1 sharpening | 边缘锐度回升 |
| 5 | 按 `4` 切到 F.0.2 YCoCg | 薄棒交叠 ghosting 减少 |
| 6 | 按 `5` 切到 F.0.3 variance | trail 收紧 |
| 7 | 按 `6` 切到 F.0.4 anti-flicker | 金色 cube 高光 firefly 消除 |
| 8 | 按 `7` 切到 F.0.5 half-res | 几乎不可辨；HUD 显示 VRAM -75% |
| 9 | 按 `8` 切到 ALL | 完整管线最佳画质 |
| 10 | 按 `R` 重置 history → 重新 stabilize | 用于反复对比 |

**重要**：每次切 preset 后等 HUD 显示 STABLE 才公平对比（history 收敛 ~30 帧）。

### 4.3 故障排查

| 现象 | 原因 / 处理 |
|------|------------|
| `[demo_taa_compare] need HDR + TAA subtables` | Phase E.3 + F.0 未启用，检查构建 |
| `Mesh.New not available` | Gfx.Mesh 模块缺失，检查 graphics.cpp 注册 |
| 切 preset 后画质看起来一样 | 等 history STABLE（HUD 显示 30/30）再观察 |
| HUD 不显示 | `win.DrawText` 为 nil，检查 Window OOP 接口版本 |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源
- 不需要修改 CMake (samples 目录由 lightc -p 自动覆盖)
- 不需要修改 CI workflow (demo 不入 smoke 链)
- 不需要 .env / API key

### 已有依赖

- HDR (Phase E.3)
- TAA (Phase F.0)
- TAA 子参数 (F.0.1 sharpness, F.0.2 clipMode, F.0.3 varianceGamma, F.0.4 antiFlicker, F.0.5 halfResHistory)
- Light.UI.Window
- Gfx.Mesh + Gfx.SetCamera + Gfx.SetPerspective + Gfx.Push/Translate/Rotate/Scale
- Window.DrawText (HUD)

---

## 6. CI 回填（待 T3 完成后填）

| 字段 | 值 |
|------|---|
| GitHub Run ID | `<pending>` |
| Commit hash | `<pending>` |
| 6/6 平台状态 | `<pending>` |
| Total duration | `<pending>` |
| Date | `<pending>` |

回填后同步更新：
- `ACCEPTANCE_PhaseF_0_7.md` 第 6 章
- `FINAL_PhaseF_0_7.md` 第 5 章

---

## 7. 总结

Phase F.0.7 实施完整，**无阻塞性遗留**。主要交付：

- demo `samples/demo_taa_compare/main.lua` (~480 行)
- 8 preset 一键切换 (1-8 数字键 + R 重置 + ESC 退出)
- 渐进式叠加设计 (OFF → F.0 → F.0.1 → ... → ALL) 让用户看到每个 Phase 边际贡献
- 高对比测试场景 (中央 cube + 8 薄棒 + 黑底)
- HUD 含 history stabilization 进度条 (避免用户切 preset 立即误判画质)
- ASCII-only HUD (γ → g / σ → sigma fallback)
- README 含推荐观察 8 步教学流程
- 零代码改动 (纯 demo Phase, 零回归风险)

**下一步**：T3 commit + push + CI 验证 6/6 平台 success。
