# Phase F.0.4 TAA Anti-flicker — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_4.md` / `ACCEPTANCE_PhaseF_0_4.md` / `FINAL_PhaseF_0_4.md`

---

## 1. 必做（阻塞性，本 Phase 内）— **全部完成 ✅**

| 任务 | 操作 | 状态 |
|------|------|------|
| commit + push 代码到 main | `git commit -F` + `git push origin main` | ✅ 完成（commit `361a56f`） |
| 监控 GitHub Actions CI 6/6 平台 success | `gh run view 25917658584` | ✅ 完成（6/6 全 success） |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 编辑 3 份文档 §CI 状态 | ✅ 完成 |

---

## 2. 推荐（Phase F.0.x 候选）

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| Phase F.0.5 — Half-res TAA history RT | TAA history VRAM -75% (移动 4K 友好) | 3h | 🟡 中 |
| Phase F.0.6 — 5-tap CAS sharpening | 替换 4-tap, 对角线频率补偿, AMD FSR2 算法 | 4h | 🟢 低 |
| Phase F.0.2 — YCoCg color-space clip | TAA blend 阶段更鲁棒 clip (独立于 anti-flicker) | 4h | 🟢 低 |
| Phase F.0.3 — Variance clipping | clip 替代算法, AABB → 均值±k×σ；与 F.0.4 协同进一步消 firefly | 3h | 🟢 低 |
| Phase F.0.7 — Split-screen A/B demo | 把 sceneTex 切两半，左 AF=on / 右 AF=off，可视化对比 | 2h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| Phase F.1 — DLSS-like upscale (TAAU) | 性能 +50% (0.7× 渲染分辨率) | TAAU 算法 + history halfRes |
| Phase F.2 — Bloom + TAA sharp 联动 | Bloom 输入用 TAA 后 sharp HDR, Bloom 质量 +10% | 调整 EndScene pipeline 顺序 |
| FLIP / SSIM perceptual A/B | 自动量化 firefly 抑制效果 | 集成 NVIDIA FLIP 库 + reference scene |

---

## 4. 用户指引（启用建议）

### 4.1 默认配置（推荐）

```lua
local Gfx = require 'Light.Graphics'
local HDR, TAA = Gfx.HDR, Gfx.TAA

HDR.Enable(1280, 720)
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    -- F.0.4 默认 antiFlicker=true, 不调用 SetAntiFlicker 即可
    -- F.0.1 默认 sharpness=0.5, 与 anti-flicker 配合最佳
    -- F.0 默认 alpha=0.92, clip=true, jitter=true
end
```

### 4.2 不同场景的 antiFlicker 建议

| 场景 | antiFlicker | sharpness | 说明 |
|------|-------------|-----------|------|
| 默认推荐 | **true** | 0.5 | F.0.1 + F.0.4 协同，平衡画质 / 性能 |
| 高细节 + 高动态 HDR (cyber-punk) | **true** | 0.8 | Karis weighting 抑制 sharpening 加剧的 firefly |
| 严格复现 F.0 行为 | false | 0.5 | 用户测试基线对比时使用 |
| 性能敏感场景 | true | 0.0 | sharpness=0 走 blit fallback，anti-flicker 仅 +0.01 ms |
| Debug / 检查 firefly 严重程度 | false | 0.0 | 完全裸 TAA blend，观察原始 firefly 行为 |

### 4.3 性能对比表

| 模式 | TAA 总开销 @ 1080p | VRAM |
|------|---------------------|------|
| F.0 only | ~0.10 ms | 16 MB history |
| F.0 + F.0.4 AF=on | ~0.11 ms (+0.01) | 16 MB |
| F.0 + F.0.1 sharp=0.5 + F.0.4 AF=on (默认) | ~0.14 ms (+0.04) | 16 MB |
| F.0 + F.0.1 sharp=1.2 + F.0.4 AF=on (高锐化) | ~0.14 ms (+0.04) | 16 MB |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（shader 改造内嵌 render_gl33.cpp）
- 不需要修改 CMake（taa_renderer.cpp 已在 F.0 加入）
- 不需要修改 CI workflow（taa.lua smoke 已在 F.0 注册）
- 不需要 .env / API key

---

## 6. CI 回填（已完成 ✅）

| 字段 | 值 |
|------|---|
| GitHub Run ID | `25917658584` |
| Commit hash | `361a56ff269f4f30889ed232f77f66f2611e559e` (`361a56f`) |
| 6/6 平台状态 | ✅ 全 success（windows/linux/macos/android/ios/web；release skipped 符合预期） |
| Total duration | ~8 分 27 秒（12:25:40Z → 12:34:07Z UTC） |
| Date | 2026-05-15 |
| Windows runtime smoke | 6 个 F.0.4 PASS + Functions covered: 17 / 17 |

已同步更新：
- `ACCEPTANCE_PhaseF_0_4.md` 第 7 章 ✅
- `FINAL_PhaseF_0_4.md` 第 8 章 ✅

---

## 7. 总结

Phase F.0.4 实施完整，**无阻塞性遗留**。主要交付：

- Karis luma weighting blend shader 改造 (GLES + GL3.3 双版本, FS_TAA blend 段加 if 分支)
- 1 backend 接口参数 `int antiFlicker = 1` (带默认值, 向后兼容)
- 2 Lua API (`SetAntiFlicker` / `GetAntiFlicker`) 默认 true
- shader 内 if 分支保护, antiFlicker=0 走 F.0 纯 alpha blend (零 ALU 回退)
- 与 F.0.1 sharpening 天然协同 (压制 sharpening 加剧的 firefly)
- smoke / demo / docs 同步更新到 17 函数

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
