/**
 * @file light_haptic.cpp
 * @brief Light.Haptic 模块 - 触觉反馈 (基于 SDL_haptic) — Phase AK 完整覆盖
 *
 * Lua API (Phase G + Phase AK 共 32 fns):
 *
 *   [Phase G] 设备发现 / 打开关闭 / 查询 / 简易 rumble / 全局控制 (22 fns)
 *     Init/Quit/GetHaptics/IsMouseHaptic
 *     Open/OpenFromMouse/Close
 *     GetID/GetName/GetFeatures/GetMaxEffects/GetMaxEffectsPlaying/GetNumAxes
 *     RumbleSupported/InitRumble/PlayRumble/StopRumble
 *     Pause/Resume/SetGain/SetAutocenter/StopAll
 *
 *   [Phase AK] Joystick 集成 (3 fns)
 *     OpenFromJoystick(joystick)            -> haptic, err   (lightuserdata SDL_Joystick*)
 *     IsJoystickHaptic(joystick)            -> bool
 *     GetHapticFromID(id)                   -> haptic, err
 *
 *   [Phase AK] HapticEffect 高级 API (7 fns) — 12 种 effect type, lua table -> union 转换
 *     EffectSupported(haptic, effect_tbl)   -> bool
 *     CreateEffect(haptic, effect_tbl)      -> effect_id (>=0) | -1, err
 *     UpdateEffect(haptic, effect_id, tbl)  -> ok, err
 *     RunEffect(haptic, effect_id, iters)   -> ok, err   (iters Uint32, HAPTIC_INFINITY ok)
 *     StopEffect(haptic, effect_id)         -> ok, err
 *     DestroyEffect(haptic, effect_id)      -> nil
 *     GetEffectStatus(haptic, effect_id)    -> bool       (需要 HAPTIC_STATUS feature)
 *
 *   Effect Table 约定 (lua):
 *     {
 *       type            = "sine",            -- 必填: constant/sine/square/triangle/
 *                                            -- sawtooth_up/sawtooth_down/ramp/
 *                                            -- spring/damper/inertia/friction/
 *                                            -- leftright/custom
 *       direction_type  = "polar",           -- polar/cartesian/spherical/steering_axis
 *                                            -- (leftright 不需要 direction)
 *       direction       = {18000, 0, 0},     -- Sint32 dir[3] (polar 仅用 dir[0])
 *       length          = 1000,              -- ms; HAPTIC_INFINITY 表无限循环
 *       delay           = 0,                 -- ms
 *       button          = 0,                 -- 触发按钮 (1-based)
 *       interval        = 0,                 -- ms 触发后冷却
 *
 *       -- Periodic 专属:
 *       period          = 100,               -- ms
 *       magnitude       = 20000,             -- Sint16 (-32768..32767)
 *       offset          = 0,                 -- Sint16
 *       phase           = 0,                 -- 0..36000 (百分之一度)
 *
 *       -- Constant 专属:
 *       level           = 20000,             -- Sint16
 *
 *       -- Ramp 专属:
 *       start           = 0,                 -- Sint16 起始强度
 *       ['end']         = 32767,             -- Sint16 结束强度  (lua 关键字, 用 ['end'])
 *
 *       -- Condition 专属 (spring/damper/inertia/friction):
 *       right_sat       = {0xFFFF, 0xFFFF, 0xFFFF},  -- Uint16[3]
 *       left_sat        = {0xFFFF, 0xFFFF, 0xFFFF},  -- Uint16[3]
 *       right_coeff     = {0, 0, 0},                  -- Sint16[3]
 *       left_coeff      = {0, 0, 0},                  -- Sint16[3]
 *       deadband        = {0, 0, 0},                  -- Uint16[3]
 *       center          = {0, 0, 0},                  -- Sint16[3]
 *
 *       -- LeftRight 专属:
 *       large_magnitude = 0xFFFF,            -- Uint16 (低频大马达)
 *       small_magnitude = 0xFFFF,            -- Uint16 (高频小马达)
 *
 *       -- Envelope (constant/periodic/ramp/custom 共用):
 *       attack_length   = 0,
 *       attack_level    = 0,
 *       fade_length     = 0,
 *       fade_level      = 0,
 *
 *       -- Custom 专属 (注: data 由 light_haptic 复制内部存储, lua 字符串可释放):
 *       channels        = 1,
 *       samples         = 100,
 *       data            = "<raw Uint16 LE bytes, channels*samples*2 字节>",
 *     }
 *
 *   Constants (25 个):
 *     Effect types (bit flags, 配合 GetFeatures/HAPTIC_*): CONSTANT/SINE/SQUARE/TRIANGLE/
 *       SAWTOOTHUP/SAWTOOTHDOWN/RAMP/SPRING/DAMPER/INERTIA/FRICTION/LEFTRIGHT/
 *       RESERVED1/2/3/CUSTOM
 *     Device features: GAIN/AUTOCENTER/STATUS/PAUSE
 *     Direction encoding: POLAR/CARTESIAN/SPHERICAL/STEERING_AXIS
 *     Special: HAPTIC_INFINITY = 4294967295 (length 无限)
 *
 * 设计要点:
 * - dev 句柄用 lightuserdata; effect_id 用 int (SDL3 SDL_HapticEffectID).
 * - effect_id 失效条件: DestroyEffect / Close / 设备拔出.
 * - Custom data 由 light_haptic malloc 副本, DestroyEffect 时 free; 用全局 map 跟踪生命周期.
 * - Joystick 集成: OpenFromJoystick 接收 lightuserdata SDL_Joystick* (与 Light.Joystick 协议一致).
 *
 * 平台覆盖:
 *   Windows DirectInput / XInput, Linux evdev, macOS IOKit.
 *   Android / iOS / Web 通常不支持独立 haptic 子系统 (但 gamepad rumble 走另一路径).
 */
#include "light.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_joystick.h>

#include <cstring>
#include <map>
#include <utility>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// 内部辅助
// ============================================================

static SDL_Haptic* CheckHaptic(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return (SDL_Haptic*)lua_touserdata(L, idx);
    }
    return nullptr;
}

// 与 Light.Joystick (Phase AI) 协议一致: lightuserdata SDL_Joystick*
static SDL_Joystick* CheckJoystick(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return (SDL_Joystick*)lua_touserdata(L, idx);
    }
    return nullptr;
}

// ------------------------------------------------------------
// Custom effect data lifetime tracker
// ------------------------------------------------------------
// SDL_HapticCustom.data 是 Uint16* 指针, SDL3 不会 deep-copy.
// 我们在 CreateEffect 时 malloc 副本, 关联到 (haptic, effect_id),
// DestroyEffect / Close 时 free.
// ------------------------------------------------------------

using CustomDataKey = std::pair<SDL_Haptic*, int>;
static std::map<CustomDataKey, std::vector<Uint16>> g_customData;

static void ReleaseCustomData(SDL_Haptic* h, int effect_id) {
    g_customData.erase({h, effect_id});
}

static void ReleaseAllCustomDataForHaptic(SDL_Haptic* h) {
    for (auto it = g_customData.begin(); it != g_customData.end(); ) {
        if (it->first.first == h) it = g_customData.erase(it);
        else ++it;
    }
}

// ------------------------------------------------------------
// Effect type 字符串 <-> Uint16 SDL_HAPTIC_*
// ------------------------------------------------------------
struct EffectTypeMap { const char* name; Uint16 value; };
static const EffectTypeMap kEffectTypes[] = {
    {"constant",       SDL_HAPTIC_CONSTANT},
    {"sine",           SDL_HAPTIC_SINE},
    {"square",         SDL_HAPTIC_SQUARE},
    {"triangle",       SDL_HAPTIC_TRIANGLE},
    {"sawtooth_up",    SDL_HAPTIC_SAWTOOTHUP},
    {"sawtoothup",     SDL_HAPTIC_SAWTOOTHUP},
    {"sawtooth_down",  SDL_HAPTIC_SAWTOOTHDOWN},
    {"sawtoothdown",   SDL_HAPTIC_SAWTOOTHDOWN},
    {"ramp",           SDL_HAPTIC_RAMP},
    {"spring",         SDL_HAPTIC_SPRING},
    {"damper",         SDL_HAPTIC_DAMPER},
    {"inertia",        SDL_HAPTIC_INERTIA},
    {"friction",       SDL_HAPTIC_FRICTION},
    {"leftright",      SDL_HAPTIC_LEFTRIGHT},
    {"left_right",     SDL_HAPTIC_LEFTRIGHT},
    {"custom",         SDL_HAPTIC_CUSTOM},
    {nullptr, 0}
};

static Uint16 ParseEffectType(const char* s) {
    if (!s) return 0;
    for (const EffectTypeMap* m = kEffectTypes; m->name; ++m) {
        if (SDL_strcasecmp(s, m->name) == 0) return m->value;
    }
    return 0;
}

static Uint8 ParseDirectionType(const char* s) {
    if (!s) return SDL_HAPTIC_POLAR;
    if (SDL_strcasecmp(s, "polar")          == 0) return SDL_HAPTIC_POLAR;
    if (SDL_strcasecmp(s, "cartesian")      == 0) return SDL_HAPTIC_CARTESIAN;
    if (SDL_strcasecmp(s, "spherical")      == 0) return SDL_HAPTIC_SPHERICAL;
    if (SDL_strcasecmp(s, "steering_axis")  == 0) return SDL_HAPTIC_STEERING_AXIS;
    if (SDL_strcasecmp(s, "steering")       == 0) return SDL_HAPTIC_STEERING_AXIS;
    return SDL_HAPTIC_POLAR;
}

// ------------------------------------------------------------
// 字段读取 helpers (table 在 idx, 缺失返回 default)
// ------------------------------------------------------------
static lua_Integer FieldInt(lua_State* L, int idx, const char* key, lua_Integer dflt) {
    lua_getfield(L, idx, key);
    lua_Integer v = lua_isnumber(L, -1) ? (lua_Integer)lua_tointeger(L, -1) : dflt;
    lua_pop(L, 1);
    return v;
}

static const char* FieldStr(lua_State* L, int idx, const char* key) {
    lua_getfield(L, idx, key);
    const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
    lua_pop(L, 1);
    return s;
}

// 读 table 字段为 array<int>[3], 不足补 0
static void FieldArray3i(lua_State* L, int idx, const char* key, int* out, int max_count = 3) {
    for (int i = 0; i < max_count; ++i) out[i] = 0;
    lua_getfield(L, idx, key);
    if (lua_istable(L, -1)) {
        int top_arr = lua_gettop(L);
        for (int i = 0; i < max_count; ++i) {
            lua_rawgeti(L, top_arr, i + 1);
            if (lua_isnumber(L, -1)) out[i] = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

// ------------------------------------------------------------
// Lua table -> SDL_HapticEffect union
// 返回 0 成功, 非0 失败 (失败时 *err 填错误信息).
// 对 CUSTOM type, custom_buf 用于接收 Uint16 数据副本 (调用者保留).
// ------------------------------------------------------------
static int ParseHapticEffect(lua_State* L, int idx, SDL_HapticEffect* out,
                             std::vector<Uint16>* custom_buf, const char** err) {
    SDL_memset(out, 0, sizeof(SDL_HapticEffect));

    if (!lua_istable(L, idx)) {
        *err = "effect must be a table";
        return -1;
    }

    // 类型 (必填)
    const char* type_str = FieldStr(L, idx, "type");
    Uint16 etype = ParseEffectType(type_str);
    if (etype == 0) {
        *err = "effect.type missing or invalid (use constant/sine/.../custom)";
        return -1;
    }

    // 公共: direction (LEFTRIGHT 没有 direction 字段)
    SDL_HapticDirection dir;
    SDL_memset(&dir, 0, sizeof(dir));
    if (etype != SDL_HAPTIC_LEFTRIGHT) {
        dir.type = ParseDirectionType(FieldStr(L, idx, "direction_type"));
        int d3[3];
        FieldArray3i(L, idx, "direction", d3, 3);
        dir.dir[0] = (Sint32)d3[0];
        dir.dir[1] = (Sint32)d3[1];
        dir.dir[2] = (Sint32)d3[2];
    }

    // 公共: replay/trigger (LEFTRIGHT 只有 length)
    Uint32 length   = (Uint32)FieldInt(L, idx, "length",   1000);
    Uint16 delay    = (Uint16)FieldInt(L, idx, "delay",    0);
    Uint16 button   = (Uint16)FieldInt(L, idx, "button",   0);
    Uint16 interval = (Uint16)FieldInt(L, idx, "interval", 0);

    // 公共: envelope (Constant/Periodic/Ramp/Custom, Condition 不用)
    Uint16 atk_len = (Uint16)FieldInt(L, idx, "attack_length", 0);
    Uint16 atk_lvl = (Uint16)FieldInt(L, idx, "attack_level",  0);
    Uint16 fade_len= (Uint16)FieldInt(L, idx, "fade_length",   0);
    Uint16 fade_lvl= (Uint16)FieldInt(L, idx, "fade_level",    0);

    out->type = etype;

    switch (etype) {
        case SDL_HAPTIC_CONSTANT: {
            auto& c = out->constant;
            c.type = etype; c.direction = dir;
            c.length = length; c.delay = delay;
            c.button = button; c.interval = interval;
            c.level         = (Sint16)FieldInt(L, idx, "level", 0);
            c.attack_length = atk_len; c.attack_level = atk_lvl;
            c.fade_length   = fade_len; c.fade_level  = fade_lvl;
            break;
        }
        case SDL_HAPTIC_SINE:
        case SDL_HAPTIC_SQUARE:
        case SDL_HAPTIC_TRIANGLE:
        case SDL_HAPTIC_SAWTOOTHUP:
        case SDL_HAPTIC_SAWTOOTHDOWN: {
            auto& p = out->periodic;
            p.type = etype; p.direction = dir;
            p.length = length; p.delay = delay;
            p.button = button; p.interval = interval;
            p.period    = (Uint16)FieldInt(L, idx, "period",    100);
            p.magnitude = (Sint16)FieldInt(L, idx, "magnitude", 0);
            p.offset    = (Sint16)FieldInt(L, idx, "offset",    0);
            p.phase     = (Uint16)FieldInt(L, idx, "phase",     0);
            p.attack_length = atk_len; p.attack_level = atk_lvl;
            p.fade_length   = fade_len; p.fade_level  = fade_lvl;
            break;
        }
        case SDL_HAPTIC_RAMP: {
            auto& r = out->ramp;
            r.type = etype; r.direction = dir;
            r.length = length; r.delay = delay;
            r.button = button; r.interval = interval;
            r.start = (Sint16)FieldInt(L, idx, "start", 0);
            r.end   = (Sint16)FieldInt(L, idx, "end",   0);  // lua 中需写 ['end']
            r.attack_length = atk_len; r.attack_level = atk_lvl;
            r.fade_length   = fade_len; r.fade_level  = fade_lvl;
            break;
        }
        case SDL_HAPTIC_SPRING:
        case SDL_HAPTIC_DAMPER:
        case SDL_HAPTIC_INERTIA:
        case SDL_HAPTIC_FRICTION: {
            auto& cd = out->condition;
            cd.type = etype; cd.direction = dir;
            cd.length = length; cd.delay = delay;
            cd.button = button; cd.interval = interval;
            int tmp[3];
            FieldArray3i(L, idx, "right_sat",   tmp); for (int i=0;i<3;++i) cd.right_sat[i]   = (Uint16)tmp[i];
            FieldArray3i(L, idx, "left_sat",    tmp); for (int i=0;i<3;++i) cd.left_sat[i]    = (Uint16)tmp[i];
            FieldArray3i(L, idx, "right_coeff", tmp); for (int i=0;i<3;++i) cd.right_coeff[i] = (Sint16)tmp[i];
            FieldArray3i(L, idx, "left_coeff",  tmp); for (int i=0;i<3;++i) cd.left_coeff[i]  = (Sint16)tmp[i];
            FieldArray3i(L, idx, "deadband",    tmp); for (int i=0;i<3;++i) cd.deadband[i]    = (Uint16)tmp[i];
            FieldArray3i(L, idx, "center",      tmp); for (int i=0;i<3;++i) cd.center[i]      = (Sint16)tmp[i];
            // condition 无 envelope
            break;
        }
        case SDL_HAPTIC_LEFTRIGHT: {
            auto& lr = out->leftright;
            lr.type = etype;
            lr.length = length;
            lr.large_magnitude = (Uint16)FieldInt(L, idx, "large_magnitude", 0);
            lr.small_magnitude = (Uint16)FieldInt(L, idx, "small_magnitude", 0);
            break;
        }
        case SDL_HAPTIC_CUSTOM: {
            auto& cu = out->custom;
            cu.type = etype; cu.direction = dir;
            cu.length = length; cu.delay = delay;
            cu.button = button; cu.interval = interval;
            cu.channels = (Uint8) FieldInt(L, idx, "channels", 1);
            cu.period   = (Uint16)FieldInt(L, idx, "period",   100);
            cu.samples  = (Uint16)FieldInt(L, idx, "samples",  0);
            cu.attack_length = atk_len; cu.attack_level = atk_lvl;
            cu.fade_length   = fade_len; cu.fade_level  = fade_lvl;

            // 复制 data 字符串到 Uint16 副本 (调用者管理生命周期)
            lua_getfield(L, idx, "data");
            if (lua_isstring(L, -1) && custom_buf) {
                size_t blen = 0;
                const char* bytes = lua_tolstring(L, -1, &blen);
                size_t expected = (size_t)cu.channels * (size_t)cu.samples * sizeof(Uint16);
                if (blen < expected) {
                    lua_pop(L, 1);
                    *err = "custom.data too short for channels*samples*2 bytes";
                    return -1;
                }
                custom_buf->resize(cu.samples * cu.channels);
                SDL_memcpy(custom_buf->data(), bytes, custom_buf->size() * sizeof(Uint16));
                cu.data = custom_buf->data();
            } else if (custom_buf) {
                // 无 data 但 type 是 custom: 允许 (后续 effect 会失败但 ParseHapticEffect 不报错)
                cu.data = nullptr;
            }
            lua_pop(L, 1);
            break;
        }
        default:
            *err = "unsupported effect type";
            return -1;
    }
    return 0;
}

// ============================================================
// Init / Quit
// ============================================================

static int l_Haptic_Init(lua_State* L) {
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_Quit(lua_State* L) {
    SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    lua_pushnil(L);
    return 1;
}

// ============================================================
// 设备发现
// ============================================================

static int l_Haptic_GetHaptics(lua_State* L) {
    int count = 0;
    SDL_HapticID* ids = SDL_GetHaptics(&count);
    if (!ids && count != 0) {
        // count==0 是合法 (无设备), 仅 ids==NULL && count>0 才算错
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }

    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)ids[i]);
        lua_setfield(L, -2, "id");
        const char* name = SDL_GetHapticNameForID(ids[i]);
        lua_pushstring(L, name ? name : "");
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i + 1);
    }
    if (ids) SDL_free(ids);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_IsMouseHaptic(lua_State* L) {
    lua_pushboolean(L, SDL_IsMouseHaptic() ? 1 : 0);
    return 1;
}

// ============================================================
// 设备打开/关闭
// ============================================================

static int l_Haptic_Open(lua_State* L) {
    SDL_HapticID id = (SDL_HapticID)luaL_checkinteger(L, 1);
    SDL_Haptic* dev = SDL_OpenHaptic(id);
    if (!dev) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_OpenFromMouse(lua_State* L) {
    SDL_Haptic* dev = SDL_OpenHapticFromMouse();
    if (!dev) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_Close(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid haptic handle");
        return 2;
    }
    // Phase AK: 释放该设备的全部 custom effect data 副本
    ReleaseAllCustomDataForHaptic(dev);
    SDL_CloseHaptic(dev);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// 设备查询
// ============================================================

static int l_Haptic_GetID(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    SDL_HapticID id = SDL_GetHapticID(dev);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetName(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    const char* name = SDL_GetHapticName(dev);
    if (!name) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetFeatures(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    Uint32 features = SDL_GetHapticFeatures(dev);
    lua_pushinteger(L, (lua_Integer)features);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetMaxEffects(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int n = SDL_GetMaxHapticEffects(dev);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetMaxEffectsPlaying(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int n = SDL_GetMaxHapticEffectsPlaying(dev);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetNumAxes(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int n = SDL_GetNumHapticAxes(dev);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// 简易 rumble
// ============================================================

static int l_Haptic_RumbleSupported(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HapticRumbleSupported(dev) ? 1 : 0);
    return 1;
}

static int l_Haptic_InitRumble(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_InitHapticRumble(dev)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_PlayRumble(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    lua_Number strength = luaL_checknumber(L, 2);  // 0..1
    lua_Integer length  = luaL_checkinteger(L, 3); // ms

    // 范围保护 (SDL3 内部不强校验, 这里收紧)
    if (strength < 0.0) strength = 0.0;
    if (strength > 1.0) strength = 1.0;
    if (length < 0) length = 0;

    if (!SDL_PlayHapticRumble(dev, (float)strength, (Uint32)length)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_StopRumble(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_StopHapticRumble(dev)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// Phase AK: Joystick 集成 (3 fns)
// ============================================================

static int l_Haptic_OpenFromJoystick(lua_State* L) {
    SDL_Joystick* joy = CheckJoystick(L, 1);
    if (!joy) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid joystick handle");
        return 2;
    }
    SDL_Haptic* dev = SDL_OpenHapticFromJoystick(joy);
    if (!dev) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_IsJoystickHaptic(lua_State* L) {
    SDL_Joystick* joy = CheckJoystick(L, 1);
    if (!joy) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_IsJoystickHaptic(joy) ? 1 : 0);
    return 1;
}

static int l_Haptic_GetHapticFromID(lua_State* L) {
    SDL_HapticID id = (SDL_HapticID)luaL_checkinteger(L, 1);
    SDL_Haptic* dev = SDL_GetHapticFromID(id);
    if (!dev) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// Phase AK: HapticEffect 高级 API (7 fns)
// ============================================================

static int l_Haptic_EffectSupported(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); return 1; }

    SDL_HapticEffect effect;
    std::vector<Uint16> tmp_buf;
    const char* err = nullptr;
    if (ParseHapticEffect(L, 2, &effect, &tmp_buf, &err) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, SDL_HapticEffectSupported(dev, &effect) ? 1 : 0);
    return 1;
}

static int l_Haptic_CreateEffect(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, "invalid haptic handle");
        return 2;
    }

    SDL_HapticEffect effect;
    // 用临时 buffer 解析, CUSTOM 成功后转移到 g_customData
    std::vector<Uint16> custom_buf;
    const char* err = nullptr;
    if (ParseHapticEffect(L, 2, &effect, &custom_buf, &err) != 0) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, err ? err : "invalid effect table");
        return 2;
    }

    int effect_id = SDL_CreateHapticEffect(dev, &effect);
    if (effect_id < 0) {
        lua_pushinteger(L, -1);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }

    // CUSTOM 成功: 将 buffer 移交 g_customData (SDL3 引用 cu.data 指针, 必须保活)
    if (effect.type == SDL_HAPTIC_CUSTOM && !custom_buf.empty()) {
        g_customData[{dev, effect_id}] = std::move(custom_buf);
        // 注: SDL_CreateHapticEffect 在内部已 deep-copy effect struct, 但 custom.data 指针指向
        // 我们的 buffer; SDL3 平台后端通常已把 samples 复制到驱动. 但为保险, 我们持有 buffer
        // 直到 DestroyEffect.
    }

    lua_pushinteger(L, effect_id);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_UpdateEffect(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid haptic handle");
        return 2;
    }
    int effect_id = (int)luaL_checkinteger(L, 2);

    SDL_HapticEffect effect;
    std::vector<Uint16> custom_buf;
    const char* err = nullptr;
    if (ParseHapticEffect(L, 3, &effect, &custom_buf, &err) != 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, err ? err : "invalid effect table");
        return 2;
    }

    if (!SDL_UpdateHapticEffect(dev, effect_id, &effect)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }

    // CUSTOM update: 替换旧 buffer
    if (effect.type == SDL_HAPTIC_CUSTOM) {
        if (!custom_buf.empty()) {
            g_customData[{dev, effect_id}] = std::move(custom_buf);
        }
    }

    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_RunEffect(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid haptic handle");
        return 2;
    }
    int effect_id = (int)luaL_checkinteger(L, 2);
    // iterations 可为 SDL_HAPTIC_INFINITY (4294967295), 必须用 lua_Number 接收避免 32-bit clip
    lua_Number iters_num = luaL_optnumber(L, 3, 1);
    if (iters_num < 0) iters_num = 0;
    Uint32 iters = (iters_num >= (lua_Number)SDL_HAPTIC_INFINITY) ? SDL_HAPTIC_INFINITY
                                                                  : (Uint32)iters_num;

    if (!SDL_RunHapticEffect(dev, effect_id, iters)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_StopEffect(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid haptic handle");
        return 2;
    }
    int effect_id = (int)luaL_checkinteger(L, 2);
    if (!SDL_StopHapticEffect(dev, effect_id)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_DestroyEffect(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        // DestroyEffect 是 void 返回, 静默忽略无效 handle
        lua_pushnil(L);
        return 1;
    }
    int effect_id = (int)luaL_checkinteger(L, 2);
    SDL_DestroyHapticEffect(dev, effect_id);
    ReleaseCustomData(dev, effect_id);
    lua_pushnil(L);
    return 1;
}

static int l_Haptic_GetEffectStatus(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    int effect_id = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_GetHapticEffectStatus(dev, effect_id) ? 1 : 0);
    return 1;
}

// ============================================================
// 全局控制
// ============================================================

static int l_Haptic_Pause(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_PauseHaptic(dev)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_Resume(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_ResumeHaptic(dev)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_SetGain(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int gain = (int)luaL_checkinteger(L, 2);
    // 范围 0..100, SDL3 内部 clamp 但不报错; 这里直接传
    if (gain < 0) gain = 0;
    if (gain > 100) gain = 100;
    if (!SDL_SetHapticGain(dev, gain)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_SetAutocenter(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int pct = (int)luaL_checkinteger(L, 2);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!SDL_SetHapticAutocenter(dev, pct)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_StopAll(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_StopHapticEffects(dev)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ============================================================
// 注册
// ============================================================

static const luaL_Reg kHapticReg[] = {
    // Phase G
    {"Init",                    l_Haptic_Init},
    {"Quit",                    l_Haptic_Quit},
    {"GetHaptics",              l_Haptic_GetHaptics},
    {"IsMouseHaptic",           l_Haptic_IsMouseHaptic},
    {"Open",                    l_Haptic_Open},
    {"OpenFromMouse",           l_Haptic_OpenFromMouse},
    {"Close",                   l_Haptic_Close},
    {"GetID",                   l_Haptic_GetID},
    {"GetName",                 l_Haptic_GetName},
    {"GetFeatures",             l_Haptic_GetFeatures},
    {"GetMaxEffects",           l_Haptic_GetMaxEffects},
    {"GetMaxEffectsPlaying",    l_Haptic_GetMaxEffectsPlaying},
    {"GetNumAxes",              l_Haptic_GetNumAxes},
    {"RumbleSupported",         l_Haptic_RumbleSupported},
    {"InitRumble",              l_Haptic_InitRumble},
    {"PlayRumble",              l_Haptic_PlayRumble},
    {"StopRumble",              l_Haptic_StopRumble},
    {"Pause",                   l_Haptic_Pause},
    {"Resume",                  l_Haptic_Resume},
    {"SetGain",                 l_Haptic_SetGain},
    {"SetAutocenter",           l_Haptic_SetAutocenter},
    {"StopAll",                 l_Haptic_StopAll},
    // Phase AK: Joystick 集成
    {"OpenFromJoystick",        l_Haptic_OpenFromJoystick},
    {"IsJoystickHaptic",        l_Haptic_IsJoystickHaptic},
    {"GetHapticFromID",         l_Haptic_GetHapticFromID},
    // Phase AK: Effect API
    {"EffectSupported",         l_Haptic_EffectSupported},
    {"CreateEffect",            l_Haptic_CreateEffect},
    {"UpdateEffect",            l_Haptic_UpdateEffect},
    {"RunEffect",               l_Haptic_RunEffect},
    {"StopEffect",              l_Haptic_StopEffect},
    {"DestroyEffect",           l_Haptic_DestroyEffect},
    {"GetEffectStatus",         l_Haptic_GetEffectStatus},
    {nullptr, nullptr}
};

// 常量表 (一次性注册)
struct HapticConst { const char* name; lua_Number value; };
static const HapticConst kHapticConsts[] = {
    // Effect types (bit flags)
    {"HAPTIC_CONSTANT",       (lua_Number)SDL_HAPTIC_CONSTANT},
    {"HAPTIC_SINE",           (lua_Number)SDL_HAPTIC_SINE},
    {"HAPTIC_SQUARE",         (lua_Number)SDL_HAPTIC_SQUARE},
    {"HAPTIC_TRIANGLE",       (lua_Number)SDL_HAPTIC_TRIANGLE},
    {"HAPTIC_SAWTOOTHUP",     (lua_Number)SDL_HAPTIC_SAWTOOTHUP},
    {"HAPTIC_SAWTOOTHDOWN",   (lua_Number)SDL_HAPTIC_SAWTOOTHDOWN},
    {"HAPTIC_RAMP",           (lua_Number)SDL_HAPTIC_RAMP},
    {"HAPTIC_SPRING",         (lua_Number)SDL_HAPTIC_SPRING},
    {"HAPTIC_DAMPER",         (lua_Number)SDL_HAPTIC_DAMPER},
    {"HAPTIC_INERTIA",        (lua_Number)SDL_HAPTIC_INERTIA},
    {"HAPTIC_FRICTION",       (lua_Number)SDL_HAPTIC_FRICTION},
    {"HAPTIC_LEFTRIGHT",      (lua_Number)SDL_HAPTIC_LEFTRIGHT},
    {"HAPTIC_RESERVED1",      (lua_Number)SDL_HAPTIC_RESERVED1},
    {"HAPTIC_RESERVED2",      (lua_Number)SDL_HAPTIC_RESERVED2},
    {"HAPTIC_RESERVED3",      (lua_Number)SDL_HAPTIC_RESERVED3},
    {"HAPTIC_CUSTOM",         (lua_Number)SDL_HAPTIC_CUSTOM},
    // Device features
    {"HAPTIC_GAIN",           (lua_Number)SDL_HAPTIC_GAIN},
    {"HAPTIC_AUTOCENTER",     (lua_Number)SDL_HAPTIC_AUTOCENTER},
    {"HAPTIC_STATUS",         (lua_Number)SDL_HAPTIC_STATUS},
    {"HAPTIC_PAUSE",          (lua_Number)SDL_HAPTIC_PAUSE},
    // Direction encoding
    {"HAPTIC_POLAR",          (lua_Number)SDL_HAPTIC_POLAR},
    {"HAPTIC_CARTESIAN",      (lua_Number)SDL_HAPTIC_CARTESIAN},
    {"HAPTIC_SPHERICAL",      (lua_Number)SDL_HAPTIC_SPHERICAL},
    {"HAPTIC_STEERING_AXIS",  (lua_Number)SDL_HAPTIC_STEERING_AXIS},
    // Special
    {"HAPTIC_INFINITY",       (lua_Number)SDL_HAPTIC_INFINITY},
    {nullptr, 0.0}
};

extern "C" LIGHT_API int luaopen_Light_Haptic(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kHapticReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    // Phase AK: 注册 25 个常量
    for (const HapticConst* c = kHapticConsts; c->name; ++c) {
        lua_pushnumber(L, c->value);
        lua_setfield(L, -2, c->name);
    }
    return 1;
}
