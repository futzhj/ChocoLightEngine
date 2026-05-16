-- Phase E.3 smoke: Light.Graphics.HDR (HDR + ACES tonemapping surface)
--
-- API coverage:
--   Enable / Disable / IsEnabled / IsSupported / Resize
--   SetExposure / GetExposure / SetGamma / GetGamma
--   GetSceneTexture
--
-- Headless guard: no GL context at smoke stage, Enable() MUST either
--   (a) return false cleanly (typical) OR
--   (b) return true if the host already has a GL ctx (some runners).
-- In case (a) subsequent Disable/IsEnabled must still work (idempotency).
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local HDR = Graphics.HDR
if type(HDR) ~= "table" then
    fail("Light.Graphics.HDR missing or not a table (got " .. type(HDR) .. ")")
end
pass("Light.Graphics.HDR subtable present")

-- ============================================================
-- 1) Module surface: 10 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetExposure", "GetExposure", "SetGamma", "GetGamma",
    "GetSceneTexture",
    -- Phase E.3.4
    "SetTonemapper", "GetTonemapper",
    -- Phase E.14 — Velocity dilation + format
    "SetVelocityDilation", "GetVelocityDilation",
    "SetVelocityFormat",   "GetVelocityFormat",
    -- Phase E.18.1 — dilation pass 半分辨率
    "SetVelocityDilationHalfRes", "GetVelocityDilationHalfRes",
    -- Phase E.18.2 — dilation pass 自动跳过单消费者
    "SetVelocityDilationAutoSkip", "GetVelocityDilationAutoSkip",
    -- Phase F.0.10.2 — Auto-TAA 开关 (split-screen 多 instance 必备)
    "SetAutoTAA", "GetAutoTAA",
    -- Phase F.0.10.3 — Auto-Bloom/SSR/MotionBlur 开关 (split-screen 多 player 必备)
    "SetAutoBloom", "GetAutoBloom",
    "SetAutoSSR", "GetAutoSSR",
    "SetAutoMotionBlur", "GetAutoMotionBlur",
    -- Phase F.0.10.6 — Auto-Tonemap + per-region Tonemap (split-screen multi-instance 必备)
    "SetAutoTonemap", "GetAutoTonemap", "Tonemap",
    -- Phase F.0.10.8 — 3D LUT (Color Grading)
    "CreateLUT3D", "DeleteLUT3D",
    "SetGradingLUT", "GetGradingLUTId", "GetGradingLUTStrength",
    -- Phase F.0.10.8.1 — .cube LUT 文件解析 (Adobe Cube LUT 1.0)
    "LoadCubeLUT",
    -- Phase F.0.10.8.2 — HALD CLUT 图像 LUT (PNG/JPG/BMP/TGA)
    "LoadHaldLUT",
    -- Phase F.0.10.8.3 — LUT 热重载 (mtime polling)
    "WatchLUT", "UnwatchLUT", "GetWatchedLUTId",
    "PollLUTReloads", "SetLUTHotReload", "GetLUTHotReload",
}
for _, k in ipairs(fn_names) do
    if type(HDR[k]) ~= "function" then
        fail("Light.Graphics.HDR." .. k .. " missing or not a function (got " .. type(HDR[k]) .. ")")
    end
end
pass("Light.Graphics.HDR module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) IsSupported / IsEnabled — initial state probes
-- ============================================================

local is_sup = HDR.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = HDR.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("IsEnabled() = false initially")

-- ============================================================
-- 3) Exposure / Gamma getter/setter round-trip
-- ============================================================

local function approx(a, b) return math.abs(a - b) < 1e-4 end

local exp0 = HDR.GetExposure()
if type(exp0) ~= "number" then
    fail("GetExposure should return number, got " .. type(exp0))
end
if not approx(exp0, 1.0) then
    fail("Initial exposure must be 1.0, got " .. tostring(exp0))
end
pass("GetExposure() default = 1.0")

HDR.SetExposure(2.5)
if not approx(HDR.GetExposure(), 2.5) then
    fail("SetExposure(2.5) did not stick: " .. tostring(HDR.GetExposure()))
end
HDR.SetExposure(0.5)
if not approx(HDR.GetExposure(), 0.5) then
    fail("SetExposure(0.5) did not stick: " .. tostring(HDR.GetExposure()))
end
HDR.SetExposure(1.0)  -- restore
pass("SetExposure / GetExposure round-trip ok")

local gam0 = HDR.GetGamma()
if type(gam0) ~= "number" then
    fail("GetGamma should return number, got " .. type(gam0))
end
if not approx(gam0, 2.2) then
    fail("Initial gamma must be 2.2, got " .. tostring(gam0))
end
pass("GetGamma() default = 2.2")

HDR.SetGamma(1.8)
if not approx(HDR.GetGamma(), 1.8) then
    fail("SetGamma(1.8) did not stick: " .. tostring(HDR.GetGamma()))
end
HDR.SetGamma(2.4)
if not approx(HDR.GetGamma(), 2.4) then
    fail("SetGamma(2.4) did not stick: " .. tostring(HDR.GetGamma()))
end
HDR.SetGamma(2.2)  -- restore
pass("SetGamma / GetGamma round-trip ok")

-- ============================================================
-- 4) GetSceneTexture — 0 when not enabled
-- ============================================================

local tex0 = HDR.GetSceneTexture()
if type(tex0) ~= "number" then
    fail("GetSceneTexture should return number/integer, got " .. type(tex0))
end
if tex0 ~= 0 then
    fail("GetSceneTexture() must be 0 when HDR disabled, got " .. tostring(tex0))
end
pass("GetSceneTexture() = 0 when disabled")

-- ============================================================
-- 5) Enable / Disable — headless robustness
-- ============================================================
--
-- Smoke runs with no GL context (light.exe invokes main.lua before Window.Open).
-- Enable(w, h) must not crash; it is allowed to return false.
-- If it does return true (some host already opened a GL ctx), Disable must
-- also work without errors.

local ok_en, ret_en = pcall(HDR.Enable, 256, 256)
if not ok_en then
    fail("HDR.Enable(256, 256) raised error: " .. tostring(ret_en))
end
if type(ret_en) ~= "boolean" then
    fail("HDR.Enable should return boolean, got " .. type(ret_en))
end
pass("HDR.Enable(256, 256) returned " .. tostring(ret_en) .. " cleanly (no throw)")

-- Enabled flag must match Enable() return value
if HDR.IsEnabled() ~= ret_en then
    fail("IsEnabled() = " .. tostring(HDR.IsEnabled()) .. " but Enable returned " .. tostring(ret_en))
end
pass("IsEnabled() tracks Enable() result")

-- Disable must always be callable (no-op if never enabled)
local ok_dis, err_dis = pcall(HDR.Disable)
if not ok_dis then
    fail("HDR.Disable() raised error: " .. tostring(err_dis))
end
if HDR.IsEnabled() ~= false then
    fail("IsEnabled() must be false after Disable(), got " .. tostring(HDR.IsEnabled()))
end
pass("HDR.Disable() always safe; IsEnabled() = false after")

-- Double Disable: idempotent
local ok_dis2 = pcall(HDR.Disable)
if not ok_dis2 then fail("double Disable() raised error") end
pass("Double Disable() idempotent")

-- Resize on disabled: must not crash, must return false (can't resize what doesn't exist)
local ok_rs, ret_rs = pcall(HDR.Resize, 512, 512)
if not ok_rs then
    fail("HDR.Resize(512, 512) on disabled state raised error: " .. tostring(ret_rs))
end
if type(ret_rs) ~= "boolean" then
    fail("HDR.Resize should return boolean, got " .. type(ret_rs))
end
pass("HDR.Resize on disabled returned " .. tostring(ret_rs) .. " cleanly")

-- ============================================================
-- 6) Bad params — argument error or false
-- ============================================================

-- Invalid size: 0 or negative. Current impl returns false cleanly.
local _, r1 = pcall(HDR.Enable, 0, 0)
if r1 == true then fail("Enable(0, 0) must not succeed") end
local _, r2 = pcall(HDR.Enable, -1, 256)
if r2 == true then fail("Enable(-1, 256) must not succeed") end
pass("Enable bad params rejected (w<=0 or h<=0)")

-- GetSceneTexture after failed Enable: still 0
if HDR.GetSceneTexture() ~= 0 then
    fail("GetSceneTexture() must be 0 after failed Enable, got " .. tostring(HDR.GetSceneTexture()))
end
pass("GetSceneTexture() stays 0 after failed Enable")

-- ============================================================
-- 7) Phase E.3.4 — Tonemapper operator (按字符串名)
-- ============================================================

-- 7.1 默认 operator = "aces"
local tm0 = HDR.GetTonemapper()
if type(tm0) ~= "string" then
    fail("GetTonemapper should return string, got " .. type(tm0))
end
if tm0 ~= "aces" then
    fail("Default tonemapper must be 'aces', got '" .. tostring(tm0) .. "'")
end
pass("GetTonemapper() default = 'aces'")

-- 7.2 4 个合法 operator 往返
local valid = { "aces", "reinhard", "uncharted2", "linear" }
for _, name in ipairs(valid) do
    HDR.SetTonemapper(name)
    local got = HDR.GetTonemapper()
    if got ~= name then
        fail("SetTonemapper('" .. name .. "') did not stick: got '" .. tostring(got) .. "'")
    end
end
pass("SetTonemapper / GetTonemapper round-trip ok (4 operators)")

-- 7.3 大小写无关
HDR.SetTonemapper("ACES")
if HDR.GetTonemapper() ~= "aces" then
    fail("Case-insensitive 'ACES' failed: got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
HDR.SetTonemapper("Reinhard")
if HDR.GetTonemapper() ~= "reinhard" then
    fail("Case-insensitive 'Reinhard' failed: got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
HDR.SetTonemapper("UnCharTeD2")
if HDR.GetTonemapper() ~= "uncharted2" then
    fail("Case-insensitive 'UnCharTeD2' failed: got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
pass("Case-insensitive SetTonemapper ok (ACES / Reinhard / UnCharTeD2)")

-- 7.4 无效名回退 ACES
HDR.SetTonemapper("not_a_real_operator")
if HDR.GetTonemapper() ~= "aces" then
    fail("Unknown name should fallback to 'aces', got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
HDR.SetTonemapper("")
if HDR.GetTonemapper() ~= "aces" then
    fail("Empty name should fallback to 'aces', got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
pass("Unknown / empty tonemapper name falls back to 'aces'")

-- 7.5 SetTonemapper 不抛 (bad-type arg 由 luaL_checkstring 处理, 抛 lua error)
local ok_bad = pcall(HDR.SetTonemapper)   -- 缺参数
if ok_bad then fail("SetTonemapper() with no arg must error") end
local ok_bad2 = pcall(HDR.SetTonemapper, 123)  -- 数字 (lua 自动转字符串 -> 未知 -> aces)
if not ok_bad2 then fail("SetTonemapper(123) lua-auto-stringified should not error") end
pass("SetTonemapper arg validation ok (nil arg errors; number stringified)")

-- 恢复默认
HDR.SetTonemapper("aces")

-- ============================================================
-- 8) Phase E.14 — velocity dilation + format
-- ============================================================
--
-- API: SetVelocityDilation(bool), GetVelocityDilation(), SetVelocityFormat(str), GetVelocityFormat()
-- Headless: smoke 运行时 HDR 未 Enable，但 API 设计接受未 Enable 状态
--   - GetVelocityDilation 返默认 true
--   - GetVelocityFormat 返默认 "rg16f"
--   - SetVelocityFormat 仅更新 state，下次 Enable 生效

-- 8.1 默认状态
local vd0 = HDR.GetVelocityDilation()
if type(vd0) ~= "boolean" then
    fail("GetVelocityDilation should return boolean, got " .. type(vd0))
end
if vd0 ~= true then
    fail("GetVelocityDilation() default must be true, got " .. tostring(vd0))
end
pass("GetVelocityDilation() default = true")

local vf0 = HDR.GetVelocityFormat()
if type(vf0) ~= "string" then
    fail("GetVelocityFormat should return string, got " .. type(vf0))
end
if vf0 ~= "rg16f" then
    fail("GetVelocityFormat() default must be 'rg16f', got '" .. tostring(vf0) .. "'")
end
pass("GetVelocityFormat() default = 'rg16f'")

-- 8.2 SetVelocityDilation round-trip
local ok_sd, err_sd = HDR.SetVelocityDilation(false)
if HDR.GetVelocityDilation() ~= false then
    fail("SetVelocityDilation(false) round-trip failed")
end
HDR.SetVelocityDilation(true)
if HDR.GetVelocityDilation() ~= true then
    fail("SetVelocityDilation(true) round-trip failed")
end
pass("SetVelocityDilation true/false round-trip ok")

-- 8.3 SetVelocityDilation bad arg → nil + err
local ok_bd, err_bd = HDR.SetVelocityDilation("yes")
if ok_bd ~= nil then
    fail("SetVelocityDilation('yes') should return nil, got " .. tostring(ok_bd))
end
if type(err_bd) ~= "string" then
    fail("SetVelocityDilation bad-arg err must be string, got " .. type(err_bd))
end
pass("SetVelocityDilation bad-arg returns nil + err string")

-- 8.4 SetVelocityFormat round-trip (未 Enable，仅更新 state)
-- 需求: 切到 rg8 后 GetVelocityFormat == "rg8"。后端未 Enable 时切换仅更新下次 RT 创建参数。
HDR.SetVelocityFormat("rg8")
if HDR.GetVelocityFormat() ~= "rg8" then
    fail("SetVelocityFormat('rg8') round-trip failed: got '" .. tostring(HDR.GetVelocityFormat()) .. "'")
end
HDR.SetVelocityFormat("rg16f")
if HDR.GetVelocityFormat() ~= "rg16f" then
    fail("SetVelocityFormat('rg16f') round-trip failed")
end
pass("SetVelocityFormat rg8/rg16f round-trip ok")

-- 8.5 SetVelocityFormat 未知名 → nil + err (不仔大小写敏感)
local ok_bf, err_bf = HDR.SetVelocityFormat("rg32f")
if ok_bf ~= nil then
    fail("SetVelocityFormat('rg32f') should return nil, got " .. tostring(ok_bf))
end
if type(err_bf) ~= "string" then
    fail("SetVelocityFormat bad-name err must be string, got " .. type(err_bf))
end
pass("SetVelocityFormat unknown name returns nil + err string")

-- 8.6 大写 "RG8" 应拒绝 (严格大小写敏感)
local ok_bf2 = HDR.SetVelocityFormat("RG8")
if ok_bf2 ~= nil then
    fail("SetVelocityFormat('RG8') should reject uppercase, got " .. tostring(ok_bf2))
end
if HDR.GetVelocityFormat() ~= "rg16f" then
    fail("After rejected 'RG8' format must remain 'rg16f'")
end
pass("SetVelocityFormat case-sensitive ('RG8' rejected)")

-- ============================================================
-- 9) Phase E.18.1 — dilation pass half-resolution
-- ============================================================
--
-- API: SetVelocityDilationHalfRes(bool), GetVelocityDilationHalfRes()
-- Headless: smoke 运行时 HDR 未 Enable，但 API 接受未 Enable 状态
--   - GetVelocityDilationHalfRes 返默认 false (兼容 Phase E.18 行为)
--   - SetVelocityDilationHalfRes 仅更新 state，下次 Enable 时影响 dilated RT 尺寸

-- 9.1 默认状态
local dhr0 = HDR.GetVelocityDilationHalfRes()
if type(dhr0) ~= "boolean" then
    fail("GetVelocityDilationHalfRes should return boolean, got " .. type(dhr0))
end
if dhr0 ~= false then
    fail("GetVelocityDilationHalfRes() default must be false, got " .. tostring(dhr0))
end
pass("GetVelocityDilationHalfRes() default = false")

-- 9.2 SetVelocityDilationHalfRes round-trip
HDR.SetVelocityDilationHalfRes(true)
if HDR.GetVelocityDilationHalfRes() ~= true then
    fail("SetVelocityDilationHalfRes(true) round-trip failed")
end
HDR.SetVelocityDilationHalfRes(false)
if HDR.GetVelocityDilationHalfRes() ~= false then
    fail("SetVelocityDilationHalfRes(false) round-trip failed")
end
pass("SetVelocityDilationHalfRes true/false round-trip ok")

-- 9.3 SetVelocityDilationHalfRes bad arg → nil + err
local ok_bdhr, err_bdhr = HDR.SetVelocityDilationHalfRes("yes")
if ok_bdhr ~= nil then
    fail("SetVelocityDilationHalfRes('yes') should return nil, got " .. tostring(ok_bdhr))
end
if type(err_bdhr) ~= "string" then
    fail("SetVelocityDilationHalfRes bad-arg err must be string, got " .. type(err_bdhr))
end
pass("SetVelocityDilationHalfRes bad-arg returns nil + err string")

-- 9.4 no-op 同值不报错
HDR.SetVelocityDilationHalfRes(false)
HDR.SetVelocityDilationHalfRes(false)
if HDR.GetVelocityDilationHalfRes() ~= false then
    fail("SetVelocityDilationHalfRes(false) twice should remain false")
end
pass("SetVelocityDilationHalfRes idempotent (no-op same value)")

-- ============================================================
-- 10) Phase E.18.2 — dilation pass auto-skip single consumer
-- ============================================================
--
-- API: SetVelocityDilationAutoSkip(bool), GetVelocityDilationAutoSkip()
-- 默认 false (Phase E.18.1 行为, 始终运行 dilation pass)
-- autoSkip=true 时仅 "SSR Temporal 单消费者 + MB 未启" 场景自动跳过 dilation pass
-- 受益: 单 SSR 场景省 1 fetch/px; 其他场景 (仅 MB / SSR+MB / 都不启) 不跳过

-- 10.1 默认状态
local das0 = HDR.GetVelocityDilationAutoSkip()
if type(das0) ~= "boolean" then
    fail("GetVelocityDilationAutoSkip should return boolean, got " .. type(das0))
end
if das0 ~= false then
    fail("GetVelocityDilationAutoSkip() default must be false, got " .. tostring(das0))
end
pass("GetVelocityDilationAutoSkip() default = false")

-- 10.2 SetVelocityDilationAutoSkip round-trip
HDR.SetVelocityDilationAutoSkip(true)
if HDR.GetVelocityDilationAutoSkip() ~= true then
    fail("SetVelocityDilationAutoSkip(true) round-trip failed")
end
HDR.SetVelocityDilationAutoSkip(false)
if HDR.GetVelocityDilationAutoSkip() ~= false then
    fail("SetVelocityDilationAutoSkip(false) round-trip failed")
end
pass("SetVelocityDilationAutoSkip true/false round-trip ok")

-- 10.3 SetVelocityDilationAutoSkip bad arg → nil + err
local ok_bdas, err_bdas = HDR.SetVelocityDilationAutoSkip("yes")
if ok_bdas ~= nil then
    fail("SetVelocityDilationAutoSkip('yes') should return nil, got " .. tostring(ok_bdas))
end
if type(err_bdas) ~= "string" then
    fail("SetVelocityDilationAutoSkip bad-arg err must be string, got " .. type(err_bdas))
end
pass("SetVelocityDilationAutoSkip bad-arg returns nil + err string")

-- 10.4 no-op 同值不报错
HDR.SetVelocityDilationAutoSkip(false)
HDR.SetVelocityDilationAutoSkip(false)
if HDR.GetVelocityDilationAutoSkip() ~= false then
    fail("SetVelocityDilationAutoSkip(false) twice should remain false")
end
pass("SetVelocityDilationAutoSkip idempotent (no-op same value)")

-- ============================================================
-- 11) Phase F.0.10.2 — SetAutoTAA / GetAutoTAA (split-screen 必备)
-- ============================================================
-- 默认 true (老 EndScene 自动调 TAA.Process), 设 false 后用户手动 TAA.Process 控时序

-- 11.1 GetAutoTAA() 默认 = true
if HDR.GetAutoTAA() ~= true then
    fail("GetAutoTAA() default must be true (零回归), got " .. tostring(HDR.GetAutoTAA()))
end
pass("GetAutoTAA() default = true (零回归)")

-- 11.2 SetAutoTAA round-trip
HDR.SetAutoTAA(false)
if HDR.GetAutoTAA() ~= false then
    fail("SetAutoTAA(false) round-trip failed, got " .. tostring(HDR.GetAutoTAA()))
end
HDR.SetAutoTAA(true)
if HDR.GetAutoTAA() ~= true then
    fail("SetAutoTAA(true) round-trip failed, got " .. tostring(HDR.GetAutoTAA()))
end
pass("SetAutoTAA true/false round-trip ok")

-- 11.3 SetAutoTAA bad arg → nil + err
local ok_at, err_at = HDR.SetAutoTAA("yes")
if ok_at ~= nil then fail("SetAutoTAA('yes') should return nil, got " .. tostring(ok_at)) end
if type(err_at) ~= "string" then fail("SetAutoTAA bad-arg err must be string, got " .. type(err_at)) end
pass("SetAutoTAA bad-arg returns nil + err string")

-- 11.4 no-op 同值
HDR.SetAutoTAA(true)
HDR.SetAutoTAA(true)
if HDR.GetAutoTAA() ~= true then
    fail("SetAutoTAA(true) twice should remain true")
end
pass("SetAutoTAA idempotent (no-op same value)")

-- 11.5 恢复默认 (后续 demo 不破坏)
HDR.SetAutoTAA(true)

-- ============================================================
-- 12) Phase F.0.10.3 — SetAutoBloom / SetAutoSSR / SetAutoMotionBlur (split-screen 必备)
-- ============================================================
-- 3 对开关同模式 (与 SetAutoTAA 一致): 默认 true, round-trip, bad-arg, idempotent
-- 关 split-screen 多 player 时必关这 3 个让用户手动 .Process(rgn)

local auto_pairs = {
    {name = "Bloom",      set = HDR.SetAutoBloom,      get = HDR.GetAutoBloom},
    {name = "SSR",        set = HDR.SetAutoSSR,        get = HDR.GetAutoSSR},
    {name = "MotionBlur", set = HDR.SetAutoMotionBlur, get = HDR.GetAutoMotionBlur},
}
for _, p in ipairs(auto_pairs) do
    -- 12.1 默认 true (零回归)
    if p.get() ~= true then
        fail("HDR.GetAuto" .. p.name .. "() default should be true, got " .. tostring(p.get()))
    end
    pass("HDR.GetAuto" .. p.name .. "() default = true (零回归)")

    -- 12.2 round-trip true → false → true
    p.set(false)
    if p.get() ~= false then
        fail("HDR.SetAuto" .. p.name .. "(false) round-trip failed")
    end
    p.set(true)
    if p.get() ~= true then
        fail("HDR.SetAuto" .. p.name .. "(true) round-trip failed")
    end
    pass("HDR.SetAuto" .. p.name .. " true/false round-trip ok")

    -- 12.3 bad-arg → nil + err
    local r, e = p.set("yes")
    if r ~= nil then fail("HDR.SetAuto" .. p.name .. "('yes') should return nil") end
    if type(e) ~= "string" then fail("HDR.SetAuto" .. p.name .. " bad-arg err must be string") end
    pass("HDR.SetAuto" .. p.name .. " bad-arg returns nil + err string")

    -- 12.4 idempotent no-op 同值
    p.set(true); p.set(true)
    if p.get() ~= true then fail("HDR.SetAuto" .. p.name .. "(true) twice should remain true") end
    pass("HDR.SetAuto" .. p.name .. " idempotent (no-op same value)")

    -- 复位 (后续 demo 不破坏)
    p.set(true)
end

-- ============================================================
-- 13) Phase F.0.10.6 — SetAutoTonemap / Tonemap (split-screen multi-instance 必备)
-- ============================================================
-- 与 SetAutoTAA / SetAutoBloom 同模式: 默认 true (零回归), round-trip, bad-arg, idempotent
-- 额外验证 Tonemap headless 退化 (HDR 未启用时返回 nil + err string, 与 Bloom.Process 同)

-- 13.1 默认 true (零回归)
if HDR.GetAutoTonemap() ~= true then
    fail("HDR.GetAutoTonemap() default should be true, got " .. tostring(HDR.GetAutoTonemap()))
end
pass("HDR.GetAutoTonemap() default = true (零回归)")

-- 13.2 round-trip true → false → true
HDR.SetAutoTonemap(false)
if HDR.GetAutoTonemap() ~= false then
    fail("HDR.SetAutoTonemap(false) round-trip failed")
end
HDR.SetAutoTonemap(true)
if HDR.GetAutoTonemap() ~= true then
    fail("HDR.SetAutoTonemap(true) round-trip failed")
end
pass("HDR.SetAutoTonemap true/false round-trip ok")

-- 13.3 bad-arg → nil + err
local r_tm, e_tm = HDR.SetAutoTonemap("yes")
if r_tm ~= nil then fail("HDR.SetAutoTonemap('yes') should return nil") end
if type(e_tm) ~= "string" then fail("HDR.SetAutoTonemap bad-arg err must be string") end
pass("HDR.SetAutoTonemap bad-arg returns nil + err string")

-- 13.4 idempotent no-op 同值
HDR.SetAutoTonemap(true); HDR.SetAutoTonemap(true)
if HDR.GetAutoTonemap() ~= true then fail("HDR.SetAutoTonemap(true) twice should remain true") end
pass("HDR.SetAutoTonemap idempotent (no-op same value)")

-- 13.5 Tonemap headless 退化 (HDR 未 Enable 时 返 nil + err)
-- 注: 本 smoke 不起窗口, HDR.IsEnabled 应为 false; Tonemap 必返 nil + err
if not HDR.IsEnabled() then
    local ok_tm, err_tm = HDR.Tonemap(0, 0, 100, 100)
    if ok_tm ~= nil then
        fail("HDR.Tonemap headless should return nil, got " .. tostring(ok_tm))
    end
    if type(err_tm) ~= "string" then
        fail("HDR.Tonemap headless err must be string, got " .. type(err_tm))
    end
    pass("HDR.Tonemap(rgn) headless returns nil + err: " .. err_tm)

    -- 13.6 验证 Tonemap 接受 params_table (headless 仍可返 nil + err, 但不能崩)
    local ok_p, err_p = HDR.Tonemap(0, 0, 100, 100, {
        exposure = 1.5, gamma = 2.4, tonemap = "uncharted2",
    })
    if ok_p ~= nil then
        fail("HDR.Tonemap(rgn, params) headless should return nil")
    end
    if type(err_p) ~= "string" then
        fail("HDR.Tonemap(rgn, params) headless err must be string")
    end
    pass("HDR.Tonemap(rgn, params={...}) headless returns nil + err")

    -- 13.7 验证 params.tonemap 接受 int (各合法值 headless 都不崩)
    for _, mode_int in ipairs({0, 1, 2, 3}) do
        local _, e = HDR.Tonemap(0, 0, 100, 100, {tonemap = mode_int})
        if type(e) ~= "string" then
            fail("HDR.Tonemap(rgn, {tonemap=" .. mode_int .. "}) headless err must be string")
        end
    end
    pass("HDR.Tonemap(rgn, {tonemap=0..3 int}) all 4 modes accepted (headless)")
end

-- 复位 (后续 demo 不破坏)
HDR.SetAutoTonemap(true)

-- ============================================================
-- 14. Phase F.0.10.8 — 3D LUT (Color Grading)
-- ============================================================

-- 14.1 CreateLUT3D size 越界 (< 4) 拒绝
--      data 使用合法长度 (3*3*3*3=81 bytes), 但 size=3 本身越界
local small = string.rep("\0", 3 * 3 * 3 * 3)
local r1, e1 = HDR.CreateLUT3D(3, small)
if r1 ~= nil then
    fail("CreateLUT3D(size=3) should be rejected")
end
if type(e1) ~= "string" then
    fail("CreateLUT3D(size=3) err must be string")
end
pass("CreateLUT3D(size=3) rejected: " .. e1)

-- 14.2 CreateLUT3D size 越界 (> 64) 拒绝
local big = string.rep("\0", 65 * 65 * 65 * 3)
local r2, e2 = HDR.CreateLUT3D(65, big)
if r2 ~= nil then
    fail("CreateLUT3D(size=65) should be rejected")
end
pass("CreateLUT3D(size=65) rejected: " .. (e2 or "?"))

-- 14.3 CreateLUT3D data 长度错 拒绝
local bad_data = string.rep("\0", 100)  -- 100 != 16*16*16*3 = 12288
local r3, e3 = HDR.CreateLUT3D(16, bad_data)
if r3 ~= nil then
    fail("CreateLUT3D(size=16, len=100) should be rejected")
end
pass("CreateLUT3D(data len mismatch) rejected: " .. (e3 or "?"))

-- 14.4 CreateLUT3D headless 路径 (HDR 未 Enable 但 backend 存在)
--      合法 size + data, headless GL context 可能返 0 (backend create 失败) 或 成功返 id
--      smoke 涧仅验证 未崩 + 返值类型正确
local identity_data = string.rep("\0", 16 * 16 * 16 * 3)
local r4, e4 = HDR.CreateLUT3D(16, identity_data)
if r4 ~= nil and type(r4) ~= "number" then
    fail("CreateLUT3D(16, valid) ret type: expected number or nil, got " .. type(r4))
end
pass("CreateLUT3D(16, valid_data) headless ok (id=" .. tostring(r4) .. ", err=" .. tostring(e4) .. ")")
-- 成功创建后需 Delete 防泄露
if type(r4) == "number" and r4 > 0 then
    local d_ok = HDR.DeleteLUT3D(r4)
    if d_ok ~= true then
        fail("DeleteLUT3D(valid id) should return true")
    end
    pass("DeleteLUT3D(valid id) round-trip ok")
end

-- 14.5 DeleteLUT3D(0) silent fail (不崩)
if HDR.DeleteLUT3D(0) ~= false then
    fail("DeleteLUT3D(0) should return false")
end
pass("DeleteLUT3D(0) returns false (silent)")

-- 14.6 SetGradingLUT(0, 0.0) round-trip (默认状态)
if HDR.SetGradingLUT(0, 0.0) ~= true then
    fail("SetGradingLUT(0, 0.0) failed")
end
if HDR.GetGradingLUTId() ~= 0 then
    fail("GetGradingLUTId(0) round-trip failed")
end
if HDR.GetGradingLUTStrength() ~= 0.0 then
    fail("GetGradingLUTStrength(0) round-trip failed")
end
pass("SetGradingLUT(0, 0.0) round-trip ok")

-- 14.7 SetGradingLUT(123, 0.7) round-trip
if HDR.SetGradingLUT(123, 0.7) ~= true then
    fail("SetGradingLUT(123, 0.7) failed")
end
if HDR.GetGradingLUTId() ~= 123 then
    fail("GetGradingLUTId(123) round-trip failed")
end
if math.abs(HDR.GetGradingLUTStrength() - 0.7) > 1e-4 then
    fail("GetGradingLUTStrength(0.7) round-trip failed")
end
pass("SetGradingLUT(123, 0.7) round-trip ok")

-- 14.8 SetGradingLUT strength clamp [0, 1]
HDR.SetGradingLUT(456, -1.0)
if HDR.GetGradingLUTStrength() ~= 0.0 then
    fail("strength=-1 should clamp to 0")
end
HDR.SetGradingLUT(456, 2.5)
if HDR.GetGradingLUTStrength() ~= 1.0 then
    fail("strength=2.5 should clamp to 1")
end
pass("SetGradingLUT strength clamp [0, 1] ok")

-- 14.9 Tonemap params 接收 lut + lutStrength 字段 (headless 仍返 nil + err, 不崩)
local ok_lp, err_lp = HDR.Tonemap(0, 0, 100, 100, {
    lut = 999, lutStrength = 0.5,
})
if ok_lp ~= nil or type(err_lp) ~= "string" then
    fail("HDR.Tonemap(rgn, {lut, lutStrength}) headless err")
end
pass("HDR.Tonemap(rgn, {lut, lutStrength}) headless returns nil + err")

-- 复位
HDR.SetGradingLUT(0, 0.0)

-- ============================================================
-- 15. Phase F.0.10.8.1 — .cube LUT 文件解析 (Adobe Cube LUT 1.0)
-- ============================================================

-- 工具: 写 tmp .cube 文件 (smoke 用) + 自动清理
-- 使用 require("Light.IOStream"/"Light.Filesystem") 显式加载 (不在全局 Light 表下)
local IO = require("Light.IOStream")
local FS = require("Light.Filesystem")
local tmp_base = FS.GetPrefPath("ChocoLight", "smoke_hdr") or ""  -- 保证可写
local function write_tmp_cube(name, content)
    local path = tmp_base .. "_tmp_smoke_" .. name .. ".cube"
    local ok, err = IO.SaveFile(path, content)
    if not ok then
        fail("write_tmp_cube: SaveFile failed: " .. tostring(err) .. " path=" .. path)
    end
    return path
end
local function cleanup_tmp(path)
    if FS and FS.RemovePath then FS.RemovePath(path) end
end

-- 15.1 LoadCubeLUT 不存在文件
local r1, e1 = HDR.LoadCubeLUT("definitely_not_exist_smoke.cube")
if r1 ~= nil then
    fail("LoadCubeLUT(missing) should return nil")
end
if type(e1) ~= "string" or not e1:find("file read failed") then
    fail("LoadCubeLUT(missing) err must contain 'file read failed', got: " .. tostring(e1))
end
pass("LoadCubeLUT(missing file) returns nil + err: " .. e1)

-- 15.2 LUT_1D_SIZE 文件 → "1D LUT not supported"
local p_1d = write_tmp_cube("1d", "LUT_1D_SIZE 16\n0.0 0.0 0.0\n")
local r2, e2 = HDR.LoadCubeLUT(p_1d)
cleanup_tmp(p_1d)
if r2 ~= nil then fail("LoadCubeLUT(1D) should return nil") end
if not e2:find("1D LUT not supported") then
    fail("LoadCubeLUT(1D) err must contain '1D LUT not supported', got: " .. e2)
end
pass("LoadCubeLUT(LUT_1D_SIZE) rejected: " .. e2)

-- 15.3 缺 LUT_3D_SIZE → "missing LUT_3D_SIZE" 或 "data row before"
local p_no_size = write_tmp_cube("no_size", "TITLE \"foo\"\n0.0 0.0 0.0\n")
local r3, e3 = HDR.LoadCubeLUT(p_no_size)
cleanup_tmp(p_no_size)
if r3 ~= nil then fail("LoadCubeLUT(no SIZE) should return nil") end
if not (e3:find("data row before") or e3:find("missing LUT_3D_SIZE")) then
    fail("LoadCubeLUT(no SIZE) err unexpected: " .. e3)
end
pass("LoadCubeLUT(no LUT_3D_SIZE) rejected: " .. e3)

-- 15.4 LUT_3D_SIZE 越界 (< 4)
local p_small = write_tmp_cube("small", "LUT_3D_SIZE 3\n")
local r4, e4 = HDR.LoadCubeLUT(p_small)
cleanup_tmp(p_small)
if r4 ~= nil then fail("LoadCubeLUT(size=3) should return nil") end
if not e4:find("out of range") then fail("LoadCubeLUT(size=3) err unexpected: " .. e4) end
pass("LoadCubeLUT(size=3) rejected: " .. e4)

-- 15.5 LUT_3D_SIZE 越界 (> 64)
local p_big = write_tmp_cube("big", "LUT_3D_SIZE 65\n")
local r5, e5 = HDR.LoadCubeLUT(p_big)
cleanup_tmp(p_big)
if r5 ~= nil then fail("LoadCubeLUT(size=65) should return nil") end
if not e5:find("out of range") then fail("LoadCubeLUT(size=65) err unexpected: " .. e5) end
pass("LoadCubeLUT(size=65) rejected: " .. e5)

-- 15.6 LUT_3D_SIZE 4 + 数据行不足 (期望 64 行, 给 2 行)
local p_short = write_tmp_cube("short",
    "LUT_3D_SIZE 4\n0.0 0.0 0.0\n0.1 0.1 0.1\n")
local r6, e6 = HDR.LoadCubeLUT(p_short)
cleanup_tmp(p_short)
if r6 ~= nil then fail("LoadCubeLUT(short data) should return nil") end
if not e6:find("data row count") then
    fail("LoadCubeLUT(short data) err unexpected: " .. e6)
end
pass("LoadCubeLUT(data row mismatch) rejected: " .. e6)

-- 15.7 合法 4³ identity LUT (含注释 + 空行 + DOMAIN + TITLE)
-- 注: 4³ = 64 数据行, identity 即 r=R/3, g=G/3, b=B/3
local function make_identity_4_cube()
    local lines = {
        "# 4³ identity LUT for smoke testing",
        "TITLE \"smoke identity\"",
        "DOMAIN_MIN 0.0 0.0 0.0",
        "DOMAIN_MAX 1.0 1.0 1.0",
        "",  -- 空行
        "LUT_3D_SIZE 4",
        "",  -- 空行
    }
    -- B 最慢, G 中, R 最快
    for b = 0, 3 do
        for g = 0, 3 do
            for r = 0, 3 do
                lines[#lines + 1] = string.format("%.6f %.6f %.6f",
                    r / 3.0, g / 3.0, b / 3.0)
            end
        end
    end
    return table.concat(lines, "\n") .. "\n"
end
local p_ok = write_tmp_cube("identity4", make_identity_4_cube())
local r7, e7 = HDR.LoadCubeLUT(p_ok)
cleanup_tmp(p_ok)
-- headless GL context 下 backend->CreateLUT3D 可能成功 (glGenTextures + glTexImage3D 都安全)
-- 或失败 ("backend CreateLUT3D failed"). 仅验证: 未崩 + 返值类型正确
if r7 ~= nil and type(r7) ~= "number" then
    fail("LoadCubeLUT(identity4) ret type: expected number or nil, got " .. type(r7))
end
pass("LoadCubeLUT(identity 4³ + comments + DOMAIN + blank lines) headless ok (id=" ..
     tostring(r7) .. ", err=" .. tostring(e7) .. ")")
if type(r7) == "number" and r7 > 0 then
    if HDR.DeleteLUT3D(r7) ~= true then
        fail("DeleteLUT3D after LoadCubeLUT failed")
    end
    pass("LoadCubeLUT → DeleteLUT3D round-trip ok")
end

-- 15.8 CRLF 行尾兼容
local p_crlf = write_tmp_cube("crlf",
    "LUT_3D_SIZE 4\r\n" .. string.rep("0.0 0.0 0.0\r\n", 64))
local r8, e8 = HDR.LoadCubeLUT(p_crlf)
cleanup_tmp(p_crlf)
if r8 ~= nil and type(r8) ~= "number" then
    fail("LoadCubeLUT(CRLF) ret type unexpected: " .. type(r8))
end
pass("LoadCubeLUT(CRLF line endings) headless ok (id=" .. tostring(r8) ..
     ", err=" .. tostring(e8) .. ")")
if type(r8) == "number" and r8 > 0 then HDR.DeleteLUT3D(r8) end

-- ============================================================
-- 16. Phase F.0.10.8.2 — HALD CLUT 图像 LUT (PNG/JPG/BMP/TGA)
-- ============================================================

-- 工具: BMP 24-bit 编码 helpers (Lua 5.1 兼容, 用 math.floor; 复用 audio.lua 模式)
local function u32_le(v)
    return string.char(v % 256, math.floor(v/256) % 256,
                       math.floor(v/65536) % 256, math.floor(v/16777216) % 256)
end
local function u16_le(v) return string.char(v % 256, math.floor(v/256) % 256) end

-- 构造一个 W×H 24-bit BMP (bottom-up, 全单色像素)
-- BMP 24-bit 文件 layout:
--   14 byte FileHeader + 40 byte DIBHeader(BITMAPINFOHEADER) + W*H*3 bytes BGR pixel data
--   每 row 必须 4-byte 对齐 (row_bytes % 4 == 0); 全单色像素故 top-down/bottom-up 无差
local function make_bmp_solid(W, H, R, G, B)
    local row_bytes  = 3 * W
    local pad        = (4 - (row_bytes % 4)) % 4
    local row_padded = row_bytes + pad
    local img_size   = row_padded * H
    local file_size  = 14 + 40 + img_size

    -- BMP File Header (14 bytes) + DIB Header (40 bytes BITMAPINFOHEADER)
    local hdr = "BM" .. u32_le(file_size) .. u32_le(0) .. u32_le(54)
             .. u32_le(40) .. u32_le(W) .. u32_le(H)
             .. u16_le(1)                 -- planes
             .. u16_le(24)                -- bpp
             .. u32_le(0)                 -- compression BI_RGB
             .. u32_le(img_size)          -- image size
             .. u32_le(0) .. u32_le(0)    -- PPM x/y
             .. u32_le(0) .. u32_le(0)    -- colors used / important
    -- pixel data: BGR 字节流 (row 4-byte align)
    local one_pixel = string.char(B % 256, G % 256, R % 256)
    local one_row   = string.rep(one_pixel, W) .. string.rep("\0", pad)
    local data      = string.rep(one_row, H)
    return hdr .. data
end

-- 16.1 LoadHaldLUT 不存在文件
local rh1, eh1 = HDR.LoadHaldLUT("definitely_not_exist_smoke.png")
if rh1 ~= nil then fail("LoadHaldLUT(missing) should return nil") end
if type(eh1) ~= "string" or not eh1:find("stbi_load failed") then
    fail("LoadHaldLUT(missing) err must contain 'stbi_load failed', got: " .. tostring(eh1))
end
pass("LoadHaldLUT(missing file) returns nil + err: " .. eh1)

-- 16.2 .txt 假装是 image (非合法图像)
local p_txt = write_tmp_cube("not_image", "this is not an image\nfoo bar\n")
-- 注: write_tmp_cube 写 .cube 扩展名, 但 stb_image 按内容判 magic, 与扩展名无关
local rh2, eh2 = HDR.LoadHaldLUT(p_txt)
cleanup_tmp(p_txt)
if rh2 ~= nil then fail("LoadHaldLUT(txt file) should return nil") end
if not eh2:find("stbi_load failed") then
    fail("LoadHaldLUT(txt) err must contain 'stbi_load failed', got: " .. eh2)
end
pass("LoadHaldLUT(non-image content) rejected: " .. eh2)

-- 16.3 1×1 BMP (合法解码但 width 1 不是 N³)
local p_1x1 = tmp_base .. "_tmp_smoke_1x1.bmp"
do
    local ok, err = IO.SaveFile(p_1x1, make_bmp_solid(1, 1, 128, 128, 128))
    if not ok then fail("write 1x1 BMP failed: " .. tostring(err)) end
end
local rh3, eh3 = HDR.LoadHaldLUT(p_1x1)
cleanup_tmp(p_1x1)
if rh3 ~= nil then fail("LoadHaldLUT(1x1) should return nil") end
if not (eh3:find("is not N") or eh3:find("not square")) then
    fail("LoadHaldLUT(1x1) err unexpected: " .. eh3)
end
pass("LoadHaldLUT(1x1 BMP, width=1 non-HALD) rejected: " .. eh3)

-- 16.4 矩形非方阵 (4×2 BMP)
local p_rect = tmp_base .. "_tmp_smoke_4x2.bmp"
do
    local ok = IO.SaveFile(p_rect, make_bmp_solid(4, 2, 0, 255, 0))
    if not ok then fail("write 4x2 BMP failed") end
end
local rh4, eh4 = HDR.LoadHaldLUT(p_rect)
cleanup_tmp(p_rect)
if rh4 ~= nil then fail("LoadHaldLUT(4x2) should return nil") end
if not eh4:find("not square") then
    fail("LoadHaldLUT(4x2) err must contain 'not square', got: " .. eh4)
end
pass("LoadHaldLUT(4x2 BMP non-square) rejected: " .. eh4)

-- 16.5 8×8 BMP HALD level=2 (合法尺寸; identity 内容简化为全 gray, parser 通过)
local p_hald2 = tmp_base .. "_tmp_smoke_hald2.bmp"
do
    local ok = IO.SaveFile(p_hald2, make_bmp_solid(8, 8, 128, 128, 128))
    if not ok then fail("write 8x8 BMP failed") end
end
local rh5, eh5 = HDR.LoadHaldLUT(p_hald2)
cleanup_tmp(p_hald2)
-- headless: backend null → "HDR backend not initialized (HALD parse ok: level=2, size=4)"
-- 或合法 → tex_id > 0
if rh5 ~= nil and type(rh5) ~= "number" then
    fail("LoadHaldLUT(8x8 HALD) ret type unexpected: " .. type(rh5))
end
if rh5 == nil and not eh5:find("parse ok") and not eh5:find("backend") then
    fail("LoadHaldLUT(8x8 HALD) headless err unexpected: " .. tostring(eh5))
end
pass("LoadHaldLUT(8x8 HALD level=2) headless ok (id=" ..
     tostring(rh5) .. ", err=" .. tostring(eh5) .. ")")
if type(rh5) == "number" and rh5 > 0 then HDR.DeleteLUT3D(rh5) end

-- ============================================================
-- 17. Phase F.0.10.8.3 — LUT 热重载 (mtime polling)
-- ============================================================

-- 17.1 默认开关 = true
local hr0 = HDR.GetLUTHotReload()
if hr0 ~= true then fail("Default GetLUTHotReload must be true, got: " .. tostring(hr0)) end
pass("Default GetLUTHotReload() = true (Phase F.0.10.8.3)")

-- 17.2 SetLUTHotReload(false/true) round-trip
HDR.SetLUTHotReload(false)
if HDR.GetLUTHotReload() ~= false then
    fail("SetLUTHotReload(false) round-trip failed, got: " .. tostring(HDR.GetLUTHotReload()))
end
HDR.SetLUTHotReload(true)
if HDR.GetLUTHotReload() ~= true then
    fail("SetLUTHotReload(true) round-trip failed, got: " .. tostring(HDR.GetLUTHotReload()))
end
pass("SetLUTHotReload(false/true) round-trip ok")

-- 17.3 SetLUTHotReload 类型错误 (非 bool 报错)
local ok_type, err_type = pcall(HDR.SetLUTHotReload, "yes")
if ok_type then fail("SetLUTHotReload(string) should raise") end
pass("SetLUTHotReload(non-bool) type-error rejected: " .. tostring(err_type):sub(1, 60))

-- 17.4 WatchLUT 不存在文件 (.cube path)
local rw1, ew1 = HDR.WatchLUT("definitely_not_exist_watch.cube")
if rw1 ~= nil then fail("WatchLUT(missing .cube) should return nil") end
if type(ew1) ~= "string" or not ew1:find("file read failed") then
    fail("WatchLUT(missing .cube) err must contain 'file read failed', got: " .. tostring(ew1))
end
pass("WatchLUT(missing .cube file) rejected: " .. ew1)

-- 17.5 WatchLUT 不存在图像 (.png path → 走 HALD parser)
local rw2, ew2 = HDR.WatchLUT("definitely_not_exist_watch.png")
if rw2 ~= nil then fail("WatchLUT(missing .png) should return nil") end
if not ew2:find("stbi_load failed") then
    fail("WatchLUT(missing .png) err must contain 'stbi_load failed', got: " .. ew2)
end
pass("WatchLUT(missing .png file) rejected: " .. ew2)

-- 17.6 UnwatchLUT(0) → false (silent)
if HDR.UnwatchLUT(0) ~= false then fail("UnwatchLUT(0) should return false") end
pass("UnwatchLUT(0) returns false (silent)")

-- 17.7 UnwatchLUT(不存在 id) → false (silent)
if HDR.UnwatchLUT(99999) ~= false then fail("UnwatchLUT(99999) should return false") end
pass("UnwatchLUT(non-existent id) returns false (silent)")

-- 17.8 GetWatchedLUTId(未注册 path) → nil
if HDR.GetWatchedLUTId("never_registered.cube") ~= nil then
    fail("GetWatchedLUTId(not_watched) should return nil")
end
pass("GetWatchedLUTId(not_watched) returns nil")

-- 17.9 PollLUTReloads 空 list → 0
local n_poll = HDR.PollLUTReloads()
if n_poll ~= 0 then fail("PollLUTReloads(empty list) should be 0, got: " .. tostring(n_poll)) end
pass("PollLUTReloads(empty watchList) returns 0")

-- 17.10 SetLUTHotReload(false) + PollLUTReloads → 0 (短路)
HDR.SetLUTHotReload(false)
if HDR.PollLUTReloads() ~= 0 then
    fail("PollLUTReloads with hot reload OFF should be 0")
end
HDR.SetLUTHotReload(true)   -- 恢复默认
pass("PollLUTReloads short-circuit (hot reload OFF) returns 0")

-- ============================================================
-- Done
-- ============================================================

print("[Phase E.3 + E.14 + E.18.1 + E.18.2 + F.0.10.2 + F.0.10.3 + F.0.10.6 + F.0.10.8 + F.0.10.8.1 + F.0.10.8.2 + F.0.10.8.3] Light.Graphics.HDR smoke PASS (" .. #fn_names .. " functions)")
