# FINAL — Phase D.x.8 TextRenderer (ECS 文字渲染)

> **6A 工作流 · 极简交付**
> 加 TextRenderer component, ECS 场景中渲染文字 (调底层 `gfx.Print`).

## 1. 交付内容

### 1.1 TextRenderer 内置 component

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:552-554`

```lua
{name='TextRenderer', defaults={
    text='',                                  -- 文本字符串 (支持 UTF-8 / CJK)
    font=nil,                                 -- Light.Graphics.Font instance (userdata 包装 table)
    color={r=1, g=1, b=1, a=1},
    visible=true,
}}
```

### 1.2 `_DrawText(tf, tr, gfx)` helper

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:793-800`

```lua
function ECSWorld:_DrawText(tf, tr, gfx)
    if not tr.font or not tr.text or tr.text == '' then return end
    local col = tr.color or {}
    gfx.SetColor(col.r or 1, col.g or 1, col.b or 1, col.a or 1)
    gfx.Print(tr.text, tr.font, tf.x or 0, tf.y or 0, 0)
end
```

**为什么不 Push/Pop**: 底层 `l_Print` (`light_graphics.cpp:802-874`) 内部已经做了
`PushMatrix → Translate(x,y,z) → ApplyTransform → ... → PopMatrix`. 上层重复 Push 会导致
parent transform 被 Print 覆盖.

**为什么 SetColor 在 Print 外**: `l_Print` 用全局 `g_ctx.drawColor` (通过 `ApplyDrawColor()`),
不接受 color 参数. 必须在 Print 前 `gfx.SetColor` 改全局 state.

### 1.3 Render 主循环加 TextRenderer 渲染

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:940-949`

```lua
-- Phase D.x.8: TextRenderer 渲染 (在 sprite/batch 之后, UI 文字覆盖)
for _, e in ipairs(self._entities) do
    local tf = e._comps.Transform2D
    local tr = e._comps.TextRenderer
    if tf and tr and tr.visible ~= false and tr.text and tr.text ~= '' and tr.font then
        local pushCount = self:_PushParentChain2D(e, gfx)
        self:_DrawText(tf, tr, gfx)
        for k = 1, pushCount do gfx.Pop() end
    end
end
```

**渲染顺序**: Sprite → SpriteBatch → **TextRenderer** (UI 文字在最上层)

## 2. 用户使用示例

```lua
-- 加载字体 (Light.Graphics.Font 是 OOP class, :New 创建 instance)
local font = Light(Light.Graphics.Font):New("font.ttf", 24)

-- 创建文字 entity
world:CreateEntity()
     :Add('Transform2D', {x=400, y=300})         -- 屏幕中心
     :Add('TextRenderer', {
         text  = "Hello, 世界!",                 -- UTF-8 支持 CJK
         font  = font,
         color = {r=1, g=0.9, b=0.3, a=1},       -- 金黄色
     })

-- 修改文本 (引擎下帧自动渲染新值)
entity._comps.TextRenderer.text = "Score: " .. tostring(score)
```

### Parent chain (UI 嵌套)

```lua
-- panel 是父 entity, 文字相对 panel 定位
local panel = world:CreateEntity():Add('Transform2D', {x=100, y=100})

world:CreateEntity()
     :Add('Transform2D', {x=10, y=20, parent=panel})  -- panel 偏移
     :Add('TextRenderer', {text="Title", font=font})
```

## 3. 验收

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx8-AC1 | TextRenderer 调 SetColor + gfx.Print 各 1 次 | ✅ smoke L634-670 |
| Dx8-AC1 | Print 参数 (text, font, x, y, z) 正确传递 | ✅ |
| Dx8-AC1 | color (r/g/b/a) 正确从 component 传 SetColor | ✅ |
| Dx8-AC2 | font=nil / text="" / visible=false 全部正确跳过 | ✅ smoke L675-701 |
| Dx8-AC3 | TextRenderer 与 parent chain 协作 (Push/Pop 平衡) | ✅ smoke L706-728 |
| 回归 | Phase D / D.x.{1, 1.1, 1.2, 4, 4.1, 5, 5.1, 5.2, 5.3, 6} 现有 smoke 全过 | 🟡 待 CI |

## 4. 设计权衡

### 4.1 为何不加 size / fontSize 字段?
- Font instance 创建时已绑 size (`Light(Font):New("font.ttf", 24)`)
- 不同 size 要不同 Font instance (atlas 内部按 size 烘焙)
- 改 size 通过换 Font instance, 不是 component 字段

### 4.2 为何不加 alignment / wrap?
- 底层 `gfx.Print` 不支持 alignment / wrap (单行简单渲染)
- 留 Phase D.x.8.1: 基于 `stbtt_GetCodepointHMetrics` 计算 text width, 加 anchor 字段
- 多行换行: 用户自行 split string + 多个 TextRenderer 解决

### 4.3 为何不参与 frustum cull (D.x.5.2/5.3)?
- 文字宽度难精确算 (UTF-8 + 字距), 简单 fallback 不 cull 安全
- UI 文字数量通常少 (< 50), cull 收益小
- 后续可加: 用 `gfx.GetTextWidth(font, text)` (待加底层 API) 算 AABB

## 5. 嵌入 Lua 字节统计

| Segment | 之前 (Dx5.3) | 现在 (Dx8) |
|---------|--------------|-----------|
| 1 | 7.4 KB | 7.4 KB |
| 2 | 8.4 KB | 8.4 KB |
| 3 | 7.5 KB | 7.8 KB |
| 4 | 8.6 KB | 9.4 KB |
| 5 | 7.1 KB | 7.1 KB |

距 16KB 硬限制 6+ KB 余量.

## 6. 已知限制

| 项 | 严重度 | 处理 |
|----|--------|------|
| 不参与 frustum cull | 低 | UI 数量少, 留 D.x.8.x |
| 不支持 alignment / wrap | 中 | 留 D.x.8.1 (基于 GetTextWidth) |
| Font 必须用户自己加载 | 已支持 | `Light.Graphics.Font:New(path, size)` |
| 没字号字段 | 中 | 通过换 Font instance 实现 |
| color SetColor 影响全局 state | 中 | 后续 sprite 若不显式 SetColor, 会承袭此 color |

## 7. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 |
