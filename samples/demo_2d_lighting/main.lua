-- ChocoLight Sample: demo_2d_lighting (Phase E.1.7)
--
-- 演示 2D forward 多光照系统:
--   - 1 ambient + 2 point lights + 1 spot light
--   - 8x6 sprite 网格 (用 ECS LitSprite component, 自动跟随 _UploadLights2D)
--   - 键盘 1/2/3 切换 light enabled, 鼠标 X 控制 spot 方向
--
-- 用法:
--   light.exe samples/demo_2d_lighting/main.lua                 # 800x600, ESC 退出
--   light.exe samples/demo_2d_lighting/main.lua [duration_sec]  # 自动退出
--
-- 主要演示项 (E.1.7 验收):
--   E.1  ambient 设置可见 (无光时 sprite 显示为环境光颜色)
--   E.2  point light 加 ambient 后视觉合理 (近灯亮, 远灯暗)
--   E.3  spot light 锥形衰减 (smoothstep 内外角度)
--   E.4  键盘交互切换 light enabled (实时反馈)
--   E.5  Light2D ECS entity 移动时 view-space 上传跟随
--
-- ASCII-only (CI / 跨平台兼容)

print("==== Light.Lighting2D Demo (Phase E.1.7) ====")

-- ==================== 1. Module loading ====================

local ECS = require("Light.ECS")
local L2D = require("Light.Lighting2D")

local function tryRequire(name)
    local ok, m = pcall(require, name)
    if ok then return m end
    return nil
end
local UI   = tryRequire("Light.UI")
local Time = tryRequire("Light.Time")

print(string.format("[1] modules: ECS=%s L2D=%s UI=%s  MaxLights=%d",
    type(ECS), type(L2D), type(UI), L2D.GetMaxLights()))

local duration = tonumber(arg and arg[1]) or 0

-- ==================== 2. ECS World setup ====================

local screenW, screenH = 800, 600
local world = ECS.World.new()

-- Camera2D (居中, zoom=1)
local cam = world:CreateEntity()
cam:Add("Transform2D", {x = 0, y = 0})
cam:Add("Camera2D",    {active = true, zoom = 1.0, viewportW = screenW, viewportH = screenH})

-- 8x6 sprite 网格 (用 LitSprite, 每个 sprite 走 Lit 路径)
-- 没有真实 image 资源时, gfx.DrawLit 也能用 (texture 0 + 顶点色)
local function makeMockImage(w, h)
    return {
        GetWidth  = function(self) return w end,
        GetHeight = function(self) return h end,
    }
end

local spriteW, spriteH = 64, 48
local gridCols, gridRows = 8, 6
local marginX = (screenW - gridCols * spriteW) / (gridCols + 1)
local marginY = (screenH - gridRows * spriteH) / (gridRows + 1)
for row = 0, gridRows - 1 do
    for col = 0, gridCols - 1 do
        local px = marginX + col * (spriteW + marginX) + spriteW * 0.5
        local py = marginY + row * (spriteH + marginY) + spriteH * 0.5
        local e = world:CreateEntity()
        e:Add("Transform2D", {x = px, y = py, z = row * gridCols + col})
        e:Add("LitSprite", {
            image  = makeMockImage(spriteW, spriteH),
            color  = {r = 1, g = 1, b = 1, a = 1},
            anchor = {ax = 0.5, ay = 0.5},
        })
    end
end
print(string.format("[2] world: %d entities (1 cam + %dx%d lit sprite grid)",
    #world._entities, gridCols, gridRows))

-- ==================== 3. Lights ====================

-- Light2D entity helper: type 1=Point, 2=Spot
local function makeLight(opts)
    local e = world:CreateEntity()
    e:Add("Transform2D", {x = opts.x, y = opts.y})
    e:Add("Light2D",     opts.light)
    return e
end

-- Point #1: 暖橘色, 屏幕左上
local light1 = makeLight{
    x = 200, y = 150,
    light = {type = 1, color = {r = 1.0, g = 0.6, b = 0.3}, range = 250, intensity = 1.5},
}
-- Point #2: 冷蓝色, 屏幕右下
local light2 = makeLight{
    x = 600, y = 450,
    light = {type = 1, color = {r = 0.3, g = 0.6, b = 1.0}, range = 250, intensity = 1.2},
}
-- Spot: 屏幕中央, 朝右上, 白色
local spot = makeLight{
    x = 400, y = 300,
    light = {type = 2, color = {r = 1.0, g = 1.0, b = 1.0},
              range = 350, intensity = 1.8,
              dirX = 1, dirY = -0.3,
              innerAngle = 15, outerAngle = 35},
}

-- Ambient
L2D.SetAmbient(0.15, 0.15, 0.20)
print(string.format("[3] lights: point1=%s point2=%s spot=%s  ambient=(0.15, 0.15, 0.20)",
    tostring(light1._id), tostring(light2._id), tostring(spot._id)))

-- ==================== 4. Headless fallback ====================

if not UI or not UI.Window then
    print("[4] UI.Window unavailable, headless mode (verify _UploadLights2D state)")
    -- 模拟 5 帧: 调 _UploadLights2D 看 Lighting2D state
    for frame = 1, 5 do
        world:_UploadLights2D(cam)
        if frame == 1 then
            print(string.format("[4] after _UploadLights2D: count=%d (expect 3)",
                  L2D.GetLightCount()))
        end
    end
    print("demo_2d_lighting headless ok")
    return
end

-- ==================== 5. Interactive window ====================

local Game = Light(Light.UI.Window):New()
local elapsed = 0
local frames  = 0
local mouseX, mouseY = screenW * 0.5, screenH * 0.5
local light1On, light2On, spotOn = true, true, true

function Game:OnOpen()
    print("[5] window opened, demo running")
    if Light.Graphics and Light.Graphics.GetBackendName then
        print("[5] backend: " .. Light.Graphics.GetBackendName())
    end
    if Light.Graphics and Light.Graphics.SupportsLit2D then
        print("[5] SupportsLit2D: " .. tostring(Light.Graphics.SupportsLit2D()))
    end
end

function Game:Update(dt)
    elapsed = elapsed + dt
    frames  = frames + 1

    -- Spot 跟随鼠标: 从 spot 位置指向鼠标
    local spotTF = spot._comps.Transform2D
    local spotLT = spot._comps.Light2D
    local dx = mouseX - spotTF.x
    local dy = mouseY - spotTF.y
    local len = math.sqrt(dx * dx + dy * dy)
    if len > 1 then
        spotLT.dirX = dx / len
        spotLT.dirY = dy / len
    end

    -- Enabled 切换由 key callback 处理
    light1._comps.Light2D.enabled = light1On
    light2._comps.Light2D.enabled = light2On
    spot._comps.Light2D.enabled   = spotOn

    if duration > 0 and elapsed >= duration then self:Close() end
end

function Game:Draw()
    -- 先清屏 (ambient 体现)
    Light.Graphics.SetColor(0.05, 0.05, 0.08, 1)
    Light.Graphics.Rectangle(1, 0, 0, 0, screenW, screenH, 0)

    -- ECS 渲染 (内部自动调 _UploadLights2D + 画所有 LitSprite)
    world:Render()

    -- 状态 HUD
    Light.Graphics.SetColor(1, 1, 0.6, 1)
    Light.Graphics.Print(string.format(
        "Phase E.1.7 Lit2D Demo  fps=%.0f  lights=%d  drawn=%d",
        frames > 0 and (frames / math.max(elapsed, 0.001)) or 0,
        L2D.GetLightCount(),
        (world._cull_stats_lit2d and world._cull_stats_lit2d.drawn) or 0
    ), nil, 10, 10, 0)
    Light.Graphics.SetColor(0.8, 0.8, 0.8, 1)
    Light.Graphics.Print(string.format(
        "[1] point#1 %s    [2] point#2 %s    [3] spot %s    mouse=(%.0f, %.0f)",
        light1On and "ON" or "OFF",
        light2On and "ON" or "OFF",
        spotOn  and "ON" or "OFF",
        mouseX, mouseY
    ), nil, 10, 30, 0)
    Light.Graphics.Print("ESC=quit  1/2/3=toggle lights  mouse=spot direction",
                          nil, 10, 50, 0)
end

function Game:OnKey(key, sc, action)
    if action ~= 1 then return end
    if key == 256 then          -- ESC
        self:Close()
    elseif key == 49 then       -- '1'
        light1On = not light1On
    elseif key == 50 then       -- '2'
        light2On = not light2On
    elseif key == 51 then       -- '3'
        spotOn  = not spotOn
    end
end

function Game:OnMouseMove(x, y)
    mouseX = x
    mouseY = y
end

Game:Open(screenW, screenH, "ChocoLight Phase E.1.7 - 2D Lighting Demo")
while Light.UI.Loop() do Light.UI.Resume() end
print(string.format("demo_2d_lighting solo ok (ran %d frames in %.2fs)", frames, elapsed))
