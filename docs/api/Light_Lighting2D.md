# Light.Lighting2D

> Phase E.1 — 2D forward 多光照状态管理

管理 2D 渲染的多光照状态：最多 16 个 Point/Spot lights + 全局 ambient + 全局 enabled 开关。配合 `Light.Graphics.DrawLit` / `Light.Graphics.DrawLitQuad`（或 ECS `LitSprite` component）使用，shader 端走 `sprite_lit_2d` forward 渲染。

```lua
local L2D = require("Light.Lighting2D")
L2D.SetAmbient(0.2, 0.2, 0.2)
local id = L2D.AddPointLight{x=200, y=100, color={r=1,g=0.8,b=0.5}, range=400}
-- ... 在某 sprite 渲染前
Light.Graphics.DrawLit(image, normalMap, x, y)
```

---

## 设计契约

### 坐标空间

- **直接调 Lua API**：`x, y` 应与 sprite 渲染时的 vertex space 一致。如果没有 camera transform，就是 world space；如果在 `gfx.Translate(-cx, -cy)` 之后调，应传 camera-relative space。
- **走 ECS（推荐）**：`Light2D.x/y` 是 **world space**，`world:Render()` 内部 `_UploadLights2D` 自动 `view_x = (world_x - cam_x) * zoom` 转换。

### slot 与 id 空间

- 16 个 slot，id ∈ `[1, 16]`，`0` 保留给"失败"返回值
- `Add*` 挑第一个 INACTIVE slot；`Remove` 把对应 slot 置 INACTIVE（slot 可被后续 `Add*` 复用）
- 不保证 id 连续；调用方应保存 `Add*` 返回的 id 用于后续 `Update / Remove`

### Update 部分字段更新语义

C++ 层 `Update` 是**全字段覆盖**；Lua 层 `UpdateLight(id, fields)` 是**部分字段更新**——只覆盖 `fields` 中明确传入的字段（基于 `lua_isnil` 守卫），缺失字段保留原值。`type` 字段不支持通过 `UpdateLight` 切换（Point↔Spot 需 `Remove` + `Add*`）。

### 角度单位

`innerAngle / outerAngle` 单位为 **度**。binding 内部 `cosf(deg * π / 180)` 转换后存到 C++ `innerCos / outerCos`，Lua 端不暴露 cos 接口。

### Spot 方向归一化

`AddSpotLight` 内部对 `dirX, dirY` 调 `Normalize2D` 归一化；零向量 fallback 为 `(1, 0)`，避免 shader 拿到 NaN。`UpdateLight` 在 type=Spot slot 上也会重新归一化。

---

## 常量

| 常量 | 值 | 备注 |
|------|----|------|
| `Light.Lighting2D.MAX_LIGHTS` | `16` | 硬上限，与 `sprite_lit_2d` shader 内 `uLight*[16]` 一致 |
| `Light.Lighting2D.TYPE_POINT` | `1` | 点光（忽略 dir / innerCos / outerCos） |
| `Light.Lighting2D.TYPE_SPOT` | `2` | 聚光（用 dir + smoothstep cone falloff） |

---

## `Light.Lighting2D.SetEnabled`

切换全局 enabled 开关

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `enabled` | `boolean` | true=启用 Lit 路径 / false=禁用 |

### 返回值

`void`

---

## `Light.Lighting2D.IsEnabled`

查询全局 enabled 状态

### 返回值

`boolean`

---

## `Light.Lighting2D.SetAmbient`

设置全局环境光

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `r` | `number` | Red ∈ [0, 1] |
| `g` | `number` | Green |
| `b` | `number` | Blue |

### 返回值

`void`

---

## `Light.Lighting2D.GetAmbient`

查询当前环境光

### 返回值

`number, number, number` — r, g, b

---

## `Light.Lighting2D.AddPointLight`

添加一个点光到第一个空闲 slot

### 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `x, y` | `number` | `0, 0` | 世界坐标（详见"坐标空间"契约） |
| `color` | `{r, g, b}` | `{1, 1, 1}` | nested table（与 Sprite component 风格一致） |
| `range` | `number` | `200` | 距离衰减半径；`d > range` 时 shader 跳过 |
| `intensity` | `number` | `1.0` | 强度乘子 |

### 返回值

| 场景 | 返回 |
|------|------|
| 成功 | `number` — id ∈ [1, 16] |
| 16 个 slot 满 / type 非法 | `nil, "lights full or invalid type"` |

### 示例

```lua
local id = Light.Lighting2D.AddPointLight{
    x = 200, y = 100,
    color = {r=1.0, g=0.8, b=0.5},
    range = 400,
    intensity = 1.5,
}
if not id then error("lights full") end
```

---

## `Light.Lighting2D.AddSpotLight`

添加一个聚光到第一个空闲 slot

### 参数

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `x, y` | `number` | `0, 0` | 位置 |
| `dirX, dirY` | `number` | `1, 0` | 朝向（内部归一化） |
| `color` | `{r, g, b}` | `{1, 1, 1}` | 颜色 |
| `range` | `number` | `200` | 距离衰减半径 |
| `intensity` | `number` | `1.0` | 强度乘子 |
| `innerAngle` | `number` | `20°` | 内锥角；锥内全亮 |
| `outerAngle` | `number` | `35°` | 外锥角；超出全暗，与 innerAngle 之间 smoothstep 过渡 |

### 返回值

同 `AddPointLight`

### 示例

```lua
local id = Light.Lighting2D.AddSpotLight{
    x = 400, y = 300,
    dirX = 1, dirY = -0.3,         -- 朝右上方
    color = {r=1, g=1, b=1},
    range = 350, intensity = 1.8,
    innerAngle = 15, outerAngle = 35,
}
```

---

## `Light.Lighting2D.UpdateLight`

部分字段更新指定 slot

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `id` | `number` | `Add*` 返回的 id |
| `fields` | `table` | 任意子集字段（缺失字段保留原值） |

`fields` 接受 `AddPointLight` / `AddSpotLight` 全部字段，但 **不支持** 切换 `type`（Point↔Spot 需 `Remove` + `Add*`）。

### 返回值

| 场景 | 返回 |
|------|------|
| 成功 | `true` |
| id 越界 / slot 已 Remove | `false` |

### 示例

```lua
-- 只改 intensity, 其他保留
Light.Lighting2D.UpdateLight(id, {intensity = 0.5})

-- 改位置 + 颜色
Light.Lighting2D.UpdateLight(id, {x=300, y=400, color={r=0,g=1,b=0}})
```

---

## `Light.Lighting2D.RemoveLight`

移除指定 slot（幂等）

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `id` | `number` | slot id |

对已移除 / 越界 id 是 no-op，不报错。

### 返回值

`void`

---

## `Light.Lighting2D.ClearLights`

清空所有 light（`active_count` 归 0；`ambient` 与 `enabled` 保留）

### 返回值

`void`

---

## `Light.Lighting2D.GetLightCount`

查询当前 active light 数

### 返回值

`number` ∈ [0, 16]

---

## `Light.Lighting2D.GetMaxLights`

返回硬上限常量（= 16）

### 返回值

`number` — 总等于 `Light.Lighting2D.MAX_LIGHTS`

---

## ECS 集成（Phase E.1.6）

`Light.ECS` 自动注册两个内置 component。

### `Light2D` component

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `enabled` | `true` | `false` 时跳过上传 |
| `type` | `1` | `1`=Point / `2`=Spot |
| `color` | `{r=1, g=1, b=1}` | RGB |
| `range` | `200` | 距离衰减半径 |
| `intensity` | `1` | 强度乘子 |
| `dirX, dirY` | `1, 0` | spot 方向（仅 type=2 用） |
| `innerAngle` | `20` | spot 内锥角（度） |
| `outerAngle` | `35` | spot 外锥角（度） |

需要配合 `Transform2D` component 提供 `x, y` 世界坐标。

**坐标转换**：`world:Render()` 内部调 `_UploadLights2D(cam2d)`，自动 `view_x = (world_x - cam_x) * zoom` 转 view space + range 同步乘以 zoom。

### `LitSprite` component

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `image` | `nil` | baseColor 纹理（与 Sprite 一致） |
| `normalMap` | `nil` | 法线贴图（nil → 平面光照） |
| `color` | `{r=1, g=1, b=1, a=1}` | 顶点色 tint |
| `visible` | `true` | `false` 时不画 |
| `anchor` | `{ax=0, ay=0}` | 锚点偏移 |
| `flipX, flipY` | `false` | 翻转 |
| `quad` | `nil` | `{qx, qy, qw, qh}` sprite sheet 裁切（可选） |

**no_network**：`LitSprite` 含 `image` / `normalMap` userdata，被标 `_builtin_no_network`，不参与 NetworkSync。

### 渲染管线集成

`World:Render()` 2D 阶段顺序：

1. `_FindActiveCamera('Camera2D', 'Transform2D')` → 取相机
2. camera push (Translate + Scale)
3. **`_UploadLights2D(cam2d)`** ← 扫描所有 `Light2D` entity → world->view space → `Lighting2D.ClearLights` + `AddPointLight/AddSpotLight`
4. Sprite z-sort 循环 → `_DrawSprite`
5. **LitSprite z-sort 循环 → `_DrawLitSprite`**（走 `gfx.DrawLit / DrawLitQuad`）
6. SpriteBatch 循环
7. TextRenderer 循环
8. camera pop

### 示例

```lua
local ECS = require("Light.ECS")
local w = ECS.World.new()

-- 相机
w:CreateEntity():Add("Transform2D", {x=0, y=0})
               :Add("Camera2D",     {active=true, zoom=1, viewportW=800, viewportH=600})

-- 灯（world space）
w:CreateEntity():Add("Transform2D", {x=200, y=150})
               :Add("Light2D",      {type=1, color={r=1, g=0.6, b=0.3}, range=300})

-- 受光照 sprite
w:CreateEntity():Add("Transform2D", {x=400, y=300})
               :Add("LitSprite",    {image=heroImg, normalMap=heroNormal})

-- 每帧
function Game:Draw() w:Render() end
```

---

## 性能与限制

| 项 | 说明 |
|----|------|
| **硬上限 16 灯** | 与 shader uniform 数组 `[16]` 严格一致；第 17 个 `Add*` 返回 `nil + err` |
| **每 lit sprite 1 draw call** | `DrawLit*` 不入 BatchRenderer；批渲染优化留到 E.2 后 `LitBatchRenderer` |
| **`UploadLighting2D` 每 draw 上传** | 当前 shader uniform 每个 lit sprite 都重传；dirty bit 优化留到 E.2.x |
| **ECS `_UploadLights2D` 每帧 ClearLights + Add** | 16 灯 × 6 行 Lua < 100 Lua call / 帧，性能可忽略 |
| **后端兼容** | GL33 Core 支持；Legacy GL 路径 `SupportsLit2D()` 返回 `false`，`DrawLit*` 静默 fallback |

---

## 实现 / 验收文档

| 文档 | 内容 |
|------|------|
| `docs/Phase E 渲染管线升级/DESIGN_PhaseE_1.md` | 整体设计 |
| `docs/Phase E 渲染管线升级/TASK_PhaseE.md` § E.1 | 8 个原子任务定义 |
| `docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_1_3.md` | C++ 状态机 |
| `docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_1_4.md` | Lua binding + smoke |
| `docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_1_5.md` | DrawLit2DQuad + GL33 后端 |
| `docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_1_6.md` | ECS 集成 |
| `docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_1_7.md` | demo + smoke 完整覆盖 |
| `samples/demo_2d_lighting/main.lua` | 200 行端到端 demo |
| `scripts/smoke/lighting2d.lua` | 28 段断言 smoke |
