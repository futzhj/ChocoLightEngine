-- ============================================================
-- Phase AT — Audio 3D 空间化 + Mixer + Effects 烟雾测试
-- ============================================================
--
-- 验证 5 个层面:
--   1. 模块加载 (Sound / SoundGroup / Effect)
--   2. Sound API 完整 (25+ 方法存在)
--   3. SoundGroup API + 嵌套 + 循环检测
--   4. Effect 6 种工厂 + Set/GetEnabled
--   5. Light.Audio Listener + GlobalVolume 7 fns
--   6. 不存在文件加载失败回 nil + err (不崩)
--
-- CI 无音频设备时, AudioBackend.Init 会失败, Sound.Load/Effect.New* 都返回 nil + err — 这是预期行为.

local Sound = require("Light.Audio.Sound")
local SoundGroup = require("Light.Audio.SoundGroup")
local Effect = require("Light.Audio.Effect")
local Audio = require("Light.Audio")

local pass_count = 0
local function pass(msg) pass_count = pass_count + 1; print("  PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. msg, 2) end

-- ==================== 1) 模块加载 ====================

print("[1] 模块加载")
if type(Sound) ~= "table" then fail("Light.Audio.Sound module not loaded") end
if type(SoundGroup) ~= "table" then fail("Light.Audio.SoundGroup module not loaded") end
if type(Effect) ~= "table" then fail("Light.Audio.Effect module not loaded") end
pass("Sound / SoundGroup / Effect 三模块都成功加载")

-- ==================== 2) Sound API 完整性 ====================

print("[2] Sound 工厂函数")
if type(Sound.Load) ~= "function" then fail("Sound.Load not function") end
if type(Sound.LoadPCM) ~= "function" then fail("Sound.LoadPCM not function") end
pass("Sound.Load / Sound.LoadPCM 存在")

-- 不存在文件应返回 nil + err
local s, err = Sound.Load("nonexistent.wav")
if s ~= nil then fail("Sound.Load(nonexistent) should return nil") end
if type(err) ~= "string" then fail("Sound.Load err type") end
pass("Sound.Load(nonexistent) -> nil, '" .. err .. "'")

-- LoadPCM 无效格式
local s2, err2 = Sound.LoadPCM("xxx", "bogus", 2, 44100)
if s2 ~= nil then fail("Sound.LoadPCM(bogus fmt) should return nil") end
pass("Sound.LoadPCM(bogus fmt) -> nil, '" .. err2 .. "'")

-- LoadPCM 数据太短
local s3, err3 = Sound.LoadPCM("", "s16", 2, 44100)
if s3 ~= nil then fail("Sound.LoadPCM(empty) should return nil") end
pass("Sound.LoadPCM(empty) -> nil, '" .. err3 .. "'")

-- ==================== 3) SoundGroup API + 嵌套 + 循环检测 ====================

print("[3] SoundGroup 嵌套 + 循环检测")
if type(SoundGroup.New) ~= "function" then fail("SoundGroup.New not function") end
pass("SoundGroup.New 存在")

local g1 = SoundGroup.New()
if g1 then
    pass("SoundGroup.New() ok")

    -- 基本方法存在
    if type(g1.SetVolume) ~= "function" then fail("group:SetVolume missing") end
    if type(g1.GetVolume) ~= "function" then fail("group:GetVolume missing") end
    if type(g1.SetPitch) ~= "function" then fail("group:SetPitch missing") end
    if type(g1.GetPitch) ~= "function" then fail("group:GetPitch missing") end
    if type(g1.Pause) ~= "function" then fail("group:Pause missing") end
    if type(g1.Resume) ~= "function" then fail("group:Resume missing") end
    if type(g1.Stop) ~= "function" then fail("group:Stop missing") end
    if type(g1.SetParent) ~= "function" then fail("group:SetParent missing") end
    if type(g1.SetEffect) ~= "function" then fail("group:SetEffect missing") end
    if type(g1.Delete) ~= "function" then fail("group:Delete missing") end
    pass("group 10+ methods all callable")

    g1:SetVolume(0.5)
    local v = g1:GetVolume()
    if math.abs(v - 0.5) > 0.01 then fail("Set/GetVolume mismatch: " .. tostring(v)) end
    pass("SetVolume(0.5) -> GetVolume() = " .. tostring(v))

    g1:SetPitch(1.5)
    local p = g1:GetPitch()
    if math.abs(p - 1.5) > 0.01 then fail("Set/GetPitch mismatch") end
    pass("SetPitch(1.5) -> GetPitch() = " .. tostring(p))

    -- 嵌套: g2 -> g1
    local g2 = SoundGroup.New(g1)
    if g2 then
        pass("SoundGroup.New(g1) 嵌套创建 ok")

        -- 循环检测: 把 g1 父级设成 g2 应该失败 (g2 已经是 g1 的子)
        local ok, e = g1:SetParent(g2)
        if ok then fail("cycle SetParent should fail") end
        pass("g1:SetParent(g2) 循环检测拒绝, err='" .. tostring(e) .. "'")

        -- 自循环: g1:SetParent(g1) 也应该失败
        local ok2, e2 = g1:SetParent(g1)
        if ok2 then fail("self-cycle should fail") end
        pass("g1:SetParent(g1) 自循环拒绝, err='" .. tostring(e2) .. "'")

        -- 合法: g1 设 nil 父级 (master)
        local ok3 = g1:SetParent(nil)
        if not ok3 then fail("SetParent(nil) should succeed") end
        pass("g1:SetParent(nil) ok (master)")

        g2:Delete()
        pass("g2:Delete() ok")
    else
        pass("SoundGroup.New(g1) skipped (engine not init?)")
    end

    g1:Delete()
    pass("g1:Delete() ok")
else
    pass("SoundGroup.New() skipped (engine not init, headless)")
end

-- ==================== 4) Effect 6 种工厂 ====================

print("[4] Effect 工厂 + 控制")
local effectFactories = {
    { "NewLowPass",  function() return Effect.NewLowPass(2000) end },
    { "NewHighPass", function() return Effect.NewHighPass(500) end },
    { "NewBandPass", function() return Effect.NewBandPass(1000) end },
    { "NewNotch",    function() return Effect.NewNotch(60, 1.0) end },
    { "NewPeak",     function() return Effect.NewPeak(2000, 6.0, 1.0) end },
    { "NewEcho",     function() return Effect.NewEcho({delay_ms=200, decay=0.3, wet=0.5, dry=0.5}) end },
}
for _, t in ipairs(effectFactories) do
    local name, factory = t[1], t[2]
    if type(Effect[name]) ~= "function" then fail("Effect." .. name .. " not function") end
    local e = factory()
    if e then
        if type(e.SetEnabled) ~= "function" then fail("effect:SetEnabled missing") end
        if type(e.GetEnabled) ~= "function" then fail("effect:GetEnabled missing") end
        e:SetEnabled(false)
        if e:GetEnabled() then fail("SetEnabled(false) ineffective") end
        e:SetEnabled(true)
        if not e:GetEnabled() then fail("SetEnabled(true) ineffective") end
        e:Delete()
        pass("Effect." .. name .. " 工厂 + Set/GetEnabled + Delete ok")
    else
        pass("Effect." .. name .. " skipped (engine not init)")
    end
end

-- ==================== 5) Light.Audio Listener + GlobalVolume ====================

print("[5] Light.Audio Listener + GlobalVolume")
local listenerFns = {
    "SetListenerPosition", "SetListenerDirection", "SetListenerWorldUp",
    "SetListenerVelocity", "GetListenerCount",
    "SetGlobalVolume", "GetGlobalVolume",
}
for _, fn in ipairs(listenerFns) do
    if type(Audio[fn]) ~= "function" then fail("Light.Audio." .. fn .. " missing") end
end
pass("Light.Audio 7 个 Phase AT fn 全部存在")

-- 调用边界
local ok = Audio.SetListenerPosition(0, 0, 0)
if ok ~= true then fail("SetListenerPosition default ok") end
pass("SetListenerPosition(0,0,0) -> true")

local ok2, e2 = Audio.SetListenerPosition("x", "y", "z")
if ok2 ~= false then fail("SetListenerPosition(strings) should fail") end
pass("SetListenerPosition('x','y','z') -> false, '" .. tostring(e2) .. "'")

Audio.SetGlobalVolume(0.8)
local gv = Audio.GetGlobalVolume()
-- 注: 如果 engine 未 init, GetGlobalVolume 返回 1.0; 否则返回 0.8
if not (math.abs(gv - 0.8) < 0.01 or math.abs(gv - 1.0) < 0.01) then
    fail("GetGlobalVolume after Set: " .. tostring(gv))
end
pass("Set/GetGlobalVolume = " .. tostring(gv))

local lc = Audio.GetListenerCount()
if type(lc) ~= "number" then fail("GetListenerCount type") end
pass("GetListenerCount = " .. tostring(lc))

-- ==================== 6) Sound 与 Group/Effect 联动 (需 audio device) ====================

print("[6] Sound -> Group/Effect (依赖 audio device, 可能 skip)")
-- 如果 engine 已 init, 用 LoadPCM 创建一个 sound (1 帧 silent f32 stereo)
local silenceData = string.rep(string.char(0), 8)  -- 1 frame f32 stereo = 8 bytes
local s = Sound.LoadPCM(silenceData, "f32", 2, 44100)
if s then
    pass("Sound.LoadPCM(1 frame f32 stereo) ok")

    -- 25 个方法存在性
    local methods = {
        "Play", "Pause", "Stop", "IsPlaying",
        "SetVolume", "GetVolume", "SetLooping", "GetLooping",
        "SetPitch", "GetPitch", "SetPan", "GetPan",
        "Set3DEnabled", "Get3DEnabled", "SetPosition", "GetPosition",
        "SetVelocity", "SetAttenuationModel", "GetAttenuationModel",
        "SetMinDistance", "GetMinDistance",
        "SetMaxDistance", "GetMaxDistance",
        "SetRolloff", "GetRolloff",
        "SetGroup", "SetEffect",
        "Delete",
    }
    for _, m in ipairs(methods) do
        if type(s[m]) ~= "function" then fail("sound:" .. m .. " missing") end
    end
    pass("sound 28 个方法全部存在")

    -- 基本调用
    s:SetVolume(0.7); local v = s:GetVolume()
    if math.abs(v - 0.7) > 0.05 then fail("Set/GetVolume mismatch") end
    pass("Set/GetVolume(0.7) ok")

    s:SetLooping(true)
    if not s:GetLooping() then fail("SetLooping(true) ineffective") end
    pass("Set/GetLooping ok")

    s:SetPitch(1.2)
    local p = s:GetPitch()
    if math.abs(p - 1.2) > 0.05 then fail("Set/GetPitch mismatch") end
    pass("Set/GetPitch(1.2) ok")

    s:SetPan(-0.5)
    local pan = s:GetPan()
    if math.abs(pan + 0.5) > 0.05 then fail("Set/GetPan mismatch") end
    pass("Set/GetPan(-0.5) ok")

    s:Set3DEnabled(true)
    if not s:Get3DEnabled() then fail("Set3DEnabled ineffective") end
    pass("Set/Get3DEnabled ok")

    s:SetPosition(1.0, 2.0, 3.0)
    local x, y, z = s:GetPosition()
    if math.abs(x - 1.0) > 0.05 or math.abs(y - 2.0) > 0.05 or math.abs(z - 3.0) > 0.05 then
        fail("Set/GetPosition mismatch")
    end
    pass("Set/GetPosition(1,2,3) ok")

    s:SetAttenuationModel("linear")
    if s:GetAttenuationModel() ~= "linear" then fail("attenuation model mismatch") end
    pass("Set/GetAttenuationModel('linear') ok")

    s:SetMinDistance(2.0)
    if math.abs(s:GetMinDistance() - 2.0) > 0.05 then fail("MinDistance mismatch") end
    pass("Set/GetMinDistance(2) ok")

    -- Sound -> Group 关联
    local g = SoundGroup.New()
    if g then
        s:SetGroup(g)
        s:SetGroup(nil)  -- 解除
        pass("sound:SetGroup(group) / sound:SetGroup(nil) ok")
        g:Delete()
    end

    -- Sound -> Effect 关联
    local eff = Effect.NewLowPass(1000)
    if eff then
        s:SetEffect(eff)
        s:SetEffect(nil)  -- 解除
        pass("sound:SetEffect(effect) / sound:SetEffect(nil) ok")
        eff:Delete()
    end

    s:Delete()
    pass("sound:Delete() ok")
else
    pass("Sound.LoadPCM skipped (engine not init in headless)")
end

-- ==================== 总结 ====================

print(string.format("\n[Phase AT smoke] All %d assertions PASSED", pass_count))
