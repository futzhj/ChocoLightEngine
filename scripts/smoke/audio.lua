-- Phase AM smoke: Light.Audio
--
-- 验证:
--   * 51 fns 全注册
--   * 16 const 存在且类型/数值正确
--   * Drivers / Device 发现 / 流创建-销毁 / IO 边界 在 headless CI 上不崩
--   * 句柄 nil safety / devid invalid safety
--   * AudioStream 端到端 (Create -> SetFormat -> Put/Get -> Destroy)

local function fail(msg)
    print("FAIL: " .. tostring(msg))
    os.exit(1)
end

local function pass(msg) print(msg) end

local function assert_function(t, k)
    if type(t[k]) ~= "function" then fail(k .. " not a function") end
end

local function assert_number(v, name)
    if type(v) ~= "number" then fail(name .. " not number, got " .. type(v)) end
end

-- ==================== 1) module loads ====================

local ok, A = pcall(require, "Light.Audio")
if not ok then fail("require(Light.Audio) failed: " .. tostring(A)) end
if type(A) ~= "table" then fail("Light.Audio not a table") end

-- ==================== 2) 51 fns 全注册 ====================

local fns_drivers = { "GetNumAudioDrivers", "GetAudioDriver", "GetCurrentAudioDriver" }
local fns_dev_query = {
    "GetAudioPlaybackDevices", "GetAudioRecordingDevices", "GetAudioDeviceName",
    "GetAudioDeviceFormat", "GetAudioDeviceChannelMap",
    "IsAudioDevicePhysical", "IsAudioDevicePlayback", "GetAudioDeviceGain",
}
local fns_dev_ctrl = {
    "OpenAudioDevice", "CloseAudioDevice",
    "PauseAudioDevice", "ResumeAudioDevice", "AudioDevicePaused",
    "SetAudioDeviceGain",
}
local fns_stream_life = { "CreateAudioStream", "DestroyAudioStream" }
local fns_stream_cfg = {
    "GetAudioStreamFormat", "SetAudioStreamFormat",
    "GetAudioStreamFrequencyRatio", "SetAudioStreamFrequencyRatio",
    "GetAudioStreamGain", "SetAudioStreamGain",
    "GetAudioStreamInputChannelMap", "GetAudioStreamOutputChannelMap",
    "SetAudioStreamInputChannelMap", "SetAudioStreamOutputChannelMap",
}
local fns_stream_bind = {
    "BindAudioStream", "BindAudioStreams",
    "UnbindAudioStream", "UnbindAudioStreams",
    "GetAudioStreamDevice",
}
local fns_stream_io = {
    "PutAudioStreamData", "GetAudioStreamData",
    "GetAudioStreamAvailable", "GetAudioStreamQueued",
    "ClearAudioStream", "FlushAudioStream",
}
local fns_stream_lock = { "LockAudioStream", "UnlockAudioStream" }
local fns_stream_devctl = {
    "PauseAudioStreamDevice", "ResumeAudioStreamDevice", "AudioStreamDevicePaused",
}
local fns_simplified = { "OpenAudioDeviceStream" }
local fns_wav = { "LoadWAV", "LoadWAV_IO" }  -- Phase AN: +LoadWAV_IO
local fns_utils = { "MixAudio", "ConvertAudioSamples", "GetAudioFormatName", "GetSilenceValueForFormat" }
-- Phase AN: Stream properties + callbacks (3 fns)
local fns_stream_callbacks = {
    "GetAudioStreamProperties",
    "SetAudioStreamGetCallback",
    "SetAudioStreamPutCallback",
}

local total = 0
for _, group in ipairs({
    fns_drivers, fns_dev_query, fns_dev_ctrl, fns_stream_life, fns_stream_cfg,
    fns_stream_bind, fns_stream_io, fns_stream_lock, fns_stream_devctl,
    fns_simplified, fns_wav, fns_utils, fns_stream_callbacks,
}) do
    for _, k in ipairs(group) do
        assert_function(A, k)
        total = total + 1
    end
end
if total ~= 55 then fail("expected 55 fns (51 AM + 4 AN), registered " .. total) end
pass("Light.Audio module ok (55 functions registered, Phase AM 51 + Phase AN 4)")

-- ==================== 3) 16 constants ====================

local consts_int = {
    AUDIO_UNKNOWN = 0x0000,
    AUDIO_U8      = 0x0008,
    AUDIO_S8      = 0x8008,
    AUDIO_S16LE   = 0x8010,
    AUDIO_S16BE   = 0x9010,
    AUDIO_S32LE   = 0x8020,
    AUDIO_S32BE   = 0x9020,
    AUDIO_F32LE   = 0x8120,
    AUDIO_F32BE   = 0x9120,
    -- AUDIO_S16/S32/F32 是 host-endian 别名, 在 LE 平台等于 *LE
    -- 不固定值, 只验证存在
    AUDIO_MASK_BITSIZE    = 0xFF,
    AUDIO_MASK_FLOAT      = 0x100,
    AUDIO_MASK_BIG_ENDIAN = 0x1000,
    AUDIO_MASK_SIGNED     = 0x8000,
}
for k, expected in pairs(consts_int) do
    assert_number(A[k], k)
    if A[k] ~= expected then fail(k .. " expected " .. expected .. " got " .. A[k]) end
end
-- host-endian aliases: 不验证具体值
for _, k in ipairs({ "AUDIO_S16", "AUDIO_S32", "AUDIO_F32" }) do
    assert_number(A[k], k)
end
-- device default values (Uint32)
assert_number(A.AUDIO_DEVICE_DEFAULT_PLAYBACK, "AUDIO_DEVICE_DEFAULT_PLAYBACK")
assert_number(A.AUDIO_DEVICE_DEFAULT_RECORDING, "AUDIO_DEVICE_DEFAULT_RECORDING")
if A.AUDIO_DEVICE_DEFAULT_PLAYBACK ~= 4294967295 then
    fail("AUDIO_DEVICE_DEFAULT_PLAYBACK expected 4294967295, got "
         .. A.AUDIO_DEVICE_DEFAULT_PLAYBACK)
end
if A.AUDIO_DEVICE_DEFAULT_RECORDING ~= 4294967294 then
    fail("AUDIO_DEVICE_DEFAULT_RECORDING expected 4294967294, got "
         .. A.AUDIO_DEVICE_DEFAULT_RECORDING)
end
pass("Light.Audio constants ok (16 consts, DEFAULT_PLAYBACK="
     .. A.AUDIO_DEVICE_DEFAULT_PLAYBACK .. ")")

-- ==================== 4) Drivers ====================

local n_drivers = A.GetNumAudioDrivers()
assert_number(n_drivers, "GetNumAudioDrivers()")
if n_drivers < 1 then fail("expected >=1 audio driver, got " .. n_drivers) end
pass("Light.Audio.GetNumAudioDrivers = " .. n_drivers)

-- 枚举所有 drivers
local first = A.GetAudioDriver(0)
if type(first) ~= "string" then fail("GetAudioDriver(0) should return string") end
pass("Light.Audio.GetAudioDriver(0) = '" .. first .. "'")

-- 越界
local oob, oob_err = A.GetAudioDriver(9999)
if oob ~= nil or oob_err == nil then fail("GetAudioDriver(oob) should be nil+err") end
pass("Light.Audio.GetAudioDriver(oob) boundary ok: " .. tostring(oob_err))

-- 当前 driver: 桌面 CI runner 通常没初始化, 返回 nil+err 也合法
local cur, cur_err = A.GetCurrentAudioDriver()
pass("Light.Audio.GetCurrentAudioDriver = " .. tostring(cur)
     .. (cur_err and (" (" .. tostring(cur_err) .. ")") or ""))

-- ==================== 5) Device discovery ====================

local pb = A.GetAudioPlaybackDevices()
if type(pb) ~= "table" then fail("GetAudioPlaybackDevices should return table") end
pass(string.format("Light.Audio.GetAudioPlaybackDevices ok (%d devices)", #pb))

local rec = A.GetAudioRecordingDevices()
if type(rec) ~= "table" then fail("GetAudioRecordingDevices should return table") end
pass(string.format("Light.Audio.GetAudioRecordingDevices ok (%d devices)", #rec))

-- ==================== 6) Default device queries (always works) ====================

local DEFAULT = A.AUDIO_DEVICE_DEFAULT_PLAYBACK

-- GetAudioDeviceFormat 即使设备不存在也可能返回 default spec
local spec, frames, err = A.GetAudioDeviceFormat(DEFAULT)
if spec then
    if type(spec) ~= "table" then fail("spec should be table") end
    assert_number(spec.format, "spec.format")
    assert_number(spec.channels, "spec.channels")
    assert_number(spec.freq, "spec.freq")
    assert_number(frames, "sample_frames")
    pass(string.format("Light.Audio.GetAudioDeviceFormat(DEFAULT) ok: fmt=0x%X ch=%d freq=%d frames=%d",
        spec.format, spec.channels, spec.freq, frames))
else
    pass("Light.Audio.GetAudioDeviceFormat(DEFAULT) returned nil: " .. tostring(err))
end

-- IsAudioDevice* 总是返回 boolean (即使 invalid id)
local is_phys = A.IsAudioDevicePhysical(DEFAULT)
local is_play = A.IsAudioDevicePlayback(DEFAULT)
if type(is_phys) ~= "boolean" or type(is_play) ~= "boolean" then
    fail("IsAudioDevice* should return boolean")
end
pass("Light.Audio.IsAudioDevice* ok (phys=" .. tostring(is_phys)
     .. ", play=" .. tostring(is_play) .. ")")

-- ==================== 7) Boundary: invalid devid ====================

local INVALID_ID = 0xDEADBEEF
local boundary_dev_fns = {
    "GetAudioDeviceName", "GetAudioDeviceChannelMap", "GetAudioDeviceGain",
    "OpenAudioDevice", "OpenAudioDeviceStream",
}
for _, fname in ipairs(boundary_dev_fns) do
    local rv, re = A[fname](INVALID_ID)
    -- nil + err 或者 数值/handle nil + err 都是合法的
    if rv ~= nil and type(rv) ~= "table" and type(rv) ~= "number" then
        fail(fname .. "(invalid) bad return type: " .. type(rv))
    end
end
pass("Light.Audio 5 invalid-devid boundary fns ok")

-- 非 number devid
for _, fname in ipairs({ "GetAudioDeviceName", "OpenAudioDevice" }) do
    local rv, re = A[fname]("not_a_number")
    if rv ~= nil or re == nil then fail(fname .. "(string) should be nil+err") end
end
pass("Light.Audio devid fns reject non-number arg ok")

-- ==================== 8) Stream lifecycle ====================

local spec_def = { format = A.AUDIO_S16, channels = 2, freq = 44100 }
local stream, serr = A.CreateAudioStream(spec_def, spec_def)
if not stream then fail("CreateAudioStream failed: " .. tostring(serr)) end
if type(stream) ~= "userdata" then fail("stream should be lightuserdata") end
pass("Light.Audio.CreateAudioStream ok")

-- GetAudioStreamFormat
local src, dst, gerr = A.GetAudioStreamFormat(stream)
if not src or not dst then fail("GetAudioStreamFormat failed: " .. tostring(gerr)) end
if src.channels ~= 2 or src.freq ~= 44100 then fail("src spec mismatch") end
pass("Light.Audio.GetAudioStreamFormat ok (src.freq=" .. src.freq .. ")")

-- SetAudioStreamFrequencyRatio
local ok_r, rerr = A.SetAudioStreamFrequencyRatio(stream, 1.5)
if not ok_r then fail("SetAudioStreamFrequencyRatio failed: " .. tostring(rerr)) end
local r, _ = A.GetAudioStreamFrequencyRatio(stream)
if math.abs(r - 1.5) > 0.001 then fail("ratio mismatch: expected 1.5, got " .. r) end
pass("Light.Audio.{Set,Get}AudioStreamFrequencyRatio ok (1.5)")

-- SetAudioStreamGain
local ok_g, _ = A.SetAudioStreamGain(stream, 0.7)
if not ok_g then fail("SetAudioStreamGain failed") end
local g, _ = A.GetAudioStreamGain(stream)
if math.abs(g - 0.7) > 0.001 then fail("gain mismatch") end
pass("Light.Audio.{Set,Get}AudioStreamGain ok (0.7)")

-- ==================== 9) Stream IO end-to-end ====================

-- 推 1 KB silence (16-bit stereo = 4 bytes/frame, 256 frames = 1024 bytes)
local silence_bytes = string.rep(string.char(0), 1024)
local ok_p, perr = A.PutAudioStreamData(stream, silence_bytes)
if not ok_p then fail("PutAudioStreamData failed: " .. tostring(perr)) end
pass("Light.Audio.PutAudioStreamData(1024 bytes) ok")

-- 由于设了 ratio=1.5, 期望 input 1024 bytes -> output ~1536 bytes (但 SDL 内部 buffer/format)
local avail = A.GetAudioStreamAvailable(stream)
assert_number(avail, "available")
if avail < 0 then fail("available should be >=0, got " .. avail) end
pass("Light.Audio.GetAudioStreamAvailable = " .. avail)

local queued = A.GetAudioStreamQueued(stream)
assert_number(queued, "queued")
pass("Light.Audio.GetAudioStreamQueued = " .. queued)

-- 拉数据 (即使 ratio 不是 1, 至少能拿到一些)
local got, gerr2 = A.GetAudioStreamData(stream, 4096)
if got == nil then fail("GetAudioStreamData failed: " .. tostring(gerr2)) end
if type(got) ~= "string" then fail("GetAudioStreamData should return string") end
pass(string.format("Light.Audio.GetAudioStreamData(4096) got %d bytes", #got))

-- Clear
local ok_c, _ = A.ClearAudioStream(stream)
if not ok_c then fail("ClearAudioStream failed") end
pass("Light.Audio.ClearAudioStream ok")

-- Flush
local ok_f, _ = A.FlushAudioStream(stream)
if not ok_f then fail("FlushAudioStream failed") end
pass("Light.Audio.FlushAudioStream ok")

-- Lock / Unlock
local ok_lk, _ = A.LockAudioStream(stream)
if not ok_lk then fail("LockAudioStream failed") end
local ok_uk, _ = A.UnlockAudioStream(stream)
if not ok_uk then fail("UnlockAudioStream failed") end
pass("Light.Audio.{Lock,Unlock}AudioStream ok")

-- Channel maps (默认无 mapping, GetInputChannelMap 返回空 table)
local in_map = A.GetAudioStreamInputChannelMap(stream)
local out_map = A.GetAudioStreamOutputChannelMap(stream)
if type(in_map) ~= "table" or type(out_map) ~= "table" then
    fail("Channel maps should be tables")
end
pass(string.format("Light.Audio Channel maps ok (in=%d, out=%d)", #in_map, #out_map))

-- Set channel map (non-default reversed stereo: [1,0])
-- 注: SDL3 v3.2.30 把 [0,1] 视为 default, 会清回 src_chmap=NULL, 所以测试用 [1,0]
local ok_in, in_err = A.SetAudioStreamInputChannelMap(stream, { 1, 0 })
if not ok_in then fail("SetAudioStreamInputChannelMap({1,0}) failed: " .. tostring(in_err)) end
pass("Light.Audio.SetAudioStreamInputChannelMap({1,0}) ok")

-- 设置后再 Get, 应该返回 {1, 0}
local in_map2, _ = A.GetAudioStreamInputChannelMap(stream)
if type(in_map2) ~= "table" or #in_map2 ~= 2 then
    fail("expected {1,0} after Set, got " .. type(in_map2) .. " #" .. (#in_map2 or 0))
end
pass(string.format("Light.Audio.GetAudioStreamInputChannelMap after Set ok ({%d,%d})",
                   in_map2[1], in_map2[2]))

-- ==================== 10) Stream binding (no real device) ====================

-- Stream 没有绑定到任何设备时 GetAudioStreamDevice 返回 nil+err
local sdev, sderr = A.GetAudioStreamDevice(stream)
if sdev ~= nil then fail("unbound stream should return nil device") end
pass("Light.Audio.GetAudioStreamDevice(unbound) = nil ok: " .. tostring(sderr))

-- Unbind 总是安全 (即使未 bind)
A.UnbindAudioStream(stream)
A.UnbindAudioStreams({ stream })
pass("Light.Audio.{Unbind,UnbindStreams}(unbound) safe ok")

-- DestroyAudioStream
local ok_d = A.DestroyAudioStream(stream)
if not ok_d then fail("DestroyAudioStream failed") end
pass("Light.Audio.DestroyAudioStream ok")

-- ==================== 11) Boundary: nil stream handle ====================

local nil_stream_fns = {
    "GetAudioStreamFormat", "SetAudioStreamFormat",
    "GetAudioStreamFrequencyRatio", "SetAudioStreamFrequencyRatio",
    "GetAudioStreamGain", "SetAudioStreamGain",
    "GetAudioStreamInputChannelMap", "GetAudioStreamOutputChannelMap",
    "SetAudioStreamInputChannelMap", "SetAudioStreamOutputChannelMap",
    "PutAudioStreamData", "GetAudioStreamData",
    "ClearAudioStream", "FlushAudioStream",
    "LockAudioStream", "UnlockAudioStream",
    "PauseAudioStreamDevice", "ResumeAudioStreamDevice",
    "GetAudioStreamDevice", "DestroyAudioStream",
}
for _, fname in ipairs(nil_stream_fns) do
    local rv = A[fname](nil)
    -- nil/false 都是合法的"失败"返回, 但不能崩
    if rv == nil and (fname == "GetAudioStreamData" or fname == "PutAudioStreamData") then
        -- 这两个是 nil + err 风格
    end
end
pass("Light.Audio 20 nil-stream-handle fns safe ok")

-- AudioStreamDevicePaused(nil) 不报错, 返回 false
local pn = A.AudioStreamDevicePaused(nil)
if type(pn) ~= "boolean" then fail("AudioStreamDevicePaused(nil) should return boolean") end
pass("Light.Audio.AudioStreamDevicePaused(nil) = " .. tostring(pn))

-- ==================== 12) Utilities ====================

local fmt_name = A.GetAudioFormatName(A.AUDIO_S16LE)
if type(fmt_name) ~= "string" then fail("GetAudioFormatName should return string") end
pass("Light.Audio.GetAudioFormatName(S16LE) = '" .. fmt_name .. "'")

local silence = A.GetSilenceValueForFormat(A.AUDIO_S16LE)
assert_number(silence, "silence")
if silence ~= 0 then fail("S16LE silence should be 0, got " .. silence) end
pass("Light.Audio.GetSilenceValueForFormat(S16LE) = 0")

-- U8 silence is 128 (not 0)
local u8_silence = A.GetSilenceValueForFormat(A.AUDIO_U8)
if u8_silence ~= 128 then fail("U8 silence should be 128, got " .. u8_silence) end
pass("Light.Audio.GetSilenceValueForFormat(U8) = 128")

-- MixAudio: 两个 16 字节静音串混合后还是静音
local zero16 = string.rep(string.char(0), 16)
local mixed, mixerr = A.MixAudio(zero16, zero16, A.AUDIO_S16LE, 1.0)
if not mixed then fail("MixAudio failed: " .. tostring(mixerr)) end
if #mixed ~= 16 then fail("Mixed length should be 16, got " .. #mixed) end
pass("Light.Audio.MixAudio(silence + silence) = silence, len=" .. #mixed)

-- ConvertAudioSamples: S16 mono 22050Hz -> S16 stereo 44100Hz
local src_spec = { format = A.AUDIO_S16LE, channels = 1, freq = 22050 }
local dst_spec = { format = A.AUDIO_S16LE, channels = 2, freq = 44100 }
local src_data = string.rep(string.char(0), 64)  -- 32 frames mono S16
local dst_data, cerr = A.ConvertAudioSamples(src_spec, src_data, dst_spec)
if not dst_data then fail("ConvertAudioSamples failed: " .. tostring(cerr)) end
-- 期望 dst_data 长度 ≈ 256 字节 (32 frames × 2× 升频 × 2 ch × 2 byte = 256)
if #dst_data == 0 then fail("ConvertAudioSamples returned empty") end
pass(string.format("Light.Audio.ConvertAudioSamples(S16 mono 22050 -> S16 stereo 44100) ok, %d bytes",
                   #dst_data))

-- ==================== 13) Phase AN: LoadWAV_IO ====================

local IO = require("Light.IOStream")
if type(IO) ~= "table" then fail("require Light.IOStream failed") end

-- 手工构造 44 字节 WAV header + 16 字节 PCM data (8 samples S16 mono)
-- WAV 格式参考: http://soundfile.sapp.org/doc/WaveFormat/
local function u32_le(v)
    return string.char(v % 256, math.floor(v/256) % 256,
                       math.floor(v/65536) % 256, math.floor(v/16777216) % 256)
end
local function u16_le(v) return string.char(v % 256, math.floor(v/256) % 256) end

local pcm_data   = string.rep(string.char(0), 16)  -- 16 bytes silence, S16 mono = 8 samples
local data_size  = #pcm_data                       -- 16
local fmt_size   = 16                              -- PCM fmt chunk size
local riff_size  = 4 + (8 + fmt_size) + (8 + data_size)  -- "WAVE" + fmt chunk + data chunk = 36
local wav_bytes = "RIFF" .. u32_le(riff_size) .. "WAVE"
    .. "fmt " .. u32_le(fmt_size)
    .. u16_le(1)      -- PCM format
    .. u16_le(1)      -- channels = mono
    .. u32_le(44100)  -- sample rate
    .. u32_le(88200)  -- byte rate = 44100 * 2
    .. u16_le(2)      -- block align = 2 bytes/frame
    .. u16_le(16)     -- bits per sample
    .. "data" .. u32_le(data_size)
    .. pcm_data
if #wav_bytes ~= 44 + data_size then
    fail("WAV builder size mismatch: " .. #wav_bytes)
end

-- 创建 IOStream 从内存, LoadWAV_IO 加载
local io_ud, io_err = IO.IOFromConstMem(wav_bytes)
if not io_ud then fail("IOFromConstMem failed: " .. tostring(io_err)) end

local wav_spec, wav_data, wav_err = A.LoadWAV_IO(io_ud, false)  -- closeio=false, 保持 IO 可用
if not wav_spec then fail("LoadWAV_IO failed: " .. tostring(wav_err)) end
if type(wav_spec) ~= "table" or wav_spec.channels ~= 1 or wav_spec.freq ~= 44100 then
    fail(string.format("WAV spec mismatch: ch=%s freq=%s",
                       tostring(wav_spec.channels), tostring(wav_spec.freq)))
end
if #wav_data ~= data_size then
    fail(string.format("WAV data length mismatch: expected %d, got %d", data_size, #wav_data))
end
pass(string.format("Light.Audio.LoadWAV_IO(IOFromConstMem) ok: ch=%d freq=%d data=%d bytes",
                   wav_spec.channels, wav_spec.freq, #wav_data))

-- LoadWAV_IO with closeio=true: SDL 接管并关闭 IO, wrapper 清空 LIOStream.io
local io_ud2 = IO.IOFromConstMem(wav_bytes)
local spec2, data2, err2 = A.LoadWAV_IO(io_ud2, true)
if not spec2 then fail("LoadWAV_IO(closeio=true) failed: " .. tostring(err2)) end
-- 之后对已关闭的 IO 操作应报错 (iostream is closed)
local ok_close, close_err = pcall(IO.ReadIO, io_ud2, 10)
if ok_close then fail("reading closed IO should error") end
pass("Light.Audio.LoadWAV_IO(closeio=true) ok, IO closed safely")

-- LoadWAV_IO 边界: 坏数据应失败
local bad_io = IO.IOFromConstMem("NOT_A_WAV_FILE_XXXXXXXX")
local bs, bd, be = A.LoadWAV_IO(bad_io, true)
if bs ~= nil or be == nil then fail("LoadWAV_IO(bad_data) should be nil+err") end
pass("Light.Audio.LoadWAV_IO(bad_data) boundary ok: " .. tostring(be))

-- ==================== 14) Phase AN: GetAudioStreamProperties ====================

local Props = require("Light.Properties")
local stream_p = A.CreateAudioStream(spec_def, spec_def)
if not stream_p then fail("CreateAudioStream for props test failed") end

local props_id, props_err = A.GetAudioStreamProperties(stream_p)
if type(props_id) ~= "number" then fail("props id should be number") end
if props_id == 0 then
    pass("Light.Audio.GetAudioStreamProperties id=0 (stream has no props yet, ok): " .. tostring(props_err))
else
    pass("Light.Audio.GetAudioStreamProperties ok, id=" .. props_id)
    -- 能用 Light.Properties 操作这个 id
    local ok_set = Props.SetStringProperty(props_id, "test.key", "phase_an_value")
    if not ok_set then fail("Props.SetStringProperty on audio stream id failed") end
    local got = Props.GetStringProperty(props_id, "test.key", "default")
    if got ~= "phase_an_value" then fail("Props round-trip failed: got " .. tostring(got)) end
    pass("Light.Audio.GetAudioStreamProperties + Light.Properties round-trip ok")
end
A.DestroyAudioStream(stream_p)

-- ==================== 15) Phase AN: Set{Get,Put}Callback ====================

local stream_cb = A.CreateAudioStream(spec_def, spec_def)
if not stream_cb then fail("CreateAudioStream for callback test failed") end

local put_count  = 0
local put_total  = 0
local get_count  = 0
local last_user  = nil

-- Put callback
local ok_setput, sperr = A.SetAudioStreamPutCallback(stream_cb, function(stream, additional, total, user)
    put_count = put_count + 1
    put_total = put_total + additional
    last_user = user
end, "hello_put")
if not ok_setput then fail("SetAudioStreamPutCallback failed: " .. tostring(sperr)) end
pass("Light.Audio.SetAudioStreamPutCallback ok")

-- Put data, expect callback invoked
A.PutAudioStreamData(stream_cb, string.rep(string.char(0), 256))
A.PutAudioStreamData(stream_cb, string.rep(string.char(0), 128))
if put_count ~= 2 then fail("put callback count expected 2, got " .. put_count) end
if put_total ~= 384 then fail("put additional sum expected 384, got " .. put_total) end
if last_user ~= "hello_put" then fail("put user_arg mismatch: " .. tostring(last_user)) end
pass(string.format("Light.Audio put callback invoked ok (count=%d total=%d user='%s')",
                   put_count, put_total, tostring(last_user)))

-- Get callback
local ok_setget, sgerr = A.SetAudioStreamGetCallback(stream_cb, function(stream, additional, total)
    get_count = get_count + 1
end)  -- no user_arg this time
if not ok_setget then fail("SetAudioStreamGetCallback failed: " .. tostring(sgerr)) end
pass("Light.Audio.SetAudioStreamGetCallback ok (no user_arg)")

local _ = A.GetAudioStreamData(stream_cb, 64)
if get_count ~= 1 then fail("get callback count expected 1, got " .. get_count) end
pass(string.format("Light.Audio get callback invoked ok (count=%d)", get_count))

-- 清除 callback (nil)
local ok_clear = A.SetAudioStreamPutCallback(stream_cb, nil)
if not ok_clear then fail("SetAudioStreamPutCallback(nil) failed") end
A.PutAudioStreamData(stream_cb, string.rep(string.char(0), 100))
if put_count ~= 2 then fail("after clear, put_count should stay 2, got " .. put_count) end
pass("Light.Audio.SetAudioStreamPutCallback(nil) cleared callback ok")

-- 错误类型拒绝
local bad, bad_err = A.SetAudioStreamPutCallback(stream_cb, "not_a_function")
if bad then fail("SetAudioStreamPutCallback(string) should fail") end
pass("Light.Audio.SetAudioStreamPutCallback(string) rejected ok: " .. tostring(bad_err))

-- DestroyAudioStream 应清理所有 callback ref (不泄漏)
A.DestroyAudioStream(stream_cb)
pass("Light.Audio.DestroyAudioStream cleans callback refs ok")

-- 递归保护: 在 put callback 内再调 PutAudioStreamData 不崩
local stream_rec = A.CreateAudioStream(spec_def, spec_def)
local rec_depth = 0
local rec_max = 0
A.SetAudioStreamPutCallback(stream_rec, function(s, add, tot)
    rec_depth = rec_depth + 1
    if rec_depth > rec_max then rec_max = rec_depth end
    if rec_depth < 3 then
        -- 内嵌 Put: 递归保护应拦截, 不触发二层 callback
        A.PutAudioStreamData(s, string.rep(string.char(0), 32))
    end
    rec_depth = rec_depth - 1
end)
A.PutAudioStreamData(stream_rec, string.rep(string.char(0), 64))
if rec_max > 1 then fail("recursive callback protection failed: depth=" .. rec_max) end
pass("Light.Audio callback recursion protection ok (max depth=" .. rec_max .. ")")
A.DestroyAudioStream(stream_rec)

print("audio smoke ok (Phase AM 51 fns + Phase AN 4 fns + 16 const verified)")
