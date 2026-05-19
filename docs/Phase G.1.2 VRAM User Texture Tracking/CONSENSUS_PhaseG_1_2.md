# Phase G.1.2 — VRAM Tracking v1.2 (User Image / Font / Mesh Texture) — 6A 合并

> **基线**: G.1 + G.1.1 已交付 (5 类 RT, 14 smoke PASS)
> **范围**: 闭合 G.1 TODO P1 §5+§6 (用户 Image + Font atlas) + §M (Mesh material texture)
> **估时**: 2h (vs 原 TODO 估 3h)

---

## 1. Align — 项目对齐

### 1.1 需求来源 (G.1 TODO P1)

| TODO | 内容 | 价值 |
|------|------|------|
| §5 用户 Image / ImageData | 50+ 处 glGen+glTexImage2D 漏 track | 多 sprite 场景能看到 |
| §6 Font glyph atlas | 1 处 hook 在 EnsureAtlas | 大字号 + CJK 时可见 |
| §M (隐含) | Mesh material texture (GLTF PBR + .png mesh tex) | 大模型加载后可见 |

### 1.2 调研结果

13 处 `g_render->CreateTexture` 调用分布:

| 文件 | 行 | 路径 | 主/Worker | 范围 |
|------|----|------|---------|------|
| `light_graphics_image.cpp` | 154 | Image sync from file (`l_Image_Call`) | 主 | ✅ Track |
| `light_graphics_image.cpp` | 175 | Image sync from bytes | 主 | ✅ Track |
| `light_graphics_image.cpp` | 517 | Font sync atlas (l_Font_Call) | 主 | ✅ Track |
| `light_graphics_image.cpp` | 723 | Font async dispatcher (`FontPushResult_`) | 主 | ✅ Track |
| `light_graphics_mesh.cpp` | 458 | Mesh sync material texture | 主 | ✅ Track |
| `light_graphics.cpp` | 1526 | SpriteAnimation frame upload | 主 | ✅ Track |
| `asset_loader.cpp` | 1142 | Image async main-thread upload (`UploadImage_`) | 主 | ✅ Track |
| `asset_loader.cpp` | 1273 | GLTF material main-thread upload | 主 | ✅ Track |
| `asset_loader.cpp` | 1604 | Image sync fallback (worker 未启动) | 主 | ✅ Track |
| `asset_loader.cpp` | 737 (glTexImage2D) | Worker shared-GL upload | ⚠️ Worker | ❌ 跳过 (race) |
| `light_ui.cpp` | (1 处, 查到了再加) | 待定 | 待定 | 待定 |

**配对 DeleteTexture**:

| 文件 | 行 | 路径 | 范围 |
|------|----|------|------|
| `light_graphics_image.cpp` | 461 | Font GC | ✅ Untrack |
| Image sync GC | 不存在 | (已知历史 bug: l_Image_Call 不设 __gc) | ❌ leak; 不修 |
| Mesh material GC | 复杂 | (Mesh GC 路径分散) | ⚠️ v1.3 改进候选 |

### 1.3 边界

- **IN**: 主线程 7 路径 Track + 1 Font GC Untrack + `BytesPerPixel` 扩 R8/RGB8 + smoke §15 + 4 件 6A + G.1 TODO 标完成
- **OUT**:
  - Worker shared-GL upload (`WorkerUploadImage_`) — 需 mutex 化 tracker, 留 G.1.3
  - Image sync `l_Image_Call` GC 缺失 = 已知 leak (G.1.7+ Type Safety 后续修)
  - Mesh / Sprite material texture Untrack — Mesh GC 路径分散, 留 v1.3
  - VBO/EBO 跟踪 — worker upload 路径, 同 race 问题留 G.1.3

### 1.4 决策

| 问 | 答 |
|---|---|
| 中央 helper (`CreateTrackedTexture`) 还是 caller hook? | **caller hook 一行**. 中央 helper 要改 9 处签名, ROI 不如直接加 1 行 Track |
| Worker upload race 如何处理? | **跳过 worker, 仅 hook 主线程 dispatcher / 主线程 caller**. 主线程上传部分约占 90%, 接受 < 10% under-report. 文档说明 |
| Format 命名 | **R8 / RGB8 / RGBA8** (与 G.1 已有 RGBA8 命名一致) |
| Category 命名 | `"User Image"` / `"Font atlas"` / `"Mesh texture"` / `"Sprite frame"` (4 类) |
| Image leak 修不修? | **不修**. 反映现实, 用户看到 Image 占用持续上升能促使他们认识到 GC 缺失. 修在 G.1.7+ |
| asset_loader.cpp Image async dispatcher hook 在哪? | `UploadImage_` 内 g_render->CreateTexture 后即调 Track (与同步路径对齐) |

---

## 2. Architect — 设计

### 2.1 数据流

```
主线程同步路径:
  Image sync:       l_Image_Call → CreateTexture → ★ Track("User Image", "RGBA8", w, h)
  Image bytes:      l_Image_Call → CreateTexture → ★ Track
  Image sync fallb: LoadImageAsync (worker 未启动) → CreateTexture → ★ Track
  Font sync:        l_Font_Call → CreateTexture → ★ Track("Font atlas", "R8", w, h)
  Mesh material:    light_graphics_mesh.cpp:458 → CreateTexture → ★ Track
  Sprite frame:     light_graphics.cpp:1526 → CreateTexture → ★ Track

主线程异步 dispatcher (worker 已上传 / 主线程上传):
  Image async UploadImage_: → CreateTexture → ★ Track
  Image async WorkerUpload: worker glTexImage2D, 主线程 dispatcher ImagePushResult_
                          → ★ Track 在 dispatcher 内
  GLTF material main:       asset_loader.cpp:1273 → CreateTexture → ★ Track
  Font async dispatcher:    FontPushResult_ → CreateTexture → ★ Track

GC:
  Font GC l_Font_GC: DeleteTexture → ★ Untrack("Font atlas", "R8", atlasW, atlasH)
  Image GC: 不存在 (已知 leak)
```

### 2.2 BytesPerPixel 扩展

```cpp
// light_gpumem.cpp: BytesPerPixel
static int BytesPerPixel(const char* format) {
    if (...) return 4;        // 已有 RGBA8
    if (std::strcmp(format, "R8")   == 0) return 1;   // 新增
    if (std::strcmp(format, "RGB8") == 0) return 3;   // 新增
    ...
}
```

### 2.3 Hook 模板

```cpp
// 创建后:
ctx->texId = g_render ? g_render->CreateTexture(w, h, 4, pixels) : 0;
if (ctx->texId) {
    LT::GpuMem::Track("User Image", "RGBA8", w, h);
}

// 删除前 (GC):
if (fc->texId && g_render) {
    LT::GpuMem::Untrack("Font atlas", "R8", fc->atlasW, fc->atlasH);
    g_render->DeleteTexture(fc->texId);
    fc->texId = 0;
}
```

---

## 3. Atomize — 任务拆分

| 任务 | 估时 | 输出 |
|------|------|------|
| T1 BytesPerPixel 扩 R8 / RGB8 | 0.1h | light_gpumem.cpp +2 行 |
| T2 light_graphics_image.cpp 4 处 Track + 1 处 Untrack | 0.4h | sync Image x2, sync Font, async Font dispatcher, Font GC |
| T3 light_graphics_mesh.cpp 1 处 Track | 0.1h | Mesh material |
| T4 light_graphics.cpp 1 处 Track | 0.1h | Sprite frame |
| T5 asset_loader.cpp 3 处 Track | 0.3h | Image UploadImage_, GLTF material, sync fallback |
| T6 smoke gpumem §15 | 0.4h | 覆盖 R8/RGB8 + Font Track/Untrack + Image Track |
| T7 4 件 6A + G.1 TODO P1 标完成 | 0.3h | docs |
| T8 CI 6/6 PASS | 0.5h | GH Actions |

**总计**: ~2.2h

---

## 4. 验收

| 项 | 验证 |
|----|------|
| `BytesPerPixel("R8")` → 1, `BytesPerPixel("RGB8")` → 3 | smoke §15 |
| 创建 Image 后 `GetMemoryStats().items` 包含 "User Image" RGBA8 | smoke §15 (headless graceful skip) |
| 创建 Font 后包含 "Font atlas" R8 | smoke §15 |
| Font GC 后 Untrack (count--) | smoke §15 |
| 32+ 老 smoke / 0 回归 | 全套回归 |
| 6/6 CI PASS | GH Actions |

---

## 5. 风险

| 风险 | 等级 | 缓解 |
|------|------|------|
| Worker 路径漏 ~10% | 低 | 主线程占 ≥ 90%; v1.3 加 mutex 后修 |
| Image leak 未修 → tracker 显示持续上升 | 低 | 反映现实, 文档说明 |
| `g_render==nullptr` (headless smoke) | 低 | `if (texId)` 守卫已存在 |
| BytesPerPixel 表新增格式 break 老 hook | 极低 | RGBA8 已有, R8/RGB8 是新增, 老 hook 不调 |

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
