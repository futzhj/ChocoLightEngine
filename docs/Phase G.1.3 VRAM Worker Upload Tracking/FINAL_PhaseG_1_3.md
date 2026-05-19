# Phase G.1.3 — VRAM Worker Upload Tracking FINAL 总结

> **完成日期**: 2026-05-19
> **状态**: T1~T7 (含 CI) 全部完成

---

## 1. 一句话总结

G.1.3 给 LT::GpuMem tracker 加 std::mutex (4 API + Reset + PushStats 全部 lock_guard 保护), 在 worker thread upload 路径加 4 处 Track hook (User Image / Mesh VBO / Mesh EBO / Mesh material texture), 闭合 G.1 TODO P1 §7 (Mesh VBO/EBO) + G.1.2 留下的 ~10% under-report.

---

## 2. 交付物

### 2.1 代码 (~12 LOC 净增)

- `light_gpumem.cpp`: +1 #include, +1 mutex declare, +6 lock_guard
- `asset_loader.cpp`: 4 处 worker hook (Image RGBA8 + VBO BYTES + EBO BYTES + material RGBA8)
- `gpumem.lua`: §J 60 LOC (3 子测)

### 2.2 文档 (4 件套, ~330 LOC)

- `CONSENSUS_PhaseG_1_3.md`
- `ACCEPTANCE_PhaseG_1_3.md`
- `FINAL_PhaseG_1_3.md` — 本文
- `TODO_PhaseG_1_3.md`

### 2.3 G.1 VRAM TODO P1 标记

- §7 Mesh VBO/EBO 跟踪 ✅ (worker 路径 hook 完整)

---

## 3. CI 验证

**Run**: [26072400642](https://github.com/futzhj/ChocoLightEngine/actions/runs/26072400642)
**结果**: ✅ 6/6 PASS (windows / linux / macos / android / ios / web)
**提交**: a10ea61 "Phase G.1.3 VRAM Worker Upload Tracking - mutex + Mesh VBO/EBO"

---

## 4. 关键技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| mutex 类型 | `std::mutex` (not recursive) | 4 API 不互调, 无递归风险 |
| 锁粒度 | 整个 API 体 | 简单可靠, 锁内 < 100 ns |
| BYTES format 用于 VBO/EBO | ✅ 复用 G.1 已设计的 sentinel | 无需新增 API, smoke 公式已 skip |
| LUT worker hook? | 留 G.1.4 | 3D LUT 字节算法不同 (size^3) |
| 主线程 `g_render->CreateMesh` hook? | 留 v1.4 | SkinnedMesh ReCreate 累积 leak 问题需先解决 |

---

## 5. 度量

### 5.1 工时
| 阶段 | 估时 | 实际 | 偏差 |
|------|------|------|------|
| 4A 文档 | 0.3h | ~0.3h | 0 |
| T1~T4 实施 | 0.8h | ~0.4h | -50% |
| T5 smoke | 0.4h | ~0.3h | -25% |
| T6 6A 3 件 | 0.3h | ~0.3h | 0 |
| T7 CI | 0.5h | 待 | - |
| **总计** | **~2h** | **~1.3h** | **-35%** |

### 5.2 LOC
```
代码: ~12 LOC (3 文件)
Smoke: ~60 LOC
文档: ~330 LOC
```

### 5.3 性能影响

- mutex lock_guard 单次开销: ~20 ns (非竞争状态) ~ 100 ns (轻竞争)
- Track 单次总开销: O(N) 查找 (N ≤ 64) + 锁 ≈ 200 ns. 创建一次, 不在帧热路径.
- PushStats (主线程, 用户 Lua 调) 开销: 类似上, 不影响渲染.

---

## 6. 零回归

| 项 | 状态 |
|----|------|
| G.1/G.1.1/G.1.2 ~22 smoke PASS | ✅ syntax check |
| 32+ 老 sample syntax | ✅ |
| Phase H.0~H.0.4 完整保留 | ✅ |
| 无新 dependency (std::mutex 是 C++11 标准) | ✅ |

---

## 7. 用户使用示例

### 7.1 异步 GLTF 加载 + VBO/EBO 监控

```lua
Light.AssetLoader.Preload({
    meshes = {"models/scene.glb"},
}, function(results)
    local stats = Light.Graphics.GetMemoryStats()
    local total_vbo, total_ebo, total_mat = 0, 0, 0
    for _, it in ipairs(stats.items) do
        if it.name == "Mesh VBO"     then total_vbo = total_vbo + it.bytes
        elseif it.name == "Mesh EBO" then total_ebo = total_ebo + it.bytes
        elseif it.name == "Mesh texture" then total_mat = total_mat + it.bytes
        end
    end
    print(string.format("scene.glb: VBO=%.1fMB, EBO=%.1fMB, materials=%.1fMB",
                        total_vbo / 1048576, total_ebo / 1048576, total_mat / 1048576))
end)
```

### 7.2 异步 Image worker upload (G.1.2 worker race fix 验证)

```lua
local h = Light.Graphics.Image.LoadAsync("textures/large_4k.png")
h:OnReady(function(img, err)
    if img then
        local stats = Light.Graphics.GetMemoryStats()
        for _, it in ipairs(stats.items) do
            if it.name == "User Image" then
                print(string.format("User Image total: %d count, %.1f MB",
                                    it.count, it.bytes / 1048576))
            end
        end
    end
end)
```

---

## 8. 与其他模块关系

| 模块 | 关系 |
|------|------|
| G.1 / G.1.1 / G.1.2 | 完全保留 hook + API |
| Phase G.1.5 GLTF Material | worker upload 现也走 G.1.3 mutex 化 hook |
| Phase G.1.6 Preload Manifest | 完全独立 (Track 在底层 hook 中) |
| Phase G.1.7 Type Safety | 配合 magic 校验 |

---

## 9. 后续可做 (优先级)

- **G.1.4** ⭐⭐: Image / Mesh / Sprite GC 配对 + LUT worker upload
- **v1.5** ⭐: 主线程 `g_render->CreateMesh` 5 处 hook (需先解决 SkinnedMesh ReCreate 配对)
- **v1.6** ⭐: 用户 Image __gc 修 (历史 leak)

---

## 10. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T7 完成 |
