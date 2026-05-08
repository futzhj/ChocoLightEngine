-- Phase I smoke: Light.Tray
--
-- 兼容性: Web/Android/iOS/无桌面环境的 Linux 上 SDL_CreateTray 会返回 NULL.
-- smoke 必须容忍 Create 返回 nil + err 路径, 但仍验证所有 17 fn 已注册.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

local ok, mod = pcall(require, "Light.Tray")
if not ok then fail("require(Light.Tray) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Tray not a table") end

for _, k in ipairs({
    "Create", "Destroy", "SetTooltip",
    "GetMenu", "AddButton", "AddCheckbox", "AddSeparator", "AddSubmenu",
    "SetEntryLabel", "GetEntryLabel",
    "SetEntryEnabled", "GetEntryEnabled",
    "SetEntryChecked", "GetEntryChecked",
    "RemoveEntry", "WasClicked", "Update",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Tray." .. k .. " missing") end
end
pass("Light.Tray module ok (17 functions)")

-- ===== 边界路径: 无 handle 的调用应安全返回 nil/false + err =====
local d, derr = mod.Destroy(nil)
if d ~= false or derr == nil then fail("Destroy(nil) should be false+err") end
pass("Light.Tray.Destroy(nil) boundary ok: " .. tostring(derr))

local t, terr = mod.SetTooltip(nil, "x")
if t ~= false or terr == nil then fail("SetTooltip(nil) should be false+err") end
pass("Light.Tray.SetTooltip(nil) boundary ok: " .. tostring(terr))

local m, merr = mod.GetMenu(nil)
if m ~= nil or merr == nil then fail("GetMenu(nil) should be nil+err") end
pass("Light.Tray.GetMenu(nil) boundary ok: " .. tostring(merr))

local b, berr = mod.AddButton(nil, "x")
if b ~= nil or berr == nil then fail("AddButton(nil) should be nil+err") end
pass("Light.Tray.AddButton(nil) boundary ok: " .. tostring(berr))

local label, lerr = mod.GetEntryLabel(nil)
if label ~= nil or lerr == nil then fail("GetEntryLabel(nil) should be nil+err") end
pass("Light.Tray.GetEntryLabel(nil) boundary ok: " .. tostring(lerr))

if mod.GetEntryEnabled(nil) ~= false then fail("GetEntryEnabled(nil) should be false") end
if mod.GetEntryChecked(nil) ~= false then fail("GetEntryChecked(nil) should be false") end
pass("Light.Tray.GetEntry{Enabled,Checked}(nil) boundary ok")

if mod.WasClicked(nil) ~= 0 then fail("WasClicked(nil) should be 0") end
pass("Light.Tray.WasClicked(nil) = 0 ok")

-- Update 不依赖 tray, 任意环境可调
mod.Update()
pass("Light.Tray.Update() ok")

-- ===== 真实 Create 试探 (CI/headless 环境通常会失败, 但失败也应是 nil + err) =====
local tray, err = mod.Create("ChocoLight CI smoke")
if tray == nil then
    -- 平台不支持或无 desktop env: 完全合规
    pass("Light.Tray.Create returned nil (no platform support) err=" .. tostring(err))
else
    pass("Light.Tray.Create succeeded (handle alive)")

    -- 设置 tooltip (容错: 即使 SetTooltip 在 dummy 后端是 noop, 也应返回 ok)
    local ok2, terr2 = mod.SetTooltip(tray, "Updated tooltip")
    if ok2 ~= true then fail("SetTooltip on valid tray failed: " .. tostring(terr2)) end
    pass("Light.Tray.SetTooltip(tray) ok")

    -- 创建 menu + 几个 entry
    local menu = mod.GetMenu(tray)
    if menu == nil then
        pass("Light.Tray.GetMenu returned nil on this backend (acceptable)")
    else
        pass("Light.Tray.GetMenu ok (menu handle alive)")

        local btn = mod.AddButton(menu, "Quit")
        if btn ~= nil then
            pass("Light.Tray.AddButton ok")
            -- SDL3 v3.2.30 Windows 后端存在上游 bug: SDL_GetTrayEntryEnabled 使用
            -- `fState & MFS_ENABLED`, 但 MFS_ENABLED == 0, 表达式恒为 false.
            -- 因此 smoke 只验证调用不崩溃 + 返回 boolean, 不对具体值断言.
            local en0 = mod.GetEntryEnabled(btn)
            if type(en0) ~= "boolean" then
                fail("GetEntryEnabled must return boolean, got " .. type(en0))
            end
            pass("Light.Tray.GetEntryEnabled returns boolean (value=" .. tostring(en0) .. ", SDL3 Win upstream bug tolerated)")
            -- 改 label
            local _, le = mod.SetEntryLabel(btn, "Quit (renamed)")
            if le then fail("SetEntryLabel failed: " .. tostring(le)) end
            local lbl = mod.GetEntryLabel(btn)
            if lbl ~= "Quit (renamed)" then
                fail("label roundtrip failed, got: " .. tostring(lbl))
            end
            pass("Light.Tray.{Set,Get}EntryLabel roundtrip = " .. lbl)
            -- Set 调用本身应不崩 (即使 Get 语义有上游 bug)
            local sok, serr = mod.SetEntryEnabled(btn, false)
            if sok ~= true then fail("SetEntryEnabled(false) failed: " .. tostring(serr)) end
            pass("Light.Tray.SetEntryEnabled(false) call ok")
        end

        local cb = mod.AddCheckbox(menu, "Auto save", true)
        if cb ~= nil then
            if mod.GetEntryChecked(cb) ~= true then fail("checkbox should be checked initially") end
            mod.SetEntryChecked(cb, false)
            if mod.GetEntryChecked(cb) ~= false then fail("uncheck failed") end
            pass("Light.Tray.{Add,Set,Get}Checkbox ok")
        end

        local sep = mod.AddSeparator(menu)
        if sep ~= nil then pass("Light.Tray.AddSeparator ok") end

        local subEntry, subMenu = mod.AddSubmenu(menu, "More")
        if subEntry ~= nil and subMenu ~= nil then
            pass("Light.Tray.AddSubmenu ok (entry + submenu handles)")
            mod.AddButton(subMenu, "Inner item")
        end
    end

    -- 清理
    local dok, derr2 = mod.Destroy(tray)
    if dok ~= true then fail("Destroy(valid) failed: " .. tostring(derr2)) end
    pass("Light.Tray.Destroy ok")
end

print("tray smoke ok")
