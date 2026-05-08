/**
 * @file light_iostream.cpp
 * @brief Light.IOStream module - SDL_iostream.h
 *
 * Foundational binary I/O abstraction. Required by future image / audio /
 * font modules that accept SDL_IOStream*. The Lua API mirrors SDL with a
 * userdata handle and __gc auto-close.
 *
 * Lua API (45 fns):
 *
 *  Construction:
 *    IOFromFile(path, mode)     -> ud | nil, err   (mode: "rb","wb","r+b",...)
 *    IOFromMem(string)          -> ud | nil, err   (writable copy of bytes)
 *    IOFromConstMem(string)     -> ud | nil, err   (read-only copy of bytes)
 *    IOFromDynamicMem()         -> ud | nil, err   (growable in-memory buffer)
 *    CloseIO(ud)                -> bool            (also auto-called by GC)
 *
 *  Metadata / status:
 *    GetIOProperties(ud)        -> properties_id (number) | 0
 *    GetIOStatus(ud)            -> integer enum
 *    GetIOSize(ud)              -> size (Sint64 as Lua number) | -1
 *    FlushIO(ud)                -> bool
 *
 *  Position:
 *    SeekIO(ud, offset, whence) -> new_pos | -1
 *    TellIO(ud)                 -> pos | -1
 *
 *  Bulk binary:
 *    ReadIO(ud, size)           -> string|nil, bytes_read
 *    WriteIO(ud, string)        -> bytes_written
 *
 *  File helpers (no IOStream needed):
 *    LoadFile(path)             -> string | nil, err
 *    SaveFile(path, string)     -> bool, err
 *    LoadFile_IO(ud, closeio)   -> string | nil, err
 *    SaveFile_IO(ud, string, closeio) -> bool, err
 *
 *  Typed read/write (28 fns: U8/S8/{U,S}{16,32,64}{LE,BE}):
 *    Read*(ud)  -> value (Lua number) | nil
 *    Write*(ud, value) -> bool
 *
 *  Constants:
 *    SEEK_SET=0, SEEK_CUR=1, SEEK_END=2
 *    STATUS_READY=0 ... STATUS_WRITEONLY=5
 *
 * Lifetime / ownership:
 *    LIOStream userdata stores the SDL_IOStream* plus an optional
 *    SDL_malloc'd payload buffer for IOFromMem / IOFromConstMem variants.
 *    GC closes the IOStream and frees the buffer. Calling CloseIO twice
 *    is safe (no-op the second time).
 *
 * NOT bound:
 *    SDL_OpenIO   (needs C-side vtable; no clean Lua surface)
 *    SDL_IOprintf / SDL_IOvprintf (use string.format + WriteIO instead)
 *
 * 64-bit precision note:
 *    Lua 5.1 numbers are doubles, so values >= 2^53 lose precision when
 *    round-tripped through Read/WriteU64*. Smoke tests stay below 2^32.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#define MT_IOSTREAM "Light.IOStream.Stream"

struct LIOStream {
    SDL_IOStream* io;
    void*         mem;   // owned buffer for IOFromMem / IOFromConstMem variants
};

// ============================================================
// helpers
// ============================================================
static LIOStream* CheckHandle(lua_State* L, int idx) {
    return (LIOStream*)luaL_checkudata(L, idx, MT_IOSTREAM);
}

static SDL_IOStream* CheckLive(lua_State* L, int idx) {
    LIOStream* h = CheckHandle(L, idx);
    if (!h->io) luaL_error(L, "iostream is closed");
    return h->io;
}

static void CloseImpl(LIOStream* h) {
    if (h->io) { SDL_CloseIO(h->io); h->io = nullptr; }
    if (h->mem) { SDL_free(h->mem); h->mem = nullptr; }
}

static int PushSdlError(lua_State* L) {
    lua_pushnil(L);
    const char* e = SDL_GetError();
    lua_pushstring(L, (e && *e) ? e : "SDL error");
    return 2;
}

static int NewHandle(lua_State* L, SDL_IOStream* io, void* owned_mem) {
    if (!io) {
        if (owned_mem) SDL_free(owned_mem);
        return PushSdlError(L);
    }
    LIOStream* h = (LIOStream*)lua_newuserdata(L, sizeof(LIOStream));
    h->io  = io;
    h->mem = owned_mem;
    luaL_getmetatable(L, MT_IOSTREAM);
    lua_setmetatable(L, -2);
    return 1;
}

// ============================================================
// Construction
// ============================================================
static int l_IO_IOFromFile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const char* mode = luaL_checkstring(L, 2);
    return NewHandle(L, SDL_IOFromFile(path, mode), nullptr);
}

static int l_IO_IOFromMem(lua_State* L) {
    size_t sz = 0;
    const char* src = luaL_checklstring(L, 1, &sz);
    if (sz == 0) {
        // SDL_IOFromMem rejects size==0; emulate empty writable buffer with
        // a 1-byte allocation that the IOStream sees as size 1; but cleaner
        // to error out so callers know.
        return luaL_error(L, "IOFromMem: empty string not supported");
    }
    void* copy = SDL_malloc(sz);
    if (!copy) return luaL_error(L, "out of memory");
    memcpy(copy, src, sz);
    SDL_IOStream* io = SDL_IOFromMem(copy, sz);
    return NewHandle(L, io, copy);
}

static int l_IO_IOFromConstMem(lua_State* L) {
    size_t sz = 0;
    const char* src = luaL_checklstring(L, 1, &sz);
    if (sz == 0) return luaL_error(L, "IOFromConstMem: empty string not supported");
    void* copy = SDL_malloc(sz);
    if (!copy) return luaL_error(L, "out of memory");
    memcpy(copy, src, sz);
    SDL_IOStream* io = SDL_IOFromConstMem(copy, sz);
    return NewHandle(L, io, copy);
}

static int l_IO_IOFromDynamicMem(lua_State* L) {
    return NewHandle(L, SDL_IOFromDynamicMem(), nullptr);
}

static int l_IO_CloseIO(lua_State* L) {
    LIOStream* h = CheckHandle(L, 1);
    bool ok = true;
    if (h->io) {
        ok = SDL_CloseIO(h->io);
        h->io = nullptr;
    }
    if (h->mem) { SDL_free(h->mem); h->mem = nullptr; }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int l_IO_Gc(lua_State* L) {
    LIOStream* h = (LIOStream*)lua_touserdata(L, 1);
    if (h) CloseImpl(h);
    return 0;
}

// ============================================================
// Metadata / status
// ============================================================
static int l_IO_GetIOProperties(lua_State* L) {
    SDL_PropertiesID p = SDL_GetIOProperties(CheckLive(L, 1));
    lua_pushnumber(L, (lua_Number)p);
    return 1;
}

static int l_IO_GetIOStatus(lua_State* L) {
    SDL_IOStatus s = SDL_GetIOStatus(CheckLive(L, 1));
    lua_pushinteger(L, (lua_Integer)s);
    return 1;
}

static int l_IO_GetIOSize(lua_State* L) {
    Sint64 sz = SDL_GetIOSize(CheckLive(L, 1));
    lua_pushnumber(L, (lua_Number)sz);
    return 1;
}

static int l_IO_FlushIO(lua_State* L) {
    lua_pushboolean(L, SDL_FlushIO(CheckLive(L, 1)) ? 1 : 0);
    return 1;
}

// ============================================================
// Position
// ============================================================
static int l_IO_SeekIO(lua_State* L) {
    SDL_IOStream* io = CheckLive(L, 1);
    Sint64 off = (Sint64)luaL_checknumber(L, 2);
    int whence = (int)luaL_checkinteger(L, 3);
    Sint64 r = SDL_SeekIO(io, off, (SDL_IOWhence)whence);
    lua_pushnumber(L, (lua_Number)r);
    return 1;
}

static int l_IO_TellIO(lua_State* L) {
    Sint64 r = SDL_TellIO(CheckLive(L, 1));
    lua_pushnumber(L, (lua_Number)r);
    return 1;
}

// ============================================================
// Bulk binary
// ============================================================
static int l_IO_ReadIO(lua_State* L) {
    SDL_IOStream* io = CheckLive(L, 1);
    size_t want = (size_t)luaL_checknumber(L, 2);
    if (want == 0) {
        lua_pushlstring(L, "", 0);
        lua_pushinteger(L, 0);
        return 2;
    }
    void* buf = SDL_malloc(want);
    if (!buf) return luaL_error(L, "out of memory");
    size_t got = SDL_ReadIO(io, buf, want);
    if (got == 0) {
        SDL_free(buf);
        lua_pushnil(L);
        lua_pushinteger(L, 0);
        return 2;
    }
    lua_pushlstring(L, (const char*)buf, got);
    SDL_free(buf);
    lua_pushinteger(L, (lua_Integer)got);
    return 2;
}

static int l_IO_WriteIO(lua_State* L) {
    SDL_IOStream* io = CheckLive(L, 1);
    size_t sz = 0;
    const char* data = luaL_checklstring(L, 2, &sz);
    size_t wrote = SDL_WriteIO(io, data, sz);
    lua_pushinteger(L, (lua_Integer)wrote);
    return 1;
}

// ============================================================
// File helpers (path-based)
// ============================================================
static int l_IO_LoadFile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    size_t sz = 0;
    void* data = SDL_LoadFile(path, &sz);
    if (!data) return PushSdlError(L);
    lua_pushlstring(L, (const char*)data, sz);
    SDL_free(data);
    return 1;
}

static int l_IO_SaveFile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    size_t sz = 0;
    const char* data = luaL_checklstring(L, 2, &sz);
    if (!SDL_SaveFile(path, data, sz)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SaveFile failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_IO_LoadFile_IO(lua_State* L) {
    LIOStream* h = CheckHandle(L, 1);
    if (!h->io) return luaL_error(L, "iostream is closed");
    bool closeio = lua_toboolean(L, 2) ? true : false;
    size_t sz = 0;
    void* data = SDL_LoadFile_IO(h->io, &sz, closeio);
    if (closeio) {
        // SDL_LoadFile_IO closes the stream regardless of success/failure
        // when closeio is true; mark our handle dead.
        h->io = nullptr;
        if (h->mem) { SDL_free(h->mem); h->mem = nullptr; }
    }
    if (!data) return PushSdlError(L);
    lua_pushlstring(L, (const char*)data, sz);
    SDL_free(data);
    return 1;
}

static int l_IO_SaveFile_IO(lua_State* L) {
    LIOStream* h = CheckHandle(L, 1);
    if (!h->io) return luaL_error(L, "iostream is closed");
    size_t sz = 0;
    const char* data = luaL_checklstring(L, 2, &sz);
    bool closeio = lua_toboolean(L, 3) ? true : false;
    bool ok = SDL_SaveFile_IO(h->io, data, sz, closeio);
    if (closeio) {
        h->io = nullptr;
        if (h->mem) { SDL_free(h->mem); h->mem = nullptr; }
    }
    if (!ok) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SaveFile_IO failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// Typed read/write (28 fns)
// Macro factory keeps the file compact and uniform.
// ============================================================
#define LIO_READ(NAME, CTYPE, PUSH)                                  \
    static int l_IO_Read##NAME(lua_State* L) {                       \
        SDL_IOStream* io = CheckLive(L, 1);                          \
        CTYPE v;                                                     \
        if (!SDL_Read##NAME(io, &v)) { lua_pushnil(L); return 1; }   \
        PUSH(L, v);                                                  \
        return 1;                                                    \
    }

#define LIO_WRITE(NAME, CTYPE, FETCH)                                \
    static int l_IO_Write##NAME(lua_State* L) {                      \
        SDL_IOStream* io = CheckLive(L, 1);                          \
        CTYPE v = (CTYPE)FETCH(L, 2);                                \
        lua_pushboolean(L, SDL_Write##NAME(io, v) ? 1 : 0);          \
        return 1;                                                    \
    }

static void PushI(lua_State* L, lua_Integer v) { lua_pushinteger(L, v); }
static void PushN(lua_State* L, lua_Number v)  { lua_pushnumber(L, v); }

// Helpers to push CTYPE through correct Lua API path.
static void PushU8(lua_State* L, Uint8 v)   { PushI(L, (lua_Integer)v); }
static void PushS8(lua_State* L, Sint8 v)   { PushI(L, (lua_Integer)v); }
static void PushU16(lua_State* L, Uint16 v) { PushI(L, (lua_Integer)v); }
static void PushS16(lua_State* L, Sint16 v) { PushI(L, (lua_Integer)v); }
static void PushU32(lua_State* L, Uint32 v) { PushN(L, (lua_Number)v); }
static void PushS32(lua_State* L, Sint32 v) { PushI(L, (lua_Integer)v); }
static void PushU64(lua_State* L, Uint64 v) { PushN(L, (lua_Number)v); }
static void PushS64(lua_State* L, Sint64 v) { PushN(L, (lua_Number)v); }

LIO_READ(U8,    Uint8,  PushU8)
LIO_READ(S8,    Sint8,  PushS8)
LIO_READ(U16LE, Uint16, PushU16)
LIO_READ(S16LE, Sint16, PushS16)
LIO_READ(U16BE, Uint16, PushU16)
LIO_READ(S16BE, Sint16, PushS16)
LIO_READ(U32LE, Uint32, PushU32)
LIO_READ(S32LE, Sint32, PushS32)
LIO_READ(U32BE, Uint32, PushU32)
LIO_READ(S32BE, Sint32, PushS32)
LIO_READ(U64LE, Uint64, PushU64)
LIO_READ(S64LE, Sint64, PushS64)
LIO_READ(U64BE, Uint64, PushU64)
LIO_READ(S64BE, Sint64, PushS64)

LIO_WRITE(U8,    Uint8,  luaL_checkinteger)
LIO_WRITE(S8,    Sint8,  luaL_checkinteger)
LIO_WRITE(U16LE, Uint16, luaL_checkinteger)
LIO_WRITE(S16LE, Sint16, luaL_checkinteger)
LIO_WRITE(U16BE, Uint16, luaL_checkinteger)
LIO_WRITE(S16BE, Sint16, luaL_checkinteger)
LIO_WRITE(U32LE, Uint32, luaL_checknumber)
LIO_WRITE(S32LE, Sint32, luaL_checkinteger)
LIO_WRITE(U32BE, Uint32, luaL_checknumber)
LIO_WRITE(S32BE, Sint32, luaL_checkinteger)
LIO_WRITE(U64LE, Uint64, luaL_checknumber)
LIO_WRITE(S64LE, Sint64, luaL_checknumber)
LIO_WRITE(U64BE, Uint64, luaL_checknumber)
LIO_WRITE(S64BE, Sint64, luaL_checknumber)

// ============================================================
// luaopen
// ============================================================
static void RegisterMetatable(lua_State* L) {
    luaL_newmetatable(L, MT_IOSTREAM);
    lua_pushcfunction(L, l_IO_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

static void SetIntField(lua_State* L, const char* name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

extern "C" LIGHT_API int luaopen_Light_IOStream(lua_State* L) {
    RegisterMetatable(L);

    static const luaL_Reg fns[] = {
        // Construction
        { "IOFromFile",        l_IO_IOFromFile        },
        { "IOFromMem",         l_IO_IOFromMem         },
        { "IOFromConstMem",    l_IO_IOFromConstMem    },
        { "IOFromDynamicMem",  l_IO_IOFromDynamicMem  },
        { "CloseIO",           l_IO_CloseIO           },
        // Metadata
        { "GetIOProperties",   l_IO_GetIOProperties   },
        { "GetIOStatus",       l_IO_GetIOStatus       },
        { "GetIOSize",         l_IO_GetIOSize         },
        { "FlushIO",           l_IO_FlushIO           },
        // Position
        { "SeekIO",            l_IO_SeekIO            },
        { "TellIO",            l_IO_TellIO            },
        // Bulk
        { "ReadIO",            l_IO_ReadIO            },
        { "WriteIO",           l_IO_WriteIO           },
        // File helpers
        { "LoadFile",          l_IO_LoadFile          },
        { "SaveFile",          l_IO_SaveFile          },
        { "LoadFile_IO",       l_IO_LoadFile_IO       },
        { "SaveFile_IO",       l_IO_SaveFile_IO       },
        // Typed read
        { "ReadU8",            l_IO_ReadU8            },
        { "ReadS8",            l_IO_ReadS8            },
        { "ReadU16LE",         l_IO_ReadU16LE         },
        { "ReadS16LE",         l_IO_ReadS16LE         },
        { "ReadU16BE",         l_IO_ReadU16BE         },
        { "ReadS16BE",         l_IO_ReadS16BE         },
        { "ReadU32LE",         l_IO_ReadU32LE         },
        { "ReadS32LE",         l_IO_ReadS32LE         },
        { "ReadU32BE",         l_IO_ReadU32BE         },
        { "ReadS32BE",         l_IO_ReadS32BE         },
        { "ReadU64LE",         l_IO_ReadU64LE         },
        { "ReadS64LE",         l_IO_ReadS64LE         },
        { "ReadU64BE",         l_IO_ReadU64BE         },
        { "ReadS64BE",         l_IO_ReadS64BE         },
        // Typed write
        { "WriteU8",           l_IO_WriteU8           },
        { "WriteS8",           l_IO_WriteS8           },
        { "WriteU16LE",        l_IO_WriteU16LE        },
        { "WriteS16LE",        l_IO_WriteS16LE        },
        { "WriteU16BE",        l_IO_WriteU16BE        },
        { "WriteS16BE",        l_IO_WriteS16BE        },
        { "WriteU32LE",        l_IO_WriteU32LE        },
        { "WriteS32LE",        l_IO_WriteS32LE        },
        { "WriteU32BE",        l_IO_WriteU32BE        },
        { "WriteS32BE",        l_IO_WriteS32BE        },
        { "WriteU64LE",        l_IO_WriteU64LE        },
        { "WriteS64LE",        l_IO_WriteS64LE        },
        { "WriteU64BE",        l_IO_WriteU64BE        },
        { "WriteS64BE",        l_IO_WriteS64BE        },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);

    // Constants
    SetIntField(L, "SEEK_SET",          (lua_Integer)SDL_IO_SEEK_SET);
    SetIntField(L, "SEEK_CUR",          (lua_Integer)SDL_IO_SEEK_CUR);
    SetIntField(L, "SEEK_END",          (lua_Integer)SDL_IO_SEEK_END);
    SetIntField(L, "STATUS_READY",      (lua_Integer)SDL_IO_STATUS_READY);
    SetIntField(L, "STATUS_ERROR",      (lua_Integer)SDL_IO_STATUS_ERROR);
    SetIntField(L, "STATUS_EOF",        (lua_Integer)SDL_IO_STATUS_EOF);
    SetIntField(L, "STATUS_NOT_READY",  (lua_Integer)SDL_IO_STATUS_NOT_READY);
    SetIntField(L, "STATUS_READONLY",   (lua_Integer)SDL_IO_STATUS_READONLY);
    SetIntField(L, "STATUS_WRITEONLY",  (lua_Integer)SDL_IO_STATUS_WRITEONLY);
    return 1;
}
