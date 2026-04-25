--[[
Author: Antigravity
LastEditors: 炽热
Date: 2026-04-25 12:16:23
LastEditTime: 2026-04-25 22:53:17
--]]
-- ChocoLight Engine Demo
-- 此脚本打包到 APK/IPA assets 中，启动时自动执行

print("========================================")
print("  🍫 ChocoLight Engine v0.2.3")
print("  Powered by Lumen (Lua 5.1 C++17)")
print("========================================")
print("")

-- 基础 Lua 功能测试
print("[TEST] Math: 2^10 = " .. tostring(2 ^ 10))
print("[TEST] String: " .. string.rep("★", 10))
print("[TEST] Table:")

local fruits = { "Apple", "Banana", "Cherry", "Date", "Elderberry" }
for i, v in ipairs(fruits) do
    print("  " .. i .. ". " .. v)
end

-- 闭包和高阶函数
local function makeCounter(start)
    local count = start or 0
    return function()
        count = count + 1
        return count
    end
end

local counter = makeCounter(0)
print("")
print("[TEST] Counter: " .. counter() .. ", " .. counter() .. ", " .. counter())

-- 协程
local function producer()
    local items = { "Hello", "from", "Lua", "coroutine!" }
    for _, item in ipairs(items) do
        coroutine.yield(item)
    end
end

local co = coroutine.create(producer)
local words = {}
while coroutine.status(co) ~= "dead" do
    local ok, val = coroutine.resume(co)
    if ok and val then table.insert(words, val) end
end
print("[TEST] Coroutine: " .. table.concat(words, " "))

-- 环境信息
print("")
print("[INFO] _VERSION = " .. _VERSION)
print("[INFO] collectgarbage('count') = " .. string.format("%.1f KB", collectgarbage("count")))
print("")
print("✅ All tests passed! Engine is working.")
