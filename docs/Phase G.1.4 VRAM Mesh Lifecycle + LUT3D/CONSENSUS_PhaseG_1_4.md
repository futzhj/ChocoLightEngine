# Phase G.1.4 — VRAM Mesh Lifecycle + LUT 3D Tracking (CONSENSUS)

## 1. 需求来源

继 Phase G.1.3 Worker Upload Tracking 后, 剩余 VRAM 跟踪盲点:
- **主线程 `g_render->CreateMesh`** 5 处 caller (Lua 直建 / GLTF 同步 / SkinnedMesh Bake/Update ReCreate / asset_loader 同步 fallback) **未 Track** → 与 worker 路径不对称
- **`DeleteMesh` 无 Untrack** → 资源释放后 VRAM stats 仍累积
- **SkinnedMesh ReCreate** 每帧 Delete+Create, 无配对会让 Track count 单调增长 (每秒 +60)
- **LUT 3D worker upload** (`WorkerUploadLUT_`) 缺 Track hook
- **SkinnedMorphMesh** VBO/EBO/morph delta textures 全部未 Track

## 2. 调研结果

### 2.1 Mesh 资源池布局 (render_gl33.cpp)

| 池 | id 高位 | 入口 | 释放 |
|----|---------|------|------|
| `meshes` (普通 mesh) | 无 | `CreateMesh` / `RegisterUploadedMesh` | `DeleteMesh` (低位路径) |
| `skinnedMeshes` (CPU/GPU skin) | `0x80000000` | `CreateSkinnedMesh` | `DeleteMesh` (高位分支) |
| `skinnedMorphMeshes` (morph) | `0xC0000000` | `CreateSkinnedMorphMesh` | Shutdown 统一 clear (无单 Delete) |
| `litMeshes` (2D Lit) | (独立 id 空间) | `CreateLitMesh` | `DeleteLitMesh` |

### 2.2 主线程 5 处 `g_render->CreateMesh` caller

1. `@e:\jinyiNew\Light\ChocoLight\src\light_graphics_mesh.cpp:176` — `l_CreateMesh` Lua 直建
2. `@e:\jinyiNew\Light\ChocoLight\src\light_graphics_mesh.cpp:600` — GLTF 同步加载 (fallback)
3. `@e:\jinyiNew\Light\ChocoLight\src\light_animation.cpp:3295` — SkinnedMesh Bake ReCreate
4. `@e:\jinyiNew\Light\ChocoLight\src\light_animation.cpp:3476` — SkinnedMesh Update ReCreate (每帧)
5. `@e:\jinyiNew\Light\ChocoLight\src\asset_loader.cpp:1258` — 同步 fallback

→ 全部走 `g_render->CreateMesh`, 在 backend 内部加 Track 即可一次性覆盖.

### 2.3 LUT 3D 字节公式

| 类型 | GL format | bpp | bytes |
|------|-----------|-----|-------|
| HDR LUT | RGB16F | 6 | `size^3 * 6` |
| LDR LUT | RGB8 | 3 | `size^3 * 3` |

size 范围 [4, 64] → bytes 范围 [192 (4³*3), 1572864 (64³*6)]

## 3. 设计方案

### 3.1 核心策略 — backend 内部 Track/Untrack 闭环

**Track 位置**: backend 内部 (`CreateMesh` / `CreateSkinnedMesh` / `CreateSkinnedMorphMesh`)
**Untrack 位置**: backend 内部 (`DeleteMesh`)
**Worker 路径**: caller 已 TrackBytes (G.1.3), `RegisterUploadedMesh` 仅写 bytes 字段不重复 Track
**5 处 caller 改动**: 0 (内部 hook 自动覆盖)

### 3.2 MeshGPU 字段扩展

```cpp
struct MeshGPU {
    // ... 原有字段 ...
    int64_t vboBytes = 0;       // DeleteMesh 时 UntrackBytes 用
    int64_t eboBytes = 0;
    int64_t morphPosBytes = 0;  // skinnedMorphMesh 用
    int64_t morphNrmBytes = 0;
};
```

### 3.3 RegisterUploadedMesh 接口扩展

```cpp
virtual uint32_t RegisterUploadedMesh(
    uint32_t vao, uint32_t vbo, uint32_t ebo, int idxCount,
    int64_t vboBytes = 0, int64_t eboBytes = 0);  // 默认 0 兼容老 backend
```

worker 主线程 Tick fence Ready 后调此函数传 bytes, backend 仅写 m 字段, 不重复 Track.

### 3.4 LUT 3D worker hook

```cpp
const int64_t lutBytes = (int64_t)st.lutSize * st.lutSize * st.lutSize * (isHDR ? 6 : 3);
LT::GpuMem::TrackBytes("LUT 3D", lutBytes);
```

LUT 3D Untrack 留 v1.6 (Lua side 无显式 Delete API).

### 3.5 类目命名约定

| 类目 | format | bytes 公式 |
|------|--------|-----------|
| `Mesh VBO` | BYTES | `vCount * sizeof(RenderVertex3D \| RenderVertex3DSkin)` |
| `Mesh EBO` | BYTES | `iCount * 4` |
| `Morph pos delta` | BYTES | `vCount * morphCount * 12` (RGB32F) |
| `Morph nrm delta` | BYTES | 同上, nrmDeltas 存在时 |
| `LUT 3D` | BYTES | `size^3 * (isHDR ? 6 : 3)` |

## 4. 任务拆分

| ID | 描述 | 文件 | 估时 |
|----|------|------|------|
| T1 | render_backend.h `RegisterUploadedMesh` 加 vboBytes/eboBytes 默认参数 | `include/render_backend.h` | 5 min |
| T2 | render_gl33.cpp MeshGPU 加 4 字节字段 | `src/render_gl33.cpp` | 5 min |
| T3 | CreateMesh 内部 TrackBytes | `src/render_gl33.cpp:9049` | 5 min |
| T4 | RegisterUploadedMesh 接 bytes 存入 m (不 Track) | `src/render_gl33.cpp:9096` | 5 min |
| T5 | DeleteMesh 3 路径 (mesh / skinnedMesh / skinnedMorphMesh) UntrackBytes | `src/render_gl33.cpp:9109` | 15 min |
| T6 | CreateSkinnedMesh + CreateSkinnedMorphMesh Track (含 morph delta) | `src/render_gl33.cpp:9466,9630` | 15 min |
| T7 | Shutdown 3 处 mesh 释放循环加 Untrack | `src/render_gl33.cpp:5067-5099` | 10 min |
| T8 | asset_loader.cpp 调 RegisterUploadedMesh 传 bytes | `src/asset_loader.cpp:1402` | 5 min |
| T9 | WorkerUploadLUT_ 加 TrackBytes | `src/asset_loader.cpp:907` | 5 min |
| T10 | smoke §K (LUT 3D) + §L (Mesh lifecycle round-trip) | `scripts/smoke/gpumem.lua` | 30 min |
| T11 | 6A 4 文档 | `docs/Phase G.1.4*/` | 40 min |
| T12 | git commit + push + CI | | 60 min CI wait |

总: ~3.5 h (实际 ~3 h, 比原估 4.5 h 收敛)

## 5. 验收标准

| 编号 | 验收点 | 验证方法 |
|------|--------|---------|
| A1 | 主线程 Mesh VBO/EBO Track ↑ 配对 ↓ 平衡 (无 leak) | smoke §L.1 Mesh VBO count 5x round-trip 稳定 |
| A2 | DeleteMesh 释放后 Mesh VBO count 回到 0 | smoke §L.2 Reset 后 count = 0 |
| A3 | SkinnedMesh ReCreate 每帧 Delete+Create 平衡 (不单调增长) | smoke §L.1 Mesh VBO count 不抖动 |
| A4 | LUT 3D worker Track 类目存在, format=BYTES, bytes > 0 | smoke §K.2 (实机 demo_lut 加载后) |
| A5 | LUT 3D 单实例 bytes 在 [192, 1572864] 区间 | smoke §K.3 (sanity) |
| A6 | RegisterUploadedMesh 老 caller 不传 bytes 仍兼容 | 编译通过, CI 6/6 |
| A7 | mutex (G.1.3) + Track/Untrack (G.1.4) 共存无 deadlock | smoke §J.1 + §K.1 PushStats 5x 连调 |
| A8 | Shutdown 全清空, 与 GpuMem::Reset 顺序无关 | CI Windows runtime smoke |

## 6. 风险评估

| 风险 | 等级 | 缓解措施 |
|------|------|---------|
| RegisterUploadedMesh 接口签名变更 break API | 🟢 低 | 加默认参数 `=0`, 老 caller 不传零兼容 |
| SkinnedMesh ReCreate 每帧 Track/Untrack 性能 | 🟢 低 | mutex lock < 1us, 60fps 累计 ~0.06 ms/s |
| skinnedMorphMeshes 在 Shutdown 跨 GpuMem::Reset 顺序 | 🟢 低 | Shutdown 和 DeleteMesh 双重 Untrack, GpuMem 自身 UntrackBytes 内部下溢保护 |
| morph delta texture bytes 公式 (RGB32F 12B/像素) | 🟢 低 | `UploadMorphDeltaTexture` 已固定 RGB32F, 公式与现实一致 |
| LUT 3D 无 Untrack (Lua side 缺 Delete API) | 🟡 中 | 留 v1.6 跟踪, 当前 Track-only 仅显示运行峰值 |

## 7. 版本说明

- 基线: Phase G.1.3 commit `1e30223`
- 目标: 一次性闭合 mesh + LUT 3D 全部 VRAM 跟踪
- 后续: v1.5 (sprite frame __gc 配对) / v1.6 (LUT Untrack + Image __gc fix)
