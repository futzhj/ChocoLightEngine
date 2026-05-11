# FINAL — Phase D.x.1.1 3D Transform parent (matrix multiply)

> **6A 工作流 · 极简交付**
> 把 D.x.1 (2D Push/Pop chain) 扩展到 3D, 用 matrix multiply 实现 parent chain.

## 1. 交付内容

### 1.1 raw string 拼接点拆段 (关键)

Segment 3 已达 15.0KB, **1KB 距 16KB 硬限制**. 立即加拼接点拆分:
- 在 Render 函数后 (line 772) 加 `)LUA" R"LUA(`
- 结果: 4 段 raw string, 各 6.7 / 8.4 / 9.3 / 7.1 KB, 全部 < 10KB

### 1.2 `_Mat4Multiply(a, b)` 4x4 列主序矩阵乘法

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:776-788`

local function, 16 个累加, ~64 mul + 48 add 每次调用.

### 1.3 `_LocalMatrix3D(tf)` + `_BuildModelMatrix3D(arg)` + `_GetWorldMatrix3D(entity, ...)`

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:790-849`

```lua
-- Local matrix only (无 parent)
function ECSWorld:_LocalMatrix3D(tf) ... end

-- World matrix (含 parent chain)
-- 智能检测: arg 含 _comps.Transform3D → entity 模式 (parent chain);
--           否则 → tf table 模式 (旧调用向后兼容)
function ECSWorld:_BuildModelMatrix3D(arg) ... end

-- 递归取 world matrix (含循环保护 visited + 32 深度限制)
function ECSWorld:_GetWorldMatrix3D(entity, visited, depth) ... end
```

### 1.4 `_DrawSkinnedMesh` 签名变 (entity, smr)

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:882-889`

```lua
-- 之前: function ECSWorld:_DrawSkinnedMesh(tf, smr)
function ECSWorld:_DrawSkinnedMesh(entity, smr)
    local model = self:_BuildModelMatrix3D(entity)  -- 触发 parent chain
    pcall(Anim.DrawSkinnedMesh, smr.mesh, smr.animator, model, smr.material)
end
```

Render 主循环也相应改为传 `e` 而非 `tf` (`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:760-768`).

## 2. 验收

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx1.1-AC1 | 3D parent translation 累加正确 (root + child = world) | ✅ smoke L375-391 |
| Dx1.1-AC2 | 旧 tf-table 调用向后兼容 (Dx4.1-AC3 不破坏) | ✅ smoke L393-404 |
| Dx1.1-AC3 | 3D parent 循环引用保护 (visited + depth limit) | ✅ smoke L406-418 |
| 回归 | Phase D / Phase D.x.4 / D.x.4.1 / D.x.5 / D.x.1 现有 smoke 全过 | ✅ 5 smoke ALL PASS |

## 3. 关键设计: 双模式 _BuildModelMatrix3D

为不破坏 Phase D.x.4.1 smoke (Dx4.1-AC3 用 `w:_BuildModelMatrix3D({x=10,y=20,z=30})` 直接传 tf table), 检测参数类型:
- `arg._comps == nil` → tf-table 模式 (旧行为, 单调 _LocalMatrix3D)
- `arg._comps.Transform3D` 存在 → entity 模式 (新行为, 递归 parent chain)

## 4. 嵌入 Lua 字节统计

| Segment | 之前 | 现在 |
|---------|------|------|
| 1 | 6.7 KB | 6.7 KB |
| 2 | 8.4 KB | 8.4 KB |
| 3 | 15.0 KB ⚠️ | **6.7 KB** ✅ |
| 4 (新) | - | **7.1 KB** ✅ |
| 总计 | 29 KB | ~29 KB |

危险解除. 后续 D.x.x 改动有充裕空间.

## 5. 已知限制

- 仅 SkinnedMeshRenderer 享受 parent matrix multiply
- `_DrawMesh` (普通 3D mesh) 仍用 gfx.Push/Translate stack 模式, **未支持 parent**. 留 Phase D.x.1.2 (matrix-based _DrawMesh 重写)

## 6. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 |
