-- Phase W smoke: Light.Loadso (SDL_loadso.h)
--
-- ASCII-only. No hard platform-specific fixture requirement: we rely on
-- Light.dll / libLight.so / libLight.dylib being next to the light
-- executable, which is the deployment invariant of this engine.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Loadso")
if not ok then fail("require(Light.Loadso) failed: " .. tostring(mod)) end

-- 1) 3 fns
for _, k in ipairs({"LoadObject", "LoadFunction", "UnloadObject"}) do
    if type(mod[k]) ~= "function" then fail("Light.Loadso." .. k .. " missing") end
end
pass("Light.Loadso module ok (3 functions)")

-- 2) LoadObject on a guaranteed-missing path returns nil + err
local h_bad, err_bad = mod.LoadObject("zz_definitely_does_not_exist_xyz_" .. tostring(os.time()))
assert(h_bad == nil, "missing .so must return nil")
assert(type(err_bad) == "string" and #err_bad > 0,
       "missing .so must return non-empty err string")
pass("LoadObject(missing) returns nil,err ok: " .. err_bad)

-- 3) Find the Light shared library alongside the executable to test success path.
--    SDL_LoadObject accepts either bare name ("Light") or full path ("C:/.../Light.dll").
--    We try bare-name first; if that fails, probe the common locations.
local Filesystem = require("Light.Filesystem")
local base = Filesystem.GetBasePath()
assert(base, "need GetBasePath to locate runtime dir")

-- Platform-agnostic candidate list. At least ONE must load on a CI runner
-- where the test binary is running (we are proof that the dlopen works).
local candidates = {
    base .. "Light.dll",        -- Windows, fully qualified
    base .. "libLight.so",      -- Linux
    base .. "libLight.dylib",   -- macOS
    "Light",                    -- SDL will append platform extension
}

local handle, load_err
for _, name in ipairs(candidates) do
    local h, e = mod.LoadObject(name)
    if h then handle = h; load_err = nil; pass("LoadObject ok: " .. name); break
    else load_err = (load_err or "") .. "\n  " .. name .. " -> " .. tostring(e) end
end
assert(handle, "no Light shared library could be loaded. Tried:" .. tostring(load_err))

-- 4) LoadFunction on a KNOWN-exported symbol: luaopen_Light_Loadso itself.
--    Every Light.* module exposes its luaopen_ entry as a DLL export.
local fn, fn_err = mod.LoadFunction(handle, "luaopen_Light_Loadso")
assert(fn, "LoadFunction(luaopen_Light_Loadso) failed: " .. tostring(fn_err))
assert(type(fn) == "userdata", "fn must be lightuserdata, got " .. type(fn))
pass("LoadFunction(luaopen_Light_Loadso) ok")

-- 5) LoadFunction on a missing symbol returns nil + err
local nfn, nerr = mod.LoadFunction(handle, "this_symbol_does_not_exist_zzz")
assert(nfn == nil and nerr ~= nil, "missing symbol must return nil,err")
pass("LoadFunction(missing) ok: " .. nerr)

-- 6) LoadFunction on non-handle raises
local rok = pcall(mod.LoadFunction, "not a handle", "x")
if rok then fail("LoadFunction(string) should raise") end
pass("LoadFunction(non-handle) raises ok")

-- 7) UnloadObject
mod.UnloadObject(handle)
pass("UnloadObject ok")

-- 8) UnloadObject(nil) is no-op
mod.UnloadObject(nil)
pass("UnloadObject(nil) no-op ok")

print("loadso smoke ok")
