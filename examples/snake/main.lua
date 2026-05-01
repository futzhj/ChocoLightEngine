---@diagnostic disable: undefined-global
--[[
  贪吃蛇 Demo —— 综合展示 ChocoLight Phase 3 新模块
  
  涉及模块:
    Light.Scene             — 场景栈 (Menu / Play / GameOver)
    Light.UI.Widget         — Button / Label 控件
    Light.Input             — 键盘输入 + 动作映射
    Light.HotReload         — main.lua 自动重载
    Light.Graphics          — 矩形/文本绘制

  操作:
    方向键 / WASD — 移动
    R             — 重新开始
    ESC           — 返回菜单
]]

-- 加载所需模块
require("Light.UI.Window")
require("Light.Graphics")
require("Light.Scene")
require("Light.UI.Widget")
require("Light.Input")
require("Light.HotReload")

local Scene  = Light.Scene
local Widget = Light.UI.Widget
local Input  = Light.Input
local Hot    = Light.HotReload

-- ============================================================
-- 全局配置
-- ============================================================
local CFG = {
  W = 800,         -- 窗口宽
  H = 600,         -- 窗口高
  CELL = 24,       -- 格子像素
  COLS = 25,       -- 列数 (CELL * COLS = 600)
  ROWS = 20,       -- 行数 (CELL * ROWS = 480)
  STEP = 0.12,     -- 蛇步进秒数 (越小越快)
}

-- ============================================================
-- 场景 1: 主菜单
-- ============================================================
local MenuScene = {}
MenuScene.__index = MenuScene

function MenuScene.New()
  local self = setmetatable({}, MenuScene)
  self.root = Widget.Container.New(0, 0, CFG.W, CFG.H)
  
  -- 标题标签
  local title = Widget.Label.New(CFG.W / 2 - 80, 100, "SNAKE GAME",
    { color = {0.4, 1.0, 0.4, 1.0} })
  self.root:AddChild(title)
  
  local hint = Widget.Label.New(CFG.W / 2 - 100, 140, "ChocoLight Phase 3 Demo",
    { color = {0.7, 0.7, 0.7, 1.0} })
  self.root:AddChild(hint)
  
  -- 开始按钮
  local btnStart = Widget.Button.New(CFG.W / 2 - 75, 250, 150, 50, "START",
    { OnClick = function()
        Scene.Replace(PlayScene.New())
      end })
  self.root:AddChild(btnStart)
  
  -- 退出按钮
  local btnQuit = Widget.Button.New(CFG.W / 2 - 75, 320, 150, 50, "QUIT",
    { OnClick = function()
        os.exit(0)
      end })
  self.root:AddChild(btnQuit)
  
  -- 操作提示
  local tipY = 450
  local tips = {
    "Controls:",
    "  Arrow keys / WASD - Move",
    "  R - Restart",
    "  ESC - Back to menu",
  }
  for i, line in ipairs(tips) do
    local lbl = Widget.Label.New(CFG.W / 2 - 100, tipY + (i - 1) * 22, line,
      { color = {0.6, 0.6, 0.6, 1.0} })
    self.root:AddChild(lbl)
  end
  
  return self
end

function MenuScene:OnEnter()
  print("[Menu] entered")
end

function MenuScene:Update(dt)
  self.root:Update(dt)
end

function MenuScene:Draw()
  self.root:Draw()
end

function MenuScene:OnMouseMove(x, y)  self.root:Dispatch("OnMouseMove", x, y) end
function MenuScene:OnMouseDown(x, y, btn) self.root:Dispatch("OnMouseDown", x, y, btn) end
function MenuScene:OnMouseUp(x, y, btn)   self.root:Dispatch("OnMouseUp", x, y, btn) end

-- ============================================================
-- 场景 2: 游戏中
-- ============================================================
PlayScene = {}
PlayScene.__index = PlayScene

function PlayScene.New()
  local self = setmetatable({}, PlayScene)
  self:Reset()
  return self
end

function PlayScene:Reset()
  -- 蛇身: 数组, 每个元素 {x, y} (格子坐标)
  self.snake = {
    {x = 12, y = 10},
    {x = 11, y = 10},
    {x = 10, y = 10},
  }
  self.dir     = {x = 1, y = 0}   -- 初始向右
  self.nextDir = {x = 1, y = 0}
  self.food    = self:RandomFood()
  self.timer   = 0
  self.score   = 0
  self.dead    = false
end

function PlayScene:RandomFood()
  while true do
    local fx = math.random(0, CFG.COLS - 1)
    local fy = math.random(0, CFG.ROWS - 1)
    -- 不能与蛇身重叠
    local ok = true
    for _, s in ipairs(self.snake) do
      if s.x == fx and s.y == fy then ok = false; break end
    end
    if ok then return {x = fx, y = fy} end
  end
end

function PlayScene:OnEnter()
  print("[Play] start")
end

function PlayScene:Update(dt)
  if self.dead then return end
  
  -- 读输入 (使用 Light.Input)
  if Input then
    if Input.IsKeyDown(265) or Input.IsKeyDown(87) then       -- Up / W
      if self.dir.y == 0 then self.nextDir = {x = 0, y = -1} end
    elseif Input.IsKeyDown(264) or Input.IsKeyDown(83) then   -- Down / S
      if self.dir.y == 0 then self.nextDir = {x = 0, y = 1} end
    elseif Input.IsKeyDown(263) or Input.IsKeyDown(65) then   -- Left / A
      if self.dir.x == 0 then self.nextDir = {x = -1, y = 0} end
    elseif Input.IsKeyDown(262) or Input.IsKeyDown(68) then   -- Right / D
      if self.dir.x == 0 then self.nextDir = {x = 1, y = 0} end
    end
  end
  
  self.timer = self.timer + dt
  if self.timer < CFG.STEP then return end
  self.timer = 0
  
  -- 应用方向变化
  self.dir = self.nextDir
  
  -- 计算新头位置
  local head = self.snake[1]
  local nh = {x = head.x + self.dir.x, y = head.y + self.dir.y}
  
  -- 撞墙
  if nh.x < 0 or nh.x >= CFG.COLS or nh.y < 0 or nh.y >= CFG.ROWS then
    self.dead = true
    Scene.Push(GameOverScene.New(self.score))
    return
  end
  
  -- 撞自己
  for _, s in ipairs(self.snake) do
    if s.x == nh.x and s.y == nh.y then
      self.dead = true
      Scene.Push(GameOverScene.New(self.score))
      return
    end
  end
  
  -- 入头
  table.insert(self.snake, 1, nh)
  
  -- 吃到食物?
  if nh.x == self.food.x and nh.y == self.food.y then
    self.score = self.score + 10
    self.food = self:RandomFood()
  else
    -- 否则去尾
    table.remove(self.snake)
  end
end

function PlayScene:Draw()
  -- 背景棋盘
  Light.Graphics.SetColor(0.05, 0.08, 0.05, 1)
  Light.Graphics.Rectangle(2, 0, 0, CFG.COLS * CFG.CELL, CFG.ROWS * CFG.CELL, 0)
  
  -- 网格线
  Light.Graphics.SetColor(0.1, 0.15, 0.1, 1)
  for i = 0, CFG.COLS do
    Light.Graphics.Line(i * CFG.CELL, 0, 0, i * CFG.CELL, CFG.ROWS * CFG.CELL, 0)
  end
  for j = 0, CFG.ROWS do
    Light.Graphics.Line(0, j * CFG.CELL, 0, CFG.COLS * CFG.CELL, j * CFG.CELL, 0)
  end
  
  -- 食物
  Light.Graphics.SetColor(1.0, 0.3, 0.3, 1)
  Light.Graphics.Rectangle(2,
    self.food.x * CFG.CELL + 2, self.food.y * CFG.CELL + 2,
    CFG.CELL - 4, CFG.CELL - 4, 0)
  
  -- 蛇身
  for i, s in ipairs(self.snake) do
    if i == 1 then
      Light.Graphics.SetColor(0.4, 1.0, 0.4, 1)  -- 头
    else
      local alpha = 1.0 - (i - 1) * 0.02
      if alpha < 0.4 then alpha = 0.4 end
      Light.Graphics.SetColor(0.2, 0.8, 0.3, alpha)
    end
    Light.Graphics.Rectangle(2,
      s.x * CFG.CELL + 1, s.y * CFG.CELL + 1,
      CFG.CELL - 2, CFG.CELL - 2, 0)
  end
  
  -- 侧边栏: 分数
  Light.Graphics.SetColor(1, 1, 1, 1)
  local sidebarX = CFG.COLS * CFG.CELL + 20
  Light.Graphics.Print("SCORE", nil, sidebarX, 30, 0)
  Light.Graphics.SetColor(0.4, 1.0, 0.4, 1)
  Light.Graphics.Print(tostring(self.score), nil, sidebarX, 60, 0)
  Light.Graphics.SetColor(0.7, 0.7, 0.7, 1)
  Light.Graphics.Print("Length: " .. #self.snake, nil, sidebarX, 100, 0)
  
  -- 操作提示
  Light.Graphics.SetColor(0.5, 0.5, 0.5, 1)
  local helpY = 200
  for i, line in ipairs({"WASD/Arrows: Move", "R: Restart", "ESC: Menu"}) do
    Light.Graphics.Print(line, nil, sidebarX, helpY + (i - 1) * 22, 0)
  end
end

function PlayScene:OnKey(key, action)
  if action ~= 1 then return end  -- 只响应按下
  if key == 256 then               -- ESC
    Scene.Replace(MenuScene.New())
  elseif key == 82 then            -- R
    self:Reset()
  end
end

-- ============================================================
-- 场景 3: 游戏结束 (覆盖在 PlayScene 上方, 半透明)
-- ============================================================
GameOverScene = {}
GameOverScene.__index = GameOverScene

function GameOverScene.New(score)
  local self = setmetatable({}, GameOverScene)
  self.score = score or 0
  
  self.root = Widget.Container.New(0, 0, CFG.W, CFG.H)
  
  -- 半透明遮罩面板
  local panel = Widget.Panel.New(CFG.W / 2 - 200, CFG.H / 2 - 150, 400, 300, {
    bgColor     = {0.0, 0.0, 0.0, 0.85},
    borderColor = {1.0, 0.3, 0.3, 1.0},
  })
  self.root:AddChild(panel)
  
  local title = Widget.Label.New(CFG.W / 2 - 70, CFG.H / 2 - 100, "GAME OVER",
    { color = {1.0, 0.3, 0.3, 1.0} })
  self.root:AddChild(title)
  
  local scoreLabel = Widget.Label.New(CFG.W / 2 - 60, CFG.H / 2 - 50,
    "Score: " .. self.score, { color = {1, 1, 1, 1} })
  self.root:AddChild(scoreLabel)
  
  -- 重玩按钮
  local btnRetry = Widget.Button.New(CFG.W / 2 - 150, CFG.H / 2 + 20, 130, 45, "RETRY",
    { OnClick = function()
        Scene.Pop()
        Scene.Replace(PlayScene.New())
      end })
  self.root:AddChild(btnRetry)
  
  -- 返回菜单按钮
  local btnMenu = Widget.Button.New(CFG.W / 2 + 20, CFG.H / 2 + 20, 130, 45, "MENU",
    { OnClick = function()
        Scene.Pop()
        Scene.Replace(MenuScene.New())
      end })
  self.root:AddChild(btnMenu)
  
  return self
end

function GameOverScene:Update(dt)
  self.root:Update(dt)
end

function GameOverScene:Draw()
  -- 先绘制底层 (PlayScene 通过 DrawAll 视角)
  -- 此处因为 Push 后栈顶才绘制, 需要 Window 调用 DrawAll 才会画到底层
  -- 简化: 画半透明黑色背景
  self.root:Draw()
end

function GameOverScene:OnMouseMove(x, y) self.root:Dispatch("OnMouseMove", x, y) end
function GameOverScene:OnMouseDown(x, y, btn) self.root:Dispatch("OnMouseDown", x, y, btn) end
function GameOverScene:OnMouseUp(x, y, btn)   self.root:Dispatch("OnMouseUp", x, y, btn) end

-- ============================================================
-- 主窗口
-- ============================================================
local Game = Light(Light.UI.Window):New()

function Game:OnOpen()
  print("[Game] window opened")
  -- 推入初始菜单场景
  Scene.Push(MenuScene.New())
  
  -- 注册热重载: main.lua 修改时自动重新加载
  if Hot then
    Hot.Watch("examples/snake/main.lua", function(path)
      print("[HotReload] reloading: " .. path)
      Scene.Clear()
      dofile(path)
    end)
    Hot.SetInterval(0.5)
  end
end

function Game:Update(dt)
  Scene.Update(dt)
  if Hot then Hot.Check(dt) end
end

function Game:Draw()
  Scene.Draw()
end

function Game:OnKey(key, scanCode, action, mods)
  Scene.Dispatch("OnKey", key, action)
end

function Game:OnMouseButton(x, y, button, action, mods)
  -- action 1 = down, 0 = up
  if action == 1 then
    Scene.Dispatch("OnMouseDown", x, y, button)
  else
    Scene.Dispatch("OnMouseUp", x, y, button)
  end
end

function Game:OnMousePosition(x, y)
  Scene.Dispatch("OnMouseMove", x, y)
end

-- ============================================================
-- 启动
-- ============================================================
math.randomseed(os.time and os.time() or 12345)
Game:Open(CFG.W, CFG.H, "ChocoLight Snake Demo")
Game:SetVSync(true)

while Light.UI.Loop() do
  Light.UI.Resume()
end
