# Phase G.1.5 — 共识文档 (Consensus)

> **6A 工作流 阶段 1 — Align 收尾**
> **创建日期**：2026-05-18
> **基于**：`ALIGNMENT_PhaseG_1_5.md` + 用户决策 Q1=A / Q2=B / Q3=A

---

## 一. 锁定的需求描述

让 `Mesh.LoadGLTFAsync` 与同步 `Mesh.LoadGLTF` 功能完全对齐，支持：

1. `withMaterial` 参数返回 (Mesh, Material) 双值
2. 5 类 PBR texture 异步加载 (baseColor / metallicRoughness / normal / emissive / occlusion)
3. 3 类 image 来源 (GLB embedded buffer_view / data: URI base64 / 相对文件路径)
4. 2 路径上传 (worker shared_ctx 直接 GL / 主线程 fallback)
5. Image 解码失败装饰性兜底 (slot=0, mesh 仍 ready)

---

## 二. 验收标准

### 2.1 功能验收

- [ ] **API 兼容性**: `Mesh.LoadGLTFAsync(path, primIdx, withMaterial)` 第三参数默认 false，1-2 参数旧调用零回归
- [ ] **Future poll 风格**: `Future:Get()` 在 `withMaterial=true` 时 push (mesh, material) 双值
- [ ] **Callback 风格**: `cb(mesh, material, err)` (with material) / `cb(mesh, err)` (无)
- [ ] **5 texture slots**: 解析后 MaterialDesc 5 字段值与同步 `LoadGLTF` 字段一致
- [ ] **3 image 来源**: GLB embedded / data:URI / 文件路径都加载成功
- [ ] **数值字段透传**: color/metallic/roughness/emissive/normalScale/occlusionStrength/alphaMode/alphaCutoff/doubleSided/mode 与同步路径一致
- [ ] **失败兜底**: image 解码失败 → slot=0 + log warn + mesh 仍 ready
- [ ] **mesh 失败传播**: 顶点/索引解析失败 → Future Error 整体失败
- [ ] **Worker shared_ctx 路径**: P95 主线程帧时间 < 1ms (与 G.1 perf benchmark 一致)
- [ ] **Fallback 主线程路径**: 一帧 5 textures upload < 10ms

### 2.2 测试验收

- [ ] **smoke 通过**: `scripts/smoke/asset_loader_async_gltf.lua` 运行 exit=0 + 0 FAIL
  - 用例: with_material + 5 texture slots + 数值字段全 + image 失败兜底 + 错误路径
- [ ] **真实 .glb fixture**: `scripts/smoke/assets_g1_5/test_box_textured.glb` 入仓 (~1-2 KB)
  - 含 1 mesh (4 顶点 quad) + 1×1 RGBA PNG embedded baseColor
  - PBR material with baseColorFactor 设为非默认值便于校验
- [ ] **CI 6/6 全平台 success** (沿用 G.1 验证矩阵)

### 2.3 性能验收

| 指标 | 目标 |
|----|----|
| Worker shared_ctx 路径主线程 P95 帧时间 | < 1ms |
| Worker fallback 路径单帧主线程 5 texture upload | < 10ms |
| Worker 串行解码 5 张 1024×1024 PNG | 50-100ms (与同步路径一致) |
| 内存峰值额外开销 | < 25 MB (5 张 1024×1024 RGBA8 中间 buffer) |

---

## 三. 技术实现方案

### 3.1 数据结构扩展 — `FutureState`

`@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h:99-115`:

新增 material 相关字段：

```cpp
// ---- GLTF Material (Phase G.1.5) ----
// withMaterial=true 时由 worker 填充, 主线程 Tick 写入 texture slot 值
struct MaterialImageJob {
    int      slotIdx;         // 0=baseColor, 1=MR, 2=normal, 3=emissive, 4=occlusion
    int      w, h;            // 解码后尺寸
    uint8_t* pixels;          // worker malloc (stbi RGBA), shared_ctx 路径 worker 释放;
                              // fallback 路径主线程释放
    uint32_t glTexId;         // shared_ctx 路径 worker 写; fallback 路径主线程写
};
bool                          gltfWithMaterial = false;  // Lua API 第三参数
char                          gltfMaterialDesc[128];      // POD 序列化的 MaterialDesc
                                                           // (sizeof(MaterialDesc) ≈ 80; 留余量)
std::vector<MaterialImageJob> gltfMaterialImages;          // 0-5 个 (按 cgltf material 实际填充)
```

设计要点：
- `MaterialDesc` POD 结构通过 char[128] 序列化 (避免拉 light_graphics_material.h 进 asset_loader.h)
- `MaterialImageJob` 数组保存 5 类 texture 中 cgltf 实际指定的那些
- `slotIdx` 用整数枚举对应 5 个 MaterialDesc 字段 (`texBaseColor` 等)

### 3.2 Worker 路径扩展

#### 3.2.1 `DecodeGLTF_` (`asset_loader.cpp:308-474`)

新增逻辑（在 mesh 解析完成 + cgltf_data 仍存活时）：

```cpp
if (state.gltfWithMaterial) {
    const cgltf_primitive* prim = ...;   // 已找到的 primitive
    MaterialDesc md = {};
    ExtractMaterial_NoTexture(md, prim->material);   // 仅提取数值字段, texture id=0

    memcpy(state.gltfMaterialDesc, &md, sizeof(MaterialDesc));

    // 解码 5 类 texture image (任一失败 slot=0 + log warn, 不影响 mesh)
    DecodeMaterialImage_(prim->material->pbr_metallic_roughness.base_color_texture,
                          0, state, gltfDir);
    // ... metallicRoughness / normal / emissive / occlusion 同理
}
```

`DecodeMaterialImage_` 是新 helper, 复用同步 `LoadGLTFImage` 的 3 来源逻辑:

```cpp
static void DecodeMaterialImage_(const cgltf_texture_view& view, int slotIdx,
                                  FutureState& st, const std::string& gltfDir) {
    if (!view.texture || !view.texture->image) return;
    const cgltf_image* img = view.texture->image;

    // 3 来源 (与同步 LoadGLTFImage 一致): buffer_view / data: URI / 文件路径
    std::vector<uint8_t> imgBytes;
    if (!ReadImageBytes_(img, gltfDir, imgBytes)) {
        CC::Log(CC::LOG_WARN, "AssetLoader: GLTF material image %d source unreachable", slotIdx);
        return;   // slot 保持 0
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory(imgBytes.data(), (int)imgBytes.size(),
                                                   &w, &h, &ch, 4);
    if (!pixels) {
        CC::Log(CC::LOG_WARN, "AssetLoader: GLTF material image %d stbi decode failed: %s",
                slotIdx, stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return;
    }

    MaterialImageJob job{};
    job.slotIdx = slotIdx;
    job.w = w; job.h = h;
    job.pixels = pixels;
    st.gltfMaterialImages.push_back(job);
}
```

#### 3.2.2 `WorkerUploadMesh_` (`asset_loader.cpp:529-604`)

mesh upload 完成后, 串行上传 5 textures:

```cpp
// 现有 mesh upload 完成 (VAO/VBO/EBO + glFlush)
...

// Phase G.1.5 — material textures 串行 GL 上传 (fence 共用 mesh fence)
for (auto& job : st.gltfMaterialImages) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) { /* log warn, slot 保持 0 */ continue; }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, job.w, job.h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, job.pixels);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        continue;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    job.glTexId = (uint32_t)tex;
    stbi_image_free(job.pixels);
    job.pixels = nullptr;
}
glFlush();
GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);   // 单 fence 等所有
```

### 3.3 主线程 Tick 扩展

#### 3.3.1 Fence Ready 路径 (`asset_loader.cpp:1018-1051`)

GLTF 分支已有 `RegisterUploadedMesh`, 新增 material 写入：

```cpp
if (task.type == TaskType::GLTF) {
    // ... 现有 RegisterUploadedMesh ...
    if (st->gltfWithMaterial) {
        WriteMaterialTextureSlots_(*st);   // 把 job.glTexId 写入 MaterialDesc 5 slots
    }
}
```

#### 3.3.2 Fallback 路径 (`asset_loader.cpp:910-941` `UploadGLTF_`)

```cpp
// 现有 g_render->CreateMesh 完成
...

if (st.gltfWithMaterial) {
    for (auto& job : st.gltfMaterialImages) {
        if (job.pixels) {   // worker 解码成功且未上传 (fallback 路径)
            uint32_t texId = g_render->CreateTexture(job.w, job.h, 4, job.pixels);
            if (texId) job.glTexId = texId;
            stbi_image_free(job.pixels);
            job.pixels = nullptr;
        }
    }
    WriteMaterialTextureSlots_(st);
}
```

### 3.4 Lua API 扩展

#### 3.4.1 `LoadGLTFAsync` 签名

`@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h:237`:

```cpp
// 加可选参数 withMaterial
std::shared_ptr<FutureState> LoadGLTFAsync(const char* path, int primIdx,
                                             bool withMaterial = false);
```

#### 3.4.2 `l_Mesh_LoadGLTFAsync` (`light_graphics_mesh.cpp:655`)

参数解析 (灵活签名兼容):

```lua
-- 支持的调用形式:
Mesh.LoadGLTFAsync(path)                          -- mesh only
Mesh.LoadGLTFAsync(path, cb)                      -- mesh + cb(mesh, err)
Mesh.LoadGLTFAsync(path, primIdx)                 -- mesh only with primIdx
Mesh.LoadGLTFAsync(path, primIdx, cb)             -- mesh + cb(mesh, err)
Mesh.LoadGLTFAsync(path, primIdx, true)           -- mesh + material (Future poll)
Mesh.LoadGLTFAsync(path, primIdx, true, cb)       -- mesh + material + cb(mesh, mat, err)
Mesh.LoadGLTFAsync(path, primIdx, false, cb)      -- mesh only + cb(mesh, err) (显式 false)
```

#### 3.4.3 `MeshPushResult_` 扩展

withMaterial=true 时 push 2 个值 (mesh, material), 否则 1 个值。

#### 3.4.4 `MeshAsyncDispatcher_` 扩展

withMaterial=true 时 callback 收 3 参 (mesh, material, err), 否则 2 参 (mesh, err)。

### 3.5 Smoke + Fixture

#### 3.5.1 Fixture: `scripts/smoke/assets_g1_5/test_box_textured.glb`

最小 GLB (~1.2 KB)：

| 内容 | 字节 |
|----|----|
| GLB Header (magic + version + length) | 12 |
| JSON Chunk header + JSON content | ~700 |
| BIN Chunk header | 8 |
| 4 vec3 positions (quad) | 48 |
| 4 vec2 UVs | 32 |
| 6 uint16 indices | 12 + 4 padding |
| 1×1 RGBA PNG embedded image | ~70 |
| **总计** | **~900 - 1200 字节** |

PBR material 设置：
- `baseColorFactor`: [0.8, 0.5, 0.2, 1.0] (橙色，便于 smoke 校验数值)
- `metallicFactor`: 0.7
- `roughnessFactor`: 0.3
- `baseColorTexture`: 1×1 红色 PNG

生成方式：用 Python `dev/gen_test_glb.py` 一次性脚本生成 (脚本入 repo, .glb 也入 repo)。

#### 3.5.2 Smoke: `scripts/smoke/asset_loader_async_gltf.lua`

测试用例：

| 用例 | 期望 |
|----|----|
| `LoadGLTFAsync(path, 0)` (无 material) | Future:Get() 返 mesh 单值 |
| `LoadGLTFAsync(path, 0, true)` (with material) | Future:Get() 返 (mesh, material) |
| material 数值字段断言 | baseColorFactor=[0.8,0.5,0.2,1.0], metallic=0.7, roughness=0.3 |
| material texture slot 断言 | texBaseColor != 0 (有 1×1 PNG) |
| `LoadGLTFAsync` callback 风格 with material | cb 收 (mesh, material, nil) |
| 错误路径: 文件不存在 | Future Error + errMsg 含 "cgltf_parse_file err" |

---

## 四. 任务边界限制

- 不改动 `Mesh.LoadGLTF` 同步 API
- 不改动 `MaterialDesc` 结构体布局
- 不引入新 backend 接口 (Q1=A 决策)
- 不引入新依赖 (复用 cgltf/stb_image)

---

## 五. 不确定性已解决

- ✅ Q1: worker GL texId 直接作 MaterialDesc.texXxx slot 值，无需 RegisterUploadedTexture
- ✅ Q2: 真实 .glb fixture 入仓 (~1.2 KB)，配套 Python generator
- ✅ Q3: callback 错误 log warn 不抛 Lua error (与现有 G.1 一致)

---

## 六. 关键假设已确认

1. cgltf_data 在主线程 Tick free (非 worker), 因为 image data 引用 buffer_view → 现有 G.1.0 已是此模式
2. shared_ctx 路径 worker 串行 GL 上传 (mesh + 5 textures) 单 fence 是正确的, 因为 GL command queue 顺序执行
3. fallback 路径主线程一帧上传 5 textures 不超过 10ms 在低端 GPU 仍可接受
4. 1×1 PNG fixture 足以验证 image upload 路径，多分辨率不在范围

---

## 七. 项目特性规范已对齐

- 错误处理风格：`nil + err` (Future:Get) / `cb(..., err)` (callback) — 与 G.1 一致
- 日志风格：`CC::Log(CC::LOG_WARN/INFO, ...)` — 与现有 asset_loader 一致
- 命名风格：`CamelCase` 函数 + `g_xxx` 全局 — 与项目一致
- POD MaterialDesc 序列化：`memcpy` + `char[128]` buffer — 避免 header 依赖
- 失败兜底：装饰性资源失败不中断主流程 — 与同步 LoadGLTF 一致

---

## 八. 下一步

进入 **6A 阶段 2 — Architect**，生成 `DESIGN_PhaseG_1_5.md`：
- 整体架构图 (mermaid)
- worker 数据流图 (mesh + 5 image jobs → fence → main Tick)
- 模块依赖关系
- 异常处理策略
- 接口契约
