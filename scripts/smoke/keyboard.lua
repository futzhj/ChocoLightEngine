-- Phase AG smoke: Light.Keyboard (SDL_keyboard.h, polling subset)
-- ASCII-only.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Keyboard")
if not ok then fail("require(Light.Keyboard) failed: " .. tostring(mod)) end

-- 1) 15 fns
local fn_names = {
    "HasKeyboard","GetKeyboards","GetKeyboardNameForID","HasScreenKeyboardSupport",
    "GetKeyboardState","ResetKeyboard","GetModState","SetModState",
    "GetKeyFromScancode","GetScancodeFromKey",
    "SetScancodeName","GetScancodeName","GetScancodeFromName",
    "GetKeyName","GetKeyFromName",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Keyboard." .. k .. " missing") end
end
pass("Light.Keyboard module ok (" .. #fn_names .. " fns)")

-- 2) Constants - sample a few
assert(type(mod.SCANCODE_A) == "number" and mod.SCANCODE_A > 0)
assert(type(mod.SCANCODE_RETURN) == "number")
assert(type(mod.SCANCODE_SPACE) == "number")
assert(type(mod.SCANCODE_F1) == "number")
assert(type(mod.SCANCODE_KP_ENTER) == "number")
assert(mod.SCANCODE_UNKNOWN == 0, "SCANCODE_UNKNOWN should be 0")
assert(mod.SCANCODE_COUNT > 200, "SCANCODE_COUNT should be > 200, got " ..
       tostring(mod.SCANCODE_COUNT))
assert(mod.KMOD_NONE == 0)
assert(type(mod.KMOD_SHIFT) == "number" and mod.KMOD_SHIFT > 0)
pass("Constants ok (SCANCODE_COUNT=" .. mod.SCANCODE_COUNT .. ")")

-- 3) Device queries don't crash
local has_kb = mod.HasKeyboard()
assert(type(has_kb) == "boolean")
local kb_list = mod.GetKeyboards()
assert(type(kb_list) == "table")
pass("HasKeyboard=" .. tostring(has_kb) .. ", GetKeyboards count=" ..
     tostring(#kb_list))

-- For each detected keyboard, name lookup must not crash.
for i, id in ipairs(kb_list) do
    local n = mod.GetKeyboardNameForID(id)
    -- name may be nil or string; just ensure no crash
    assert(n == nil or type(n) == "string")
    if i == 1 then
        pass("First keyboard name: " .. tostring(n))
    end
end

-- 4) HasScreenKeyboardSupport
local has_sk = mod.HasScreenKeyboardSupport()
assert(type(has_sk) == "boolean")
pass("HasScreenKeyboardSupport=" .. tostring(has_sk))

-- 5) GetKeyboardState
local state = mod.GetKeyboardState()
assert(type(state) == "table")
-- Length should be SCANCODE_COUNT (or 0 on uninited subsystem)
if #state > 0 then
    assert(#state >= mod.SCANCODE_COUNT - 5,
           "state length " .. #state .. " too small vs SCANCODE_COUNT " ..
           mod.SCANCODE_COUNT)
    -- SDL stores state[scancode] in a 0-indexed C array; we expose it as
    -- 1-indexed Lua. So scancode N is at table[N+1]; state[1] is scancode 0
    -- (SCANCODE_UNKNOWN), which should never be pressed.
    assert(state[1] == false, "scancode 0 should never be pressed")
end
pass("GetKeyboardState returned table of length " .. #state)

-- 6) ResetKeyboard then state should all be false
mod.ResetKeyboard()
local state2 = mod.GetKeyboardState()
if #state2 > 0 then
    for i = 1, #state2 do
        if state2[i] ~= false then
            fail("after Reset, scancode " .. (i - 1) ..
                 " is still pressed?")
        end
    end
end
pass("ResetKeyboard ok (all keys false)")

-- 7) Mod state round-trip
mod.SetModState(mod.KMOD_SHIFT)
local m = mod.GetModState()
-- SDL may filter or normalize bits; just ensure SHIFT is set
local has_shift = (m % (mod.KMOD_SHIFT * 2) >= mod.KMOD_SHIFT)
assert(has_shift, "after SetModState(SHIFT), GetModState=" .. tostring(m))
mod.SetModState(mod.KMOD_NONE)
pass("Mod state round-trip ok (state=" .. tostring(m) .. ")")

-- 8) Scancode <-> Keycode round trip
-- SCANCODE_A with no modifiers should map to keycode 'a' (0x61)
local kc_a = mod.GetKeyFromScancode(mod.SCANCODE_A, mod.KMOD_NONE, false)
assert(kc_a == 0x61 or kc_a == string.byte("a"),
       "SCANCODE_A -> keycode: got " .. tostring(kc_a))

local sc_back, mod_back = mod.GetScancodeFromKey(kc_a)
assert(sc_back == mod.SCANCODE_A,
       "keycode 'a' -> scancode mismatch: " .. tostring(sc_back))
assert(type(mod_back) == "number")
pass("Scancode <-> Keycode round trip ok (SCANCODE_A <-> 'a')")

-- 9) Names
local n_a = mod.GetScancodeName(mod.SCANCODE_A)
assert(n_a == "A", "GetScancodeName(A) = " .. tostring(n_a))
local sc_a_back = mod.GetScancodeFromName("A")
assert(sc_a_back == mod.SCANCODE_A, "GetScancodeFromName(A) = " ..
       tostring(sc_a_back))

local n_kc = mod.GetKeyName(kc_a)
assert(n_kc == "A", "GetKeyName('a' keycode) = " .. tostring(n_kc))
local kc_back = mod.GetKeyFromName("A")
assert(kc_back == kc_a, "GetKeyFromName('A') = " .. tostring(kc_back))
pass("Scancode/Keycode name <-> code round-trip ok")

-- 10) SetScancodeName: rename and restore
local original = mod.GetScancodeName(mod.SCANCODE_F12)
assert(mod.SetScancodeName(mod.SCANCODE_F12, "TestKey") == true)
local renamed = mod.GetScancodeName(mod.SCANCODE_F12)
assert(renamed == "TestKey",
       "SetScancodeName did not stick: got " .. tostring(renamed))
-- Restore
mod.SetScancodeName(mod.SCANCODE_F12, original)
assert(mod.GetScancodeName(mod.SCANCODE_F12) == original,
       "Failed to restore F12 name")
pass("SetScancodeName round-trip ok (was '" .. original .. "')")

-- 11) Unknown name -> SCANCODE_UNKNOWN
local sc_unknown = mod.GetScancodeFromName("zz_definitely_not_a_key")
assert(sc_unknown == mod.SCANCODE_UNKNOWN,
       "unknown name should map to SCANCODE_UNKNOWN, got " ..
       tostring(sc_unknown))
pass("Unknown scancode name -> UNKNOWN ok")

print("keyboard smoke ok")
