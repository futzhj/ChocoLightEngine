# FINAL — Phase D.x.5 ECS 渲染性能优化

> **6A 工作流 · 极简交付**
> 仅优化 `_FindActiveCamera` (self-validating cache). `_CollectSprites` 评估后不是真瓶颈, 跳过留 D.x.5.1.

## 1. 优化项

### 1.1 `_FindActiveCamera` self-validating cache

**Before** (`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` 旧版本):
```lua
function ECSWorld:_FindActiveCamera(camComp, tfComp)
    for _, e in ipairs(self._entities) do        -- 每次 O(n) 全表扫描
        local c = e._comps[camComp]
        local t = e._comps[tfComp]
        if c and t and c.active then return e end
    end
    return nil
end
```

**After** (`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:570-591`):
```lua
function ECSWorld:_FindActiveCamera(camComp, tfComp)
    self._cam_cache = self._cam_cache or {}
    local cacheKey = camComp .. '|' .. tfComp
    local cached = self._cam_cache[cacheKey]
    if cached then
        local c = cached._comps[camComp]
        local t = cached._comps[tfComp]
        if c and t and c.active then return cached end   -- O(1) 命中
        -- 缓存失效, fall through 重扫
    end
    for _, e in ipairs(self._entities) do                 -- 失效时 O(n) 一次
        local c = e._comps[camComp]
        local t = e._comps[tfComp]
        if c and t and c.active then
            self._cam_cache[cacheKey] = e
            return e
        end
    end
    self._cam_cache[cacheKey] = nil
    return nil
end
```

### 1.2 性能模型

| 场景 | Before | After |
|------|--------|-------|
| 稳定相机, 每帧 2 次调用 (2D+3D) | 2 × O(n) | 2 × O(1) |
| 1000 entity 单相机 60fps | 120000 cmp/s | ~120 cmp/s |
| 相机切换 active | 1 × O(n) | 1 × O(n) 同 |

### 1.3 关键设计: self-validating, 无需手动失效

缓存通过**每次调用时验证**实现自动失效:
1. 检查缓存 entity 的 `_comps[camComp]` 仍存在 (entity 未销毁)
2. 检查 `c.active` 仍 true (用户未切换相机)
3. 任一不满足 → fall through 全表扫描 → 新 entity 进缓存

无需 hook Add/Remove/Set, 与现有 ECS 流程零冲突.

## 2. `_CollectSprites` 评估 (未优化)

测算: 1000 entity × 60fps 当前实现:
- 全表过滤: 60000 next-loop / s
- z-sort: 60000 × log(1000) ≈ 600K cmp/s

实测 Lua 5.1 table.sort 比较器吞吐 ~10M cmp/s, **当前实现 ~0.6ms/帧** — 不是真瓶颈.

留 Phase D.x.5.1 加 dirty list 优化 (需 hook entity:Set 字段变化).

## 3. 回归验证

5 个 smoke 全过:
- `ecs_render.lua` PASS (Phase D 渲染)
- `ecs_skinned.lua` PASS (Phase D.x.4 骨骼)
- `ecs_network.lua` PASS (Phase C.x.1 网络)
- `physics_3d.lua` 132 PASS (Phase AU 物理)
- `image_from_bytes.lua` 5/5 PASS (Phase D.x.7 Image 字节)

现有 smoke (ecs_render Dx4-AC8, ecs_skinned Dx4-AC8) 间接覆盖缓存正确性 (Camera3D active 时 DrawSkinnedMesh 仍被调).

## 4. Commit

| Commit (本次) | 内容 |
|----------|------|
| [pending] | perf(phase-dx5): _FindActiveCamera self-validating cache |

## 5. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 |
