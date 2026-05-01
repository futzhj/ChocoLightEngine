/**
 * @file light_graphics_spriteanimation.cpp
 * @brief Light.Graphics.SpriteAnimation 模块 — 帧动画播放器 (纯 Lua 嵌入)
 *
 * 设计:
 *   - 持有 Image 帧数组 + 帧间隔
 *   - 状态: playing / paused / stopped
 *   - 支持循环模式 / 单次播放
 *   - 帧事件回调: OnFrame(idx), OnLoop(), OnComplete()
 *
 * Lua API:
 *   local frames = { Light(Light.Graphics.Image):New("f1.png"), ... }
 *   local anim = Light.Graphics.SpriteAnimation.New(frames, 0.1, true)
 *
 *   anim:Play()
 *   anim:Pause()
 *   anim:Stop()                 -- 重置到第 0 帧
 *   anim:Resume()
 *   anim:SetFrame(idx)
 *   anim:SetLoop(bool)
 *   anim:SetSpeed(multiplier)   -- 1.0 = 正常, 2.0 = 两倍速
 *   anim:Update(dt)
 *   anim:Draw(x, y, ...)        -- 转发到 Light.Graphics.Draw(currentFrame, x, y, ...)
 *   anim:GetFrame()             -- 返回当前帧 Image
 *   anim:GetFrameIndex()        -- 返回当前帧索引 (1-based)
 *   anim:IsPlaying()
 *   anim:IsFinished()
 *
 *   -- 事件回调 (用户可覆盖)
 *   function anim:OnFrame(idx) end
 *   function anim:OnLoop() end
 *   function anim:OnComplete() end
 */

#include "light.h"
#include <cstring>

static const char s_SpriteAnimLuaScript[] = R"LUA(
local SpriteAnim = {}
SpriteAnim.__index = SpriteAnim

-- 默认事件回调 (用户可覆盖)
function SpriteAnim:OnFrame(idx) end
function SpriteAnim:OnLoop() end
function SpriteAnim:OnComplete() end

-- 创建实例
-- frames: Image 数组 (1-based)
-- frameTime: 每帧持续秒数 (默认 0.1)
-- loop: 是否循环 (默认 true)
function SpriteAnim.New(frames, frameTime, loop)
  assert(type(frames) == 'table' and #frames > 0, 'SpriteAnimation: frames must be non-empty table')
  local self = setmetatable({}, SpriteAnim)
  self.__frames    = frames
  self.__frameTime = frameTime or 0.1
  self.__loop      = (loop ~= false)  -- 默认 true
  self.__index_    = 1     -- 当前帧索引 (1-based)
  self.__elapsed   = 0     -- 累计时间
  self.__state     = 'stopped'  -- playing / paused / stopped
  self.__speed     = 1.0
  return self
end

-- 状态控制
function SpriteAnim:Play()
  self.__state = 'playing'
end

function SpriteAnim:Pause()
  if self.__state == 'playing' then
    self.__state = 'paused'
  end
end

function SpriteAnim:Resume()
  if self.__state == 'paused' then
    self.__state = 'playing'
  end
end

function SpriteAnim:Stop()
  self.__state   = 'stopped'
  self.__index_  = 1
  self.__elapsed = 0
end

-- 配置
function SpriteAnim:SetFrame(idx)
  local n = #self.__frames
  if n == 0 then return end
  -- 钳位到 [1, n]
  if idx < 1 then idx = 1 elseif idx > n then idx = n end
  self.__index_  = idx
  self.__elapsed = 0
  self:OnFrame(idx)
end

function SpriteAnim:SetLoop(loop)
  self.__loop = (loop and true or false)
end

function SpriteAnim:SetSpeed(speed)
  self.__speed = tonumber(speed) or 1.0
end

function SpriteAnim:SetFrameTime(t)
  self.__frameTime = tonumber(t) or 0.1
end

-- 查询
function SpriteAnim:GetFrame()
  return self.__frames[self.__index_]
end

function SpriteAnim:GetFrameIndex()
  return self.__index_
end

function SpriteAnim:GetFrameCount()
  return #self.__frames
end

function SpriteAnim:IsPlaying()
  return self.__state == 'playing'
end

function SpriteAnim:IsFinished()
  -- 非循环模式 + 已播完最后一帧
  return (not self.__loop) and self.__index_ >= #self.__frames and self.__elapsed >= self.__frameTime
end

-- 帧推进
function SpriteAnim:Update(dt)
  if self.__state ~= 'playing' then return end
  if self.__frameTime <= 0 then return end

  self.__elapsed = self.__elapsed + dt * self.__speed
  -- 多帧推进 (大 dt 跳跃)
  while self.__elapsed >= self.__frameTime do
    self.__elapsed = self.__elapsed - self.__frameTime
    local n = #self.__frames
    local nextIdx = self.__index_ + 1
    if nextIdx > n then
      if self.__loop then
        nextIdx = 1
        self:OnLoop()
      else
        -- 停在最后一帧, 触发 OnComplete
        self.__index_ = n
        self.__state  = 'stopped'
        self.__elapsed = 0
        self:OnComplete()
        return
      end
    end
    self.__index_ = nextIdx
    self:OnFrame(nextIdx)
  end
end

-- 绘制 (转发到 Light.Graphics.Draw)
function SpriteAnim:Draw(...)
  local frame = self.__frames[self.__index_]
  if frame and Light and Light.Graphics and Light.Graphics.Draw then
    Light.Graphics.Draw(frame, ...)
  end
end

return SpriteAnim
)LUA";

// ==================== luaopen_Light_Graphics_SpriteAnimation ====================
// 注意: 需要先确保 Light.Graphics 表存在

static void EnsureGraphicsTable(lua_State* L) {
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Graphics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Graphics");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);  // 移除 Light 表, 仅留 Graphics
}

int luaopen_Light_Graphics_SpriteAnimation(lua_State* L) {
    EnsureGraphicsTable(L);

    lua_pushstring(L, "SpriteAnimation");
    lua_rawget(L, -2);

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "SpriteAnimation");

        if (!luaL_loadstring(L, s_SpriteAnimLuaScript) && !lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_rawset(L, -3);
            lua_pushstring(L, "SpriteAnimation");
            lua_rawget(L, -2);
        } else {
            lua_error(L);
        }
    }

    lua_remove(L, -2);
    return 1;
}
