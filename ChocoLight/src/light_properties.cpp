/**
 * @file light_properties.cpp
 * @brief Light.Properties module - SDL3 typed property bag
 *
 * Lua API (18 fns):
 *
 *  Lifecycle:
 *    Light.Properties.GetGlobalProperties()           -> id
 *    Light.Properties.CreateProperties()              -> id|0, err
 *    Light.Properties.DestroyProperties(id)           -> nil  (nil-safe; id=0 ok)
 *    Light.Properties.CopyProperties(src, dst)        -> ok, err
 *
 *  Locking (advisory; SDL props are already thread-safe):
 *    Light.Properties.LockProperties(id)              -> ok, err
 *    Light.Properties.UnlockProperties(id)            -> nil
 *
 *  Setters (5 typed):
 *    Light.Properties.SetPointerProperty(id, name, lightuserdata|nil) -> ok, err
 *    Light.Properties.SetStringProperty (id, name, str|nil)           -> ok, err
 *    Light.Properties.SetNumberProperty (id, name, sint64)            -> ok, err
 *    Light.Properties.SetFloatProperty  (id, name, float)             -> ok, err
 *    Light.Properties.SetBooleanProperty(id, name, bool)              -> ok, err
 *
 *  Getters (5 typed; default returned when missing or wrong type):
 *    Light.Properties.GetPointerProperty(id, name, [default_lud])     -> lightuserdata|default
 *    Light.Properties.GetStringProperty (id, name, [default])         -> string
 *    Light.Properties.GetNumberProperty (id, name, [default])         -> number  (Sint64)
 *    Light.Properties.GetFloatProperty  (id, name, [default])         -> number
 *    Light.Properties.GetBooleanProperty(id, name, [default])         -> bool
 *
 *  Inspection:
 *    Light.Properties.HasProperty       (id, name)    -> bool
 *    Light.Properties.GetPropertyType   (id, name)    -> int (PROPERTY_TYPE_*)
 *    Light.Properties.ClearProperty     (id, name)    -> ok, err
 *
 * Constants (6):
 *    PROPERTY_TYPE_INVALID / _POINTER / _STRING / _NUMBER / _FLOAT / _BOOLEAN
 *
 * NOT bound (require trampoline / callback plumbing):
 *  - SDL_SetPointerPropertyWithCleanup
 *  - SDL_EnumerateProperties
 *
 * Notes:
 *  - SDL_PropertiesID is Uint32; we expose it as a plain Lua integer.
 *    Treat 0 as "invalid / global-of-no-resource"; SDL accepts 0 in
 *    DestroyProperties as no-op.
 *  - SDL_GetNumberProperty returns Sint64; lua_Number (53-bit mantissa)
 *    handles values up to 2^53 exactly. Beyond that precision is lost.
 *
 * No SDL_Init dependency.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// helpers
// ============================================================
static SDL_PropertiesID CheckPropsId(lua_State* L, int idx) {
    return (SDL_PropertiesID)luaL_checkinteger(L, idx);
}

static int PushBoolErr(lua_State* L, bool ok, const char* fallback) {
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) return 1;
    const char* e = SDL_GetError();
    lua_pushstring(L, (e && *e) ? e : fallback);
    return 2;
}

// ============================================================
// Lifecycle
// ============================================================
static int l_Props_GetGlobalProperties(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SDL_GetGlobalProperties());
    return 1;
}

static int l_Props_CreateProperties(lua_State* L) {
    SDL_PropertiesID id = SDL_CreateProperties();
    if (id == 0) {
        lua_pushinteger(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_CreateProperties failed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int l_Props_DestroyProperties(lua_State* L) {
    if (lua_isnoneornil(L, 1)) return 0;
    SDL_PropertiesID id = (SDL_PropertiesID)luaL_optinteger(L, 1, 0);
    if (id == 0) return 0;
    SDL_DestroyProperties(id);
    return 0;
}

static int l_Props_CopyProperties(lua_State* L) {
    SDL_PropertiesID src = CheckPropsId(L, 1);
    SDL_PropertiesID dst = CheckPropsId(L, 2);
    return PushBoolErr(L, SDL_CopyProperties(src, dst), "SDL_CopyProperties failed");
}

// ============================================================
// Locking
// ============================================================
static int l_Props_LockProperties(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    return PushBoolErr(L, SDL_LockProperties(id), "SDL_LockProperties failed");
}

static int l_Props_UnlockProperties(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    SDL_UnlockProperties(id);
    return 0;
}

// ============================================================
// Setters
// ============================================================
static int l_Props_SetPointerProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    void* value = nullptr;
    if (!lua_isnoneornil(L, 3)) {
        if (!lua_islightuserdata(L, 3)) {
            return luaL_error(L, "arg 3: expected lightuserdata or nil, got %s",
                              luaL_typename(L, 3));
        }
        value = lua_touserdata(L, 3);
    }
    return PushBoolErr(L, SDL_SetPointerProperty(id, name, value),
                       "SDL_SetPointerProperty failed");
}

static int l_Props_SetStringProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    // nil deletes the property (SDL semantics).
    const char* value = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
    return PushBoolErr(L, SDL_SetStringProperty(id, name, value),
                       "SDL_SetStringProperty failed");
}

static int l_Props_SetNumberProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Sint64 value = (Sint64)luaL_checknumber(L, 3);
    return PushBoolErr(L, SDL_SetNumberProperty(id, name, value),
                       "SDL_SetNumberProperty failed");
}

static int l_Props_SetFloatProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float value = (float)luaL_checknumber(L, 3);
    return PushBoolErr(L, SDL_SetFloatProperty(id, name, value),
                       "SDL_SetFloatProperty failed");
}

static int l_Props_SetBooleanProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);
    bool value = lua_toboolean(L, 3) != 0;
    return PushBoolErr(L, SDL_SetBooleanProperty(id, name, value),
                       "SDL_SetBooleanProperty failed");
}

// ============================================================
// Getters
// ============================================================
static int l_Props_GetPointerProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    void* defv = nullptr;
    if (!lua_isnoneornil(L, 3)) {
        if (!lua_islightuserdata(L, 3)) {
            return luaL_error(L, "arg 3 (default): expected lightuserdata or nil, got %s",
                              luaL_typename(L, 3));
        }
        defv = lua_touserdata(L, 3);
    }
    void* p = SDL_GetPointerProperty(id, name, defv);
    if (p == nullptr) {
        // Distinguish "explicit nil default returned" from "lightuserdata 0":
        // if the original default was nil, push nil; otherwise it can never be 0
        // since we only accept lightuserdata above.
        if (defv == nullptr) lua_pushnil(L);
        else lua_pushlightuserdata(L, p);
    } else {
        lua_pushlightuserdata(L, p);
    }
    return 1;
}

static int l_Props_GetStringProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const char* defv = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
    const char* s = SDL_GetStringProperty(id, name, defv);
    if (s == nullptr) lua_pushnil(L);
    else              lua_pushstring(L, s);
    return 1;
}

static int l_Props_GetNumberProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Sint64 defv = (Sint64)luaL_optnumber(L, 3, 0);
    Sint64 v = SDL_GetNumberProperty(id, name, defv);
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

static int l_Props_GetFloatProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float defv = (float)luaL_optnumber(L, 3, 0.0);
    float v = SDL_GetFloatProperty(id, name, defv);
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

static int l_Props_GetBooleanProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    bool defv = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checkany(L, 3);
        defv = lua_toboolean(L, 3) != 0;
    }
    bool v = SDL_GetBooleanProperty(id, name, defv);
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

// ============================================================
// Inspection
// ============================================================
static int l_Props_HasProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_pushboolean(L, SDL_HasProperty(id, name) ? 1 : 0);
    return 1;
}

static int l_Props_GetPropertyType(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_pushinteger(L, (lua_Integer)SDL_GetPropertyType(id, name));
    return 1;
}

static int l_Props_ClearProperty(lua_State* L) {
    SDL_PropertiesID id = CheckPropsId(L, 1);
    const char* name = luaL_checkstring(L, 2);
    return PushBoolErr(L, SDL_ClearProperty(id, name), "SDL_ClearProperty failed");
}

// ============================================================
// luaopen
// ============================================================
static const luaL_Reg kPropsReg[] = {
    { "GetGlobalProperties",  l_Props_GetGlobalProperties  },
    { "CreateProperties",     l_Props_CreateProperties     },
    { "DestroyProperties",    l_Props_DestroyProperties    },
    { "CopyProperties",       l_Props_CopyProperties       },
    { "LockProperties",       l_Props_LockProperties       },
    { "UnlockProperties",     l_Props_UnlockProperties     },
    { "SetPointerProperty",   l_Props_SetPointerProperty   },
    { "SetStringProperty",    l_Props_SetStringProperty    },
    { "SetNumberProperty",    l_Props_SetNumberProperty    },
    { "SetFloatProperty",     l_Props_SetFloatProperty     },
    { "SetBooleanProperty",   l_Props_SetBooleanProperty   },
    { "GetPointerProperty",   l_Props_GetPointerProperty   },
    { "GetStringProperty",    l_Props_GetStringProperty    },
    { "GetNumberProperty",    l_Props_GetNumberProperty    },
    { "GetFloatProperty",     l_Props_GetFloatProperty     },
    { "GetBooleanProperty",   l_Props_GetBooleanProperty   },
    { "HasProperty",          l_Props_HasProperty          },
    { "GetPropertyType",      l_Props_GetPropertyType      },
    { "ClearProperty",        l_Props_ClearProperty        },
    { nullptr, nullptr },
};

#define LIGHT_PUSH_INT_CONST(NAME, VALUE)        \
    lua_pushinteger(L, (lua_Integer)(VALUE));    \
    lua_setfield(L, -2, NAME)

extern "C" LIGHT_API int luaopen_Light_Properties(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kPropsReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    LIGHT_PUSH_INT_CONST("PROPERTY_TYPE_INVALID", SDL_PROPERTY_TYPE_INVALID);
    LIGHT_PUSH_INT_CONST("PROPERTY_TYPE_POINTER", SDL_PROPERTY_TYPE_POINTER);
    LIGHT_PUSH_INT_CONST("PROPERTY_TYPE_STRING",  SDL_PROPERTY_TYPE_STRING);
    LIGHT_PUSH_INT_CONST("PROPERTY_TYPE_NUMBER",  SDL_PROPERTY_TYPE_NUMBER);
    LIGHT_PUSH_INT_CONST("PROPERTY_TYPE_FLOAT",   SDL_PROPERTY_TYPE_FLOAT);
    LIGHT_PUSH_INT_CONST("PROPERTY_TYPE_BOOLEAN", SDL_PROPERTY_TYPE_BOOLEAN);

    return 1;
}

#undef LIGHT_PUSH_INT_CONST
