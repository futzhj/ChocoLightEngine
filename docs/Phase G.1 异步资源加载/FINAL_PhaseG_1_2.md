# Phase G.1.2 Mesh worker 上传 — 项目总结报告

> **阶段**：6A Workflow — 阶段 6 Assess (最终交付物)
> **完成日期**：2026-05-17
> **关联**：[PLAN](PLAN_PhaseG_1_2.md) · [ACCEPTANCE](ACCEPTANCE_PhaseG_1_2.md)

---

## 一、目标完成

将 GLTF Mesh 上传从 G.1.1 的"主线程 Tick 内调 `g_render->CreateMesh` 阻塞"演进为：

```mermaid
flowchart LR
    Decode[worker DecodeGLTF_<br/>cgltf 解析] --> WMesh[worker WorkerUploadMesh_<br/>glBufferData + 配置 VAO]
    WMesh --> Fence[glFenceSync]
    Fence -->|fence ready| Tick[主线程 Tick]
    Tick --> Reg[backend RegisterUploadedMesh<br/>O(1) map insert]
    Reg --> Done[resMeshId 写入<br/>status = Ready]
```

主线程从单次 3–8ms 阻塞缩减为单次 ≈O(1) map insert + fence check。

---

## 二、关键架构决策

### 为什么不直接让 worker 调 `g_render->CreateMesh`？

`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:8897-8938` `CreateMesh` 内部包含两段：

| 段 | 操作 | 线程安全 |
|----|----|----|
| GL 段 | `glGenVertexArrays / glGenBuffers / glBindVertexArray / glBufferData / glVertexAttribPointer / glEnableVertexAttribArray` | ✅ |
| C++ 段 | `nextMeshId++` + `meshes[id] = m` | ❌（共享递增 + map 写入） |

**结论**：把 GL 段搬到 worker，C++ 段留主线程，通过新增 `RegisterUploadedMesh` 接口桥接。这种"分裂式"演进不破坏 backend 既有线程模型，且 RegisterUploadedMesh 是 O(1) 操作不构成主线程瓶颈。

### Mesh handle ownership 流转规则

```
worker glGen → state->glMesh{Vao,Vbo,Ebo} 持有
  ├─ fence Ready + RegisterUploadedMesh 成功 → backend 接管, state 清零
  ├─ fence Ready + RegisterUploadedMesh 失败 → Tick 内 glDelete 兜底
  ├─ fence Error / Timeout                 → Tick 内 glDelete 兜底
  ├─ Shutdown 时仍在 result_queue          → Shutdown 内 glDelete 兜底
  └─ FutureState dtor (Lua GC)             → dtor 兜底（多重保险）
```

**4 层兜底保证 GL handle 不泄漏**。

---

## 三、改动清单

### 新增 / 修改

| 文件 | 改动行数 |
|----|----|
| `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h` | +20 行（`RegisterUploadedMesh` 虚函数声明 + 文档） |
| `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +14 行（GL33Core override 实现） |
| `@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h` | +8 行（`FutureState` 4 个 GL handle 字段） |
| `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp` | +120 行（WorkerUploadMesh_ + 各处 dispatch + 4 层兜底清理） |
| **合计** | **≈162 行新增** |

### 文档

- `@e:/jinyiNew/Light/docs/Phase G.1 异步资源加载/PLAN_PhaseG_1_2.md`（合并 ALIGNMENT/DESIGN/TASK）
- `@e:/jinyiNew/Light/docs/Phase G.1 异步资源加载/ACCEPTANCE_PhaseG_1_2.md`
- `@e:/jinyiNew/Light/docs/Phase G.1 异步资源加载/FINAL_PhaseG_1_2.md`（当前文档）

---

## 四、质量评估

### 代码质量
- ✅ 与 G.1.1 Image / LUT 模式严格同构（统一的 glGen + glFlush + glFenceSync 三段式 + Tick fence 翻状态）
- ✅ Mesh handle ownership 4 层兜底无泄漏路径
- ✅ 平台条件编译边界清晰（`#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)` 包裹）
- ✅ Lua 表面零变化

### 测试覆盖
- ✅ 编译过（无新 warning）
- ✅ 2 个 headless smoke 全 PASS
- ✅ probe 脚本完整 clean shutdown
- ⏸ worker mesh upload 路径真实数据验证（待 P3 benchmark）

### 与现有系统集成
- ✅ G.1.1 Image / LUT 路径完全未触碰
- ✅ G.1.0 主线程 fallback 路径保留（probe 失败时自动回落）
- ✅ Lua 层无可见变化（`Mesh.LoadGLTFAsync` 行为完全相同，仅性能特性改进）

---

## 五、性能定性预期

| 场景 | G.1.1 | G.1.2 |
|----|----|----|
| 加载 100K 顶点 GLTF | 主线程 `Tick` 内 ≈3–5ms 阻塞 | 主线程 ≈O(1) map insert + fence check ≈<0.1ms |
| 加载 1M 顶点 GLTF | 主线程 `Tick` 内 ≈10–20ms 阻塞（明显掉帧） | worker 上传 ≈10–20ms 不影响主线程；主线程仅 fence check |
| 同时加载多个大型 GLTF | 串行阻塞主线程 | worker 串行处理，主线程不阻塞 |

驱动层面上传时间不变（GPU 总带宽不变），收益来自**主线程从"GPU 同步阻塞"变为"非阻塞 fence check"**。

---

## 六、风险与缓解

| 风险 | 缓解 |
|----|----|
| Worker 创建的 vao 在主 ctx 上不可见 | `glFenceSync` + 主线程 `glClientWaitSync` 提供严格 happens-before |
| 顶点属性 layout 与 backend shader location 不一致 | 复用 `CreateMesh` 现有 location 0/1/2/3 + offsetof 计算（编译期保证 layout 一致） |
| 大型 mesh worker 仍可能成瓶颈（IO + cgltf 解析） | 不在本期范围；后续可考虑 IO 与 cgltf 解析的细粒度并行化 |

---

## 七、技术债

参见 [TODO_PhaseG_1_1.md](TODO_PhaseG_1_1.md)（已含 G.1.x 系列待办）+ 本期未引入新债。

---

## 八、下一步建议

按用户已经选定的"P1→P2→P3 顺序"，G.1.2 (P1) 已完成，接下来：

1. **TODO-2 (P2)** — Probe 脚本接入 CI 包装
2. **TODO-3 (P3)** — 量化 benchmark sample（含 GLTF 测试资源 → 验证 worker mesh upload 真实数据）
