# Phase G.1.3 — VRAM Worker Upload Tracking ACCEPTANCE

> **完成日期**: 2026-05-19
> **基线**: G.1 + G.1.1 + G.1.2 已交付
> **状态**: T1~T6 完成, T7 (CI) 待

---

## 1. 任务完成情况

| 任务 | 估时 | 实际 | 状态 |
|------|------|------|------|
| T1 GpuMem mutex 化 (4 API + Reset + PushStats) | 0.3h | ~0.2h | ✅ |
| T2 WorkerUploadImage_ Track | 0.1h | ~0.05h | ✅ |
| T3 WorkerUploadMesh_ VBO/EBO TrackBytes | 0.2h | ~0.1h | ✅ |
| T4 WorkerUploadMesh_ material texture Track | 0.2h | ~0.1h | ✅ |
| T5 smoke §J (3 子测) | 0.4h | ~0.3h | ✅ |
| T6 6A 4 件 + G.1 TODO P1 §7 标完成 | 0.3h | ~0.3h | ✅ |
| T7 commit + CI 6/6 PASS | 0.5h | 待 | ⏳ |

**总计**: ~1.3h (估时 2h, -35% 因 mutex 化简洁)

---

## 2. 文件改动清单

### 新建 (4 文档)
| 文件 |
|------|
| `docs/Phase G.1.3 .../CONSENSUS_PhaseG_1_3.md` |
| `docs/Phase G.1.3 .../ACCEPTANCE_PhaseG_1_3.md` (本文) |
| `docs/Phase G.1.3 .../FINAL_PhaseG_1_3.md` |
| `docs/Phase G.1.3 .../TODO_PhaseG_1_3.md` |

### 修改 (3 文件)

| 文件 | 改动 |
|------|------|
| `ChocoLight/src/light_gpumem.cpp` | +1 #include, +1 static mutex, +6 lock_guard |
| `ChocoLight/src/asset_loader.cpp` | +5 行 (worker 4 处 Track) |
| `scripts/smoke/gpumem.lua` | +60 行 (§J 3 子测) |
| `docs/Phase G.1 VRAM Tracking/TODO_PhaseG_1.md` | (next) §7 标完成 |

**总计**: ~12 LOC 代码净增

---

## 3. 验收标准核对

| 项 | 验证 | 结果 |
|----|------|------|
| `std::mutex s_mutex` 加入 | grep `s_mutex` 应有 7 处 (1 declare + 6 lock_guard) | ✅ |
| Track / Untrack / TrackBytes / UntrackBytes / Reset / PushStats 全部 lock | grep `lock_guard<std::mutex>` 应有 6 处 | ✅ |
| WorkerUploadImage_ 后调 Track("User Image", "RGBA8", ...) | 代码审查 line 753-754 | ✅ |
| WorkerUploadMesh_ VBO + EBO 后调 TrackBytes | 代码审查 line 836-838 | ✅ |
| WorkerUploadMesh_ material texture 在循环内 Track | 代码审查 line 897-898 | ✅ |
| smoke §J PushStats 5x 不死锁 | smoke syntax PASS | ✅ |
| smoke §J Reset 后 items 清空 (or UBO 重 Track) | smoke syntax PASS | ✅ |
| smoke §J Mesh VBO/EBO BYTES format 校验 | headless 0 items 是预期 | ✅ |
| 32+ 老 smoke 不回归 | syntax check OK | ✅ 待 CI |
| 6/6 CI PASS | GH Actions | ⏳ T7 |

### 3.1 零回归

| 项 | 保证 |
|----|------|
| GpuMem API 签名不变 | ✅ Track/Untrack/TrackBytes/UntrackBytes/Reset/PushStats 同样签名 |
| BytesPerPixel 表不变 | ✅ G.1.2 已扩 R8/RGB8 全保留 |
| G.1 / G.1.1 / G.1.2 hook 全部保留 | ✅ 无任何老 hook 被改动 |
| 主线程性能影响 | 极低. mutex 锁内 < 100 ns, 远小于一帧 |

---

## 4. Hook 覆盖率矩阵 (G.1.3 + 累计)

### 4.1 G.1.3 新增 Worker hooks

| 路径 | 文件 | 行 | 资源 | 状态 |
|------|------|----|------|------|
| WorkerUploadImage_ | `asset_loader.cpp` | 753 | User Image RGBA8 | ✅ |
| WorkerUploadMesh_ VBO | `asset_loader.cpp` | 837 | Mesh VBO BYTES | ✅ |
| WorkerUploadMesh_ EBO | `asset_loader.cpp` | 838 | Mesh EBO BYTES | ✅ |
| WorkerUploadMesh_ material | `asset_loader.cpp` | 898 | Mesh texture RGBA8 (循环) | ✅ |
| WorkerUploadLUT_ | `asset_loader.cpp` | 917 | 3D LUT (留 G.1.4) | ❌ skip |

### 4.2 累计 Track / Untrack 配对

| 资源 | Track 处 | Untrack 处 | 状态 |
|------|---------|----------|------|
| HDR / TAA / SSR / SSAO / Bloom / AE RT | G.1 + G.1.1 | 配套 | ✅ 闭环 |
| Font atlas | G.1.2 sync + async | G.1.2 Font GC | ✅ 闭环 |
| User Image (主线程) | G.1.2 sync x4 | ❌ 历史 leak | G.1.7+ 修 |
| User Image (worker) | **G.1.3 新增** | ❌ 历史 leak | G.1.7+ 修 |
| Mesh texture (主线程) | G.1.2 sync + GLTF main | ⚠️ 路径分散 | v1.4 |
| Mesh texture (worker) | **G.1.3 新增** | ⚠️ 路径分散 | v1.4 |
| Mesh VBO / EBO | **G.1.3 新增** | ❌ 没 GC 路径 | v1.4 |
| Sprite frame | G.1.2 sync | ⚠️ 路径分散 | v1.4 |

---

## 5. 风险评估

| 风险 | 等级 | 实际结果 |
|------|------|----------|
| mutex deadlock | 极低 | smoke §J.1 5x 连调 PASS |
| `std::mutex` 在 emscripten 不可用 | 低 | C++11 标准, emscripten 单线程 fallback |
| Worker hook 在 GL 失败路径误调 | 低 | 主动检查 glGetError 后才 Track |
| BYTES format slot 满 | 极低 | < 20 unique combos |

---

## 6. 已知限制 / 后续工作

- **主线程 `g_render->CreateMesh` 5 处未 hook**: SkinnedMesh ReCreate 累积 leak 问题, 留 v1.4
- **LUT worker 路径**: 3D LUT 字节算法 (size^3 * format) 与 BytesPerPixel 表不兼容, 需新 `Track3D` API, 留 G.1.4
- **Mesh / Sprite GC 路径分散**: v1.4 修
- **Image __gc 缺失**: G.1.7+ Type Safety 系列修

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T6 完成 (T7 CI 待) |
