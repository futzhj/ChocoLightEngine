# demo_ecs_render

Phase D 示例:演示 **ECS × 渲染** 集成。

## 功能

- 用 `Light.ECS` 创建 N 个移动 entity (默认 50,可命令行调整)
- 每个 entity 携带内置 `Transform2D` + `Sprite` + 用户 `Velocity` component
- 用户 `Move` system 每帧更新 `Transform2D` (含边界反弹)
- `world:Render()` 触发内置渲染管线 (含 `Camera2D` 视图变换 + Sprite z-sort)
- demo 自带 fallback: 没有 `Sprite.image` 资源时,用 `Light.Graphics.Rectangle` 画占位矩形,让用户无外部素材也能看到 ECS 数据流可视化效果
- HUD 显示 entity 数量、帧数、运行时间

## 用法

一份 `main.lua` 三种模式 (通过 `arg[1]` 切换):

### Solo (单机, 默认)

```bash
light.exe samples/demo_ecs_render/main.lua                       # 50 entity, 无限运行
light.exe samples/demo_ecs_render/main.lua solo                  # 同上
light.exe samples/demo_ecs_render/main.lua solo 200 5            # 200 entity, 5 秒后自动退出
```

参数 (solo):

| 位置 | 含义 | 默认 |
|------|------|------|
| `arg[2]` | entity 数量 | `50` |
| `arg[3]` | 自动退出秒数 (`0` = 无限) | `0` |

### Server / Client (网络化, 演示 Phase D × Phase C.x.1 整合)

```bash
# 终端 A — 启动 server (无窗口, 跑模拟 + 广播 ecs_delta)
light.exe samples/demo_ecs_render/main.lua server [port=9111]

# 终端 B — 启动 client (开窗显示 mirror world)
light.exe samples/demo_ecs_render/main.lua client [host=127.0.0.1] [port=9111]
```

**网络化关键点** (server 端):

```lua
world:MarkRenderNetworked()  -- 把内置 Transform2D/Sprite/etc 标 networked
world:NetworkSync(room)       -- 之后 world:Update 自动 Broadcast('ecs_delta', ...)
```

**网络化关键点** (client 端):

```lua
local mirror = ECS.MirrorFromRoom(room)  -- 自动 OnEvent('ecs_delta') hook
mirror:Render()                          -- 内置渲染直接用收到的 entity 状态
```

### Headless 模式

无 `Light.UI` 时自动跳过窗口, 跑 5 帧 ECS 验证后退出 — CI runtime smoke 友好.

按键:

| 键 | 动作 |
|----|------|
| ESC | 退出 demo |

## 预期输出 (摘要)

```
==== Light.ECS Render Demo (Phase D) ====
[1] modules: ECS=table UI=table Time=table
[2] world created, builtin components: Transform2D, Sprite, Camera2D, Transform3D, MeshRenderer, Camera3D
[3] created 50 entities with Transform2D+Velocity+Sprite
[3] added Camera2D entity at (0,0) zoom=1
[5] window opened, running ECS render demo
[5] backend: GL33Core
[I] Window opened: 800x600 'ChocoLight Phase D — ECS Render Demo'
demo_ecs_render ok (ran 475 frames in 2.00s)
```

(运行 2 秒、~237 fps、Exit=0)

## 关键代码片段

### ECS 设置

```lua
local world = ECS.World.new()
-- 内置 6 个渲染 component 已自动注册 (Transform2D/Sprite/Camera2D/Transform3D/MeshRenderer/Camera3D)
world:RegisterComponent('Velocity', {vx=0, vy=0})  -- 用户自定义 component

world:AddSystem('Move', {'Transform2D', 'Velocity'}, function(ents, dt)
    for _, e in ipairs(ents) do
        local tf, v = e._comps.Transform2D, e._comps.Velocity
        e:Set('Transform2D', {x = tf.x + v.vx*dt, y = tf.y + v.vy*dt})
    end
end)
```

### Game loop 接入

```lua
function Game:Update(dt) world:Update(dt) end  -- 跑用户 system
function Game:Draw()     world:Render()    end  -- 跑内置渲染 (2D camera → sprite)
```

### Camera entity

```lua
local cam = world:CreateEntity()
cam:Add('Transform2D', {x=0, y=0})
cam:Add('Camera2D',    {active=true, zoom=1.0})
-- _Render_Camera2D_Setup 会自动找到 active=true 的相机, 应用 Push/Scale/Translate
```

## 升级到真实 Sprite 渲染

把任意 PNG (推荐 64x64) 命名为 `hero.png` 放到 sample 目录,然后修改 `main.lua`:

```lua
-- entity 创建处
local heroImg = Light(Light.Graphics.Image):New('samples/demo_ecs_render/hero.png')
e:Add('Sprite', {
    image = heroImg,  -- ← 关键: 设置 image 后内置 _DrawSprite 会自动调 Light.Graphics.Draw
    color = {...},
    anchor = {ax=0.5, ay=0.5},
})
```

之后 demo 中"fallback Rectangle 绘制"段会自动跳过 (因为 `sp.image ~= nil`),改用真实 Light.Graphics 渲染。

## 已知限制

- demo 默认无 image 资源, 用 fallback Rectangle 画矩形. 用户可提供 hero.png 体验完整 Sprite 渲染
- 没演示 3D (Transform3D + MeshRenderer + Camera3D) — 留 Phase D 后续 demo 补充
- 没演示网络同步 — 见 `samples/demo_ecs_network` (Phase C/C.x.1)
- HUD 用 `Light.Graphics.Print` 默认字体, 部分平台/字体缺失可能不显示文字
