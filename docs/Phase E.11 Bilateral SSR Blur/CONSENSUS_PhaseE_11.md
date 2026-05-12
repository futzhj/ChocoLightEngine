# Phase E.11 Bilateral SSR Blur — CONSENSUS 文档

> **阶段**：6A Workflow — 阶段 1 续 Align（共识锁定）
> **状态**：✅ 用户已拍板（"全启可调 Q1=B + Q2=B"）
> **基线**：Phase E.10 SSR Blur（commit `d64e6b4`，CI 6/6 green，main HEAD）
> **决策时刻**：2026-05-12 15:30+ UTC+08:00

---

## 1. 最终方案规格

### 1.1 用户拍板路线

**"全启可调"** = Q1=B (depthSigma 可调) + Q2=B (bilateral 开关) + Q3=A (smoke 视新 API 自动加检查点)

**核心理念**：最大化用户灵活性，允许：
- 在运行时切换 Gaussian / Bilateral 两种 blur 模式（A/B 对比可见效果）
- 调整 bilateral 深度敏感度（场景特化）
- 默认行为更优（bilateral on, 与"纯升级"目标一致）

### 1.2 SSR Lua API 表面积变化

```
Phase E.10:  24 函数  (lifecycle 5 + autoEnable 2 + params 16 + debug 1)
Phase E.11:  28 函数  (lifecycle 5 + autoEnable 2 + params 20 + debug 1)
            +4 = 2 新对 setter/getter
```

**新增 Lua API 详情**：

| API | 类型 | 范围 | 默认 | 含义 |
|-----|------|------|------|------|
| `SetBilateralEnabled(bool)` / `GetBilateralEnabled() -> bool` | bool | `true`/`false` | **`true`** | blur 走 bilateral / 走 Gaussian (Phase E.10) |
| `SetBlurDepthSigma(float)` / `GetBlurDepthSigma() -> float` | float | `[50, 500]` | **`200`** | bilateral 深度权重灵敏度 |

**API 行为表**：

| `BlurEnabled` | `BilateralEnabled` | 实际行为 |
|---------------|--------------------|----------|
| false | * (任意) | 无 blur（与 Phase E.9 相同） |
| true | false | 走 Phase E.10 纯 Gaussian（向后兼容） |
| true | true | **Phase E.11 bilateral**（默认） |

---

## 2. 关键决策记录

### 2.1 Q1 拍板：depthSigma 参数化（B）

**最终选择**：`SetBlurDepthSigma(float [50, 500])` Lua API

**原因**：
- SSR 反射对深度敏感度高于 SSAO（反射射线可指向远处）
- 不同场景最佳 σ 不同：
  - 近景密集物体：σ=300 (严格门控)
  - 远景开阔场景：σ=100 (宽松, 让远处反射更柔和)
- SSAO 硬编码 200 是历史决策，Phase E.11 取其作为默认值即可

**clamp 边界**：
- 下限 50：再低则 bilateral 失效（接近纯 Gaussian）
- 上限 500：再高则跨边缘 weight 衰减过陡，可能出现噪点

### 2.2 Q2 拍板：bilateral 开关（B）

**最终选择**：`SetBilateralEnabled(bool)` Lua API

**原因**：
- 用户拍板"全启可调"明确希望保留 A/B 对比能力
- demo 中 B 切换 Blur 时再加 V 键切换 Bilateral/Gaussian，方便用户视觉对比
- Phase E.10 → E.11 升级路径有"软降级"开关，降低用户风险

**默认值**：`true`
- bilateral 永远视觉更优
- 新建项目天然得到最佳体验
- 老项目需手动 `SetBilateralEnabled(false)` 才能回退（罕见）

### 2.3 Q3 拍板：smoke 检查点（A，但隐含调整）

**用户选项**虽然是 A，但由于 Q1=B + Q2=B 加了 4 个新 API，smoke surface 必须包含它们。

**实际方案**：smoke 检查点数：49 → 56（+7）
- Surface 检查 +2（新 API 在表中）
- Default value 检查 +2 (`BilateralEnabled=true`, `DepthSigma=200`)
- Round-trip 检查 +2 (`SetBilateralEnabled(false)`, `SetBlurDepthSigma(150)`)
- Clamp 检查 +1 (`SetBlurDepthSigma(0) → 50`, `SetBlurDepthSigma(1000) → 500`)
- Surface count 提示文字更新

**总检查点**：49 (E.10) → 56 (E.11)

---

## 3. 资源与性能预算

### 3.1 内存预算

```
Phase E.10 → E.11 内存预算变化:
  RT 资源:        0 byte (无新增 RT, 复用 Phase E.10 blur ping-pong)
  shader 程序:    +1 program (FS_SSR_BLUR 升级为 bilateral, 单 shader 不双 program)
  uniform cache:  +2 GLint (locSSRBlur_DepthTex, locSSRBlur_Sigma)
  state struct:   +5 byte (1 bool bilateralEnabled + 1 float blurDepthSigma)
  TOTAL: 几乎零增量（< 1 KB shader 程序对象 + 几 byte state）
```

### 3.2 GPU 性能预算

```
@1080p (1920×1080 → half-res 960×540):
  Phase E.10 5-tap Gaussian:
    H pass: 5 tex fetches × 540×540×0.95 ≈ 1.45 M fetches
    V pass: 5 fetches × 同尺寸 = 1.45 M fetches
    TOTAL fetches: ~2.9 M, GPU time ≈ 0.25 ms
  Phase E.11 5-tap Bilateral:
    H pass: 5 + 5 (depth) = 10 fetches × 540×540 ≈ 2.9 M fetches
    V pass: 同上 = 2.9 M fetches
    TOTAL fetches: ~5.8 M, GPU time ≈ 0.35 ms
  额外耗时: ~0.1 ms (+40%, 但绝对值仍 < 0.5 ms)
```

### 3.3 验收性能阈值

- ✅ 高端 PC（GTX 1060+）：< 0.5 ms（含 H+V+composite）
- ✅ 中端 GPU（HD 630 等集显）：< 1.5 ms
- ✅ 移动端（Adreno 640 / Mali G76）：< 3 ms（half-res 优势 + bilateral 仅 +40% tex fetch）

---

## 4. 接口设计共识

### 4.1 backend virtual 签名变化

```cpp
// Phase E.10 (废弃)
virtual void DrawSSRBlur(uint32_t srcTex, uint32_t dstFbo,
                          int dstW, int dstH,
                          int axis, float radius) {}

// Phase E.11 (新)
virtual void DrawSSRBlur(uint32_t srcTex, uint32_t depthTex,
                          uint32_t dstFbo, int dstW, int dstH,
                          int axis, float radius,
                          bool bilateral, float depthSigma) {}
```

**变化点**：
- +1 参数 `depthTex`（Phase E.11 必需，bilateral 用于权重计算）
- +2 参数 `bilateral` + `depthSigma`（运行时分支选择 / 参数化）

**ABI 影响**：
- Legacy backend 继续 no-op
- GL33 backend 升级签名 + shader 升级
- 无外部 backend 实现者

### 4.2 SSRRenderer State 字段

```cpp
struct State {
    // ... Phase E.10 已有字段 ...
    bool    bilateralEnabled = true;    // Phase E.11 默认 true
    float   blurDepthSigma   = 200.0f;  // Phase E.11 默认 200
};
```

### 4.3 Lua bindings 增量

```cpp
// light_graphics.cpp 内
static int l_SSR_SetBilateralEnabled(lua_State* L) { /* ... */ }
static int l_SSR_GetBilateralEnabled(lua_State* L) { /* ... */ }
static int l_SSR_SetBlurDepthSigma(lua_State* L)   { /* ... */ }
static int l_SSR_GetBlurDepthSigma(lua_State* L)   { /* ... */ }

// ssr_funcs[] 注册 +4
```

---

## 5. Shader 设计共识

### 5.1 FS_SSR_BLUR 升级（dual profile）

```glsl
// 共同结构（GLES3 + GL33 两套带 #version 头）

uniform sampler2D uSrcTex;       // slot 0
uniform sampler2D uDepthTex;     // slot 1 (Phase E.11 新)
uniform vec2  uTexel;
uniform int   uAxis;
uniform float uRadius;
uniform int   uBilateral;        // Phase E.11 新, 0 = Gaussian, 非 0 = Bilateral
uniform float uDepthSigma;       // Phase E.11 新

void main() {
    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    vec2 off1 = dir * uRadius;
    vec2 off2 = dir * uRadius * 2.0;

    const float W0 = 0.227027;
    const float W1 = 0.194594;
    const float W2 = 0.121622;

    if (uBilateral == 0) {
        // Phase E.10 路径 (Gaussian, 向后兼容)
        vec4 c = texture(uSrcTex, vUV) * W0;
        c += texture(uSrcTex, vUV + off1) * W1;
        c += texture(uSrcTex, vUV - off1) * W1;
        c += texture(uSrcTex, vUV + off2) * W2;
        c += texture(uSrcTex, vUV - off2) * W2;
        FragColor = c;
    } else {
        // Phase E.11 路径 (Bilateral)
        float cDepth = texture(uDepthTex, vUV).r;
        vec4  sum   = texture(uSrcTex, vUV) * W0;
        float wsum  = W0;
        // ±1
        vec2 uv;  float d, w;
        uv = vUV + off1; d = texture(uDepthTex, uv).r;
        w = W1 * exp(-abs(cDepth - d) * uDepthSigma);
        sum += texture(uSrcTex, uv) * w; wsum += w;
        uv = vUV - off1; d = texture(uDepthTex, uv).r;
        w = W1 * exp(-abs(cDepth - d) * uDepthSigma);
        sum += texture(uSrcTex, uv) * w; wsum += w;
        // ±2
        uv = vUV + off2; d = texture(uDepthTex, uv).r;
        w = W2 * exp(-abs(cDepth - d) * uDepthSigma);
        sum += texture(uSrcTex, uv) * w; wsum += w;
        uv = vUV - off2; d = texture(uDepthTex, uv).r;
        w = W2 * exp(-abs(cDepth - d) * uDepthSigma);
        sum += texture(uSrcTex, uv) * w; wsum += w;

        FragColor = sum / max(wsum, 1e-4);
    }
}
```

**单 shader 双模式**的优势：
- 减少 program 数（无需 SSRBlur + SSRBlurBilateral 两个）
- runtime uniform 切换，零 shader recompile
- shader cache hit 率 100%

**潜在问题**：
- 分支可能影响 GPU 性能（NV 系基本无影响，AMD/移动端可能有惩罚）
- → 用户拍板 Q2=B 时已知此 tradeoff

---

## 6. demo 增强共识

```lua
-- samples/demo_ssr/main.lua 新增按键
V (Phase E.11) : toggle Bilateral on/off  -- 当 BlurEnabled=true 时有效
,/. (Phase E.11) : BlurDepthSigma -/+ 25
```

HUD 增加一行：
```
SSR Blur: ON  Bilateral=ON  radius=2.00  σ=200
```

---

## 7. smoke 检查点共识

新增 section L（在现有 K 之后）：

```
-- ============================================================
-- L) Phase E.11 — Bilateral 与 DepthSigma
-- ============================================================

-- Surface +2
"SetBilateralEnabled", "GetBilateralEnabled",
"SetBlurDepthSigma", "GetBlurDepthSigma",

-- Default
GetBilateralEnabled() == true
math.abs(GetBlurDepthSigma() - 200.0) < 1e-4

-- Round-trip
SetBilateralEnabled(false) -> false
SetBilateralEnabled(true) -> true
SetBlurDepthSigma(150) -> 150

-- Clamp
SetBlurDepthSigma(0) -> 50
SetBlurDepthSigma(1000) -> 500
```

---

## 8. CI 集成共识

`scripts\smoke\ssr.lua` 已挂入 CI workflow（Phase E.10 加入），smoke 升级自动跑。
**workflow 文件无需修改**。

---

## 9. 任务边界限制

### 9.1 必须做（IN）

- [x] backend `DrawSSRBlur` 签名升级（+3 参数）
- [x] FS_SSR_BLUR shader dual profile 升级
- [x] GL33Backend uniform cache +2
- [x] SSRRenderer State +2 字段
- [x] SSRRenderer Process 传递新参数
- [x] 4 个新 Lua API + ssr_funcs[] 注册
- [x] smoke ssr.lua 56/56 检查点
- [x] demo_ssr V/,/. 键 + HUD
- [x] demo_ssr README 更新
- [x] API_REFERENCE 更新
- [x] 完整 6A 文档（7 份）

### 9.2 不做（OUT）

- ❌ G-buffer roughness（Phase E.12+）
- ❌ Temporal accumulation（Phase E.13+）
- ❌ Blur tap count preset 3/5/7（Phase E.10.x 候选）
- ❌ Per-pixel sigma scaling（与 roughness 绑定，Phase E.12+）
- ❌ 改 Phase E.9 raw SSR shader

---

## 10. 验收标准（锁定）

### 10.1 硬指标（必通过）

1. **编译**：Light.dll 0 error / 0 warning（GLES3 + GL33 都 link 成功）
2. **smoke**：ssr.lua 56/56 PASS（local + CI）
3. **CI**：6/6 平台 build + Windows runtime smoke 全过
4. **不回归**：SSAO smoke / 其他 smoke 通过
5. **API**：SSR 表暴露 28 函数（24 → 28）
6. **demo headless**：`demo_ssr ok` exit 0

### 10.2 软指标（期望，无强制）

1. **视觉**：Bilateral on 时反射 leak 视觉改善（有显示器测试时）
2. **性能**：bilateral 额外开销 < 0.1 ms @ 1080p（profiling 时）
3. **文档**：API_REFERENCE Phase E.11 段完整

---

## 11. 接下来流程

```
✅ ALIGNMENT       (本 phase)
✅ CONSENSUS       (本文件)
→  DESIGN          (架构、shader 详细、数据流) — 下一步
   TASK            (~12 原子任务拆分)
   Approve         (用户最后签字，开工)
   Automate        (T1-T4 实施)
   Assess          (ACCEPTANCE/FINAL/TODO + CI 验证)
```

---

## 12. 共识签字

| 拍板项 | 决策 | 签字 |
|--------|------|------|
| Q1 depthSigma 参数化 | ✅ B — Lua API 暴露 | 用户 |
| Q2 bilateral 开关 | ✅ B — `SetBilateralEnabled` | 用户 |
| Q3 smoke 检查点 | ✅ A → 自动增至 56 | 用户隐含 |
| 默认 BilateralEnabled | ✅ true（自动升级） | AI 推断 + 用户认可 |
| 默认 BlurDepthSigma | ✅ 200（与 SSAO 一致） | AI 推断 + 用户认可 |
| sigma 范围 | ✅ [50, 500] | AI 推断 + 用户认可 |
| SSR API 数 | ✅ 24 → 28 | AI 推断 + 用户认可 |
| 工作量预估 | ~1 日（与 Phase E.10 同档） | AI |

---

> **下一步**：写 DESIGN_PhaseE_11.md（架构 + shader 详细 + 数据流 + 接口契约 + 异常策略）。
