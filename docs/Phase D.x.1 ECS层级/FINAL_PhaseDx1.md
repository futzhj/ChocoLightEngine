# FINAL — Phase D.x.1 ECS 层级 (parent chain)

> **6A 工作流 · 精简交付**
> Transform2D 加 parent 字段, sprite 渲染递归 push parent transform.
> 3D parent (matrix multiply) 留 D.x.1.1.

## 1. 交付内容

### 1.1 Transform2D / Transform3D 增加 `parent` 字段

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:528-533`

```lua
{name='Transform2D', defaults={..., parent=nil}},
{name='Transform3D', defaults={..., parent=nil}},
```

- `parent` 是 entity 引用 (Lua table, 含 `_comps` 字段)
- `nil` = 无 parent (root entity)

### 1.2 `_PushParentChain2D(entity, gfx)` helper

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:612-636`

```lua
function ECSWorld:_PushParentChain2D(entity, gfx)
    -- 1. 从 leaf → root 收集 chain (跳过 self)
    -- 2. 反向 push: 先 root (最外层), 最后 leaf 的 parent
    -- 3. 返回 push 次数, 调用方负责 Pop 平衡
    -- 安全: visited 表防循环 + 32 深度限制
end
```

### 1.3 Render 主循环改动

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:725-732`

```lua
for i = 1, #sprites do
    local item = sprites[i]
    local pushCount = self:_PushParentChain2D(item.entity, gfx)
    self:_DrawSprite(item.tf, item.sprite, gfx)
    for k = 1, pushCount do gfx.Pop() end       -- 平衡 parent chain
end
```

**特点**:
- 不破坏 _DrawSprite 签名 (现有 mock smoke 不受影响)
- parent=nil 时 pushCount=0, 行为与 Phase D 完全一致 (向后兼容)

## 2. 验收

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx1-AC1 | root + 子 entity 渲染时, Push 总次数 = 3 (root self + child parent + child self) | ✅ smoke L407-446 |
| Dx1-AC2 | Translate 顺序: root self → child's parent (root again) → child self | ✅ |
| Dx1-AC3 | 循环引用 (a.parent=b, b.parent=a) 不崩 Render | ✅ smoke L449-466 |
| 回归 | Phase D / D.x.4 / D.x.4.1 / D.x.5 现有 smoke 全过 | ✅ 5 smoke ALL PASS |

## 3. 用户使用

```lua
-- root entity (无 parent, 位置 (100, 200))
local root = world:CreateEntity():Add('Transform2D', {x=100, y=200})
                                  :Add('Sprite',      {image=heroImg})

-- 子 entity (相对 root 局部坐标 (10, 20), 世界坐标 = (110, 220))
local child = world:CreateEntity():Add('Transform2D', {x=10, y=20, parent=root})
                                   :Add('Sprite',      {image=swordImg})

-- 动态改 parent
child._comps.Transform2D.parent = anotherEntity   -- 切换到不同 root
```

## 4. 已知限制 / 后续

| 项 | 严重度 | 处理 |
|----|--------|------|
| 3D parent (Transform3D) 未实现 matrix multiply | 中 | 留 Phase D.x.1.1 (3D 需 4x4 矩阵乘法, 与 2D push/pop stack 不同) |
| parent 是 entity 引用, 不是 entity_id | 低 | userdata 在 JSON 序列化时变 nil, network 同步 client 端需自己重建 parent 关系 |
| 32 深度限制 | 低 | 真实游戏树深度 ≤ 10, 32 足够; 超出时安全终止 |

## 5. 嵌入 Lua 字节统计

| Segment | 之前 | 现在 |
|---------|------|------|
| 1 | 6.7 KB | 6.7 KB |
| 2 | 8.4 KB | 8.4 KB |
| 3 | 12.5 KB | ~13.5 KB |
| 总计 | 28 KB | ~29 KB |

⚠️ Segment 3 达到 13.5KB, 距离 14KB 安全阈值剩 0.5KB. **下次大块改动前必须加新拼接点拆段**.

## 6. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 |
