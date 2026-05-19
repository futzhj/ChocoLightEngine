local lfs = require("lfs")
assert(type(lfs.attributes) == "function")
assert(type(lfs.dir) == "function")
assert(type(lfs.mkdir) == "function")
assert(type(lfs.rmdir) == "function")
assert(type(lfs.currentdir) == "function")

local md5 = require("md5")
assert(md5.sumhexa("abc") == "900150983cd24fb0d6963f7d28e17f72")
assert(#md5.sum("abc") == 16)

local sha1 = require("sha1")
assert(sha1.sumhexa("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#sha1.sum("abc") == 20)

local cur = assert(lfs.currentdir())
assert(type(cur) == "string" and #cur > 0)
local attr = assert(lfs.attributes(cur))
assert(type(attr) == "table")
assert(attr.mode == "directory" or attr.mode == "file" or attr.mode == "other")

print("[smoke] lua compat utils ok")
