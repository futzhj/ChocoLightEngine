/**
 * @file light_keyboard.cpp
 * @brief Light.Keyboard module - SDL_keyboard.h
 *
 * Polling-style keyboard API. Complements the existing event-driven path
 * in Light.Input by exposing direct SDL keyboard state queries.
 *
 * No Window-coupled APIs are bound here (StartTextInput / Composition /
 * TextInputArea / ScreenKeyboardShown / GetKeyboardFocus). They require
 * an SDL_Window* userdata and are deferred until a shared Window type is
 * established for cross-module use.
 *
 * Lua API (15 fns):
 *
 *  Device queries:
 *    HasKeyboard()                          -> bool
 *    GetKeyboards()                         -> {instance_id, ...} | nil, err
 *    GetKeyboardNameForID(id)               -> string | nil
 *    HasScreenKeyboardSupport()             -> bool
 *
 *  State:
 *    GetKeyboardState()                     -> {bool, ...}  (1-indexed,
 *                                              length == SDL_SCANCODE_COUNT)
 *    ResetKeyboard()
 *    GetModState()                          -> integer (Keymod bitfield)
 *    SetModState(mod)
 *
 *  Scancode <-> Keycode:
 *    GetKeyFromScancode(scancode, mod, key_event_bool)  -> keycode
 *    GetScancodeFromKey(keycode)            -> scancode, mod
 *    SetScancodeName(scancode, name)        -> bool
 *    GetScancodeName(scancode)              -> string
 *    GetScancodeFromName(name)              -> scancode
 *    GetKeyName(keycode)                    -> string
 *    GetKeyFromName(name)                   -> keycode
 *
 * Constants exposed on the module table:
 *    SCANCODE_*  (~100 frequently used keys)
 *    KMOD_*      (17 keymod bits)
 *
 * Lazy init:
 *    SDL keyboard queries require SDL_INIT_VIDEO. Light.Window already
 *    initializes that on creation; for headless smoke we lazy-init on
 *    the first call and surface (false, err) on platforms where the
 *    video subsystem is unavailable.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// Lazy SDL_INIT_VIDEO
// ============================================================
static bool g_kbdSubsysInited = false;
static bool EnsureVideoSubsystem() {
    if (g_kbdSubsysInited) return true;
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        g_kbdSubsysInited = true;
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    g_kbdSubsysInited = true;
    return true;
}
static int InitErr(lua_State* L, int nrets) {
    const char* e = SDL_GetError();
    lua_pushnil(L);
    lua_pushstring(L, (e && *e) ? e : "SDL_INIT_VIDEO failed");
    return 2;
}

// ============================================================
// Device queries
// ============================================================
static int l_K_HasKeyboard(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HasKeyboard() ? 1 : 0);
    return 1;
}
static int l_K_GetKeyboards(lua_State* L) {
    if (!EnsureVideoSubsystem()) return InitErr(L, 2);
    int count = 0;
    SDL_KeyboardID* ids = SDL_GetKeyboards(&count);
    if (!ids) {
        // Some platforms return NULL with no actual error when 0 devices.
        lua_createtable(L, 0, 0);
        return 1;
    }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, (lua_Number)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    return 1;
}
static int l_K_GetKeyboardNameForID(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_pushnil(L); return 1; }
    SDL_KeyboardID id = (SDL_KeyboardID)luaL_checknumber(L, 1);
    const char* n = SDL_GetKeyboardNameForID(id);
    if (!n) { lua_pushnil(L); return 1; }
    lua_pushstring(L, n);
    return 1;
}
static int l_K_HasScreenKeyboardSupport(lua_State* L) {
    lua_pushboolean(L, SDL_HasScreenKeyboardSupport() ? 1 : 0);
    return 1;
}

// ============================================================
// State
// ============================================================
static int l_K_GetKeyboardState(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_pushnil(L); return 1; }
    int numkeys = 0;
    const bool* state = SDL_GetKeyboardState(&numkeys);
    if (!state || numkeys <= 0) {
        lua_createtable(L, 0, 0);
        return 1;
    }
    lua_createtable(L, numkeys, 0);
    for (int i = 0; i < numkeys; ++i) {
        lua_pushboolean(L, state[i] ? 1 : 0);
        lua_rawseti(L, -2, i + 1);  // 1-indexed: scancode N at table[N+1]
    }
    return 1;
}
static int l_K_ResetKeyboard(lua_State* L) {
    if (!EnsureVideoSubsystem()) return 0;
    SDL_ResetKeyboard();
    return 0;
}
static int l_K_GetModState(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, (lua_Integer)SDL_GetModState());
    return 1;
}
static int l_K_SetModState(lua_State* L) {
    if (!EnsureVideoSubsystem()) return 0;
    SDL_Keymod m = (SDL_Keymod)luaL_checkinteger(L, 1);
    SDL_SetModState(m);
    return 0;
}

// ============================================================
// Scancode <-> Keycode
// ============================================================
static int l_K_GetKeyFromScancode(lua_State* L) {
    SDL_Scancode sc = (SDL_Scancode)luaL_checkinteger(L, 1);
    SDL_Keymod m = (SDL_Keymod)luaL_optinteger(L, 2, SDL_KMOD_NONE);
    bool key_event = lua_toboolean(L, 3) ? true : false;
    SDL_Keycode k = SDL_GetKeyFromScancode(sc, m, key_event);
    lua_pushnumber(L, (lua_Number)k);  // SDL_Keycode is 32-bit
    return 1;
}
static int l_K_GetScancodeFromKey(lua_State* L) {
    SDL_Keycode k = (SDL_Keycode)luaL_checknumber(L, 1);
    SDL_Keymod m = SDL_KMOD_NONE;
    SDL_Scancode sc = SDL_GetScancodeFromKey(k, &m);
    lua_pushinteger(L, (lua_Integer)sc);
    lua_pushinteger(L, (lua_Integer)m);
    return 2;
}
static int l_K_SetScancodeName(lua_State* L) {
    SDL_Scancode sc = (SDL_Scancode)luaL_checkinteger(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_pushboolean(L, SDL_SetScancodeName(sc, name) ? 1 : 0);
    return 1;
}
static int l_K_GetScancodeName(lua_State* L) {
    SDL_Scancode sc = (SDL_Scancode)luaL_checkinteger(L, 1);
    const char* n = SDL_GetScancodeName(sc);
    if (!n) { lua_pushnil(L); return 1; }
    lua_pushstring(L, n);
    return 1;
}
static int l_K_GetScancodeFromName(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)SDL_GetScancodeFromName(name));
    return 1;
}
static int l_K_GetKeyName(lua_State* L) {
    SDL_Keycode k = (SDL_Keycode)luaL_checknumber(L, 1);
    const char* n = SDL_GetKeyName(k);
    if (!n) { lua_pushnil(L); return 1; }
    lua_pushstring(L, n);
    return 1;
}
static int l_K_GetKeyFromName(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushnumber(L, (lua_Number)SDL_GetKeyFromName(name));
    return 1;
}

// ============================================================
// luaopen_Light_Keyboard
// ============================================================
static const luaL_Reg kReg[] = {
    { "HasKeyboard",              l_K_HasKeyboard              },
    { "GetKeyboards",             l_K_GetKeyboards             },
    { "GetKeyboardNameForID",     l_K_GetKeyboardNameForID     },
    { "HasScreenKeyboardSupport", l_K_HasScreenKeyboardSupport },
    { "GetKeyboardState",         l_K_GetKeyboardState         },
    { "ResetKeyboard",            l_K_ResetKeyboard            },
    { "GetModState",              l_K_GetModState              },
    { "SetModState",              l_K_SetModState              },
    { "GetKeyFromScancode",       l_K_GetKeyFromScancode       },
    { "GetScancodeFromKey",       l_K_GetScancodeFromKey       },
    { "SetScancodeName",          l_K_SetScancodeName          },
    { "GetScancodeName",          l_K_GetScancodeName          },
    { "GetScancodeFromName",      l_K_GetScancodeFromName      },
    { "GetKeyName",               l_K_GetKeyName               },
    { "GetKeyFromName",           l_K_GetKeyFromName           },
    { nullptr, nullptr },
};

#define LK_PUSH_SC(name) do {                              \
    lua_pushinteger(L, (lua_Integer)SDL_SCANCODE_##name);  \
    lua_setfield(L, -2, "SCANCODE_" #name);                \
} while (0)

#define LK_PUSH_KMOD(name) do {                            \
    lua_pushinteger(L, (lua_Integer)SDL_KMOD_##name);      \
    lua_setfield(L, -2, "KMOD_" #name);                    \
} while (0)

extern "C" LIGHT_API int luaopen_Light_Keyboard(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kReg);

    // --- Letters
    LK_PUSH_SC(A); LK_PUSH_SC(B); LK_PUSH_SC(C); LK_PUSH_SC(D);
    LK_PUSH_SC(E); LK_PUSH_SC(F); LK_PUSH_SC(G); LK_PUSH_SC(H);
    LK_PUSH_SC(I); LK_PUSH_SC(J); LK_PUSH_SC(K); LK_PUSH_SC(L);
    LK_PUSH_SC(M); LK_PUSH_SC(N); LK_PUSH_SC(O); LK_PUSH_SC(P);
    LK_PUSH_SC(Q); LK_PUSH_SC(R); LK_PUSH_SC(S); LK_PUSH_SC(T);
    LK_PUSH_SC(U); LK_PUSH_SC(V); LK_PUSH_SC(W); LK_PUSH_SC(X);
    LK_PUSH_SC(Y); LK_PUSH_SC(Z);
    // --- Top-row digits
    LK_PUSH_SC(1); LK_PUSH_SC(2); LK_PUSH_SC(3); LK_PUSH_SC(4); LK_PUSH_SC(5);
    LK_PUSH_SC(6); LK_PUSH_SC(7); LK_PUSH_SC(8); LK_PUSH_SC(9); LK_PUSH_SC(0);
    // --- Function
    LK_PUSH_SC(F1); LK_PUSH_SC(F2); LK_PUSH_SC(F3); LK_PUSH_SC(F4);
    LK_PUSH_SC(F5); LK_PUSH_SC(F6); LK_PUSH_SC(F7); LK_PUSH_SC(F8);
    LK_PUSH_SC(F9); LK_PUSH_SC(F10); LK_PUSH_SC(F11); LK_PUSH_SC(F12);
    // --- Edit / nav
    LK_PUSH_SC(ESCAPE);
    LK_PUSH_SC(RETURN);
    LK_PUSH_SC(SPACE);
    LK_PUSH_SC(TAB);
    LK_PUSH_SC(BACKSPACE);
    LK_PUSH_SC(DELETE);
    LK_PUSH_SC(INSERT);
    LK_PUSH_SC(HOME);
    LK_PUSH_SC(END);
    LK_PUSH_SC(PAGEUP);
    LK_PUSH_SC(PAGEDOWN);
    // --- Arrows
    LK_PUSH_SC(LEFT); LK_PUSH_SC(RIGHT); LK_PUSH_SC(UP); LK_PUSH_SC(DOWN);
    // --- Punctuation
    LK_PUSH_SC(MINUS);
    LK_PUSH_SC(EQUALS);
    LK_PUSH_SC(LEFTBRACKET);
    LK_PUSH_SC(RIGHTBRACKET);
    LK_PUSH_SC(BACKSLASH);
    LK_PUSH_SC(SEMICOLON);
    LK_PUSH_SC(APOSTROPHE);
    LK_PUSH_SC(GRAVE);
    LK_PUSH_SC(COMMA);
    LK_PUSH_SC(PERIOD);
    LK_PUSH_SC(SLASH);
    // --- Modifiers
    LK_PUSH_SC(LCTRL); LK_PUSH_SC(RCTRL);
    LK_PUSH_SC(LSHIFT); LK_PUSH_SC(RSHIFT);
    LK_PUSH_SC(LALT); LK_PUSH_SC(RALT);
    LK_PUSH_SC(LGUI); LK_PUSH_SC(RGUI);
    // --- Keypad
    LK_PUSH_SC(KP_0); LK_PUSH_SC(KP_1); LK_PUSH_SC(KP_2); LK_PUSH_SC(KP_3);
    LK_PUSH_SC(KP_4); LK_PUSH_SC(KP_5); LK_PUSH_SC(KP_6); LK_PUSH_SC(KP_7);
    LK_PUSH_SC(KP_8); LK_PUSH_SC(KP_9);
    LK_PUSH_SC(KP_DIVIDE);
    LK_PUSH_SC(KP_MULTIPLY);
    LK_PUSH_SC(KP_MINUS);
    LK_PUSH_SC(KP_PLUS);
    LK_PUSH_SC(KP_ENTER);
    LK_PUSH_SC(KP_PERIOD);
    // --- Lock keys
    LK_PUSH_SC(CAPSLOCK);
    LK_PUSH_SC(NUMLOCKCLEAR);
    LK_PUSH_SC(SCROLLLOCK);
    LK_PUSH_SC(PRINTSCREEN);
    LK_PUSH_SC(PAUSE);

    // --- Keymod bits
    LK_PUSH_KMOD(NONE);
    LK_PUSH_KMOD(LSHIFT);  LK_PUSH_KMOD(RSHIFT);  LK_PUSH_KMOD(SHIFT);
    LK_PUSH_KMOD(LCTRL);   LK_PUSH_KMOD(RCTRL);   LK_PUSH_KMOD(CTRL);
    LK_PUSH_KMOD(LALT);    LK_PUSH_KMOD(RALT);    LK_PUSH_KMOD(ALT);
    LK_PUSH_KMOD(LGUI);    LK_PUSH_KMOD(RGUI);    LK_PUSH_KMOD(GUI);
    LK_PUSH_KMOD(NUM);     LK_PUSH_KMOD(CAPS);
    LK_PUSH_KMOD(MODE);    LK_PUSH_KMOD(SCROLL);

    // --- Useful sentinels
    lua_pushinteger(L, (lua_Integer)SDL_SCANCODE_UNKNOWN);
    lua_setfield(L, -2, "SCANCODE_UNKNOWN");
    lua_pushinteger(L, (lua_Integer)SDL_SCANCODE_COUNT);
    lua_setfield(L, -2, "SCANCODE_COUNT");
    return 1;
}
