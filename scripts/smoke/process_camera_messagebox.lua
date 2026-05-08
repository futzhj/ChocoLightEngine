-- Phase E smoke: Light.Process / Light.Camera / Light.MessageBox
-- 仅做 API 注册和无副作用的边界路径验证, CI 桌面/无相机/无 GUI 环境也能跑通.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

-- ==================== Light.Process ====================
do
    local ok, mod = pcall(require, "Light.Process")
    if not ok then fail("require(Light.Process) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.Process is not a table") end
    for _, k in ipairs({ "Run", "Read", "Wait", "Kill", "Destroy" }) do
        if type(mod[k]) ~= "function" then
            fail("Light.Process." .. k .. " missing or not a function")
        end
    end
    pass("Light.Process module ok")

    -- 边界: 非 table args -> nil + err
    local h, err = mod.Run(nil, false)
    if h ~= nil or err == nil then fail("Run(nil) should return nil + err") end
    pass("Light.Process.Run(nil) boundary ok: " .. tostring(err))

    -- 边界: 空 table args
    h, err = mod.Run({}, false)
    if h ~= nil or err == nil then fail("Run({}) should return nil + err") end
    pass("Light.Process.Run({}) boundary ok: " .. tostring(err))

    -- 边界: 非字符串元素
    h, err = mod.Run({ 123 }, false)
    if h ~= nil or err == nil then fail("Run({123}) should return nil + err") end
    pass("Light.Process.Run({123}) boundary ok: " .. tostring(err))

    -- 边界: 非法 handle 调用 Read/Wait/Kill/Destroy
    local s, ec, e = mod.Read(nil)
    if s ~= nil or e == nil then fail("Read(nil) should return nil,nil,err") end
    pass("Light.Process.Read(nil) boundary ok: " .. tostring(e))
end

-- ==================== Light.Camera ====================
do
    local ok, mod = pcall(require, "Light.Camera")
    if not ok then fail("require(Light.Camera) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.Camera is not a table") end
    for _, k in ipairs({
        "GetCameras", "GetName", "Open", "GetPermissionState",
        "AcquireFrame", "Close",
    }) do
        if type(mod[k]) ~= "function" then
            fail("Light.Camera." .. k .. " missing or not a function")
        end
    end
    pass("Light.Camera module ok")

    -- GetCameras: 桌面 CI 通常无相机, 但函数应返回 table (可能为空) 而非 nil
    local list, err = mod.GetCameras()
    if list == nil then
        -- 部分平台无相机后端时也可能返回 nil + err, 接受
        pass("Light.Camera.GetCameras returned nil (no backend): " .. tostring(err))
    else
        if type(list) ~= "table" then fail("GetCameras must return table") end
        pass("Light.Camera.GetCameras count=" .. tostring(#list))
    end

    -- 非法 id -> GetName 应返回 nil + err
    local name, e = mod.GetName(0)
    if name ~= nil and type(name) ~= "string" then
        fail("GetName(0) returned unexpected type: " .. type(name))
    end
    pass("Light.Camera.GetName(0) returned " .. tostring(name) .. " err=" .. tostring(e))

    -- 非法 handle -> AcquireFrame/Close 边界
    local frame, ferr = mod.AcquireFrame(nil)
    if frame ~= nil or ferr == nil then fail("AcquireFrame(nil) should be nil+err") end
    pass("Light.Camera.AcquireFrame(nil) boundary ok: " .. tostring(ferr))
end

-- ==================== Light.MessageBox ====================
do
    local ok, mod = pcall(require, "Light.MessageBox")
    if not ok then fail("require(Light.MessageBox) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.MessageBox is not a table") end
    for _, k in ipairs({ "ShowSimple", "Show" }) do
        if type(mod[k]) ~= "function" then
            fail("Light.MessageBox." .. k .. " missing or not a function")
        end
    end
    pass("Light.MessageBox module ok")

    -- 边界: Show 缺 title -> nil + err
    local r, err = mod.Show({ message = "x", buttons = { "OK" } })
    if r ~= nil or err == nil then fail("Show without title should be nil+err") end
    pass("Light.MessageBox.Show(no title) boundary ok: " .. tostring(err))

    -- 边界: Show 缺 message
    r, err = mod.Show({ title = "T", buttons = { "OK" } })
    if r ~= nil or err == nil then fail("Show without message should be nil+err") end
    pass("Light.MessageBox.Show(no message) boundary ok: " .. tostring(err))

    -- 边界: Show 缺 buttons
    r, err = mod.Show({ title = "T", message = "M" })
    if r ~= nil or err == nil then fail("Show without buttons should be nil+err") end
    pass("Light.MessageBox.Show(no buttons) boundary ok: " .. tostring(err))

    -- 边界: 空 buttons
    r, err = mod.Show({ title = "T", message = "M", buttons = {} })
    if r ~= nil or err == nil then fail("Show with empty buttons should be nil+err") end
    pass("Light.MessageBox.Show(empty buttons) boundary ok: " .. tostring(err))

    -- 边界: 非字符串按钮元素
    r, err = mod.Show({ title = "T", message = "M", buttons = { 1, 2 } })
    if r ~= nil or err == nil then fail("Show with non-string buttons should be nil+err") end
    pass("Light.MessageBox.Show(non-string buttons) boundary ok: " .. tostring(err))

    -- ShowSimple/Show 真正弹窗会阻塞 GUI 子系统, CI headless 不调用; 仅验注册.
end

print("process_camera_messagebox smoke ok")
