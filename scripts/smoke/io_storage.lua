-- io_storage.lua
-- Phase C 烟雾测试 (无窗口环境)
-- 验证: Light.IO 与 Light.Storage 模块 API 表已注册到 Lua 全局命名空间
-- 实际异步加载/存储读写需在带 OS 文件系统的环境中运行集成测试

local function fail(message)
  error(message, 2)
end

local function assert_type(value, expected, label)
  if type(value) ~= expected then
    fail(label .. ": expected type " .. expected .. ", got " .. type(value))
  end
end

local function assert_function(value, label)
  if type(value) ~= "function" then
    fail(label .. ": expected function, got " .. type(value))
  end
end

local function require_table(name)
  local ok, module_or_error = pcall(require, name)
  if not ok then
    fail("require(" .. name .. ") failed: " .. tostring(module_or_error))
  end
  assert_type(module_or_error, "table", name)
  return module_or_error
end

-- ==================== Light.IO (SDL_AsyncIO) ====================

local IO = require_table("Light.IO")
assert_function(IO.LoadAsync, "Light.IO.LoadAsync")
assert_function(IO.Poll,      "Light.IO.Poll")

-- 调一次 Poll, 队列为空时应返回 0 不报错
local triggered = IO.Poll()
assert_type(triggered, "number", "Light.IO.Poll() return")
if triggered ~= 0 then
  fail("Light.IO.Poll() should be 0 on empty queue, got " .. tostring(triggered))
end

-- ==================== Light.Storage (SDL_Storage) ====================

local Storage = require_table("Light.Storage")
assert_function(Storage.OpenUser,     "Light.Storage.OpenUser")
assert_function(Storage.OpenLocalDir, "Light.Storage.OpenLocalDir")
assert_function(Storage.CloseUser,    "Light.Storage.CloseUser")
assert_function(Storage.Space,        "Light.Storage.Space")

-- Title 子表 (只读, 自动延迟打开)
assert_type(Storage.Title, "table", "Light.Storage.Title")
assert_function(Storage.Title.Read,   "Light.Storage.Title.Read")
assert_function(Storage.Title.Exists, "Light.Storage.Title.Exists")
assert_function(Storage.Title.Size,   "Light.Storage.Title.Size")

-- User 子表 (读写, 需先 OpenUser)
assert_type(Storage.User, "table", "Light.Storage.User")
assert_function(Storage.User.Read,      "Light.Storage.User.Read")
assert_function(Storage.User.Write,     "Light.Storage.User.Write")
assert_function(Storage.User.Exists,    "Light.Storage.User.Exists")
assert_function(Storage.User.Delete,    "Light.Storage.User.Delete")
assert_function(Storage.User.Size,      "Light.Storage.User.Size")
assert_function(Storage.User.Mkdir,     "Light.Storage.User.Mkdir")
assert_function(Storage.User.Enumerate, "Light.Storage.User.Enumerate")
assert_function(Storage.User.Rename,    "Light.Storage.User.Rename")
assert_function(Storage.User.Glob,      "Light.Storage.User.Glob")

-- User 在未 OpenUser 时调 Write, 应返回 false + err 字符串 (不崩溃)
local ok, err = Storage.User.Write("__smoke__.txt", "hello")
if ok then
  fail("Light.Storage.User.Write should fail before OpenUser, but returned ok")
end
assert_type(err, "string", "Light.Storage.User.Write error message")

-- Mkdir 未 OpenUser 时 也应安全 返回错误
local mok, merr = Storage.User.Mkdir("__smoke_dir__")
if mok then
  fail("Light.Storage.User.Mkdir should fail before OpenUser, but returned ok")
end
assert_type(merr, "string", "Light.Storage.User.Mkdir error message")

-- Enumerate 未 OpenUser 时 返回 nil + err
local list, eerr = Storage.User.Enumerate(".")
if list ~= nil then
  fail("Light.Storage.User.Enumerate should return nil before OpenUser")
end
assert_type(eerr, "string", "Light.Storage.User.Enumerate error message")

-- Space 未 OpenUser 时 返回 nil + err
local space, serr = Storage.Space()
if space ~= nil then
  fail("Light.Storage.Space should return nil before OpenUser")
end
assert_type(serr, "string", "Light.Storage.Space error message")

-- Rename 未 OpenUser 时 返回 false + err
local rok, rerr = Storage.User.Rename("a", "b")
if rok then
  fail("Light.Storage.User.Rename should fail before OpenUser, but returned ok")
end
assert_type(rerr, "string", "Light.Storage.User.Rename error message")

-- Glob 未 OpenUser 时 返回 nil + err
local glist, gerr = Storage.User.Glob(".", "*.sav")
if glist ~= nil then
  fail("Light.Storage.User.Glob should return nil before OpenUser")
end
assert_type(gerr, "string", "Light.Storage.User.Glob error message")

print("io_storage ok")
