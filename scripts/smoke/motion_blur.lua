-- Phase E.15+E.16+E.17+E.18 smoke: Light.Graphics.MotionBlur (velocity-driven motion blur surface)
--
-- API coverage (15 functions):
--   Lifecycle 5: Enable / Disable / IsEnabled / IsSupported / Resize
--   AutoEnable 2: SetAutoEnable / GetAutoEnable          (default false)
--   Params     4: SetStrength / GetStrength (clamp [0, 4])
--                 SetSampleCount / GetSampleCount (clamp [1, 32])
--   Phase E.16 2: SetMode / GetMode (default 0; clamp [0, 2])
--                 0=combined / 1=camera_only / 2=object_only
--   Phase E.17 2: SetHalfRes / GetHalfRes (default false)
--                 true=half-res (VRAM -75%, perf ~4x)
--
-- Phase E.18 behavior upgrade (no new API; reuses HDR.SetVelocityDilation):
--   - Velocity 9-tap max-length dilation moved to an independent HDR EndScene pass.
--   - When HDR.SetVelocityDilation(true) (default) AND backend supports dilation pass,
--     SSR Temporal + Motion Blur consumers share one pre-dilated velocity tex,
--     skipping inline 9-tap (~50% velocity-fetch savings in multi-consumer scenes).
--   - Backward compatible: consumer falls back to raw velocity + inline 9-tap when
--     dilation pass unsupported / RT creation failed.
--
-- Headless guard: same as hdr.lua. Enable() MUST either
--   (a) return false cleanly when no GL ctx (typical) OR
--   (b) return true if host already has GL ctx.
-- All Set/Get round-trip + clamp must work regardless of Enable state.
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local MB = Graphics.MotionBlur
if type(MB) ~= "table" then
    fail("Light.Graphics.MotionBlur missing or not a table (got " .. type(MB) .. ")")
end
pass("Light.Graphics.MotionBlur subtable present")

-- ============================================================
-- 1) Module surface: 11 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetStrength", "GetStrength",
    "SetSampleCount", "GetSampleCount",
    "SetMode", "GetMode",                       -- Phase E.16
    "SetHalfRes", "GetHalfRes",                 -- Phase E.17
    "Process",                                  -- Phase F.0.10.3 (region overload)
    -- Phase F.0.10.9.x.2 — Multi-Instance MotionBlur (5 fn)
    "CreateInstance", "DestroyInstance", "SetActiveInstance",
    "GetActiveInstance", "GetInstanceCount",
    -- Phase F.0.10.9.x.3 — Clone + Snapshot
    "CloneInstance", "GetState",
    -- Phase F.0.10.9.x.4 — SetState (反向 GetState)
    "SetState",
}
for _, k in ipairs(fn_names) do
    if type(MB[k]) ~= "function" then
        fail("Light.Graphics.MotionBlur." .. k .. " missing or not a function (got " .. type(MB[k]) .. ")")
    end
end
pass("Light.Graphics.MotionBlur module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) Initial state probes (IsSupported / IsEnabled)
-- ============================================================

local is_sup = MB.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = MB.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("IsEnabled() = false initially")

-- ============================================================
-- 3) Default values round-trip
-- ============================================================

local function approx(a, b) return math.abs(a - b) < 1e-4 end

-- AutoEnable default = false
local ae0 = MB.GetAutoEnable()
if type(ae0) ~= "boolean" then
    fail("GetAutoEnable should return boolean, got " .. type(ae0))
end
if ae0 ~= false then
    fail("Default GetAutoEnable() must be false, got " .. tostring(ae0))
end
pass("GetAutoEnable() default = false")

-- Strength default = 1.0
local s0 = MB.GetStrength()
if type(s0) ~= "number" then
    fail("GetStrength should return number, got " .. type(s0))
end
if not approx(s0, 1.0) then
    fail("Default GetStrength() must be 1.0, got " .. tostring(s0))
end
pass("GetStrength() default = 1.0")

-- SampleCount default = 8
local n0 = MB.GetSampleCount()
if type(n0) ~= "number" then     -- lua_pushinteger 在 Lumen 中可能映射为 number
    fail("GetSampleCount should return number/integer, got " .. type(n0))
end
if math.floor(n0 + 0.5) ~= 8 then
    fail("Default GetSampleCount() must be 8, got " .. tostring(n0))
end
pass("GetSampleCount() default = 8")

-- ============================================================
-- 4) Set/Get round-trip
-- ============================================================

MB.SetAutoEnable(true)
if MB.GetAutoEnable() ~= true then
    fail("SetAutoEnable(true) round-trip failed")
end
MB.SetAutoEnable(false)
if MB.GetAutoEnable() ~= false then
    fail("SetAutoEnable(false) round-trip failed")
end
pass("SetAutoEnable / GetAutoEnable round-trip ok")

MB.SetStrength(2.5)
if not approx(MB.GetStrength(), 2.5) then
    fail("SetStrength(2.5) round-trip failed (got " .. tostring(MB.GetStrength()) .. ")")
end
MB.SetStrength(1.0)        -- 复位
pass("SetStrength / GetStrength round-trip ok")

MB.SetSampleCount(16)
if MB.GetSampleCount() ~= 16 then
    fail("SetSampleCount(16) round-trip failed (got " .. tostring(MB.GetSampleCount()) .. ")")
end
MB.SetSampleCount(8)        -- 复位
pass("SetSampleCount / GetSampleCount round-trip ok")

-- ============================================================
-- 5) Clamp behavior (strength [0, 4], sampleCount [1, 32])
-- ============================================================

MB.SetStrength(-1.0)
if not approx(MB.GetStrength(), 0.0) then
    fail("SetStrength(-1) should clamp to 0, got " .. tostring(MB.GetStrength()))
end
pass("SetStrength clamp lower bound (0)")

MB.SetStrength(99.0)
if not approx(MB.GetStrength(), 4.0) then
    fail("SetStrength(99) should clamp to 4, got " .. tostring(MB.GetStrength()))
end
pass("SetStrength clamp upper bound (4)")

MB.SetStrength(1.0)         -- 复位

MB.SetSampleCount(0)
if MB.GetSampleCount() ~= 1 then
    fail("SetSampleCount(0) should clamp to 1, got " .. tostring(MB.GetSampleCount()))
end
pass("SetSampleCount clamp lower bound (1)")

MB.SetSampleCount(99)
if MB.GetSampleCount() ~= 32 then
    fail("SetSampleCount(99) should clamp to 32, got " .. tostring(MB.GetSampleCount()))
end
pass("SetSampleCount clamp upper bound (32)")

MB.SetSampleCount(8)        -- 复位

-- ============================================================
-- 6) Enable / Disable cycle (headless friendly)
-- ============================================================

-- Enable 在 headless 下可能返 false (无 GL ctx) 或 true (有 ctx). 都接受.
local r = MB.Enable(640, 480)
if type(r) ~= "boolean" then
    fail("Enable should return boolean, got " .. type(r))
end
if r and not MB.IsEnabled() then
    fail("Enable returned true but IsEnabled() = false")
end
pass("Enable(640, 480) returns boolean (= " .. tostring(r) .. ")")

-- Resize 同样允许 headless 失败
local r2 = MB.Resize(800, 600)
if type(r2) ~= "boolean" then
    fail("Resize should return boolean, got " .. type(r2))
end
pass("Resize(800, 600) returns boolean (= " .. tostring(r2) .. ")")

-- Disable 必须 idempotent (无论 Enable 是否成功)
MB.Disable()
if MB.IsEnabled() ~= false then
    fail("After Disable, IsEnabled() must be false, got " .. tostring(MB.IsEnabled()))
end
pass("Disable() -> IsEnabled() = false")

-- 二次 Disable idempotent
MB.Disable()
pass("Double Disable() idempotent")

-- ============================================================
-- 7) Phase E.16 — Mode default / round-trip / clamp
-- ============================================================

-- 默认 mode = 0 (combined, 与 Phase E.15 行为一致)
local mode0 = MB.GetMode()
if type(mode0) ~= "number" then
    fail("GetMode should return number/integer, got " .. type(mode0))
end
if math.floor(mode0 + 0.5) ~= 0 then
    fail("Default GetMode() must be 0 (combined), got " .. tostring(mode0))
end
pass("GetMode() default = 0 (combined)")

-- round-trip 1 (camera_only)
MB.SetMode(1)
if math.floor(MB.GetMode() + 0.5) ~= 1 then
    fail("SetMode(1) round-trip failed (got " .. tostring(MB.GetMode()) .. ")")
end

-- round-trip 2 (object_only)
MB.SetMode(2)
if math.floor(MB.GetMode() + 0.5) ~= 2 then
    fail("SetMode(2) round-trip failed (got " .. tostring(MB.GetMode()) .. ")")
end
pass("SetMode / GetMode round-trip ok (1=camera_only, 2=object_only)")

-- clamp 下界
MB.SetMode(-5)
if math.floor(MB.GetMode() + 0.5) ~= 0 then
    fail("SetMode(-5) should clamp to 0, got " .. tostring(MB.GetMode()))
end
pass("SetMode clamp lower bound (0)")

-- clamp 上界
MB.SetMode(99)
if math.floor(MB.GetMode() + 0.5) ~= 2 then
    fail("SetMode(99) should clamp to 2, got " .. tostring(MB.GetMode()))
end
pass("SetMode clamp upper bound (2)")

MB.SetMode(0)               -- 复位为 combined

-- ============================================================
-- 8) Phase E.17 — HalfRes default / round-trip / no-op
-- ============================================================

-- 默认 false (full-res, 与 Phase E.15/E.16 一致)
local hr0 = MB.GetHalfRes()
if type(hr0) ~= "boolean" then
    fail("GetHalfRes should return boolean, got " .. type(hr0))
end
if hr0 ~= false then
    fail("Default GetHalfRes() must be false, got " .. tostring(hr0))
end
pass("GetHalfRes() default = false")

-- round-trip true → false
MB.SetHalfRes(true)
if MB.GetHalfRes() ~= true then
    fail("SetHalfRes(true) round-trip failed")
end
MB.SetHalfRes(false)
if MB.GetHalfRes() ~= false then
    fail("SetHalfRes(false) round-trip failed")
end
pass("SetHalfRes / GetHalfRes round-trip ok")

-- SetHalfRes 不应损坏 IsEnabled 状态类型 (headless 友好)
MB.SetHalfRes(true)
if type(MB.IsEnabled()) ~= "boolean" then
    fail("SetHalfRes 不应影响 IsEnabled 返回类型")
end
MB.SetHalfRes(false)        -- 复位
pass("SetHalfRes does not corrupt IsEnabled state")

-- ============================================================
-- Phase F.0.10.3 — Process(region) overload defense (6 PASS)
-- ============================================================
-- HDR 未启 + MB 未启 时, Process 应返 nil + err string (silent skip, 不崩)
-- 4-args 防御 / 部分 args 拒绝 / w<0 拒绝 / 类型错 luaL_error 抛
-- 这些验证均在 headless / HDR-off 环境运行 (与 TAA.Process 同模式)

-- 测试 1: 无参 Process (full-screen) - HDR 未启 → nil + err
local r1, e1 = MB.Process()
if r1 ~= nil then
    fail("MB.Process() with HDR off should return nil; got " .. tostring(r1))
end
if type(e1) ~= "string" then
    fail("MB.Process() with HDR off should return err string; got " .. type(e1))
end
pass("MB.Process() with HDR off returns nil + err string")

-- 测试 2: 4 args region Process - HDR 未启 → nil + err
local r2, e2 = MB.Process(0, 0, 100, 100)
if r2 ~= nil or type(e2) ~= "string" then
    fail("MB.Process(0,0,100,100) with HDR off should return nil + err; got " ..
         tostring(r2) .. ", " .. type(e2))
end
pass("MB.Process(x,y,w,h) with HDR off returns nil + err string")

-- 测试 3: 部分 region 参数 (3 个) → 拒绝 (与 TAA.Process 同模式)
local r3, e3 = MB.Process(0, 0, 100)
if r3 ~= nil or type(e3) ~= "string" or not string.find(e3, "expected 0 or 4 args") then
    fail("MB.Process(0,0,100) should reject with 'expected 0 or 4 args' err; got " ..
         tostring(r3) .. ", " .. tostring(e3))
end
pass("MB.Process partial args rejected (3 args)")

-- 测试 4: w<0 拒绝
local r4, e4 = MB.Process(0, 0, -1, 100)
if r4 ~= nil or type(e4) ~= "string" or not string.find(e4, "w/h must be >= 0") then
    fail("MB.Process(0,0,-1,100) should reject with 'w/h must be >= 0' err; got " ..
         tostring(r4) .. ", " .. tostring(e4))
end
pass("MB.Process w<0 rejected")

-- 测试 5: h<0 拒绝
local r5, e5 = MB.Process(0, 0, 100, -1)
if r5 ~= nil or type(e5) ~= "string" or not string.find(e5, "w/h must be >= 0") then
    fail("MB.Process(0,0,100,-1) should reject with 'w/h must be >= 0' err; got " ..
         tostring(r5) .. ", " .. tostring(e5))
end
pass("MB.Process h<0 rejected")

-- 测试 6: 类型错 (传 string 而非 integer) → luaL_error 抛
local ok6 = pcall(function() MB.Process("a", "b", "c", "d") end)
if ok6 then
    fail("MB.Process('a','b','c','d') should throw luaL_error; succeeded")
end
pass("MB.Process type error throws luaL_error")

-- ============================================================
-- Phase F.0.10.3 — HDR.SetAutoMotionBlur / GetAutoMotionBlur (4 PASS)
-- ============================================================
local HDR = Graphics.HDR
if type(HDR) ~= "table" then fail("HDR subtable missing") end

-- 默认 true
if HDR.GetAutoMotionBlur() ~= true then
    fail("HDR.GetAutoMotionBlur() default should be true, got " ..
         tostring(HDR.GetAutoMotionBlur()))
end
pass("HDR.GetAutoMotionBlur() default = true")

-- round-trip true → false → true
HDR.SetAutoMotionBlur(false)
if HDR.GetAutoMotionBlur() ~= false then
    fail("HDR.SetAutoMotionBlur(false) round-trip failed")
end
HDR.SetAutoMotionBlur(true)
if HDR.GetAutoMotionBlur() ~= true then
    fail("HDR.SetAutoMotionBlur(true) round-trip failed")
end
pass("HDR.SetAutoMotionBlur round-trip ok")

-- 同值 no-op (set true 两次)
HDR.SetAutoMotionBlur(true)
HDR.SetAutoMotionBlur(true)
if HDR.GetAutoMotionBlur() ~= true then
    fail("HDR.SetAutoMotionBlur idempotent failed")
end
pass("HDR.SetAutoMotionBlur idempotent (same value no-op)")

-- 类型错 (传 string) → nil + err
local r7, e7 = HDR.SetAutoMotionBlur("yes")
if r7 ~= nil or type(e7) ~= "string" then
    fail("HDR.SetAutoMotionBlur('yes') should return nil + err; got " ..
         tostring(r7) .. ", " .. type(e7))
end
pass("HDR.SetAutoMotionBlur type error returns nil + err")

-- ============================================================
-- Phase F.0.10.9.x.2 — Multi-Instance MotionBlur (5 fn round-trip)
-- ============================================================

MB.SetActiveInstance(0)   -- 防御性复位

-- MI.1 初始
if MB.GetInstanceCount() ~= 1 then fail("MI.1 初始 count expect 1") end
if MB.GetActiveInstance() ~= 0 then fail("MI.1 初始 active expect 0") end
pass("MI.1 初始 instance count=1, active=0")

-- MI.2 Create x3 + 槽满
local mid1, mid2, mid3 = MB.CreateInstance(), MB.CreateInstance(), MB.CreateInstance()
if mid1 ~= 1 or mid2 ~= 2 or mid3 ~= 3 then
    fail("MI.2 Create x3 expect 1/2/3, got " .. tostring(mid1) .. "/" .. tostring(mid2) .. "/" .. tostring(mid3))
end
if MB.GetInstanceCount() ~= 4 then fail("MI.2 count expect 4") end
if MB.CreateInstance() ~= 0 then fail("MI.2 第 4 次 Create expect 0") end
pass("MI.2 Create x3 + 第 4 次 returns 0 (槽满)")

-- MI.3 SetActiveInstance round-trip
if not MB.SetActiveInstance(mid2) then fail("MI.3 SetActive(2) failed") end
if MB.GetActiveInstance() ~= 2 then fail("MI.3 GetActive expect 2") end
MB.SetActiveInstance(0)
pass("MI.3 SetActiveInstance round-trip (0 <-> 2)")

-- MI.4 Per-instance 参数隔离 (strength)
MB.SetActiveInstance(0); MB.SetStrength(0.5)
MB.SetActiveInstance(mid1); MB.SetStrength(2.5)
MB.SetActiveInstance(0)
if math.abs(MB.GetStrength() - 0.5) > 1e-4 then
    fail("MI.4 instance 0 strength expect 0.5, got " .. MB.GetStrength())
end
MB.SetActiveInstance(mid1)
if math.abs(MB.GetStrength() - 2.5) > 1e-4 then
    fail("MI.4 instance 1 strength expect 2.5, got " .. MB.GetStrength())
end
MB.SetActiveInstance(0)
pass("MI.4 Per-instance 参数隔离 (strength 0=0.5, 1=2.5)")

-- MI.5 DestroyInstance(0) 拒绝 + SetActiveInstance(无效) 拒绝
if MB.DestroyInstance(0) ~= false then fail("MI.5 Destroy(0) should reject") end
if MB.SetActiveInstance(99) ~= false then fail("MI.5 SetActive(99) should reject") end
pass("MI.5 DestroyInstance(0) + SetActiveInstance(无效) 双拒绝")

-- MI.6 清理
MB.DestroyInstance(mid1); MB.DestroyInstance(mid2); MB.DestroyInstance(mid3)
if MB.GetInstanceCount() ~= 1 then fail("MI.6 cleanup count expect 1") end
pass("MI.6 Destroy 全部 user instance, count=1")

-- ============================================================
-- Phase F.0.10.9.x.3 — Clone + GetState
-- ============================================================

MB.SetActiveInstance(0)

-- CS.1 GetState 字段完整
local s0 = MB.GetState()
if type(s0) ~= "table" then fail("CS.1 GetState should return table") end
local req = { "strength", "sample_count", "mode", "half_res",
              "auto_enable", "enabled", "supported" }
for _, k in ipairs(req) do
    if s0[k] == nil then fail("CS.1 GetState missing field: " .. k) end
end
pass("CS.1 GetState 字段完整 (" .. #req .. " 字段)")

-- CS.2 Clone(0) 复制 default profile
MB.SetActiveInstance(0)
MB.SetStrength(1.5); MB.SetSampleCount(12); MB.SetMode(1); MB.SetHalfRes(true)
local cid = MB.CloneInstance(0)
if cid <= 0 or cid > 3 then fail("CS.2 Clone(0) expect [1,3], got " .. cid) end
MB.SetActiveInstance(cid)
local sc = MB.GetState()
if math.abs(sc.strength - 1.5) > 1e-4 then fail("CS.2 cloned strength expect 1.5, got " .. sc.strength) end
if sc.sample_count       ~= 12   then fail("CS.2 cloned sample_count expect 12, got "  .. sc.sample_count) end
if sc.mode               ~= 1    then fail("CS.2 cloned mode expect 1, got " .. sc.mode) end
if sc.half_res           ~= true then fail("CS.2 cloned half_res expect true") end
if sc.enabled            ~= false then fail("CS.2 cloned instance expect enabled=false") end
pass("CS.2 Clone(0) 复制调参 + enabled=false")

-- CS.3 Per-instance 隔离
MB.SetActiveInstance(cid); MB.SetStrength(3.0)
MB.SetActiveInstance(0)
if math.abs(MB.GetStrength() - 1.5) > 1e-4 then
    fail("CS.3 default strength 被污染, expect 1.5, got " .. MB.GetStrength())
end
pass("CS.3 Per-instance 修改隔离")

-- CS.4 Clone 非法
if MB.CloneInstance(99) ~= 0 then fail("CS.4 Clone(99) expect 0") end
pass("CS.4 Clone(无效 srcId) 返 0")

-- CS.5 槽满
local e1 = MB.CloneInstance(0); local e2 = MB.CloneInstance(0)
if e1 == 0 or e2 == 0 then fail("CS.5 setup: Clone x2 should succeed") end
if MB.CloneInstance(0) ~= 0 then fail("CS.5 第 4 次 Clone expect 0 (槽满)") end
pass("CS.5 Clone on full slots returns 0")

-- 清理
MB.DestroyInstance(cid); MB.DestroyInstance(e1); MB.DestroyInstance(e2)

-- ============================================================
-- Phase F.0.10.9.x.4 — SetState (反向 GetState)
-- ============================================================
MB.SetActiveInstance(0)
local mb_before_setstate = MB.GetState()

-- SS.1 round-trip: 改 4 字段 (mode int 0..2, half_res bool)
do
    local ok, n = MB.SetState({ strength = 0.7, sample_count = 12, mode = 1, half_res = true })
    if not ok or n ~= 4 then fail("SS.1 expect ok+applied=4, got " .. tostring(n)) end
    local s = MB.GetState()
    if math.abs(s.strength - 0.7) > 1e-4 then fail("SS.1 strength not applied") end
    if s.sample_count ~= 12              then fail("SS.1 sample_count not applied") end
    if s.mode ~= 1                       then fail("SS.1 mode not applied") end
    if s.half_res ~= true                then fail("SS.1 half_res not applied") end
    pass("SS.1 SetState round-trip 4 字段")
end

-- SS.2 partial + invalid type
do
    local _, n = MB.SetState({ strength = 0.4, sample_count = "bad" })
    if n ~= 1 then fail("SS.2 expect applied=1, got " .. tostring(n)) end
    pass("SS.2 SetState 类型错误 silent skip")
end

-- SS.3 入参非 table
do
    local r, e = MB.SetState({})
    -- 空 table: applied=0, ok=false (因为 applied=0 → ok=false)
    if r ~= false then fail("SS.3 SetState({}) expect ok=false (no field applied)") end
    pass("SS.3 SetState 空 table → ok=false applied=0")
end

MB.SetState(mb_before_setstate)

-- ============================================================
-- Final summary
-- ============================================================

print("")
print("=== Light.Graphics.MotionBlur smoke OK (Phase E.15+E.16+E.17+F.0.10.3+F.0.10.9.x.2+F.0.10.9.x.3+F.0.10.9.x.4) ===")
