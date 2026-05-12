# ALIGNMENT — Phase E.8.x · G-buffer normal RT 升级 SSAO

> 6A 工作流 · 阶段 1 · Align（对齐）
> 目标：模糊需求 → 精确规范

---

## 1. 项目上下文分析

### 1.1 现状（Phase E.8 完成基线）

**HDR FBO 结构**（`@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:2713-2764`）：
- 1 个 color attachment：`RGBA16F` 纹理（GL_LINEAR + CLAMP_TO_EDGE）
- 1 个 depth attachment：`GL_DEPTH_COMPONENT24` renderbuffer（不是 texture！）
- 通过 `hdrFboDepthRB` map 维护 fbo→depthRB 关联

**SSAO normal 重建路径**（`@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:1432-1433`）：
```glsl
vec3 P = ReconstructViewPos(vUV, d);
vec3 N = normalize(cross(dFdy(P), dFdx(P)));   // 屏幕空间 ddx/ddy 重建面法
```
**问题**：
- ddx/ddy 在物体边缘 / 极端视角不可靠（屏幕空间 4×4 quad 单元，物体边界处法线翻折）
- 多边形之间的 normal 突变会被屏幕空间填充错误
- 对比 G-buffer 真实法线：可视化对比下边缘条纹噪声明显（HBAO 论文已论证）

**现有写 HDR FBO 的 shader 清单**（attachment 0）：

| Shader | 用途 | 当前 fragment out | 是否有 view-space normal |
|--------|------|------------------|------------------------|
| `FS_SOURCE` | 2D batch（Draw / DrawQuad）| `vec4 FragColor` | ❌（无 normal 输入）|
| `FS_LIT2D_SOURCE` | 2D Lit（Lighting2D）| `vec4 FragColor` | ✅（normal map 扰动后）|
| `FS_UNLIT_SOURCE` | 3D Unlit（mesh）| `vec4 FragColor` | ❌（vs 不传）|
| `FS_PBR_SOURCE` | 3D PBR（mesh）| `vec4 FragColor` | ✅（vs 传 + view 变换）|
| Skin 变体（×N）| GPU skinning | 同上 | 同上 |

### 1.2 技术栈与约束

- **GL3.3 Core / GLES 3.0+**：原生支持 MRT（`GL_MAX_DRAW_BUFFERS >= 4`）
- **WebGL 2 = GLES 3.0**：原生支持
- **Legacy backend**：no-op，本 phase 不涉及
- **Shader 双 profile**：GLES3 (`#version 300 es`) + GL3.3 (`#version 330 core`) 都需要更新

---

## 2. 原始需求

> 升级 SSAO 法线源：HDR FBO 加 MRT (RGBA16F + RG16F view-normal)，去掉 ddx/ddy 重建噪声。质量↑显著。工程量~1 天

---

## 3. 边界确认（任务范围）

### 3.1 范围内（必做）

1. **HDR FBO 升级 MRT**：第 2 个 color attachment 存 view-space normal（RG16F，z 重建）
2. **所有写 HDR 的 shader 升级**：增加 `out vec2 FragNormal`
3. **SSAO shader 升级**：去掉 `dFdx/dFdy`，改读 `uNormalTex`
4. **render_backend.h 增 1-2 个虚接口**：可能需要 `GetHDRNormalTex(fbo)` 等
5. **smoke + demo + CI**：保持原有，验证视觉无回归

### 3.2 范围外（不做）

1. ❌ 不修改 SSAO 算法本身（仍是 16 Hammersley kernel + bilateral blur）
2. ❌ 不引入 G-buffer 的其他 channel（如 albedo、roughness 等，仅 normal）
3. ❌ 不改变 HDR depth 路径（仍是 renderbuffer，不是 texture）
4. ❌ 不影响 Phase E.3-E.7 的现有 shader（Bloom / AE / LensDirt / Streak / LensFlare 都是 post-process，读 HDR color tex，不写 attachment 1）
5. ❌ 不暴露 normal RT 给 Lua（保留私有；未来 SSR/SSGI 再考虑）

### 3.3 兼容性边界

- ✅ Phase E.8.x 后所有 Phase E demo 视觉无回归（attachment 1 内容不影响 attachment 0 的渲染）
- ✅ 用户 Lua API 100% 不变（`Light.Graphics.SSAO.*` 19 fn 全部保留）
- ✅ 默认参数不变（radius=0.5 / bias=0.025 / intensity=1.0 ...）

---

## 4. 需求理解（对现有项目的理解）

### 4.1 Multiple Render Target (MRT) 在 GL3.3 / GLES3.0 的标准做法

```glsl
// Fragment shader
out vec4 FragColor;     // attachment 0
out vec2 FragNormal;    // attachment 1

void main() {
    FragColor  = vec4(albedo, 1.0);
    FragNormal = encodeViewNormal(viewN);   // viewN.xy（z 用 sqrt(1-x²-y²) 重建）
}

// Vertex shader (PBR / Lit2D 已有 normal)
out vec3 vNormalView;
void main() {
    vNormalView = normalize(mat3(view * model) * aNormal);
    ...
}
```

```cpp
// CPU 端 FBO 创建（多 attachment）
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrTex,    0);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normalTex, 0);
GLenum drawBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
glDrawBuffers(2, drawBufs);
```

### 4.2 Normal 编码方案对比

| 方案 | 内存 | 精度 | 复杂度 | 推荐 |
|------|------|------|--------|------|
| **RG16F**（仅 xy + sqrt z 重建）| 4 字节/像素 | 高 | 简单 | ⭐ 推荐 |
| RGB10A2 | 4 字节/像素 | 中 | 中 | ❌ |
| RGB8 | 3 字节/像素 | 低 | 简单 | ❌ |
| Octahedral 16-bit | 4 字节 | 高 | 复杂 | 短期 ❌ |

> **决策**：RG16F + xy 直接存 + sqrt z 重建（z 总是 ≥ 0 即背向相机）。@1080p：4 MB / 单 frame，可接受。

### 4.3 2D shader 怎么写 normal？

**问题**：2D batch / Lit2D 在屏幕空间，没有真实 view-space normal。

**选项**：

- **A. 写 `(0, 0)` → encode 表示 N=(0, 0, 1)（朝向相机）**：所有 shader 都写，逻辑统一。SSAO 在 2D 场景输出全 1（按 Phase E.8 设计），不影响视觉。
- **B. `glDrawBuffers(1, COLOR_0)` 在 2D shader 调用前切换**：复杂，每次切换 batch 都要重置。
- **C. 让 SSAO 在 2D 模式下自动 disable**：检测当前 projection 是否正交。

> **决策**：选项 A — 简单统一，所有 shader 默认写 `(0, 0)`，SSAO 拿到的 normal=(0, 0, 1) 在 ddx/ddy 缺失时也能算 AO（虽然结果可能怪，但用户已确认 SSAO 仅 3D 适用）。

### 4.4 CreateHDRFBO 接口升级方案

**A. 默认 MRT（推荐）**：所有 HDR FBO 创建即带 normal attachment
- 优点：简单，所有 shader 路径统一
- 缺点：未启用 SSAO 时浪费 4MB normal RT 内存（@1080p）

**B. 显式 MRT 切换**：新增 `CreateHDRFBOWithNormal(w, h, &outColor, &outNormal)`
- 优点：未启用 SSAO 时省内存
- 缺点：需要 shader 双变体（写 normal vs 不写），运行时 link 切换

**C. SSAO Enable 时切换**：HDR FBO 默认单 RT，SSAO.Enable 时重建为 MRT
- 优点：完全向后兼容
- 缺点：HDR RT 重建 = 销毁 + 重建，所有 sceneTex 引用失效，需要重新 RegisterCanvas

> **决策建议**：**选项 A（默认 MRT）**，最简单清晰。4MB @1080p 是可接受代价，未来 SSR / SSGI 也会用同 normal RT。

### 4.5 Backend 虚接口最小改动

**新增（1 个）**：
```cpp
virtual uint32_t GetHDRNormalTex(uint32_t fbo) const { return 0; }   // 返回关联的 normal tex id
```

**修改（1 个）**：
```cpp
virtual uint32_t CreateHDRFBO(int w, int h, uint32_t* outColorTex, uint32_t* outNormalTex = nullptr);
//                                                                        ^^^^^^^^^^^^^^^^^^^^^^^^^^
//                                                                        新增可选参数; 旧调用兼容 (nullptr)
```

**SSAO 内部**：`Process(fbo, hdrTex)` 内部用 `backend->GetHDRNormalTex(fbo)` 拿 normal，传给 shader。

### 4.6 SSAO shader 改动

**FS_SSAO**：
- 加 `uniform sampler2D uNormalTex;`
- `vec3 N = normalize(DecodeNormal(texture(uNormalTex, vUV).rg));` 替代 ddx/ddy
- 解码：`vec3 N = vec3(rg, sqrt(1 - dot(rg, rg)));`
- 注意：unsupported normal（RG=0）→ N=(0,0,1)，AO 算出来不可靠；天空像素仍走 `d>=0.9999` 分支

---

## 5. 疑问澄清（关键决策点）

### Q1: MRT 启用模式（最关键）

| 选项 | 描述 | 内存 | 工程量 | 推荐 |
|------|------|------|--------|------|
| **A. 默认 MRT** | HDR FBO 永远 MRT，所有 shader 写 normal | +4MB @1080p | ★★ 中 | ⭐ |
| B. 增量 MRT | SSAO Enable 时重建 HDR FBO 为 MRT；shader 双变体 | 按需 | ★★★★ 大 | ❌ |
| C. SSAO 内部双 RT | HDR 单 RT 不变，SSAO 自管 normal RT，3D shader 二次 pass 写入 | +4MB | ★★★ 大 | ❌ |

> **我倾向 A**：4MB 代价小，简化逻辑，未来扩展（SSR）一致。

### Q2: 2D shader 是否输出真实 normal map 扰动？

- **A. 2D 一律写 (0, 0)**：简单，SSAO 在 2D 无效（用户已确认）
- **B. Lit2D 输出 normal map 扰动后的 view-N**：复杂；Lit2D 只在 +Z 平面，view-N 计算需要假设相机方向

> **我倾向 A**：与 SSAO 仅 3D 适用一致。

### Q3: 是否同步升级现有 demo？

- **A. demo_ssao 不变 + README 加说明**：节省工程量
- **B. demo_ssao 加 normal 可视化模式（按 N 键切换显示 normal RT）**：方便视觉验收

> **我倾向 B**：可视化模式 ~10 行代码，对验收价值高。

### Q4: 是否需要新 Lua API？

- **A. 完全不暴露 normal RT**：保持 SSAO API 19 fn 不变
- **B. 加 `Light.Graphics.SSAO.GetNormalTexId()` debug 接口**：方便 demo 可视化

> **我倾向 B**：1 行 API + 1 个 binding，与 LensFlare.GetFlareTextureId 风格一致。

### Q5: 平台兼容性回退策略？

- **A. MRT 创建失败 → SSAO Disable 静默回退**：保持 ddx/ddy 路径作为 fallback
- **B. MRT 创建失败 → HDR 整体 disable**：保守

> **我倾向 A**：保留 ddx/ddy fallback shader 变体，若 MRT 不可用则用旧路径。但这意味着 shader 需要双变体（uniform 控制 ddx/ddy vs uNormalTex），增加复杂度。

> **简化版 A'**：MRT 创建失败 → SSAO `supported=false`，HDR 仍单 RT 工作。其他模块（Bloom / AE / Streak / LensFlare）不受影响。

> **我倾向 A'**：复杂度最低；现代 GL3.3+ / GLES3+ 都原生支持 MRT，几乎不会失败。

### Q6: 是否触发既有 demo CI 回归测试？

所有 Phase E demo 共享 HDR FBO，attachment 0 内容完全不变，理论上 100% 兼容。但：

- **A. 仅 ssao.lua + demo_ssao 验证**：信任 attachment 隔离
- **B. CI 每个 Phase E demo 都要 headless smoke 验证**：保险

> **我倾向 A**：现有 CI 已覆盖 Bloom / AE / Streak / LensFlare smoke，attachment 0 内容由 shader 直接控制，attachment 1 写入完全独立。若有问题 CI 会暴露。

---

## 6. 决策汇总（用户已确认 2026-05-12）

| Q# | 决策 | 选择 |
|----|------|------|
| Q1 | MRT 启用模式 | **A. 默认 MRT + 失败静默 disable** ✅ 用户选定 |
| Q2 | 2D shader normal | **A. 写 (0, 0) → +Z**（推荐采纳）|
| Q3 | demo 升级 | **B. 加 normal 可视化（N 键 toggle）**（推荐采纳）|
| Q4 | 新 Lua API | **B. 加 `GetNormalTexId()` debug**（推荐采纳）|
| Q5 | 兼容性回退 | 已含在 Q1=A 中（MRT 失败 → SSAO supported=false 静默）|
| Q6 | CI 回归 | **A. 仅 ssao smoke + demo**（推荐采纳）|

---

## 7. 验收标准（草拟）

### 7.1 功能验收

- ✅ HDR FBO 创建后第 2 个 attachment 是 RG16F normal tex
- ✅ 所有写 HDR 的 shader 输出 (FragColor + FragNormal)
- ✅ SSAO shader 正确读取 normal tex（ddx/ddy 路径删除）
- ✅ demo_ssao 视觉对比：边缘条纹噪声消失，AO 更自然
- ✅ Phase E 所有现有 smoke 仍通过

### 7.2 CI 验收

- ✅ build-windows / linux / macos / ios / android / web 全 6/6 绿
- ✅ ssao smoke 50+ 断言全过
- ✅ Phase E.3-E.7 smoke 无回归

### 7.3 性能验收（非阻塞）

- 内存：HDR RT 多 4 MB @1080p（+24% RGBA16F→双 attachment）
- GPU：所有 shader 多 1 个 attachment 写入，~0.05 ms 增量
- SSAO 自身：节省 ddx/ddy 计算（~0.02 ms）
- 净影响：可忽略

---

## 8. 项目特性规范对齐

- ✅ **风格**：与 Phase E.3-E.8 一致：6A 工作流 / 4 子阶段拆分 / smoke ~50 断言 / demo + README / CI 注册
- ✅ **代码规范**：注释、命名、reverse 清理（与 SSAORenderer 一致）
- ✅ **API 兼容**：Lua `Light.Graphics.SSAO.*` 19 fn 全部保留
- ✅ **文档**：ALIGNMENT / DESIGN / TASK / ACCEPTANCE / FINAL / TODO 6 份
- ✅ **质量门控**：6A 阶段 4 Approve 人工审查后再实施

---

**等待用户对 Q1-Q6 的最终确认**。我已给出每个问题的推荐答案，无中断决策时全部采用推荐路径。
