# Phase F.0.2 TAA YCoCg Clip — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_2.md` / `ACCEPTANCE_PhaseF_0_2.md` / `FINAL_PhaseF_0_2.md`

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
| Phase F.0.5 — Half-res TAA history RT | TAA history VRAM -75% (移动 4K 友好) | 3h | 🟡 中 |
| Phase F.0.7 — Split-screen A/B demo | 屏幕分两半，左 RGB clip / 右 YCoCg clip 直接可视化对比 | 2h | 🟢 低 |
| Phase F.0.3 — Variance clipping | clip 替代算法 (与 F.0.2 互斥)；优先级降低，F.0.2 已基本覆盖色彩边缘需求 | 3h | 🟢 低 |
| Phase F.0.6 — 5-tap CAS sharpening | 替换 F.0.1 4-tap, 对角线频率补偿, AMD FSR2 算法 | 4h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| Phase F.1 — DLSS-like upscale (TAAU) | 性能 +50% (0.7× 渲染分辨率) | TAAU 算法 + history halfRes |
| Phase F.2 — Bloom + TAA sharp 联动 | Bloom 输入用 TAA 后 sharp HDR, Bloom 质量 +10% | 调整 EndScene pipeline 顺序 |
| FLIP / SSIM perceptual A/B | 自动量化 clip 算法画质收益 | 集成 NVIDIA FLIP 库 + reference scene |

---

## 4. 用户指引（启用建议）

### 4.1 默认配置（推荐）

```lua
local Gfx = require 'Light.Graphics'
local HDR, TAA = Gfx.HDR, Gfx.TAA

HDR.Enable(1280, 720)
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    -- F.0.2 默认 clipMode="ycocg", 不调用 SetClipMode 即可
    -- F.0.4 默认 antiFlicker=true, 与 F.0.2 互补
    -- F.0.1 默认 sharpness=0.5, 与上述配合最佳
    -- F.0   默认 alpha=0.92, neighborhoodClip=true, jitter=true
end
```

### 4.2 不同场景的 clipMode 建议

| 场景 | clipMode | 说明 |
|------|----------|------|
| **默认推荐** | **`"ycocg"`** | F.0.2 主题默认，色彩边缘最鲁棒 |
| 高色度边缘场景 (cyber-punk / 旗帜 / 天空) | `"ycocg"` | YCoCg 优势最显著 |
| 严格复现 F.0 行为 | `"rgb"` | 用户测试基线对比时使用 |
| 性能极端敏感场景 | `"rgb"` | 节省 +0.05 ms (但 YCoCg 总开销仍 < 0.2 ms 远低于 1% 帧预算) |
| Debug / 检查特定 clip 算法 | `"rgb"` 或 `"ycocg"` | 显式切换观察对比 |

### 4.3 性能对比表

| 模式 | TAA 总开销 @ 1080p | VRAM |
|------|---------------------|------|
| F.0 only | ~0.10 ms | 16 MB history |
| F.0 + F.0.2 (clipMode="ycocg") | ~0.15 ms (+0.05) | 16 MB |
| F.0 + F.0.2 + F.0.4 (默认) | ~0.16 ms (+0.06) | 16 MB |
| F.0 + F.0.1 sharp=0.5 + F.0.2 + F.0.4 (默认推荐) | ~0.19 ms (+0.09) | 16 MB |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（shader 改造内嵌 render_gl33.cpp）
- 不需要修改 CMake（taa_renderer.cpp 已在 F.0 加入）
- 不需要修改 CI workflow（taa.lua smoke 已在 F.0 注册）
- 不需要 .env / API key

---

## 6. CI 回填（待 T5 完成后填）

| 字段 | 值 |
|------|---|
| GitHub Run ID | `<pending>` |
| Commit hash | `<pending>` |
| 6/6 平台状态 | `<pending>` |
| Total duration | `<pending>` |
| Date | `<pending>` |

回填后同步更新：
- `ACCEPTANCE_PhaseF_0_2.md` 第 7 章
- `FINAL_PhaseF_0_2.md` 第 8 章

---

## 7. 总结

Phase F.0.2 实施完整，**无阻塞性遗留**。主要交付：

- YCoCg lift 形式 (1/4, 1/2, 1/4 系数, integer-reversible) shader 改造 (GLES + GL3.3 双版本, FS_TAA clip 段加 if 分支)
- 1 backend 接口参数 `int clipMode = 1` (带默认值, 向后兼容)
- 2 Lua API (`SetClipMode` / `GetClipMode`) 默认 "ycocg"，大小写不敏感解析 + 规范化小写存储
- shader 内嵌套 if 分支保护，clipMode="rgb" 时严格复现 F.0 RGB AABB clip 路径 (零 ALU 增量)
- 与 F.0.4 anti-flicker 独立互补 (clip 限制 history 范围 + blend 降权 high-luma firefly)
- 与 F.0.1 sharpening 完全正交 (作用不同 pipeline 阶段)
- smoke 19 fn / demo HUD / docs 同步更新

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
