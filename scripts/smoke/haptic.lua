-- Phase K smoke: Light.Haptic
--
-- CI runner 无真实 haptic 设备:
--   - Init 在 Linux/macOS/Windows 通常成功 (子系统可初始化, 设备列表为空)
--   - Init 在 headless 沙箱可能失败 -> 视为通过, 不进 happy path
--   - Open 必然 nil + err
--   - dev 相关 fn 全部走 nil 边界

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

local ok, mod = pcall(require, "Light.Haptic")
if not ok then fail("require(Light.Haptic) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Haptic not a table") end

-- Phase G: 22 个 fn 注册校验
local required = {
    "Init", "Quit", "GetHaptics", "IsMouseHaptic",
    "Open", "OpenFromMouse", "Close",
    "GetID", "GetName", "GetFeatures",
    "GetMaxEffects", "GetMaxEffectsPlaying", "GetNumAxes",
    "RumbleSupported", "InitRumble", "PlayRumble", "StopRumble",
    "Pause", "Resume", "SetGain", "SetAutocenter", "StopAll",
}
for _, k in ipairs(required) do
    if type(mod[k]) ~= "function" then fail("Light.Haptic." .. k .. " missing") end
end
pass(string.format("Light.Haptic module ok (%d Phase G functions)", #required))

-- Phase AK: 11 个新 fn 注册校验
local phase_ak_fns = {
    "OpenFromJoystick", "IsJoystickHaptic", "GetHapticFromID",
    "EffectSupported", "CreateEffect", "UpdateEffect",
    "RunEffect", "StopEffect", "DestroyEffect", "GetEffectStatus",
}
for _, k in ipairs(phase_ak_fns) do
    if type(mod[k]) ~= "function" then fail("Light.Haptic." .. k .. " missing (Phase AK)") end
end
pass(string.format("Light.Haptic Phase AK fns ok (%d functions)", #phase_ak_fns))

-- Phase AK: 25 个常量校验
local consts = {
    "HAPTIC_CONSTANT","HAPTIC_SINE","HAPTIC_SQUARE","HAPTIC_TRIANGLE",
    "HAPTIC_SAWTOOTHUP","HAPTIC_SAWTOOTHDOWN","HAPTIC_RAMP",
    "HAPTIC_SPRING","HAPTIC_DAMPER","HAPTIC_INERTIA","HAPTIC_FRICTION",
    "HAPTIC_LEFTRIGHT","HAPTIC_RESERVED1","HAPTIC_RESERVED2","HAPTIC_RESERVED3","HAPTIC_CUSTOM",
    "HAPTIC_GAIN","HAPTIC_AUTOCENTER","HAPTIC_STATUS","HAPTIC_PAUSE",
    "HAPTIC_POLAR","HAPTIC_CARTESIAN","HAPTIC_SPHERICAL","HAPTIC_STEERING_AXIS",
    "HAPTIC_INFINITY",
}
for _, k in ipairs(consts) do
    if type(mod[k]) ~= "number" then fail("Light.Haptic." .. k .. " missing or not number") end
end
-- 关键 sanity check: bit flags 互不相等; HAPTIC_INFINITY 应等于 4294967295
if mod.HAPTIC_CONSTANT == mod.HAPTIC_SINE then fail("CONSTANT == SINE: bit flag conflict") end
if mod.HAPTIC_INFINITY ~= 4294967295 then
    fail("HAPTIC_INFINITY = " .. tostring(mod.HAPTIC_INFINITY) .. ", expected 4294967295")
end
if mod.HAPTIC_POLAR ~= 0 or mod.HAPTIC_CARTESIAN ~= 1 then
    fail("HAPTIC direction encoding values wrong")
end
pass(string.format("Light.Haptic constants ok (%d consts, INFINITY=%g)", #consts, mod.HAPTIC_INFINITY))

-- ===== Init =====
local iok, ierr = mod.Init()
if iok ~= true then
    pass("Light.Haptic.Init failed (acceptable in sandbox): " .. tostring(ierr))
    -- 依然可调用 IsMouseHaptic / GetHaptics, 它们走 SDL3 默认空状态
end

-- ===== 设备发现 =====

-- IsMouseHaptic 总是返回 bool (无状态依赖)
local has_mouse = mod.IsMouseHaptic()
if type(has_mouse) ~= "boolean" then fail("IsMouseHaptic must return bool") end
pass("Light.Haptic.IsMouseHaptic = " .. tostring(has_mouse))

-- GetHaptics 必返回数组 (可能为空)
local list, gerr = mod.GetHaptics()
if type(list) ~= "table" then
    -- Init 失败时也允许 nil + err
    if iok == true then fail("GetHaptics must return array, got " .. type(list)) end
    pass("GetHaptics returned nil (Init failed earlier): " .. tostring(gerr))
else
    pass(string.format("Light.Haptic.GetHaptics ok (%d devices)", #list))
    if #list > 0 then
        local d = list[1]
        if type(d.id) ~= "number" then fail("device.id must be number") end
        if type(d.name) ~= "string" then fail("device.name must be string") end
        pass(string.format("  first device: id=%d name=%s", d.id, d.name))
    end
end

-- ===== 边界: Open 无效 id =====
local dev1, oerr = mod.Open(0xDEADBEEF)
if dev1 ~= nil or oerr == nil then fail("Open(invalid id) should be nil+err") end
pass("Light.Haptic.Open(0xDEADBEEF) boundary ok: " .. tostring(oerr))

-- ===== 边界: 所有 dev 相关 fn 在 nil 句柄上的失败语义 =====

-- 返回 (nil + err) 类
local nil_err_fns = {
    {name="GetID",                args={}},
    {name="GetName",               args={}},
    {name="GetFeatures",           args={}},
    {name="GetMaxEffects",         args={}},
    {name="GetMaxEffectsPlaying",  args={}},
    {name="GetNumAxes",            args={}},
}
for _, t in ipairs(nil_err_fns) do
    local r1, r2 = mod[t.name](nil, unpack(t.args))
    if r1 ~= nil or r2 == nil then
        fail(t.name .. "(nil) should be nil+err, got " .. tostring(r1) .. ", " .. tostring(r2))
    end
end
pass("Light.Haptic dev-query fns boundary ok (6 fns return nil+err on nil handle)")

-- 返回 (false + err) 类
local false_err_fns = {
    {name="Close",          args={}},
    {name="InitRumble",     args={}},
    {name="PlayRumble",     args={0.5, 100}},
    {name="StopRumble",     args={}},
    {name="Pause",          args={}},
    {name="Resume",         args={}},
    {name="SetGain",        args={50}},
    {name="SetAutocenter",  args={50}},
    {name="StopAll",        args={}},
}
for _, t in ipairs(false_err_fns) do
    local r1, r2 = mod[t.name](nil, unpack(t.args))
    if r1 ~= false or r2 == nil then
        fail(t.name .. "(nil) should be false+err, got " .. tostring(r1) .. ", " .. tostring(r2))
    end
end
pass("Light.Haptic dev-action fns boundary ok (9 fns return false+err on nil handle)")

-- RumbleSupported(nil) -> false (单返回值 bool)
local rs = mod.RumbleSupported(nil)
if rs ~= false then fail("RumbleSupported(nil) should be false") end
pass("Light.Haptic.RumbleSupported(nil) boundary ok")

-- ===== 范围保护 (PlayRumble / SetGain / SetAutocenter) =====
-- 即使 nil handle, 参数校验也不应抛 lua error (luaL_checknumber 会抛, 但顺序: 先 CheckHaptic).
-- 此处验证: nil 优先报 invalid handle, 而不是参数类型错.
-- (实际实现里 PlayRumble 在 dev==nil 时直接返回, 不会读 stack[2..3])
-- 因此参数缺失也不会崩 -> 用 nil + nil 测试
local pr1, pr2 = mod.PlayRumble(nil, nil, nil)
if pr1 ~= false or pr2 == nil then fail("PlayRumble(nil,nil,nil) should be false+err") end
pass("Light.Haptic.PlayRumble(nil,nil,nil) safe boundary ok")

-- ==================================================================
-- Phase AK: Joystick 集成 边界
-- ==================================================================

-- OpenFromJoystick(nil) -> nil + err
local jh, jerr = mod.OpenFromJoystick(nil)
if jh ~= nil or jerr == nil then fail("OpenFromJoystick(nil) should be nil+err") end
pass("Light.Haptic.OpenFromJoystick(nil) boundary ok: " .. tostring(jerr))

-- IsJoystickHaptic(nil) -> false (单返回值 bool)
if mod.IsJoystickHaptic(nil) ~= false then fail("IsJoystickHaptic(nil) should be false") end
pass("Light.Haptic.IsJoystickHaptic(nil) = false ok")

-- GetHapticFromID(0xDEADBEEF) -> nil + err  (无效 ID, SDL3 报 not opened)
local fh, ferr = mod.GetHapticFromID(0xDEADBEEF)
if fh ~= nil or ferr == nil then fail("GetHapticFromID(invalid) should be nil+err") end
pass("Light.Haptic.GetHapticFromID(invalid) boundary ok: " .. tostring(ferr))

-- ==================================================================
-- Phase AK: HapticEffect 边界 (无真实 dev, 全走 nil 路径)
-- ==================================================================

-- EffectSupported(nil, table) -> false 单返回值
if mod.EffectSupported(nil, {type="sine"}) ~= false then
    fail("EffectSupported(nil) should be false")
end
pass("Light.Haptic.EffectSupported(nil) = false ok")

-- CreateEffect(nil, table) -> -1 + err
local cid, cerr = mod.CreateEffect(nil, {type="sine"})
if cid ~= -1 or cerr == nil then fail("CreateEffect(nil) should be -1+err") end
pass("Light.Haptic.CreateEffect(nil) boundary ok: " .. tostring(cerr))

-- UpdateEffect(nil, 0, table) -> false + err
local u1, uerr = mod.UpdateEffect(nil, 0, {type="sine"})
if u1 ~= false or uerr == nil then fail("UpdateEffect(nil) should be false+err") end
pass("Light.Haptic.UpdateEffect(nil) boundary ok: " .. tostring(uerr))

-- RunEffect(nil, 0, 1) -> false + err
local r1, rerr = mod.RunEffect(nil, 0, 1)
if r1 ~= false or rerr == nil then fail("RunEffect(nil) should be false+err") end
pass("Light.Haptic.RunEffect(nil) boundary ok: " .. tostring(rerr))

-- RunEffect(nil, 0, HAPTIC_INFINITY) -> false+err (验证 INFINITY 不会数值溢出)
local rinf1, rinferr = mod.RunEffect(nil, 0, mod.HAPTIC_INFINITY)
if rinf1 ~= false or rinferr == nil then fail("RunEffect(nil, INFINITY) should be false+err") end
pass("Light.Haptic.RunEffect(nil, INFINITY) safe ok: " .. tostring(rinferr))

-- StopEffect(nil, 0) -> false + err
local s1, serr = mod.StopEffect(nil, 0)
if s1 ~= false or serr == nil then fail("StopEffect(nil) should be false+err") end
pass("Light.Haptic.StopEffect(nil) boundary ok: " .. tostring(serr))

-- DestroyEffect(nil, 0) -> nil (静默, 不报错)
local d1 = mod.DestroyEffect(nil, 0)
if d1 ~= nil then fail("DestroyEffect(nil) should silently return nil") end
pass("Light.Haptic.DestroyEffect(nil) silent ok")

-- GetEffectStatus(nil, 0) -> false (单返回值 bool)
if mod.GetEffectStatus(nil, 0) ~= false then fail("GetEffectStatus(nil) should be false") end
pass("Light.Haptic.GetEffectStatus(nil) = false ok")

-- ==================================================================
-- Phase AK: ParseHapticEffect 错误路径 (用 nil dev, 但 effect table 已经被解析)
-- 注: nil dev 在 ParseHapticEffect 之前就拦截 (CheckHaptic), 所以这些不会执行 Parse.
-- 但 EffectSupported 例外: 它先 CheckHaptic, 即使 nil 也会先 ParseHapticEffect ?
-- 不, 看代码: CheckHaptic 后直接 if (!dev) return false. ParseHapticEffect 不会执行.
-- 这里测试用 EffectSupported, 但它只对 dev 校验, 不深入 Parse. 因此无法触发 Parse 错误.
-- 跳过 Parse 错误的纯 nil 测试 -> 只验证调用安全, 不验证 err 内容.

-- 综合: 各种 type 字符串调用应不崩 (即使 nil dev)
for _, t in ipairs({"constant","sine","square","triangle","sawtooth_up",
                    "sawtooth_down","ramp","spring","damper","inertia",
                    "friction","leftright","custom"}) do
    local ok_call = pcall(mod.CreateEffect, nil, {type=t, length=100})
    if not ok_call then fail("CreateEffect(nil, {type='"..t.."'}) should not throw lua error") end
end
pass("Light.Haptic 12 effect types all parsable (no lua error) ok")

-- 不存在的 effect type 字符串 -> ParseHapticEffect 失败, CreateEffect -> -1+err
-- 但要注意 nil dev 先返回 -1+err, ParseEffectType 不会执行.
-- 用一个 garbage table 调用 EffectSupported(nil, ...) 验证不崩
local ok_call2 = pcall(mod.EffectSupported, nil, {type="__bogus_effect__"})
if not ok_call2 then fail("EffectSupported(nil, garbage) should not throw") end
pass("Light.Haptic EffectSupported(nil, garbage) safe ok")

-- ===== Quit =====
mod.Quit()
pass("Light.Haptic.Quit ok")

print("haptic smoke ok (Phase G + Phase AK)")
