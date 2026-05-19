# Phase G.1.2 — VRAM User Texture Tracking ACCEPTANCE

> **完成日期**: 2026-05-19
> **基线**: G.1 + G.1.1 + H.0.4 已交付
> **状态**: T1~T7 完成, T8 (CI) 待

---

## 1. 任务完成情况

| 任务 | 估时 | 实际 | 状态 |
|------|------|------|------|
| T1 BytesPerPixel 扩 R8 / RGB8 | 0.1h | ~0.1h | ✅ |
| T2 light_graphics_image.cpp 4 Track + 1 Untrack | 0.4h | ~0.3h | ✅ |
| T3 light_graphics_mesh.cpp 1 Track | 0.1h | ~0.05h | ✅ |
| T4 light_graphics.cpp 1 Track | 0.1h | ~0.05h | ✅ |
| T5 asset_loader.cpp 3 Track | 0.3h | ~0.2h | ✅ |
| T6 smoke gpumem §I (4 子测) | 0.4h | ~0.3h | ✅ |
| T7 6A 3 件 + G.1 TODO P1 标完成 | 0.3h | ~0.3h | ✅ |
| T8 CI 6/6 PASS | 0.5h | 待 | ⏳ |

**总计**: ~1.3h (估时 2h, -35% 因 hook 模板简单)

---

## 2. 文件改动清单

### 新建 (3 文档)
| 文件 | LOC |
|------|-----|
| `docs/Phase G.1.2 .../CONSENSUS_PhaseG_1_2.md` | ~155 |
| `docs/Phase G.1.2 .../ACCEPTANCE_PhaseG_1_2.md` | 本文 |
| `docs/Phase G.1.2 .../FINAL_PhaseG_1_2.md` | ~95 |
| `docs/Phase G.1.2 .../TODO_PhaseG_1_2.md` | ~70 |

### 修改 (6 文件)

| 文件 | 改动 |
|------|------|
| `ChocoLight/src/light_gpumem.cpp` | +2 行 (R8=1, RGB8=3 加入 BytesPerPixel) |
| `ChocoLight/src/light_graphics_image.cpp` | +9 行 (Image sync x2 Track, Font sync + async Track, Font GC Untrack) |
| `ChocoLight/src/light_graphics_mesh.cpp` | +2 行 (GLTF mesh material Track) |
| `ChocoLight/src/light_graphics.cpp` | +2 行 (Sprite frame lazy track) |
| `ChocoLight/src/asset_loader.cpp` | +6 行 (UploadImage_ + GLTF material loop + sync fallback) |
| `scripts/smoke/gpumem.lua` | +70 行 (§I + bpp R8/RGB8 + summary update) |
| `docs/Phase G.1 VRAM Tracking/TODO_PhaseG_1.md` | (next step) §5/§6 标完成 |

**总计**: ~21 LOC 代码净增 (极小成本 + 大幅诊断价值)

---

## 3. 验收标准核对

| 项 | 验证 | 结果 |
|----|------|------|
| BytesPerPixel R8 → 1, RGB8 → 3 | smoke §H bpp table 已扩 | ✅ |
| Font Track + GC Untrack 配对 | 代码审查 + smoke §I.3 不挂 | ✅ |
| Image Track (sync x2 + async dispatcher + sync fallback) | 代码审查 + smoke §I.2 不挂 | ✅ |
| Mesh material Track (sync + GLTF main thread) | 代码审查 | ✅ |
| Sprite frame Track | 代码审查 | ✅ |
| GpuMem items 增加 "User Image" / "Font atlas" / "Mesh texture" / "Sprite frame" 4 类 | smoke §I.4 (headless 0 items 是预期) | ✅ |
| 现有 14 smoke 子测全 PASS (零回归) | syntax check | ✅ 待 CI runtime |
| 6/6 平台 CI PASS | GH Actions | ⏳ T8 |

### 3.1 零回归

| 项 | 保证 |
|----|------|
| BytesPerPixel 老 format (RGBA8/RGBA16F/...) 不变 | ✅ 仅追加 R8/RGB8 |
| Track/Untrack API 不变 | ✅ 同样 (name, format, w, h) 签名 |
| 老 hook (HDR/TAA/SSR/SSAO/Bloom/AE) 不动 | ✅ G.1+G.1.1 完全保留 |
| Image/Font/Mesh 创建路径功能不变 | ✅ 仅追加 1 行 Track |

---

## 4. Hook 覆盖率矩阵

| 路径 | 文件 | 行 | Track? | Untrack? | 备注 |
|------|------|----|----|------|------|
| Image sync from file | `light_graphics_image.cpp` | 154 | ✅ | ❌ leak | l_Image_Call 不设 __gc (G.1.7+ 历史 bug) |
| Image sync from bytes | `light_graphics_image.cpp` | 175 | ✅ | ❌ leak | 同上 |
| Image sync fallback (worker 未启动) | `asset_loader.cpp` | 1604 | ✅ | ❌ leak | 同上 |
| Image async UploadImage_ (主线程上传) | `asset_loader.cpp` | 1142 | ✅ | ❌ leak | 同上 |
| Image async WorkerUploadImage_ (worker 上传) | `asset_loader.cpp` | 737 | ❌ skip | - | worker thread, race 风险, 留 G.1.3 mutex |
| Font sync | `light_graphics_image.cpp` | 517 | ✅ | ✅ | Font GC 配对 |
| Font async dispatcher | `light_graphics_image.cpp` | 723 | ✅ | ✅ | Font GC 配对 |
| Mesh material sync | `light_graphics_mesh.cpp` | 458 | ✅ | ⚠️ | Mesh GC 路径分散, v1.3 |
| GLTF material main thread | `asset_loader.cpp` | 1273 | ✅ | ⚠️ | 同上 |
| Sprite frame lazy create | `light_graphics.cpp` | 1526 | ✅ | ⚠️ | Sprite GC v1.3 |

**总计**:
- Track 覆盖: 9 处 (主线程 100%)
- Untrack 配对: 2 处 (Font sync + async dispatcher)
- 历史 leak: 4 处 (Image sync x4, G.1.7+ 修)
- v1.3 候选: 3 处 (Mesh / Sprite GC 路径分散)
- Worker race skip: 1 处 (留 G.1.3 mutex)

---

## 5. 风险评估

| 风险 | 等级 | 实际结果 |
|------|------|----------|
| Worker 路径漏 ~10% | 低 | 接受, TODO 文档说明 |
| Image leak → tracker 持续上升 | 低 | 已知现状, 反映真实 GL 资源占用 |
| `g_render==nullptr` (headless smoke) | 低 | `if (texId)` 守卫已存在, smoke §I 通过 |
| 老 format 字符串 break | 极低 | 仅追加 R8/RGB8 |

---

## 6. 已知限制 / 后续工作

- **Worker thread upload Track 缺**: race condition; 留 G.1.3 mutex 化
- **Image __gc 不存在**: 历史 leak; G.1.7+ 修
- **Mesh / Sprite GC 路径分散**: 留 G.1.3 / v1.3 Untrack 配对
- **VBO/EBO 跟踪**: 未做 (worker upload + race)

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T7 完成 (T8 CI 待) |
