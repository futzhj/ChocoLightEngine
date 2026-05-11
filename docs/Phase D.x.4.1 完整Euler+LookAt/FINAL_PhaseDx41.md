# FINAL — Phase D.x.4.1 完整 Euler + LookAt

> **6A 工作流 · 合并 Stage**
> Phase D.x.4 基础上补齐: 完整 ZYX Euler 矩阵 / LookAt helper / AnimationState.looping 桥接.

## 1. 状态总览

| 维度 | 状态 |
|------|------|
| atomic tasks | **3/3 完成** (Dx4.1-T1~T3 实施 + smoke + 文档) |
| smoke 验证 | `ecs_skinned.lua` 加 3 个新断言, ALL PASS |
| 回归 4 个 smoke | ecs_render / ecs_network / physics_3d / image_from_bytes 全过 |
| 实际耗时 | ~0.5h (估时 2.5h, 大部分代码逻辑直接) |

## 2. 交付改动

### 2.1 `_BuildModelMatrix3D` 升级 — 完整 ZYX Euler

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:725-755`

```lua
-- M = T * Rz * Ry * Rx * S (列主序)
function ECSWorld:_BuildModelMatrix3D(tf)
    local rx, ry, rz = ...  -- 单位度
    -- R = Rz * Ry * Rx 展开 (9 个元素), 应用 scale, 加 translation
    return {16 elements column-major}
end
```

**支持**: 三轴独立旋转 (rx/ry/rz) + 三轴独立缩放 (sx/sy/sz) + 平移 (x/y/z).

### 2.2 `ECS.LookAt` helper

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:759-786`

```lua
local viewMat = Light.ECS.LookAt(eyeX, eyeY, eyeZ,
                                   targetX, targetY, targetZ,
                                   upX, upY, upZ)
-- 返回 16-element 列主序 view matrix
-- 用法: 直接传给 Light.Graphics.SetCamera 的特殊扩展, 或用户自己 transform 顶点
```

**实现细节**:
- forward = normalize(target - eye)
- right = normalize(forward × up)
- up' = right × forward (重新正交化)
- 处理零向量退化 (1e-10 阈值)

### 2.3 AnimationState.looping 桥接

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:835-839`

```lua
-- 在 _AnimationSystem 内, 检测 looping 变化 → animator:SetLooping(b)
if as.looping ~= cache.lastLooping and type(an.SetLooping) == 'function' then
    pcall(an.SetLooping, an, as.looping and true or false)
    cache.lastLooping = as.looping
end
```

## 3. 验收

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx4.1-AC1 | looping 字段桥接 SetLooping | ✅ smoke L292-312 |
| Dx4.1-AC2 | ECS.LookAt 16-element 列主序矩阵 | ✅ smoke L314-330 |
| Dx4.1-AC3 | _BuildModelMatrix3D 完整 ZYX (identity + 平移 + rx=90 + 缩放) | ✅ smoke L332-372 |
| 回归 | Phase D / Phase D.x.4 现有 smoke 全过 | ✅ |

## 4. 嵌入 Lua 字节统计

| Segment | 之前 | 现在 |
|---------|------|------|
| 1 | 6.7 KB | 6.7 KB |
| 2 | 8.4 KB | 8.4 KB |
| 3 | 11.0 KB | ~12.5 KB |
| 总计 | 26.7 KB | ~28 KB |

距 13KB 单段阈值仍有 0.5KB 余量, 但 Phase D.x.4.2 前应主动加新拼接点.

## 5. 后续 Phase 建议

| Phase | 主题 | 工作量 |
|-------|------|--------|
| Phase D.x.4.2 | 多 Animator 共享 Skeleton (角色 LOD) | 3-4h |
| Phase D.x.5 | _FindActiveCamera 缓存 + dirty sprite list | 2-3h |
| Phase D.x.1 | parent 层级 + 递归世界矩阵 | 2-3h |

## 6. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 (与 Phase D.x.4 同 session 交付) |
