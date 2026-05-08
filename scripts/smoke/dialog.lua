-- Phase L smoke: Light.Dialog
--
-- 关键约束: Windows CI runner 调 ShowOpenFile/Save/Folder 会真的弹出
-- 原生 IFileDialog (即使 headless 会话). 这会阻塞 CI 直到 timeout.
--
-- 因此 smoke 只验证 fn 注册 + 边界 + Poll API 安全, **不真 launch dialog**.
-- 真实 launch + callback 流程由 samples/demo_dialog/main.lua 提供给开发者.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

local ok, mod = pcall(require, "Light.Dialog")
if not ok then fail("require(Light.Dialog) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Dialog not a table") end

-- 6 个 fn 注册校验
for _, k in ipairs({
    "ShowOpenFile", "ShowSaveFile", "ShowOpenFolder",
    "PollEvents", "PollResult", "IsSupported",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Dialog." .. k .. " missing") end
end
pass("Light.Dialog module ok (6 functions)")

-- IsSupported 总返回 bool
local sup = mod.IsSupported()
if type(sup) ~= "boolean" then fail("IsSupported must return bool") end
pass("Light.Dialog.IsSupported = " .. tostring(sup))

-- ===== 边界路径: opts 类型校验 (失败前置, 不会 launch dialog) =====

-- opts = number (非法类型) -> nil + err
local r1, e1 = mod.ShowOpenFile(123)
if r1 ~= nil or e1 == nil then fail("ShowOpenFile(number) should be nil+err") end
pass("Light.Dialog.ShowOpenFile(number) boundary ok: " .. tostring(e1))

local r1b, e1b = mod.ShowSaveFile(true)
if r1b ~= nil or e1b == nil then fail("ShowSaveFile(bool) should be nil+err") end
pass("Light.Dialog.ShowSaveFile(bool) boundary ok: " .. tostring(e1b))

local r1c, e1c = mod.ShowOpenFolder("string")
if r1c ~= nil or e1c == nil then fail("ShowOpenFolder(string) should be nil+err") end
pass("Light.Dialog.ShowOpenFolder(string) boundary ok: " .. tostring(e1c))

-- callback = string (非法类型) -> nil + err
local r2, e2 = mod.ShowOpenFile(nil, "not a function")
if r2 ~= nil or e2 == nil then fail("callback string should be nil+err") end
pass("Light.Dialog.ShowOpenFile(nil, string) callback type check ok: " .. tostring(e2))

local r2b, e2b = mod.ShowSaveFile(nil, 12345)
if r2b ~= nil or e2b == nil then fail("callback number should be nil+err") end
pass("Light.Dialog.ShowSaveFile(nil, number) callback type check ok: " .. tostring(e2b))

-- filters 不是 table (非法类型) -> nil + err
local r3, e3 = mod.ShowOpenFile({ filters = "*.txt" })
if r3 ~= nil or e3 == nil then fail("filters as string should be nil+err") end
pass("Light.Dialog.ShowOpenFile(filters=string) boundary ok: " .. tostring(e3))

-- filters 项不是 table -> nil + err
local r3b, e3b = mod.ShowOpenFile({ filters = { "wrong" } })
if r3b ~= nil or e3b == nil then fail("filters[1]=string should be nil+err") end
pass("Light.Dialog.ShowOpenFile(filters={string}) boundary ok: " .. tostring(e3b))

-- ===== Poll API 在没有 launch 任何 dialog 的状态下 =====

-- PollResult 不存在的 id -> nil
local pr = mod.PollResult(999999)
if pr ~= nil then fail("PollResult(non-existent) should be nil") end
pass("Light.Dialog.PollResult(non-existent) = nil ok")

local pr2 = mod.PollResult(-1)
if pr2 ~= nil then fail("PollResult(-1) should be nil") end
pass("Light.Dialog.PollResult(-1) = nil ok")

-- PollEvents 没有 pending -> 返回 0 (single int)
local d = mod.PollEvents()
if d ~= 0 then fail("PollEvents (no pending) should be 0, got " .. tostring(d)) end
pass("Light.Dialog.PollEvents (empty) = 0 ok")

-- 多次调用应继续返回 0
local d2 = mod.PollEvents()
if d2 ~= 0 then fail("repeat PollEvents should still be 0") end
pass("Light.Dialog.PollEvents (repeat) = 0 ok")

-- ===== PollResult 参数类型: 非数字 -> luaL_checkinteger 会抛 lua error =====
-- 但 pcall 包住验证错误确实抛出, 而不是崩溃
local cok, cerr = pcall(mod.PollResult, "not a number")
if cok then fail("PollResult('not a number') should raise lua error") end
pass("Light.Dialog.PollResult(non-int) raises lua error as expected")

print("dialog smoke ok (boundary-only; no real dialog launched)")
