# Phase G.1.3 — VRAM Worker Upload Tracking (mutex + Mesh VBO/EBO + Worker Image) — 6A 合并

> **基线**: G.1 + G.1.1 + G.1.2 已交付 (CI 6/6 PASS, ~22 smoke 子测)
> **范围**: 闭合 G.1 TODO P1 §7 (Mesh VBO/EBO) + worker upload race fix
> **估时**: 2h (Tracker mutex 0.5h + 3 处 worker hook 1h + smoke + 4 件 6A 0.5h)

---

## 1. Align — 项目对齐

### 1.1 需求来源 (G.1 TODO P1 §7)

> #### 7. Mesh VBO/EBO 跟踪 ❌ 未做
> - **难点**: `asset_loader.cpp` worker thread 上传, 需要 **mutex 化 tracker**
> - **改动**: tracker 加 `std::mutex` + 2-3 处 hook 在 mesh upload 路径
> - **估时**: 2h (含 mutex 化)

附带价值: **G.1.2 留下的 ~10% under-report** (worker upload 路径漏跟踪) 一并修.

### 1.2 调研结果 — Worker thread 直调 GL 共 4 处

| 函数 | 行 | 资源 | 备注 |
|------|----|------|------|
| `WorkerUploadImage_` | asset_loader.cpp:726+ | 1 个 RGBA8 texture | G.1.2 留 |
| `WorkerUploadMesh_` (VBO/EBO) | :770-771 | 1 VBO + 1 EBO | TODO §7 |
| `WorkerUploadMesh_` (material) | :842 | N 个 RGBA8 textures | G.1.2 留 |
| `WorkerUploadLUT_` | :917 | 1 个 3D LUT | 留 G.1.4 (R32F 3D 字节算法不同) |

### 1.3 调研结果 — 主线程 `g_render->CreateMesh` 共 6 处

| caller | 行 |
|--------|----|
| `light_graphics_mesh.cpp::l_Mesh_New (sync from verts)` | :176 |
| `light_graphics_mesh.cpp::LoadGLTF_sync` | :600 |
| `light_animation.cpp::l_SkinnedMesh_Bake` (ReCreate) | :3295 |
| `light_animation.cpp::l_SkinnedMesh_Update` (ReCreate) | :3476 |
| `asset_loader.cpp::UploadGLTF_` (主线程 fallback) | :1251 |

**SkinnedMesh ReCreate 问题**: `DeleteMesh` 后 `CreateMesh` (Phase AV/AW 设计), 每帧 update 一次 = Track 累加无 Untrack → 累积 leak. **保守策略**: G.1.3 跳过主线程 `g_render->CreateMesh` 路径, 留给 v1.4 (需要先解决 Mesh GC 路径分散问题).

### 1.4 边界

- **IN**:
  - Tracker mutex 化 (4 API + Reset + PushStats)
  - WorkerUploadImage_ 加 Track (User Image RGBA8) — 闭合 G.1.2 §3.1
  - WorkerUploadMesh_ 加 Track:
    - VBO bytes (Mesh VBO, BYTES format)
    - EBO bytes (Mesh EBO, BYTES format)
    - material textures (Mesh texture RGBA8)
  - smoke §J: mutex API 不挂 + 异步加载场景 (headless 退化为 不挂验证)
  - 6A 4 件 + G.1 TODO P1 §7 标完成
- **OUT**:
  - 主线程 `g_render->CreateMesh` 5 处 Track (留 v1.4 — 需先做 Mesh GC 配对)
  - LUT worker upload (留 G.1.4)
  - Mesh / Sprite / Image GC Untrack (留 v1.4 / G.1.7+ Type Safety 系列)
  - VBO/EBO 在 SkinnedMesh ReCreate 路径 Untrack (同上)

### 1.5 决策

| 问 | 答 |
|---|---|
| mutex 类型 | `std::mutex` (锁全局 items 数组, 简单可靠; 锁粒度 = 整个 API) |
| 是否用 `std::recursive_mutex`? | **不用**. 4 API 不互相调用, 也不通过 lua_State 回调; 无递归风险 |
| Worker 与主线程 race 场景 | Worker `WorkerUploadMesh_` 调 Track 时, 主线程 dispatcher 同时调 `PushStats` (Lua callback) → mutex 必要 |
| LUT worker 跳过理由 | 3D LUT 字节算法是 size^3 * format_bytes, BytesPerPixel 表不支持; 单独加 `Track3D` API 不在本期范围 |
| 主线程 Mesh 跳过理由 | SkinnedMesh 每帧 ReCreate → 无 Untrack 配对会累积 leak; 留 v1.4 同 GC 一并修 |
| Untrack 主线程 Mesh GC 在哪? | 当前**没有** Lua Mesh userdata GC. 留 v1.4 |

---

## 2. Architect — 设计

### 2.1 mutex 化策略

```cpp
namespace {
    constexpr int GPU_MEM_MAX_ITEMS = 64;
    static GpuMemItem s_items[GPU_MEM_MAX_ITEMS];
    static std::mutex s_mutex;        // ★ G.1.3 新增
}

void Track(name, format, w, h) {
    std::lock_guard<std::mutex> lock(s_mutex);   // ★
    // ... 原逻辑不变
}

// Untrack / TrackBytes / UntrackBytes / Reset / PushStats 同上
```

**风险**: 主线程 `PushStats` 在 `lua_pushstring` 等期间持锁; worker 期间也持锁. 但锁粒度 < 100 ns (just 8 个字符串/整数 push), 不影响性能.

### 2.2 Worker hook 模板

```cpp
// WorkerUploadImage_ (line 740 后):
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, st.imgW, st.imgH, 0, ...);
if (glGetError() == GL_NO_ERROR) {
    LT::GpuMem::Track("User Image", "RGBA8", st.imgW, st.imgH);
}

// WorkerUploadMesh_ (VBO/EBO):
glBufferData(GL_ARRAY_BUFFER, vboBytes, ...);
glBufferData(GL_ELEMENT_ARRAY_BUFFER, eboBytes, ...);
// 错误检查后:
LT::GpuMem::TrackBytes("Mesh VBO", (int64_t)st.gltfVertCount * sizeof(RenderVertex3D));
LT::GpuMem::TrackBytes("Mesh EBO", (int64_t)st.gltfIdxCount * sizeof(uint32_t));

// WorkerUploadMesh_ (material texture, 循环内):
glTexImage2D(...);
if (texErr == GL_NO_ERROR) {
    LT::GpuMem::Track("Mesh texture", "RGBA8", job.w, job.h);
}
```

### 2.3 字节计算公式

```
RenderVertex3D = 48 bytes (pos float3 + normal float3 + uv float2 + color float4)
uint32_t idx   = 4 bytes

VBO bytes = vertCount * 48
EBO bytes = idxCount * 4
```

例: 一个 10000-vert / 30000-idx mesh = 480 KB VBO + 120 KB EBO = ~600 KB.

---

## 3. Atomize — 任务拆分

| 任务 | 估时 | 输出 |
|------|------|------|
| T1 GpuMem mutex 化 | 0.3h | 6 个 lock_guard, 1 个 #include <mutex> |
| T2 WorkerUploadImage_ Track | 0.1h | 1 个 Track |
| T3 WorkerUploadMesh_ VBO/EBO Track | 0.2h | 2 个 TrackBytes |
| T4 WorkerUploadMesh_ material texture Track | 0.2h | 1 个 Track 在循环内 |
| T5 smoke §J | 0.4h | mutex 不挂 + format BYTES 在 bpp 表中跳过 |
| T6 6A 4 件 + G.1 TODO P1 §7 标完成 | 0.3h | docs |
| T7 commit + CI 6/6 PASS | 0.5h | GH Actions |

**总计**: ~2h

---

## 4. 验收

| 项 | 验证 |
|----|------|
| `std::mutex` 加入 4 API + Reset + PushStats | grep `lock_guard` 应有 6 处 |
| WorkerUploadImage_ 后 stats 包含 "User Image" (无 headless GL 时仍能调) | smoke §J |
| WorkerUploadMesh_ 后 stats 包含 "Mesh VBO" / "Mesh EBO" / "Mesh texture" | smoke §J |
| BYTES format 在 §H 公式表内被 skip (不验证 bpp) | smoke §H 已有逻辑 |
| 32+ 老 smoke 不回归 | 全套回归 |
| 6/6 CI PASS | GH Actions |

---

## 5. 风险

| 风险 | 等级 | 缓解 |
|------|------|------|
| mutex 锁粒度过粗导致主线程被 worker 阻塞 | 极低 | 锁内 < 100 ns, 远小于一帧 (16 ms) |
| `std::mutex` 在 emscripten/Web 不可用 | 低 | std::mutex 是 C++11 标准, emscripten 支持 (单线程 fallback) |
| Worker hook 在 GL 失败路径不调 Track | 低 | 主动检查 glGetError 后才 Track |
| BYTES format slots > 64 上限 | 极低 | 每个 (vert/idx count) 占一个 slot; 实际 < 20 unique combos |

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
