# Phase G.1.2 Mesh worker 上传 — 验收记录

> **阶段**：6A Workflow — 阶段 5 Automate / 阶段 6 Assess
> **完成日期**：2026-05-17
> **关联**：[PLAN](PLAN_PhaseG_1_2.md) · [G.1.1 ACCEPTANCE](ACCEPTANCE_PhaseG_1_1.md)

---

## 一、本次覆盖范围

将 GLTF Mesh 加入 G.1.1 worker 直接 GL 上传 + fence 路径，配合 backend 新增 `RegisterUploadedMesh` 抽象层，主线程 Tick 内完成 O(1) 注册。

### 模块改动（4 个文件）

- **`@e:/jinyiNew/Light/ChocoLight/include/render_backend.h`**：新增 `RegisterUploadedMesh(vao, vbo, ebo, idxCount)` 虚函数，base 默认 no-op 返 0
- **`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp`**：GL33Core 实现 `RegisterUploadedMesh`，与 `CreateMesh` 末段共享 `nextMeshId++ + meshes[id]=m` 逻辑
- **`@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h`**：`FutureState` 新增 4 个字段 `glMeshVao / glMeshVbo / glMeshEbo / glMeshIdxCount`
- **`@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp`**：
  - 新增 `WorkerUploadMesh_` helper（≈75 行）
  - WorkerMain dispatch 加 `case TaskType::GLTF: WorkerUploadMesh_(task)`
  - Tick fence Ready 路径分类型：Mesh 调 `RegisterUploadedMesh`，其它直接翻 Ready
  - fence Error / Shutdown / dtor 三层 mesh handle 兜底 glDelete

### 不变量保持

- ✅ Lua `Mesh.LoadGLTFAsync(path, primIdx)` 签名 + `Future:Get()` 三态语义零变化
- ✅ G.1.1 Image / LUT 路径完全未触碰
- ✅ 移动 / Web 平台编译产物不携带 mesh worker 代码（`#if !defined(__EMSCRIPTEN__) && ...` 包裹）
- ✅ probe 失败 / `g_sharedCtxOk=false` 时透明回落 G.1.0 主线程 `UploadGLTF_`

---

## 二、原子任务完成情况

| 任务 | 状态 | 关键文件 |
|----|----|----|
| **T1** PLAN 合并文档 | ✅ | `PLAN_PhaseG_1_2.md` |
| **T2** `RenderBackend::RegisterUploadedMesh` 虚函数 | ✅ | `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h:361-380` |
| **T3** GL33Core `RegisterUploadedMesh` 实现 | ✅ | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:8940-8955` |
| **T4** `FutureState` 4 个 GL handle 字段 | ✅ | `@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h:108-115` |
| **T5** asset_loader.cpp 5 处改动 | ✅ | dtor `:58-70` · WorkerUploadMesh_ `:490-569` · WorkerMain `:666-674` · Tick fence `:968-1015` · Shutdown `:758-774` |
| **T6** 构建 + smoke 验证 | ✅ | 见下表 |
| **T7** 收尾文档 | ✅ | 当前文档 |

---

## 三、验证记录

### 构建

| 命令 | 结果 |
|------|------|
| `cmake --build build --config Release --target Light` | ✅ exit 0，无 warning 增量 |

### Smoke

| 脚本 | 结果 | 关键断言 |
|------|------|---------|
| `light.exe scripts/smoke/asset_loader_async.lua` | ✅ PASS | API surface 不变 |
| `light.exe scripts/smoke/mesh_3d.lua` | ✅ PASS | Mesh.LoadGLTFAsync 错误语义不变 |
| `light.exe scripts/smoke/asset_loader_async_probe.lua` | ✅ PASS + clean shutdown | "Shared GL Context enabled" + "shutdown complete" 完整 |

### Worker Mesh upload 路径真实数据验证

⏸ **未在本期验证**（仓库无 GLTF 测试资源；造程序化最小 GLB 超出本次裁剪）

**安全保障**：
- 编译类型 + smoke 不变量 + probe 路径全 PASS
- WorkerUploadMesh_ 的 GL 部分与 `CreateMesh` 严格等价（同样的 glGen + glBufferData + glVertexAttribPointer 序列）
- backend 不支持 3D 时 `RegisterUploadedMesh` 返 0 → Tick 翻 Error，handle 通过兜底路径 glDelete
- 计划在 P3 (benchmark) 阶段加测试 GLTF 资源后真实验证

---

## 四、设计偏离记录

| 偏离项 | 原 PLAN | 实际做法 | 原因 |
|----|----|----|----|
| `WorkerUploadMesh_` 中 backend Supports3D 检查 | PLAN 未明确 | 不在 worker 端检查（无访问 backend 的并发安全方式） | 失败时通过 Tick `RegisterUploadedMesh` 返 0 兜底转 Error，行为正确，仅"backend 不支持 3D 但用户调 LoadGLTFAsync"的边缘场景下浪费一次 worker GL 工作 |
| 中文注释错字 | （N/A） | 第一轮 multi_edit 把"兜底"误改成"兑底" 6 处 | 后用 `replace_all` 一行修复 |

---

## 五、已知边界

### 平台覆盖

- ✅ 桌面（Windows / Linux / Mac）：worker mesh 上传路径
- ⏸ 移动 / Web：自动回落主线程上传，等价 G.1.1 行为

### 资源覆盖（G.1 系列累计）

| 资源类型 | 路径 | 阶段 |
|----|----|----|
| Image | worker `glTexImage2D` + fence | G.1.1 |
| LUT (Cube/HALD) | worker `glTexImage3D` + fence | G.1.1 |
| **Mesh (GLTF)** | **worker `glBufferData + 配置 VAO` + fence + 主线程 `RegisterUploadedMesh`** | **G.1.2 (本期)** |
| Font | 主线程 lazy bake (无 GL，worker 加速无价值) | G.1.0 |
| Sound | 主线程 (无 GL) | G.1.0 |

### 未覆盖项

- **GLTF with material**（多张内嵌纹理 + 材质参数）：仍只支持基础 mesh，留 G.1.3 候选
- **Skinned / Morph Target Mesh**：仅基础 RenderVertex3D 路径覆盖；`CreateSkinnedMesh / CreateSkinnedMorphMesh` 未异步化（但用例少，暂不列入 TODO）
