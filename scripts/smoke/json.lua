local JSON = require("Light.Plugins.JSON")
assert(Light.Plugins.JSON == JSON)
assert(type(JSON.Encode) == "function")
assert(type(JSON.Decode) == "function")
assert(type(JSON.Null) == "function")
assert(type(JSON.IsNull) == "function")

local text = assert(JSON.Encode({ name = "choco", count = 3, ok = true }))
assert(type(text) == "string")
local obj = assert(JSON.Decode(text))
assert(obj.name == "choco")
assert(obj.count == 3)
assert(obj.ok == true)

local arr = assert(JSON.Decode("[1,2,3]"))
assert(arr[1] == 1 and arr[2] == 2 and arr[3] == 3)
local nullValue = assert(JSON.Decode("null"))
assert(JSON.IsNull(nullValue) == true)

local bad, err = JSON.Decode("{")
assert(bad == nil and type(err) == "string")

local json = require("json")
assert(type(json.encode) == "function")
assert(type(json.decode) == "function")
assert(json.decode(json.encode({x = 1})).x == 1)

print("[smoke] json ok")
