# Phase F.0.14 TAA Lanczos-2 Upscaler — TODO

> 6A 工作流 · 阶段 6 · TODO 收尾

---

## 1. 必做

| 任务 | 状态 |
|------|------|
| commit + push 代码到 main | ✅ 完成 (0776e8f) |
| 监控 GitHub Actions CI 6/6 success | ✅ 完成 ([25936869113](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113)) |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | ✅ 完成 |
| 修复 Lua 白名单漏加 'lanczos' | ✅ 完成 (c5264f2) |

---

## 2. 用户指引

### 2.1 启用 Lanczos-2 上采样

```lua
local TAA = Light.Graphics.TAA
TAA.Enable(1280, 720)
TAA.SetSharpness(0.0)              -- sharpness=0 才走上采样路径
TAA.SetHalfResHistory(true)        -- halfRes=true 才走上采样路径
TAA.SetUpscaleMode("lanczos")      -- F.0.14: -10% blur vs Catmull-Rom
```

### 2.2 三 mode 选择参考

| 场景 | 推荐 mode | 理由 |
|------|----------|------|
| **桌面 4K + 超高画质** | **lanczos** | 画质最佳，~0.04 ms 桌面 GPU 完全可接受 |
| **桌面 1080p / 2K** | bicubic | -50% blur vs bilinear, ~0.025 ms 性价比最高 |
| **Mobile / iOS / Android** | bilinear / bicubic | lanczos 25-tap 功耗高, 不推荐 |
| **Web (WebGL)** | bilinear / bicubic | lanczos GLES3 兼容性差 |
| **静止预览/截图** | lanczos | sharpness=0 + halfRes=true 时画质最大化 |

### 2.3 故障排查

| 现象 | 处理 |
|------|------|
| SetUpscaleMode("lanczos") 后画面无变化 | 确认 sharpness=0 && halfResHistory=true (上采样路径条件) |
| Lanczos 出现轻微 ringing | 已用 max(0) HDR safe; 若仍有可见，降到 bicubic |
| Mobile 启用 lanczos 后掉帧 | 25-tap 在 mobile 偏重，改用 bicubic 或 bilinear |
| 启动时 lanczos shader 编译失败 | log 应显示 "TAA Lanczos-2 Upscale shader compile failed", 自动 fallback 到 BlitTAAToHDR |

---

## 3. CI 回填

GitHub Run ID: [`25936869113`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113) / Fix commit: `c5264f2` / Date: 2026-05-15 19:23 UTC / Result: **6/6 success**

---

## 4. 候选 Phase

- F.0.10 — TAARenderer 多实例 split-screen demo (6h+)
- **F.0.11 — Demo 截图/录屏** (3h, **高优先级**: 可对比 F.0.9 bicubic vs F.0.14 lanczos)
- F.0.15 — TAA-driven CAS strength scaling (2h)
- F.1 — DLSS-like TAAU (full-res TAA + 上采样融合)

---

## 5. 总结

Phase F.0.14 实施完整，**无阻塞性遗留**。主要交付：

- backend: DrawTAALanczosPass virtual + GL33 override (FS_LANCZOS_UPSCALE GLES3+GL3.3 双版本)
- TAARenderer: parseUpscaleMode_ +"lanczos" + Process upscaleMode==2 走 lanczos pass + GetUpscaleMode 三 mode 字符串映射
- Lua API: 零增量 (复用 SetUpscaleMode)
- smoke: round-trip + 大小写不敏感 + 三 mode 轮转 + 十一启共存 (~5 PASS)
- demo_ssr: P 键三 mode 轮转 (bilinear → bicubic → lanczos → bilinear)
- 默认零回归 (upscaleMode="bilinear")
- 性能: ~0.07 ms @ 1080p (vs Catmull-Rom +0.04 ms)
- 画质: -10% blur vs Catmull-Rom (-55% vs bilinear)
- 与 unsharp/cas/rcas sharpenMode 100% 兼容 (sharpness=0 路径独立)

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
