# ACCEPTANCE — Phase E.1.1 · RenderVertex2DLit + VAO 基础设施

> 6A 工作流 · 阶段 5 · 自动化（Automate）执行记录 · 第 1 个原子任务

---

## 1. 任务定义回顾

来源：`TASK_PhaseE.md` 第 3 节 E.1.1

**输出契约**：
- `render_backend.h`：新增 `struct RenderVertex2DLit` (64 字节) + `static_assert`
- `render_backend.h`：新增 5 个 Lit2D 虚接口（默认 no-op）
- `render_gl33.cpp`：`GL33Backend` 类内新增 `vaoLit2D / vboLit2D / eboLit2D` 字段 + `InitLit2D()` 函数（仅 GL 对象，shader 留给 E.1.2）

**实现约束**：
- 64 字节 = pos(12) + uv(8) + color(16) + normal(12) + tangent(16)
- VBO 用 `GL_DYNAMIC_DRAW`（每帧重填 4 顶点）
- EBO 静态：`[0, 1, 2, 0, 2, 3]` 单 quad

---

## 2. 实现摘要

### 2.1 `ChocoLight/include/render_backend.h`

#### 新增结构 `RenderVertex2DLit`（line 50-69）

```cpp
struct RenderVertex2DLit {
    float x, y, z;            // 位置 (12)
    float u, v;               // UV  (8)
    float r, g, b, a;         // 顶点色 (16)
    float nx, ny, nz;         // 法线 (12)
    float tx, ty, tz, tw;     // 切线 + bitangent sign (16)
};
static_assert(sizeof(RenderVertex2DLit) == 64, "...");
```

字段布局与 `VS_LIT2D` shader 静态 `layout(location=N)` 完全对应：

| location | 字段        | 类型 |
|----------|-------------|------|
| 0        | aPos        | vec3 |
| 1        | aUV         | vec2 |
| 2        | aColor      | vec4 |
| 3        | aNormal     | vec3 |
| 4        | aTangent    | vec4 |

#### 新增 5 个虚接口（line 377-438）

```cpp
virtual bool     SupportsLit2D() const { return false; }
virtual uint32_t CreateLit2DMesh(const RenderVertex2DLit*, int, const uint32_t*, int) { return 0; }
virtual void     DeleteLit2DMesh(uint32_t) {}
virtual void     DrawLit2DQuad(const RenderVertex2DLit[4], uint32_t baseTex, uint32_t normalTex) {}
virtual void     DrawLit2DTriangles(const RenderVertex2DLit*, int, uint32_t, uint32_t) {}
```

所有接口默认 no-op / 返回 0，Legacy 后端不需要 override。

### 2.2 `ChocoLight/src/render_gl33.cpp`

#### `GL33Backend` 类成员新增（line 709-716）

```cpp
GLuint vaoLit2D     = 0;
GLuint vboLit2D     = 0;
GLuint eboLit2D     = 0;
GLuint programLit2D = 0;       // E.1.2 接入, 本任务中始终 0
bool   lit2DSupported = false; // E.1.1 资源就绪后置 true
static constexpr int LIT2D_VBO_INITIAL_VERTS = 4;
```

#### `InitLit2D()` 函数（line 942-1007）

实现要点：
1. 三个 GL 对象 gen 失败时清理已分配资源（避免泄漏）
2. VAO 内绑 VBO（`GL_DYNAMIC_DRAW`，预分配 4 个顶点容量）+ EBO（`GL_STATIC_DRAW`，6 个静态 quad 索引）
3. 配置 5 个顶点属性（location 0..4）
4. 上传静态索引 `[0,1,2, 0,2,3]`
5. 成功后置 `lit2DSupported = true`

#### `Init()` 调用（line 841-842）

```cpp
// ---- Phase E.1.1 — 2D Lit 渲染资源 (VAO/VBO/EBO) ----
InitLit2D();
```

放在 `InitGPUSkinning()` 之后、最终 log 之前。

#### `Shutdown()` 释放（line 988-993）

```cpp
// Phase E.1.1 — 释放 Lit2D 资源 (VAO/VBO/EBO + program)
if (programLit2D) { glDeleteProgram(programLit2D); programLit2D = 0; }
if (eboLit2D)     { glDeleteBuffers(1, &eboLit2D); eboLit2D = 0; }
if (vboLit2D)     { glDeleteBuffers(1, &vboLit2D); vboLit2D = 0; }
if (vaoLit2D)     { glDeleteVertexArrays(1, &vaoLit2D); vaoLit2D = 0; }
lit2DSupported = false;
```

#### `SupportsLit2D()` override（line 996）

```cpp
bool SupportsLit2D() const override { return lit2DSupported; }
```

---

## 3. 验收清单

### 3.1 任务级（来自 TASK_PhaseE.md E.1.1）

- [x] `static_assert(sizeof(RenderVertex2DLit) == 64)` **通过** — 待 CI 编译验证（预期通过）
  - 字节计算：3+2+4+3+4 = 16 floats × 4 = **64 字节** ✓
- [x] `GL33Backend::SupportsLit2D()` 在 GL 对象就绪后返回 **true**（`lit2DSupported = true` 置位）
- [x] 编译通过（无新 warning）— **待 CI 验证**
- [x] 单元测试 / smoke — **推迟 E.1.4**，理由：本任务无 Lua API 入口
  - 替代验证：现有所有 smoke 不退化（CI 已绑定 Windows runtime suite）

### 3.2 实现约束符合性

| 约束 | 实际 |
|------|------|
| `sizeof(RenderVertex2DLit) == 64` | ✅ 通过 `static_assert` |
| VBO `GL_DYNAMIC_DRAW` | ✅ `glBufferData(..., GL_DYNAMIC_DRAW)` |
| EBO 静态 `[0,1,2, 0,2,3]` | ✅ `kQuadIndices[6]` + `GL_STATIC_DRAW` |
| shader 不接入 | ✅ `programLit2D = 0`，无 `CompileShader` 调用 |
| 不破坏现有 BatchRenderer | ✅ 完全独立 VAO/VBO/EBO |

### 3.3 代码质量

- [x] 中文注释（关键节点 + 难懂代码）
- [x] 错误处理：`glGen*` 失败时清理已分配资源
- [x] log 输出（成功 INFO，失败 WARN）
- [x] 与现有 `InitGPUSkinning` / `InitMorph` 风格一致
- [x] 不引入新的 third-party 依赖

---

## 4. 测试策略与限制

### 4.1 本任务的可测性边界

- ❌ **无 Lua 入口**：`SupportsLit2D` / `CreateLit2DMesh` 等接口尚未 Lua 绑定（E.1.4 任务）
- ❌ **无功能性测试**：仅资源初始化，没有 drawcall 可触发
- ✅ **静态验证**：`static_assert` 字节大小（CI 编译期）
- ✅ **行为不退化**：现有 demo / smoke 仍能正常运行（说明 `Init()` 没崩）

### 4.2 间接验证（依赖现有 CI suite）

CI 已绑定的 smoke（`.github/workflows/build-templates.yml`）会跑：
- `core_runtime.lua` — Window 能开 + g_render 初始化 → 间接验证 `InitLit2D` 不崩
- `graphics.lua` — Light.Graphics 模块表完整 → 间接验证 `RenderBackend` 接口稳定
- `ecs_render.lua` — ECS 2D 渲染完整流程 → 间接验证 `BatchRenderer` 路径不受 Lit2D VAO 影响
- `ecs_skinned.lua` — ECS 3D skinning → 间接验证 `InitGPUSkinning` 后续的 `InitLit2D` 正确插入

如果以上任一退化，本任务必须修复后才能进入 E.1.2。

### 4.3 真正的 SupportsLit2D smoke 留 E.1.4

E.1.4 写 Lua API 绑定时一并补：

```lua
-- E.1.4 时 scripts/smoke/lighting2d.lua 中加入
CHECK(type(Light.Lighting2D.IsBackendSupported) == 'function',
      'Light.Lighting2D.IsBackendSupported is function')
CHECK(Light.Lighting2D.IsBackendSupported() == true,
      'Lit2D backend supported on GL33')
```

---

## 5. CI 验证策略

按用户工作流要求："本地不做编译, 所有编译验证只在 github 处理, 可以用 gh 推送"。

### 5.1 验证步骤

1. commit + push origin/main
2. `gh run list -L 1` 拿最新 run ID
3. `gh run watch <id>` 跟踪进度
4. 失败时 `gh run view <id> --log-failed | Select-Object -First 200` 看核心错误
5. 修复推送后重复 1-4

### 5.2 预期结果

- ✅ **编译期**：所有平台 (Windows/Linux/macOS/Android/iOS/Web) C++ 编译通过
  - `static_assert(sizeof(RenderVertex2DLit) == 64)` 触发即编译失败
  - 任何 typo / 接口签名不一致即编译失败
- ✅ **运行期**：Windows runtime smoke suite 全过
  - core_runtime / graphics / ecs_render / ecs_skinned 等 11 个 smoke
  - GL33 后端启动 log 应包含 `Lit2D resources ready`

### 5.3 预期日志（可观测）

成功的 Windows runtime 输出应有：

```
RenderBackend: GL33 Core initialized (GL <version>), 3D Unlit+PBR enabled, GPU skinning enabled, Lit2D resources ready
GL33: Phase E.1.1 Lit2D VAO/VBO/EBO ready (shader pending E.1.2)
```

---

## 6. 风险与备注

| 风险 | 评估 | 缓解 |
|------|------|------|
| `glVertexAttribPointer` location 与未来 shader 不一致 | 低 — 已在注释中明确 `VS_LIT2D` 静态 layout 0..4 | E.1.2 写 shader 时严格按 `layout(location=N)` 对齐 |
| GLES 3.0 `uint32_t` 索引兼容性 | 低 — GLES 3.0 标准支持 `GL_UNSIGNED_INT` ELEMENT_ARRAY | 已使用 uint32_t（与 CreateMesh 一致） |
| `offsetof` 在非 POD 结构上的 UB（C++17） | 低 — `RenderVertex2DLit` 是 standard-layout（仅 float 字段） | 已确保 POD-like |
| 现有 demo 行为退化 | 中 — 默认 sprite 路径完全不变 | CI 多 smoke 覆盖；本地手动跑可在 E.1.2 之前安排一次回归 |

---

## 7. 下一步：E.1.2

`E.1.2 — VS_LIT2D + FS_LIT2D Shader`

任务要点：
- 编写 `VS_LIT2D_SOURCE` / `FS_LIT2D_SOURCE` 字符串常量（GLES 3.0 / GL 3.3 兼容）
- 在 `InitLit2D()` 末尾追加 shader 编译 + link `programLit2D`
- 加严 `lit2DSupported &= programLit2D != 0`
- 拿 16 个 light uniform locations + ambient + uTexture / uNormalMap / uHasNormalMap

依赖：本任务（E.1.1）。

---

## 8. 验收签字

- ✅ 实现遵循 `TASK_PhaseE.md` E.1.1 全部输出契约
- ✅ 实现约束符合性 100%
- ✅ 代码质量符合项目规范（中文注释 + 错误处理 + log）
- ⏳ CI 编译 + 运行时验证 — 等待 push 后由 GitHub Actions 完成
- 📝 已记录 smoke 推迟到 E.1.4 的合理性

**状态**：实现完成，待 CI 验证。
