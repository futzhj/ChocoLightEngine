# FINAL — Phase D.x.1.2 普通 3D MeshRenderer parent chain

> **6A 工作流 · 极简交付**
> 把 D.x.1 (2D Push/Pop chain) 镜像到 3D MeshRenderer.
> 与 D.x.1.1 SkinnedMesh (matrix-based) 互补.

## 1. 交付内容

### 1.1 `_PushParentChain3D(entity, gfx)`

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:638-664`

```lua
function ECSWorld:_PushParentChain3D(entity, gfx)
    -- 1. 收集 parent chain (跳过 self)
    -- 2. 反向 push: 先 root 最外层, leaf 的 parent 最内层
    -- 3. 用 Translate(x,y,z) + Rotate*3 + Scale(x,y,z) 三轴
    -- 4. 返回 push 次数, 调用方 Pop 平衡
    -- 安全: visited 表 + 32 深度限制
end
```

### 1.2 Render 主循环钩入

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:780-789`

```lua
for _, e in ipairs(self._entities) do
    local tf = e._comps.Transform3D
    local mr = e._comps.MeshRenderer
    if tf and mr and mr.visible ~= false and mr.mesh then
        local pushCount = self:_PushParentChain3D(e, gfx)
        self:_DrawMesh(tf, mr, gfx)
        for k = 1, pushCount do gfx.Pop() end
    end
end
```

不破坏 _DrawMesh 签名, 现有 mock smoke 全过.

## 2. 与 SkinnedMesh 路径区别

| 维度 | 普通 Mesh (D.x.1.2) | SkinnedMesh (D.x.1.1) |
|------|---------------------|------------------------|
| 渲染 API | `mesh:Draw(matOrTexId)` | `Light.Animation.DrawSkinnedMesh(mesh, animator, modelMat, mat)` |
| modelMat | gfx stack 隐式 (Push/Translate/...) | 显式 16-element table |
| parent chain | gfx.Push * N (递归 push) | matrix multiply * N |
| 兼容性 | 与现有 _DrawMesh 共存 | _DrawSkinnedMesh 重构 (但兼容 tf-table 调用) |

## 3. 验收

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx1.2-AC1 | child 3D mesh 有 parent 时, Push 数 = parent_count + self | ✅ smoke L450-484 |
| Dx1.2-AC2 | Translate 顺序: parent 先, self 后 | ✅ |
| Dx1.2-AC3 | Pop 数与 Push 平衡 | ✅ |
| 回归 | Phase D / D.x.1 / D.x.1.1 现有 smoke 全过 | ✅ 5 smoke ALL PASS |

## 4. 现在 ECS 层级支持完整

| 类型 | parent 支持 | 模式 |
|------|-------------|------|
| Sprite (Transform2D) | ✅ D.x.1 | gfx Push/Pop chain |
| MeshRenderer (Transform3D) | ✅ **D.x.1.2** | gfx Push/Pop chain |
| SkinnedMeshRenderer (Transform3D) | ✅ D.x.1.1 | matrix multiply chain |
| Camera2D / Camera3D (Transform 都是 root) | N/A | (相机一般无 parent) |

## 5. 嵌入 Lua 字节统计

| Segment | 之前 (D.x.1.1) | 现在 |
|---------|---------------|------|
| 1 | 6.7 KB | 6.7 KB |
| 2 | 8.4 KB | 8.4 KB |
| 3 | 6.7 KB | ~7.5 KB |
| 4 | 7.1 KB | 7.1 KB |

仍距 14KB 阈值有充裕余量.

## 6. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 |
