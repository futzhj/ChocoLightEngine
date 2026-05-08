/**
 * @file light_atomic.cpp
 * @brief Light.Atomic module - SDL_atomic.h
 *
 * Atomic primitives are useful even in single-threaded Lua: they let Lua
 * coordinate state with C threads created by other modules (e.g. SDL_thread,
 * IO completion callbacks, audio device callbacks). The atomics live as
 * full userdata so the GC owns their storage.
 *
 * Lua API (15 fns):
 *
 *  Spinlock:
 *    NewSpinlock()         -> spinlock_userdata
 *    TryLockSpinlock(sl)   -> bool
 *    LockSpinlock(sl)
 *    UnlockSpinlock(sl)
 *
 *  Memory barriers:
 *    MemoryBarrierAcquire()
 *    MemoryBarrierRelease()
 *
 *  AtomicInt:
 *    NewAtomicInt([initial=0])              -> atomic_int_userdata
 *    CompareAndSwapAtomicInt(a, old, new)   -> bool   (true if swap happened)
 *    SetAtomicInt(a, v)                     -> previous_value
 *    GetAtomicInt(a)                        -> int
 *    AddAtomicInt(a, v)                     -> previous_value
 *
 *  AtomicU32:
 *    NewAtomicU32([initial=0])              -> atomic_u32_userdata
 *    CompareAndSwapAtomicU32(a, old, new)   -> bool
 *    SetAtomicU32(a, v)                     -> previous_value
 *    GetAtomicU32(a)                        -> uint32 (as Lua number)
 *
 * NOT bound:
 *    SDL_AtomicPointer (rarely needed from Lua; would require
 *    lightuserdata semantics for old/new operands)
 *    SDL_CompilerBarrier / SDL_CPUPauseInstruction (compiler intrinsics,
 *    no runtime use from Lua)
 *
 * Type safety:
 *    Each atomic uses a distinct metatable; passing a wrong-type userdata
 *    raises a Lua error via luaL_checkudata.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#define MT_ATOMIC_INT  "Light.Atomic.Int"
#define MT_ATOMIC_U32  "Light.Atomic.U32"
#define MT_SPINLOCK    "Light.Atomic.Spinlock"

// ============================================================
// Spinlock
// ============================================================
static int l_Atomic_NewSpinlock(lua_State* L) {
    SDL_SpinLock* p = (SDL_SpinLock*)lua_newuserdata(L, sizeof(SDL_SpinLock));
    *p = 0; // unlocked
    luaL_getmetatable(L, MT_SPINLOCK);
    lua_setmetatable(L, -2);
    return 1;
}

static SDL_SpinLock* CheckSpinlock(lua_State* L, int idx) {
    return (SDL_SpinLock*)luaL_checkudata(L, idx, MT_SPINLOCK);
}

static int l_Atomic_TryLockSpinlock(lua_State* L) {
    SDL_SpinLock* p = CheckSpinlock(L, 1);
    lua_pushboolean(L, SDL_TryLockSpinlock(p) ? 1 : 0);
    return 1;
}

static int l_Atomic_LockSpinlock(lua_State* L) {
    SDL_SpinLock* p = CheckSpinlock(L, 1);
    SDL_LockSpinlock(p);
    return 0;
}

static int l_Atomic_UnlockSpinlock(lua_State* L) {
    SDL_SpinLock* p = CheckSpinlock(L, 1);
    SDL_UnlockSpinlock(p);
    return 0;
}

// ============================================================
// Memory barriers
// ============================================================
static int l_Atomic_MemoryBarrierAcquire(lua_State* /*L*/) {
    SDL_MemoryBarrierAcquireFunction();
    return 0;
}

static int l_Atomic_MemoryBarrierRelease(lua_State* /*L*/) {
    SDL_MemoryBarrierReleaseFunction();
    return 0;
}

// ============================================================
// AtomicInt
// ============================================================
static SDL_AtomicInt* CheckAtomicInt(lua_State* L, int idx) {
    return (SDL_AtomicInt*)luaL_checkudata(L, idx, MT_ATOMIC_INT);
}

static int l_Atomic_NewAtomicInt(lua_State* L) {
    int initial = (int)luaL_optinteger(L, 1, 0);
    SDL_AtomicInt* p = (SDL_AtomicInt*)lua_newuserdata(L, sizeof(SDL_AtomicInt));
    SDL_SetAtomicInt(p, initial);
    luaL_getmetatable(L, MT_ATOMIC_INT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_Atomic_CompareAndSwapAtomicInt(lua_State* L) {
    SDL_AtomicInt* a = CheckAtomicInt(L, 1);
    int oldv = (int)luaL_checkinteger(L, 2);
    int newv = (int)luaL_checkinteger(L, 3);
    lua_pushboolean(L, SDL_CompareAndSwapAtomicInt(a, oldv, newv) ? 1 : 0);
    return 1;
}

static int l_Atomic_SetAtomicInt(lua_State* L) {
    SDL_AtomicInt* a = CheckAtomicInt(L, 1);
    int v = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, SDL_SetAtomicInt(a, v));
    return 1;
}

static int l_Atomic_GetAtomicInt(lua_State* L) {
    SDL_AtomicInt* a = CheckAtomicInt(L, 1);
    lua_pushinteger(L, SDL_GetAtomicInt(a));
    return 1;
}

static int l_Atomic_AddAtomicInt(lua_State* L) {
    SDL_AtomicInt* a = CheckAtomicInt(L, 1);
    int v = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, SDL_AddAtomicInt(a, v));
    return 1;
}

// ============================================================
// AtomicU32
// ============================================================
static SDL_AtomicU32* CheckAtomicU32(lua_State* L, int idx) {
    return (SDL_AtomicU32*)luaL_checkudata(L, idx, MT_ATOMIC_U32);
}

static int l_Atomic_NewAtomicU32(lua_State* L) {
    // Lua 5.1's lua_Number is double; cast through unsigned to keep
    // wrap-around semantics consistent.
    Uint32 initial = (Uint32)luaL_optnumber(L, 1, 0.0);
    SDL_AtomicU32* p = (SDL_AtomicU32*)lua_newuserdata(L, sizeof(SDL_AtomicU32));
    SDL_SetAtomicU32(p, initial);
    luaL_getmetatable(L, MT_ATOMIC_U32);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_Atomic_CompareAndSwapAtomicU32(lua_State* L) {
    SDL_AtomicU32* a = CheckAtomicU32(L, 1);
    Uint32 oldv = (Uint32)luaL_checknumber(L, 2);
    Uint32 newv = (Uint32)luaL_checknumber(L, 3);
    lua_pushboolean(L, SDL_CompareAndSwapAtomicU32(a, oldv, newv) ? 1 : 0);
    return 1;
}

static int l_Atomic_SetAtomicU32(lua_State* L) {
    SDL_AtomicU32* a = CheckAtomicU32(L, 1);
    Uint32 v = (Uint32)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)SDL_SetAtomicU32(a, v));
    return 1;
}

static int l_Atomic_GetAtomicU32(lua_State* L) {
    SDL_AtomicU32* a = CheckAtomicU32(L, 1);
    lua_pushnumber(L, (lua_Number)SDL_GetAtomicU32(a));
    return 1;
}

// ============================================================
// luaopen
// ============================================================
extern "C" LIGHT_API int luaopen_Light_Atomic(lua_State* L) {
    // Register metatables (no methods - just type tags).
    luaL_newmetatable(L, MT_SPINLOCK);   lua_pop(L, 1);
    luaL_newmetatable(L, MT_ATOMIC_INT); lua_pop(L, 1);
    luaL_newmetatable(L, MT_ATOMIC_U32); lua_pop(L, 1);

    static const luaL_Reg fns[] = {
        // Spinlock
        { "NewSpinlock",                  l_Atomic_NewSpinlock                  },
        { "TryLockSpinlock",              l_Atomic_TryLockSpinlock              },
        { "LockSpinlock",                 l_Atomic_LockSpinlock                 },
        { "UnlockSpinlock",               l_Atomic_UnlockSpinlock               },
        // Barriers
        { "MemoryBarrierAcquire",         l_Atomic_MemoryBarrierAcquire         },
        { "MemoryBarrierRelease",         l_Atomic_MemoryBarrierRelease         },
        // AtomicInt
        { "NewAtomicInt",                 l_Atomic_NewAtomicInt                 },
        { "CompareAndSwapAtomicInt",      l_Atomic_CompareAndSwapAtomicInt      },
        { "SetAtomicInt",                 l_Atomic_SetAtomicInt                 },
        { "GetAtomicInt",                 l_Atomic_GetAtomicInt                 },
        { "AddAtomicInt",                 l_Atomic_AddAtomicInt                 },
        // AtomicU32
        { "NewAtomicU32",                 l_Atomic_NewAtomicU32                 },
        { "CompareAndSwapAtomicU32",      l_Atomic_CompareAndSwapAtomicU32      },
        { "SetAtomicU32",                 l_Atomic_SetAtomicU32                 },
        { "GetAtomicU32",                 l_Atomic_GetAtomicU32                 },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
