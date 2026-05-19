local Codec = require("Light.Plugins.Codec")
assert(Light.Plugins.Codec == Codec)
assert(type(Codec) == "table")
assert(type(Codec.HexEncode) == "function")
assert(type(Codec.HexDecode) == "function")
assert(type(Codec.URLEncode) == "function")
assert(type(Codec.URLDecode) == "function")
assert(type(Codec.Base64Encode) == "function")
assert(type(Codec.Base64Decode) == "function")

local raw = string.char(0, 1, 2, 15, 16, 255)
assert(Codec.HexEncode(raw) == "0001020f10ff")
assert(Codec.HexEncode(raw, true) == "0001020F10FF")
assert(Codec.HexDecode("0001020f10ff") == raw)

local bad, err = Codec.HexDecode("abc")
assert(bad == nil and type(err) == "string")

local url = Codec.URLEncode("a b+c/?")
assert(url == "a%20b%2Bc%2F%3F")
assert(Codec.URLDecode(url) == "a b+c/?")

local b64 = Codec.Base64Encode("hello")
assert(b64 == "aGVsbG8=")
assert(Codec.Base64Decode(b64) == "hello")

print("[smoke] codec ok")
