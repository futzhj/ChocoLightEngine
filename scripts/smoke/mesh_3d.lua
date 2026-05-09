-- Phase AS.2 smoke: Light.Graphics.Mesh + 3D camera + 深度测试
--
-- CI runner 上无 GL context, 关键验证:
--   * 模块加载 + 函数注册
--   * Mesh.New 在无 GL 时返回 nil + err string (不崩)
--   * SetPerspective / SetCamera / SetDepthTest 调用安全
--   * 兼容性: AS.1 Canvas/Shader 仍可用

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- ==================== 1) Light.Graphics 3D 控制 ====================

local ok, G = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(G)) end

local g_fns = { "SetPerspective", "SetCamera", "SetDepthTest", "GetDepthTest" }
for _, k in ipairs(g_fns) do
    if type(G[k]) ~= "function" then
        fail("Graphics." .. k .. " not a function (got " .. type(G[k]) .. ")")
    end
end
pass("Light.Graphics 3D control fns ok (" .. #g_fns .. " fns)")

-- SetPerspective: 不崩
G.SetPerspective(60, 16/9, 0.1, 1000)
pass("SetPerspective(60, 16/9, 0.1, 1000) ok")

-- SetCamera: 6 参 + 9 参两种形式
G.SetCamera(0, 5, 10,  0, 0, 0)
pass("SetCamera(eye, target) 6-arg form ok")
G.SetCamera(0, 5, 10,  0, 0, 0,  0, 1, 0)
pass("SetCamera(eye, target, up) 9-arg form ok")

-- SetDepthTest / GetDepthTest round-trip
G.SetDepthTest(true)
local enabled = G.GetDepthTest()
if type(enabled) ~= "boolean" then fail("GetDepthTest should return boolean") end
if enabled ~= true then fail("after SetDepthTest(true), GetDepthTest = " .. tostring(enabled)) end
pass("SetDepthTest(true) -> GetDepthTest = true")

G.SetDepthTest(false)
if G.GetDepthTest() ~= false then
    fail("SetDepthTest(false) did not stick")
end
pass("SetDepthTest(false) -> GetDepthTest = false")

-- ==================== 2) Light.Graphics.Mesh 模块 ====================

local ok2, Mesh = pcall(require, "Light.Graphics.Mesh")
if not ok2 then fail("require(Light.Graphics.Mesh) failed: " .. tostring(Mesh)) end
if type(Mesh) ~= "table" then fail("Light.Graphics.Mesh not a table") end

if type(Mesh.New) ~= "function" then fail("Mesh.New not function") end
if type(Mesh.GetVertexFormat) ~= "function" then fail("Mesh.GetVertexFormat not function") end
pass("Light.Graphics.Mesh module loaded (2 static fns)")

-- GetVertexFormat
local fmt = Mesh.GetVertexFormat()
if type(fmt) ~= "string" then fail("GetVertexFormat should return string") end
if not fmt:find("pos3") or not fmt:find("normal3") or not fmt:find("uv2") or not fmt:find("color4") then
    fail("VertexFormat string missing expected fields: " .. fmt)
end
pass("Mesh.GetVertexFormat() = '" .. fmt .. "'")

-- ==================== 3) Mesh.New 边界路径 ====================

-- 空 vertex table → 失败
local m1, err1 = Mesh.New({}, {})
if m1 ~= nil then fail("Mesh.New({}, {}) should fail") end
if type(err1) ~= "string" then fail("expected err string, got " .. type(err1)) end
pass("Mesh.New({}, {}) -> nil, '" .. err1 .. "'")

-- vertex 长度不是 12 倍数 → 失败
local m2, err2 = Mesh.New({ 1, 2, 3, 4, 5 }, { 1, 2, 3 })
if m2 ~= nil then fail("Mesh.New(5 floats) should fail") end
pass("Mesh.New(5 floats) -> nil, '" .. tostring(err2) .. "'")

-- index 不是 3 倍数 → 失败
local v12 = { 0,0,0, 0,1,0, 0,0, 1,1,1,1 }  -- 1 vertex (12 floats)
-- 复制 3 次 (3 vertices)
local v36 = {}
for i = 1, 3 do
    for _, f in ipairs(v12) do table.insert(v36, f) end
end
local m3, err3 = Mesh.New(v36, { 1, 2 })  -- 2 indices, not multiple of 3
if m3 ~= nil then fail("Mesh.New with 2 indices should fail") end
pass("Mesh.New(3 verts, 2 indices) -> nil, '" .. tostring(err3) .. "'")

-- ==================== 4) 合法 mesh: 三角形 ====================

-- 3 顶点 + 3 索引 (一个三角形) — 在 headless 下 g_render==nullptr 应失败但不崩
local triVerts = {
    --  pos      | normal  | uv  | color
    -1, 0, 0,    0, 0, 1,  0, 0,  1, 0, 0, 1,
     1, 0, 0,    0, 0, 1,  1, 0,  0, 1, 0, 1,
     0, 1, 0,    0, 0, 1,  0.5, 1, 0, 0, 1, 1,
}
local triIndices = { 1, 2, 3 }
local mesh, err4 = Mesh.New(triVerts, triIndices)
-- mesh 在无 window 时应该是 nil + err
if mesh ~= nil then
    -- 如果 CI 实际有 GL 上下文, 可能成功创建
    pass("Mesh.New(triangle) returned userdata (GL context active)")
    if type(mesh.GetVertexCount) ~= "function" then fail("mesh:GetVertexCount missing") end
    if type(mesh.GetIndexCount) ~= "function" then fail("mesh:GetIndexCount missing") end
    if type(mesh.Draw) ~= "function" then fail("mesh:Draw missing") end
    if type(mesh.Delete) ~= "function" then fail("mesh:Delete missing") end

    local vc = mesh:GetVertexCount()
    local ic = mesh:GetIndexCount()
    if vc ~= 3 then fail("expected vertexCount=3, got " .. vc) end
    if ic ~= 3 then fail("expected indexCount=3, got " .. ic) end
    pass("mesh:GetVertexCount=" .. vc .. ", GetIndexCount=" .. ic)

    -- mesh:Draw 不崩 (无纹理 + 有纹理两种)
    mesh:Draw()
    mesh:Draw(0)
    pass("mesh:Draw() / Draw(0) ok")

    mesh:Delete()
    pass("mesh:Delete() ok")
else
    pass("Mesh.New(triangle) returned nil in headless (err='" .. tostring(err4) .. "') - expected")
end

-- ==================== 5) 兼容性回归 ====================

-- AS.1 Canvas Shader 仍可用
local ok5, Canvas = pcall(require, "Light.Graphics.Canvas")
if not ok5 then fail("require(Light.Graphics.Canvas) regression!") end
if type(Canvas.GetTextureId) ~= "function" then fail("Canvas.GetTextureId regression!") end

local ok6, Shader = pcall(require, "Light.Graphics.Shader")
if not ok6 then fail("require(Light.Graphics.Shader) regression!") end
if type(Shader.New) ~= "function" then fail("Shader.New regression!") end

-- AR Light.Event
local ok7, Event = pcall(require, "Light.Event")
if not ok7 then fail("require(Light.Event) regression!") end
if type(Event.Push) ~= "function" then fail("Event.Push regression!") end

pass("Light.Graphics.Canvas / Shader + Light.Event backward compat ok")

print("mesh_3d smoke ok")
