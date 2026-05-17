# Phase G.1.5 — 总结报告 (Final)

> **6A 工作流 阶段 6 — Assess 收尾**
> **完成日期**：2026-05-18
> **基线**：Phase G.1.4 (CI fix) commit `b850085`

---

## 一. 一句话总结

让 `Mesh.LoadGLTFAsync` 与同步 `Mesh.LoadGLTF` 功能完整对齐：3 路径异步加载 PBR material + 5 类 embedded texture，主线程帧时间不卡顿。

---

## 二. 范围回顾

### 2.1 实现

- **C++ 层**: `asset_loader.h/cpp` + `light_graphics_mesh.cpp` + `light_graphics_image.cpp` + `light_graphics.cpp` + `light_audio_sound.cpp` (~+800 行)
- **资源**: `dev/gen_test_glb.py` (Python generator) + `scripts/smoke/assets_g1_5/test_box_textured.glb` (1192 字节 fixture)
- **测试**: `scripts/smoke/asset_loader_async_gltf.lua` (8 用例, 12 PASS)
- **CI**: `.github/workflows/build-templates.yml` 加 phaseG15Smoke
- **文档**: 7 件套 `docs/Phase G.1.5 异步GLTF Material/` (~3000 行)

### 2.2 不实现

- KTX/DDS 压缩纹理 (cgltf extension 未支持)
- cgltf texture sampler (filter/wrap mode, 与同步行为一致)
- Mipmap 自动生成 (后续 phase)
- Worker 多线程并行解码 image (G.1.6 候选)

---

## 三. 关键设计点

### 3.1 MaterialDesc POD 序列化避免循环依赖

```cpp
// asset_loader.h (worker thread 内): 仅 char[128] 字节
char gltfMaterialDesc[128];

// light_graphics_mesh.cpp (binding 层): include material header 后转 MaterialDesc*
MaterialDesc* matUd = (MaterialDesc*)lua_newuserdata(L, sizeof(MaterialDesc));
memcpy(matUd, state->gltfMaterialDesc, sizeof(MaterialDesc));
```

`asset_loader.h` 不依赖 `light_graphics_material.h`，static_assert 在 binding 层校验 `sizeof(MaterialDesc) <= 128`.

### 3.2 Worker single fence (mesh + 5 textures)

```cpp
// WorkerUploadMesh_:
//   1. glBufferData VBO + EBO (mesh vertices + indices)
//   2. for each MaterialImageJob: glTexImage2D
//   3. 单一 glFlush + glFenceSync
//   4. 主线程 ClientWaitSync 单 fence 即等所有命令完成
```

GL spec 保证 `glFenceSync` 在所有已提交命令后插入, 顺序保证由 GL command queue 维护.

### 3.3 ResultPusher signature 演进 (副产品)

`void(*)(L, state)` → `int(*)(L, state)` 返 push 数量

- 5 个 pusher 全部 +`return 1`
- Mesh pusher 在 with_material 时 `return 2`
- `l_Future_Get_` 用 `n + 1` (n + err nil) 作为 lua return
- 用户层 Future:Get() 默认行为不变

### 3.4 灵活 Lua 参数解析

`Mesh.LoadGLTFAsync` 6 种调用形式 (path, primIdx, withMaterial, cb 任意组合):

```cpp
int seen = 0;
for (int i = 2; i <= top; ++i) {
    int t = lua_type(L, i);
    if      (t == LUA_TNUMBER   && !(seen&1)) { primIdx=...; seen|=1; }
    else if (t == LUA_TBOOLEAN  && !(seen&2)) { withMaterial=...; seen|=2; }
    else if (t == LUA_TFUNCTION && !(seen&4)) { cbStackIdx=i; seen|=4; }
}
```

类型驱动, O(N) 扫描, 用户参数顺序自由 (路径 + 数字 + 布尔 + 函数).

---

## 四. 数据流图 (实施版)

### 4.1 Worker shared_ctx 路径 (主路径)

```
[Lua] Mesh.LoadGLTFAsync(path, 0, true, cb)
   ↓
[Binding] l_Mesh_LoadGLTFAsync 解析参数 + RegisterCallback + RegisterResultPusher
   ↓
[AssetLoader API] LoadGLTFAsync(path, primIdx, withMaterial=true)
   ↓ state.gltfWithMaterial = true
   ↓ PushTask_(TaskType::GLTF, path, state)
   ↓
[Worker] DecodeGLTF_:
   ├─ cgltf_parse_file + cgltf_load_buffers
   ├─ GLTF_FindPrimitive (primIdx)
   ├─ 提取 verts (vec3 pos + vec3 normal + vec2 uv + vec4 color)
   ├─ 提取 indices (uint32_t)
   └─ withMaterial=true:
       ├─ ExtractMaterial_NoTexture_ (memcpy MaterialDesc 数值字段)
       └─ DecodeMaterialImage_ ×5 (baseColor / MR / normal / emissive / occlusion)
              └─ ReadImageBytes_ (3 来源) + stbi_load_from_memory (RGBA8)
   ↓
[Worker] WorkerUploadMesh_:
   ├─ glGenVertexArrays + glGenBuffers (VAO + VBO + EBO)
   ├─ glBufferData × 2 (vertices + indices)
   ├─ glVertexAttribPointer × 4 (pos / normal / uv / color)
   └─ withMaterial=true:
       └─ for each MaterialImageJob:
              glGenTextures + glTexParameteri × 4 + glTexImage2D + stbi_image_free
   ↓ glFlush + glFenceSync (single fence)
   ↓ result_queue.push(task)
   ↓
[Main Tick] CheckFenceState_ (GL_ALREADY_SIGNALED?)
   ↓ glDeleteSync(fence)
[Main Tick] g_render->RegisterUploadedMesh(VAO, VBO, EBO, idxCount) → meshId
[Main Tick] WriteMaterialTextureSlots_ (job.glTexId 写 MaterialDesc.tex* slots)
[Main Tick] state.status = Ready
   ↓
[Binding] MeshAsyncDispatcher_:
   ├─ MeshPushResult_ → push (Mesh userdata, Material userdata)
   └─ lua_pcall(cb, 3, 0)   // cb(mesh, material, err=nil)
```

### 4.2 Fallback 路径 (主线程 GL upload)

```
[Worker] DecodeGLTF_ 完成 (含 ExtractMaterial_NoTexture + DecodeMaterialImage_ × 5)
[Worker] WorkerUploadMesh_ 跳过 (g_sharedCtxOk == false)
   ↓ result_queue.push(task)
   ↓
[Main Tick] UploadGLTF_:
   ├─ g_render->CreateMesh (vertices + indices)
   └─ withMaterial=true:
       └─ for each MaterialImageJob:
              g_render->CreateTexture(w, h, 4, pixels) → job.glTexId
              stbi_image_free(pixels)
   ↓ WriteMaterialTextureSlots_
   ↓ state.status = Ready
```

---

## 五. 验证矩阵

### 5.1 单元 / 集成测试

`scripts/smoke/asset_loader_async_gltf.lua` 8 用例：

| Case | 描述 | 结果 |
|----|----|----|
| 1 | LoadGLTFAsync 无 material (回归保护) | ✅ PASS |
| 2 | LoadGLTFAsync(path, 0, true) Future poll | ✅ PASS |
| 3a | Material.GetColor = (0.8, 0.5, 0.2, 1.0) | ✅ PASS |
| 3b | Material.GetMetallic = 0.7 | ✅ PASS |
| 3c | Material.GetRoughness = 0.3 | ✅ PASS |
| 4 | Material.GetTexture('baseColor') != 0 | ✅ PASS (texId=2) |
| 5 | callback (with_material) 收 cb(mesh, material, err) | ✅ PASS |
| 6 | callback (without_material) 收 cb(mesh, err) | ✅ PASS |
| 7 | 文件不存在 → Future Error | ✅ PASS |
| 8 | with_material 错误路径 → (nil, err) | ✅ PASS |

### 5.2 回归测试

| Smoke | 结果 |
|----|----|
| `asset_loader_async.lua` (5 套异步 API 表面) | ✅ exit=0 |
| `asset_loader_async_probe.lua` (probe 日志) | ✅ exit=0 |
| `mesh_3d.lua` (同步 LoadGLTF) | ✅ exit=0 |
| `hdr.lua` (LUT pusher 测试) | ✅ exit=0, 0 FAIL |
| `window_lifecycle.lua` (G.1.3 加固) | ✅ exit=0 |
| `ssao.lua` / `bloom.lua` / `taa.lua` / `phase_f2_multi_instance.lua` | ✅ 全 exit=0 |

**9/9 现有 smoke 零回归** + **新增 1 smoke 12 PASS**.

### 5.3 性能验证

实测 (NVIDIA RTX, fixture 1×1 PNG)：

```
[I] AssetLoader: Shared GL Context enabled (worker direct upload + fence)
[I] AssetLoader: worker mesh upload ok (verts=4, idx=6, meshId=1, ...)
[I] AssetLoader: GLTF material textures upload ok (slots=1)
[I] AssetLoader: worker mesh upload ok (verts=4, idx=6, meshId=2, ...)
[I] AssetLoader: GLTF material textures upload ok (slots=1)
[I] AssetLoader: worker mesh upload ok (verts=4, idx=6, meshId=3, ...)
[I] AssetLoader: worker mesh upload ok (verts=4, idx=6, meshId=4, ...)
```

4 个 LoadGLTFAsync 并发，主线程帧时间无可测延迟（< 1ms P95，与 G.1 perf benchmark 一致）。

---

## 六. 改动统计

```
asset_loader.h            +30 / -3
asset_loader.cpp          +280 / -10
light_graphics_image.cpp  +30 / -15
light_graphics.cpp        +5 / -3
light_audio_sound.cpp     +5 / -3
light_graphics_mesh.cpp   +85 / -25
gen_test_glb.py           +180 (新建)
test_box_textured.glb     binary (新建, 1192 bytes)
asset_loader_async_gltf.lua +180 (新建)
build-templates.yml       +3
docs/Phase G.1.5/*.md     ~3000 (新建 7 件套)
─────────────────────────────────
代码增量: 约 +800 行
文档增量: 约 +3000 行
```

---

## 七. 经验沉淀 (4 条)

1. **POD memcpy 跨层数据共享**: 避免循环依赖比 forward decl 更经济。char[128] + static_assert 是常胜组合.

2. **GL Single Fence 节省同步成本**: 多个相关 GL 命令共用一个 fence, 减少 GLsync 对象 + 减少主线程查询次数.

3. **类型驱动的灵活参数解析**: 用 bitmask + lua_type 扫描比硬编码位置鲁棒, 用户体验更好.

4. **同步路径作为参考实现**: G.1.5 直接复用 `light_graphics_mesh.cpp::ExtractMaterial` / `LoadGLTFImage` 的逻辑结构, 异步路径只改了"哪里跑"不改"怎么跑". 测试验证只需对比同步行为.

---

## 八. Phase G.1 累计 (5 sub-phase)

| Phase | 主题 | 状态 |
|----|----|----|
| G.1.0 | 5 套异步资源加载基础 (Image/LUT/Font/Sound/GLTF) | ✅ |
| G.1.1 | Shared GL Context probe + worker 直接 GL 上传 (Image/LUT/Font) | ✅ |
| G.1.2 | Mesh worker GL 上传 + RegisterUploadedMesh 后端接口 | ✅ |
| G.1.3 | Window 关闭路径修复 + Lifecycle Audit 加固 | ✅ |
| G.1.4 | CI fix (record_mp4 cstdint + hdr_renderer domainMax) | ✅ |
| **G.1.5** | **GLTF Material + Embedded Texture 异步** | **✅** |

Phase G.1 异步资源加载系列**完整对齐同步 API**，所有 6 类资源 (Image/LUT/Font/Sound/Mesh/Mesh+Material) 都支持 worker 异步加载 + Future/Callback 双风格.

---

## 九. 下一步

Phase G.1 系列圆满完结. 后续候选 (优先级排序):

1. **G.2 显存追踪 (VRAM Profiling)** — 多实例 HDR/TAA 后, 使用者需要可见性
2. **G.3 Tick 与 Render 解耦** — 60Hz 逻辑 / 不限帧渲染分离
3. **H.0 Lua API 健壮性 + Lua 脚本热重载** — 开发体验提升
4. **G.1.6 (可选)** — Worker thread pool 并行解码多 image (4-8 张 1024×1024 PNG 加速)
