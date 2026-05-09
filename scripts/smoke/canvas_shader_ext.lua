-- Phase AS.1 smoke: Canvas 增强 + Shader uniform 扩展
--
-- CI runner 上无 GL context (headless), 关键验证:
--   * 新增方法/常量全部注册到 Lua
--   * 在 g_render == nullptr / Canvas 未创建 / Shader Unsupported 情况下安全失败
--   * 无窗口时调用 Push/PopCanvas / Shader setter 不崩

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- ==================== 1) Light.Graphics 模块加载 + PushCanvas/PopCanvas ====================

local ok, G = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(G)) end
if type(G) ~= "table" then fail("Light.Graphics not a table") end

if type(G.PushCanvas) ~= "function" then fail("Graphics.PushCanvas not function") end
if type(G.PopCanvas) ~= "function" then fail("Graphics.PopCanvas not function") end
pass("Light.Graphics.PushCanvas / PopCanvas registered ok")

-- 现有 SetCanvas / GetCanvas 仍存在 (回归)
if type(G.SetCanvas) ~= "function" then fail("Graphics.SetCanvas regression!") end
if type(G.GetCanvas) ~= "function" then fail("Graphics.GetCanvas regression!") end
pass("Light.Graphics SetCanvas/GetCanvas backward compat ok")

-- PopCanvas 在空栈时仅警告不崩
G.PopCanvas()
G.PopCanvas()
pass("PopCanvas on empty stack is safe (no crash)")

-- ==================== 2) Light.Graphics.Canvas Class 元表 ====================

local ok2, Canvas = pcall(require, "Light.Graphics.Canvas")
if not ok2 then fail("require(Light.Graphics.Canvas) failed: " .. tostring(Canvas)) end

-- Canvas table 应有 4 个新方法
local canvas_fns = { "GetTextureId", "GetWidth", "GetHeight", "Clear" }
for _, k in ipairs(canvas_fns) do
    if type(Canvas[k]) ~= "function" then
        fail("Canvas." .. k .. " not a function (got " .. type(Canvas[k]) .. ")")
    end
end
pass("Light.Graphics.Canvas 4 enhanced methods registered ok")

-- 在没有 GL context 时无法实例化 canvas, 但 class 上的方法接受 nil ctx 应安全
-- 模拟: 用空 table 作 self, 方法应返回 0 / 不崩
local self_stub = {}
local id = Canvas.GetTextureId(self_stub)
if type(id) ~= "number" then fail("GetTextureId should return number") end
if id ~= 0 then fail("GetTextureId on stub should return 0, got " .. id) end
pass("Canvas.GetTextureId(stub) = 0 (no crash)")

local w = Canvas.GetWidth(self_stub)
local h = Canvas.GetHeight(self_stub)
if type(w) ~= "number" or type(h) ~= "number" then
    fail("GetWidth/GetHeight should return number")
end
pass("Canvas.GetWidth/GetHeight on stub = " .. w .. "/" .. h)

Canvas.Clear(self_stub, 0.5, 0.5, 0.5, 1.0)
pass("Canvas.Clear(stub) ok (no crash)")

-- ==================== 3) Light.Graphics.Image 增加 GetTextureId ====================

local ok3, Image = pcall(require, "Light.Graphics.Image")
if not ok3 then fail("require(Light.Graphics.Image) failed: " .. tostring(Image)) end

if type(Image.GetTextureId) ~= "function" then
    fail("Image.GetTextureId not registered (Phase AS.1)")
end
local img_id = Image.GetTextureId(self_stub)
if type(img_id) ~= "number" then fail("Image.GetTextureId should return number") end
pass("Light.Graphics.Image.GetTextureId registered ok (stub returns " .. img_id .. ")")

-- 现有 Image 方法仍存在
for _, k in ipairs({ "GetWidth", "GetHeight", "GetDepth", "GetDimensions" }) do
    if type(Image[k]) ~= "function" then fail("Image." .. k .. " regression!") end
end
pass("Light.Graphics.Image backward compat ok")

-- ==================== 4) Light.Graphics.Shader 6 新 setter ====================

local ok4, Shader = pcall(require, "Light.Graphics.Shader")
if not ok4 then fail("require(Light.Graphics.Shader) failed: " .. tostring(Shader)) end

-- Shader 模块层 fn (类常量)
if type(Shader.New) ~= "function" then fail("Shader.New regression!") end
if type(Shader.UseDefault) ~= "function" then fail("Shader.UseDefault regression!") end
if type(Shader.IsSupported) ~= "function" then fail("Shader.IsSupported regression!") end
pass("Light.Graphics.Shader module-level fns ok")

-- 测试 IsSupported, 在 headless / GL33 都应返回 boolean
local supported = Shader.IsSupported()
if type(supported) ~= "boolean" then fail("IsSupported should return boolean") end
pass("Shader.IsSupported() = " .. tostring(supported))

-- 在 headless 模式 (无 GL) 创建 shader 应优雅失败 (返回 nil + err)
-- 注意: 不能在没有 GL context 时实例化 shader, 我们只验证 New 接受 string 参数不崩
local vs = "void main() { gl_Position = vec4(0.0); }"
local fs = "void main() { gl_FragColor = vec4(1.0); }"
local sh, err = Shader.New(vs, fs)
-- sh 可能是 nil (headless) 或 userdata (有 GL); 都接受
if sh == nil then
    pass("Shader.New() returned nil in headless mode (err=" .. tostring(err) .. ")")
else
    pass("Shader.New() returned userdata (GL context active)")
    -- 如果成功创建, 验证 6 个新 setter 都是 method
    -- 用 Shader 元表查 method 注册
    local mt = getmetatable(sh)
    if type(mt) == "table" then
        local new_methods = {
            "SetMat3", "SetIVec2", "SetIVec3", "SetIVec4",
            "SetFloatArray", "SetVec2Array", "SetTexture",
        }
        for _, k in ipairs(new_methods) do
            if type(mt[k]) ~= "function" then
                fail("Shader metatable missing " .. k)
            end
        end
        pass("Shader metatable has 7 new setters (SetMat3/SetIVec*/SetFloatArray/SetVec2Array/SetTexture)")

        -- 调用每个 setter (有 GL context) 验证不崩
        sh:SetMat3("uMat3", { 1, 0, 0, 0, 1, 0, 0, 0, 1 })
        sh:SetIVec2("uIVec2", 1, 2)
        sh:SetIVec3("uIVec3", 1, 2, 3)
        sh:SetIVec4("uIVec4", 1, 2, 3, 4)
        sh:SetFloatArray("uArr", { 1.0, 2.0, 3.0, 4.0 })
        sh:SetVec2Array("uArr2", { 1.0, 2.0, 3.0, 4.0 })
        sh:SetTexture("uTex", 0, 1)  -- texId=0 是无效但应 silent 不崩
        pass("Shader new setters callable on instance (no crash)")

        -- 边界: SetVec2Array 奇数长度 (3) 应警告不崩
        sh:SetVec2Array("uOdd", { 1, 2, 3 })
        pass("SetVec2Array with odd length (3) is safe")

        -- 边界: SetFloatArray 空表
        sh:SetFloatArray("uEmpty", {})
        pass("SetFloatArray with empty table is safe")

        -- 边界: SetFloatArray 超过 256 截断
        local big = {}
        for i = 1, 300 do big[i] = i end
        sh:SetFloatArray("uBig", big)
        pass("SetFloatArray with 300 elements truncated (no crash)")
    end
end

-- ==================== 5) 兼容性回归 ====================

-- Light.UI 仍有 Phase AQ Pen/TextInput 常量
local ok5, UI = pcall(require, "Light.UI")
if not ok5 then fail("require(Light.UI) regression!") end
if type(UI.PEN_AXIS_PRESSURE) ~= "number" then fail("UI.PEN_AXIS_PRESSURE regression!") end
if type(UI.TEXTINPUT_TYPE_TEXT) ~= "number" then fail("UI.TEXTINPUT_TYPE_TEXT regression!") end
pass("Light.UI Phase AQ + AR constants still registered")

-- Light.Event 仍可加载
local ok6, Event = pcall(require, "Light.Event")
if not ok6 then fail("require(Light.Event) regression!") end
if type(Event.Push) ~= "function" then fail("Event.Push regression!") end
pass("Light.Event Phase AR module still works")

print("canvas_shader_ext smoke ok")
