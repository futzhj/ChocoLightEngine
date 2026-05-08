-- Phase F smoke: Light.Clipboard / Light.URL / Light.Display / Light.Cursor
-- 仅做 API 注册和无副作用的边界路径验证, 适配 CI headless 环境.
-- 注意: 真正调用 SetClipboardText / OpenURL / WarpGlobal 会有副作用, 故 CI 仅注册校验.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

-- ==================== Light.Clipboard ====================
do
    local ok, mod = pcall(require, "Light.Clipboard")
    if not ok then fail("require(Light.Clipboard) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.Clipboard not a table") end
    for _, k in ipairs({ "SetText", "GetText", "HasText" }) do
        if type(mod[k]) ~= "function" then
            fail("Light.Clipboard." .. k .. " missing or not a function")
        end
    end
    pass("Light.Clipboard module ok")

    -- HasText 不会修改剪贴板, 安全调用
    local has = mod.HasText()
    if type(has) ~= "boolean" then fail("HasText must return boolean") end
    pass("Light.Clipboard.HasText returned " .. tostring(has))

    -- GetText 也不修改剪贴板
    local txt, err = mod.GetText()
    if txt == nil and err == nil then fail("GetText nil + nil err inconsistent") end
    pass("Light.Clipboard.GetText length=" .. tostring(txt and #txt or 0))
end

-- ==================== Light.URL ====================
do
    local ok, mod = pcall(require, "Light.URL")
    if not ok then fail("require(Light.URL) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.URL not a table") end
    if type(mod.Open) ~= "function" then fail("Light.URL.Open missing") end
    pass("Light.URL module ok")
    -- 不在 CI 实际打开 URL (会触发浏览器), 仅验证签名
end

-- ==================== Light.Display ====================
do
    local ok, mod = pcall(require, "Light.Display")
    if not ok then fail("require(Light.Display) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.Display not a table") end
    for _, k in ipairs({
        "GetAll", "GetPrimary", "GetForPoint", "GetName",
        "GetBounds", "GetUsableBounds", "GetContentScale", "GetCurrentMode",
    }) do
        if type(mod[k]) ~= "function" then
            fail("Light.Display." .. k .. " missing")
        end
    end
    pass("Light.Display module ok")

    -- GetAll: 即使 headless CI 视频子系统未初始化, 也应返回 table 或 nil+err
    local list, err = mod.GetAll()
    if list ~= nil and type(list) ~= "table" then
        fail("GetAll must return table or nil+err")
    end
    pass("Light.Display.GetAll list=" .. tostring(list and #list or "nil")
         .. " err=" .. tostring(err))

    -- GetPrimary: 同上
    local pid, perr = mod.GetPrimary()
    if pid ~= nil and type(pid) ~= "number" then
        fail("GetPrimary must return integer or nil+err")
    end
    pass("Light.Display.GetPrimary id=" .. tostring(pid) .. " err=" .. tostring(perr))

    -- 非法 id 路径
    local r, e = mod.GetBounds(0)
    if r ~= nil and type(r) ~= "table" then fail("GetBounds(0) bad type") end
    pass("Light.Display.GetBounds(0) returned " .. tostring(r) .. " err=" .. tostring(e))
end

-- ==================== Light.Cursor ====================
do
    local ok, mod = pcall(require, "Light.Cursor")
    if not ok then fail("require(Light.Cursor) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.Cursor not a table") end
    for _, k in ipairs({
        "CreateSystem", "Set", "Destroy", "Show", "Hide",
        "IsVisible", "WarpGlobal",
    }) do
        if type(mod[k]) ~= "function" then
            fail("Light.Cursor." .. k .. " missing")
        end
    end
    pass("Light.Cursor module ok")

    -- 边界: 未知名字 → nil + err (此路径不依赖视频子系统)
    local h, err = mod.CreateSystem("notarealcursor")
    if h ~= nil or err == nil then
        fail("CreateSystem(invalid) should be nil+err")
    end
    pass("Light.Cursor.CreateSystem(invalid) boundary ok: " .. tostring(err))

    -- IsVisible 不依赖 cursor 创建, 应返回 boolean
    local v = mod.IsVisible()
    if type(v) ~= "boolean" then fail("IsVisible must return boolean") end
    pass("Light.Cursor.IsVisible = " .. tostring(v))

    -- Destroy(nil) 边界
    local ok2, e2 = mod.Destroy(nil)
    if ok2 ~= false or e2 == nil then fail("Destroy(nil) should be false+err") end
    pass("Light.Cursor.Destroy(nil) boundary ok: " .. tostring(e2))
end

print("clipboard_url_display_cursor smoke ok")
