-- Phase O smoke: Light.Time (SDL3 timer + realtime clock + date/time)
--
-- ASCII-only per Windows CI convention.

local function pass(msg) print(msg) end
-- Use error() rather than os.exit so the LSP recognizes fail() as
-- noreturn (avoids tons of "needs nil check" warnings after pcall-style
-- guards). Unhandled lua error still propagates to light.exe -> exit 1.
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Time")
if not ok then fail("require(Light.Time) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Time not a table") end

-- 1) 14 fns
for _, k in ipairs({
    "GetTicks", "GetTicksNS", "GetPerformanceCounter", "GetPerformanceFrequency",
    "Delay", "DelayNS", "DelayPrecise",
    "GetCurrentTime", "GetDateTimeLocalePreferences",
    "TimeToDateTime", "DateTimeToTime",
    "GetDaysInMonth", "GetDayOfYear", "GetDayOfWeek",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Time." .. k .. " missing") end
end
pass("Light.Time module ok (14 functions)")

-- 2) 5 constants
assert(mod.DATE_FORMAT_YYYYMMDD == 0)
assert(mod.DATE_FORMAT_DDMMYYYY == 1)
assert(mod.DATE_FORMAT_MMDDYYYY == 2)
assert(mod.TIME_FORMAT_24HR     == 0)
assert(mod.TIME_FORMAT_12HR     == 1)
pass("Light.Time constants ok (3 date + 2 time)")

-- 3) tick counters monotonic non-negative
local t0 = mod.GetTicks()
local t0_ns = mod.GetTicksNS()
local pc0 = mod.GetPerformanceCounter()
local pf  = mod.GetPerformanceFrequency()
assert(type(t0) == "number" and t0 >= 0, "GetTicks() must be non-negative number")
assert(type(t0_ns) == "number" and t0_ns >= 0, "GetTicksNS() must be non-negative number")
assert(type(pc0) == "number" and pc0 >= 0, "PerformanceCounter must be non-negative")
assert(type(pf) == "number" and pf > 0, "PerformanceFrequency must be > 0")
pass(string.format("ticks=%d ticks_ns=%d perf_counter=%d perf_freq=%d",
                   t0, t0_ns, pc0, pf))

-- 4) Delay 1ms; ticks must move forward
mod.Delay(1)
local t1 = mod.GetTicks()
local pc1 = mod.GetPerformanceCounter()
assert(t1 >= t0, "ticks must be monotonic non-decreasing across Delay(1)")
assert(pc1 > pc0, "perf counter must advance across Delay(1)")
pass(string.format("Delay(1) ok: dt_ms=%d dpc=%d", t1 - t0, pc1 - pc0))

-- DelayNS / DelayPrecise smoke (1us each)
mod.DelayNS(1000)
mod.DelayPrecise(1000)
pass("DelayNS / DelayPrecise (1us each) ok")

-- Negative delay -> clamped to 0, no error
mod.Delay(-5)
pass("Delay(-5) clamp ok")

-- 5) GetCurrentTime + TimeToDateTime + DateTimeToTime round trip
local now, err = mod.GetCurrentTime()
if now == nil then fail("GetCurrentTime failed: " .. tostring(err)) end
assert(now, "unreachable")
assert(type(now) == "number" and now > 0, "current time must be positive ns")

local dt_utc, e2 = mod.TimeToDateTime(now, false)
if dt_utc == nil then fail("TimeToDateTime(utc) failed: " .. tostring(e2)) end
assert(dt_utc, "unreachable")
assert(dt_utc.year >= 2024 and dt_utc.year <= 2200, "year sane: " .. tostring(dt_utc.year))
assert(dt_utc.month >= 1 and dt_utc.month <= 12, "month range")
assert(dt_utc.day >= 1 and dt_utc.day <= 31, "day range")
assert(dt_utc.hour >= 0 and dt_utc.hour <= 23, "hour range")
assert(dt_utc.minute >= 0 and dt_utc.minute <= 59, "minute range")
assert(dt_utc.second >= 0 and dt_utc.second <= 60, "second range (leap second tolerated)")
assert(dt_utc.day_of_week >= 0 and dt_utc.day_of_week <= 6, "day_of_week 0..6")
pass(string.format("TimeToDateTime(utc) ok: %04d-%02d-%02d %02d:%02d:%02d (dow=%d)",
                   dt_utc.year, dt_utc.month, dt_utc.day,
                   dt_utc.hour, dt_utc.minute, dt_utc.second, dt_utc.day_of_week))

local dt_local, e3 = mod.TimeToDateTime(now, true)
if dt_local == nil then fail("TimeToDateTime(local) failed: " .. tostring(e3)) end
assert(dt_local, "unreachable")
pass(string.format("TimeToDateTime(local) ok: utc_offset=%d", dt_local.utc_offset))

-- Round-trip back to ticks - allow up to 1s of rounding due to nanosecond
-- field default of 0 when re-injected; we set it explicitly so round trip
-- should be exact (within whole-ns precision lost via double).
local rt_ticks, e4 = mod.DateTimeToTime(dt_utc)
if rt_ticks == nil then fail("DateTimeToTime failed: " .. tostring(e4)) end
assert(rt_ticks, "unreachable")
local diff = math.abs(rt_ticks - now)
-- 53-bit double can lose ~64 ns at "now" ticks (~10^18 ns); allow 1us slack.
assert(diff < 1e9, "DateTimeToTime round-trip drift > 1s: " .. tostring(diff))
pass(string.format("DateTime round-trip drift = %d ns (within 1s)", diff))

-- 6) DateTimeToTime against a known epoch (1970-01-01T00:00:00 UTC) -> 0
local epoch_dt = {
    year=1970, month=1, day=1, hour=0, minute=0, second=0,
    nanosecond=0, day_of_week=4, utc_offset=0,
}
local epoch_t, e5 = mod.DateTimeToTime(epoch_dt)
if epoch_t == nil then fail("DateTimeToTime(epoch) failed: " .. tostring(e5)) end
assert(epoch_t, "unreachable")
assert(epoch_t == 0, "epoch must be 0 ns, got " .. tostring(epoch_t))
pass("DateTimeToTime(epoch UTC) == 0 ok")

-- Invalid dt: missing required field
local bad_t, bad_e = mod.DateTimeToTime({hour=12})
if bad_t ~= nil or bad_e == nil then fail("DateTimeToTime({hour=12}) should fail") end
pass("DateTimeToTime missing-field boundary ok: " .. tostring(bad_e))

-- Non-table dt
local nt_t, nt_e = mod.DateTimeToTime("not a table")
if nt_t ~= nil or nt_e == nil then fail("DateTimeToTime(string) should fail") end
pass("DateTimeToTime(string) boundary ok: " .. tostring(nt_e))

-- 7) Calendar helpers - well-known values
assert(mod.GetDaysInMonth(2024, 2)  == 29, "2024-02 leap year: 29 days")
assert(mod.GetDaysInMonth(2025, 2)  == 28, "2025-02 non-leap: 28 days")
assert(mod.GetDaysInMonth(2025, 1)  == 31, "2025-01: 31 days")
assert(mod.GetDaysInMonth(2025, 4)  == 30, "2025-04: 30 days")
assert(mod.GetDaysInMonth(2000, 2)  == 29, "2000-02 century leap: 29 days")
assert(mod.GetDaysInMonth(1900, 2)  == 28, "1900-02 century non-leap: 28 days")
pass("GetDaysInMonth leap-year cases ok")

-- SDL3 SDL_GetDayOfYear is 0-indexed: "Day 0 is the first day of the year."
-- Source: ChocoLight/build/_deps/sdl3-src/src/time/SDL_time.c:49
assert(mod.GetDayOfYear(2025, 1, 1)  == 0,   "Jan 1 must be doy 0 (0-indexed)")
assert(mod.GetDayOfYear(2025, 12, 31)== 364, "Dec 31 non-leap must be doy 364")
assert(mod.GetDayOfYear(2024, 12, 31)== 365, "Dec 31 leap year must be doy 365")
pass("GetDayOfYear ok (0-indexed per SDL3)")

assert(mod.GetDayOfWeek(2025, 12, 25) == 4) -- 2025-12-25 is Thursday
assert(mod.GetDayOfWeek(1970, 1, 1)   == 4) -- 1970-01-01 is Thursday
pass("GetDayOfWeek ok")

-- 8) GetDateTimeLocalePreferences returns table or (nil, err)
local prefs, perr = mod.GetDateTimeLocalePreferences()
if prefs == nil then
    -- some platforms (CI Windows session-0) may not have a locale - accept err
    pass("GetDateTimeLocalePreferences not available on this host: " .. tostring(perr))
else
    assert(type(prefs) == "table", "prefs must be table")
    assert(type(prefs.date_format) == "number", "date_format must be number")
    assert(type(prefs.time_format) == "number", "time_format must be number")
    assert(prefs.date_format >= 0 and prefs.date_format <= 2, "date_format in 0..2")
    assert(prefs.time_format >= 0 and prefs.time_format <= 1, "time_format in 0..1")
    pass(string.format("GetDateTimeLocalePreferences ok: date=%d time=%d",
                       prefs.date_format, prefs.time_format))
end

print("time smoke ok")
