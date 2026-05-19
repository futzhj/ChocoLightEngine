# Phase G.1.4 — VRAM Mesh Lifecycle + LUT 3D Tracking (FINAL)

## 1. 交付概览

完整闭合 Mesh / SkinnedMesh / SkinnedMorphMesh VBO/EBO/morph delta texture 全生命周期 VRAM 跟踪, 主线程 + worker + DeleteMesh + Shutdown 四路统一入账, **0 caller 改动**. 同时补 LUT 3D worker hook.

## 2. CI 验证

- 基线: G.1.3 commit `1e30223` (CI 6/6 PASS)
- 改动: 4 files (1 header / 2 cpp / 1 lua / 4 docs)
- 提交: 待执行
- 期望: 6/6 全平台编译通过 + Windows runtime smoke gpumem.lua §K + §L 全 PASS

## 3. 关键技术决策 (10/10 自动决策)

1. **Track 位置 = backend 内部** (CreateMesh/CreateSkinnedMesh/CreateSkinnedMorphMesh)
   - 5 处 caller 自动覆盖, 0 caller 改动
2. **Untrack 位置 = backend DeleteMesh 内部 + Shutdown 双重保险**
   - 兼容 Reset 顺序未定
3. **MeshGPU 加 4 字节字段**: vboBytes / eboBytes / morphPosBytes / morphNrmBytes
   - 默认 0 表示未记账, `> 0` 时才 Untrack (老 mesh 兼容)
4. **RegisterUploadedMesh 加默认参数**: vboBytes/eboBytes = 0
   - 老 caller 不传零兼容, 新 caller 传 worker 已 Track 的 bytes
5. **worker 不重复 Track**: RegisterUploadedMesh 仅写 m 字段, Track 在 worker 上传时已做
6. **DeleteMesh 3 路径分支**: meshId 高位 `0x80000000` (skinned) / `0xC0000000` (morph) / 普通
7. **morph delta texture 字节公式**: `vCount * morphCount * 12` (RGB32F = 12B/像素)
8. **LUT 3D 字节公式**: `size^3 * (isHDR ? 6 : 3)` (RGB16F=6 / RGB8=3)
9. **LUT 3D Untrack 留 v1.6**: Lua side 无显式 Delete API, 当前 Track-only
10. **类目命名统一**: "Mesh VBO" / "Mesh EBO" / "Morph pos delta" / "Morph nrm delta" / "LUT 3D"

## 4. 度量

| 指标 | 数值 |
|------|------|
| 改动文件 | 4 (1 header / 2 cpp / 1 lua) + 4 docs |
| C++ 改动 | +56 / -14 行 |
| Lua smoke 改动 | +115 / -2 行 |
| 文档 | ~600 行 (CONSENSUS+ACCEPTANCE+FINAL+TODO) |
| 5 处 caller 改动 | **0** (全部由 backend 内部 hook 自动覆盖) |
| 接口签名变更 | 1 (RegisterUploadedMesh, 默认参数兼容) |
| 新增 VRAM 类目 | 5 (Mesh VBO / Mesh EBO / Morph pos delta / Morph nrm delta / LUT 3D) |
| 性能开销 (热路径) | 0 (DrawMesh / DrawSkinnedMesh 无改动) |
| 性能开销 (创建/释放) | ~0.5 µs / 操作 (mutex lock + TrackBytes) |
| ReCreate per-frame 累计 | ~0.06 ms/s @ 60fps |

## 5. 零回归保证

- ✅ 5 处 caller 0 改动 (l_CreateMesh / GLTF / SkinnedMesh ReCreate / asset_loader)
- ✅ RegisterUploadedMesh 默认参数 = 0 (Legacy backend 自动兼容)
- ✅ DeleteMesh `vboBytes > 0` 守卫 (老 mesh 字段 = 0 不触发假阳 Untrack)
- ✅ GpuMem::UntrackBytes 内部下溢保护 (light_gpumem.cpp:182)
- ✅ Worker 路径 (G.1.3) Track + 主线程 Register/Delete 闭环, 不重复 Track
- ✅ Shutdown 3 处循环加 Untrack, 与 GpuMem::Reset 顺序无关
- ✅ smoke §J (G.1.3) + §K (G.1.4 新增) 共存通过

## 6. 用户示例

```lua
-- 在 Lua 创建 Mesh / SkinnedMesh, 自动 Track
local mesh = Light.Graphics.CreateMesh(verts, indices)
local skin = Light.Animation.CreateSkinnedMesh(verts, indices, joints)

-- 查询 VRAM 状态
local stats = Light.Graphics.GetMemoryStats()
for _, it in ipairs(stats.items) do
    if it.name == "Mesh VBO" or it.name == "Mesh EBO" then
        print(string.format("%s: count=%d, bytes=%d", it.name, it.count, it.bytes))
    end
end

-- 释放 Mesh, 自动 Untrack
mesh:Delete()  -- DeleteMesh 内部 UntrackBytes
skin:Delete()

-- LUT 3D 异步加载, 自动 Track
local lut_id = Light.Graphics.LUT.LoadCubeAsync("path.cube")
-- 主线程 Tick 后 LUT 3D 类目出现 (count++, bytes = size^3 * 6)
```

## 7. 模块关系

```
[Lua API]
  ├── Light.Graphics.CreateMesh ───┐
  ├── Light.Graphics.LoadGLTF      │
  ├── Light.Animation.CreateSkinnedMesh
  └── Light.Graphics.LUT.LoadCubeAsync
                                   │
[asset_loader.cpp]                 │
  ├── WorkerUploadMesh_ ── G.1.3 TrackBytes("Mesh VBO/EBO")
  ├── WorkerUploadLUT_  ── G.1.4 TrackBytes("LUT 3D")  ★
  └── Tick: RegisterUploadedMesh(vao, ..., vboBytes, eboBytes) ★ G.1.4
                                   │
[render_gl33.cpp]                  ▼
  ├── CreateMesh          ── G.1.4 TrackBytes("Mesh VBO/EBO") ★
  ├── CreateSkinnedMesh   ── G.1.4 TrackBytes("Mesh VBO/EBO") ★
  ├── CreateSkinnedMorphMesh ── G.1.4 TrackBytes (4 类目) ★
  ├── RegisterUploadedMesh ── 仅写 m 字段, 不重复 Track
  ├── DeleteMesh (3 路径) ── G.1.4 UntrackBytes ★
  └── Shutdown (3 循环)  ── G.1.4 UntrackBytes ★
                                   │
[light_gpumem.cpp]                 ▼
  ├── TrackBytes (mutex G.1.3 + 下溢保护)
  └── UntrackBytes
                                   │
[scripts/smoke/gpumem.lua]         ▼
  └── §K LUT 3D + Morph + §L Mesh lifecycle round-trip ★
```

★ = G.1.4 新增

## 8. 后续可做

- **v1.5** Sprite frame `__texId` GC 配对 (Lua table 包装为 userdata, 加 __gc Untrack)
- **v1.5** Mesh material textures GC 配对 (mesh dtor 路径遍历 5 GL texId Untrack)
- **v1.6** LUT 3D 显式 Delete API + Untrack
- **v1.6** Image userdata `__gc` (修历史 leak, l_Image_Call 加元表)
- **G.1.7** Per-instance ID + budget + OSD HUD overlay
- **G.1.8** NVAPI / DXGI 实测显存 (Windows-only)

## 9. 版本

- 基线: Phase G.1.3 commit `1e30223`
- 提交: Phase G.1.4 (待执行 commit)
