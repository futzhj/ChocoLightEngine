# FINAL — Phase D.x.6 SpriteBatch (共享 Push/Pop/SetColor)

> **6A 工作流 · 极简交付**
> 加 SpriteBatch component, 共享 transform + color + image, N quad 共 1 Push/Pop/SetColor.

## 1. 交付内容

### 1.1 SpriteBatch 内置 component

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:548-551`

```lua
{name='SpriteBatch', defaults={
    image=nil,
    quads={},                                        -- list of {x, y, qx, qy, qw, qh}
    color={r=1, g=1, b=1, a=1},
    visible=true,
}}
```

### 1.2 `_DrawSpriteBatch(tf, batch, gfx)`

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:702-723`

```lua
function ECSWorld:_DrawSpriteBatch(tf, batch, gfx)
    if not batch.image or not batch.quads then return end
    gfx.Push()
    gfx.Translate(tf.x, tf.y, 0)
    -- ... 共享 rotate / scale ...
    gfx.SetColor(...)                                -- 1 次, 共享
    for q in batch.quads do
        gfx.DrawQuad(batch.image, q.x, q.y, 0, q.qx, q.qy, q.qw, q.qh)
    end
    gfx.Pop()
end
```

### 1.3 Render 主循环加 batch 渲染

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:821-830`

在 sprite 循环之后, 仍属 2D 阶段 (camera transform 共享). 支持 parent chain.

## 2. 性能分析: N quad batch vs N 单 Sprite

### 2.1 Lua → C 调用数 (每帧)

| 操作 | N 单 Sprite | 1 SpriteBatch | 节省 |
|------|-------------|---------------|------|
| Push | N | 1 | N-1 |
| Translate | N | 1 | N-1 |
| (rotate/scale 仅非 default 才调) | N max | 1 max | N-1 |
| SetColor | N | 1 | N-1 |
| DrawQuad | N | N | 0 |
| Pop | N | 1 | N-1 |
| **总计** | ~6N | ~4 + N | **5N − 4** |

### 2.2 100-quad batch 对比

- 100 单 Sprite: ~600 Lua→C 调用
- 1 SpriteBatch: ~104 Lua→C 调用
- 节省 ~83% lua→C 调用开销

### 2.3 实测场景: 1000 粒子, 60fps

- 之前 (1000 Sprite): 6000 Lua→C 调用/帧 = 360k/s
- 现在 (1 SpriteBatch 1000 quad): 1004 Lua→C 调用/帧 = 60k/s
- 节省 **6×** Lua→C 调用

> **注**: 这是 lua-side overhead 节省. GPU draw call 仍是 N 次 (每 DrawQuad 1 个 glDrawElements). 真 GPU batch (VBO 合并) 需 C++ 后端支持, 留 Phase D.x.6.1.

## 3. 验收

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx6-AC1 | 3 quad SpriteBatch: Push=1, Pop=1 (共享) | ✅ smoke L488-516 |
| Dx6-AC2 | SetColor 仅 1 次 (vs 3 次 单 Sprite) | ✅ |
| Dx6-AC3 | 3 quad → 3 DrawQuad 调用 | ✅ |
| Dx6-AC4 | DrawQuad 参数 (x/y/qx/qy) 正确传递 | ✅ |
| Dx6-AC5 | Transform2D 一次 Translate 共享给所有 quad | ✅ |
| 回归 | Phase D / D.x.{1,1.1,1.2,4,4.1,5,5.1} 现有 smoke 全过 | ✅ 5 smoke ALL PASS |

## 4. 用户使用示例

### 4.1 UI grid (10x10 = 100 tile)

```lua
local quads = {}
local atlas = Image.Load("ui_atlas.png")
for row = 0, 9 do
    for col = 0, 9 do
        quads[#quads + 1] = {
            x = col * 32, y = row * 32,
            qx = (col % 8) * 32, qy = (row % 8) * 32,
            qw = 32, qh = 32,
        }
    end
end
world:CreateEntity()
     :Add('Transform2D', {x=100, y=100})
     :Add('SpriteBatch', {image=atlas, quads=quads})
```

### 4.2 粒子系统 (1000 particle)

```lua
-- 每帧 update quads (粒子位置变), 1 个 SpriteBatch entity
function ParticleSystem:update(dt, batch)
    for i, p in ipairs(self.particles) do
        batch.quads[i].x = p.x
        batch.quads[i].y = p.y
    end
end
```

## 5. 嵌入 Lua 字节统计

| Segment | 之前 | 现在 |
|---------|------|------|
| 1 | ~7.2 KB | ~7.2 KB |
| 2 | 8.4 KB | 8.4 KB |
| 3 | ~8.3 KB | ~9.2 KB |
| 4 | 7.1 KB | 7.1 KB |

距 14KB 阈值充裕.

## 6. 已知限制 / 后续

| 项 | 严重度 | 处理 |
|----|--------|------|
| 仍 N 次 DrawQuad → N glDrawElements | 中 | 留 Phase D.x.6.1 (C++ VBO batch API) |
| quads 内每 item 不可单独设 color | 低 | 用户需要时拆为多个 SpriteBatch (每色 1 个) |
| 不支持 individual anchor/flip per quad | 低 | 拆为多 SpriteBatch 或回退到 Sprite |

## 7. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 |
