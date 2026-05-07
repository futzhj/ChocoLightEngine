-- ============================================================================
-- ChocoLight Phase A 性能压力测试场景
-- ============================================================================
-- 用途: 验证 Sprite Batcher (Phase A4-A7) 真实场景下的 draw call 收益
--
-- 测试场景 (按 1/2/3/4 切换):
--   1. 1024 粒子 (BatchRenderer 应将 draw call 从 1024 降至 1)
--   2. 大量精灵 sprites (1000 同纹理,期望 1 draw call)
--   3. 文本压力 (1000 字符同字体,期望 1 draw call)
--   4. Tilemap 一屏 (20x15 tile 同 tileset,期望 1 draw call)
--
-- 切换/控制:
--   - 数字键 1/2/3/4: 切换场景
--   - ESC: 退出
--   - F1: 打印诊断信息(Backend Name + 当前帧 draw call,需 GetBackendName 实现)
--
-- 验证手段(本地/真机):
--   - 用 RenderDoc 抓帧观察 draw call 数
--   - 或用 GPU Profiler (Xcode/RenderDoc/Adreno Profiler) 观测帧时间
-- ============================================================================

local Game = Light(Light.UI.Window):New()

local SCREEN_W, SCREEN_H = 1024, 768
local currentScene = 1  -- 1..4
local frameCount = 0
local startTime = 0

-- ==================== 场景 1: 1024 粒子 ====================

local emitter

local function setupParticles()
  emitter = Light(Light.Graphics.Particles):New(1024)
  emitter:SetPosition(SCREEN_W * 0.5, SCREEN_H * 0.5)
  emitter:SetEmitRate(800)
  emitter:SetLifeRange(1.0, 2.0)
  emitter:SetSpeedRange(60, 200)
  emitter:SetAngleRange(0, math.pi * 2)
  emitter:SetSizeRange(3, 8)
  emitter:SetColorRange({1, 0.6, 0.1, 1}, {1, 0.1, 0.1, 0})
  emitter:SetGravity(0, 80)
  emitter:Start()
end

-- ==================== 场景 4: Tilemap 一屏 ====================

local tilemap

local function setupTilemap()
  -- 简化数据 (无外部 tileset; 仅模块构造,实际 tile 渲染需要图像)
  -- 此处仅展示 API,真实压力需配合 tileset image
  -- (略 — 真实 demo 需用户提供 tileset.png + 数据)
end

-- ==================== Window 回调 ====================

function Game:OnOpen()
  startTime = os.clock()
  print("========================================")
  print("ChocoLight Phase A Performance Benchmark")
  print("========================================")
  if Light.Graphics.GetBackendName then
    print("Backend: " .. Light.Graphics.GetBackendName())
  end
  print("Scenes: 1/2/3/4 (digit keys) | ESC = quit")
  print("Current scene:", currentScene)
  setupParticles()
  setupTilemap()
end

function Game:OnKey(key, scancode, action, mods)
  if action ~= 1 then return end  -- 仅 press
  if key == 256 then  -- ESC
    self:Close()
  elseif key >= 48 and key <= 57 then  -- 0-9
    local n = key - 48
    if n >= 1 and n <= 4 then
      currentScene = n
      print("Scene -> " .. currentScene)
    end
  elseif key == 290 then  -- F1
    local elapsed = os.clock() - startTime
    local fps = frameCount / math.max(elapsed, 0.001)
    print(string.format("Frames=%d, Elapsed=%.2fs, FPS~=%.1f", frameCount, elapsed, fps))
    if Light.Graphics.GetBackendName then
      print("Backend: " .. Light.Graphics.GetBackendName())
    end
  end
end

function Game:Update(dt)
  if currentScene == 1 and emitter then
    emitter:Update(dt)
  end
  frameCount = frameCount + 1
end

function Game:Draw()
  Light.Graphics.SetColor(1, 1, 1, 1)

  if currentScene == 1 and emitter then
    -- 1024 粒子: 这是 Phase A 的核心收益场景
    emitter:Draw()
    Light.Graphics.SetColor(1, 1, 0, 1)
    Light.Graphics.Print("Scene 1: 1024 particles", nil, 20, 20, 0)

  elseif currentScene == 2 then
    -- sprite 压力 (无纹理纯色 quad 模拟)
    for y = 0, 24 do
      for x = 0, 39 do
        local r = (x % 2 == 0) and 1.0 or 0.6
        local g = (y % 2 == 0) and 0.6 or 1.0
        Light.Graphics.SetColor(r, g, 0.4, 1)
        Light.Graphics.Rectangle(2, x * 25, y * 30, 0, 24, 28, 0)  -- 1000 quad
      end
    end
    Light.Graphics.SetColor(1, 1, 0, 1)
    Light.Graphics.Print("Scene 2: 1000 colored quads", nil, 20, 20, 0)

  elseif currentScene == 3 then
    -- 文本压力 (默认字体, 多行)
    Light.Graphics.SetColor(1, 1, 1, 1)
    for i = 0, 24 do
      Light.Graphics.Print("BatchRenderer reduces draw calls from N to ~1 for same texture", nil, 20, i * 28 + 50, 0)
    end
    Light.Graphics.SetColor(1, 1, 0, 1)
    Light.Graphics.Print("Scene 3: ~1500 chars same font", nil, 20, 20, 0)

  else
    -- Scene 4: tilemap 占位
    Light.Graphics.SetColor(0.2, 0.6, 1.0, 1)
    Light.Graphics.Rectangle(2, 100, 100, 0, 800, 500, 0)
    Light.Graphics.SetColor(1, 1, 0, 1)
    Light.Graphics.Print("Scene 4: tilemap (provide tileset.png)", nil, 20, 20, 0)
  end

  -- 通用提示
  Light.Graphics.SetColor(0.7, 0.7, 0.7, 1)
  Light.Graphics.Print("Keys: 1/2/3/4 switch | F1 stats | ESC quit", nil, 20, SCREEN_H - 30, 0)
end

-- ==================== 启动 ====================

Game:Open(SCREEN_W, SCREEN_H, "ChocoLight Phase A Perf Benchmark")
while Light.UI.Loop() do
  Light.UI.Resume()
end
