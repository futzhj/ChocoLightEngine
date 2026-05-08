/**
 * @file light_clipboard.cpp
 * @brief Light.Clipboard module - System clipboard read/write (SDL_clipboard)
 *
 * Lua API (10 fns):
 *
 *  Text (cross-platform):
 *    Light.Clipboard.SetText(text)          -> ok, err|nil
 *    Light.Clipboard.GetText()              -> text|nil, err|nil
 *    Light.Clipboard.HasText()              -> bool
 *
 *  Primary selection (Linux/X11 only on most platforms):
 *    Light.Clipboard.SetPrimarySelectionText(text) -> ok, err
 *    Light.Clipboard.GetPrimarySelectionText()     -> text|nil, err
 *    Light.Clipboard.HasPrimarySelectionText()     -> bool
 *
 *  Typed data (MIME):
 *    Light.Clipboard.ClearClipboardData()        -> ok, err
 *    Light.Clipboard.GetClipboardData(mime)      -> data|nil, err   (binary string)
 *    Light.Clipboard.HasClipboardData(mime)      -> bool
 *    Light.Clipboard.GetClipboardMimeTypes()     -> {string,...}|nil, err
 *
 * NOT bound:
 *    SDL_SetClipboardData (requires C-callback trampoline + lifetime mgmt)
 *
 * Platform notes:
 *    - SDL clipboard requires SDL_INIT_VIDEO. We lazily init it and tolerate
 *      headless CI failure by surfacing the error to Lua.
 *    - Web target uses navigator.clipboard which may need a user gesture.
 *    - PrimarySelection is meaningful only on X11; elsewhere it usually
 *      returns "" / false.
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
static bool EnsureVideoInit(lua_State* /*L*/, const char** err_out) {
    // SDL_clipboard subsystem is owned by VIDEO. Idempotent init.
    if (SDL_WasInit(SDL_INIT_VIDEO)) return true;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) return true;
    *err_out = SDL_GetError();
    return false;
}

// ============================================================
// Text
// ============================================================
static int l_Clipboard_SetText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    bool ok = SDL_SetClipboardText(text);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) {
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushnil(L);
    return 2;
}

static int l_Clipboard_GetText(lua_State* L) {
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushnil(L);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    // SDL3 always returns a malloc'd string (empty when nothing); caller frees.
    char* text = SDL_GetClipboardText();
    if (!text) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushstring(L, text);
    SDL_free(text);
    lua_pushnil(L);
    return 2;
}

static int l_Clipboard_HasText(lua_State* L) {
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HasClipboardText() ? 1 : 0);
    return 1;
}

// ============================================================
// Primary selection (X11)
// ============================================================
static int l_Clipboard_SetPrimarySelectionText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    bool ok = SDL_SetPrimarySelectionText(text);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Clipboard_GetPrimarySelectionText(lua_State* L) {
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushnil(L);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    char* text = SDL_GetPrimarySelectionText();
    if (!text) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, text);
    SDL_free(text);
    lua_pushnil(L);
    return 2;
}

static int l_Clipboard_HasPrimarySelectionText(lua_State* L) {
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HasPrimarySelectionText() ? 1 : 0);
    return 1;
}

// ============================================================
// Typed clipboard data (MIME)
// ============================================================
static int l_Clipboard_ClearClipboardData(lua_State* L) {
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    bool ok = SDL_ClearClipboardData();
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    return 1;
}

static int l_Clipboard_GetClipboardData(lua_State* L) {
    const char* mime = luaL_checkstring(L, 1);
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushnil(L);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    size_t size = 0;
    void* data = SDL_GetClipboardData(mime, &size);
    if (!data) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "no data for mime type");
        return 2;
    }
    lua_pushlstring(L, (const char*)data, size);
    SDL_free(data);
    return 1;
}

static int l_Clipboard_HasClipboardData(lua_State* L) {
    const char* mime = luaL_checkstring(L, 1);
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HasClipboardData(mime) ? 1 : 0);
    return 1;
}

static int l_Clipboard_GetClipboardMimeTypes(lua_State* L) {
    const char* ierr = nullptr;
    if (!EnsureVideoInit(L, &ierr)) {
        lua_pushnil(L);
        lua_pushstring(L, ierr ? ierr : "SDL video init failed");
        return 2;
    }
    size_t count = 0;
    char** mimes = SDL_GetClipboardMimeTypes(&count);
    if (!mimes) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetClipboardMimeTypes failed");
        return 2;
    }
    // The pointer block + each string are allocated by SDL; SDL_free on the
    // outer pointer is enough (header documents single-allocation).
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; ++i) {
        lua_pushstring(L, mimes[i] ? mimes[i] : "");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    SDL_free(mimes);
    return 1;
}

// ============================================================
// luaopen
// ============================================================
extern "C" LIGHT_API int luaopen_Light_Clipboard(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "SetText",                      l_Clipboard_SetText                      },
        { "GetText",                      l_Clipboard_GetText                      },
        { "HasText",                      l_Clipboard_HasText                      },
        { "SetPrimarySelectionText",      l_Clipboard_SetPrimarySelectionText      },
        { "GetPrimarySelectionText",      l_Clipboard_GetPrimarySelectionText      },
        { "HasPrimarySelectionText",      l_Clipboard_HasPrimarySelectionText      },
        { "ClearClipboardData",           l_Clipboard_ClearClipboardData           },
        { "GetClipboardData",             l_Clipboard_GetClipboardData             },
        { "HasClipboardData",             l_Clipboard_HasClipboardData             },
        { "GetClipboardMimeTypes",        l_Clipboard_GetClipboardMimeTypes        },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
