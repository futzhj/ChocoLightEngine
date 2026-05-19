# Phase G.1.4 — TODO

## 1. 用户配置 (无)

本阶段无需用户配置变更. 现有 Mesh / SkinnedMesh / GLTF / LUT 3D Lua API **零改动**.

## 2. 推荐验证

实机验证 (用户参与):

```lua
-- 1. 加载 GLTF (worker 路径)
local future = Light.Graphics.LoadGLTFAsync("path.glb", true)  -- withMaterial
-- ... 等 future:IsReady() ...

-- 2. 检查 Mesh VBO/EBO 类目
local stats = Light.Graphics.GetMemoryStats()
for _, it in ipairs(stats.items) do
    if it.name == "Mesh VBO" or it.name == "Mesh EBO" then
        print(it.name, it.count, it.bytes)
    end
end

-- 3. 释放, 检查 Untrack 闭合
mesh:Delete()
local stats2 = Light.Graphics.GetMemoryStats()
-- 期望: Mesh VBO/EBO count 减少 1

-- 4. 加载 LUT 3D
Light.Graphics.LUT.LoadCubeAsync("lut.cube")
-- ... future ready ...
-- 期望: GetMemoryStats 出现 "LUT 3D" 类目, format=BYTES, bytes > 0

-- 5. SkinnedMesh ReCreate per-frame 不漂
for i = 1, 60 do
    Light.Animation.UpdateAll(0.016)
    -- 内部 SkinnedMesh ReCreate per-frame
end
-- 期望: Mesh VBO count 不单调增长 (每帧 Track / Untrack 配对)
```

## 3. 已知限制

| 限制 | 影响 | 后续 |
|------|------|------|
| LUT 3D 无 Untrack | LUT 持续 Track, 进程结束才清 | v1.6 加显式 Delete API |
| Sprite frame __gc 缺失 | Lua table __texId 释放无 hook | v1.5 |
| Mesh material textures 释放路径 | mesh dtor 不遍历 5 GL texId Untrack | v1.5 |
| Image userdata __gc 缺失 | l_Image_Call 不设元表, 历史 leak | v1.6 |
| litMesh / 2D Lit pool | 未加 Track | 优先级低, 待评估 |

## 4. 增强候选 (按 ROI 排序)

### 🔥 高 ROI

1. **v1.5 Sprite frame __gc 配对** (~2h)
   - Lua table → userdata wrapper, __gc 调 DeleteTexture + Untrack
   - 闭合 Sprite frame 释放路径

2. **v1.5 Mesh material textures GC** (~1.5h)
   - MeshUserdata 持 5 GL texId 列表, mesh:Delete() 遍历 Untrack
   - 闭合 Mesh material 释放路径

### 🟡 中 ROI

3. **v1.6 LUT 3D 显式 Delete + Untrack** (~1h)
   - `Light.Graphics.LUT.Delete(id)` 调 DeleteTexture3D + UntrackBytes("LUT 3D", bytes)
   - 需 backend 加 DeleteTexture3D 接口

4. **v1.6 Image userdata __gc fix** (~1h, G.1.7+ Type Safety 系列范畴)
   - l_Image_Call 加元表 with __gc, 修历史 leak

### ⚪ 低 ROI

5. **G.1.7 Per-instance ID + budget + OSD HUD** (~5h)
6. **G.1.8 NVAPI / DXGI 实测显存** (~3h, Windows-only)

## 5. 文档状态

- ✅ CONSENSUS_PhaseG_1_4.md (设计 + 任务拆分)
- ✅ ACCEPTANCE_PhaseG_1_4.md (验收 + 风险)
- ✅ FINAL_PhaseG_1_4.md (交付总结 + 度量)
- ✅ TODO_PhaseG_1_4.md (本文件)
- ✅ CI run `26074068905` 6/6 success (build-web / build-ios / build-linux / build-android / build-windows / build-macos)
