-- Phase AE smoke: Light.IOStream (SDL_iostream.h)
-- ASCII-only.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.IOStream")
if not ok then fail("require(Light.IOStream) failed: " .. tostring(mod)) end

-- 1) 45 fns
local fn_names = {
    "IOFromFile","IOFromMem","IOFromConstMem","IOFromDynamicMem","CloseIO",
    "GetIOProperties","GetIOStatus","GetIOSize","FlushIO",
    "SeekIO","TellIO","ReadIO","WriteIO",
    "LoadFile","SaveFile","LoadFile_IO","SaveFile_IO",
    "ReadU8","ReadS8","ReadU16LE","ReadS16LE","ReadU16BE","ReadS16BE",
    "ReadU32LE","ReadS32LE","ReadU32BE","ReadS32BE",
    "ReadU64LE","ReadS64LE","ReadU64BE","ReadS64BE",
    "WriteU8","WriteS8","WriteU16LE","WriteS16LE","WriteU16BE","WriteS16BE",
    "WriteU32LE","WriteS32LE","WriteU32BE","WriteS32BE",
    "WriteU64LE","WriteS64LE","WriteU64BE","WriteS64BE",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.IOStream." .. k .. " missing") end
end
pass("Light.IOStream module ok (" .. #fn_names .. " fns)")

-- 2) Constants
assert(mod.SEEK_SET == 0 and mod.SEEK_CUR == 1 and mod.SEEK_END == 2,
       "SEEK_* constants")
assert(type(mod.STATUS_READY) == "number")
pass("Constants ok")

-- ============================================================
-- IOFromConstMem (read-only)
-- ============================================================
local payload = "Hello, IOStream!"
local rio = mod.IOFromConstMem(payload)
assert(type(rio) == "userdata", "IOFromConstMem must return userdata")

local sz = mod.GetIOSize(rio)
assert(sz == #payload, "size mismatch: " .. tostring(sz))
assert(mod.TellIO(rio) == 0, "initial pos must be 0")

local data, n = mod.ReadIO(rio, 5)
assert(data == "Hello" and n == 5, "ReadIO 5 bytes")
assert(mod.TellIO(rio) == 5, "pos after read")

-- Seek to start
assert(mod.SeekIO(rio, 0, mod.SEEK_SET) == 0)
local full, fn = mod.ReadIO(rio, 100)
assert(full == payload, "full read mismatch")
assert(fn == #payload, "bytes count mismatch: " .. tostring(fn))

-- After EOF, ReadIO returns nil + 0
local eof, ec = mod.ReadIO(rio, 1)
assert(eof == nil and ec == 0, "EOF must return nil + 0")

mod.CloseIO(rio)
pass("IOFromConstMem read + seek + EOF ok")

-- Use-after-close
local rok = pcall(mod.ReadIO, rio, 1)
if rok then fail("ReadIO on closed stream should raise") end
pass("Use-after-close raises ok")

-- ============================================================
-- IOFromDynamicMem (writable, growing)
-- ============================================================
local dyn = mod.IOFromDynamicMem()
assert(type(dyn) == "userdata")

assert(mod.WriteU8(dyn, 0xAB) == true, "WriteU8")
assert(mod.WriteU16LE(dyn, 0x1234) == true, "WriteU16LE")
assert(mod.WriteU16BE(dyn, 0x5678) == true, "WriteU16BE")
assert(mod.WriteU32LE(dyn, 0xDEADBEEF) == true, "WriteU32LE")
assert(mod.WriteS32BE(dyn, -1) == true, "WriteS32BE -1")

-- 1+2+2+4+4 = 13 bytes written
assert(mod.GetIOSize(dyn) == 13, "size after writes: " .. tostring(mod.GetIOSize(dyn)))

-- Seek back and read
assert(mod.SeekIO(dyn, 0, mod.SEEK_SET) == 0)
assert(mod.ReadU8(dyn) == 0xAB, "ReadU8 round-trip")
assert(mod.ReadU16LE(dyn) == 0x1234, "ReadU16LE round-trip")
assert(mod.ReadU16BE(dyn) == 0x5678, "ReadU16BE round-trip")
assert(mod.ReadU32LE(dyn) == 0xDEADBEEF, "ReadU32LE round-trip")
assert(mod.ReadS32BE(dyn) == -1, "ReadS32BE -1 round-trip")
pass("Dynamic mem typed read/write round-trip ok")

mod.CloseIO(dyn)

-- ============================================================
-- LoadFile / SaveFile via temp file
-- ============================================================
local tmp = os.getenv("TEMP") or os.getenv("TMP") or "/tmp"
tmp = tmp .. "/light_iostream_smoke_" .. os.time() .. ".bin"

local content = "binary\0\1\2\3 bytes\255"
local sok, serr = mod.SaveFile(tmp, content)
if not sok then fail("SaveFile: " .. tostring(serr)) end

local back, lerr = mod.LoadFile(tmp)
if not back then fail("LoadFile: " .. tostring(lerr)) end
assert(back == content,
       string.format("round-trip mismatch: len in=%d out=%d", #content, #back))
pass("LoadFile/SaveFile binary round-trip ok")

os.remove(tmp)

-- ============================================================
-- IOFromFile + ReadIO
-- ============================================================
local tmp2 = tmp .. ".f2"
mod.SaveFile(tmp2, "abcdef")
local f = mod.IOFromFile(tmp2, "rb")
assert(type(f) == "userdata", "IOFromFile failed")
assert(mod.GetIOSize(f) == 6)
local d, _ = mod.ReadIO(f, 6)
assert(d == "abcdef", "IOFromFile read mismatch")
mod.CloseIO(f)
os.remove(tmp2)
pass("IOFromFile read ok")

-- ============================================================
-- Status
-- ============================================================
local s = mod.IOFromConstMem("xyz")
assert(mod.GetIOStatus(s) == mod.STATUS_READY, "fresh stream STATUS_READY")
mod.CloseIO(s)
pass("GetIOStatus ok")

-- ============================================================
-- Empty IOFromMem must error (boundary)
-- ============================================================
local rb = pcall(mod.IOFromMem, "")
if rb then fail("IOFromMem(empty) should raise") end
pass("IOFromMem(empty) raises ok")

print("iostream smoke ok")
