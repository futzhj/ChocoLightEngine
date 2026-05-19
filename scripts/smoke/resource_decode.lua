local sep = package.config:sub(1, 1)
local base = "scripts" .. sep .. "smoke" .. sep

dofile(base .. "resource_decode_tcp_sp.lua")
dofile(base .. "resource_decode_map_m10.lua")
dofile(base .. "resource_decode_map_01m.lua")

print("[resource_decode] PASS")
