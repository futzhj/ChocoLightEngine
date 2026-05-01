/**
 * @file light_scene.cpp
 * @brief Light.Scene 模块 — 场景栈管理器 (纯 Lua 嵌入)
 *
 * 设计理念:
 *   - 场景栈 (Scene Stack): 后进先出, 栈顶场景接收所有事件/更新/绘制
 *   - 生命周期: OnEnter / OnExit / OnPause / OnResume
 *   - 与 Window 解耦: Window 仅需调用 Scene.Update/Draw/Dispatch
 *
 * Lua API:
 *   -- 定义场景 (继承 Light.Scene)
 *   local Menu = Light(Light.Scene):New()
 *   function Menu:OnEnter() end
 *   function Menu:OnExit() end
 *   function Menu:OnPause() end
 *   function Menu:OnResume() end
 *   function Menu:Update(dt) end
 *   function Menu:Draw() end
 *   function Menu:OnKey(key, action) end
 *
 *   -- 栈操作
 *   Light.Scene.Push(Menu)        -- 旧栈顶 OnPause, 新场景 OnEnter
 *   Light.Scene.Pop()             -- 栈顶 OnExit, 新栈顶 OnResume
 *   Light.Scene.Replace(NewScene) -- 旧栈顶 OnExit, 新场景 OnEnter
 *   Light.Scene.Clear()           -- 清空所有场景 (倒序 OnExit)
 *   Light.Scene.Top()             -- 获取栈顶场景 (无则 nil)
 *   Light.Scene.Count()           -- 栈中场景数量
 *
 *   -- 在 Window:Update/Draw 中转发
 *   Light.Scene.Update(dt)
 *   Light.Scene.Draw()
 *   Light.Scene.Dispatch("OnKey", key, action) -- 分发事件到栈顶
 */

#include "light.h"
#include <cstring>

static const char s_SceneLuaScript[] = R"LUA(
-- ==================== Scene 基类 ====================
local Scene = {}

-- 默认生命周期 (子类可覆盖)
function Scene:OnEnter()  end  -- 入栈时调用 (一次)
function Scene:OnExit()   end  -- 出栈时调用 (一次)
function Scene:OnPause()  end  -- 被新场景覆盖时调用
function Scene:OnResume() end  -- 上层场景出栈, 重新成为栈顶时调用
function Scene:Update(dt) end
function Scene:Draw()     end

-- ==================== 场景栈 (内部状态) ====================
-- 使用闭包 upvalue 隔离状态, 避免全局污染
local stack = {}

local function safeCall(scene, method, ...)
  if scene and type(scene[method]) == 'function' then
    local ok, err = pcall(scene[method], scene, ...)
    if not ok then
      print("[Scene] " .. method .. " error: " .. tostring(err))
    end
  end
end

-- ==================== 栈操作 ====================
function Scene.Push(scene)
  if not scene then return end
  -- 旧栈顶暂停
  local top = stack[#stack]
  if top then safeCall(top, 'OnPause') end
  -- 新场景入栈并进入
  stack[#stack + 1] = scene
  safeCall(scene, 'OnEnter')
end

function Scene.Pop()
  local n = #stack
  if n == 0 then return nil end
  local top = stack[n]
  safeCall(top, 'OnExit')
  stack[n] = nil
  -- 新栈顶恢复
  local newTop = stack[n - 1]
  if newTop then safeCall(newTop, 'OnResume') end
  return top
end

function Scene.Replace(scene)
  if not scene then return end
  -- 旧栈顶退出 (无 OnPause/OnResume, 因为是替换)
  local n = #stack
  if n > 0 then
    safeCall(stack[n], 'OnExit')
    stack[n] = nil
  end
  -- 新场景入栈
  stack[#stack + 1] = scene
  safeCall(scene, 'OnEnter')
end

function Scene.Clear()
  -- 倒序退出所有场景
  for i = #stack, 1, -1 do
    safeCall(stack[i], 'OnExit')
    stack[i] = nil
  end
end

function Scene.Top()
  return stack[#stack]
end

function Scene.Count()
  return #stack
end

-- ==================== 帧循环转发 ====================
function Scene.Update(dt)
  local top = stack[#stack]
  if top then safeCall(top, 'Update', dt) end
end

function Scene.Draw()
  local top = stack[#stack]
  if top then safeCall(top, 'Draw') end
end

-- 事件分发 (动态方法名)
function Scene.Dispatch(method, ...)
  local top = stack[#stack]
  if top then safeCall(top, method, ...) end
end

-- 全栈遍历版本 (Update 和 Draw 全部场景, 用于半透明叠加)
function Scene.UpdateAll(dt)
  for i = 1, #stack do safeCall(stack[i], 'Update', dt) end
end

function Scene.DrawAll()
  for i = 1, #stack do safeCall(stack[i], 'Draw') end
end

return Scene
)LUA";

// ==================== luaopen_Light_Scene ====================

int luaopen_Light_Scene(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Scene");
    lua_rawget(L, -2);

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Scene");

        if (!luaL_loadstring(L, s_SceneLuaScript) && !lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_rawset(L, -3);
            lua_pushstring(L, "Scene");
            lua_rawget(L, -2);
        } else {
            lua_error(L);
        }
    }

    lua_remove(L, -2);
    return 1;
}
