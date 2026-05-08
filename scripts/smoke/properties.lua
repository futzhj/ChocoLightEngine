-- Phase Y smoke: Light.Properties (SDL_properties.h)
--
-- ASCII-only. No SDL_Init dependency. Properties are SDL's typed bag
-- backed by a thread-safe hashtable.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Properties")
if not ok then fail("require(Light.Properties) failed: " .. tostring(mod)) end

-- 1) 19 fns
local fn_names = {
    "GetGlobalProperties", "CreateProperties", "DestroyProperties", "CopyProperties",
    "LockProperties", "UnlockProperties",
    "SetPointerProperty", "SetStringProperty", "SetNumberProperty",
    "SetFloatProperty",   "SetBooleanProperty",
    "GetPointerProperty", "GetStringProperty", "GetNumberProperty",
    "GetFloatProperty",   "GetBooleanProperty",
    "HasProperty", "GetPropertyType", "ClearProperty",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Properties." .. k .. " missing") end
end
pass("Light.Properties module ok (" .. #fn_names .. " functions)")

-- 2) Constants - SDL_PropertyType is sequential 0..5
assert(mod.PROPERTY_TYPE_INVALID == 0, "INVALID must be 0")
assert(mod.PROPERTY_TYPE_POINTER == 1, "POINTER must be 1")
assert(mod.PROPERTY_TYPE_STRING  == 2, "STRING must be 2")
assert(mod.PROPERTY_TYPE_NUMBER  == 3, "NUMBER must be 3")
assert(mod.PROPERTY_TYPE_FLOAT   == 4, "FLOAT must be 4")
assert(mod.PROPERTY_TYPE_BOOLEAN == 5, "BOOLEAN must be 5")
pass("Light.Properties constants ok (6)")

-- 3) GetGlobalProperties - single global instance, must be non-zero
local g = mod.GetGlobalProperties()
assert(type(g) == "number" and g ~= 0, "GetGlobalProperties must be non-zero, got " .. tostring(g))
local g2 = mod.GetGlobalProperties()
assert(g == g2, "GetGlobalProperties must be stable across calls")
pass(string.format("GetGlobalProperties ok (id=%d, stable)", g))

-- 4) CreateProperties - new id, distinct from global
local p = mod.CreateProperties()
assert(type(p) == "number" and p ~= 0, "CreateProperties must return non-zero id")
assert(p ~= g, "CreateProperties must differ from global")
pass(string.format("CreateProperties ok (id=%d)", p))

-- 5) Setters + Getters round-trip - all 4 scalar types
assert(mod.SetStringProperty(p, "title", "phase y test"))
assert(mod.GetStringProperty(p, "title") == "phase y test", "string round-trip failed")
pass("String set/get round-trip ok")

assert(mod.SetNumberProperty(p, "count", 42))
assert(mod.GetNumberProperty(p, "count") == 42, "number round-trip failed")
-- Negative
assert(mod.SetNumberProperty(p, "neg", -1234567))
assert(mod.GetNumberProperty(p, "neg") == -1234567, "negative number round-trip failed")
pass("Number set/get round-trip ok (positive + negative)")

assert(mod.SetFloatProperty(p, "ratio", 1.5))
local r = mod.GetFloatProperty(p, "ratio")
assert(math.abs(r - 1.5) < 1e-6, "float round-trip drift: " .. tostring(r))
pass("Float set/get round-trip ok")

assert(mod.SetBooleanProperty(p, "enabled", true))
assert(mod.GetBooleanProperty(p, "enabled") == true, "bool true failed")
assert(mod.SetBooleanProperty(p, "muted", false))
assert(mod.GetBooleanProperty(p, "muted") == false, "bool false failed")
pass("Boolean set/get round-trip ok")

-- 6) Pointer property - lightuserdata round-trip via Light.Loadso handle
local Loadso = require("Light.Loadso")
local Filesystem = require("Light.Filesystem")
local lib_handle
for _, name in ipairs({
    Filesystem.GetBasePath() .. "Light.dll",
    Filesystem.GetBasePath() .. "libLight.so",
    Filesystem.GetBasePath() .. "libLight.dylib",
    "Light",
}) do
    local h = Loadso.LoadObject(name)
    if h then lib_handle = h; break end
end
assert(lib_handle, "need a real lightuserdata to test pointer property")

assert(mod.SetPointerProperty(p, "lib", lib_handle))
local got_lib = mod.GetPointerProperty(p, "lib")
assert(got_lib == lib_handle, "pointer round-trip mismatch")
pass("Pointer set/get round-trip ok (real lightuserdata)")
Loadso.UnloadObject(lib_handle)

-- 7) HasProperty + GetPropertyType
assert(mod.HasProperty(p, "title"), "title must exist")
assert(not mod.HasProperty(p, "nonexistent"), "nonexistent must not exist")
assert(mod.GetPropertyType(p, "title")   == mod.PROPERTY_TYPE_STRING,  "type(title) must be STRING")
assert(mod.GetPropertyType(p, "count")   == mod.PROPERTY_TYPE_NUMBER,  "type(count) must be NUMBER")
assert(mod.GetPropertyType(p, "ratio")   == mod.PROPERTY_TYPE_FLOAT,   "type(ratio) must be FLOAT")
assert(mod.GetPropertyType(p, "enabled") == mod.PROPERTY_TYPE_BOOLEAN, "type(enabled) must be BOOLEAN")
assert(mod.GetPropertyType(p, "lib")     == mod.PROPERTY_TYPE_POINTER, "type(lib) must be POINTER")
assert(mod.GetPropertyType(p, "missing") == mod.PROPERTY_TYPE_INVALID, "type(missing) must be INVALID")
pass("HasProperty + GetPropertyType ok")

-- 8) Default value on missing key - all 5 typed getters
assert(mod.GetStringProperty(p, "missing", "fallback") == "fallback", "default string")
assert(mod.GetNumberProperty(p, "missing", 99) == 99, "default number")
assert(math.abs(mod.GetFloatProperty(p, "missing", 3.14) - 3.14) < 1e-6, "default float")
assert(mod.GetBooleanProperty(p, "missing", true) == true, "default bool true")
assert(mod.GetBooleanProperty(p, "missing", false) == false, "default bool false")
local ptr_default = mod.GetPointerProperty(p, "missing", nil)
assert(ptr_default == nil, "default pointer (nil) must return nil")
pass("Default value fallback ok (5 typed getters)")

-- 9) ClearProperty
assert(mod.ClearProperty(p, "title"))
assert(not mod.HasProperty(p, "title"), "ClearProperty must remove the key")
assert(mod.GetPropertyType(p, "title") == mod.PROPERTY_TYPE_INVALID,
       "type after Clear must be INVALID")
pass("ClearProperty ok")

-- 10) SetStringProperty(nil) deletes the key (SDL semantics)
mod.SetStringProperty(p, "to_be_deleted", "transient")
assert(mod.HasProperty(p, "to_be_deleted"), "precondition")
mod.SetStringProperty(p, "to_be_deleted", nil)
assert(not mod.HasProperty(p, "to_be_deleted"), "Set(nil) must delete")
pass("SetStringProperty(nil) deletes key ok")

-- 11) Lock / Unlock - advisory; just verify they don't error
assert(mod.LockProperties(p))
mod.UnlockProperties(p)
pass("Lock/Unlock ok")

-- 12) CopyProperties
local src = mod.CreateProperties()
local dst = mod.CreateProperties()
mod.SetNumberProperty(src, "x", 100)
mod.SetStringProperty(src, "name", "copy_test")
assert(mod.CopyProperties(src, dst))
assert(mod.GetNumberProperty(dst, "x") == 100, "copy: number missing")
assert(mod.GetStringProperty(dst, "name") == "copy_test", "copy: string missing")
mod.DestroyProperties(src)
mod.DestroyProperties(dst)
pass("CopyProperties ok")

-- 13) Wrong-type get returns default (SDL silent fallback)
mod.SetStringProperty(p, "stringy", "abc")
assert(mod.GetNumberProperty(p, "stringy", 7) == 7,
       "wrong-type number get must return default")
assert(mod.GetBooleanProperty(p, "stringy", false) == false,
       "wrong-type bool get must return default")
pass("Wrong-type get falls back to default ok")

-- 14) DestroyProperties
mod.DestroyProperties(p)
mod.DestroyProperties(0)    -- no-op
mod.DestroyProperties(nil)  -- no-op
pass("DestroyProperties + nil/0 boundary ok")

-- 15) Boundary: pointer property with non-lightuserdata raises
local rok = pcall(mod.SetPointerProperty, mod.GetGlobalProperties(), "x", "not lud")
if rok then fail("SetPointerProperty(string) should raise") end
pass("SetPointerProperty(non-lud) raises ok")

print("properties smoke ok")
