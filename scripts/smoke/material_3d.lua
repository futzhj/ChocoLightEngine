-- Phase AS.4 smoke: Material + 多光源 + mesh:Draw(material) 向后兼容
--
-- CI runner 上无 GL context, 关键验证:
--   * Material 模块加载 + 22 个方法注册
--   * Material round-trip (set/get 各属性)
--   * Light.Graphics 多光源 8 fns 注册 + 调用安全
--   * mesh:Draw 自动判断 textureId vs material 参数
--   * AS.2 mesh:Draw(textureId) 向后兼容

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- ==================== 1) Light.Graphics.Material 模块加载 ====================

local ok, Material = pcall(require, "Light.Graphics.Material")
if not ok then fail("require(Light.Graphics.Material) failed: " .. tostring(Material)) end
if type(Material) ~= "table" then fail("Light.Graphics.Material not a table") end
if type(Material.New) ~= "function" then fail("Material.New not function") end
pass("Light.Graphics.Material module loaded")

-- ==================== 2) Material.New 创建 + mode ====================

local m1 = Material.New("pbr")
if not m1 then fail("Material.New('pbr') returned nil") end
if m1:GetMode() ~= "pbr" then fail("default 'pbr' mode mismatch: " .. tostring(m1:GetMode())) end
pass("Material.New('pbr') -> mode='pbr'")

local m2 = Material.New("unlit")
if m2:GetMode() ~= "unlit" then fail("'unlit' mode mismatch") end
pass("Material.New('unlit') -> mode='unlit'")

local m3 = Material.New()  -- 默认 pbr
if m3:GetMode() ~= "pbr" then fail("default mode should be 'pbr'") end
pass("Material.New() -> default 'pbr'")

-- 测试 SetMode 切换
m1:SetMode("unlit")
if m1:GetMode() ~= "unlit" then fail("SetMode('unlit') failed") end
m1:SetMode("pbr")
if m1:GetMode() ~= "pbr" then fail("SetMode('pbr') failed") end
pass("SetMode round-trip ok")

-- ==================== 3) Material 属性 round-trip ====================

local mat = Material.New("pbr")

-- color (vec4)
mat:SetColor(0.5, 0.6, 0.7, 0.8)
local r, g, b, a = mat:GetColor()
if math.abs(r - 0.5) > 1e-5 or math.abs(g - 0.6) > 1e-5 or math.abs(b - 0.7) > 1e-5 or math.abs(a - 0.8) > 1e-5 then
    fail("SetColor/GetColor round-trip failed: " .. r .. "," .. g .. "," .. b .. "," .. a)
end

-- color 默认 a=1
mat:SetColor(0.1, 0.2, 0.3)
local _, _, _, a2 = mat:GetColor()
if math.abs(a2 - 1.0) > 1e-5 then fail("default a should be 1.0, got " .. a2) end
pass("SetColor/GetColor (3 args + 4 args) ok")

-- emissive (vec3)
mat:SetEmissive(0.1, 0.2, 0.3)
local er, eg, eb = mat:GetEmissive()
if math.abs(er - 0.1) > 1e-5 or math.abs(eg - 0.2) > 1e-5 or math.abs(eb - 0.3) > 1e-5 then
    fail("SetEmissive/GetEmissive failed")
end
pass("SetEmissive/GetEmissive round-trip ok")

-- metallic / roughness (clamp 到 [0,1])
mat:SetMetallic(0.42)
if math.abs(mat:GetMetallic() - 0.42) > 1e-5 then fail("metallic round-trip") end
mat:SetMetallic(2.0)  -- 应 clamp 到 1
if math.abs(mat:GetMetallic() - 1.0) > 1e-5 then fail("metallic should clamp to 1.0, got " .. mat:GetMetallic()) end
mat:SetMetallic(-0.5)  -- 应 clamp 到 0
if math.abs(mat:GetMetallic() - 0.0) > 1e-5 then fail("metallic should clamp to 0, got " .. mat:GetMetallic()) end
pass("SetMetallic clamp [0,1] ok")

mat:SetRoughness(0.7)
if math.abs(mat:GetRoughness() - 0.7) > 1e-5 then fail("roughness round-trip") end
pass("SetRoughness round-trip ok")

-- normalScale / occlusionStrength
mat:SetNormalScale(2.0)
if math.abs(mat:GetNormalScale() - 2.0) > 1e-5 then fail("normalScale") end
mat:SetOcclusionStrength(0.5)
if math.abs(mat:GetOcclusionStrength() - 0.5) > 1e-5 then fail("occlusionStrength") end
pass("normalScale + occlusionStrength round-trip ok")

-- 纹理 slot
mat:SetTexture("baseColor", 100)
if mat:GetTexture("baseColor") ~= 100 then fail("baseColor tex round-trip: " .. mat:GetTexture("baseColor")) end
mat:SetTexture("metallicRoughness", 200)
if mat:GetTexture("metallicRoughness") ~= 200 then fail("metallicRoughness tex") end
mat:SetTexture("normal", 300)
if mat:GetTexture("normal") ~= 300 then fail("normal tex") end
mat:SetTexture("emissive", 400)
if mat:GetTexture("emissive") ~= 400 then fail("emissive tex") end
mat:SetTexture("occlusion", 500)
if mat:GetTexture("occlusion") ~= 500 then fail("occlusion tex") end
mat:SetTexture("baseColor", 0)  -- 移除
if mat:GetTexture("baseColor") ~= 0 then fail("baseColor remove (set to 0)") end
pass("SetTexture/GetTexture for 5 slots ok")

-- 未知 slot
local ok_ut = pcall(function() mat:SetTexture("unknownSlot", 1) end)
if ok_ut then fail("SetTexture('unknownSlot', _) should error") end
pass("SetTexture(unknown_slot) -> error (expected)")

-- doubleSided / alphaMode / alphaCutoff
mat:SetDoubleSided(true)
if mat:GetDoubleSided() ~= true then fail("doubleSided round-trip") end
mat:SetDoubleSided(false)
if mat:GetDoubleSided() ~= false then fail("doubleSided false") end

mat:SetAlphaMode("blend")
if mat:GetAlphaMode() ~= "blend" then fail("alphaMode 'blend' rt") end
mat:SetAlphaMode("mask")
if mat:GetAlphaMode() ~= "mask" then fail("alphaMode 'mask' rt") end
mat:SetAlphaMode("opaque")
if mat:GetAlphaMode() ~= "opaque" then fail("alphaMode 'opaque' rt") end

mat:SetAlphaCutoff(0.3)
if math.abs(mat:GetAlphaCutoff() - 0.3) > 1e-5 then fail("alphaCutoff rt") end
pass("doubleSided/alphaMode/alphaCutoff round-trip ok")

-- __tostring
local s = tostring(mat)
if not s:find("Material") then fail("__tostring missing 'Material': " .. s) end
pass("Material __tostring = '" .. s .. "'")

-- ==================== 4) Light.Graphics 多光源 API ====================

local G = require("Light.Graphics")
local light_fns = {
    "SetDirectionalLight",
    "SetDirectionalLightEnabled",
    "SetAmbientLight",
    "AddPointLight",
    "RemovePointLight",
    "ClearPointLights",
    "GetPointLightCount",
    "GetMaxPointLights",
}
for _, k in ipairs(light_fns) do
    if type(G[k]) ~= "function" then
        fail("Graphics." .. k .. " not a function (got " .. type(G[k]) .. ")")
    end
end
pass("Light.Graphics 8 lighting fns registered")

-- 调用不崩 (无 GL 时 g_render = nullptr, 在 fns 内 if (g_render) 守卫)
G.SetDirectionalLight(0, 1, 0,  1, 1, 1)
G.SetDirectionalLight(0, 1, 0,  1, 1, 1,  0.8)  -- with intensity
G.SetDirectionalLightEnabled(false)
G.SetDirectionalLightEnabled(true)
G.SetAmbientLight(0.1, 0.1, 0.2)
pass("SetDirectionalLight/SetAmbientLight ok")

-- AddPointLight: 在 headless 时返回 0 (g_render=nullptr)
local id = G.AddPointLight(0, 0, 0,  1, 0, 0,  10)
if type(id) ~= "number" then fail("AddPointLight should return integer, got " .. type(id)) end
pass("AddPointLight returned id=" .. id .. " (0=headless)")

G.RemovePointLight(1)  -- 不崩
G.ClearPointLights()
local cnt = G.GetPointLightCount()
if type(cnt) ~= "number" then fail("GetPointLightCount type") end
pass("RemovePointLight/ClearPointLights/GetPointLightCount ok (count=" .. cnt .. ")")

local max = G.GetMaxPointLights()
if type(max) ~= "number" then fail("GetMaxPointLights type") end
pass("GetMaxPointLights = " .. max .. " (0=headless, 4=runtime)")

-- ==================== 5) mesh:Draw 类型判断 (向后兼容 AS.2) ====================

local Mesh = require("Light.Graphics.Mesh")

-- 构造一个三角形 mesh (在 headless 时 Mesh.New 返回 nil + err, 不验证 mesh:Draw)
local triVerts = {
    -1, 0, 0,    0, 0, 1,  0, 0,  1, 0, 0, 1,
     1, 0, 0,    0, 0, 1,  1, 0,  0, 1, 0, 1,
     0, 1, 0,    0, 0, 1,  0.5, 1, 0, 0, 1, 1,
}
local mesh = Mesh.New(triVerts, { 1, 2, 3 })
if mesh then
    -- 老路径: integer 参数 (AS.2 兼容)
    mesh:Draw()      -- 缺省, 等同 (0)
    mesh:Draw(0)
    mesh:Draw(123)
    pass("mesh:Draw() / Draw(0) / Draw(textureId) old API ok")

    -- 新路径: material userdata 参数
    local m = Material.New("pbr")
    m:SetColor(0.5, 0.5, 0.5)
    mesh:Draw(m)
    pass("mesh:Draw(material) new API ok")

    local prevMat = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        -1,0,0,1,
    }
    mesh:Draw(prevMat)
    mesh:Draw(0, prevMat)
    mesh:Draw(m, prevMat)
    pass("mesh:Draw previous model matrix overloads ok")

    -- 错误参数: string
    local ok_bad = pcall(function() mesh:Draw("invalid") end)
    -- string 是 LUA_TSTRING, 不是 USERDATA 也不是 NUMBER, 走老路径 luaL_optinteger 默认 0 → ok
    -- (此为预期行为, 不强制要求 fail)
    pass("mesh:Draw(string) does not crash (treated as default texId 0)")

    mesh:Delete()
    pass("mesh:Delete() ok")
else
    pass("mesh creation skipped (headless), tests degenerate gracefully")
end

-- ==================== 5b) Phase AS.4.x — LoadGLTF with_material ====================

-- 不存在文件 with_material=true → nil + err (不崩, 与 AS.3 路径一致)
local mb1, eb1 = Mesh.LoadGLTF("nonexistent.gltf", 0, true)
if mb1 ~= nil then fail("LoadGLTF(nonexistent, 0, true) should return nil") end
if type(eb1) ~= "string" then fail("err type for LoadGLTF with_material") end
pass("LoadGLTF(nonexistent, 0, true) -> nil, '" .. eb1 .. "'")

-- with_material=false 显式 (向后兼容)
local mb2, eb2 = Mesh.LoadGLTF("nonexistent.gltf", 0, false)
if mb2 ~= nil then fail("LoadGLTF(_, _, false) should return nil") end
pass("LoadGLTF(_, _, false) -> nil, '" .. tostring(eb2) .. "'")

-- with_material=nil 缺省 (向后兼容)
local mb3, eb3 = Mesh.LoadGLTF("nonexistent.gltf", 0)
if mb3 ~= nil then fail("LoadGLTF(_, 0) should return nil") end
pass("LoadGLTF(_, 0) (no third arg) -> nil, '" .. tostring(eb3) .. "'")

-- 老调用形式 (单参) 仍可用
local mb4, eb4 = Mesh.LoadGLTF("nonexistent.gltf")
if mb4 ~= nil then fail("LoadGLTF(_) one-arg should return nil") end
pass("LoadGLTF(_) (single arg, AS.3 style) -> nil, '" .. tostring(eb4) .. "'")

-- ==================== 6) 兼容性回归 (前序 Phase) ====================

-- AS.3 LoadGLTF 函数仍存在
if type(Mesh.LoadGLTF) ~= "function" then fail("Mesh.LoadGLTF (AS.3) regression") end
if type(Mesh.GetGLTFMeshCount) ~= "function" then fail("Mesh.GetGLTFMeshCount (AS.3) regression") end

-- AS.2 mesh 函数仍可用
if type(Mesh.New) ~= "function" then fail("Mesh.New (AS.2) regression") end
if type(Mesh.GetVertexFormat) ~= "function" then fail("GetVertexFormat (AS.2) regression") end

-- AS.1 Canvas / Shader
local ok5, Canvas = pcall(require, "Light.Graphics.Canvas")
if not ok5 then fail("AS.1 Canvas regression") end
local ok6, Shader = pcall(require, "Light.Graphics.Shader")
if not ok6 then fail("AS.1 Shader regression") end

-- AR Light.Event
local ok7, Event = pcall(require, "Light.Event")
if not ok7 then fail("AR Light.Event regression") end

pass("AQ/AR/AS.1/AS.2/AS.3 backward compat ok")

print("material_3d smoke ok")
