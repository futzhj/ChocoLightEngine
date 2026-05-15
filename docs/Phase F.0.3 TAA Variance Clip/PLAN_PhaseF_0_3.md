# Phase F.0.3 TAA Variance Clipping — PLAN (Align + Architect + Atomize)

> 6A 工作流 · 阶段 1+2+3 合并文档
> 基线：Phase F.0 (`bc82376`) + F.0.1 (`011a549`) + F.0.4 (`361a56f`) + F.0.2 (`919d44f` + docs `c36e7b2`)
> 算法参考：Marco Salvi "An Excursion in Temporal Supersampling", GDC 2016；UE5 default temporal AA

---

## 1. Align — 任务定义与边界

### 1.1 原始需求

- 在 Phase F.0.2 YCoCg AABB clip 基础上扩展第三种 clip 模式 — **Variance clipping**
- 算法：基于 9 邻域**一阶矩 (mean)** 与 **二阶矩 (mean of squared)** 计算标准差，clip 范围 = `mean ± gamma·sigma`
- 暴露 Lua API 控制 variance gamma 系数（典型范围 [0, 4]，默认 1.0）
- 复用 F.0.2 已有的 `SetClipMode(string)` 接口 — 添加第三个合法值 `"variance"`

### 1.2 边界确认（明确范围）

**包含**：
- shader 内 variance clipping 路径（GLES3 + GL33 双版本对称）
- variance 在 **YCoCg** 空间执行（Salvi 推荐，UE5 默认 — 复用 F.0.2 的 RGBToYCoCg/YCoCgToRGB 函数）
- 第 3 个 ClipMode 值 `"variance"` 接入 `Light.Graphics.TAA.SetClipMode`
- 新增 `Set/GetVarianceGamma` Lua API（gamma 系数 [0, 4] clamp）
- backend 接口扩展（`DrawTAAPass` 加 `float varianceGamma = 1.0f` 默认参数）
- TAARenderer state + Process 透传 + clamp
- smoke 测试覆盖默认值 + clamp + round-trip + invalid + 与 F.0.1/F.0.2/F.0.4 共存
- demo HUD 加 `vg=1.0` 字段
- API 文档详细描述（数学定义 + Salvi 引用 + UE5 验证）
- 6A 文档三件套（PLAN/ACCEPTANCE/FINAL/TODO）

**不包含**：
- variance clipping 在 RGB 空间的备选路径（与 Salvi 论文一致只在 YCoCg）
- Half-res TAA history（保留给 F.0.5）
- Split-screen A/B demo（保留给 F.0.7）
- chroma rotation 或 OBB clip（更激进的 clip 几何，远期）

### 1.3 项目上下文（基于已有代码 + Phase F.0.2 实现）

- `render_gl33.cpp::FS_TAA_SOURCE` (GLES3) 和第二份 GL33 版均已有 `uClipMode` 嵌套 `if` 分支结构，扩展为 3 路径自然
- `RGBToYCoCg` / `YCoCgToRGB` 函数已在 shader 内可复用
- `GL33Backend::DrawTAAPass` 已有 `int clipMode` 参数与 `glUniform1i(locTAA_ClipMode, clipMode)` 上传逻辑
- `taa_renderer.cpp::parseClipMode_` 已是 case-insensitive lambda 风格，扩展第 3 个 `"variance"` 值自然
- Lua API 风格已有 `SetClipMode/GetClipMode` (string enum) + `SetSharpness/GetSharpness` (float clamp)，新 API 沿用后者

### 1.4 智能决策清单

| # | 决策点 | 选项 | 选择 | 理由 |
|---|--------|------|------|------|
| D1 | 算法 | (a) AABB+chroma rotation (b) variance YCoCg (c) variance RGB | **(b) variance YCoCg** | Salvi 2016 + UE5 主流；YCoCg 域提供更紧凑的 clip 范围 |
| D2 | gamma 默认值 | 0.75 / 1.0 / 1.25 / 1.5 | **1.0** | Salvi 论文推荐；UE5 默认 1.0 |
| D3 | gamma 范围 | [0, 2] / [0, 4] / [0, 8] | **[0, 4]** | UE5 实测范围；> 4 实际等效无 clip |
| D4 | clip mode 默认 | 保持 "ycocg" / 改为 "variance" | **保持 "ycocg"** | F.0.2 默认值, 不引入回归; 用户主动 SetClipMode("variance") 切换 |
| D5 | API 命名 | `SetVarianceGamma` / `SetVarianceN` / `SetVarianceClipFactor` | **`SetVarianceGamma`** | 与 paper 术语一致 (γ 系数) |
| D6 | gamma 越界处理 | reject / clamp | **clamp** | 与 `SetSharpness` 一致 (silent clamp) |
| D7 | Lua API 数量 | 19 → 21 | **+2 fn** | SetVarianceGamma + GetVarianceGamma |

所有决策均基于业界标准 + Phase F.0 一致性，零用户拍板需求。

### 1.5 验收标准

- [ ] shader 双源支持 `uClipMode == 2` variance 路径
- [ ] `SetClipMode("variance")` 正常切换；GetClipMode 返回 "variance"
- [ ] `SetVarianceGamma(value)` clamp 到 [0, 4]
- [ ] gamma=0 等效极端激进（mn=mx=mean）；gamma>=2 接近无 clip
- [ ] CI 6/6 平台 success；Windows runtime smoke ALL TESTS PASSED with 21/21 functions covered
- [ ] F.0.1 sharpness + F.0.2 ycocg + F.0.3 variance + F.0.4 antiFlicker 四启共存

---

## 2. Architect — 系统设计

### 2.1 整体架构

```
                Phase F.0.3 — Variance Clipping (在 F.0.2 嵌套 if 分支基础上扩展第 3 路径)

   uClipMode == 0   ──→   F.0   RGB AABB clip (字面照搬)
   uClipMode == 1   ──→   F.0.2 YCoCg AABB clip (mean of min/max)
   uClipMode == 2   ──→   F.0.3 YCoCg variance clip (mean ± gamma·sigma)  ◀── 新增
                              │
                              ├── 新增 uniform float uVarianceGamma (默认 1.0, [0,4])
                              ├── 9 个采样点 RGBToYCoCg → 同时累加 m1 = sum/9 + m2 = sumSq/9
                              ├── sigma = sqrt(max(0, m2 - m1*m1))
                              ├── mn = m1 - gamma·sigma; mx = m1 + gamma·sigma
                              └── clamp(histYCoCg, mn, mx) → YCoCgToRGB
```

### 2.2 数学定义（Salvi 2016 / UE5）

```
对 9-tap 邻域 N = {center + 8 neighbors}:
  m1   = (1/|N|) · Σ_{i ∈ N} c_i                   // 一阶矩 (mean)
  m2   = (1/|N|) · Σ_{i ∈ N} c_i ⊙ c_i             // 二阶矩 (mean of squared, 逐通道)
  σ²   = m2 - m1 ⊙ m1                              // 方差 (Welford / König-Huygens 公式)
  σ    = sqrt(max(σ², 0))                          // 防浮点负数
  γ    = uVarianceGamma                            // 收紧系数, 典型 [0.75, 1.5]
  mn   = m1 - γ·σ
  mx   = m1 + γ·σ
  hist = clamp(hist, mn, mx)
```

### 2.3 GLSL 实现（GLES3 + GL33 双源对称）

```glsl
} else if (uClipMode == 2) {
    // Phase F.0.3 — Variance clipping in YCoCg space (Salvi 2016 / UE5)
    // m1 = mean(YCoCg(N9)); m2 = mean(YCoCg(N9)^2); sigma = sqrt(m2 - m1^2)
    vec3 sum = vec3(0.0), sumSq = vec3(0.0);
    vec3 s;
    s = RGBToYCoCg(cur.rgb);                                                     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2(-1.0,-1.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2( 0.0,-1.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2( 1.0,-1.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2(-1.0, 0.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2( 1.0, 0.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2(-1.0, 1.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2( 0.0, 1.0)).rgb);     sum += s; sumSq += s*s;
    s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2( 1.0, 1.0)).rgb);     sum += s; sumSq += s*s;
    vec3 m1    = sum   * (1.0 / 9.0);
    vec3 m2    = sumSq * (1.0 / 9.0);
    vec3 sigma = sqrt(max(m2 - m1 * m1, vec3(0.0)));
    vec3 mn    = m1 - uVarianceGamma * sigma;
    vec3 mx    = m1 + uVarianceGamma * sigma;
    vec3 histY = clamp(RGBToYCoCg(hist.rgb), mn, mx);
    hist.rgb   = YCoCgToRGB(histY);
}
```

### 2.4 性能估算 @ 1080p

| 模式 | clip 段 ALU 估算 | 总 TAA 增量 |
|------|------------------|-------------|
| F.0 RGB AABB (`uClipMode=0`) | 9 fetch + 9 min/max | 0.10 ms baseline |
| F.0.2 YCoCg AABB (`uClipMode=1`) | + 9 mat3 mul + min/max | +0.05 ms |
| **F.0.3 YCoCg variance (`uClipMode=2`)** | **+ 9 mat3 mul + 9 sq + sum + sqrt + 2 mul/sub** | **+0.07 ms** |

variance 比 AABB 多 9 个 dot product (`s*s`) + 1 sqrt + 几个 mul/sub。估算开销 < 1080p @ 60fps 帧预算的 0.5%。

### 2.5 接口契约

#### Backend 接口扩展

```cpp
// render_backend.h
virtual void DrawTAAPass(...,
                         int   /*clipMode*/      = 1,    // F.0.2: 0=RGB, 1=YCoCg, 2=variance
                         float /*varianceGamma*/ = 1.0f) // F.0.3: variance clip 收紧系数 [0, 4]
{}
```

#### TAARenderer state + API

```cpp
class TAARenderer {
private:
    int   clipMode = 1;          // F.0.2 默认 YCoCg
    float varianceGamma = 1.0f;  // F.0.3 默认 Salvi 推荐

public:
    bool        SetClipMode(const char* mode);           // 接受 "rgb"/"ycocg"/"variance" (case-insensitive)
    const char* GetClipMode() const;
    void  SetVarianceGamma(float gamma);                 // clamp [0, 4]
    float GetVarianceGamma() const;
};
```

#### Lua API（+ 2 fn → 21 fn 总）

```lua
Light.Graphics.TAA.SetClipMode("variance")               -- F.0.2 第 3 个值
Light.Graphics.TAA.SetVarianceGamma(1.0)                 -- F.0.3 新增, 默认 1.0
Light.Graphics.TAA.GetVarianceGamma() → number           -- F.0.3 新增
```

### 2.6 异常处理

| 输入 | 行为 |
|------|------|
| `SetClipMode("variance")` | 切到 variance 模式 |
| `SetClipMode("Variance")` / `"VARIANCE"` | 大小写不敏感, 等效 |
| `SetVarianceGamma(2.5)` | 接受, 写入 |
| `SetVarianceGamma(-1)` / `5` | clamp 到 [0, 4] |
| `SetVarianceGamma("foo")` | 类型错, 返 nil + err |
| `SetVarianceGamma(nil)` | 类型错, 返 nil + err |

---

## 3. Atomize — 任务原子化

### T0 PLAN 文档（本文件）✅

### T1 Shader (GLES3 + GL33 双源)

输入：`render_gl33.cpp::FS_TAA_SOURCE`（GLES3 行 2096-2216 + GL33 行 2769-2884）

输出：
- 每份加 `uniform float uVarianceGamma;` 声明（uClipMode 同行附近）
- 每份在 `uClipMode == 1` else 之前插入 `else if (uClipMode == 2) { variance 路径 }`
- 保留原 `uClipMode == 1` (YCoCg AABB) 与 else (RGB AABB) 两路径

### T2 Backend Interface + Implementation

输入：`render_backend.h` + `render_gl33.cpp::GL33Backend`

输出：
- `render_backend.h::DrawTAAPass` 加 `float varianceGamma = 1.0f` 默认参数
- `GL33Backend::locTAA_VarianceGamma` GLint 字段 (-1)
- Init 内 `glGetUniformLocation(programTAA, "uVarianceGamma")`
- DrawTAAPass impl 加 `float varianceGamma` 参数 + `glUniform1f(locTAA_VarianceGamma, varianceGamma)`
- Shutdown reset locTAA_VarianceGamma = -1

### T3 TAARenderer State + API

输入：`taa_renderer.h` + `taa_renderer.cpp`

输出：
- `taa_renderer.h` 加 SetVarianceGamma / GetVarianceGamma 声明 + Phase F.0.3 注释
- `taa_renderer.cpp` state 加 `float varianceGamma = 1.0f`
- Process 内 DrawTAAPass 调用末尾追加 `g.varianceGamma`
- `parseClipMode_` 加 `"variance"` 第 3 个识别项（lower-case 比对）
- SetVarianceGamma 实现：clamp [0, 4]
- GetVarianceGamma 实现：直接返回

### T4 Lua API + smoke + demo + docs

输入：`light_graphics.cpp` + `taa.lua` + `demo_ssr/main.lua` + `Light_Graphics.md`

输出：
- l_TAA_SetVarianceGamma：luaL_checknumber + clamp，return 1
- l_TAA_GetVarianceGamma：lua_pushnumber，return 1
- taa_funcs[] +2 fn → 21 fn
- taa.lua 加 surface check 21 fn + variance round-trip + gamma 默认 / clamp / type-error / 四启共存 (~10 PASS)
- demo_ssr/main.lua HUD 加 `vg=%.2f`
- Light_Graphics.md 速查表 19 → 21 行 + Set/GetVarianceGamma 完整文档段（Salvi 公式 + UE5 验证 + γ 推荐 [0.75, 1.5]）

### T5 6A 文档三件套

输入：本 PLAN

输出：
- ACCEPTANCE_PhaseF_0_3.md
- FINAL_PhaseF_0_3.md
- TODO_PhaseF_0_3.md

### T6 Commit + Push + CI

输入：完整改动

输出：
- commit 主代码
- push origin main
- 监控 GitHub Actions CI 6/6 success
- 回填 ACCEPTANCE / FINAL / TODO

---

## 4. 风险分析

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| variance σ² 浮点负数 (m2 < m1²) | 中 | shader NaN | `max(0)` 保护 |
| gamma=0 极端激进导致 history 完全黑 | 低 | 测试时单帧黑屏 | 文档警告；clamp[0,4] |
| sqrt(0) 在某些 GPU 慢 | 低 | 5% 性能波动 | 业界普遍接受 |
| 与 F.0.1 sharpening / F.0.4 antiFlicker 互相影响 | 低 | 视觉退化 | 各 phase 作用 pipeline 不同阶段，独立 |
| shader 长度超过驱动限制 | 极低 | 编译失败 | GLES 标准支持 ≥ 1024 行 fragment shader |

---

## 5. 验证策略

### 5.1 静态验证

- `lightc -p taa.lua` Lua 语法 + chunk 编译验证
- `lightc -p demo_ssr/main.lua` Lua 语法验证
- C++ 编译触发 CI 全平台

### 5.2 运行时验证（CI Windows runtime smoke）

- 21 fn surface
- ClipMode round-trip "variance"
- VarianceGamma 默认 1.0
- VarianceGamma clamp [0, 4] (-1 → 0, 5 → 4)
- VarianceGamma type-error (string/boolean/nil)
- F.0.1 sharp=0.5 + F.0.2 clipMode='variance' + F.0.3 vg=1.5 + F.0.4 antiFlicker=true 四启共存

### 5.3 视觉验证（可选, demo_ssr）

- 用户手动按键切换：当前 demo 的 G 键已绑 antiFlicker；clipMode 不绑新键，靠 HUD 直接读出
- 用户手动调用 `Light.Graphics.TAA.SetClipMode("variance")` 切换
