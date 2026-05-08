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

-- 22 个 fn 注册校验
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
pass(string.format("Light.Haptic module ok (%d functions)", #required))

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

-- ===== Quit =====
mod.Quit()
pass("Light.Haptic.Quit ok")

print("haptic smoke ok")
