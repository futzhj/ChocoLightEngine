-- Phase AD smoke: Light.Mutex (SDL_mutex.h)
--
-- ASCII-only. Lua is single-threaded, so we only verify single-threaded
-- safe sequences (no contention scenarios, no recursive locking, no
-- WaitSemaphore / WaitCondition that would block forever).

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Mutex")
if not ok then fail("require(Light.Mutex) failed: " .. tostring(mod)) end

-- 1) 25 fns
local fn_names = {
    "NewMutex","LockMutex","TryLockMutex","UnlockMutex","DestroyMutex",
    "NewRWLock","LockRWLockForReading","LockRWLockForWriting",
    "TryLockRWLockForReading","TryLockRWLockForWriting","UnlockRWLock","DestroyRWLock",
    "NewSemaphore","DestroySemaphore","WaitSemaphore","TryWaitSemaphore",
    "WaitSemaphoreTimeout","SignalSemaphore","GetSemaphoreValue",
    "NewCondition","DestroyCondition","SignalCondition","BroadcastCondition",
    "WaitCondition","WaitConditionTimeout",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Mutex." .. k .. " missing") end
end
pass("Light.Mutex module ok (" .. #fn_names .. " functions)")

-- ============================================================
-- Mutex
-- ============================================================

local m = mod.NewMutex()
assert(type(m) == "userdata", "NewMutex must return userdata")
pass("NewMutex ok")

-- 2) Lock + Unlock single round-trip
mod.LockMutex(m)
mod.UnlockMutex(m)
pass("LockMutex/UnlockMutex round-trip ok")

-- 3) TryLockMutex on unlocked: must succeed
assert(mod.TryLockMutex(m) == true, "TryLockMutex on free mutex must succeed")
mod.UnlockMutex(m)
pass("TryLockMutex(free) -> true ok")

-- 4) Destroy ok, second Destroy is a no-op
mod.DestroyMutex(m)
mod.DestroyMutex(m)  -- safe double-destroy
pass("DestroyMutex idempotent ok")

-- 5) Use-after-destroy raises
local rok = pcall(mod.LockMutex, m)
if rok then fail("LockMutex on destroyed mutex should raise") end
pass("Use-after-destroy raises ok")

-- ============================================================
-- RWLock
-- ============================================================

local rw = mod.NewRWLock()
assert(type(rw) == "userdata", "NewRWLock must return userdata")
pass("NewRWLock ok")

-- 6) Reading lock + unlock
mod.LockRWLockForReading(rw)
mod.UnlockRWLock(rw)
pass("LockRWLockForReading + Unlock ok")

-- 7) Writing lock + unlock
mod.LockRWLockForWriting(rw)
mod.UnlockRWLock(rw)
pass("LockRWLockForWriting + Unlock ok")

-- 8) Try-read on free rwlock
assert(mod.TryLockRWLockForReading(rw) == true, "TryReading on free rwlock must succeed")
mod.UnlockRWLock(rw)
pass("TryLockRWLockForReading(free) ok")

-- 9) Try-write on free rwlock
assert(mod.TryLockRWLockForWriting(rw) == true, "TryWriting on free rwlock must succeed")
mod.UnlockRWLock(rw)
pass("TryLockRWLockForWriting(free) ok")

-- 10) Destroy + reuse raises
mod.DestroyRWLock(rw)
local rok2 = pcall(mod.LockRWLockForReading, rw)
if rok2 then fail("LockRWLockForReading on destroyed rwlock should raise") end
pass("RWLock use-after-destroy raises ok")

-- ============================================================
-- Semaphore
-- ============================================================

-- 11) NewSemaphore(2): value starts at 2
local sem = mod.NewSemaphore(2)
assert(type(sem) == "userdata", "NewSemaphore must return userdata")
assert(mod.GetSemaphoreValue(sem) == 2, "initial value must be 2")
pass("NewSemaphore(2) ok")

-- 12) TryWaitSemaphore decrements value
assert(mod.TryWaitSemaphore(sem) == true, "TryWait on val=2 must succeed")
assert(mod.GetSemaphoreValue(sem) == 1, "value must be 1 after TryWait")
assert(mod.TryWaitSemaphore(sem) == true, "TryWait on val=1 must succeed")
assert(mod.GetSemaphoreValue(sem) == 0, "value must be 0 after second TryWait")
pass("TryWaitSemaphore decrement ok")

-- 13) TryWaitSemaphore on empty must fail
assert(mod.TryWaitSemaphore(sem) == false, "TryWait on val=0 must fail")
assert(mod.GetSemaphoreValue(sem) == 0, "value must remain 0 on failed TryWait")
pass("TryWaitSemaphore(empty) -> false ok")

-- 14) Signal increments
mod.SignalSemaphore(sem)
assert(mod.GetSemaphoreValue(sem) == 1, "Signal must increment")
pass("SignalSemaphore ok")

-- 15) WaitSemaphoreTimeout(0) - immediate, behaves like TryWait
assert(mod.WaitSemaphoreTimeout(sem, 0) == true, "TimeoutWait(0) on val=1 must succeed")
assert(mod.GetSemaphoreValue(sem) == 0)
assert(mod.WaitSemaphoreTimeout(sem, 0) == false, "TimeoutWait(0) on val=0 must fail")
pass("WaitSemaphoreTimeout ok (immediate variant)")

-- 16) Destroy + use-after raises
mod.DestroySemaphore(sem)
local rok3 = pcall(mod.GetSemaphoreValue, sem)
if rok3 then fail("GetSemaphoreValue on destroyed sem should raise") end
pass("Semaphore use-after-destroy raises ok")

-- ============================================================
-- Condition (single-thread: only test non-blocking ops)
-- ============================================================

local cond = mod.NewCondition()
assert(type(cond) == "userdata", "NewCondition must return userdata")
pass("NewCondition ok")

-- 17) Signal/Broadcast on a condition with no waiter is allowed and a no-op
mod.SignalCondition(cond)
mod.BroadcastCondition(cond)
pass("Signal/BroadcastCondition (no waiter) ok")

-- 18) WaitConditionTimeout(0, mutex) returns false (immediate timeout) without
-- ever blocking. We need a held mutex.
local m2 = mod.NewMutex()
mod.LockMutex(m2)
local r = mod.WaitConditionTimeout(cond, m2, 0)
assert(type(r) == "boolean", "WaitConditionTimeout must return boolean")
-- false means timed out (expected); true means signaled (rare but allowed)
pass("WaitConditionTimeout(0) returned boolean ok (value=" .. tostring(r) .. ")")
mod.UnlockMutex(m2)
mod.DestroyMutex(m2)

-- 19) Destroy condition
mod.DestroyCondition(cond)
local rok4 = pcall(mod.SignalCondition, cond)
if rok4 then fail("SignalCondition on destroyed cond should raise") end
pass("Condition use-after-destroy raises ok")

-- ============================================================
-- Type safety
-- ============================================================

-- 20) Pass mutex where rwlock expected
local mx = mod.NewMutex()
local rok5 = pcall(mod.LockRWLockForReading, mx)
if rok5 then fail("LockRWLockForReading(mutex) should raise") end
pass("Type safety: LockRWLockForReading(mutex) raises ok")
mod.DestroyMutex(mx)

print("mutex smoke ok")
