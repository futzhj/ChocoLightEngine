# Phase G.1.2 — TODO 清单

> **完成日期**: 2026-05-19
> **状态**: T1~T8 完成

---

## 1. 用户配置

无新增配置. 默认行为:
- `Light.Graphics.GetMemoryStats().items[]` 自动包含 `"User Image"` / `"Font atlas"` / `"Mesh texture"` / `"Sprite frame"` 4 类 (当对应资源创建时).
- BytesPerPixel 表自动支持 `R8=1`, `RGB8=3`.

---

## 2. 推荐验证

### 2.1 smoke
```powershell
.\build\Release\Light.exe scripts\smoke\gpumem.lua
# 期待: §A~§I 全 PASS (G.1+G.1.1+G.1.2 累计 ~22 子测)
```

### 2.2 实机验证
```powershell
# 加载 10 个 sprite 后查 stats:
.\build\Release\Light.exe samples\demo_taau\main.lua
# 在 Lua console 调:
local s = Light.Graphics.GetMemoryStats()
for _, it in ipairs(s.items) do
    if it.name == "User Image" or it.name == "Sprite frame" or
       it.name == "Mesh texture" or it.name == "Font atlas" then
        print(it.name, it.count, it.bytes / 1024, "KB")
    end
end
```

---

## 3. 已知限制

### 3.1 Worker thread upload Track 缺失
**现状**: `WorkerUploadImage_` (asset_loader.cpp:737) 直接调 `glGenTextures + glTexImage2D`, 不走 `g_render->CreateTexture`. Worker thread 调 `LT::GpuMem::Track` 会 race (tracker 无 mutex).
**影响**: 当 shared GL context 可用时, 异步 Image 加载会漏 Track. 主线程 `UploadImage_` fallback 路径不漏 (CONSENSUS §2 已 hook).
**升级**: G.1.3 — Tracker mutex 化 + worker hook.
**估时**: 2h.

### 3.2 Image __gc 不存在 → 永久 leak
**现状**: `l_Image_Call` 不设 `__gc` 元表 (line 183 仅 `lua_setfield(L, 1, "__instance")`). Image 创建的 GL texture 不会被 DeleteTexture, tracker 计入永不 Untrack.
**影响**: 用户多次 `Light.Graphics.Image()` 后 stats 持续上升, **反映真实 GL 资源 leak**.
**升级**: G.1.7+ Type Safety 系列修. 加 `l_Image_GC` + 元表设置 + LT::GpuMem::Untrack.
**估时**: 1h.

### 3.3 Mesh / Sprite GC Untrack 路径分散
**现状**: Mesh material texture 在 Mesh GC 时是否 Delete 不明确; Sprite frame 在 SpriteAnimation GC 时同样问题.
**影响**: 这两类 item 的 count 会随时间增长.
**升级**: G.1.3 / v1.3 — 走通 Mesh GC + Sprite GC 路径 Untrack.
**估时**: 1.5h.

### 3.4 VBO/EBO 跟踪未做
**现状**: `WorkerUploadMesh_` (asset_loader.cpp:762) glGenBuffers, 同样 worker race.
**影响**: 大 GLB 模型 (~100K verts × 48 B = ~5MB) 不计入.
**升级**: G.1.3 mutex 化后即可加.
**估时**: 1h (依赖 §3.1 mutex).

### 3.5 ImageData 不跟踪
**说明**: `ImageData` 仅 pixel buffer (CPU RAM), 无 GPU 资源. 严格说不属于 VRAM tracker 范畴, 故不跟踪.
**替代**: 若用户需要监控 ImageData RAM, 用 `collectgarbage("count")` Lua 端.

---

## 4. 增强候选 (按 ROI)

### 4.1 G.1.3 — Tracker mutex 化 + worker hook ⭐⭐⭐
**估时**: 2h
**收益**: 闭合所有 worker upload 路径漏 Track; 准备 VBO/EBO + Mesh GC + Sprite GC 升级.
**实现**:
- `LT::GpuMem::Track/Untrack` 内加 `std::mutex` (单 lock, 已有静态数组无别的 thread state)
- Worker `WorkerUploadImage_` 后 + `WorkerUploadMesh_` 后调 Track
- Smoke 加 §J: 异步 Image / Mesh 完成后查 stats

### 4.2 G.1.4 — Image __gc 补 ⭐⭐
**估时**: 1h
**收益**: 闭合 Image leak (已知历史 bug); 用户 Image 占用稳定不再涨.
**实现**:
- `l_Image_Call` 加 metatable + `l_Image_GC` 配对
- GC 内调 `g_render->DeleteTexture(ctx->texId)` + `LT::GpuMem::Untrack`
- Magic 校验防 use-after-free

### 4.3 G.1.5 — Mesh / Sprite GC Untrack ⭐⭐
**估时**: 1.5h
**收益**: 闭合 Mesh material + Sprite frame leak.
**前置**: 找到对应 GC 路径 (Mesh userdata GC + SpriteAnimation GC).

### 4.4 LT::GpuMem::TrackTextureChannels(cat, w, h, channels) helper ⭐
**估时**: 0.3h
**收益**: caller 写 `Track("User Image", "RGBA8", w, h)` → `TrackTextureChannels("User Image", w, h, 4)` 一致性提高.
**实现**: light_gpumem.cpp 加包装函数, 内部 dispatch RGBA8/RGB8/R8.

---

## 5. 文档状态

| 文档 | 状态 |
|------|------|
| CONSENSUS | ✅ |
| ACCEPTANCE | ✅ |
| FINAL | ✅ |
| TODO | ✅ 本文 |

注: G.1.2 是 4A 轻量, 无独立 ALIGNMENT/DESIGN/TASK 文件 (合并入 CONSENSUS).

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
