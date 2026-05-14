# Phase E.14 Velocity Dilation + RG8 Format — ALIGNMENT 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）
> **目标**：在 Phase E.13 motion vector velocity buffer 之上做底层优化 —— 几何边缘 dilation 抗锯齿 + 可选 RG8 存储压缩
> **当前状态**：规划草案，尚未进入实现

---

## 1. 核心结论

Phase E.14 在 Phase E.13 完整 velocity buffer 基础上做两项**底层优化**：

1. **Velocity dilation**：SSRTemporal shader 采样 velocity 时改为 **3x3 max-length 邻域**，抑制几何边缘 1-pixel 误差导致的 Temporal SSR halo / 飞点（业界 TAA / motion blur 经典做法）
2. **RG8 可选格式**：velocity texture 可选切换为 `GL_RG8` + bias/scale 编码，VRAM 从 RG16F 的 8MB/1080p 降到 2MB/1080p（移动端显存友好）

通俗说：E.13 给每个像素发了"移动小箭头"，但几何边缘的箭头有 1 pixel 抖动；E.14 在边缘像素上"参考一下邻居家的箭头取最大值"。同时把箭头从 16-bit float 改成 8-bit unsigned + 校准刻度，移动端不再吃 8MB 显存。

---

## 2. 已确认决策

| 决策项 | 结论 | 备注 |
|---|---|---|
| E.14 范围 | **合并 dilation + RG8 可选** | 用户拍板 2026-05-14 |
| dilation 算法 | **3x3 max-length 邻域** | 业界主流；与 TAA / motion blur 共用模式 |
| dilation 实施位置 | **SSRTemporal shader 内 inline** | 当前唯一消费者是 SSR Temporal；避免新增 RT |
| dilation 默认 | **默认开启，可关闭** | Lua API `HDR.SetVelocityDilation(bool)` |
| RG8 默认 | **默认仍 RG16F，可选切到 RG8** | Lua API `HDR.SetVelocityFormat("rg16f"/"rg8")`；切换需 ReleaseRT + CreateRT |
| RG8 编码 | **UNORM + bias/scale** `[-scale, +scale] → [0, 1]` | GL3.3 + ES3 兼容；避免 SNORM 扩展依赖 |
| 默认 velocity scale | **`kVelocityScale = 0.25`** | 即 ±0.25 UV / frame，约 ±540 px @ 1920×1080 / ±270 px @ 960×540，覆盖现实运动 |
| 兼容性 | **旧 Lua / demo / smoke 全部 0 改动通过** | dilation 默认 ON 行为是质量增量，不破坏现有 Temporal SSR 视觉 |
| 本地验证约束 | 遵循前序约束：不做本地 CMake build / runtime smoke，优先静态检查与 CI | 与 Phase E.12/E.13 一致 |

---

## 3. 现有项目上下文（Phase E.13 闭环后）

### 3.1 Velocity buffer 创建路径

```
HDRRenderer::CreateRT (hdr_renderer.cpp:65)
  └─ backend->CreateHDRFBO(w, h, &tex, &normalTex, &velocityTex)
       └─ render_gl33.cpp:3617 CreateHDRFBO
            ├─ 行 3648-3666: 创建 velocityTex
            │    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr)
            │    GL_NEAREST + CLAMP_TO_EDGE
            └─ 行 3731-3734: hdrFboVelocityTex[fbo] = velocityTex; *outVelocityTex = velocityTex
```

| 项 | 当前 | E.14 目标 |
|---|---|---|
| internalFormat | `GL_RG16F` | `GL_RG16F`（默认） 或 `GL_RG8`（可选） |
| filter | `GL_NEAREST` | 不变 |
| wrap | `CLAMP_TO_EDGE` | 不变 |
| 创建路径 | 单一接口 | 增加 `velocityFormat` 入参 |

### 3.2 Velocity 写入路径（4 个 3D shader）

| Shader | 文件位置 | velocity 写入 |
|---|---|---|
| Static Unlit | `render_gl33.cpp:288-307` (GL33), `~612-633` (ES3) | `FragVelocity = curUV - prevUV` |
| Static PBR | `render_gl33.cpp:351-444` (GL33), `~676-768` (ES3) | 同上 |
| Skin Unlit | 类似 | 同上 |
| Skin Morph PBR | 类似 | 同上 |

**E.14 改动**：所有 4 个 shader × 2 profile 的 velocity 输出都要按 `velocityFormat` 走两条编码路径：

```glsl
// 当前 (RG16F)
FragVelocity = curUV - prevUV;

// E.14 RG8 模式
FragVelocity = (curUV - prevUV) / (2.0 * uVelocityScale) + 0.5;
// 等价 (curUV - prevUV) ∈ [-uVelocityScale, +uVelocityScale] → [0, 1] UNORM
```

### 3.3 Velocity 消费路径（唯一）

```
SSRRenderer::Process (ssr_renderer.cpp:418-424)
  └─ backend->DrawSSRTemporal(..., GetHDRVelocityTex(hdrFbo), ...)
       └─ render_gl33.cpp:DrawSSRTemporal 上传 uVelocityTex 到 slot 3
            └─ FS_SSR_TEMPORAL_SOURCE (render_gl33.cpp:~1880 + ~2240, 双 profile)
                 prevUV = vUV - texture(uVelocityTex, vUV).rg
```

**E.14 改动**：把单点采样改为 3x3 max-length，并感知 RG8 编码：

```glsl
// 辅助函数: 取邻域最大长度的 velocity (业界 TAA classic)
vec2 SampleVelocityDilated(sampler2D tex, vec2 uv, vec2 texel, int dilation) {
    if (dilation == 0) return DecodeVelocity(texture(tex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        vec2 v = DecodeVelocity(texture(tex, uv + vec2(dx, dy) * texel).rg);
        float l = dot(v, v);
        if (l > bestLen) { bestLen = l; bestV = v; }
    }
    return bestV;
}
```

### 3.4 HDR post 链

| Pass | 顺序 | 是否依赖 velocity |
|---|---|---|
| Bloom (Phase E.4) | EndScene 前 | 否 |
| SSR + Temporal SSR | EndScene 前 | **是**（仅 Temporal） |
| Lens FX (Phase E.5/E.6) | EndScene 前 | 否 |
| Auto Exposure | EndScene 前 | 否 |
| Tonemap | EndScene 末尾 | 否 |
| `CommitVelocityHistory()` | Tonemap 后 | velocity 历史推进 |

**E.14 改动面**：仅 SSR Temporal pass + HDR RT 创建参数 + 4 个 3D shader velocity 编码。HDR post 链结构不变。

### 3.5 Lua / 用户脚本接入路径

当前 Lua API（Phase E.13 后）：

```lua
Light.Graphics.HDR.Enable(w, h)         -- 自动带 velocity
Light.Graphics.HDR.Disable()
Light.Graphics.SSR.SetTemporalEnabled(bool)
Light.Graphics.SSR.SetTemporalAlpha(f)
Light.Graphics.SSR.SetRejectionMode(int)
```

E.14 新增（与 Phase E.13 风格一致）：

```lua
-- Dilation 开关 (默认 true)
Light.Graphics.HDR.SetVelocityDilation(bool)   -- nil + err on bad arg
Light.Graphics.HDR.GetVelocityDilation() -> bool

-- 存储格式切换 (默认 "rg16f"; 切换会 ReleaseRT + CreateRT)
Light.Graphics.HDR.SetVelocityFormat(string)   -- "rg16f" | "rg8"
Light.Graphics.HDR.GetVelocityFormat() -> string
```

不需要新增模块 / 子表；都挂在 `HDR` 子表，与 Phase E.13 接入点对齐。

---

## 4. 任务边界

### In Scope

| 项 | 说明 |
|---|---|
| GL33 backend velocity texture 格式切换 | 支持 `GL_RG16F` (默认) + `GL_RG8` (可选) |
| 4 个 3D shader × 2 profile velocity 编码分支 | 通过新 uniform `uVelocityScale` (RG8 时生效) |
| SSRTemporal shader 3x3 max-length dilation | 通过新 uniform `uVelocityDilation` (0/1) + `uVelocityScale` |
| HDRRenderer / SSRRenderer state 扩展 | 持有 velocityDilation / velocityFormat 状态 |
| Lua API：HDR.SetVelocityDilation / GetVelocityDilation | 与 Phase E.13 同模式 |
| Lua API：HDR.SetVelocityFormat / GetVelocityFormat | 切换触发 RT 重建 |
| 默认行为不破坏 | dilation 默认 ON 是质量增量；format 默认 RG16F 与 E.13 完全相同 |
| 静态 smoke 覆盖 | `scripts/smoke/hdr.lua` + `ssr.lua` 加新 API 表面与边界检查 |
| 文档 | `docs/Phase E.14 Velocity Dilation RG8/` 完整 6A 文档；`docs/api/Light_Graphics.md` 补 HDR 子表新 API |
| CI 6 平台 | 与 E.13 同样 PR push → 6 平台 build + Windows runtime smoke |

### Out of Scope（明确不做）

| 项 | 原因 |
|---|---|
| Velocity dilation 独立 fullscreen pass | 当前 velocity 唯一消费者是 SSR Temporal，shader inline 足够；将来 motion blur / TAA 引入再抽离 |
| 5x5 / cross-pattern dilation | 3x3 是业界 sweet spot；更大 kernel 性能成本高且收益有限 |
| RG8 → RG16F 动态切换的"无缝"重建 | 切换时直接 ReleaseRT + CreateRT，与 HDR.Disable + HDR.Enable 同模式 |
| 自适应 `uVelocityScale` | 固定 0.25；动态评估需历史统计，复杂度溢出本期 |
| velocity 写入侧 dilation | 写入侧不能 dilation（每个像素只能写自己一格） |
| CPU skin / morph 路径写 velocity | 与 Phase E.13 留 TODO 一致，本期不动 |
| API_REFERENCE.md 全局 SSR 段重写 | 与 Phase E.13 留 TODO 一致 |

---

## 5. 关键决策点（剩余、需用户审阅）

我已经把"明显默认"决策直接采用。以下两项**对工程行为有影响**但我倾向于直接采用合理默认，列出来供你审阅；若你不反对就视为已确认。

### 5.1 默认 `kVelocityScale` 数值

| 候选 | UV 上限 | 1920×1080 像素上限 / 帧 | 适用场景 |
|---|---|---|---|
| 0.10 | ±0.10 UV | ±192 px | 慢节奏游戏（解谜 / 策略） |
| **0.25**（**默认采用**） | ±0.25 UV | ±480 px / ±540 px | 通用动作类，足以覆盖 90% 现实运动 |
| 0.50 | ±0.50 UV | ±960 px | 极端快速摄像机 / 子弹时间 |

**采用 0.25**。精度损失约为 0.25 × 2 / 256 ≈ 0.002 UV ≈ 2 像素 @ 1080p，对 SSR Temporal 视觉影响可忽略。

### 5.2 切换 RG8 时是否要"自动 ResetVelocityHistory"

| 候选 | 行为 |
|---|---|
| 不重置 | 切换后第一帧 history 还是 RG16F 编码的旧数据，shader 用 RG8 decode 会读到错值 |
| **自动重置**（**默认采用**） | `SetVelocityFormat` 内部走 `Disable + Enable` 的等价路径；下一帧 `uHasVelocityHistory = 0`，velocity 输出 0 |

**采用自动重置**。与 `Enable(w, h)` 在尺寸变化时调 `ReleaseRT + CreateRT` 同模式，行为可预期。

---

## 6. 验收标准

| 类别 | 标准 |
|---|---|
| **静态** | 4 个 Lua smoke + 1 个 ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO 文档完整；`git diff --check` clean；`lightc -p` 全部 exit 0 |
| **CI** | GitHub Actions 6 平台 build 全 success；Windows runtime smoke 中 `hdr.lua` + `ssr.lua` 0 fail |
| **行为** | 默认 (dilation ON, RG16F) 下，所有 Phase E.12/E.13 demo + smoke 视觉与帧时间无回归 |
| **可观察** | Lua 端能 query `HDR.GetVelocityDilation()` / `HDR.GetVelocityFormat()` 与设置同步 |
| **真机视觉**（用户侧） | 桌面 GL3.3 下，开 dilation 后 Temporal SSR 几何边缘抖动 / halo 减弱；切到 RG8 后 VRAM 减少且视觉差异可接受 |

---

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| RG8 精度不足导致 Temporal SSR 边缘错位 | 视觉伪影 | 默认保持 RG16F；`kVelocityScale = 0.25` 在 1080p 下精度损失 < 0.002 UV |
| 3x3 dilation 在低端机性能下降 | 帧时间增加 | dilation 默认 ON 但提供 Lua 关闭开关；fullscreen pass 内 9 次 texture() 与 Bloom 同量级 |
| 切换 format 重建 RT 卡顿 | 单帧延迟 | 与 `HDR.Disable + Enable` 同模式，预期用户在场景切换或选项菜单触发，不是热路径 |
| 新 uniform 在旧 shader 缓存里找不到 | 上传被 driver 跳过 | 与 Phase E.13 `uHasVelocityHistory` 同模式，已验证 GL3.3 driver 行为正确 |
| 测试用 `ssr.lua` 无 HDR 上下文 | API 调用返回 nil + err | smoke 需要构造 HDR Enable 上下文或显式跳过；headless 路径要 graceful |

---

## 8. 输入 / 输出契约

### 输入
- Phase E.13 已落地（commit `9f32401` 及之前）
- main 分支当前 head：`9f32401`
- 6 平台 CI 处于 green 基线

### 输出
- 1 个新目录 `docs/Phase E.14 Velocity Dilation RG8/`
- 改动文件预估 ≈ 6~8 个（render_gl33.cpp、render_backend.h、hdr_renderer.h/cpp、ssr_renderer.cpp、light_graphics_hdr.cpp Lua 绑定、hdr.lua/ssr.lua smoke、Light_Graphics.md）
- 1 个新 commit feat + 1 个 docs CI green 跟踪 commit（与 Phase E.13 模式一致）

---

## 9. 决策最终共识（用户拍板 2026-05-14）

1. ✅ 范围合并 dilation + RG8
2. ✅ 默认 `kVelocityScale = 0.25`（5.1）
3. ✅ 切 RG8 时自动 ResetVelocityHistory（5.2）
4. ✅ Lua API 暴露 4 个新接口：`HDR.Set/GetVelocityDilation` + `HDR.Set/GetVelocityFormat`
5. ✅ dilation 默认开启（可通过 Lua API 关闭）

**下一步**：进入 6A 阶段 2 Architect，生成 `DESIGN_PhaseE_14.md` + `IMPLEMENTATION_PLAN_PhaseE_14.md`。
