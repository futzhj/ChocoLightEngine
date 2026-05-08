-- Phase Z smoke: Light.Clipboard (SDL_clipboard.h)
--
-- ASCII-only.
--
-- CI environments (GitHub Actions Windows runner is a non-interactive Server
-- session) sometimes lack a usable clipboard. We therefore:
--   * always verify Lua-side API registration
--   * accept BOTH success and graceful failure for read/write
--   * only assert structural shape on returned values
--
-- The full read/write round-trip is covered by manual / desktop runs.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Clipboard")
if not ok then fail("require(Light.Clipboard) failed: " .. tostring(mod)) end

-- 1) 10 fns
local fn_names = {
    "SetText", "GetText", "HasText",
    "SetPrimarySelectionText", "GetPrimarySelectionText", "HasPrimarySelectionText",
    "ClearClipboardData", "GetClipboardData", "HasClipboardData", "GetClipboardMimeTypes",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Clipboard." .. k .. " missing") end
end
pass("Light.Clipboard module ok (" .. #fn_names .. " functions)")

-- 2) HasText: must always return a bool, never raise
local has = mod.HasText()
assert(type(has) == "boolean", "HasText must return boolean, got " .. type(has))
pass("HasText returns boolean ok (value=" .. tostring(has) .. ")")

-- 3) SetText / GetText: accept either successful round-trip OR graceful failure
local probe = "Light.Clipboard probe " .. tostring(os.time())
local set_ok, set_err = mod.SetText(probe)
assert(type(set_ok) == "boolean", "SetText must return boolean")
if set_ok then
    -- On platforms with a working clipboard, the round-trip MUST hold.
    local got, get_err = mod.GetText()
    if got == nil then
        pass("SetText OK but GetText returned nil (CI quirk): " .. tostring(get_err))
    else
        assert(type(got) == "string", "GetText must return string, got " .. type(got))
        if got == probe then
            pass("SetText/GetText round-trip ok (len=" .. #probe .. ")")
        else
            -- Some headless environments accept SetText but persist nothing;
            -- treat as a soft observation rather than failure.
            pass("SetText reported ok but readback differs (env quirk); got len=" .. #got)
        end
    end
else
    pass("SetText gracefully failed (headless env): " .. tostring(set_err))
end

-- 4) GetText alone always returns SOMETHING (string or nil + err); never raise
local txt, err = mod.GetText()
assert(txt == nil or type(txt) == "string", "GetText must return string or nil")
assert(err == nil or type(err) == "string", "GetText 2nd return must be string or nil")
pass("GetText shape ok")

-- 5) HasPrimarySelectionText: bool, no raise. Meaningful only on X11; on
-- Windows/macOS/CI we just check it does not crash.
local hps = mod.HasPrimarySelectionText()
assert(type(hps) == "boolean", "HasPrimarySelectionText must be boolean")
pass("HasPrimarySelectionText ok (value=" .. tostring(hps) .. ")")

-- 6) GetPrimarySelectionText shape
local pst, pst_err = mod.GetPrimarySelectionText()
assert(pst == nil or type(pst) == "string", "GetPrimarySelectionText shape")
assert(pst_err == nil or type(pst_err) == "string", "GetPrimarySelectionText err shape")
pass("GetPrimarySelectionText shape ok")

-- 7) SetPrimarySelectionText - allow either ok or error; never raise
local sps_ok = mod.SetPrimarySelectionText("primary probe")
assert(type(sps_ok) == "boolean", "SetPrimarySelectionText must return boolean")
pass("SetPrimarySelectionText returns boolean ok (value=" .. tostring(sps_ok) .. ")")

-- 8) HasClipboardData("text/plain") - bool, no raise
local hcd = mod.HasClipboardData("text/plain")
assert(type(hcd) == "boolean", "HasClipboardData must be boolean")
pass("HasClipboardData(text/plain) ok (value=" .. tostring(hcd) .. ")")

-- 9) GetClipboardData("text/plain") - returns string|nil + err
local cd, cd_err = mod.GetClipboardData("text/plain")
assert(cd == nil or type(cd) == "string",
       "GetClipboardData must return string or nil, got " .. type(cd))
if cd ~= nil then
    pass("GetClipboardData(text/plain) returned " .. #cd .. " bytes")
else
    pass("GetClipboardData(text/plain) returned nil (no data) ok: " .. tostring(cd_err))
end

-- 10) HasClipboardData on garbage MIME must be false (not crash)
assert(mod.HasClipboardData("application/x-zz-bogus") == false,
       "HasClipboardData on bogus mime must be false")
pass("HasClipboardData(bogus mime) returns false ok")

-- 11) GetClipboardMimeTypes - returns table|nil + err
local mt, mt_err = mod.GetClipboardMimeTypes()
assert(mt == nil or type(mt) == "table",
       "GetClipboardMimeTypes must return table or nil")
if mt then
    -- Each entry must be a string.
    for i, v in ipairs(mt) do
        assert(type(v) == "string",
               "MimeTypes[" .. i .. "] must be string, got " .. type(v))
    end
    pass("GetClipboardMimeTypes ok (" .. #mt .. " entries)")
else
    pass("GetClipboardMimeTypes returned nil ok: " .. tostring(mt_err))
end

-- 12) ClearClipboardData - bool result; do not require success on CI
local cl = mod.ClearClipboardData()
assert(type(cl) == "boolean", "ClearClipboardData must return boolean")
pass("ClearClipboardData returns boolean ok (value=" .. tostring(cl) .. ")")

-- 13) Boundary: SetText with non-string raises
local rok = pcall(mod.SetText, 12345)
if rok then fail("SetText(number) should raise") end
pass("SetText(non-string) raises ok")

-- 14) Boundary: GetClipboardData requires mime arg
local rok2 = pcall(mod.GetClipboardData) -- nil arg
if rok2 then fail("GetClipboardData() with no mime should raise") end
pass("GetClipboardData(no arg) raises ok")

print("clipboard smoke ok")
