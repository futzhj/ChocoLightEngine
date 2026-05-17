# Phase F.1.1 Mipmap LOD Bias — PLAN 文档

> **阶段**：6A Workflow 合并版 (ALIGNMENT + CONSENSUS + DESIGN + TASK)
> **目标**：TAAU 启用时自动调整纹理采样的 mipmap LOD bias，让低分辨率渲染下纹理细节锐度提升至接近 native 水平
> **基线**：Phase F.1.0.1 完结 (commit pending, 2026-05-17)
> **创建日期**：2026-05-17
> **配套**：FINAL_PhaseF_1_1.md (实施记录) + ACCEPTANCE_PhaseF_1_1.md (验收)

---

## 1. 背景

Phase F.1.0 / F.1.0.1 交付了 TAAU (render-res 渲染 + output-res TAA 累积上采样), 但纹理采样仍用默认 LOD. 当 renderScale=0.667 (FSR2 Balanced) 时:
- 渲染分辨率小, GPU 选用更大 mipmap level (更模糊的纹理)
- 即使 TAA 时序累积补回了几何/亮度细节, 纹理细节本身已被 mipmap 选择"截断"
- 视觉差异: 远处材质 (砖墙、石头、树皮) 在 0.667 scale 下比 native 明显糊

**业界共识**:
- UE4 / Unity HDRP TAAU: `mipBias = log2(renderScale)`
- AMD FSR2 / FSR3: `mipBias = log2(renderScale) - 1.0`
- NVIDIA DLSS: `mipBias = log2(renderScale) - 1.0` (与 FSR2 一致)
- ChocoLight 选 **`mipBias = log2(renderScale) - 0.7`** — 介于 UE4 与 FSR2 之间, 兼顾锐度和 alias 风险

| renderScale | log2(scale) | bias = log2-0.7 |
|---|---|---|
| 1.0 (native) | 0.0 | 0.0 (无偏移, 等同 F.0) |
| 0.75 (quality) | -0.415 | -1.115 |
| 0.667 (balanced) | -0.585 | -1.285 |
| 0.5 (performance) | -1.0 | -1.7 |

负 bias = 偏向更小 mipmap level (更高频纹理), 负值越大锐度越强但 aliasing/firefly 风险越高 (需 TAA 时序累积稳定).

---

## 2. 设计

### 2.1 GLSL 实现

GLSL `texture(sampler, uv, bias)` 第三参数添加到自动计算的 LOD level. 所有 3D mesh shader 的纹理采样替换为带 bias 的版本:

**老 (Phase F.0)**:
```glsl
vec4 base = texture(uTexBaseColor, vTexCoord);
```

**新 (Phase F.1.1)**:
```glsl
uniform float uMipBias;   // 默认 0.0 (零回归)
vec4 base = texture(uTexBaseColor, vTexCoord, uMipBias);
```

**关键不变量**:
- 默认 `uMipBias = 0.0` → `texture(s, uv, 0.0)` 等同 `texture(s, uv)` (零回归保证)
- TAAU 启用 + autoMipBias=true 时, backend 自动计算并上传非零 bias

### 2.2 受影响的 shader 列表

需改造的 **3D mesh** shader (双 profile GLES3 + GL3.3 = 4 个 shader 源):

| Shader | GLES3 行 | GL3.3 行 | 采样的纹理 |
|---|---|---|---|
| `FS_UNLIT_SOURCE` (PBR Unlit) | 282 | 658 | `uTexBaseColor`, `uTexEmissive` |
| `FS_PBR_SOURCE` (完整 PBR) | 343 | 718 | `uTexBaseColor`, `uTexMR`, `uTexNormal`, `uTexOcclusion`, `uTexEmissive` |

**总计**: 2 shader × 2 profile = 4 个改动点; 约 ~14 处 `texture()` 调用增 bias 参数.

**不纳入** (零回归保障):
- `FS_SOURCE` (2D batch 渲染器, sprite + UI/HUD): UI 文本/图标本身大多不 mipmap, 加 bias 反而会引入 alias
- `FS_LIT2D_SOURCE` (2D 灯光精灵): 与 2D batch 同, 主要服务 sprite, 不参与 jitter
- 后处理 shader (Bloom/SSAO/SSR/TAA/Sharpen 等): 全屏 quad 采样, 不涉及 mipmap LOD 决策

### 2.5 Backend 接口

```cpp
// render_backend.h 新增
class RenderBackend {
    // ...
    /// Phase F.1.1 — 设置 3D mesh shader 纹理采样的 LOD bias.
    /// 默认 0.0 (零回归); TAAU 启用 + autoMipBias=true 时由 TAARenderer 自动调.
    /// @param  bias  通常负值, [-4.0, +4.0]; 越负越锐, 越正越糊.
    virtual void SetMipBias(float bias) {}
    virtual float GetMipBias() const { return 0.0f; }
};
```

```cpp
// render_gl33.cpp impl
class RenderBackendGL33 : public RenderBackend {
    float mipBias_ = 0.0f;

    void SetMipBias(float bias) override {
        mipBias_ = clampf(bias, -4.0f, 4.0f);
    }
    float GetMipBias() const override { return mipBias_; }

    // 在 Draw3D / DrawLit2D / DrawMesh 路径里, glUseProgram 后:
    if (locXxx_MipBias >= 0) glUniform1f(locXxx_MipBias, mipBias_);
};
```

每 shader program 新增 GLint `locXxx_MipBias`, buildProgram 后 `glGetUniformLocation`.

### 2.6 TAARenderer 自动 hook

```cpp
// taa_renderer.cpp 新增
namespace TAARenderer {

bool autoMipBias_ = true;  // 默认 ON: TAAU 启用时自动调 bias

// 内部 hook: 在 SetTAAUEnabled / SetRenderScale 完成 HDR 重建后调
static void updateMipBias_() {
    if (!g.backend || !autoMipBias_) return;

    float bias = 0.0f;   // 默认 / TAAU 关闭 / native scale
    if (g.taauEnabled && g.renderScale > 0.0f && g.renderScale < 1.0f) {
        // FSR2 / DLSS 风格: bias = log2(scale) - 0.7
        //   - log2(0.667) - 0.7 = -1.285
        //   - log2(0.5)   - 0.7 = -1.7
        bias = std::log2(g.renderScale) - 0.7f;
    }
    g.backend->SetMipBias(bias);
}

void SetAutoMipBias(bool flag) {
    autoMipBias_ = flag;
    if (flag) updateMipBias_();   // 立即 sync
    else      g.backend->SetMipBias(0.0f);   // 关 auto -> 复位
}
bool GetAutoMipBias() { return autoMipBias_; }

}
```

`autoMipBias_` 是**单一全局**字段 (不是 per-instance) — 因为 backend `mipBias_` 是单一全局, 只能反映当前 active instance 的 TAAU 状态. F.1.0.1 的 multi-HDR × TAAU 场景下每帧切 active instance, mipBias 也跟着切, 与 SetActiveInstance 配合工作.

### 2.7 Lua API

```lua
-- 高级用户手动设 bias (覆盖 auto)
Light.Graphics.TAA.SetMipBias(-1.5)
Light.Graphics.TAA.GetMipBias()        -- → -1.5

-- 关闭 auto (强制 bias=0 即使 TAAU ON)
Light.Graphics.TAA.SetAutoMipBias(false)
Light.Graphics.TAA.GetAutoMipBias()    -- → false
```

**总计**: 4 新 Lua 函数 (SetMipBias / GetMipBias / SetAutoMipBias / GetAutoMipBias).

### 2.8 SetActiveInstance hook

切 TAA active instance 时, 该 instance 的 taauEnabled / renderScale 可能不同 → 需要重新计算 mipBias:

```cpp
bool SetActiveInstance(int id) {
    // ... 原有逻辑
    g_active = id;
    updateMipBias_();   // ★ F.1.1: 切 active 后重算 bias
    return true;
}
```

### 2.9 默认行为与零回归

| 场景 | 行为 |
|---|---|
| F.0 路径 (taauEnabled=false 整个生命周期) | bias=0, shader `texture(s, uv, 0.0)` 等同 `texture(s, uv)`, 零回归 |
| F.1.0 / F.1.0.1 (无 F.1.1 时 auto) | bias=0, shader 行为不变, 真机可能轻微有 mipmap 选 high level (略糊) |
| F.1.1 + TAAU ON @ balanced | bias≈-1.285, 远处纹理锐 — 配合 TAA 累积稳定 |
| F.1.1 + autoMipBias=false + TAAU ON | bias=0, 与 F.1.0 一样略糊 (用户主动选择) |

---

## 3. 风险与缓解

| 风险 | 缓解 |
|---|---|
| GLSL `texture(s, uv, bias)` 的 GLES3 兼容 | GLES 3.0+ 全支持 (`texture` 重载第 3 个 bias 参数). 若 backend 是 legacy GL 2.x 则不支持 — 但 ChocoLight legacy backend 没有 PBR shader, 不影响 |
| 静态画面 `bias < 0` 引入 alias / shimmer | 由 TAA 时序累积吸收; 若用户关 TAA + 关 jitter, alias 会显现, 是 trade-off. 默认行为 (autoMipBias 仅 TAAU 启用时生效) 已规避此情况 |
| 纹理 LOD bias 与 anisotropic filtering 冲突 | 不冲突, bias 只调 LOD level, anisotropic 仍按 sampler state 工作 |
| 部分硬件对极端 bias < -2 表现差 | clamp [-4, +4]; 实际默认范围 [-1.7, 0] 都很温和, 远未达极限 |
| 多 shader 改动量大, 漏改导致部分纹理不锐 | 严格按受影响列表逐个改, 加 grep 验证 (PR review 时全文搜 `texture(uTex`) |

---

## 4. 任务拆分

| 任务 | 内容 | 估时 |
|---|---|---|
| T1 | Backend 接口: SetMipBias / GetMipBias 虚函数 + render_gl33 实现 (state cache, no shader yet) | 30 min |
| T2 | 改 4 个 3D shader: 增 uMipBias uniform + texture() 加 bias 参 (FS_UNLIT × 2 + FS_PBR × 2) | 1 h |
| T3 | render_gl33 程序绑定: 4 个 GLint loc + buildProgram 后 glGetUniformLocation + Draw 路径 push uniform | 45 min |
| T4 | TAARenderer 自动 hook: SetTAAUEnabled / SetRenderScale / SetActiveInstance 调 updateMipBias_(); SetAutoMipBias / GetAutoMipBias / SetMipBias / GetMipBias 4 新 API | 45 min |
| T5 | Lua bridge 4 函数 + taa_funcs[] 注册 + smoke 4-6 检查点 | 30 min |
| T6 | 构建 + 运行 demo 零回归 + 真机视觉检查 (用户) + 填 ACCEPTANCE/FINAL | 45 min |

**总预计**: 5 小时 (含 shader 改动验证)

---

## 5. 验收门槛

- ✅ smoke 4 新检查点通过 (default / clamp / autoMipBias 切换 / 与 renderScale 联动)
- ✅ demo_ssr / demo_taa_split2 / demo_taau / demo_multi_hdr_pip 零回归
- ✅ 所有 3D shader 编译成功 (CI 6 平台)
- ⏳ 真机视觉: 用户在 demo_taau 切 0.667 / 0.5 scale, 确认远处纹理 vs F.1.0 (无 bias) 锐度提升

---

## 6. Commit 拆分 (建议 PR review)

1. **Backend** (T1 + T3): `render_backend.h` + `render_gl33.cpp` (loc 缓存 + Draw push)
2. **Shaders** (T2): `render_gl33.cpp` 内的 8 个 shader 改 texture() 调用
3. **TAA** (T4): `taa_renderer.cpp` + `taa_renderer.h` 4 新 API + auto hook
4. **Lua + Smoke** (T5): `light_graphics.cpp` + `scripts/smoke/taa.lua`
5. **Docs** (T6): `docs/Phase F.1.1 Mipmap LOD Bias/` 3 件套
