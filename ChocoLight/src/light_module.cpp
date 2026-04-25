/**
 * @file light_module.cpp
 * @brief Light 核心模块入口 — luaopen_Light 和模块注册工具
 * @note 精确还原自 Light.dll IDA 反编译 sub_1800A53F0 / sub_1800A52F0
 *
 * 核心数据:
 *   unk_1802CA230 — 7276 字节嵌入 Lua OOP 框架 (明文)
 *   aLocalDemowindo — 2359 字节 Demo 窗口脚本
 *
 * OOP 框架特性:
 *   - 原型链继承 (prototype chain via getmetatable)
 *   - 运算符重载 (__add/__sub/__mul/__div/__concat)
 *   - New/Extends/Is/Mixin/Clone/Cast/Send/RespondTo
 *   - SetWeak 引用模式 / SetMethodCache 缓存策略
 *   - Description 自省调试
 */

#include "light.h"

// ==================== 嵌入 Lua OOP 框架 (unk_1802CA230) ====================
// 从 IDA 逐字节提取的完整 7276 字节脚本
// 这是 Light 引擎的核心 — 所有模块的 New/Extends/Is 都依赖此框架

static const char g_lightInitScript[] = R"LUA(
local __cache = false
local DefaultObjectName = 'table'
local STRING = 'string'
local TABLE = 'table'
local FUNCTION = 'function'
local INDEX, NEW_INDEX = '__index', '__newindex'
local READ_ACCESSOR, WRITE_ACCESSOR = INDEX, NEW_INDEX
local MODE_ACCESSOR, CALL_ACCESSOR, PAIRS_ACCESSOR = '__mode', '__call', '__pairs'
local STRING_ACCESSOR, NAME_ACCESSOR = '__tostring', '__name'
local ADD_ACCESSOR, SUB_ACCESSOR, MUL_ACCESSOR, DIV_ACCESSOR = '__add', '__sub', '__mul', '__div'
local CONCAT_ACCESSOR, NIL_ACCESSOR = '__concat', nil

local function __index(t, k, accessor)
  local proto, ret = t, nil

  if accessor then
    while proto do
      ret = rawget(proto, accessor)

      if ret ~= nil then
        return ret
      end

      proto = getmetatable(proto)
    end

    return
  end

  accessor = READ_ACCESSOR

  while proto do
    ret = rawget(proto, k)

    if ret ~= nil then
      if __cache and type(ret) == FUNCTION and not rawget(t, k) then
        rawset(t, k, ret)
      end

      return ret
    end

    ret = rawget(proto, accessor)

    if ret ~= nil then
      ret = ret(t, k)

      if ret ~= nil then
        return ret
      end
    end

    proto = getmetatable(proto)
  end
end

local function __newindex(t, k, v)
  local accessor = __index(t, nil, WRITE_ACCESSOR)

  if accessor then
    accessor(t, k, v)
    return
  end

  rawset(t, k, v)
end

local function __send(t, accessor, ...)
  local method = __index(t, nil, accessor)
  return method and method(...) or nil
end

local function __call(self, ...)
  return __send(self, CALL_ACCESSOR, self, ...)
end

local function __add(op1, op2)
  return __send(op1, ADD_ACCESSOR, op1, op2)
end

local function __sub(op1, op2)
  return __send(op1, SUB_ACCESSOR, op1, op2)
end

local function __mul(op1, op2)
  return __send(op1, MUL_ACCESSOR, op1, op2)
end

local function __div(op1, op2)
  return __send(op1, DIV_ACCESSOR, op1, op2)
end

local function __concat(op1, op2)
  return __send(op1, CONCAT_ACCESSOR, op1, op2)
end

local function __pairs(self)
  return __index(self, nil, PAIRS_ACCESSOR) or next, self, nil
end

local function __tostring(self)
  local __tostring = __index(self, nil, STRING_ACCESSOR) or DefaultObjectName

  if type(__tostring) == 'function' then
    return __tostring(self)
  end

  return string.format('%s: %p', __tostring, self)
end

local function fallback(...) error('object is a static module') end

local function extends(proto, extend)
  local metatable = {
    __index = __index,
    __newindex = __newindex,
    __call = __call,
    __add = __add,
    __sub = __sub,
    __mul = __mul,
    __div = __div,
    __concat = __concat,
    __pairs = __pairs,
    __tostring = __tostring,
    __metatable = extend or false
  }
  rawset(proto, '__metatable', metatable)
  return setmetatable(proto, metatable)
end

local function getArgs(func)
  local args = {}
  for i = 1, debug.getinfo(func).nparams, 1 do
    table.insert(args, debug.getlocal(func, i))
  end
  return args
end

local function getAttributes(object, info)
  for k, v in pairs(object) do
    local t = type(rawget(object, k))

    if k ~= '__metatable' then
      if t == 'function' then
        table.insert(info, '@field ' .. k .. ' ' .. 'fun(' .. table.concat(getArgs(v), ", ") .. ')')
      else
        table.insert(info, '@field ' .. k .. ' ' .. t)
      end
    end
  end

  return info
end

local object, Object

local function checkSelf(self)
  if getmetatable(self) == Object then
    return rawget(self, '__self') or error(string.format('Illegal param %s', self))
  end

  if self == object then
    fallback()
  end

  if type(self) ~= TABLE then
    error(string.format('object must be <table> but given <%s> %s', type(self), self))
  end

  return self
end

local function checkProto(self)
  if getmetatable(self) == Object then
    return rawget(self, '__proto') or error(string.format('Illegal param %s', self))
  end

  if type(self) ~= TABLE then
    error(string.format('object must be <table> but given <%s>', type(self)))
  end

  return self
end

local function new(self, ...)
  self = extends({}, checkSelf(self))
  local accessor = __index(self, nil, CALL_ACCESSOR)
  if accessor then
    accessor(self, ...)
  end

  return self
end

local function class(self, extend)
  return extends(checkSelf(self), extend)
end

local function send(self, k, ...)
  local method = checkSelf(self)[k]
  if type(method) == 'function' then
    return method(...)
  end
end

local function respondTo(self, k)
  return type(checkProto(self)[k]) == 'function'
end

local function mixin(self, extend)
  self = checkSelf(self)

  for k, v in pairs(extend) do
    if not (string.find(k, '__') or k == 'new' or k == 'init') then
      if not self[k] then
        rawset(self, k, rawget(extend, k))
      end
    end
  end

  return self
end

local function clone(self)
  self = checkSelf(self)

  local mirror = {}

  for k, v in pairs(self) do
    rawset(mirror, k, v)
  end

  return extends(mirror, getmetatable(self))
end

local function cast(self, proto)
  if getmetatable(self) ~= Object then
    error('casting only support <object>')
    return nil
  end

  return rawset(self, '__proto', proto)
end

local function is(self, extend)
  self = checkSelf(self)

  while self do
    if self == extend then
      return true
    end

    self = getmetatable(self)
  end

  return false
end

local function setWeak(self, mode)
  local metatable = rawget(checkSelf(self), '__metatable')
  if metatable then
    rawset(metatable, '__mode', mode)
    return true
  end
  return false
end

local function description(self, name, extend)
  self = checkSelf(self)

  name = "@class " .. name
  if extend then
    name = name .. ' : ' .. extend
  end

  local info = {name}
  getAttributes(self, info)

  for i = 1, #info do
    print(string.format('--- %s', info[i]))
  end
end

local function setMethodCache(mode)
  if mode then
    __cache = true
    return
  end

  __cache = false
end

object = {
  New = new,
  Extends = class,
  Send = send,
  RespondTo = respondTo,
  Mixin = mixin,
  Clone = clone,
  Is = is,
  SetWeak = setWeak,
  Description = description,
  SetMethodCache = setMethodCache
}

Object = {
  New = new,
  Extends = class,
  Cast = cast,
  Send = send,
  RespondTo = respondTo,
  Mixin = mixin,
  Clone = clone,
  Is = is,
  SetWeak = setWeak,
  Description = description
}

rawset(Object, '__index', function (t, k)
  if k == 'proto' then
    return rawget(t, '__proto')
  end

  return rawget(rawget(t, '__self'), k) or __index(rawget(t, '__proto'), k) or Object[k]
end)
rawset(Object, '__newindex', function (t, k, v)
  local accessor = __index(rawget(t, '__proto'), nil, '__newindex')
  t = rawget(t, '__self')

  if accessor then
    accessor(t, k, v)
    return
  end

  rawset(t, k, v)
end)

setmetatable(Object, {
  __newindex = fallback
})

object = (
  function (self, class, call)
    return setmetatable(self, {
      __call = function (self, table)
        return call(table)
      end,
      __newindex = fallback,
      __metatable = false
    })
  end
  )(
  object, Object,
  function (proto)
    proto = checkSelf(proto)
    return extends({__self = proto, __proto = proto}, Object)
  end
)

return object
)LUA";

// ==================== Demo 窗口脚本 (aLocalDemowindo) ====================
// 从 IDA 0x1802CBEA0 逐字节提取的完整 2359 字节

static const char g_lightDemoScript[] = R"LUA(
local DemoWindow = Light(Light.UI.Window):New()
local nDirection = true
local n = 0
local Line = Light.Graphics.Line;
local Dimensions = {Width = 0, Height = 0}

function DemoWindow:OnOpen()
  Dimensions.Width, Dimensions.Height = DemoWindow:GetDimensions()
end

function DemoWindow:Draw()
  -- Background
  Light.Graphics.Push()
  Light.Graphics.SetColor(35 / 255, 35 / 255, 35 / 255, 1)
  Light.Graphics.Rectangle(
    Light.Graphics.FillMode,
    0, 0, 0,
    Dimensions.Width, Dimensions.Height, 0
  )
  Light.Graphics.Pop()

  -- Center rect
  Light.Graphics.Push()
  Light.Graphics.SetColor(50 / 255, 50 / 255, 50 / 255, 1)
  Light.Graphics.Translate(Dimensions.Width / 2 - 150, Dimensions.Height / 2 - 150, 0)
  Light.Graphics.Polygon(
    Light.Graphics.FillMode,
    0, 0, 0,
    300, 0, 0,
    300, 300, 0,
    0, 300, 0
  )
  Light.Graphics.SetColor(1, 1, 1, 1)
  -- L shape
  Light.Graphics.Polygon(
    Light.Graphics.FillMode,
    100, 0, 0, -- Top left
    135 + n, 0, 0, -- Top right
    135 + n, 180, 0, -- Corner
    200, 220, 0, -- Bottom right
    100, 220, 0 -- Bottom left
  )
  Light.Graphics.Translate(160 + n, 100, 0)
  Light.Graphics.Circle(
    Light.Graphics.FillMode,
    0, 0, 0,
    8,
    16,
    0, -n * 2, 0
  )
  Light.Graphics.Pop()

  Light.Graphics.Push()
  Light.Graphics.Translate(25, Dimensions.Height - 50, 0)
  Light.Graphics.Print("Copyright (c) 2024, Jakit Liang", 0, -25, 0)
  Light.Graphics.Print("Licensed under the BSD 2-Clause License.", 0, 0, 0)
  Light.Graphics.Pop()

  Light.Graphics.Push()
  Light.Graphics.Translate(Dimensions.Width - 150, Dimensions.Height - 50, 0)
  Light.Graphics.Print("ChocoLight v0.1", 0, 0, 0)
  Light.Graphics.Pop()
end

function DemoWindow:Update(dt)
  if nDirection then
    n = n + 0.12
    if n > 20 then
      nDirection = false
    end
  else
    n = n - 0.12
    if n < 0.2 then
      nDirection = true
    end
  end
  Dimensions.Width, Dimensions.Height = DemoWindow:GetDimensions()
end

function DemoWindow:OnKey(key, scanCode, action, mods)
  print(self, key, scanCode, action, mods)
end

function DemoWindow:OnMouseButton(x, y, button, action, mods)
  print(self, "OnMouseButton", x, y, button, action, mods)
end

function DemoWindow:OnMousePosition(x, y)
  print(self, "OnMousePosition", x, y)
end

DemoWindow:Open()
)LUA";

// ==================== EnsureLightTable (sub_1800A52F0) ====================
// 确保全局 "Light" 表存在 — 首次调用时加载 OOP 框架

void LT::EnsureLightTable(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "Light");

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);

        // 加载 OOP 框架 (原始使用 CC::Safe 容器包装, 此处直接使用明文)
        if (luaL_loadstring(L, g_lightInitScript) || lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_error(L);
        }

        // 存入注册表 + 全局表
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "Light");
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_GLOBALSINDEX, "Light");

        // 注册 __lightDemo 闭包 (sub_1800A5250)
        lua_pushcfunction(L, [](lua_State* Ls) -> int {
            int top = lua_gettop(Ls);
            if (luaL_loadstring(Ls, g_lightDemoScript) || lua_pcall(Ls, 0, LUA_MULTRET, 0)) {
                const char* msg = lua_tostring(Ls, -1);
                if (msg) {
                    CC::Log(CC::LOG_ERROR, "%s", msg);
                }
            }
            return lua_gettop(Ls) - top;
        });
        lua_setfield(L, LUA_REGISTRYINDEX, "__lightDemo");

        // 再次获取 Light 表
        lua_getfield(L, LUA_REGISTRYINDEX, "Light");
    }
}

// ==================== RegisterModule ====================
// 通用模块注册辅助 — 提取自所有 luaopen_* 的共同模式

void LT::RegisterModule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    EnsureLightTable(L);

    lua_pushstring(L, name);
    lua_rawget(L, -2);

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, name);
        lua_createtable(L, 0, 0);

        if (funcs) {
            luaL_setfuncs(L, funcs, 0);
        }

        lua_rawset(L, -3);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
    }

    lua_remove(L, -2);
}

// ==================== luaopen_Light (0x1800A53F0) ====================

int luaopen_Light(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "Light");

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);

        if (luaL_loadstring(L, g_lightInitScript) || lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_error(L);
        }

        // 存入注册表 (模块内部使用)
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "Light");

        // 同时设为全局变量 (用户脚本通过 Light.xxx 访问!)
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_GLOBALSINDEX, "Light");

        lua_pushcfunction(L, [](lua_State* Ls) -> int {
            int top = lua_gettop(Ls);
            if (luaL_loadstring(Ls, g_lightDemoScript) || lua_pcall(Ls, 0, LUA_MULTRET, 0)) {
                const char* msg = lua_tostring(Ls, -1);
                if (msg) {
                    CC::Log(CC::LOG_ERROR, "%s", msg);
                }
            }
            return lua_gettop(Ls) - top;
        });
        lua_setfield(L, LUA_REGISTRYINDEX, "__lightDemo");

        lua_getfield(L, LUA_REGISTRYINDEX, "Light");
    }

    return 1;
}

// ==================== Network 空通知 (0x1800B04F0 / 0x1800B0500) ====================

void LT::Network::NotifyEmpty() {}

void LT::Network::NotifyMainEmpty() {}
