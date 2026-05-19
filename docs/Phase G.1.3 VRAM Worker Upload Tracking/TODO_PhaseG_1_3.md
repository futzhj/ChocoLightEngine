# Phase G.1.3 — TODO 清单

> **完成日期**: 2026-05-19
> **状态**: T1~T7 完成

---

## 1. 用户配置

无新增配置. 默认行为:
- `Light.Graphics.GetMemoryStats().items[]` 在异步 GLTF 加载后自动包含:
  - `"Mesh VBO"` (BYTES format)
  - `"Mesh EBO"` (BYTES format)
  - `"Mesh texture"` (RGBA8)
  - `"User Image"` (RGBA8) — worker upload 路径
- 其他 hook (Font / 主线程 Image / Mesh material main) 已在 G.1.2 交付.

---

## 2. 推荐验证

### 2.1 smoke
```powershell
.\build\Release\Light.exe scripts\smoke\gpumem.lua
# 期待: §A~§J 全 PASS (~25 子测累计)
```

### 2.2 实机
```powershell
# GLTF 大模型加载后查 stats
.\build\Release\Light.exe samples\demo_gltf\main.lua
# Lua console 调:
local s = Light.Graphics.GetMemoryStats()
for _, it in ipairs(s.items) do
    if it.name == "Mesh VBO" or it.name == "Mesh EBO" then
        print(it.name, it.count, it.bytes / 1024, "KB")
    end
end
```

---

## 3. 已知限制

### 3.1 主线程 `g_render->CreateMesh` 未 hook
**现状**: 5 处主线程 caller (sync from verts / GLTF main thread / SkinnedMesh ReCreate x2 / async fallback) 未加 Track.
**原因**: SkinnedMesh ReCreate 每帧累积 leak 风险 (无 Untrack 配对).
**升级**: v1.4 — 同时做 Mesh GC 路径 + ReCreate 配对.
**估时**: 1.5h.

### 3.2 LUT worker upload 未 hook
**现状**: `WorkerUploadLUT_` (asset_loader.cpp:907+) 直接 glGenTextures + glTexImage3D, 不在 G.1.3 范围.
**原因**: 3D LUT 字节算法 (`size^3 * format_bytes`) 与 BytesPerPixel 表不兼容, 需新 `Track3D(name, format, size)` API.
**升级**: G.1.4 — 加 3D 资源跟踪.
**估时**: 1h.

### 3.3 Mesh / Sprite / Image GC 路径分散
**现状**: G.1.3 只 Track 不 Untrack. items 持续累加 (反映真实 GL leak).
**升级**: v1.4 — 各类 GC 路径补 Untrack.
**估时**: 2h.

### 3.4 mutex 化无性能 benchmark
**现状**: 仅理论估算 (~20-100 ns 锁开销).
**升级**: 若发现帧时长抖动, 加 perf timer 测.
**估时**: 0.5h.

---

## 4. 增强候选

### 4.1 G.1.4 — 资源 GC 配对 + LUT 3D ⭐⭐⭐
**估时**: 3h
**收益**: 闭合所有已知 leak (Image / Mesh material / Sprite / Mesh VBO/EBO Untrack); 加 3D LUT 跟踪.

### 4.2 v1.4 — 主线程 Mesh hook + SkinnedMesh ReCreate ⭐⭐
**估时**: 1.5h
**前置**: 4.1.

### 4.3 v1.6 — Image __gc 修 (G.1.7+ Type Safety) ⭐⭐
**估时**: 1h
**收益**: 闭合 Image leak.

---

## 5. 文档状态

| 文档 | 状态 |
|------|------|
| CONSENSUS | ✅ |
| ACCEPTANCE | ✅ |
| FINAL | ✅ |
| TODO | ✅ 本文 |

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
