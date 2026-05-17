-- Phase F.2 — 多实例 binding + TAAU downstream resize smoke 测试
-- 在不打开窗口的情况下验证5 个新模块的 multi-instance API 存在 + 行为正确.

local function fail(msg) io.write("[FAIL] " .. msg .. "\n"); os.exit(1) end
local function pass(msg) io.write("[PASS] " .. msg .. "\n") end

local G = Light.Graphics

-- ==================== 1) 多实例 API 存在性检查 ====================
local mods = { "AutoExposure", "LensDirt", "Streak", "LensFlare", "SSAO" }
local must_have = { "CreateInstance", "DestroyInstance", "SetActiveInstance",
                    "GetActiveInstance", "GetInstanceCount", "CloneInstance" }
for _, mn in ipairs(mods) do
    local m = G[mn]
    if m == nil then fail("Light.Graphics." .. mn .. " missing") end
    for _, fn in ipairs(must_have) do
        if type(m[fn]) ~= "function" then
            fail(mn .. "." .. fn .. " not a function (got " .. type(m[fn]) .. ")")
        end
    end
    pass(mn .. " has 6 multi-instance functions")
end

-- ==================== 2) 默认 instance count = 1, active = 0 ====================
for _, mn in ipairs(mods) do
    local m = G[mn]
    if m.GetInstanceCount() ~= 1 then
        fail(mn .. ".GetInstanceCount() should default to 1, got " .. tostring(m.GetInstanceCount()))
    end
    if m.GetActiveInstance() ~= 0 then
        fail(mn .. ".GetActiveInstance() should default to 0, got " .. tostring(m.GetActiveInstance()))
    end
end
pass("All 5 modules: default count=1, active=0")

-- ==================== 3) Create / Set Active / Destroy 路径 ====================
for _, mn in ipairs(mods) do
    local m = G[mn]
    local id = m.CreateInstance()
    if id < 1 or id > 3 then fail(mn .. ".CreateInstance returned invalid id " .. tostring(id)) end
    if m.GetInstanceCount() ~= 2 then fail(mn .. " count after Create != 2") end

    if not m.SetActiveInstance(id) then fail(mn .. ".SetActiveInstance(" .. id .. ") failed") end
    if m.GetActiveInstance() ~= id then fail(mn .. ".GetActiveInstance != " .. id) end

    -- 切回 default
    if not m.SetActiveInstance(0) then fail(mn .. ".SetActiveInstance(0) failed") end

    -- 销毁
    if not m.DestroyInstance(id) then fail(mn .. ".DestroyInstance failed") end
    if m.GetInstanceCount() ~= 1 then fail(mn .. " count after Destroy != 1") end
end
pass("All 5 modules: Create/SetActive/Destroy round-trip ok")

-- ==================== 4) 4 instance 上限 ====================
local m = G.LensDirt
local ids = {}
for i = 1, 3 do
    table.insert(ids, m.CreateInstance())
end
if m.GetInstanceCount() ~= 4 then fail("LensDirt count after 3 Create != 4") end
local extra = m.CreateInstance()
if extra ~= 0 then fail("LensDirt 5th CreateInstance should return 0 (full), got " .. tostring(extra)) end
pass("LensDirt: 4 instance 上限")

-- 清理
for _, id in ipairs(ids) do m.DestroyInstance(id) end
if m.GetInstanceCount() ~= 1 then fail("LensDirt cleanup failed") end
pass("LensDirt: cleanup back to count=1")

-- ==================== 5) Clone 路径 ====================
G.LensFlare.SetIntensity(0.7)   -- 改 default 参数
local cloneId = G.LensFlare.CloneInstance(0)
if cloneId < 1 then fail("LensFlare.CloneInstance(0) failed") end
G.LensFlare.SetActiveInstance(cloneId)
if math.abs(G.LensFlare.GetIntensity() - 0.7) > 0.001 then
    fail("Clone 应继承 default intensity=0.7, got " .. tostring(G.LensFlare.GetIntensity()))
end
G.LensFlare.SetActiveInstance(0)
G.LensFlare.DestroyInstance(cloneId)
pass("LensFlare: Clone 继承参数 ok")

-- ==================== 6) 边界检查 ====================
-- 销毁 0 (default) 应失败
if G.SSAO.DestroyInstance(0) then fail("SSAO.DestroyInstance(0) should fail (cannot destroy default)") end
-- 切到未分配应失败
if G.SSAO.SetActiveInstance(99) then fail("SSAO.SetActiveInstance(99) should fail") end
pass("SSAO: 边界检查 (不能销毁 default, 不能切到未分配 id)")

io.write("\n[OK] Phase F.2 multi-instance smoke 全部通过\n")
