# Phase AS.3 — cgltf 集成 + .gltf/.glb 加载 — 对齐文档

> **6A 工作流 Stage 1**: Align (Phase AS.3)

---

## 1. 第三方库选型

### 1.1 候选库对比

| 库 | 大小 | 许可 | 集成方式 | 评价 |
|---|---|---|---|---|
| **cgltf** | ~6000 行单文件 | MIT | header-only + 1 个 .c (define impl) | **推荐** |
| tinygltf | ~10000 行 + json + stb | MIT | C++ header-only, 但依赖 json.hpp | 大依赖 |
| 自实现 JSON + glTF | ~2000+ 行 | - | 全自研 | 工作量太大 |

### 1.2 cgltf 选型理由

- 与项目现有 single-header 库模式 (stb_image, stb_truetype, miniaudio) **完全一致**
- C 语言, 无 STL/异常依赖, 跨平台 0 修改
- 同时支持 .gltf (JSON+bin) 和 .glb (二进制单文件)
- 自动处理 buffer accessor 解码 (uint8/uint16/uint32 索引, float/half pos/normal/uv)
- MIT 许可, 商业友好
- 当前 v1.14, 维护活跃

### 1.3 集成步骤

1. 下载 cgltf.h v1.14 到 `ChocoLight/third_party/cgltf.h` (单文件 ~280KB)
2. 创建 `ChocoLight/third_party/cgltf_impl.c`:
   ```c
   #define CGLTF_IMPLEMENTATION
   #include "cgltf.h"
   ```
3. `CMakeLists.txt` 加 `cgltf_impl.c` 到源列表 (与 `stb_impl.c` / `miniaudio_impl.c` 一致)

---

## 2. 范围

### 2.1 In-Scope (本子 Phase 必做)

#### A. `Light.Graphics.Mesh.LoadGLTF(path, [primitive_index])` 新静态方法

```lua
-- 加载 .gltf 或 .glb, 返回第一个 primitive 作为 Mesh
local mesh = Light.Graphics.Mesh.LoadGLTF("models/cube.gltf")
local mesh, err = Light.Graphics.Mesh.LoadGLTF("models/cube.glb")  -- 失败返回 nil + err

-- 加载指定 primitive (一个 mesh 可能有多个 primitive, 不同材质)
local mesh = Light.Graphics.Mesh.LoadGLTF("models/cube.glb", 0)  -- 0-indexed

-- 元数据查询
local count = Light.Graphics.Mesh.GetGLTFMeshCount("models/scene.glb") -> int
```

#### B. 处理逻辑

1. **文件类型识别**: 路径 `.gltf` → JSON+bin; `.glb` → binary
2. **解析**: 调 `cgltf_parse_file()` + `cgltf_load_buffers()`
3. **取第 N 个 primitive** (默认 0): 一个 .gltf 可能有多个 mesh, 每个 mesh 多个 primitive
4. **顶点属性提取**:
   - POSITION (vec3): **必须** 存在, 否则失败
   - NORMAL (vec3): 缺失时 fill (0, 1, 0)
   - TEXCOORD_0 (vec2): 缺失时 fill (0, 0)
   - COLOR_0 (vec4 或 vec3): 缺失时 fill (1, 1, 1, 1); vec3 时 a=1
5. **索引提取**: cgltf 可能是 uint8/uint16/uint32, 统一转 uint32
6. **构造 RenderVertex3D 数组 + 索引 uint32 数组**, 调 `g_render->CreateMesh()`
7. **释放 cgltf 资源**

#### C. 错误处理 (失败返回 nil + err string)

| 错误 | 错误消息 |
|---|---|
| 文件不存在 / 无法打开 | "cannot open file: <path>" |
| 解析失败 (JSON 错或 GLB magic 错) | "parse failed: <cgltf err>" |
| 缓冲区加载失败 | "buffer load failed" |
| primitive_index 越界 | "primitive index N out of range (total M)" |
| 缺失 POSITION 属性 | "primitive has no POSITION attribute" |
| 顶点数为 0 | "primitive has 0 vertices" |
| 无 GL context (headless) | "no render backend" |

#### D. Smoke 测试

由于 CI 没有真实 .gltf 文件 (会增加 repo 大小), smoke 应:
1. 验证 `LoadGLTF` 是 function
2. 验证不存在的路径返回 nil + err
3. 不验证实际加载 (留给手动测试)
4. 可选: 内嵌一个最小 GLB 二进制字符串 (一个三角形, ~200 bytes), 测试解析路径

### 2.2 Out-of-Scope (留 AS.4 或后续)

- **多 primitive 同时加载** (返回多个 Mesh) — 留 AS.3.x 扩展
- **材质属性提取** (baseColorFactor, metallicFactor 等) — 留 AS.4 PBR 时一起做
- **纹理加载** (从 glTF 引用的 .png/.jpg) — 留 AS.4
- **动画 / 蒙皮** — 留更后续
- **Tangent 顶点属性** — 留 AS.4 法线贴图

---

## 3. 关键决策预案 (Q1~Q4)

### Q1 — cgltf.h 如何获取?

**方案 A** (推荐): **用户 (或脚本) 下载 cgltf.h v1.14 到 `ChocoLight/third_party/cgltf.h`**, 然后我做后续  
**方案 B**: 我用 `Invoke-WebRequest` 下载 (需要用户网络审批)  
**方案 C**: vendor 进 git (固定版本, 不易升级)

A 推荐: 最稳, 用户可手动 review 文件来源。或者 B 自动化 (我会请求审批)。

### Q2 — LoadGLTF 是否暴露 primitive 索引?

**方案 A** (推荐): `LoadGLTF(path, [primitive_index=0])` — 默认 0, 用户可显式指定  
**方案 B**: 只加载第一个 primitive  
**方案 C**: 加载全部 primitive 并返回 array

A 推荐: 灵活而不复杂; AS.3.x 可加 `LoadAllPrimitives` 返回 table。

### Q3 — 缺失顶点属性的填充策略

**方案 A** (推荐): 自动填充默认值 (normal=(0,1,0), uv=(0,0), color=(1,1,1,1))  
**方案 B**: 失败 (要求 .gltf 提供完整属性)  
**方案 C**: 让 Lua 端选择 (传 options 表)

A 推荐: 与多数游戏引擎行为一致 (用户体验好), 默认值合理。

### Q4 — Mesh 元数据查询是否暴露?

**方案 A** (推荐): 暴露 `Light.Graphics.Mesh.GetGLTFMeshCount(path) -> int` (查询不加载)  
**方案 B**: 不暴露, 用户必须遍历 LoadGLTF 索引  
**方案 C**: 加更全面的 `GetGLTFInfo(path)` 返回 table

A 推荐: 单一查询足够多数场景, AS.3.x 可扩展更全面 info。

---

## 4. 工作量估算

| 子模块 | C++ 行 | smoke 行 | 备注 |
|---|---|---|---|
| cgltf.h + cgltf_impl.c | (外部) ~6000+1 | - | 仅 1 行 .c |
| CMakeLists.txt 加源 | 1 | - | |
| `LoadGLTF` 实现 (在 light_graphics_mesh.cpp 内) | ~250 | ~80 | 含 vertex 提取/索引转换 |
| `GetGLTFMeshCount` 实现 | ~50 | - | |
| 错误处理 + 边界 | ~80 | - | |
| smoke 加 LoadGLTF 路径 | - | ~50 | |
| 文档 | - | - | |
| **合计** | **~380** (我方代码) | ~80 | + ~280KB cgltf.h |

预期 ~5h, 1 commit, 6 平台 CI 验证。

---

## 5. 关键约束

- **不破坏现有 Light.Graphics.Mesh API** — LoadGLTF 是新静态方法, 不动 New/Draw 等
- **不破坏现有 RenderVertex3D** — LoadGLTF 内部直接构造同样格式
- **headless 友好** — 无 GL 时 LoadGLTF 在到达 CreateMesh 前就失败 (返回 nil + err)
- **跨平台 stable** — cgltf 是纯 C, 各平台编译无差异

---

## 6. 关键决策待确认

请用户确认 Q1~Q4 + cgltf.h 获取方式:

1. **全部推荐 (Q1~Q4 全 A) + 我自动下载 cgltf.h** — 自动化, ~5h
2. **全部推荐 + 用户手动下载 cgltf.h** — 你下载到 `ChocoLight/third_party/cgltf.h`, 我做后续
3. **不引入 cgltf, 跳到 AS.4** — Lua 用户自己解析 .gltf (不实际)
4. **分别详细选择**
