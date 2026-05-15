# Phase F.0.8 TAA Motion-Adaptive γ — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告
> 关联：`PLAN_PhaseF_0_8.md` / `ACCEPTANCE_PhaseF_0_8.md` / `TODO_PhaseF_0_8.md`

---

## 1. 交付物总览

| 交付物 | 文件 | 行数 |
|--------|------|------|
| GLES3 + GL33 shader | `ChocoLight/src/render_gl33.cpp` (FS_TAA) | +20 (含注释 / dynGamma 分支) |
| Backend struct + Init + Shutdown + DrawTAAPass | 同上 | +14 |
| Backend interface | `ChocoLight/include/render_backend.h` (DrawTAAPass virtual) | +10 |
| TAARenderer | `taa_renderer.h` + `.cpp` | +30 (state +2 + Set/Get +4 + Process 多 2 参数) |
| Lua API | `light_graphics.cpp` (4 函数 + taa_funcs[]) | +50 |
| smoke | `scripts/smoke/taa.lua` | +130 (Phase F.0.8 测试段 + surface 25→29) |
| demo | `samples/demo_ssr/main.lua` | +14 (Q 键 + HUD sγ/mγ + Keys help) |
| API doc | `docs/api/Light_Graphics.md` | +80 (速查表 +2 + 完整文档段) |
| 6A docs | `docs/Phase F.0.8 TAA Motion Adaptive Gamma/` 4 文件 | ~400 |

**累计**: 代码 ~140 行 + 文档 ~530 行

---

## 2. 核心方案

### 2.1 数据流

```
shader 内 (仅 uClipMode==2 && uMotionAdaptiveGamma==1):
    velocity = SampleVelocity(vUV)              // 已为 reproject 而 sample
    velLen = length(velocity)                    // UV 单位
    motionFactor = clamp(velLen / (4 * texel.x), 0, 1)   // 4 px 临界
    dynGamma = mix(varianceGamma, motionGamma, motionFactor)
    clip = [m1 - dynGamma·σ, m1 + dynGamma·σ]
```

### 2.2 关键性质

- **零额外 fetch**: velocity 已 sample (DRY)
- **+4 ALU 仅 motion-adaptive ON**: length / clamp / mix; OFF 时静默走老路径
- **零回归**: motionAdaptive=false 默认，clipMode!=variance 时不影响
- **接口默认参数**: 老 caller 自动 motionGamma=1.5 + motionAdaptive=0
- **简单可控**: 单个 `motionGamma` + 单个开关，无复杂 threshold / 曲线

---

## 3. API surface (新增 4 个, TAA 25 → 29 fn)

```lua
-- 启用 motion-adaptive γ (UE5 默认)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.0)            -- 静止 γ (严格防 ghost)
TAA.SetMotionGamma(1.5)              -- 高速 γ (宽容防 trail)
TAA.SetMotionAdaptive(true)
```

---

## 4. Phase F.0 系列累计 (8 sub-phase)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0   | TAA 主管线 | 13 |
| F.0.1 | 4-tap unsharp | +2 (15) |
| F.0.4 | Karis anti-flicker | +2 (17) |
| F.0.2 | YCoCg AABB clip | +2 (19) |
| F.0.3 | YCoCg variance clip | +2 (21) |
| F.0.5 | half-res history | +2 (23) |
| F.0.7 | compare demo | +0 (23) |
| F.0.6 | 5-tap CAS sharpening | +2 (25) |
| **F.0.8** | **motion-adaptive γ** | **+4 (29)** |

**累计**: 29 fn / 3 shader (FS_TAA + FS_SHARPEN + FS_CAS) / 2 backend pass / 1 backend 接口扩展 / 3 demos

---

## 5. CI 状态（待回填）

| 平台 | 状态 |
|------|------|
| build-windows | ⏳ |
| build-linux/macos/android/ios/web | ⏳ |

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 6. 工程反思

### 做得好

1. **复用 velocity sample**: 节省 1 fetch
2. **shader 默认 OFF 路径零成本**: motionAdaptive=0 时跳过 if 分支
3. **接口默认参数**: 老 caller 零改动
4. **决策矩阵全自动**: 8/8 决策点基于 UE5 标准

### 可改进

1. **未做 perceptual A/B 测试**: 静止 vs 高速场景画质量化
2. **threshold 4*texel.x 硬编码**: 未来可能需要用户可调
3. **linear lerp**: smoothstep 平滑过渡更优雅但 +ALU
4. **未实现 motion-adaptive sharpness**: 未来候选 (高速时降低 sharpness 减 trail 强化)

---

## 7. Phase F.0.x 后续候选

- F.0.9 — Custom upsampler (bicubic / Lanczos 替代 F.0.5 bilinear)
- F.0.10 — TAARenderer 多实例化 + 真 split-screen
- F.0.11 — Demo 截图 / 录屏
- F.0.12 — RCAS (FSR2 增强)
- F.1 — DLSS-like TAAU
