/**
 * @file light_time.cpp
 * @brief Light.Time module - SDL3 timer + realtime clock + date/time
 *
 * Lua API:
 *
 *  Tick / performance counters (SDL_timer.h):
 *    Light.Time.GetTicks()                    -> ms (number)
 *    Light.Time.GetTicksNS()                  -> ns (number, may lose
 *                                                precision after ~104 days)
 *    Light.Time.GetPerformanceCounter()       -> hi-res counter (number)
 *    Light.Time.GetPerformanceFrequency()     -> counts per second (number)
 *
 *  Sleep helpers (use sparingly on CI; smoke uses 1ms):
 *    Light.Time.Delay(ms)                     -> (void)
 *    Light.Time.DelayNS(ns)                   -> (void)
 *    Light.Time.DelayPrecise(ns)              -> (void)
 *
 *  Realtime clock (SDL_time.h):
 *    Light.Time.GetCurrentTime()              -> ns_since_epoch | nil, err
 *    Light.Time.GetDateTimeLocalePreferences()-> {date_format, time_format}
 *    Light.Time.TimeToDateTime(ticks, local?) -> {year, month, day, hour,
 *                                                  minute, second,
 *                                                  nanosecond, day_of_week,
 *                                                  utc_offset} | nil, err
 *    Light.Time.DateTimeToTime(dt)            -> ticks | nil, err
 *
 *  Calendar helpers (cheap stateless math):
 *    Light.Time.GetDaysInMonth(year, month)   -> int
 *    Light.Time.GetDayOfYear(y, m, d)         -> int
 *    Light.Time.GetDayOfWeek(y, m, d)         -> int (0=Sunday)
 *
 * Constants:
 *    DATE_FORMAT_YYYYMMDD = 0
 *    DATE_FORMAT_DDMMYYYY = 1
 *    DATE_FORMAT_MMDDYYYY = 2
 *    TIME_FORMAT_24HR     = 0
 *    TIME_FORMAT_12HR     = 1
 *
 * Phase AR additions:
 *    Light.Time.AddTimer(ms, fn) -> id (one-shot timer; return 0 on failure)
 *    Light.Time.RemoveTimer(id) -> bool
 *      Implementation: SDL_AddTimer fires on SDL's internal thread, then
 *      SDL_PushEvent forwards to the main loop where Time_OnTimerEvent runs
 *      the Lua callback safely. Timers are one-shot; for repeated callbacks,
 *      the Lua side wraps with another AddTimer call (avoids re-entrancy and
 *      cross-thread state churn).
 *
 * Not bound (intentional):
 *    SDL_TimeToWindows / SDL_TimeFromWindows
 *      - Windows-specific FILETIME interop; out of cross-platform scope.
 *
 * No SDL_Init dependency: SDL_GetTicks / SDL_GetCurrentTime / calendar
 * helpers are all process-wide and safe pre-Init.
 *
 * Numeric range: lua_Number is double (53-bit precision). Uint64 ticks /
 * SDL_Time exceed this range only after ~285 years from epoch, so storing
 * ns_since_epoch as a double is safe for any plausible real-world clock.
 */
#include "light.h"
#include "light_time.h"   // Phase H.0 — LT::TickRender namespace

#include <SDL3/SDL.h>
#include <unordered_map>
#include <mutex>
#include <vector>         // PhysicsRegistry (Phase H.0)
#include <algorithm>      // std::clamp (Phase H.0)
#include <cmath>          // std::lround (Phase H.0)

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Phase AR — Timer infrastructure
// =========================================================
// SDL 内部线程 → PushEvent → 主线程 dispatch → Lua callback
// 仅允许主线程访问 g_timerCallbackRef; SDL 回调仅读取 timer_id 不访问表。
static Uint32                                  g_timerEventType = 0;     ///< SDL_RegisterEvents 返回的事件类型 ID, 0=未初始化
static int                                     g_nextTimerId    = 1;     ///< Lua 层稳定 timer_id
static std::unordered_map<int, int>            g_timerCallbackRef;       ///< timer_id → Lua registry ref
static std::unordered_map<int, SDL_TimerID>    g_timerSdlMap;            ///< timer_id → SDL_TimerID (只在主线程访问)
static std::mutex                              g_timerMutex;             ///< 仅保护 g_nextTimerId 递增

/// SDL_AddTimer 回调 — 在 SDL 内部线程触发, 不能调 lua_*
/// 仅推送一个 Timer 事件到队列, 让主线程 dispatch 时调 Lua
static Uint32 SDLCALL TimerCallback(void* userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)timerID; (void)interval;
    int timer_id = (int)(intptr_t)userdata;

    // 推一个 SDL_USEREVENT (类型 = g_timerEventType), code = timer_id
    SDL_Event ev;
    SDL_zero(ev);
    ev.user.type      = g_timerEventType;
    ev.user.timestamp = SDL_GetTicksNS();
    ev.user.code      = timer_id;
    ev.user.data1     = nullptr;
    ev.user.data2     = nullptr;
    SDL_PushEvent(&ev);

    return 0;  // 返回 0 = 不重复 (单次计时器)
}

/// 主线程处理 Timer 事件 — 由 light_ui.cpp DispatchEvents 调用
extern "C" LIGHT_API void Time_OnTimerEvent(lua_State* L, int timer_id) {
    if (!L) return;

    auto it = g_timerCallbackRef.find(timer_id);
    if (it == g_timerCallbackRef.end()) return;  // 已被 RemoveTimer 清理

    int ref = it->second;

    // 取出 Lua callback 并调用 (单次触发, 调后释放 ref)
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "Timer[%d] callback: %s", timer_id, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // 清理 ref 和映射
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    g_timerCallbackRef.erase(timer_id);
    g_timerSdlMap.erase(timer_id);
}

/// 初始化 Timer 事件类型 (在 luaopen_Light_Time 中调用, 只注册一次)
static void EnsureTimerEventTypeRegistered() {
    if (g_timerEventType == 0) {
        Uint32 ev = SDL_RegisterEvents(1);
        if (ev != (Uint32)-1) {
            g_timerEventType = ev;
        }
    }
}

/// 导出给 platform_window_sdl3.cpp 查询: 如果返回事件类型匹配, 则应该转为 Event::Timer
extern "C" LIGHT_API Uint32 Time_GetTimerEventType() {
    return g_timerEventType;
}

// ===========================================================
// Tick / performance counters
// ===========================================================
static int l_Time_GetTicks(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetTicks());
    return 1;
}
static int l_Time_GetTicksNS(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetTicksNS());
    return 1;
}
static int l_Time_GetPerformanceCounter(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetPerformanceCounter());
    return 1;
}
static int l_Time_GetPerformanceFrequency(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetPerformanceFrequency());
    return 1;
}

// ===========================================================
// Sleep helpers
// ===========================================================
static int l_Time_Delay(lua_State* L) {
    lua_Number ms = luaL_checknumber(L, 1);
    if (ms < 0) ms = 0;
    SDL_Delay((Uint32)ms);
    return 0;
}
static int l_Time_DelayNS(lua_State* L) {
    lua_Number ns = luaL_checknumber(L, 1);
    if (ns < 0) ns = 0;
    SDL_DelayNS((Uint64)ns);
    return 0;
}
static int l_Time_DelayPrecise(lua_State* L) {
    lua_Number ns = luaL_checknumber(L, 1);
    if (ns < 0) ns = 0;
    SDL_DelayPrecise((Uint64)ns);
    return 0;
}

// ===========================================================
// Realtime clock
// ===========================================================
static int l_Time_GetCurrentTime(lua_State* L) {
    SDL_Time t = 0;
    if (!SDL_GetCurrentTime(&t)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetCurrentTime failed");
        return 2;
    }
    // SDL_Time is Sint64 ns since Unix epoch
    lua_pushnumber(L, (lua_Number)t);
    return 1;
}

static int l_Time_GetDateTimeLocalePreferences(lua_State* L) {
    SDL_DateFormat df = SDL_DATE_FORMAT_YYYYMMDD;
    SDL_TimeFormat tf = SDL_TIME_FORMAT_24HR;
    if (!SDL_GetDateTimeLocalePreferences(&df, &tf)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetDateTimeLocalePreferences failed");
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)df);
    lua_setfield(L, -2, "date_format");
    lua_pushinteger(L, (lua_Integer)tf);
    lua_setfield(L, -2, "time_format");
    return 1;
}

// Push SDL_DateTime as Lua table.
static void PushDateTime(lua_State* L, const SDL_DateTime& dt) {
    lua_newtable(L);
    lua_pushinteger(L, dt.year);        lua_setfield(L, -2, "year");
    lua_pushinteger(L, dt.month);       lua_setfield(L, -2, "month");
    lua_pushinteger(L, dt.day);         lua_setfield(L, -2, "day");
    lua_pushinteger(L, dt.hour);        lua_setfield(L, -2, "hour");
    lua_pushinteger(L, dt.minute);      lua_setfield(L, -2, "minute");
    lua_pushinteger(L, dt.second);      lua_setfield(L, -2, "second");
    lua_pushinteger(L, dt.nanosecond);  lua_setfield(L, -2, "nanosecond");
    lua_pushinteger(L, dt.day_of_week); lua_setfield(L, -2, "day_of_week");
    lua_pushinteger(L, dt.utc_offset);  lua_setfield(L, -2, "utc_offset");
}

// Read SDL_DateTime from Lua table at idx, with sane defaults.
// Returns false + leaves err on stack if a required field is missing/bad.
static bool ReadDateTime(lua_State* L, int idx, SDL_DateTime& out, const char*& err) {
    err = nullptr;
    if (lua_type(L, idx) != LUA_TTABLE) {
        err = "dt must be a table";
        return false;
    }

    auto getInt = [&](const char* k, int def, bool required) -> int {
        lua_getfield(L, idx, k);
        int v = def;
        int t = lua_type(L, -1);
        if (t == LUA_TNUMBER) {
            v = (int)lua_tointeger(L, -1);
        } else if (t != LUA_TNIL) {
            err = k;  // signal type error via field name
        } else if (required && t == LUA_TNIL) {
            err = k;
        }
        lua_pop(L, 1);
        return v;
    };

    out.year       = getInt("year",        1970, true);
    if (err) return false;
    out.month      = getInt("month",       1,    true);
    if (err) return false;
    out.day        = getInt("day",         1,    true);
    if (err) return false;
    out.hour       = getInt("hour",        0,    false);
    if (err) return false;
    out.minute     = getInt("minute",      0,    false);
    if (err) return false;
    out.second     = getInt("second",      0,    false);
    if (err) return false;
    out.nanosecond = getInt("nanosecond",  0,    false);
    if (err) return false;
    out.day_of_week= getInt("day_of_week", 0,    false);
    if (err) return false;
    out.utc_offset = getInt("utc_offset",  0,    false);
    if (err) return false;
    return true;
}

static int l_Time_TimeToDateTime(lua_State* L) {
    SDL_Time ticks = (SDL_Time)luaL_checknumber(L, 1);
    bool localTime = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        localTime = lua_toboolean(L, 2) != 0;
    }
    SDL_DateTime dt;
    if (!SDL_TimeToDateTime(ticks, &dt, localTime)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_TimeToDateTime failed");
        return 2;
    }
    PushDateTime(L, dt);
    return 1;
}

static int l_Time_DateTimeToTime(lua_State* L) {
    SDL_DateTime dt;
    const char* err = nullptr;
    if (!ReadDateTime(L, 1, dt, err)) {
        lua_pushnil(L);
        char msg[128];
        SDL_snprintf(msg, sizeof(msg), "invalid dt field: %s", err ? err : "?");
        lua_pushstring(L, msg);
        return 2;
    }
    SDL_Time t = 0;
    if (!SDL_DateTimeToTime(&dt, &t)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_DateTimeToTime failed");
        return 2;
    }
    lua_pushnumber(L, (lua_Number)t);
    return 1;
}

// ===========================================================
// Calendar helpers
// ===========================================================
static int l_Time_GetDaysInMonth(lua_State* L) {
    int year = (int)luaL_checkinteger(L, 1);
    int month = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, SDL_GetDaysInMonth(year, month));
    return 1;
}
static int l_Time_GetDayOfYear(lua_State* L) {
    int y = (int)luaL_checkinteger(L, 1);
    int m = (int)luaL_checkinteger(L, 2);
    int d = (int)luaL_checkinteger(L, 3);
    lua_pushinteger(L, SDL_GetDayOfYear(y, m, d));
    return 1;
}
static int l_Time_GetDayOfWeek(lua_State* L) {
    int y = (int)luaL_checkinteger(L, 1);
    int m = (int)luaL_checkinteger(L, 2);
    int d = (int)luaL_checkinteger(L, 3);
    lua_pushinteger(L, SDL_GetDayOfWeek(y, m, d));
    return 1;
}

// ===========================================================
// Phase AR — AddTimer / RemoveTimer
// ===========================================================
// Lua: AddTimer(ms, fn) -> id
//   ms: 延迟毫秒 (>=0)
//   fn: function() callback (单次触发)
// 返回: int id (>0 表示成功, 0 表示失败)
static int l_Time_AddTimer(lua_State* L) {
    Uint32 ms = (Uint32)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    EnsureTimerEventTypeRegistered();
    if (g_timerEventType == 0) {
        // 事件类型注册失败 (极端情况)
        lua_pushinteger(L, 0);
        return 1;
    }

    // 分配稳定 Lua 层 timer_id
    int timer_id;
    {
        std::lock_guard<std::mutex> lock(g_timerMutex);
        timer_id = g_nextTimerId++;
    }

    // 存储 callback ref (callback 在栈顶, 都会 luaL_ref 弹出)
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_timerCallbackRef[timer_id] = ref;

    // 启动 SDL 定时器
    SDL_TimerID sdl_id = SDL_AddTimer(ms, TimerCallback, (void*)(intptr_t)timer_id);
    if (sdl_id == 0) {
        // 启动失败, 清理
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        g_timerCallbackRef.erase(timer_id);
        lua_pushinteger(L, 0);
        return 1;
    }
    g_timerSdlMap[timer_id] = sdl_id;

    lua_pushinteger(L, timer_id);
    return 1;
}

// Lua: RemoveTimer(id) -> bool
static int l_Time_RemoveTimer(lua_State* L) {
    int timer_id = (int)luaL_checkinteger(L, 1);

    auto sdl_it = g_timerSdlMap.find(timer_id);
    if (sdl_it == g_timerSdlMap.end()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    bool ok = SDL_RemoveTimer(sdl_it->second);
    g_timerSdlMap.erase(sdl_it);

    auto ref_it = g_timerCallbackRef.find(timer_id);
    if (ref_it != g_timerCallbackRef.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref_it->second);
        g_timerCallbackRef.erase(ref_it);
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ============================================================================
// Phase H.0 — Tick-Render 解耦 — LT::TickRender 实现 + Lua wrapper
//
// 设计:
//   主循环每帧累积 wall-clock; 累积器超过 fixedDt 时触发若干次 fixed step.
//   状态全局单例 (主循环只有一个), 非线程安全 (Lua VM 单线程).
//
// 详见 docs/Phase H.0 Tick-Render Decouple/DESIGN_PhaseH_0.md
// 接口声明: ChocoLight/include/light_time.h
// ============================================================================

namespace LT {
namespace TickRender {

namespace {  // 文件局部 — 不暴露状态

struct State {
    // 配置 (用户可改)
    double fixedDt              = kDefaultFixedDt;
    int    maxFixedStepsPerFrame = kDefaultMaxFixedStepsPerFrame;
    double frameTimeClamp       = kDefaultFrameTimeClamp;

    // 运行时状态 (BeginFrame / ConsumeFixedStep / FinalizeFrame 维护)
    double accumulator          = 0.0;
    double lastTime             = 0.0;     // 0 = 未初始化, 首帧用 lastFrameTime=0 处理
    double lastFrameTime        = 0.0;     // 上一帧 frameTime (供 GetLastFrameTime)
    double alpha                = 0.0;     // [0, 1)
    int    lastFixedStepCount   = 0;

    // 诊断 (节流 spiral log)
    bool   warnedSpiralLastFrame = false;
};

// 全局单例; 多窗口未来扩展可改 thread_local, 但当前主循环只有一个
State g_state;

// ----- 内部 helper -----

// 用 SDL_GetPerformanceCounter 取单调高精度 wall-clock (秒)
// 与 PlatformWindow::GetTime() 等价 (后者也是 SDL_GetPerformanceCounter 实现)
double NowSeconds() {
    static Uint64 s_freq  = SDL_GetPerformanceFrequency();
    static Uint64 s_start = SDL_GetPerformanceCounter();
    Uint64 now = SDL_GetPerformanceCounter();
    return (double)(now - s_start) / (double)s_freq;
}

// log 节流: 在状态切换边界 (off→on / on→off) 打一次
void LogSpiralEdge(bool nowSpiraling) {
    if (nowSpiraling && !g_state.warnedSpiralLastFrame) {
        CC::Log(CC::LOG_WARN,
            "TickRender: spiral guard hit (frameTime=%.3fs, %d fixed steps capped at maxStep=%d, "
            "consider reducing fixedHz or maxStep)",
            g_state.lastFrameTime, g_state.lastFixedStepCount, g_state.maxFixedStepsPerFrame);
        g_state.warnedSpiralLastFrame = true;
    } else if (!nowSpiraling && g_state.warnedSpiralLastFrame) {
        CC::Log(CC::LOG_INFO, "TickRender: spiral guard recovered");
        g_state.warnedSpiralLastFrame = false;
    }
}

}  // anonymous namespace

// ============================================================================
// 主循环驱动接口
// ============================================================================

void Init() {
    g_state = State{};   // 全部默认值 (含 lastTime=0 → 首帧 frameTime=0)
}

void Shutdown() {
    g_state = State{};
}

void BeginFrame() {
    double now = NowSeconds();
    double frameTime = (g_state.lastTime == 0.0) ? 0.0 : (now - g_state.lastTime);
    g_state.lastTime = now;

    // spiral guard: 单帧 frameTime 上限 (防 alt-tab/debug pause 后回来累积器爆炸)
    if (frameTime > g_state.frameTimeClamp) frameTime = g_state.frameTimeClamp;
    if (frameTime < 0.0) frameTime = 0.0;   // 防御 (单调时钟下不应发生)

    g_state.lastFrameTime    = frameTime;
    g_state.accumulator     += frameTime;
    g_state.lastFixedStepCount = 0;
}

bool ShouldStepFixed() {
    return (g_state.accumulator >= g_state.fixedDt) &&
           (g_state.lastFixedStepCount < g_state.maxFixedStepsPerFrame);
}

void ConsumeFixedStep() {
    g_state.accumulator -= g_state.fixedDt;
    if (g_state.accumulator < 0.0) g_state.accumulator = 0.0;   // 防御 (浮点累积)
    g_state.lastFixedStepCount++;
}

void FinalizeFrame() {
    // 触顶时累积器仍可能超过 fixedDt → 强制截顶 (防止下帧再 spiral)
    bool spiraled = (g_state.lastFixedStepCount >= g_state.maxFixedStepsPerFrame) &&
                    (g_state.accumulator >= g_state.fixedDt);
    LogSpiralEdge(spiraled);

    // 累积器最大值 = fixedDt * factor (默认 4)
    double accMax = g_state.fixedDt * kAccumulatorMaxFactor;
    if (g_state.accumulator > accMax) g_state.accumulator = accMax;

    // alpha = accumulator / fixedDt; 一般 ∈ [0, 1) 但若触顶可能 < kAccumulatorMaxFactor
    g_state.alpha = g_state.accumulator / g_state.fixedDt;
    // 用户语义期待 alpha ∈ [0, 1) → 截顶 0.999..
    if (g_state.alpha >= 1.0) g_state.alpha = 1.0 - 1e-9;
    if (g_state.alpha < 0.0)  g_state.alpha = 0.0;
}

// ============================================================================
// 查询 / 配置
// ============================================================================

double GetFixedDt()                  { return g_state.fixedDt; }
int    GetFixedHz()                  { return (int)std::lround(1.0 / g_state.fixedDt); }
int    GetMaxFixedStepsPerFrame()    { return g_state.maxFixedStepsPerFrame; }
double GetFrameTimeClamp()           { return g_state.frameTimeClamp; }
double GetAlpha()                    { return g_state.alpha; }
double GetAccumulator()              { return g_state.accumulator; }
int    GetLastStepCount()            { return g_state.lastFixedStepCount; }
double GetLastFrameTime()            { return g_state.lastFrameTime; }

void SetFixedHz(int hz) {
    int clamped = std::clamp(hz, kFixedHzMin, kFixedHzMax);
    if (clamped != hz) {
        CC::Log(CC::LOG_WARN, "TickRender: fixedHz=%d clamped to %d (range [%d, %d])",
                hz, clamped, kFixedHzMin, kFixedHzMax);
    }
    g_state.fixedDt = 1.0 / (double)clamped;
}

void SetMaxFixedStepsPerFrame(int n) {
    int clamped = std::clamp(n, kMaxStepMin, kMaxStepMax);
    if (clamped != n) {
        CC::Log(CC::LOG_WARN, "TickRender: maxFixedStepsPerFrame=%d clamped to %d (range [%d, %d])",
                n, clamped, kMaxStepMin, kMaxStepMax);
    }
    g_state.maxFixedStepsPerFrame = clamped;
}

void SetFrameTimeClamp(double s) {
    double clamped = std::clamp(s, kFrameClampMin, kFrameClampMax);
    if (clamped != s) {
        CC::Log(CC::LOG_WARN, "TickRender: frameTimeClamp=%.4fs clamped to %.4fs (range [%.2f, %.2f])",
                s, clamped, kFrameClampMin, kFrameClampMax);
    }
    g_state.frameTimeClamp = clamped;
}

}  // namespace TickRender

// ============================================================================
// Phase H.0 — PhysicsRegistry 实现 (T4A/T4B 物理 auto-step 基础设施)
// ============================================================================

namespace PhysicsRegistry {

namespace {

struct Entry {
    void*       world;          // 类型擦除指针 (b2World* 或 btDynamicsWorld* 包装类型)
    StepFn      stepFn;         // 物理子系统注入的回调
    bool        autoStep;       // 默认 false; 用户 SetAutoStep(true) 启用
};

// 用 vector 保持注册顺序 (FIFO Step); World 数量通常 ≤ 5 → linear scan 足够
// 若未来 World 数 > 100 可改 unordered_map<void*, Entry>.
std::vector<Entry> g_worlds;

// 寻指针: 返 -1 = 未找到
int FindIndex_(void* w) {
    for (size_t i = 0; i < g_worlds.size(); ++i) {
        if (g_worlds[i].world == w) return (int)i;
    }
    return -1;
}

}  // anonymous namespace

void RegisterWorld(void* world, StepFn stepFn) {
    if (!world || !stepFn) return;
    int idx = FindIndex_(world);
    if (idx >= 0) {
        // 重复注册 → 更新 stepFn (autoStep 保留)
        g_worlds[idx].stepFn = stepFn;
        return;
    }
    g_worlds.push_back(Entry{world, stepFn, false});
}

void UnregisterWorld(void* world) {
    int idx = FindIndex_(world);
    if (idx < 0) return;
    g_worlds.erase(g_worlds.begin() + idx);
}

void SetAutoStep(void* world, bool on) {
    int idx = FindIndex_(world);
    if (idx < 0) return;
    g_worlds[idx].autoStep = on;
}

bool GetAutoStep(void* world) {
    int idx = FindIndex_(world);
    return (idx >= 0) && g_worlds[idx].autoStep;
}

void StepAllAuto(double dt) {
    // 早退 (热路径优化): 列表为空时零开销
    if (g_worlds.empty()) return;
    // 注意: 用户在 stepFn 内不可调 RegisterWorld / UnregisterWorld
    //   (会 invalidate iterator). 当前调用链 stepFn → Box2D/Bullet step → user callback,
    //   user callback 不应注册 world (业务约束).
    for (size_t i = 0; i < g_worlds.size(); ++i) {
        if (g_worlds[i].autoStep && g_worlds[i].stepFn) {
            g_worlds[i].stepFn(g_worlds[i].world, dt);
        }
    }
}

int GetWorldCount() {
    return (int)g_worlds.size();
}

int GetAutoStepWorldCount() {
    int n = 0;
    for (const auto& e : g_worlds) if (e.autoStep) ++n;
    return n;
}

}  // namespace PhysicsRegistry

}  // namespace LT

// ============================================================================
// Phase H.0 — extern "C" 桥接 (供 light_ui.cpp 主循环调用)
// ============================================================================

// 主循环每个 fixed step 调一次. 等同 LT::PhysicsRegistry::StepAllAuto(dt).
// extern "C" 避免 C++ name mangling, 简化 light_ui 与 light_time 跨 translation
// unit 调用 (无需 #include 完整 header 也能链接成功 — 但本项目仍 include).
extern "C" void Light_PhysicsAutoStepAll(double dt) {
    LT::PhysicsRegistry::StepAllAuto(dt);
}

// ============================================================================
// Phase H.0 — Lua wrapper (Light.Time.SetFixedTimestep / GetAlpha / ...)
// 11 fn 注册到 kTimeReg[] (与 Phase AR 现有 Light.Time fn 共存于同一 Lua 表)
// ============================================================================

namespace {

// 用 luaL_checknumber 接收 Lua number; 越界由 LT::TickRender::SetXxx 内部 clamp
// (友好降级 — 不 raise; 避免破坏热路径)

int l_Time_SetFixedTimestep(lua_State* L) {
    lua_Number hz = luaL_checknumber(L, 1);
    LT::TickRender::SetFixedHz((int)hz);
    return 0;
}

int l_Time_GetFixedTimestep(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)LT::TickRender::GetFixedHz());
    return 1;
}

int l_Time_GetFixedDt(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LT::TickRender::GetFixedDt());
    return 1;
}

int l_Time_SetMaxFixedStepsPerFrame(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    LT::TickRender::SetMaxFixedStepsPerFrame((int)n);
    return 0;
}

int l_Time_GetMaxFixedStepsPerFrame(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)LT::TickRender::GetMaxFixedStepsPerFrame());
    return 1;
}

int l_Time_SetFrameTimeClamp(lua_State* L) {
    lua_Number s = luaL_checknumber(L, 1);
    LT::TickRender::SetFrameTimeClamp((double)s);
    return 0;
}

int l_Time_GetFrameTimeClamp(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LT::TickRender::GetFrameTimeClamp());
    return 1;
}

int l_Time_GetAlpha(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LT::TickRender::GetAlpha());
    return 1;
}

int l_Time_GetAccumulator(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LT::TickRender::GetAccumulator());
    return 1;
}

int l_Time_GetLastStepCount(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)LT::TickRender::GetLastStepCount());
    return 1;
}

int l_Time_GetLastFrameTime(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LT::TickRender::GetLastFrameTime());
    return 1;
}

}  // anonymous namespace

// ===========================================================
// luaopen_Light_Time
// ===========================================================
static const luaL_Reg kTimeReg[] = {
    { "GetTicks",                    l_Time_GetTicks                    },
    { "GetTicksNS",                  l_Time_GetTicksNS                  },
    { "GetPerformanceCounter",       l_Time_GetPerformanceCounter       },
    { "GetPerformanceFrequency",     l_Time_GetPerformanceFrequency     },
    { "Delay",                       l_Time_Delay                       },
    { "DelayNS",                     l_Time_DelayNS                     },
    { "DelayPrecise",                l_Time_DelayPrecise                },
    { "GetCurrentTime",              l_Time_GetCurrentTime              },
    { "GetDateTimeLocalePreferences",l_Time_GetDateTimeLocalePreferences},
    { "TimeToDateTime",              l_Time_TimeToDateTime              },
    { "DateTimeToTime",              l_Time_DateTimeToTime              },
    { "GetDaysInMonth",              l_Time_GetDaysInMonth              },
    { "GetDayOfYear",                l_Time_GetDayOfYear                },
    { "GetDayOfWeek",                l_Time_GetDayOfWeek                },
    // Phase AR — Callback timers (one-shot)
    { "AddTimer",                    l_Time_AddTimer                    },
    { "RemoveTimer",                 l_Time_RemoveTimer                 },
    // Phase H.0 — Tick-Render 解耦 (11 fn)
    { "SetFixedTimestep",            l_Time_SetFixedTimestep            },
    { "GetFixedTimestep",            l_Time_GetFixedTimestep            },
    { "GetFixedDt",                  l_Time_GetFixedDt                  },
    { "SetMaxFixedStepsPerFrame",    l_Time_SetMaxFixedStepsPerFrame    },
    { "GetMaxFixedStepsPerFrame",    l_Time_GetMaxFixedStepsPerFrame    },
    { "SetFrameTimeClamp",           l_Time_SetFrameTimeClamp           },
    { "GetFrameTimeClamp",           l_Time_GetFrameTimeClamp           },
    { "GetAlpha",                    l_Time_GetAlpha                    },
    { "GetAccumulator",              l_Time_GetAccumulator              },
    { "GetLastStepCount",            l_Time_GetLastStepCount            },
    { "GetLastFrameTime",            l_Time_GetLastFrameTime            },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Time(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kTimeReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    lua_pushinteger(L, (lua_Integer)SDL_DATE_FORMAT_YYYYMMDD);
    lua_setfield(L, -2, "DATE_FORMAT_YYYYMMDD");
    lua_pushinteger(L, (lua_Integer)SDL_DATE_FORMAT_DDMMYYYY);
    lua_setfield(L, -2, "DATE_FORMAT_DDMMYYYY");
    lua_pushinteger(L, (lua_Integer)SDL_DATE_FORMAT_MMDDYYYY);
    lua_setfield(L, -2, "DATE_FORMAT_MMDDYYYY");

    lua_pushinteger(L, (lua_Integer)SDL_TIME_FORMAT_24HR);
    lua_setfield(L, -2, "TIME_FORMAT_24HR");
    lua_pushinteger(L, (lua_Integer)SDL_TIME_FORMAT_12HR);
    lua_setfield(L, -2, "TIME_FORMAT_12HR");

    // Phase AR — 预注册 Timer 事件类型 (避免 AddTimer 调用时才注册造成丢事件)
    EnsureTimerEventTypeRegistered();

    return 1;
}
