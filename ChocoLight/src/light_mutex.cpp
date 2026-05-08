/**
 * @file light_mutex.cpp
 * @brief Light.Mutex module - SDL_mutex.h
 *
 * Synchronization primitives. Lua is single-threaded, so these are mostly
 * useful when shared with C-side threads (audio callbacks, future
 * SDL_thread bindings). Smoke tests therefore assert API shape and
 * single-thread-safe sequences only (no contention scenarios).
 *
 * Lua API (24 fns):
 *
 *  Mutex:
 *    NewMutex()        -> mutex_ud | nil, err
 *    LockMutex(m)
 *    TryLockMutex(m)   -> bool
 *    UnlockMutex(m)
 *    DestroyMutex(m)             (also auto-released by GC)
 *
 *  RWLock:
 *    NewRWLock()                  -> rwlock_ud | nil, err
 *    LockRWLockForReading(rw)
 *    LockRWLockForWriting(rw)
 *    TryLockRWLockForReading(rw)  -> bool
 *    TryLockRWLockForWriting(rw)  -> bool
 *    UnlockRWLock(rw)
 *    DestroyRWLock(rw)
 *
 *  Semaphore:
 *    NewSemaphore(initial)        -> sem_ud | nil, err
 *    DestroySemaphore(sem)
 *    WaitSemaphore(sem)
 *    TryWaitSemaphore(sem)        -> bool
 *    WaitSemaphoreTimeout(sem, ms)-> bool
 *    SignalSemaphore(sem)
 *    GetSemaphoreValue(sem)       -> uint32 (as Lua number)
 *
 *  Condition:
 *    NewCondition()               -> cond_ud | nil, err
 *    DestroyCondition(cond)
 *    SignalCondition(cond)
 *    BroadcastCondition(cond)
 *    WaitCondition(cond, mutex)
 *    WaitConditionTimeout(cond, mutex, ms) -> bool
 *
 * NOT bound:
 *    SDL_InitState helpers (SDL_ShouldInit/SDL_ShouldQuit/SDL_SetInitialized)
 *    - These are intended for C subsystem boot races and have no clean
 *      Lua semantics; the InitState struct must live in stable C memory.
 *
 * Lifetime:
 *    Each handle is a full userdata holding the raw SDL_* pointer plus a
 *    "destroyed" flag. The __gc metamethod destroys the underlying object
 *    if Destroy<X> has not been called explicitly. Calling Destroy<X>
 *    twice is a no-op.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#define MT_MUTEX      "Light.Mutex.Mutex"
#define MT_RWLOCK     "Light.Mutex.RWLock"
#define MT_SEMAPHORE  "Light.Mutex.Semaphore"
#define MT_CONDITION  "Light.Mutex.Condition"

// ============================================================
// Generic handle wrapper
// ============================================================
struct LMutexHandle    { SDL_Mutex*     p; };
struct LRWLockHandle   { SDL_RWLock*    p; };
struct LSemaphoreHandle{ SDL_Semaphore* p; };
struct LConditionHandle{ SDL_Condition* p; };

// ============================================================
// Mutex
// ============================================================
static LMutexHandle* CheckMutexHandle(lua_State* L, int idx) {
    return (LMutexHandle*)luaL_checkudata(L, idx, MT_MUTEX);
}

static SDL_Mutex* CheckLiveMutex(lua_State* L, int idx) {
    LMutexHandle* h = CheckMutexHandle(L, idx);
    if (!h->p) luaL_error(L, "mutex has been destroyed");
    return h->p;
}

static int l_Mutex_NewMutex(lua_State* L) {
    LMutexHandle* h = (LMutexHandle*)lua_newuserdata(L, sizeof(LMutexHandle));
    h->p = nullptr;
    luaL_getmetatable(L, MT_MUTEX);
    lua_setmetatable(L, -2);
    h->p = SDL_CreateMutex();
    if (!h->p) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    return 1;
}

static int l_Mutex_LockMutex(lua_State* L) {
    SDL_LockMutex(CheckLiveMutex(L, 1));
    return 0;
}

static int l_Mutex_TryLockMutex(lua_State* L) {
    lua_pushboolean(L, SDL_TryLockMutex(CheckLiveMutex(L, 1)) ? 1 : 0);
    return 1;
}

static int l_Mutex_UnlockMutex(lua_State* L) {
    SDL_UnlockMutex(CheckLiveMutex(L, 1));
    return 0;
}

static int l_Mutex_DestroyMutex(lua_State* L) {
    LMutexHandle* h = CheckMutexHandle(L, 1);
    if (h->p) { SDL_DestroyMutex(h->p); h->p = nullptr; }
    return 0;
}

static int l_Mutex_GcMutex(lua_State* L) {
    LMutexHandle* h = (LMutexHandle*)lua_touserdata(L, 1);
    if (h && h->p) { SDL_DestroyMutex(h->p); h->p = nullptr; }
    return 0;
}

// ============================================================
// RWLock
// ============================================================
static LRWLockHandle* CheckRWLockHandle(lua_State* L, int idx) {
    return (LRWLockHandle*)luaL_checkudata(L, idx, MT_RWLOCK);
}

static SDL_RWLock* CheckLiveRWLock(lua_State* L, int idx) {
    LRWLockHandle* h = CheckRWLockHandle(L, idx);
    if (!h->p) luaL_error(L, "rwlock has been destroyed");
    return h->p;
}

static int l_Mutex_NewRWLock(lua_State* L) {
    LRWLockHandle* h = (LRWLockHandle*)lua_newuserdata(L, sizeof(LRWLockHandle));
    h->p = nullptr;
    luaL_getmetatable(L, MT_RWLOCK);
    lua_setmetatable(L, -2);
    h->p = SDL_CreateRWLock();
    if (!h->p) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    return 1;
}

static int l_Mutex_LockRWLockForReading(lua_State* L) {
    SDL_LockRWLockForReading(CheckLiveRWLock(L, 1));
    return 0;
}

static int l_Mutex_LockRWLockForWriting(lua_State* L) {
    SDL_LockRWLockForWriting(CheckLiveRWLock(L, 1));
    return 0;
}

static int l_Mutex_TryLockRWLockForReading(lua_State* L) {
    lua_pushboolean(L, SDL_TryLockRWLockForReading(CheckLiveRWLock(L, 1)) ? 1 : 0);
    return 1;
}

static int l_Mutex_TryLockRWLockForWriting(lua_State* L) {
    lua_pushboolean(L, SDL_TryLockRWLockForWriting(CheckLiveRWLock(L, 1)) ? 1 : 0);
    return 1;
}

static int l_Mutex_UnlockRWLock(lua_State* L) {
    SDL_UnlockRWLock(CheckLiveRWLock(L, 1));
    return 0;
}

static int l_Mutex_DestroyRWLock(lua_State* L) {
    LRWLockHandle* h = CheckRWLockHandle(L, 1);
    if (h->p) { SDL_DestroyRWLock(h->p); h->p = nullptr; }
    return 0;
}

static int l_Mutex_GcRWLock(lua_State* L) {
    LRWLockHandle* h = (LRWLockHandle*)lua_touserdata(L, 1);
    if (h && h->p) { SDL_DestroyRWLock(h->p); h->p = nullptr; }
    return 0;
}

// ============================================================
// Semaphore
// ============================================================
static LSemaphoreHandle* CheckSemaphoreHandle(lua_State* L, int idx) {
    return (LSemaphoreHandle*)luaL_checkudata(L, idx, MT_SEMAPHORE);
}

static SDL_Semaphore* CheckLiveSemaphore(lua_State* L, int idx) {
    LSemaphoreHandle* h = CheckSemaphoreHandle(L, idx);
    if (!h->p) luaL_error(L, "semaphore has been destroyed");
    return h->p;
}

static int l_Mutex_NewSemaphore(lua_State* L) {
    Uint32 initial = (Uint32)luaL_optnumber(L, 1, 0.0);
    LSemaphoreHandle* h = (LSemaphoreHandle*)lua_newuserdata(L, sizeof(LSemaphoreHandle));
    h->p = nullptr;
    luaL_getmetatable(L, MT_SEMAPHORE);
    lua_setmetatable(L, -2);
    h->p = SDL_CreateSemaphore(initial);
    if (!h->p) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    return 1;
}

static int l_Mutex_DestroySemaphore(lua_State* L) {
    LSemaphoreHandle* h = CheckSemaphoreHandle(L, 1);
    if (h->p) { SDL_DestroySemaphore(h->p); h->p = nullptr; }
    return 0;
}

static int l_Mutex_WaitSemaphore(lua_State* L) {
    SDL_WaitSemaphore(CheckLiveSemaphore(L, 1));
    return 0;
}

static int l_Mutex_TryWaitSemaphore(lua_State* L) {
    lua_pushboolean(L, SDL_TryWaitSemaphore(CheckLiveSemaphore(L, 1)) ? 1 : 0);
    return 1;
}

static int l_Mutex_WaitSemaphoreTimeout(lua_State* L) {
    Sint32 ms = (Sint32)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_WaitSemaphoreTimeout(CheckLiveSemaphore(L, 1), ms) ? 1 : 0);
    return 1;
}

static int l_Mutex_SignalSemaphore(lua_State* L) {
    SDL_SignalSemaphore(CheckLiveSemaphore(L, 1));
    return 0;
}

static int l_Mutex_GetSemaphoreValue(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetSemaphoreValue(CheckLiveSemaphore(L, 1)));
    return 1;
}

static int l_Mutex_GcSemaphore(lua_State* L) {
    LSemaphoreHandle* h = (LSemaphoreHandle*)lua_touserdata(L, 1);
    if (h && h->p) { SDL_DestroySemaphore(h->p); h->p = nullptr; }
    return 0;
}

// ============================================================
// Condition
// ============================================================
static LConditionHandle* CheckConditionHandle(lua_State* L, int idx) {
    return (LConditionHandle*)luaL_checkudata(L, idx, MT_CONDITION);
}

static SDL_Condition* CheckLiveCondition(lua_State* L, int idx) {
    LConditionHandle* h = CheckConditionHandle(L, idx);
    if (!h->p) luaL_error(L, "condition has been destroyed");
    return h->p;
}

static int l_Mutex_NewCondition(lua_State* L) {
    LConditionHandle* h = (LConditionHandle*)lua_newuserdata(L, sizeof(LConditionHandle));
    h->p = nullptr;
    luaL_getmetatable(L, MT_CONDITION);
    lua_setmetatable(L, -2);
    h->p = SDL_CreateCondition();
    if (!h->p) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    return 1;
}

static int l_Mutex_DestroyCondition(lua_State* L) {
    LConditionHandle* h = CheckConditionHandle(L, 1);
    if (h->p) { SDL_DestroyCondition(h->p); h->p = nullptr; }
    return 0;
}

static int l_Mutex_SignalCondition(lua_State* L) {
    SDL_SignalCondition(CheckLiveCondition(L, 1));
    return 0;
}

static int l_Mutex_BroadcastCondition(lua_State* L) {
    SDL_BroadcastCondition(CheckLiveCondition(L, 1));
    return 0;
}

static int l_Mutex_WaitCondition(lua_State* L) {
    SDL_Condition* c = CheckLiveCondition(L, 1);
    SDL_Mutex* m = CheckLiveMutex(L, 2);
    SDL_WaitCondition(c, m);
    return 0;
}

static int l_Mutex_WaitConditionTimeout(lua_State* L) {
    SDL_Condition* c = CheckLiveCondition(L, 1);
    SDL_Mutex* m = CheckLiveMutex(L, 2);
    Sint32 ms = (Sint32)luaL_checkinteger(L, 3);
    lua_pushboolean(L, SDL_WaitConditionTimeout(c, m, ms) ? 1 : 0);
    return 1;
}

static int l_Mutex_GcCondition(lua_State* L) {
    LConditionHandle* h = (LConditionHandle*)lua_touserdata(L, 1);
    if (h && h->p) { SDL_DestroyCondition(h->p); h->p = nullptr; }
    return 0;
}

// ============================================================
// luaopen
// ============================================================
static void RegisterMetatable(lua_State* L, const char* name, lua_CFunction gc) {
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

extern "C" LIGHT_API int luaopen_Light_Mutex(lua_State* L) {
    RegisterMetatable(L, MT_MUTEX,     l_Mutex_GcMutex);
    RegisterMetatable(L, MT_RWLOCK,    l_Mutex_GcRWLock);
    RegisterMetatable(L, MT_SEMAPHORE, l_Mutex_GcSemaphore);
    RegisterMetatable(L, MT_CONDITION, l_Mutex_GcCondition);

    static const luaL_Reg fns[] = {
        // Mutex
        { "NewMutex",                  l_Mutex_NewMutex                  },
        { "LockMutex",                 l_Mutex_LockMutex                 },
        { "TryLockMutex",              l_Mutex_TryLockMutex              },
        { "UnlockMutex",               l_Mutex_UnlockMutex               },
        { "DestroyMutex",              l_Mutex_DestroyMutex              },
        // RWLock
        { "NewRWLock",                 l_Mutex_NewRWLock                 },
        { "LockRWLockForReading",      l_Mutex_LockRWLockForReading      },
        { "LockRWLockForWriting",      l_Mutex_LockRWLockForWriting      },
        { "TryLockRWLockForReading",   l_Mutex_TryLockRWLockForReading   },
        { "TryLockRWLockForWriting",   l_Mutex_TryLockRWLockForWriting   },
        { "UnlockRWLock",              l_Mutex_UnlockRWLock              },
        { "DestroyRWLock",             l_Mutex_DestroyRWLock             },
        // Semaphore
        { "NewSemaphore",              l_Mutex_NewSemaphore              },
        { "DestroySemaphore",          l_Mutex_DestroySemaphore          },
        { "WaitSemaphore",             l_Mutex_WaitSemaphore             },
        { "TryWaitSemaphore",          l_Mutex_TryWaitSemaphore          },
        { "WaitSemaphoreTimeout",      l_Mutex_WaitSemaphoreTimeout      },
        { "SignalSemaphore",           l_Mutex_SignalSemaphore           },
        { "GetSemaphoreValue",         l_Mutex_GetSemaphoreValue         },
        // Condition
        { "NewCondition",              l_Mutex_NewCondition              },
        { "DestroyCondition",          l_Mutex_DestroyCondition          },
        { "SignalCondition",           l_Mutex_SignalCondition           },
        { "BroadcastCondition",        l_Mutex_BroadcastCondition        },
        { "WaitCondition",             l_Mutex_WaitCondition             },
        { "WaitConditionTimeout",      l_Mutex_WaitConditionTimeout      },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
