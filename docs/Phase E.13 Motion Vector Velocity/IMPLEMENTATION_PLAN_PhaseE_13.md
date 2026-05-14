# Phase E.13 Motion Vector Velocity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a full motion-vector velocity buffer to the HDR/SSR pipeline so Temporal SSR can reproject dynamic objects, GPU skinning, and morph targets more accurately.

**Architecture:** Extend the existing HDR MRT from `color + normal` to `color + normal + velocity`; write screen-space velocity during the main 3D draw; make SSR Temporal prefer `velocityTex` while preserving the Phase E.12 matrix reprojection fallback. Previous state is explicit for ordinary meshes and automatic for Animator-driven GPU skin/morph paths.

**Tech Stack:** C++17, OpenGL 3.3 Core / GLES 3.0 shader strings, Lua C API, ChocoLight `RenderBackend`, `HDRRenderer`, `SSRRenderer`, `Light.Graphics.Mesh`, and `Light.Animation`.

---

## 0. Hard Constraints

- [ ] Do not run local C++ build commands: `cmake`, `cmake --build`, `msbuild`, or local `light.exe` runtime smoke.
- [ ] Local verification is limited to static checks such as `git diff --check`, file review, and source grep.
- [ ] Real compilation/runtime verification must happen through GitHub Actions CI after commit and push to `origin`.
- [ ] New backend virtual methods must have safe default implementations so Legacy backends remain compatible.
- [ ] New Lua parameters must be optional so existing scripts keep working.

---

## 1. File Responsibility Map

| File | Responsibility |
|---|---|
| `ChocoLight/include/render_backend.h` | Backend API for velocity RT, previous model, previous skin/morph state, and SSR Temporal velocity texture. |
| `ChocoLight/src/render_gl33.cpp` | GL33 resource creation, shader changes, velocity output, previous-state upload, SSR velocity sampling. |
| `ChocoLight/include/hdr_renderer.h` | Optional public query for HDR velocity texture. |
| `ChocoLight/src/hdr_renderer.cpp` | Request velocity RT and expose/query velocity attachment. |
| `ChocoLight/include/ssr_renderer.h` | Document velocity-first Temporal SSR. |
| `ChocoLight/src/ssr_renderer.cpp` | Pass `velocityTex` to backend Temporal pass. |
| `ChocoLight/src/light_graphics_mesh.cpp` | Parse optional `prevModelMat4` for `mesh:Draw`. |
| `ChocoLight/src/light_animation.cpp` | Store Animator previous pose and pass previous joints/morph weights to backend GPU paths. |
| `scripts/smoke/ssr.lua` | Add compatibility coverage for new optional parameters where feasible. |
| `samples/demo_ssr/main.lua` | Add dynamic-object velocity demo/HUD notes. |
| `docs/api/Light_Graphics.md` | Document `mesh:Draw(..., prevModelMat4)`. |
| `docs/api/Light_Animation.md` | Document `DrawSkinnedMesh(..., prevTransform)` and automatic previous pose. |
| `docs/Phase E.13 Motion Vector Velocity/ACCEPTANCE_PhaseE_13.md` | Record task completion and CI evidence. |
| `docs/Phase E.13 Motion Vector Velocity/FINAL_PhaseE_13.md` | Final delivery summary after CI. |
| `docs/Phase E.13 Motion Vector Velocity/TODO_PhaseE_13.md` | Deferred work: real-window visual validation, exact CPU velocity, roughness-aware filtering. |

---

## 2. Task 1 — Backend API Surface

**Files:**
- Modify: `ChocoLight/include/render_backend.h`

- [ ] **Step 1: Extend `CreateHDRFBO` signature**

Replace the current HDR FBO virtual with this compatible signature:

```cpp
virtual uint32_t CreateHDRFBO(int /*w*/, int /*h*/,
                               uint32_t* /*outColorTex*/,
                               uint32_t* /*outNormalTex*/ = nullptr,
                               uint32_t* /*outVelocityTex*/ = nullptr) { return 0; }
```

- [ ] **Step 2: Add velocity texture query**

Add next to `GetHDRNormalTex`:

```cpp
virtual uint32_t GetHDRVelocityTex(uint32_t /*fbo*/) const { return 0; }
```

- [ ] **Step 3: Add ordinary mesh previous-model contract**

Add near `DrawMeshMaterial`:

```cpp
virtual void SetNextPreviousModelMatrix(const float* /*prevModelMat4*/) {}
```

- [ ] **Step 4: Extend GPU skin and morph draw contracts**

Use default arguments so old call sites can be migrated gradually:

```cpp
virtual void DrawSkinnedMeshMaterial(uint32_t /*meshId*/, const MaterialDesc* /*desc*/,
                                      const float* /*jointMatrices*/, int /*jointCount*/,
                                      const float* /*prevJointMatrices*/ = nullptr,
                                      int /*prevJointCount*/ = 0) {}

virtual void DrawSkinnedMorphMeshMaterial(uint32_t /*meshId*/, const MaterialDesc* /*desc*/,
                                           const float* /*jointMatrices*/, int /*jointCount*/,
                                           const float* /*morphWeights*/, int /*morphTargetCount*/,
                                           const float* /*prevJointMatrices*/ = nullptr,
                                           int /*prevJointCount*/ = 0,
                                           const float* /*prevMorphWeights*/ = nullptr,
                                           int /*prevMorphTargetCount*/ = 0) {}
```

- [ ] **Step 5: Extend Temporal SSR contract**

Insert `velocityTex` after `depthTex`:

```cpp
virtual void DrawSSRTemporal(uint32_t /*curReflectTex*/,
                              uint32_t /*historyTex*/,
                              uint32_t /*depthTex*/,
                              uint32_t /*velocityTex*/,
                              uint32_t /*dstFbo*/,
                              int /*w*/, int /*h*/,
                              const float* /*reprojectMat4*/,
                              const float* /*invProjMat4*/,
                              float /*blendAlpha*/,
                              int /*rejectionMode*/,
                              int /*hasHistory*/) {}
```

- [ ] **Step 6: Verify affected call sites by static search**

Run:

```powershell
Select-String -Path ChocoLight/include/render_backend.h,ChocoLight/src/render_gl33.cpp,ChocoLight/src/ssr_renderer.cpp,ChocoLight/src/hdr_renderer.cpp,ChocoLight/src/light_animation.cpp -Pattern "CreateHDRFBO|DrawSSRTemporal|DrawSkinnedMeshMaterial|DrawSkinnedMorphMeshMaterial|SetNextPreviousModelMatrix|GetHDRVelocityTex"
```

Expected: all non-interface signatures and calls are updated in later tasks.

---

## 3. Task 2 — HDR Velocity Attachment

**Files:**
- Modify: `ChocoLight/src/render_gl33.cpp`
- Modify: `ChocoLight/src/hdr_renderer.cpp`
- Modify: `ChocoLight/include/hdr_renderer.h`

- [ ] **Step 1: Add GL33 velocity map**

Near `hdrFboNormalTex`, add:

```cpp
std::unordered_map<uint32_t, uint32_t> hdrFboVelocityTex;
```

- [ ] **Step 2: Update GL33 `CreateHDRFBO` override**

Change the override to:

```cpp
uint32_t CreateHDRFBO(int w, int h, uint32_t* outTex,
                      uint32_t* outNormalTex,
                      uint32_t* outVelocityTex) override
```

- [ ] **Step 3: Create optional RG16F velocity texture**

After normal texture creation, add:

```cpp
GLuint velocityTex = 0;
if (outVelocityTex) {
    glGenTextures(1, &velocityTex);
    if (!velocityTex) {
        glDeleteTextures(1, &tex);
        if (normalTex) glDeleteTextures(1, &normalTex);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, velocityTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}
```

Every later failure path must delete `velocityTex` when non-zero.

- [ ] **Step 4: Attach velocity and configure MRT draw buffers**

Use this attachment logic:

```cpp
if (normalTex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normalTex, 0);
if (velocityTex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, velocityTex, 0);
if (normalTex && velocityTex) {
    const GLenum drawBufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, drawBufs);
} else if (normalTex) {
    const GLenum drawBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBufs);
} else {
    const GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBufs);
}
```

- [ ] **Step 5: Store, query, and delete velocity texture**

On success:

```cpp
if (velocityTex) {
    hdrFboVelocityTex[fbo] = velocityTex;
    *outVelocityTex = velocityTex;
}
```

Add override:

```cpp
uint32_t GetHDRVelocityTex(uint32_t fbo) const override {
    auto it = hdrFboVelocityTex.find(fbo);
    return (it != hdrFboVelocityTex.end()) ? it->second : 0;
}
```

In `DeleteHDRFBO`, erase/delete `hdrFboVelocityTex[fbo]` the same way normal texture is handled.

- [ ] **Step 6: Request velocity from `HDRRenderer::CreateRT`**

Change:

```cpp
uint32_t normalTex = 0;
uint32_t fbo = g.backend->CreateHDRFBO(w, h, &tex, &normalTex);
```

to:

```cpp
uint32_t normalTex = 0;
uint32_t velocityTex = 0;
uint32_t fbo = g.backend->CreateHDRFBO(w, h, &tex, &normalTex, &velocityTex);
```

- [ ] **Step 7: Expose HDR velocity query**

In `hdr_renderer.h`:

```cpp
uint32_t GetVelocityTexture();
```

In `hdr_renderer.cpp`:

```cpp
uint32_t GetVelocityTexture() {
    return (g.backend && g.fbo) ? g.backend->GetHDRVelocityTex(g.fbo) : 0;
}
```

- [ ] **Step 8: Static check**

Run:

```powershell
git diff --check -- ChocoLight/include/render_backend.h ChocoLight/src/render_gl33.cpp ChocoLight/include/hdr_renderer.h ChocoLight/src/hdr_renderer.cpp
```

Expected: exit code `0`.

---

## 4. Task 3 — Static Mesh Velocity State and Shader Output

**Files:**
- Modify: `ChocoLight/src/render_gl33.cpp`

- [ ] **Step 1: Add GL33 velocity state fields**

Near `viewMatrix` and `hasView`, add:

```cpp
Mat4 prevViewProj;
bool hasPrevViewProjForVelocity = false;
bool hasNextPrevModel = false;
Mat4 nextPrevModel;
```

- [ ] **Step 2: Add `SetNextPreviousModelMatrix` override**

```cpp
void SetNextPreviousModelMatrix(const float* prevModelMat4) override {
    if (!prevModelMat4) {
        hasNextPrevModel = false;
        return;
    }
    std::memcpy(nextPrevModel.m, prevModelMat4, sizeof(nextPrevModel.m));
    hasNextPrevModel = true;
}
```

- [ ] **Step 3: Add velocity uniform helper**

```cpp
void UploadVelocityUniforms(GLuint program3D, const Mat4& curModel, const Mat4* prevModelOverride) {
    if (!program3D) return;
    Mat4 curViewProj = projection * (hasView ? viewMatrix : Mat4::Identity());
    Mat4 prevModel = prevModelOverride ? *prevModelOverride : curModel;
    GLint locCurVP = glGetUniformLocation(program3D, "uCurViewProj");
    GLint locPrevVP = glGetUniformLocation(program3D, "uPrevViewProj");
    GLint locPrevM = glGetUniformLocation(program3D, "uPrevModel");
    GLint locHas = glGetUniformLocation(program3D, "uHasVelocityHistory");
    if (locCurVP >= 0) glUniformMatrix4fv(locCurVP, 1, GL_FALSE, curViewProj.m);
    if (locPrevVP >= 0) glUniformMatrix4fv(locPrevVP, 1, GL_FALSE, prevViewProj.m);
    if (locPrevM >= 0) glUniformMatrix4fv(locPrevM, 1, GL_FALSE, prevModel.m);
    if (locHas >= 0) glUniform1i(locHas, hasPrevViewProjForVelocity ? 1 : 0);
}
```

If `Mat4::Identity()` is unavailable, use the project’s existing explicit identity construction pattern.

- [ ] **Step 4: Call helper in `DrawMeshMaterial`**

After `uMVP` and `uModel` upload:

```cpp
Mat4 prevModel;
const Mat4* prevModelPtr = nullptr;
if (hasNextPrevModel) {
    prevModel = nextPrevModel;
    prevModelPtr = &prevModel;
}
UploadVelocityUniforms(program3D, modelview, prevModelPtr);
hasNextPrevModel = false;
prevViewProj = projection * (hasView ? viewMatrix : Mat4::Identity());
hasPrevViewProjForVelocity = true;
```

- [ ] **Step 5: Update static mesh vertex shader strings**

In both GLES3 and GL33 `VS3D_SOURCE`, add:

```glsl
out vec4 vCurClip;
out vec4 vPrevClip;
uniform mat4 uPrevViewProj;
uniform mat4 uPrevModel;
```

In `main`:

```glsl
vec4 worldPrev = uPrevModel * vec4(aPos, 1.0);
gl_Position = uMVP * vec4(aPos, 1.0);
vCurClip = gl_Position;
vPrevClip = uPrevViewProj * worldPrev;
```

Keep existing `vWorldPos`, `vNormalW`, `vUV`, and `vColor` behavior.

- [ ] **Step 6: Update fragment shader velocity outputs**

In both GLES3 and GL33 `FS_UNLIT_SOURCE` and `FS_PBR_SOURCE`, add:

```glsl
layout(location=2) out vec2 FragVelocity;
in vec4 vCurClip;
in vec4 vPrevClip;
uniform int uHasVelocityHistory;
vec2 ClipToUV(vec4 p) {
    float w = max(abs(p.w), 1e-6);
    return (p.xy / w) * 0.5 + 0.5;
}
```

Before each successful fragment output path returns:

```glsl
FragVelocity = (uHasVelocityHistory == 1) ? (ClipToUV(vCurClip) - ClipToUV(vPrevClip)) : vec2(0.0);
```

- [ ] **Step 7: Static shader search**

Run:

```powershell
Select-String -Path ChocoLight/src/render_gl33.cpp -Pattern "FragVelocity|vCurClip|vPrevClip|uPrevViewProj|uPrevModel|uHasVelocityHistory"
```

Expected: names appear in both GLES3 and GL33 shader blocks and in C++ uniform upload.

---

## 5. Task 4 — GPU Skin and Morph Previous Pose

**Files:**
- Modify: `ChocoLight/src/render_gl33.cpp`
- Modify: `ChocoLight/src/light_animation.cpp`

- [ ] **Step 1: Add Animator previous-pose fields**

In `Animator` near `jointMatrices` and `morphWeights`:

```cpp
std::vector<float> prevJointMatrices;
std::vector<float> prevMorphWeights;
bool hasPrevPose = false;
```

- [ ] **Step 2: Add snapshot helper**

```cpp
static void SnapshotAnimatorPoseForVelocity(Animator* an) {
    if (!an || an->jointMatrices.empty()) {
        if (an) {
            an->prevJointMatrices.clear();
            an->prevMorphWeights.clear();
            an->hasPrevPose = false;
        }
        return;
    }
    an->prevJointMatrices = an->jointMatrices;
    an->prevMorphWeights = an->morphWeights;
    an->hasPrevPose = true;
}
```

- [ ] **Step 3: Snapshot at start of `l_Animator_Update`**

After reading `dt`, call:

```cpp
SnapshotAnimatorPoseForVelocity(an);
```

- [ ] **Step 4: Clear previous pose on manual seek and immediate transitions**

In `l_Animator_SetCurrentTime` and the immediate transition branch, add:

```cpp
an->prevJointMatrices.clear();
an->prevMorphWeights.clear();
an->hasPrevPose = false;
```

- [ ] **Step 5: Add previous joint UBO in GL33**

Near `uboJointMatrices`:

```cpp
GLuint uboPrevJointMatrices = 0;
static constexpr GLuint PREV_UBO_BINDING_POINT = 1;
```

Create/delete it wherever `uboJointMatrices` is created/deleted. Bind shader block name `PrevJointBlock` to binding point `1` for skin and skin+morph programs.

- [ ] **Step 6: Update skin/morph vertex shaders**

In both GLES3 and GL33 skin shader strings, add:

```glsl
out vec4 vCurClip;
out vec4 vPrevClip;
uniform mat4 uPrevViewProj;
layout(std140) uniform PrevJointBlock { mat4 uPrevJointMats[64]; };
```

For morph shader strings also add:

```glsl
uniform float uPrevMorphWeights[8];
```

Compute current and previous skinned positions separately; set:

```glsl
vCurClip = gl_Position;
vPrevClip = uPrevViewProj * prevPos;
```

- [ ] **Step 7: Pass previous state from `light_animation.cpp`**

When calling backend GPU skin draw, pass previous joints only if `an->hasPrevPose` and previous joint count equals current joint count. For morph, also pass `an->prevMorphWeights.data()` and size when available. If previous data is unavailable, pass `nullptr, 0` so backend falls back to current data.

- [ ] **Step 8: Backend fallback upload**

In GL33 skin/morph draw overrides, use current joints as previous joints when `prevJointMatrices == nullptr` or count mismatch. Use current morph weights when previous morph weights are missing.

---

## 6. Task 5 — Lua Previous Transform Wiring

**Files:**
- Modify: `ChocoLight/src/light_graphics_mesh.cpp`
- Modify: `ChocoLight/src/light_animation.cpp`
- Modify: `docs/api/Light_Graphics.md`
- Modify: `docs/api/Light_Animation.md`

- [ ] **Step 1: Add optional mat4 reader in `light_graphics_mesh.cpp`**

```cpp
static bool ReadOptionalMat4(lua_State* L, int idx, float* outMat, bool* hasMat) {
    *hasMat = false;
    if (lua_isnoneornil(L, idx)) return true;
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    for (int i = 0; i < 16; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_type(L, -1) != LUA_TNUMBER) {
            lua_pop(L, 1);
            return false;
        }
        outMat[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    *hasMat = true;
    return true;
}
```

- [ ] **Step 2: Use optional previous model in `mesh:Draw`**

At the top of `l_Mesh_Draw`, after backend/id checks:

```cpp
float prevModel[16];
bool hasPrevModel = false;
if (!ReadOptionalMat4(L, 3, prevModel, &hasPrevModel)) {
    return luaL_error(L, "mesh:Draw prevModel must be a 16-element table or nil");
}
if (hasPrevModel) {
    g_render->SetNextPreviousModelMatrix(prevModel);
}
```

Accepted call forms:

```lua
mesh:Draw()
mesh:Draw(textureId)
mesh:Draw(material)
mesh:Draw(textureId, prevModel)
mesh:Draw(material, prevModel)
```

- [ ] **Step 3: Add optional previous transform to `Animation.DrawSkinnedMesh`**

After current transform parsing in `l_Anim_DrawSkinnedMesh`, parse arg 5:

```cpp
float prevModelMat[16];
bool hasPrevModelMat = false;
if (!lua_isnoneornil(L, 5)) {
    if (!ReadMat4FromTable(L, 5, prevModelMat)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "prev transform must be a 16-element table or nil");
        return 2;
    }
    hasPrevModelMat = true;
}
```

Pass this data into GPU helper functions. CPU fallback remains camera-only/object-fallback for E.13.

- [ ] **Step 4: Update API docs**

Document:

```lua
mesh:Draw(material_or_texture_or_nil, prevModelMat4_or_nil)
Animation.DrawSkinnedMesh(mesh, animator, transformMat4_or_nil, material_or_nil, prevTransformMat4_or_nil)
```

Explain in plain language: the previous transform is “上一帧模型矩阵”; if omitted, object motion falls back to camera-only velocity.

---

## 7. Task 6 — SSR Temporal Velocity Sampling

**Files:**
- Modify: `ChocoLight/src/render_gl33.cpp`
- Modify: `ChocoLight/src/ssr_renderer.cpp`
- Modify: `ChocoLight/include/ssr_renderer.h`

- [ ] **Step 1: Add velocity sampler uniforms to Temporal shader**

In both Temporal shader blocks:

```glsl
uniform sampler2D uVelocityTex;
uniform int uHasVelocityTex;
```

Replace previous-UV computation with:

```glsl
vec2 prevUV;
if (uHasVelocityTex == 1) {
    vec2 vel = texture(uVelocityTex, vUV).rg;
    if (abs(vel.x) > 0.5 || abs(vel.y) > 0.5) {
        FragColor = cur;
        return;
    }
    prevUV = vUV - vel;
} else {
    float depth = texture(uDepthTex, vUV).r;
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 prevClip = uReprojectMat * ndc;
    float w = max(prevClip.w, 1e-6);
    prevUV = (prevClip.xy / w) * 0.5 + 0.5;
}
```

Keep existing bounds rejection and neighborhood clipping.

- [ ] **Step 2: Add GL33 uniform locations and bind slot 3**

Add fields:

```cpp
GLint locSSRTemporal_VelocityTex = -1;
GLint locSSRTemporal_HasVelocityTex = -1;
```

During init:

```cpp
locSSRTemporal_VelocityTex = glGetUniformLocation(programSSRTemporal, "uVelocityTex");
locSSRTemporal_HasVelocityTex = glGetUniformLocation(programSSRTemporal, "uHasVelocityTex");
if (locSSRTemporal_VelocityTex >= 0) glUniform1i(locSSRTemporal_VelocityTex, 3);
```

In `DrawSSRTemporal`:

```cpp
if (velocityTex) {
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, velocityTex);
}
if (locSSRTemporal_HasVelocityTex >= 0) glUniform1i(locSSRTemporal_HasVelocityTex, velocityTex ? 1 : 0);
```

- [ ] **Step 3: Pass velocity texture from SSRRenderer**

In `SSRRenderer::Process`, after normal texture query:

```cpp
uint32_t velocityTex = g.backend->GetHDRVelocityTex(hdrFbo);
```

Update backend call:

```cpp
g.backend->DrawSSRTemporal(g.reflectTex,
                           g.historyTexs[readIdx],
                           g.depthTex,
                           velocityTex,
                           g.historyFbos[writeIdx],
                           g.srcW, g.srcH,
                           reprojMat, invProj,
                           g.temporalAlpha,
                           g.rejectionMode,
                           g.hasPrevViewProj ? 1 : 0);
```

- [ ] **Step 4: Update comments in `ssr_renderer.h`**

Document that E.13 uses velocity first and falls back to E.12 matrix reprojection when `velocityTex == 0`.

---

## 8. Task 7 — Smoke, Demo, and Docs

**Files:**
- Modify: `scripts/smoke/ssr.lua`
- Modify: `samples/demo_ssr/main.lua`
- Modify: `docs/api/Light_Graphics.md`
- Modify: `docs/api/Light_Animation.md`
- Create: `docs/Phase E.13 Motion Vector Velocity/ACCEPTANCE_PhaseE_13.md`
- Create: `docs/Phase E.13 Motion Vector Velocity/FINAL_PhaseE_13.md`
- Create: `docs/Phase E.13 Motion Vector Velocity/TODO_PhaseE_13.md`

- [ ] **Step 1: Add smoke compatibility checks**

In `scripts/smoke/ssr.lua`, add a section that keeps old SSR Temporal API checks and documents that velocity is an internal render-path feature. If an existing Mesh smoke fixture is available in the script, add optional-argument syntax coverage for `mesh:Draw(nil, identityMat4)`; otherwise record coverage in acceptance as API-doc/static only.

- [ ] **Step 2: Update demo HUD**

In `samples/demo_ssr/main.lua`, add HUD text showing:

```lua
"Temporal SSR: velocity buffer path enabled when HDR velocity attachment is available"
```

If the demo already has a moving reflective object, preserve it and add previous-transform tracking in Lua. If not, do not add a large new scene in E.13; record real visual validation in TODO.

- [ ] **Step 3: Create acceptance document**

Create `ACCEPTANCE_PhaseE_13.md` with sections:

```markdown
# Phase E.13 Motion Vector Velocity — ACCEPTANCE

## Static Verification
- [ ] `git diff --check` passed
- [ ] All `CreateHDRFBO` signatures updated
- [ ] All `DrawSSRTemporal` signatures updated
- [ ] GLES3 and GL33 shader blocks contain velocity outputs

## CI Verification
- [ ] GitHub Actions run id:
- [ ] Windows:
- [ ] Linux:
- [ ] macOS:
- [ ] Android:
- [ ] iOS:
- [ ] Web:

## Deferred Visual Validation
- Real-window SSR ghosting comparison remains user/team validation.
```

- [ ] **Step 4: Create final and TODO documents**

`FINAL_PhaseE_13.md` must summarize delivered behavior only after CI succeeds.

`TODO_PhaseE_13.md` must include:

```markdown
# Phase E.13 Motion Vector Velocity — TODO

- Real-window visual validation for dynamic SSR ghosting.
- Exact CPU skin/morph velocity instead of camera-only fallback.
- Roughness-aware Temporal SSR weighting after material/roughness G-buffer design.
- User shader velocity output contract if custom MRT shader support is added.
```

---

## 9. Task 8 — Static Verification, Commit, Push, CI

**Files:**
- Verify: all changed files

- [ ] **Step 1: Run local static whitespace check only**

```powershell
git diff --check
```

Expected: exit code `0`.

- [ ] **Step 2: Review signature consistency by search**

```powershell
Select-String -Path ChocoLight/include/render_backend.h,ChocoLight/src/render_gl33.cpp,ChocoLight/src/ssr_renderer.cpp,ChocoLight/src/hdr_renderer.cpp,ChocoLight/src/light_animation.cpp -Pattern "CreateHDRFBO|DrawSSRTemporal|DrawSkinnedMeshMaterial|DrawSkinnedMorphMeshMaterial|GetHDRVelocityTex|SetNextPreviousModelMatrix"
```

Expected: interface, overrides, and call sites use the same parameter order.

- [ ] **Step 3: Commit in small chunks**

Recommended commit sequence:

```powershell
git add docs/Phase\ E.13\ Motion\ Vector\ Velocity
git commit -m "docs: add Phase E.13 velocity buffer plan"

git add ChocoLight/include/render_backend.h ChocoLight/src/render_gl33.cpp ChocoLight/include/hdr_renderer.h ChocoLight/src/hdr_renderer.cpp
git commit -m "feat: add HDR velocity buffer plumbing"

git add ChocoLight/src/render_gl33.cpp ChocoLight/src/light_animation.cpp ChocoLight/src/light_graphics_mesh.cpp
git commit -m "feat: write motion vectors for 3D animation paths"

git add ChocoLight/include/ssr_renderer.h ChocoLight/src/ssr_renderer.cpp ChocoLight/src/render_gl33.cpp
git commit -m "feat: use velocity buffer in temporal SSR"

git add scripts/smoke/ssr.lua samples/demo_ssr/main.lua docs/api docs/Phase\ E.13\ Motion\ Vector\ Velocity
git commit -m "docs: finalize Phase E.13 velocity validation"
```

- [ ] **Step 4: Push only to origin**

```powershell
git push origin main
```

- [ ] **Step 5: Watch CI**

```powershell
gh run list --limit 5
gh run watch <run-id>
```

If CI fails:

```powershell
gh run view <run-id> --log-failed
```

Fix based on logs, commit, push again, and update `ACCEPTANCE_PhaseE_13.md`.

---

## 10. Plan Self-Review

- [x] Spec coverage: HDR velocity RT, static mesh, GPU skin, GPU morph, SSR Temporal fallback, Lua docs, demo/docs, and CI workflow are mapped to tasks.
- [x] Scope control: exact CPU skin/morph velocity and roughness-aware filtering stay deferred.
- [x] Type consistency: names match the current codebase entry points reviewed for `CreateHDRFBO`, `DrawSSRTemporal`, `DrawSkinnedMeshMaterial`, `DrawSkinnedMorphMeshMaterial`, `mesh:Draw`, and `Animation.DrawSkinnedMesh`.
- [x] Local verification policy: no C++ build or local runtime smoke is requested.
