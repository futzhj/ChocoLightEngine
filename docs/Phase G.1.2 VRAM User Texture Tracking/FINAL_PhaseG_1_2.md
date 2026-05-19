# Phase G.1.2 — VRAM User Texture Tracking FINAL 总结

> **完成日期**: 2026-05-19
> **状态**: T1~T8 (含 CI) 全部完成

---

## 1. 一句话总结

G.1.2 在 G.1/G.1.1 基础上扩展 VRAM tracker, 覆盖用户 Image / Font glyph atlas / Mesh material texture / Sprite frame 4 大类高层资源, 9 处 caller hook + 配套 Font GC Untrack, BytesPerPixel 表新增 R8 / RGB8, smoke 加 §I (4 子测), CI 6/6 全绿.

---

## 2. 交付物

### 2.1 代码 (~21 LOC 净增)

- C++: 6 文件改动
  - `light_gpumem.cpp` +2 (R8/RGB8 BytesPerPixel)
  - `light_graphics_image.cpp` +9 (4 Track + Font GC Untrack)
  - `light_graphics_mesh.cpp` +2 (GLTF mesh material)
  - `light_graphics.cpp` +2 (Sprite frame lazy)
  - `asset_loader.cpp` +6 (3 路径 Track)
- Smoke: `gpumem.lua` +70 (§I 4 子测 + bpp 扩 + summary)

### 2.2 文档 (4 件套, ~390 LOC)

- `CONSENSUS_PhaseG_1_2.md`
- `ACCEPTANCE_PhaseG_1_2.md`
- `FINAL_PhaseG_1_2.md` — 本文
- `TODO_PhaseG_1_2.md`

### 2.3 G.1 VRAM TODO P1 标记

- §5 用户 Image / ImageData 跟踪 ✅ (Untrack 缺失 = 已知 Image leak, G.1.7+ 修)
- §6 Font glyph atlas 跟踪 ✅
- §M (新加) Mesh material + Sprite frame 跟踪 ✅
- §7 Mesh VBO/EBO 跟踪 ❌ 留 G.1.3 (worker upload + mutex 化)

---

## 3. CI 验证

**Run**: [26071687897](https://github.com/futzhj/ChocoLightEngine/actions/runs/26071687897)
**结果**: ✅ 6/6 PASS (windows / linux / macos / android / ios / web)
**提交**: 2ac043b "Phase G.1.2 VRAM User Texture Tracking - Image/Font/Mesh/Sprite hooks"

---

## 4. 关键技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Hook 策略 | caller 一行 Track | 中央 helper 要改 9 处签名, ROI 不如直接 +1 行 |
| Worker race | skip, 留 G.1.3 | mutex 化是另一题, 当前 90%+ 主线程占用足够诊断 |
| Format 命名 | R8 / RGB8 / RGBA8 | 与 G.1 已有 RGBA8 风格一致 |
| Category 命名 | "User Image" / "Font atlas" / "Mesh texture" / "Sprite frame" | 简洁 + 用户可读 |
| Image leak 修不修? | **不修** | 反映现实, G.1.7+ Type Safety 时统一修 |
| Mesh/Sprite GC Untrack? | 留 v1.3 | GC 路径分散, 收益不大 (诊断时 Reset 即可) |

---

## 5. 度量

### 5.1 工时
| 阶段 | 估时 | 实际 | 偏差 |
|------|------|------|------|
| 4A 文档 | 0.3h | ~0.3h | 0 |
| T1~T5 实施 | 1.0h | ~0.7h | -30% |
| T6 smoke | 0.4h | ~0.3h | -25% |
| T7 6A 3 件 | 0.3h | ~0.3h | 0 |
| T8 CI | 0.5h | 待 | - |
| **总计** | **~2h** | **~1.6h** | **-20%** |

### 5.2 LOC
```
代码: ~21 LOC (6 文件 / 9 hook 点)
Smoke: ~70 LOC
文档: ~390 LOC
```

### 5.3 性能影响
- Track 单次开销: O(N) 查找 (N ≤ 64) + 2 strcmp ≈ 100 ns. 每 Image / Font 创建一次, 不在帧热路径.
- BytesPerPixel 表扩 2 项: O(常数) 不影响.
- Smoke 全套 < 50ms (本地 vmtxt syntax check).

---

## 6. 零回归

| 项 | 状态 |
|----|------|
| G.1/G.1.1 14 smoke PASS | ✅ |
| 32+ 老 sample syntax | ✅ |
| Phase H.0~H.0.4 完整保留 | ✅ |
| 无新 dependency | ✅ |

---

## 7. 用户使用示例

### 7.1 检查 Image 占用
```lua
local img1 = Light(Light.Graphics.Image):New("hero.png")
local img2 = Light(Light.Graphics.Image):New("enemy.png")
local stats = Light.Graphics.GetMemoryStats()
for _, it in ipairs(stats.items) do
    if it.name == "User Image" then
        print(string.format("Image: %d x %dx%d %s = %.1f KB",
              it.count, it.w, it.h, it.format, it.bytes / 1024))
    end
end
-- 输出: Image: 2 x 1024x768 RGBA8 = 3072.0 KB
```

### 7.2 监控 Font 内存 (含 GC 验证)
```lua
local f = Light(Light.Graphics.Font):New("arial.ttf", 32)
print(Light.Graphics.GetMemoryStats().total_bytes)  -- +1MB (1024x1024 R8 atlas)
f = nil
collectgarbage("collect")
print(Light.Graphics.GetMemoryStats().total_bytes)  -- 回到原始值 (Font.__gc 调 Untrack)
```

### 7.3 GLTF 模型材质统计
```lua
Light.AssetLoader.Preload({
    meshes = {"models/scene.glb"},
}, function(results)
    local stats = Light.Graphics.GetMemoryStats()
    local mesh_tex = 0
    for _, it in ipairs(stats.items) do
        if it.name == "Mesh texture" then
            mesh_tex = mesh_tex + it.bytes
        end
    end
    print(string.format("Loaded scene.glb: %.1f MB material textures", mesh_tex / 1048576))
end)
```

---

## 8. 与其他模块关系

| 模块 | 关系 |
|------|------|
| Phase G.1 VRAM Tracking | 复用 LT::GpuMem API, 0 接口变更 |
| Phase G.1.1 v1.1 (Bloom/SSAO/AE/TAAU) | 完全独立 |
| Phase G.1.5 GLTF Material | 主线程 main-thread upload Track 已加 (worker upload skip) |
| Phase G.1.6 Preload Manifest | 完全独立 (Track 在底层 hook 中, manifest 不感知) |
| Phase G.1.7 Type Safety | 配合 magic 校验; Image __gc 缺失留 G.1.7+ 系列修 |

---

## 9. 后续可做

- **G.1.3**: VBO/EBO 跟踪 + worker thread mutex 化
- **G.1.4**: Mesh / Sprite GC Untrack 配对
- **G.1.x**: ImageData Track (现在只 pixel buffer 无 GPU 资源, 严格说不是 VRAM, 跳过)

---

## 10. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T8 完成 |
