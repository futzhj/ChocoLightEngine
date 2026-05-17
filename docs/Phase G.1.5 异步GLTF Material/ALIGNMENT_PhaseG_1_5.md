# Phase G.1.5 — 异步 glTF Material + Embedded Texture 对齐文档

> **6A 工作流 阶段 1 — Align (对齐)**
> **创建日期**：2026-05-18
> **依赖**：Phase G.1.0/1.1/1.2/1.3 已交付 + CI 6/6 全绿

---

## 一. 原始需求

> "让 `Mesh.LoadGLTFAsync` 与同步 `Mesh.LoadGLTF` 功能对齐 (含 material + embedded image), 让游戏在加载模型时主线程不卡顿"

来源：`@e:/jinyiNew/Light/docs/Phase G.1 异步资源加载/TODO_PhaseG_1.md` T2 + `@e:/jinyiNew/Light/docs/HANDOFF_REMAINING_TASKS.md` 异步资源加载 §1.

---

## 二. 项目上下文分析

### 2.1 现有同步 `Mesh.LoadGLTF` 完整能力

`@e:/jinyiNew/Light/ChocoLight/src/light_graphics_mesh.cpp:512-598`:

```lua
local mesh = Mesh.LoadGLTF(path, primIdx)                       -- 仅 mesh
local mesh, material = Mesh.LoadGLTF(path, primIdx, true)       -- with material
```

3 个核心 helper:

| Helper | 行号 | 职责 |
|----|----|----|
| `GLTF_ParseAndLoad` | 196-212 | `cgltf_parse_file` + `cgltf_load_buffers` |
| `GLTF_ExtractPrimitive` | 237-352 | 提取 POSITION/NORMAL/TEXCOORD_0/COLOR_0 + indices |
| `LoadGLTFImage` | 375-443 | **3 种来源**: buffer_view (GLB) / data: URI (base64) / 相对文件路径 |
| `ExtractMaterial` | 446-508 | 提取 PBR 材质 + 5 textures + alphaMode/doubleSided |

`MaterialDesc` (`light_graphics_material.cpp` 已成熟) 全字段：

```cpp
struct MaterialDesc {
    int      mode;                    // 0=Unlit, 1=PBR
    float    color[4];                // base color factor
    float    metallic, roughness;
    float    emissive[3];
    float    normalScale, occlusionStrength;
    int      alphaMode;               // 0=OPAQUE, 1=BLEND, 2=MASK
    float    alphaCutoff;
    int      doubleSided;
    uint32_t texBaseColor;            // 5 个 GL texture id (5 slots)
    uint32_t texMetallicRoughness;
    uint32_t texNormal;
    uint32_t texEmissive;
    uint32_t texOcclusion;
};
```

### 2.2 现有异步 `Mesh.LoadGLTFAsync` 缺失

`@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:308-474` (`DecodeGLTF_`)：

- ✅ 顶点 + 索引完整提取
- ❌ **完全丢弃 material**
- ❌ **完全丢弃 embedded image**

`@e:/jinyiNew/Light/ChocoLight/src/light_graphics_mesh.cpp:617-686` (`l_Mesh_LoadGLTFAsync`)：

- 仅 2 参数 (path, primIdx) 或 (path, cb) 风格
- Future:Get() 仅返 Mesh，无 Material

### 2.3 已有的 worker / 主线程基础设施 (G.1.0/1.1/1.2)

可直接复用：

| 设施 | 位置 | 用途 |
|----|----|----|
| `WorkerUploadImage_` | `asset_loader.cpp:486` | worker 直接 glTexImage2D + fence |
| `WorkerUploadMesh_` | `asset_loader.cpp:529` | worker 直接 VBO/EBO + fence |
| `RegisterUploadedMesh` | `render_backend.h` | 主线程 backend 注册已上传 mesh |
| `g_sharedCtxOk` probe | `asset_loader.cpp:142` | 决定走 worker 上传还是主线程 fallback |
| `g_render->CreateTexture` | `render_backend.h` | 主线程 fallback texture upload |
| `stbi_load_from_memory` | 已被同步路径用 | 解码 GLB embedded image (worker 线程安全) |

### 2.4 现有约束

- **single worker thread**：`asset_loader.cpp:127 std::thread g_worker` — 单工作线程，所有任务串行
- **GL 调用限制**：worker 仅在 `g_sharedCtxOk == true` 路径下做 GL 调用
- **cgltf_data 生命周期**：parse 后 buffer_view 引用此结构，free 之前 image data 都有效

---

## 三. 边界与范围确认

### 3.1 IS 范围

1. ✅ `Mesh.LoadGLTFAsync(path, primIdx, withMaterial)` 第三参数 (默认 false 向后兼容)
2. ✅ Future poll 风格 + Callback 风格都支持 with_material
3. ✅ 5 类 texture 全支持 (baseColor / metallicRoughness / normal / emissive / occlusion)
4. ✅ 3 类 image 来源 (GLB embedded / data: URI / 相对文件)
5. ✅ MaterialDesc 全数值字段完整透传
6. ✅ Worker 直接 GL 上传 textures (shared_ctx 路径)
7. ✅ Fallback 主线程上传 (无 shared_ctx)
8. ✅ Image 解码失败装饰性兜底 (mesh 仍 ready, slot=0 + log warn)
9. ✅ smoke 覆盖 with_material 路径 + 失败 image 兜底

### 3.2 IS NOT 范围

1. ❌ **不**改 `Mesh.LoadGLTF` 同步 API (它已是参考实现)
2. ❌ **不**支持 cgltf texture sampler (filter/wrap mode) — 与同步行为一致 (默认 LINEAR + CLAMP)
3. ❌ **不**支持 KTX/DDS 压缩纹理 (cgltf 默认走 PNG/JPG/etc)
4. ❌ **不**做 mipmap 自动生成 (与同步行为一致)
5. ❌ **不**做 embedded image 多线程并发解码 (单 worker 线程，串行解码; 后续 G.1.6 候选)
6. ❌ **不**改 MaterialDesc 结构体布局 (向后兼容)
7. ❌ **不**做 sRGB / color space conversion (现有 backend 默认行为不变)

---

## 四. 智能决策点

按用户全局规则"主动决策、少询问"，以下决策**已基于现有项目代码与同类工程惯例自主决定**，仅在 §五列必须确认事项。

### 4.1 决策清单（全部自动决策）

| # | 决策点 | 选择 | 依据 |
|----|----|----|----|
| 1 | embedded image 解码并发 | **串行 (单 worker)** | 现有 worker 单线程, 改并发要重构 task queue + 锁; 主线程不阻塞已是首要目标 |
| 2 | 多 texture 上传策略 | **shared_ctx 路径 single fence; fallback 主线程串行 Create** | 与 `WorkerUploadMesh_` 一致, 减少 fence 数量 |
| 3 | image 解码失败处理 | **slot=0 + log warn + mesh 仍 ready** | 与同步 `LoadGLTFImage` 失败行为一致 (line 437 失败返 0) |
| 4 | API 签名 | **第三参数 withMaterial 默认 false** | 同步 `LoadGLTF` 已是此签名 (line 515), 完全对齐 |
| 5 | Future:Get() 返回值 | **withMaterial=false 返 1 值; =true 返 2 值 (mesh, material)** | 与同步路径返回值数量一致 |
| 6 | callback 风格签名 | **`cb(mesh, material, err)`** (with_material) **/ `cb(mesh, err)`** (无) | 保持现有 cb 签名风格, 多一个 material 参数 |
| 7 | texture sampler | **忽略 cgltf sampler, 用 backend 默认** | 与同步行为一致, 保持简单 |
| 8 | cgltf_data 生命周期 | **保留到主线程 Tick 完成 material 写入后 free** | 现有 G.1.0 已是此模式, image data 引用 buffer_view 需此对象存活 |
| 9 | FutureState 扩展方式 | **加 std::vector<MaterialImageJob> 数组 + MaterialDesc 字段** | 平铺设计 (KISS, 与现有 image/lut/font/sound 字段风格一致) |
| 10 | worker → 主线程通信 | **复用现有 single fence + result_queue 机制** | 保留 G.1.0/1.1/1.2 架构不变 |
| 11 | MaterialDesc 数值字段 | **worker 完整提取 (memcpy POD), 仅 texture id slot 主线程改写** | 数值字段无 GL 依赖, worker 安全 |
| 12 | smoke 资源 | **新增 GLB 测试文件 (含 base color texture)** | 与同步 LoadGLTF 测试同套路径; 用 stbi 生成 1x1 testpng base64 inline |
| 13 | 错误传播 | **image 失败仅 log, mesh 失败立即 Error 整体** | mesh 是核心数据, image 是装饰 |
| 14 | TextureUserdata 共享 | **不创建 Image userdata, 直接传 GL texId** | 与同步 `ExtractMaterial` 行为一致 (无 Image userdata) |
| 15 | Worker 内 stbi 设置 | **stbi_set_flip_vertically_on_load_thread(0)** | 与现有 worker 行为一致, 避免 Y 翻转 |

### 4.2 性能预期

基于 sample (1080p, 5 张 1024×1024 PNG textures, 10K 顶点 mesh)：

| 路径 | 主线程帧时间影响 | 总加载时间 |
|----|----|----|
| **同步 LoadGLTF (with_material)** | ~50-80ms 主线程卡顿 | 50-80ms |
| **G.1.5 异步 (shared_ctx 路径)** | < 0.1ms (仅 RegisterUploadedMesh + 5 RegisterUploadedTexture) | 50-80ms (worker) + 0 帧延迟 |
| **G.1.5 异步 (fallback 路径)** | 主线程一帧 5 × CreateTexture ≈ 5-10ms | 50-80ms (worker decode) + 1 帧上传 |

shared_ctx 路径目标：**P95 主线程帧时间 < 1ms (with_material)**, 与 G.1 perf benchmark 数据一致。

---

## 五. 必须用户确认的关键决策

### Q1. RegisterUploadedTexture API 是否新增

`render_backend.h` 当前有 `RegisterUploadedMesh`, **没有** `RegisterUploadedTexture`。

worker 直接 `glGenTextures + glTexImage2D` 后, 主线程 Tick 拿 fence 完成时:

- **选项 A**: 不新增 backend API, worker 上传后产生的 GL texture id 就是最终 id (因为 backend 内部 textures 没有像 meshes 那样的 std::map<id, MeshState>; CreateTexture 直接返 GLuint 即 id)
- **选项 B**: 新增 `RegisterUploadedTexture(GLuint texId)` 接口对称 (虽然内部为 no-op)

**推荐 A** (更简单, 无 backend 改动): 让 worker 写入的 GL texId 直接作为 MaterialDesc.texXxx slot 值。

### Q2. CI smoke 资源准备

- **选项 A**: smoke 内嵌 base64 GLB (~5KB) 作为 fixture, 不依赖外部文件
- **选项 B**: 在 `scripts/smoke/assets_g1_5/` 放真实 .glb 文件 (需 git 入仓)

**推荐 A** (CI 友好, 不增加 repo 体积): smoke 内 `string.char(0x67, 0x6c, 0x54, 0x46, ...)` 构造最小 GLB header + 1x1 PNG embedded image。

### Q3. 异步加载失败时, 主线程 callback 是否抛 Lua error？

现有 G.1 callback 路径在 `MeshAsyncDispatcher_` 内 `lua_pcall`, error 仅 log warn 不抛出。

**推荐保持现有行为** (G.1 系列一致), with_material 不变。

---

## 六. 待用户回答

| 问题 | 选项 |
|----|----|
| Q1 RegisterUploadedTexture | A. 不新增 (推荐) / B. 新增空接口 |
| Q2 smoke 资源 | A. 内嵌 base64 (推荐) / B. 真实 .glb 入仓 |
| Q3 callback 错误 | A. log warn 不抛 (推荐, 现有行为) / B. 抛 Lua error |

---

## 七. 下一步

用户确认 Q1/Q2/Q3 后 → 进入 §五 智能决策的最终修正 → 生成 `CONSENSUS_PhaseG_1_5.md` (明确范围 + 验收标准 + 技术方案锁定)。
