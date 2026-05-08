-- Phase T smoke: Light.Filesystem (SDL_filesystem.h)
--
-- ASCII-only per Windows CI convention.
-- Uses GetPrefPath as a guaranteed-writable working area to avoid
-- depending on the current working directory layout.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Filesystem")
if not ok then fail("require(Light.Filesystem) failed: " .. tostring(mod)) end

-- 1) 10 fns
for _, k in ipairs({
    "GetBasePath", "GetPrefPath", "GetUserFolder", "GetCurrentDirectory",
    "CreateDirectory", "RemovePath", "RenamePath", "CopyFile",
    "GetPathInfo", "GlobDirectory",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Filesystem." .. k .. " missing") end
end
pass("Light.Filesystem module ok (10 functions)")

-- 2) Folder constants (sequential 0..10)
assert(mod.FOLDER_HOME == 0,        "FOLDER_HOME must be 0")
assert(mod.FOLDER_DESKTOP == 1,     "FOLDER_DESKTOP must be 1")
assert(mod.FOLDER_VIDEOS == 10,     "FOLDER_VIDEOS must be 10")
-- 3) PathType constants
assert(mod.PATHTYPE_NONE == 0,      "PATHTYPE_NONE")
assert(mod.PATHTYPE_FILE == 1,      "PATHTYPE_FILE")
assert(mod.PATHTYPE_DIRECTORY == 2, "PATHTYPE_DIRECTORY")
assert(mod.PATHTYPE_OTHER == 3,     "PATHTYPE_OTHER")
-- 4) Glob flag
assert(mod.GLOB_CASEINSENSITIVE == 1, "GLOB_CASEINSENSITIVE must be 1")
pass("Light.Filesystem constants ok (16)")

-- 5) GetBasePath
local base = mod.GetBasePath()
assert(type(base) == "string" and #base > 0, "GetBasePath empty")
pass("GetBasePath ok: " .. base)

-- 6) GetCurrentDirectory
local cwd = mod.GetCurrentDirectory()
assert(type(cwd) == "string" and #cwd > 0, "GetCurrentDirectory empty")
pass("GetCurrentDirectory ok: " .. cwd)

-- 7) GetPrefPath - writable scratch area
local pref = mod.GetPrefPath("LightCI", "phase_t")
assert(type(pref) == "string" and #pref > 0, "GetPrefPath empty")
pass("GetPrefPath ok: " .. pref)

-- 8) Inside pref/, run a full mutation lifecycle:
--    create dir -> copy file -> rename -> stat -> glob -> remove
local subdir   = pref .. "phase_t_smoke"
local file_a   = subdir .. "/a.txt"
local file_b   = subdir .. "/b.txt"
local file_a_renamed = subdir .. "/a_renamed.txt"

-- best-effort cleanup from prior aborted run (errors ignored)
mod.RemovePath(file_a)
mod.RemovePath(file_b)
mod.RemovePath(file_a_renamed)
mod.RemovePath(subdir)

-- CreateDirectory
local ok_cd, err_cd = mod.CreateDirectory(subdir)
assert(ok_cd, "CreateDirectory failed: " .. tostring(err_cd))
pass("CreateDirectory ok: " .. subdir)

-- Write a file using Lua io (Light.Filesystem doesn't expose write yet,
-- but we just need ANY file to test path ops).
local f = io.open(file_a, "wb")
assert(f, "open file_a for write failed")
f:write("hello phase t\n")
f:close()

-- GetPathInfo on an existing file
local info, ierr = mod.GetPathInfo(file_a)
assert(info, "GetPathInfo failed: " .. tostring(ierr))
assert(info.type == mod.PATHTYPE_FILE,
       "info.type must be PATHTYPE_FILE, got " .. tostring(info.type))
assert(info.size > 0, "info.size must be > 0, got " .. tostring(info.size))
assert(type(info.modify_time) == "number", "info.modify_time missing")
pass(string.format("GetPathInfo(file) ok: type=%d size=%d modify_time=%s",
                   info.type, info.size, tostring(info.modify_time)))

-- GetPathInfo on a directory
local dinfo = mod.GetPathInfo(subdir)
assert(dinfo and dinfo.type == mod.PATHTYPE_DIRECTORY,
       "subdir info wrong: " .. tostring(dinfo and dinfo.type))
pass("GetPathInfo(directory) ok")

-- GetPathInfo on non-existent path: bool failure path
local ninfo, nerr = mod.GetPathInfo(subdir .. "/does_not_exist_xyz")
assert(ninfo == nil and nerr ~= nil, "GetPathInfo(missing) must return nil,err")
pass("GetPathInfo(missing) ok: " .. tostring(nerr))

-- CopyFile
local ok_cp, err_cp = mod.CopyFile(file_a, file_b)
assert(ok_cp, "CopyFile failed: " .. tostring(err_cp))
local b_info = mod.GetPathInfo(file_b)
assert(b_info and b_info.type == mod.PATHTYPE_FILE, "copied file missing")
assert(b_info.size == info.size, "copied size differs")
pass("CopyFile ok (size matches)")

-- RenamePath
local ok_rn, err_rn = mod.RenamePath(file_a, file_a_renamed)
assert(ok_rn, "RenamePath failed: " .. tostring(err_rn))
local rn_info = mod.GetPathInfo(file_a_renamed)
assert(rn_info and rn_info.type == mod.PATHTYPE_FILE, "renamed file not found")
pass("RenamePath ok")

-- GlobDirectory
local list, lerr = mod.GlobDirectory(subdir, "*.txt", 0)
assert(list, "GlobDirectory failed: " .. tostring(lerr))
assert(#list >= 2, "GlobDirectory must return >= 2 .txt files, got " .. #list)
local seen_renamed, seen_b = false, false
for _, name in ipairs(list) do
    if name == "a_renamed.txt" then seen_renamed = true end
    if name == "b.txt"         then seen_b = true end
end
assert(seen_renamed and seen_b, "GlobDirectory missing expected entries")
pass(string.format("GlobDirectory ok (%d entries)", #list))

-- GlobDirectory case-insensitive
local list_ci = mod.GlobDirectory(subdir, "*.TXT", mod.GLOB_CASEINSENSITIVE)
assert(list_ci and #list_ci == #list,
       "case-insensitive glob should match same files: " .. tostring(list_ci and #list_ci))
pass("GlobDirectory CASEINSENSITIVE ok")

-- RemovePath - must remove leaf files first (RemovePath only removes empty dirs)
mod.RemovePath(file_a_renamed)
mod.RemovePath(file_b)
local ok_rm, err_rm = mod.RemovePath(subdir)
assert(ok_rm, "RemovePath(empty dir) failed: " .. tostring(err_rm))
local gone = mod.GetPathInfo(subdir)
assert(gone == nil, "subdir must be gone after RemovePath")
pass("RemovePath ok (cleanup complete)")

-- 9) Boundary
local err_ok = pcall(mod.GetUserFolder, 999)
-- GetUserFolder out-of-range returns (nil, err) WITHOUT raising, since we
-- handle the bound check ourselves and push nil/err.
local uf, uerr = mod.GetUserFolder(999)
assert(uf == nil and uerr ~= nil, "GetUserFolder(999) must return nil,err")
pass("GetUserFolder(999) boundary ok: " .. tostring(uerr))

local cd_fail, cd_err = mod.CreateDirectory("")
-- Empty path: SDL behavior is platform-dependent but should fail OR
-- succeed with current dir; we just verify the API returns sane values.
assert(type(cd_fail) == "boolean", "CreateDirectory must return boolean")
pass("CreateDirectory(empty) returns boolean: " .. tostring(cd_fail))

local pi_fail, pi_err = mod.GetPathInfo("Z:/this/definitely/does/not/exist/" .. tostring(os.time()))
assert(pi_fail == nil and pi_err ~= nil, "missing-path GetPathInfo must return nil,err")
pass("GetPathInfo(missing absolute) boundary ok")

print("filesystem smoke ok")
