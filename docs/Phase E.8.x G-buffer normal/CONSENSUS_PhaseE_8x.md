# CONSENSUS — Phase E.8.x · G-buffer normal RT 升级 SSAO

> 6A 工作流 · 阶段 1 · Align 输出
> 基于 ALIGNMENT 用户决策（2026-05-12 Q1=A 等）形成最终共识。

---

## 1. 明确需求

### 1.1 一句话概括

**升级 HDR FBO 为 MRT（color + view-space normal），让 SSAO shader 直接读真实 G-buffer normal，去掉 ddx/ddy 重建噪声。**

### 1.2 用户视觉预期

- ✅ SSAO 边缘条纹噪声消失
- ✅ 物体角落 / 倾斜面 AO 更自然
- ✅ 极端视角（>60°）AO 不再翻折错误
- ⚠️ 整体亮度无变化（attachment 0 不变）
- ⚠️ Lua API 100% 不变（`Light.Graphics.SSAO.*` 19 fn 全保留 + 新加 1 个 debug）

---

## 2. 技术实现方案

### 2.1 HDR FBO MRT 方案（用户选 A）

```cpp
// CreateHDRFBO 升级
GLuint hdrTex      = ...;  // RGBA16F (现有)
GLuint normalTex   = ...;  // RG16F (新增) ★
GLuint depthRB     = ...;  // GL_DEPTH_COMPONENT24 (不变)

glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrTex,    0);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normalTex, 0);  // ★
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
glDrawBuffers(2, bufs);  // ★ 启用 MRT 写入

// 检查 GL_FRAMEBUFFER_COMPLETE; 失败 → CreateHDRFBO 返回 0
//   → SupportsHDR() 仍可能返回 true (单 RT path), 但 SupportsSSAO 必须 = false
```

### 2.2 Shader 升级清单

| Shader | 改动 | 影响 |
|--------|------|------|
| `FS_SOURCE` (2D batch) | + `out vec2 FragNormal = vec2(0, 0)` | 微 |
| `FS_LIT2D_SOURCE` (Lit2D) | + `out vec2 FragNormal = vec2(0, 0)` | 微（暂不写真 normal）|
| `FS_UNLIT_SOURCE` (3D) | + `out vec2 FragNormal = encode(viewN)` + VS 传 view-N | 中 |
| `FS_PBR_SOURCE` (3D PBR) | + `out vec2 FragNormal = encode(viewN)` + VS 传 view-N | 中 |
| 4 个 Skin 变体 | 同 unlit/pbr | 中 |
| `FS_SSAO_SOURCE` | 删 `dFdx/dFdy` 重建；改读 `uNormalTex` | 中 |
| Bloom / AE / LensDirt / Streak / LensFlare / Tonemap / SSAOBlur / SSAOComposite | 不变（不写 HDR FBO 的 attachment 0/1）| 0 |

### 2.3 Normal 编码 / 解码

```glsl
// Vertex shader
out vec3 vNormalView;
void main() {
    // model -> view space normal (3x3 inverse transpose; 假设 view * model 是仿射 = 用 mat3 + normalize)
    vNormalView = normalize(mat3(uView * uModel) * aNormal);
    ...
}

// Fragment shader (写 attachment 1)
out vec2 FragNormal;
void main() {
    vec3 N = normalize(vNormalView);
    // 简化: view-space N 中 z 一般 ≥ 0 (背向相机), 仅存 xy + sqrt 重建
    // 对极端情况 (z < 0): 视为退化, 让 SSAO 在该像素不计算 (fall back to ao=1)
    FragNormal = N.xy * 0.5 + 0.5;   // [-1,1] -> [0,1] for RG16F
}

// SSAO shader (读 attachment 1)
uniform sampler2D uNormalTex;
vec3 N;
{
    vec2 enc = texture(uNormalTex, vUV).rg;
    vec2 nxy = enc * 2.0 - 1.0;
    float zsq = max(0.0, 1.0 - dot(nxy, nxy));
    N = vec3(nxy, sqrt(zsq));
    // (RG=0,0) -> N=(0,0,1), 退化情况 SSAO 仍能算 (但 AO 可能不准)
}
```

### 2.4 Backend 接口（新增 / 修改）

```cpp
// render_backend.h

// 修改: 新增 outNormalTex 可选参数 (向后兼容: 调用者传 nullptr 不需要 normal RT)
virtual uint32_t CreateHDRFBO(int w, int h,
                               uint32_t* outColorTex,
                               uint32_t* outNormalTex = nullptr) { return 0; }

// 修改: DeleteHDRFBO 内部释放 normal tex
virtual void DeleteHDRFBO(uint32_t /*fbo*/, uint32_t /*colorTex*/) {}
//        ^ normal tex 仍由后端内部 map 关联管理 (与 depthRB 一致), 用户不感知

// 新增: SSAO 模块用来取关联的 normal tex
virtual uint32_t GetHDRNormalTex(uint32_t /*fbo*/) const { return 0; }

// 修改: SSAO Process 接口加 normalTex 参数
virtual void DrawSSAO(uint32_t /*depthTex*/, uint32_t /*noiseTex*/,
                       uint32_t /*normalTex*/,         // ★ 新增
                       uint32_t /*dstFbo*/, ...) {}
```

### 2.5 SSAORenderer 模块改动（最小化）

```cpp
// ssao_renderer.cpp Process()
void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    ...
    // ★ Phase E.8.x — 拿 G-buffer normal tex (替代 ddx/ddy)
    uint32_t normalTex = g.backend->GetHDRNormalTex(hdrFbo);
    if (!normalTex) {
        // MRT 不可用 → SSAO 也不应启用 (Init 阶段已经 supported=false)
        return;
    }

    // BlitHDRDepthToSSAO ... (不变)
    g.backend->DrawSSAO(g.depthTex, g.noiseTex,
                         normalTex,     // ★ 新增
                         g.fbos[0], ...);
    ...
}
```

### 2.6 HDRRenderer 改动（无）

`HDRRenderer::CreateRT()` 内部调用 `backend->CreateHDRFBO(w, h, &tex)`，传 `nullptr` 给 outNormalTex 即可。但要让 SSAO 能用 normal RT，HDR 必须创建带 normal 的 FBO。

**两选项**：

- **a. HDRRenderer 总是创建 MRT**：内部默认传 `&normalTex` 给 CreateHDRFBO。注意：`HDRRenderer::g.normalTex` 不需要存（用户不感知；后端通过 fbo 索引查）。
- **b. HDR 增 SetUseGBufferNormal(bool) 控制**：复杂，否决。

> **决策**：a — HDRRenderer::CreateRT 总是请求带 normal 的 MRT。CreateHDRFBO 内部失败时返回 0，HDR 整体 disable（与现有失败路径一致）。

### 2.7 SupportsSSAO 升级

```cpp
// render_gl33.cpp
bool SupportsSSAO() const override {
    // 现有: tonemap + bloom + 全屏 quad 都 ready
    return tonemapSupported && bloomSupported && lensFlareSupported && /* ... */
        // ★ 新增: 必须 MRT 创建过至少一次成功 (实际由 HDR FBO 创建时探测)
        ssaoSupported;   // ssaoSupported 在 InitLensFx 已检查 shader; 不变
}
```

实际：
- `ssaoSupported` 在 InitLensFx 检查 SSAO shader 编译
- HDR FBO 创建失败 → `HDR.Enable()` 返回 false → SSAO.OnHDREnabled 不触发 → SSAO 不启用
- 自然链路保护，无需额外检查

### 2.8 CreateHDRFBO 失败回退

```cpp
uint32_t CreateHDRFBO(int w, int h, uint32_t* outColorTex, uint32_t* outNormalTex) {
    // 1. 创建 color tex (RGBA16F)
    GLuint colorTex = ...;
    // 2. 创建 normal tex (RG16F) — 只在 outNormalTex != null 时
    GLuint normalTex = 0;
    if (outNormalTex) {
        glGenTextures(1, &normalTex);
        glBindTexture(GL_TEXTURE_2D, normalTex);
        glTexImage2D(..., GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
        // 失败检查 (glGetError 等); 失败 → 释放 colorTex 返回 0
    }
    // 3. 创建 depth RB
    // 4. 创建 FBO 并绑定 attachments
    // 5. 检查 FRAMEBUFFER_COMPLETE; 失败 → 全释放返回 0
    // 6. 写入 hdrFboNormalTex map (新增, 与 hdrFboDepthRB 平行)
    // 7. *outColorTex = colorTex; *outNormalTex = normalTex; return fbo;
}
```

---

## 3. 验收标准

### 3.1 功能验收

- [ ] HDR FBO 创建后 attachment 1 = RG16F normal tex
- [ ] glDrawBuffers(2) 正确启用双 attachment 写入
- [ ] 所有写 HDR 的 shader 输出 (FragColor + FragNormal)
- [ ] PBR / Unlit shader VS 正确传递 view-space normal
- [ ] SSAO shader 读 normal tex 替代 ddx/ddy
- [ ] demo_ssao N 键 toggle 显示 normal RT 可视化（debug）
- [ ] Lua `Light.Graphics.SSAO.GetNormalTexId()` 返回有效 tex id（启用时）/ 0（未启用）

### 3.2 视觉验收

- [ ] demo_ssao SSAO ON：边缘无条纹噪声（对比 Phase E.8）
- [ ] demo_ssao SSAO OFF：完全等同 Phase E.8（attachment 0 不变）
- [ ] Phase E.3-E.7 demo 视觉无回归

### 3.3 CI 验收

- [ ] build-windows / linux / macos / ios / android / web 全 6/6 绿
- [ ] ssao smoke 50+ 断言全过 + 新增 GetNormalTexId 断言
- [ ] Phase E 累计 smoke 无回归

### 3.4 性能验收（非阻塞）

- 内存：HDR RT +4 MB @1080p（24% 增量）
- GPU：MRT 写入开销 ~0.05 ms / 帧
- SSAO 自身：节省 ddx/ddy ~0.02 ms / 帧
- 净影响：~+0.03 ms / 帧（可忽略）

---

## 4. 任务边界限制

### 4.1 范围内

✅ HDR FBO MRT 升级（默认 MRT）
✅ 4 个 3D shader（Unlit / PBR / 各 Skin 变体）+ 2D shader 写 normal
✅ SSAO shader 改读 normal tex
✅ 1 个 backend 新接口 + 2 个修改
✅ Lua 加 1 个 debug API（`GetNormalTexId`）
✅ demo 加 N 键可视化
✅ smoke + CI 注册

### 4.2 范围外

❌ 不修改 SSAO 算法（kernel / blur / composite 全部不变）
❌ 不引入 G-buffer 其他 channel（albedo / metallic / roughness）
❌ 不改 HDR depth 路径（仍 renderbuffer，不 texture）
❌ 不暴露 normal RT 给 Lua 编辑（仅 debug 读）
❌ 不影响 Bloom / AE / LensDirt / Streak / LensFlare 现有逻辑
❌ 不向 Phase E.3-E.7 的 demo 做改动

---

## 5. 关键假设（已确认）

1. ✅ 用户选 A：默认 MRT + 失败静默 disable（2026-05-12）
2. ✅ Q2-Q6 采纳推荐（2D 写 (0,0) / demo 加 N 键 / 加 GetNormalTexId / SSAO 不可用 / 仅 ssao smoke）
3. ✅ MRT 在 GL3.3 / GLES3 / WebGL2 平台都原生支持（`GL_MAX_DRAW_BUFFERS >= 4`）
4. ✅ RG16F 纹理在所有 6 平台都可创建（GL3.3 + GLES3 + iOS GLES3 + emscripten WebGL2）
5. ✅ Phase E 累计 6 commit 全绿基线（head `0acfcfb`）

---

## 6. 子阶段拆分（4 子阶段）

| 子阶段 | 主题 | 文件改动 | 行数估计 |
|--------|------|----------|---------|
| **E.8.x.1** Backend MRT | render_gl33.cpp + render_backend.h MRT FBO + 1 新接口 + map | ~150 |
| **E.8.x.2** Shaders | FS_SOURCE / FS_LIT2D / FS_UNLIT / FS_PBR + 4 Skin VS/FS + FS_SSAO | ~200 |
| **E.8.x.3** Module + Lua | SSAORenderer Process 改动 + GetNormalTexId Lua API + smoke + demo N 键 | ~80 |
| **E.8.x.4** Docs | ALIGNMENT / CONSENSUS / DESIGN / TASK / ACCEPTANCE / FINAL / TODO | ~600 |

**总代码 ~430 行 + docs ~600 行**

---

## 7. 共识完成

✅ 所有不确定性已解决（Q1-Q6）
✅ 技术方案与现有架构对齐（最小侵入；attachment 1 隔离）
✅ 验收标准具体可测试
✅ 项目特性规范已对齐（与 Phase E.8 一致）

**进入 6A 阶段 2: Architect**。
