# ACCEPTANCE — Phase E.1.5 · DrawLit2DQuad + Lit2D 渲染落地

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.5**：把 E.1.2 (shader) + E.1.3 (state) + E.1.4 (Lua API) 拼起来，让 `Light.Graphics.DrawLit` 在 GL33 后端真正驱动 sprite_lit_2d shader 渲染，并提供 `DrawLit2DTriangles` / `CreateLit2DMesh` 等扩展接口。

---

## 1. 改动摘要

| 文件 | 类型 | 改动量 | 关键改动 |
|------|------|--------|----------|
| `ChocoLight/include/render_backend.h` | 修改 | +14 行 | 前向声明 `namespace Lighting2D { struct State; }`; 新增虚接口 `UploadLighting2D(const Lighting2D::State*)` 默认 no-op |
| `ChocoLight/src/light_lighting2d.cpp` | 修改 | +1 行 / -3 行 | include `render_backend.h`; 把 `UploadToShader` 从 no-op 改为 `backend->UploadLighting2D(&g_state)` 转发 |
| `ChocoLight/src/render_gl33.cpp` | 修改 | +205 行 | (1) include `light_lighting2d.h`；(2) 类内追加 `vboLit2DCapacity` + `litMeshes` 池 + `nextLitMeshId`；(3) `InitLit2D` 设 capacity；(4) `Shutdown` 释放池；(5) 实现 `EnsureLit2DVBOCapacity` + `BeginLit2DDraw` + `EndLit2DDraw` helper + `UploadLighting2D` + `DrawLit2DQuad` + `DrawLit2DTriangles` + `CreateLit2DMesh` + `DeleteLit2DMesh` |
| `ChocoLight/src/light_graphics.cpp` | 修改 | +115 行 | 实现 `l_DrawLit` + `l_DrawLitQuad`；`graphics_funcs[]` 注册 `DrawLit` / `DrawLitQuad` |
| `scripts/smoke/lighting2d.lua` | 修改 | +20 行 | 追加 § 14：`Light.Graphics.DrawLit/DrawLitQuad` API 表面存在检查 + no-window guard（不崩） |

---

## 2. 关键设计

### 2.1 `RenderBackend::UploadLighting2D` 虚接口

```cpp
namespace Lighting2D { struct State; }       // render_backend.h 前向声明
...
virtual void UploadLighting2D(const Lighting2D::State* state) {}
```

- **前向声明而非 include**：避免把 `light_lighting2d.h` 拉进所有使用 backend 的翻译单元；只在 GL33 实现里真正 include
- **签名为指针而非 SOA 数组**：把 SOA 拆分留给 backend 内部（GL33 一次性 build 临时数组），接口可读
- **默认 no-op**：Legacy / 不支持 Lit2D 的后端自动安全；调用方应通过 `SupportsLit2D()` 守卫

### 2.2 `Lighting2D::UploadToShader` 真实实现

```cpp
void UploadToShader(RenderBackend* backend, uint32_t programId) {
    (void)programId;                          // 保留参数, 留作未来 multi-program
    if (!backend) return;
    backend->UploadLighting2D(&g_state);
}
```

E.1.3 的 no-op 占位被替换为单行转发。`programId` 参数签名保留以维持向后兼容（GL33 内部持有 `programLit2D`，不依赖外部传入）。

### 2.3 GL33Backend `UploadLighting2D` SOA 上传

```cpp
constexpr int M = Lighting2D::MAX_LIGHTS;   // 16
int   types[M];
float poss[M*3], dirs[M*3], cols[M*3];      // vec3 padded (z=0 for 2D)
float rngs[M], intens[M], ics[M], ocs[M];
int cnt = 0;
for (int i = 0; i < M; ++i) {
    const auto& l = state->lights[i];
    if (l.type == Lighting2D::TYPE_INACTIVE) continue;
    types[cnt] = l.type;
    poss[cnt*3+0] = l.pos[0]; poss[cnt*3+1] = l.pos[1]; poss[cnt*3+2] = 0.0f;
    // ... 其他字段
    ++cnt;
}

glUniform3fv(locLit2D_Ambient,    1, state->ambient);
glUniform1i (locLit2D_LightCount, cnt);
if (cnt > 0) {
    glUniform1iv(locLit2D_LightType,      cnt, types);
    glUniform3fv(locLit2D_LightPos,       cnt, poss);
    // ... 7 个数组一次性上传
}
```

- **栈分配**：8 个临时数组共 ~600 字节，远低于线程栈 1MB
- **dense pack**：跳过 INACTIVE slot，shader 侧只看 `cnt` 个有效灯
- **`cnt == 0` 安全**：仅设 `lightCount=0`，shader 侧 `for` 循环立即 break

### 2.4 `DrawLit2DQuad` / `DrawLit2DTriangles` 共享 Begin/End helper

```cpp
void BeginLit2DDraw(uint32_t baseTex, uint32_t normTex) {
    glUseProgram(programLit2D);
    glUniformMatrix4fv(locLit2D_MVP,   1, GL_FALSE, (projection * modelview).m);
    glUniformMatrix4fv(locLit2D_Model, 1, GL_FALSE, modelview.m);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, baseTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, normTex);
    glUniform1i(locLit2D_HasNormalMap, normTex ? 1 : 0);
    Lighting2D::UploadToShader(this, programLit2D);
}

void EndLit2DDraw() {
    glBindVertexArray(0);
    glUseProgram(program);            // 切回默认 2D
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
}
```

提取 prologue/epilogue 让 Quad / Triangles / 未来 Mesh draw 三种路径共享，避免 30+ 行重复代码。

### 2.5 动态 VBO 扩容（`EnsureLit2DVBOCapacity`）

```cpp
int vboLit2DCapacity = 0;             // 类字段, InitLit2D 设为 LIT2D_VBO_INITIAL_VERTS=4

void EnsureLit2DVBOCapacity(int vertexCount) {
    if (vertexCount <= vboLit2DCapacity) return;
    int newCap = vboLit2DCapacity ? vboLit2DCapacity : LIT2D_VBO_INITIAL_VERTS;
    while (newCap < vertexCount) newCap *= 2;          // 翻倍增长
    glBindBuffer(GL_ARRAY_BUFFER, vboLit2D);
    glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(RenderVertex2DLit), nullptr, GL_DYNAMIC_DRAW);
    vboLit2DCapacity = newCap;
}
```

- **VBO 重分配不影响 VAO**：`vaoLit2D` 内 `glVertexAttribPointer` 绑定的是 vbo handle，不是其内容，重分配 size 不破坏 attribute layout
- `DrawLit2DQuad` 固定 4 verts，永不触发扩容（初始容量已 = 4）
- `DrawLit2DTriangles(count)` 任意 count，按需扩容

### 2.6 Lit2D 永久 mesh 池

```cpp
std::unordered_map<uint32_t, MeshGPU> litMeshes;
uint32_t                              nextLitMeshId = 0xA0000001u;
```

- **高位掩码 `0xA0000000`**：与 `meshes` (无前缀) / `skinnedMeshes` (`0x80000000`) / `skinnedMorphMeshes` (`0xC0000000`) 区分
- **复用 `MeshGPU` POD**：`vao/vbo/ebo/indexCount`，与 3D mesh 一致
- **`CreateLit2DMesh` 失败 fallback**：任一 GL 对象创建失败时清理已创建对象后返回 0，无泄漏

### 2.7 Lua API `l_DrawLit` / `l_DrawLitQuad`

- **签名对齐 `l_Draw` / `l_DrawQuad`**：第一参 image, 第二参 **normalMap**（新增），其余 (x, y, z, transforms) 不变
- **`SupportsLit2D` 守卫**：后端不支持时直接 `return 0`，不崩、不警告
- **BatchRenderer Flush**：在调用 `DrawLit2DQuad` 前主动 `BatchRenderer::Flush()`，保证累积的普通 sprite 先出，再画 Lit sprite，顺序正确
- **默认 normal/tangent**：`N = (0, 0, 1)`、`T = (1, 0, 0, 1)` —— 平面 sprite。未来如需自定义法线传顶点流接口可扩展

---

## 3. 数据流（端到端）

```
Lua:
  Light.Lighting2D.AddPointLight({x=200, y=100, color={r=1,g=0.8,b=0.5}})
       ↓ (写 Lighting2D::g_state.lights[0])
  Light.Graphics.DrawLit(hero, hero_normal, 150, 200)
       ↓ l_DrawLit
       ↓
C++ (light_graphics.cpp):
  BatchRenderer::Flush()              ← 把之前累积的普通 sprite 出栈
  g_render->PushMatrix() + Translate + Transform
  build 4× RenderVertex2DLit (含 N+T)
  g_render->DrawLit2DQuad(verts, baseTex, normTex)
       ↓
C++ (render_gl33.cpp · GL33Backend):
  BeginLit2DDraw(baseTex, normTex):
    glUseProgram(programLit2D)
    glUniformMatrix4fv(uMVP / uModel)
    glActiveTexture(0/1) + glBindTexture
    glUniform1i(uHasNormalMap)
    Lighting2D::UploadToShader(this, programLit2D)   ← 转发回 backend
      → GL33Backend::UploadLighting2D(state):
          glUseProgram(programLit2D)
          build SOA 8 数组
          glUniform3fv(uAmbient) + glUniform1i(uLightCount)
          glUniform1iv/3fv/1fv ×7 (16 灯)             ← 一次性上传
  glBindVertexArray(vaoLit2D)
  glBufferSubData(vboLit2D, 4 verts)
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr)
  EndLit2DDraw():
    glUseProgram(program) + glBindVertexArray(vao)   ← 切回默认 2D
       ↓
GPU: sprite_lit_2d shader → 多光 forward 渲染
```

---

## 4. 验收清单（对齐 `TASK_PhaseE.md` § E.1.5）

| 验收标准 | 状态 | 证据 |
|----------|------|------|
| `Light.Graphics.DrawLit(img, nil, x, y)` 单 sprite 显示 | ⏸ E.1.7 demo 验证 | C++ 路径已通；headless smoke 无 GL context 无法肉眼检验 |
| 加 1 个 point + ambient 后颜色合理 | ⏸ E.1.7 demo 验证 | shader 已被 E.1.2 验证 link 成功，uniform 已被 E.1.5 上传 |
| 加 normalMap 凹凸感正确 | ⏸ E.1.7 demo 验证 | `uHasNormalMap` 路径已实现，TBN 在 VS_LIT2D 内构造 |
| 1000 lit quad < 16ms | ⏸ E.1.7 性能 benchmark | `EnsureLit2DVBOCapacity` 翻倍增长 + 单 drawcall/sprite 已就绪 |
| **本地 build + smoke 全通过** | ✅ | `Light.dll` 重新编译成功；`lighting2d.lua` 22 段 PASS + `graphics.lua` 11 PASS |

---

## 5. 本地验证记录

```text
[OK] lightc -p lighting2d.lua

cmake --build ChocoLight\build --config Release --target Light
  light_particles.cpp / tilemap / ui / render_backend / render_gl33 /
  render_legacy / video_backend_ffmpeg
  Light.vcxproj -> ChocoLight\build\bin\Release\Light.dll       ✓ 编译通过

light.exe scripts\smoke\lighting2d.lua:
  [Light] Engine loaded: 96 modules from Light.dll
  PASS: Light.Lighting2D module surface ok (11 functions)
  ... (20 段)
  PASS: Light.Graphics.DrawLit / DrawLitQuad function surface ok    ← E.1.5 新增
  PASS: DrawLit / DrawLitQuad no-window guard ok (no crash)          ← E.1.5 新增
  ==== Light.Lighting2D smoke DONE ====

light.exe scripts\smoke\graphics.lua:
  [Light.Graphics smoke] 通过 11 / 失败 0    ← 既有功能未破
```

`GetBackendName="None"` 表示 headless 环境没有 GL 后端 → `SupportsLit2D` 返回 false → `DrawLit` 立即返回 0；防御路径正确生效。

---

## 6. 性能与资源占用

| 资源 | 大小 | 备注 |
|------|------|------|
| `programLit2D` | 1 个 GL program | E.1.2 创建一次，永久持有 |
| `vaoLit2D` / `vboLit2D` / `eboLit2D` | 1 套 + 4 verts 初始 VBO | `DrawLit2DTriangles` 调用时按需翻倍扩容 |
| Lit2D uniform 上传 | 每次 Draw 一次性上传 ~600 字节 | 8 个数组 + ambient + count |
| `litMeshes` 池 | 按需，每条目独立 VAO+VBO+EBO | 高位 `0xA0000000` 区分 |
| 栈临时数组 | ~600 字节 / draw | 远低于 1MB 栈空间 |

每个 lit sprite = 1 draw call（无 batch）；后续 E.2 / 优化可考虑同纹理 lit sprite 也走 batch。

---

## 7. 与后续任务的衔接

| 任务 | 状态 | 衔接点 |
|------|------|--------|
| **E.1.6** ECS `Light2D` / `LitSprite` | ✅ 解锁 | `light_ecs.cpp::_DrawLitSprite()` 直接调 `g_render->DrawLit2DQuad` 或 Lua `Light.Graphics.DrawLit` |
| **E.1.7** demo + 视觉验收 smoke | ✅ 解锁 | `samples/demo_2d_lighting/main.lua` 用 `Light.Lighting2D` + `Light.Graphics.DrawLit` 端到端跑通 4 个验收点 |
| **E.1.8** API 文档同步 | ⏸ 等 E.1.7 demo 锁定接口 | 把 11 + 2 Lua API 写进 `docs/api/Light_Lighting2D.md` 和 `Light_Graphics.md` |

---

## 8. 已知限制与未来改进

| 限制 | 说明 / 缓解 |
|------|------------|
| **每 sprite 1 draw call** | 当前 `DrawLit2DQuad` 不入 BatchRenderer。E.2 后可加 `LitBatchRenderer` 同纹理同 Lighting state 合并 |
| **顶点 normal/tangent 硬编码** | Lua 端 `l_DrawLit` 永远传 `(0,0,1)/(1,0,0,1)`。未来如需逐顶点法线（粒子定向法线、3D-look-at sprite），可通过 `DrawLit2DTriangles` 顶点流接口 |
| **`UploadLighting2D` 每帧每 draw 上传** | 当前每个 lit sprite 都上传一次（哪怕 state 没变）。优化：在 Lighting2D 加 dirty bit；DrawLit2DQuad 只在 dirty 时调 Upload。延后到 E.2.x |
| **headless smoke 无视觉验证** | E.1.7 demo 提供肉眼 + 截图断言（demo README 列 ambient/point/spot/normalMap 各场景的预期效果） |

---

## 9. 提交信息建议

```
feat(phase-e1.5): DrawLit2DQuad + 2D Lit forward 渲染落地

- render_backend.h: + UploadLighting2D(state*) virtual + forward decl
- light_lighting2d.cpp: UploadToShader -> backend->UploadLighting2D
- render_gl33.cpp (+205 lines):
    - vboLit2DCapacity 字段 + litMeshes 池 + nextLitMeshId
    - EnsureLit2DVBOCapacity (×2 增长)
    - BeginLit2DDraw / EndLit2DDraw (prologue/epilogue 复用)
    - UploadLighting2D: 16-light SOA + glUniform3fv/1iv/1fv ×8
    - DrawLit2DQuad: 4 verts + 静态 EBO + glDrawElements
    - DrawLit2DTriangles: 任意 count + glDrawArrays + 扩容
    - CreateLit2DMesh / DeleteLit2DMesh + Shutdown 释放池

- light_graphics.cpp:
    - l_DrawLit + l_DrawLitQuad (与 l_Draw/l_DrawQuad 平行)
    - SupportsLit2D 守卫 + BatchRenderer::Flush
    - graphics_funcs[]: 注册 DrawLit / DrawLitQuad

- scripts/smoke/lighting2d.lua: + § 14 DrawLit API surface + no-window guard
    本地: 22 段 PASS + DONE (graphics.lua 11 PASS 不破)

docs: ACCEPTANCE_PhaseE_1_5.md
```
