-- Phase AC smoke: Light.Atomic (SDL_atomic.h)
--
-- ASCII-only. Pure single-thread invariants: set/get/cas/add semantics.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Atomic")
if not ok then fail("require(Light.Atomic) failed: " .. tostring(mod)) end

-- 1) 15 fns
local fn_names = {
    "NewSpinlock", "TryLockSpinlock", "LockSpinlock", "UnlockSpinlock",
    "MemoryBarrierAcquire", "MemoryBarrierRelease",
    "NewAtomicInt", "CompareAndSwapAtomicInt", "SetAtomicInt", "GetAtomicInt", "AddAtomicInt",
    "NewAtomicU32", "CompareAndSwapAtomicU32", "SetAtomicU32", "GetAtomicU32",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Atomic." .. k .. " missing") end
end
pass("Light.Atomic module ok (" .. #fn_names .. " functions)")

-- ============================================================
-- Spinlock
-- ============================================================

local sl = mod.NewSpinlock()
assert(type(sl) == "userdata", "NewSpinlock must return userdata")
pass("NewSpinlock ok")

-- 2) TryLock on fresh spinlock must succeed
assert(mod.TryLockSpinlock(sl) == true, "fresh spinlock must accept TryLock")
-- 3) Second TryLock must fail (already held)
assert(mod.TryLockSpinlock(sl) == false, "held spinlock must reject TryLock")
mod.UnlockSpinlock(sl)
-- 4) After unlock, TryLock again succeeds
assert(mod.TryLockSpinlock(sl) == true, "unlocked spinlock must accept TryLock")
mod.UnlockSpinlock(sl)
pass("Spinlock acquire/release/contention ok")

-- 5) LockSpinlock + UnlockSpinlock blocking variant works
mod.LockSpinlock(sl)
mod.UnlockSpinlock(sl)
pass("Lock/UnlockSpinlock ok")

-- ============================================================
-- Memory barriers - just verify they don't crash
-- ============================================================
mod.MemoryBarrierAcquire()
mod.MemoryBarrierRelease()
pass("MemoryBarriers ok (no-crash)")

-- ============================================================
-- AtomicInt
-- ============================================================

-- 6) NewAtomicInt with default (0)
local a0 = mod.NewAtomicInt()
assert(type(a0) == "userdata", "NewAtomicInt must return userdata")
assert(mod.GetAtomicInt(a0) == 0, "default initial must be 0")
pass("NewAtomicInt() default ok")

-- 7) NewAtomicInt with explicit initial
local a = mod.NewAtomicInt(42)
assert(mod.GetAtomicInt(a) == 42, "explicit initial must persist")
pass("NewAtomicInt(42) ok")

-- 8) SetAtomicInt returns the PREVIOUS value
local prev = mod.SetAtomicInt(a, 100)
assert(prev == 42, "SetAtomicInt must return previous value, got " .. tostring(prev))
assert(mod.GetAtomicInt(a) == 100, "value must be updated after Set")
pass("SetAtomicInt returns previous ok")

-- 9) AddAtomicInt returns the PREVIOUS value (SDL semantics)
local prev2 = mod.AddAtomicInt(a, 7)
assert(prev2 == 100, "AddAtomicInt must return previous value")
assert(mod.GetAtomicInt(a) == 107, "value must be incremented after Add")
pass("AddAtomicInt returns previous ok")

-- 10) AddAtomicInt with negative
mod.AddAtomicInt(a, -50)
assert(mod.GetAtomicInt(a) == 57, "negative add ok")
pass("AddAtomicInt(negative) ok")

-- 11) CompareAndSwapAtomicInt: success path
mod.SetAtomicInt(a, 10)
assert(mod.CompareAndSwapAtomicInt(a, 10, 20) == true, "CAS with matching expected must succeed")
assert(mod.GetAtomicInt(a) == 20, "value must be swapped")
pass("CompareAndSwapAtomicInt success path ok")

-- 12) CompareAndSwapAtomicInt: failure path
assert(mod.CompareAndSwapAtomicInt(a, 999, 0) == false, "CAS with mismatched expected must fail")
assert(mod.GetAtomicInt(a) == 20, "value must be unchanged on CAS fail")
pass("CompareAndSwapAtomicInt failure path ok")

-- 13) Type safety: passing wrong-type userdata raises
local sl2 = mod.NewSpinlock()
local rok = pcall(mod.GetAtomicInt, sl2)
if rok then fail("GetAtomicInt(spinlock) should raise") end
pass("Type safety: GetAtomicInt(spinlock) raises ok")

-- 14) Wrong arg type for spinlock fn
local rok2 = pcall(mod.TryLockSpinlock, a)
if rok2 then fail("TryLockSpinlock(atomic_int) should raise") end
pass("Type safety: TryLockSpinlock(atomic_int) raises ok")

-- ============================================================
-- AtomicU32
-- ============================================================

-- 15) NewAtomicU32 default
local u0 = mod.NewAtomicU32()
assert(type(u0) == "userdata", "NewAtomicU32 must return userdata")
assert(mod.GetAtomicU32(u0) == 0, "default initial must be 0")
pass("NewAtomicU32() default ok")

-- 16) NewAtomicU32 with large value (>= 2^31, requires unsigned)
local big = 0x80000001  -- 2^31 + 1
local u = mod.NewAtomicU32(big)
local got = mod.GetAtomicU32(u)
assert(got == big, "large unsigned must round-trip; expected " .. big .. " got " .. tostring(got))
pass("NewAtomicU32 large value (>= 2^31) ok")

-- 17) SetAtomicU32 returns previous
local prev3 = mod.SetAtomicU32(u, 0)
assert(prev3 == big, "SetAtomicU32 previous mismatch")
assert(mod.GetAtomicU32(u) == 0, "value must reset")
pass("SetAtomicU32 returns previous ok")

-- 18) CAS U32
mod.SetAtomicU32(u, 5)
assert(mod.CompareAndSwapAtomicU32(u, 5, 8) == true)
assert(mod.GetAtomicU32(u) == 8)
assert(mod.CompareAndSwapAtomicU32(u, 5, 0) == false, "stale CAS must fail")
assert(mod.GetAtomicU32(u) == 8, "value unchanged on CAS fail")
pass("CompareAndSwapAtomicU32 ok (success + fail paths)")

-- 19) Cross-type metatable rejection
local rok3 = pcall(mod.GetAtomicU32, a)  -- pass AtomicInt to U32 getter
if rok3 then fail("GetAtomicU32(atomic_int) should raise") end
pass("Type safety: GetAtomicU32(atomic_int) raises ok")

print("atomic smoke ok")
