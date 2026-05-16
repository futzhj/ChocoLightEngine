-- Phase E.4 smoke: Light.Graphics.Bloom (Bloom post-processing surface)
--
-- API coverage (15 functions):
--   Enable / Disable / IsEnabled / IsSupported / Resize
--   SetAutoEnable / GetAutoEnable
--   SetThreshold / GetThreshold
--   SetIntensity / GetIntensity
--   SetRadius / GetRadius
--   SetLevels / GetLevels
--
-- Headless guard: no GL context at smoke stage, Enable() typically
-- returns false (IsSupported=false). Subsequent calls must still be safe (no-op).
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local Bloom = Graphics.Bloom
if type(Bloom) ~= "table" then
    fail("Light.Graphics.Bloom missing or not a table (got " .. type(Bloom) .. ")")
end
pass("Light.Graphics.Bloom subtable present")

-- ============================================================
-- 1) Module surface: 15 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetThreshold", "GetThreshold",
    "SetIntensity", "GetIntensity",
    "SetRadius", "GetRadius",
    "SetLevels", "GetLevels",
    "Process",                              -- Phase F.0.10.3 (region overload)
    -- Phase F.0.10.9.x.2 — Multi-Instance Bloom (5 fn, 与 HDR/TAA 同模板)
    "CreateInstance", "DestroyInstance", "SetActiveInstance",
    "GetActiveInstance", "GetInstanceCount",
    -- Phase F.0.10.9.x.3 — Clone + Snapshot (1-line setup + state introspection)
    "CloneInstance", "GetState",
}
for _, k in ipairs(fn_names) do
    if type(Bloom[k]) ~= "function" then
        fail("Light.Graphics.Bloom." .. k .. " missing or not a function (got " .. type(Bloom[k]) .. ")")
    end
end
pass("Light.Graphics.Bloom module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) IsSupported / IsEnabled — initial state probes
-- ============================================================

local is_sup = Bloom.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = Bloom.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("Initial IsEnabled() == false")

-- ============================================================
-- 3) AutoEnable flag (default true)
-- ============================================================

local ae_default = Bloom.GetAutoEnable()
if type(ae_default) ~= "boolean" then
    fail("GetAutoEnable should return boolean, got " .. type(ae_default))
end
if ae_default ~= true then
    fail("Default GetAutoEnable() must be true, got " .. tostring(ae_default))
end
pass("Default GetAutoEnable() == true")

Bloom.SetAutoEnable(false)
if Bloom.GetAutoEnable() ~= false then
    fail("SetAutoEnable(false) round-trip failed")
end
pass("SetAutoEnable(false) round-trip ok")

Bloom.SetAutoEnable(true)
if Bloom.GetAutoEnable() ~= true then
    fail("SetAutoEnable(true) round-trip failed")
end
pass("SetAutoEnable(true) round-trip ok")

-- ============================================================
-- 4) Threshold round-trip + clamp
-- ============================================================

local thr_default = Bloom.GetThreshold()
if type(thr_default) ~= "number" then
    fail("GetThreshold should return number, got " .. type(thr_default))
end
pass("Default GetThreshold() = " .. tostring(thr_default))

Bloom.SetThreshold(2.5)
if math.abs(Bloom.GetThreshold() - 2.5) > 1e-3 then
    fail("SetThreshold(2.5) round-trip failed, got " .. tostring(Bloom.GetThreshold()))
end
pass("SetThreshold(2.5) round-trip ok")

-- Negative value clamps to 0
Bloom.SetThreshold(-1.0)
if Bloom.GetThreshold() < 0.0 then
    fail("SetThreshold(-1.0) should clamp to 0, got " .. tostring(Bloom.GetThreshold()))
end
pass("SetThreshold(-1.0) clamps to >= 0 (got " .. tostring(Bloom.GetThreshold()) .. ")")

-- Restore default
Bloom.SetThreshold(1.0)

-- ============================================================
-- 5) Intensity round-trip + clamp
-- ============================================================

local int_default = Bloom.GetIntensity()
if type(int_default) ~= "number" then
    fail("GetIntensity should return number, got " .. type(int_default))
end
pass("Default GetIntensity() = " .. tostring(int_default))

Bloom.SetIntensity(1.5)
if math.abs(Bloom.GetIntensity() - 1.5) > 1e-3 then
    fail("SetIntensity(1.5) round-trip failed, got " .. tostring(Bloom.GetIntensity()))
end
pass("SetIntensity(1.5) round-trip ok")

Bloom.SetIntensity(-0.5)
if Bloom.GetIntensity() < 0.0 then
    fail("SetIntensity(-0.5) should clamp to 0, got " .. tostring(Bloom.GetIntensity()))
end
pass("SetIntensity(-0.5) clamps to >= 0 (got " .. tostring(Bloom.GetIntensity()) .. ")")

Bloom.SetIntensity(0.8)

-- ============================================================
-- 6) Radius round-trip + clamp [0, 1]
-- ============================================================

local rad_default = Bloom.GetRadius()
if type(rad_default) ~= "number" then
    fail("GetRadius should return number, got " .. type(rad_default))
end
pass("Default GetRadius() = " .. tostring(rad_default))

Bloom.SetRadius(0.5)
if math.abs(Bloom.GetRadius() - 0.5) > 1e-3 then
    fail("SetRadius(0.5) round-trip failed, got " .. tostring(Bloom.GetRadius()))
end
pass("SetRadius(0.5) round-trip ok")

-- Out-of-range clamps to [0, 1]
Bloom.SetRadius(-2.0)
if Bloom.GetRadius() < 0.0 then
    fail("SetRadius(-2.0) should clamp to 0, got " .. tostring(Bloom.GetRadius()))
end
pass("SetRadius(-2.0) clamps to >= 0 (got " .. tostring(Bloom.GetRadius()) .. ")")

Bloom.SetRadius(5.0)
if Bloom.GetRadius() > 1.0 then
    fail("SetRadius(5.0) should clamp to 1, got " .. tostring(Bloom.GetRadius()))
end
pass("SetRadius(5.0) clamps to <= 1 (got " .. tostring(Bloom.GetRadius()) .. ")")

Bloom.SetRadius(0.7)

-- ============================================================
-- 7) Levels round-trip + clamp [2, 8]
-- ============================================================

local lv_default = Bloom.GetLevels()
if type(lv_default) ~= "number" then
    fail("GetLevels should return number, got " .. type(lv_default))
end
pass("Default GetLevels() = " .. tostring(lv_default))

Bloom.SetLevels(4)
if Bloom.GetLevels() ~= 4 then
    fail("SetLevels(4) round-trip failed, got " .. tostring(Bloom.GetLevels()))
end
pass("SetLevels(4) round-trip ok")

-- Below minimum clamps to 2
Bloom.SetLevels(1)
if Bloom.GetLevels() < 2 then
    fail("SetLevels(1) should clamp to >= 2, got " .. tostring(Bloom.GetLevels()))
end
pass("SetLevels(1) clamps to >= 2 (got " .. tostring(Bloom.GetLevels()) .. ")")

-- Above maximum clamps to 8
Bloom.SetLevels(100)
if Bloom.GetLevels() > 8 then
    fail("SetLevels(100) should clamp to <= 8, got " .. tostring(Bloom.GetLevels()))
end
pass("SetLevels(100) clamps to <= 8 (got " .. tostring(Bloom.GetLevels()) .. ")")

Bloom.SetLevels(5)

-- ============================================================
-- 8) Enable / Disable / Resize lifecycle (headless tolerant)
-- ============================================================
-- In smoke (headless / no GL ctx), Enable typically returns false.
-- All paths must be safe and Disable must not crash.

local en_result = Bloom.Enable(640, 480)
if type(en_result) ~= "boolean" then
    fail("Enable() should return boolean, got " .. type(en_result))
end
pass("Enable(640, 480) returned boolean (value=" .. tostring(en_result) .. ")")

-- IsEnabled should match Enable's return
if Bloom.IsEnabled() ~= en_result then
    fail("After Enable, IsEnabled mismatch (Enable=" .. tostring(en_result) ..
         ", IsEnabled=" .. tostring(Bloom.IsEnabled()) .. ")")
end
pass("IsEnabled matches Enable return")

-- Resize (no-op when not enabled)
local rs_result = Bloom.Resize(800, 600)
if type(rs_result) ~= "boolean" then
    fail("Resize() should return boolean, got " .. type(rs_result))
end
pass("Resize(800, 600) returned boolean (value=" .. tostring(rs_result) .. ")")

-- Disable always safe
Bloom.Disable()
if Bloom.IsEnabled() ~= false then
    fail("After Disable, IsEnabled must be false, got " .. tostring(Bloom.IsEnabled()))
end
pass("Disable() -> IsEnabled() == false")

-- Disable twice safe
Bloom.Disable()
pass("Double Disable() safe (no error)")

-- ============================================================
-- 9) AutoEnable=false prevents HDR link (HDR may be unavailable in smoke,
--    but the SetAutoEnable + GetAutoEnable interaction works)
-- ============================================================

Bloom.SetAutoEnable(false)
-- Even if HDR.Enable succeeds in a host that has GL, Bloom should NOT auto-enable
local HDR = Graphics.HDR
if type(HDR) == "table" and type(HDR.Enable) == "function" then
    local hdr_enabled = HDR.Enable(640, 480)
    if hdr_enabled then
        -- Bloom must not be enabled because autoEnable=false
        if Bloom.IsEnabled() then
            HDR.Disable()
            Bloom.SetAutoEnable(true)
            fail("HDR.Enable triggered Bloom auto-enable even with SetAutoEnable(false)")
        end
        pass("AutoEnable=false: HDR.Enable did not trigger Bloom auto-enable")
        HDR.Disable()
    else
        pass("AutoEnable=false: HDR.Enable returned false (headless / unsupported), test skipped")
    end
end

-- Restore default
Bloom.SetAutoEnable(true)

-- ============================================================
-- Phase F.0.10.3 — Process(region) overload defense (6 PASS)
-- ============================================================
-- HDR 未启 + Bloom 未启 时, Process 应返 nil + err string (silent skip, 不崩)
-- 与 MotionBlur.Process 同模式

-- 测试 1: 无参 Process (full-screen) - HDR 未启 → nil + err
local r1, e1 = Bloom.Process()
if r1 ~= nil then
    fail("Bloom.Process() with HDR off should return nil; got " .. tostring(r1))
end
if type(e1) ~= "string" then
    fail("Bloom.Process() with HDR off should return err string; got " .. type(e1))
end
pass("Bloom.Process() with HDR off returns nil + err string")

-- 测试 2: 4 args region Process - HDR 未启 → nil + err
local r2, e2 = Bloom.Process(0, 0, 100, 100)
if r2 ~= nil or type(e2) ~= "string" then
    fail("Bloom.Process(0,0,100,100) with HDR off should return nil + err; got " ..
         tostring(r2) .. ", " .. type(e2))
end
pass("Bloom.Process(x,y,w,h) with HDR off returns nil + err string")

-- 测试 3: 部分 region 参数 (3 个) → 拒绝
local r3, e3 = Bloom.Process(0, 0, 100)
if r3 ~= nil or type(e3) ~= "string" or not string.find(e3, "expected 0 or 4 args") then
    fail("Bloom.Process(0,0,100) should reject with 'expected 0 or 4 args' err; got " ..
         tostring(r3) .. ", " .. tostring(e3))
end
pass("Bloom.Process partial args rejected (3 args)")

-- 测试 4: w<0 拒绝
local r4, e4 = Bloom.Process(0, 0, -1, 100)
if r4 ~= nil or type(e4) ~= "string" or not string.find(e4, "w/h must be >= 0") then
    fail("Bloom.Process(0,0,-1,100) should reject with 'w/h must be >= 0' err; got " ..
         tostring(r4) .. ", " .. tostring(e4))
end
pass("Bloom.Process w<0 rejected")

-- 测试 5: h<0 拒绝
local r5, e5 = Bloom.Process(0, 0, 100, -1)
if r5 ~= nil or type(e5) ~= "string" or not string.find(e5, "w/h must be >= 0") then
    fail("Bloom.Process(0,0,100,-1) should reject with 'w/h must be >= 0' err; got " ..
         tostring(r5) .. ", " .. tostring(e5))
end
pass("Bloom.Process h<0 rejected")

-- 测试 6: 类型错 (传 string 而非 integer) → luaL_error 抛
local ok6 = pcall(function() Bloom.Process("a", "b", "c", "d") end)
if ok6 then
    fail("Bloom.Process('a','b','c','d') should throw luaL_error; succeeded")
end
pass("Bloom.Process type error throws luaL_error")

-- ============================================================
-- Phase F.0.10.9.x.2 — Multi-Instance Bloom (5 fn round-trip)
-- 与 HDR/TAA multi-instance 同模板: default + 3 user, MAX=4
-- ============================================================

-- 防御性: 强制复位 active=0 (前面章节可能切走)
Bloom.SetActiveInstance(0)

-- I.1 初始状态
local mi_cnt0 = Bloom.GetInstanceCount()
local mi_act0 = Bloom.GetActiveInstance()
if mi_cnt0 ~= 1 then fail("MI.1 初始 count expect 1, got " .. tostring(mi_cnt0)) end
if mi_act0 ~= 0 then fail("MI.1 初始 active expect 0, got " .. tostring(mi_act0)) end
pass("MI.1 初始 instance count=1, active=0")

-- I.2 CreateInstance × 3 → id 应为 1, 2, 3
local id1 = Bloom.CreateInstance()
local id2 = Bloom.CreateInstance()
local id3 = Bloom.CreateInstance()
if id1 ~= 1 or id2 ~= 2 or id3 ~= 3 then
    fail("MI.2 Create x3 expect 1/2/3, got " .. tostring(id1) .. "/" .. tostring(id2) .. "/" .. tostring(id3))
end
if Bloom.GetInstanceCount() ~= 4 then
    fail("MI.2 after Create x3 count expect 4, got " .. tostring(Bloom.GetInstanceCount()))
end
pass("MI.2 Create x3 -> id=1/2/3, count=4")

-- I.3 第 4 次 Create 应返 0 (槽满, MAX_INSTANCES=4)
local id_full = Bloom.CreateInstance()
if id_full ~= 0 then fail("MI.3 Create on full slots expect 0, got " .. tostring(id_full)) end
pass("MI.3 第 4 次 CreateInstance returns 0 (槽满)")

-- I.4 SetActiveInstance round-trip
if not Bloom.SetActiveInstance(id2) then fail("MI.4 SetActive(2) failed") end
if Bloom.GetActiveInstance() ~= 2 then fail("MI.4 GetActive after set expect 2") end
Bloom.SetActiveInstance(0)
if Bloom.GetActiveInstance() ~= 0 then fail("MI.4 切回 0 失败") end
pass("MI.4 SetActiveInstance round-trip (0 <-> 2)")

-- I.5 Per-instance 参数隔离 (instance 0 vs 1 各自 SetIntensity 不互扰)
Bloom.SetActiveInstance(0); Bloom.SetIntensity(0.3)
Bloom.SetActiveInstance(id1); Bloom.SetIntensity(1.5)
Bloom.SetActiveInstance(0)
if math.abs(Bloom.GetIntensity() - 0.3) > 1e-4 then
    fail("MI.5 instance 0 intensity 被污染, expect 0.3, got " .. Bloom.GetIntensity())
end
Bloom.SetActiveInstance(id1)
if math.abs(Bloom.GetIntensity() - 1.5) > 1e-4 then
    fail("MI.5 instance 1 intensity 期望 1.5, got " .. Bloom.GetIntensity())
end
Bloom.SetActiveInstance(0)
pass("MI.5 Per-instance 参数隔离 (instance 0=0.3, 1=1.5)")

-- I.6 DestroyInstance(0) 拒绝 (default 不可销毁)
if Bloom.DestroyInstance(0) ~= false then fail("MI.6 Destroy(0) should reject") end
pass("MI.6 DestroyInstance(0) 拒绝 (default 不可销毁)")

-- I.7 SetActiveInstance(无效 id) 拒绝
if Bloom.SetActiveInstance(99) ~= false then fail("MI.7 SetActive(99) should reject") end
if Bloom.SetActiveInstance(-1) ~= false then fail("MI.7 SetActive(-1) should reject") end
pass("MI.7 SetActiveInstance(无效 id) 拒绝")

-- I.8 清理: Destroy 全部 user instance + 验证 count
Bloom.DestroyInstance(id1)
Bloom.DestroyInstance(id2)
Bloom.DestroyInstance(id3)
if Bloom.GetInstanceCount() ~= 1 then
    fail("MI.8 cleanup count expect 1, got " .. tostring(Bloom.GetInstanceCount()))
end
if Bloom.GetActiveInstance() ~= 0 then
    fail("MI.8 cleanup active expect 0, got " .. tostring(Bloom.GetActiveInstance()))
end
pass("MI.8 Destroy 全部 user instance, count=1, active=0")

-- ============================================================
-- Phase F.0.10.9.x.3 — Clone + GetState (1-line setup + snapshot)
-- ============================================================

Bloom.SetActiveInstance(0)

-- CS.1 GetState 字段完整性 (default instance)
local s0 = Bloom.GetState()
if type(s0) ~= "table" then fail("CS.1 GetState should return table") end
local required = { "threshold", "intensity", "radius", "levels", "auto_enable", "enabled", "supported" }
for _, k in ipairs(required) do
    if s0[k] == nil then fail("CS.1 GetState missing field: " .. k) end
end
pass("CS.1 GetState 字段完整 (" .. #required .. " 字段)")

-- CS.2 Clone(0) 复制 default profile 到新 instance + 字段一致
-- 注: threshold ∈ [0, +∞), intensity ∈ [0, +∞), radius ∈ [0, 1] (BloomRenderer clamp)
Bloom.SetActiveInstance(0)
Bloom.SetThreshold(1.5); Bloom.SetIntensity(0.8); Bloom.SetRadius(0.7)
local cid = Bloom.CloneInstance(0)
if cid <= 0 or cid > 3 then fail("CS.2 Clone(0) expect [1,3], got " .. cid) end
Bloom.SetActiveInstance(cid)
local sc = Bloom.GetState()
if math.abs(sc.threshold - 1.5) > 1e-4 then fail("CS.2 cloned threshold expect 1.5, got " .. sc.threshold) end
if math.abs(sc.intensity - 0.8) > 1e-4 then fail("CS.2 cloned intensity expect 0.8, got " .. sc.intensity) end
if math.abs(sc.radius    - 0.7) > 1e-4 then fail("CS.2 cloned radius expect 0.7, got " .. sc.radius) end
if sc.enabled ~= false then fail("CS.2 cloned instance expect enabled=false (待自己 Enable)") end
pass("CS.2 Clone(0) 复制调参 + enabled=false")

-- CS.3 Clone 后修改新 instance 不影响原 instance (per-instance 隔离)
Bloom.SetActiveInstance(cid)
Bloom.SetIntensity(99.0)   -- 此值会被 clamp 到 [0, 4], 但仅修改 cloned instance
Bloom.SetActiveInstance(0)
if math.abs(Bloom.GetIntensity() - 0.8) > 1e-4 then
    fail("CS.3 default instance intensity 被污染, expect 0.8, got " .. Bloom.GetIntensity())
end
pass("CS.3 Clone 后 per-instance 修改隔离 (default 不被污染)")

-- CS.4 Clone(srcId) 非法参数拒绝
if Bloom.CloneInstance(99) ~= 0  then fail("CS.4 Clone(99) expect 0") end
if Bloom.CloneInstance(-1) ~= 0  then fail("CS.4 Clone(-1) expect 0") end
pass("CS.4 Clone(无效 srcId) 返 0")

-- CS.5 槽满后 Clone 返 0
local extra1 = Bloom.CloneInstance(0)
local extra2 = Bloom.CloneInstance(0)
if extra1 == 0 or extra2 == 0 then fail("CS.5 setup: Clone x2 should both succeed") end
local extra3 = Bloom.CloneInstance(0)
if extra3 ~= 0 then fail("CS.5 第 4 次 Clone expect 0 (槽满), got " .. extra3) end
pass("CS.5 Clone on full slots returns 0")

-- 清理
Bloom.DestroyInstance(cid)
Bloom.DestroyInstance(extra1)
Bloom.DestroyInstance(extra2)

print("[OK] Phase E.4 + F.0.10.3 + F.0.10.9.x.2 + F.0.10.9.x.3 smoke (Light.Graphics.Bloom): all checks passed")
