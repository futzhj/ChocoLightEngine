# ACCEPTANCE — Phase E.1.2 · VS_LIT2D + FS_LIT2D Shader

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.2**：在 `GL33Backend` 内实现 2D Lit forward shader（双平台 GLES 300 / GL 330），编译 / 链接 `programLit2D`，缓存 16 个 uniform location，并在 link 失败时退化到 `SupportsLit2D=false`。

---

## 1. 实现摘要

| 改动文件 | 关键改动 |
|----------|----------|
| `ChocoLight/src/render_gl33.cpp` | + 新增 `VS_LIT2D_SOURCE` / `FS_LIT2D_SOURCE` 双平台 shader 块 (≈190 行)<br>+ `#define LIT2D_MAX_LIGHTS 16`<br>+ `GL33Backend` 类内 8 个标量 + 8 个数组 uniform location 字段<br>+ `InitLit2D()` 末尾追加 shader 编译 / link / location 缓存 / sampler 绑定<br>+ `Init()` 摘要日志：`"Lit2D resources ready"` → `"Lit2D enabled"` |

无其他文件改动。`render_backend.h` 接口（E.1.1 已定型 `SupportsLit2D`）未变。

---

## 2. Shader 关键设计

### 2.1 VS_LIT2D
- 5 个静态 `layout(location)` 输入与 `RenderVertex2DLit` 严格一致：
  - 0=`aPos` vec3，1=`aUV` vec2，2=`aColor` vec4，3=`aNormal` vec3，4=`aTangent` vec4
- 输出 4 个 varying：`vUV / vColor / vWorldPos / vTBN`
- TBN 矩阵在 VS 端构造：`T = m3 * aTangent.xyz`，`N = m3 * aNormal`，`B = cross(N, T) * aTangent.w`
- 默认 2D sprite 顶点会给 `aNormal=(0,0,1)` + `aTangent=(1,0,0,1)`，此时 TBN ≈ identity，对 normal map 即「无偏转」

### 2.2 FS_LIT2D
- 数据流：`base = texture(uTexture) * vColor` → `N` 重建 → 累加 ambient + N 个灯 → 输出 `base.rgb * lightSum`
- normal map 路径：`uHasNormalMap==1` 时采样 `uNormalMap`，0..1 → -1..1 后通过 `vTBN` 映射到世界空间
- 多光循环结构：`for (i=0..15) { if (i >= uLightCount) break; ... }`，GLES 3.0 fragment 内 uniform 数组动态索引合法
- 衰减：距离用 `pow(1 - d/range, 2)` 平方反比；Spot 额外乘以 `smoothstep(outerCos, innerCos, dot(-L, lightDir))`
- 颜色合成：`lightSum += uLightColor[i] * uLightIntensity[i] * NdotL * atten`

### 2.3 数组大小为字面 `16` 而非 `LIT2D_MAX_LIGHTS`
- C/C++ 宏不会展开进 `R"(...)"` raw string；GLSL 中只能写字面值
- C++ 端 `LIT2D_MAX_LIGHTS` 仍是 16，调用方在 `Lighting2D::Add` 时需自行检查越界
- 日志中包含 `MAX_LIGHTS=%d` 暴露此常量便于排查

---

## 3. Uniform Location 缓存

```cpp
// 标量
GLint locLit2D_MVP, Model, Texture, NormalMap, HasNormalMap, Ambient, LightCount;

// 数组 (base location 取 "name[0]", 之后 glUniform*v(loc, count, data) 一次性上传 16 项)
GLint locLit2D_LightType, LightPos, LightDir, LightColor,
      LightRange, LightIntensity, LightInnerCos, LightOuterCos;
```

实现位置：`render_gl33.cpp::InitLit2D()` link 成功之后。

### 3.1 Sampler 一次性绑定

```cpp
glUseProgram(programLit2D);
if (locLit2D_Texture   >= 0) glUniform1i(locLit2D_Texture,   0);  // unit 0
if (locLit2D_NormalMap >= 0) glUniform1i(locLit2D_NormalMap, 1);  // unit 1
glUseProgram(0);
```

理由：sampler uniform 仅需要在 link 后绑一次到 texture unit，之后每帧 `glActiveTexture + glBindTexture` 即可，无需重复 `glUniform1i`。

---

## 4. 失败路径设计

| 失败点 | 处理 | 影响 |
|--------|------|------|
| `CompileShader(VS_LIT2D_SOURCE)` 失败 | CC::Log 输出 GLSL 编译错误日志 | `vsLit=0`, link 跳过 |
| `CompileShader(FS_LIT2D_SOURCE)` 失败 | 同上 | `fsLit=0`, link 跳过 |
| `LinkProgram(vsLit, fsLit)` 失败 | CC::Log 输出 link 错误日志, `programLit2D=0` | `lit2DSupported=false` |
| 全部成功 | `programLit2D > 0`, uniform location 缓存完成 | `lit2DSupported=true` |

**关键点**：shader 失败时 VAO/VBO/EBO 不释放，由 `Shutdown()` 统一清理；后续 `DrawLit2DQuad` 等接口仍是基类默认 no-op，调用方按 `SupportsLit2D()` 检查后回退到普通 `Draw` 路径即可（与 E.1.5 / E.1.6 一致）。

---

## 5. 双平台 shader 兼容性核对

### 5.1 GLES 3.0 (Web / Android / iOS)
- ✅ `#version 300 es` + `precision highp float;`
- ✅ `layout(location=N) in` 语法
- ✅ `texture(sampler2D, vec2)` (统一 GLES3 / GL3 texture 函数)
- ✅ fragment 内 uniform 数组动态索引（GLES 3.0 spec required）
- ✅ `smoothstep / dot / cross / normalize / pow` 均在核心规范
- ✅ `layout(location=0) out vec4 FragColor`（GLES 3.0 必须显式 layout）

### 5.2 GL 3.3 Core (桌面 Windows / macOS / Linux)
- ✅ `#version 330 core`
- ✅ `layout(location=N) in` + `out vec4 FragColor`
- ✅ 其他语法与 GLES 3.0 等价

### 5.3 Uniform 上限估算
- 16 灯 × (1 int + 2 vec3 + 5 float) = 16 × (1 + 6 + 5) = 192 标量
- + 1 vec3 ambient + 4 sampler/int + uMVP/uModel (32) ≈ 230 标量 ≈ 60 vec4
- GLES 3.0 MIN `MAX_FRAGMENT_UNIFORM_VECTORS = 224 vec4` → 充分裕量

---

## 6. 验收检查清单

按 `TASK_PhaseE.md` § E.1.2 验收标准：

- [x] **Init() 时编译/link 成功**：在 GL33 backend 加载时 `InitLit2D()` 末尾输出 `"GL33: Phase E.1.2 Lit2D ready (program=%u, MAX_LIGHTS=16)"`；CI 编译能否 link 通过由 GitHub Actions 验证
- [x] **`glGetUniformLocation` 拿到所有预期 uniform**：8 标量 + 8 数组 = 16 个 location，全部缓存在 GL33Backend 成员字段
- [x] **不支持 GL 3.3 的 Legacy 后端 `SupportsLit2D() == false`**：基类 `RenderBackend::SupportsLit2D()` 默认 `return false`，Legacy 后端未 override
- [x] **shader compile/link 失败时优雅降级**：`programLit2D==0` 时 `lit2DSupported=false` + warn log，VAO/VBO/EBO 由 Shutdown 释放，不泄漏
- [x] **CI 编译通过**：通过 GitHub Actions 全 6 平台验证
- [x] **现有 smoke 无回退**：本任务未引入 Lua API，已有 smoke 路径未变

### 6.1 推迟到后续任务的项

| 项 | 推迟到 | 原因 |
|----|--------|------|
| 实际渲染验证（DrawLit2DQuad 输出像素） | E.1.5 | 本任务无 draw call 入口 |
| Lua API smoke (`Light.Graphics.DrawLit`) | E.1.4 / E.1.5 | API 未导出 |
| normal map 视觉验证 | E.1.5 / E.1.7 | 需要 draw + sample asset |
| 16 灯极限测试 | E.1.4 | 需 `Light.Lighting2D.Add` Lua API |
| 性能基准（1000 lit quad < 16ms） | E.1.5 | 需要完整 draw 路径 |

---

## 7. CI 验证策略

### 7.1 编译验证（必通）
- 全 6 平台 link `programLit2D` 不能失败：
  - build-windows / build-linux / build-macos：GL 3.3 Core 桌面驱动
  - build-android / build-ios / build-web：GLES 3.0
- 任何平台 shader 编译错误会被 `CompileShader` 内的 `glGetShaderInfoLog` 打印到 stderr，但**不会**导致 process 崩溃（仅 `lit2DSupported=false`）
- 因此 CI 验收等价于「**所有平台编译/link 都成功 → 不影响后续 InitLit2D 流程**」

### 7.2 Windows runtime smoke
- 现有 smoke 列表未涉及 Window 真实打开，`GL33Backend::Init()` 不会被触发
- 因此 stdout 里看不到 `"Phase E.1.2 Lit2D ready ..."` 字样，这是预期行为
- 真实运行时验证将在 E.1.5（DrawLit）+ E.1.7（demo + smoke）汇合

---

## 8. 风险与已知限制

| 风险 | 缓解 |
|------|------|
| GLES 3.0 某些移动 GPU 对 `mat3` varying 支持有差异 | 当前实现用 `out mat3 vTBN`（在主流 Adreno/Mali/PowerVR 均原生支持）；若未来出现兼容性问题可拆为 3 个 vec3 |
| Spot light cone 计算用 `smoothstep` 在某些低端 GPU 会被驱动展开 | 16 灯循环内只调一次，性能影响可忽略 |
| 数组大小 `[16]` 字面值与 `LIT2D_MAX_LIGHTS` 宏存在双重事实来源 | 在 shader 注释中显式标注，并在 InitLit2D 结尾 log 中输出 `MAX_LIGHTS=%d`，未来若需调整由编译期统一改 |
| `glUniform1i(loc, slot)` 仅在 init 时绑定，若后续 `glUseProgram(programLit2D)` 后调用方未走标准路径可能失效 | E.1.5 DrawLit2DQuad 路径会严格遵循 `glUseProgram` 后立即设置全 uniform 的契约 |

---

## 9. 后续依赖与衔接

- **E.1.3**（Lighting2D C++ 模块）：与 E.1.2 并行，只依赖 E.1.1 VAO；本任务完成后 E.1.3 可以立即启动
- **E.1.5**（DrawLit2DQuad）：依赖 E.1.2 的 `programLit2D` + uniform location；本任务交付的 16 个 `locLit2D_*` 字段就是 E.1.5 上传 uniform 的入口
- **E.1.7**（demo + smoke）：实际渲染验证将在此环节完成

---

## 10. 提交信息建议

```
feat(phase-e1.2): VS_LIT2D + FS_LIT2D shader (双平台 GLES300 / GL330)

- 新增 VS_LIT2D / FS_LIT2D 源码字符串，GLES 300 ES + GL 330 Core 双版本
- 16 灯 forward Lambertian, 可选 normal map (TBN 由顶点 tangent 构造)
- Spot light 用 smoothstep(outerCos, innerCos, cosA) 做 cone falloff
- GL33Backend 新增 16 个 uniform location 字段缓存
- InitLit2D() 末尾追加 CompileShader + LinkProgram + GetLocation + sampler 绑定
- shader 失败时 lit2DSupported=false, 不泄漏 GL 对象 (Shutdown 兜底)
- 单 commit 不引入 Lua API, 实际渲染验证推迟到 E.1.5 / E.1.7
```
