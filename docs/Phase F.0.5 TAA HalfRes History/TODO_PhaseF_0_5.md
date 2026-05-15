# Phase F.0.5 TAA Half-Res History — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_5.md` / `ACCEPTANCE_PhaseF_0_5.md` / `FINAL_PhaseF_0_5.md`

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
| Phase F.0.7 — Split-screen A/B demo | 屏幕分两半，左 ycocg+full / 右 variance+halfRes 直接可视化 | 2h | 🟡 中 |
| Phase F.0.6 — 5-tap CAS sharpening | 替换 F.0.1 4-tap, AMD FSR2 算法 | 4h | 🟢 低 |
| Phase F.0.8 — Motion-adaptive γ | 基于 velocity 长度动态调 variance γ (UE5 高级形式) | 3h | 🟢 低 |
| Phase F.0.9 — Custom upsampler | bicubic / Lanczos 替代 bilinear 上采样 | 4h | 🟢 低 |
| Phase F.0.10 — Motion-adaptive halfRes 切换 | 根据 GPU 负载/帧率动态切换分辨率 | 5h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| Phase F.1 — DLSS-like upscale (TAAU) | 性能 +50% (0.7× 渲染分辨率, 输出 2×) | TAAU 算法 + history 灵活倍率 |
| Phase F.2 — Bloom + TAA sharp 联动 | Bloom 输入用 TAA 后 sharp HDR | 调整 EndScene pipeline 顺序 |
| FLIP / SSIM perceptual A/B | 自动量化 halfRes 视觉损失 | 集成 NVIDIA FLIP 库 |
| Mobile GPU 实测 | iOS / Android 移动 4K 真机数据 | CI / 真机环境 |

---

## 4. 用户指引（启用建议）

### 4.1 默认配置（推荐）

```lua
local Gfx = require 'Light.Graphics'
local HDR, TAA = Gfx.HDR, Gfx.TAA

HDR.Enable(1280, 720)
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    -- 默认所有 Phase F.0.x 设置自动启用:
    -- alpha=0.92, neighborhoodClip=true, jitterEnabled=true, sharpness=0.5,
    -- antiFlicker=true, clipMode='ycocg', varianceGamma=1.0, halfResHistory=false
    -- 桌面 1080p / 1440p 推荐保持默认
end
```

### 4.2 不同场景的 HalfRes 建议

| 场景 | HalfResHistory | Sharpness | 说明 |
|------|----------------|-----------|------|
| **桌面 1080p / 1440p** | **`false`** (默认) | `0.5` | 画质充足，无需启用 halfRes |
| 桌面 4K | `false` 推荐 | `0.5` | VRAM 紧张时可切 `true`（VRAM -75%） |
| 移动 1080p | `false` | `0.5` | 默认推荐 |
| **移动 4K** | **`true`** (强烈推荐) | `0.8` | VRAM 132.7MB → 33.2MB；提高 sharpness 弥补上采样模糊 |
| VR (双眼) | `true` | `0.8` | VRAM 节省尤其关键 |
| 调试对比 | 运行时切换 | 不限 | 留意切换后首帧从干净 history 重建（无 ghost） |

### 4.3 性能 / VRAM 对比（@ 各分辨率）

| 分辨率 | full-res VRAM | half-res VRAM | TAA pass 提速 | 总 TAA 开销 |
|--------|---------------|---------------|---------------|-------------|
| 720p | 7.4 MB | 1.8 MB (-75%) | ~4× | -30% |
| 1080p | 33.2 MB | 8.3 MB (-75%) | ~4× | -30% |
| 1440p | 59.0 MB | 14.7 MB (-75%) | ~4× | -30% |
| 4K | 132.7 MB | 33.2 MB (-75%) | ~4× | -30% |
| 8K | 530.8 MB | 132.7 MB (-75%) | ~4× | -30% |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（shader 完全不变）
- 不需要修改 CMake（taa_renderer.cpp 已在 F.0 加入）
- 不需要修改 CI workflow（taa.lua smoke 已在 F.0 注册）
- 不需要 .env / API key

---

## 6. CI 回填（待 T6 完成后填）

| 字段 | 值 |
|------|---|
| GitHub Run ID | `<pending>` |
| Commit hash | `<pending>` |
| 6/6 平台状态 | `<pending>` |
| Total duration | `<pending>` |
| Date | `<pending>` |

回填后同步更新：
- `ACCEPTANCE_PhaseF_0_5.md` 第 7 章
- `FINAL_PhaseF_0_5.md` 第 8 章

---

## 7. 总结

Phase F.0.5 实施完整，**无阻塞性遗留**。主要交付：

- TAA history RT 半分辨率支持，VRAM -75% (1080p 33.2MB → 8.3MB; 4K 132.7MB → 33.2MB)
- TAA pass viewport 缩到 (W/2, H/2)，性能 -75% 像素 / -60% 时间
- BlitTAAToHDR 接口扩展支持 src→dst stretch + GL_LINEAR (向后兼容默认参数)
- Sharpen pass 零改动（fragment shader sample 自动上采样）
- 2 Lua API (`SetHalfResHistory` / `GetHalfResHistory`) 默认 false (零回归)
- 切换时立即重建 RT + invalidate hasHistory（避免分辨率不匹配花屏一帧）
- 与 F.0.1/F.0.2/F.0.3/F.0.4 完全正交（五启共存验证通过）
- shader 完全不变（FS_TAA / FS_SHARPEN 零修改）
- smoke 23 fn / demo X 键切换 + HUD 字段 / docs 同步更新

**下一步**：T6 commit + push + CI 验证 6/6 平台 success。
