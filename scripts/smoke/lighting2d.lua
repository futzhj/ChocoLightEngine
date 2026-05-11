-- Phase E.1.4 smoke: Light.Lighting2D (2D forward multi-light state)
--
-- API coverage:
--   SetEnabled / IsEnabled
--   SetAmbient / GetAmbient
--   AddPointLight / AddSpotLight (incl. 16-light cap)
--   UpdateLight (incl. invalid id / partial update)
--   RemoveLight / ClearLights (idempotency)
--   GetLightCount / GetMaxLights
--   Constants: MAX_LIGHTS / TYPE_POINT / TYPE_SPOT
--
-- Pure API test, no GPU dependency. CI verifies via both
-- 'lightc -p' syntax check and Windows 'light.exe' runtime smoke.
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Lighting2D")
if not ok then fail("require(Light.Lighting2D) failed: " .. tostring(mod)) end

-- ============================================================
-- 1) Module surface: 11 functions + 3 constants
-- ============================================================

local fn_names = {
    "SetEnabled", "IsEnabled",
    "SetAmbient", "GetAmbient",
    "AddPointLight", "AddSpotLight",
    "UpdateLight", "RemoveLight", "ClearLights",
    "GetLightCount", "GetMaxLights",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then
        fail("Light.Lighting2D." .. k .. " missing or not a function")
    end
end
pass("Light.Lighting2D module surface ok (" .. #fn_names .. " functions)")

assert(mod.MAX_LIGHTS == 16, "MAX_LIGHTS must be 16, got " .. tostring(mod.MAX_LIGHTS))
assert(mod.TYPE_POINT == 1, "TYPE_POINT must be 1, got " .. tostring(mod.TYPE_POINT))
assert(mod.TYPE_SPOT  == 2, "TYPE_SPOT must be 2, got " .. tostring(mod.TYPE_SPOT))
pass("Constants ok (MAX_LIGHTS=16, TYPE_POINT=1, TYPE_SPOT=2)")

-- ============================================================
-- 2) Enabled / Ambient
-- ============================================================

mod.SetEnabled(false)
assert(mod.IsEnabled() == false, "SetEnabled(false) did not stick")
mod.SetEnabled(true)
assert(mod.IsEnabled() == true,  "SetEnabled(true) did not stick")
pass("SetEnabled / IsEnabled ok")

mod.SetAmbient(0.1, 0.2, 0.3)
local ar, ag, ab = mod.GetAmbient()
local function approx(a, b) return math.abs(a - b) < 1e-5 end
assert(approx(ar, 0.1) and approx(ag, 0.2) and approx(ab, 0.3),
       "GetAmbient mismatch: " .. tostring(ar) .. "," .. tostring(ag) .. "," .. tostring(ab))
pass("SetAmbient / GetAmbient ok")

-- ============================================================
-- 3) Clear baseline: GetLightCount must be 0 after clear
-- ============================================================

mod.ClearLights()
assert(mod.GetLightCount() == 0, "ClearLights should zero count")
pass("ClearLights baseline ok")

-- ============================================================
-- 4) AddPointLight + GetLightCount basic
-- ============================================================

local id1 = mod.AddPointLight({ x = 100, y = 200, color = {r=1, g=0, b=0}, range = 150, intensity = 1.5 })
assert(type(id1) == "number" and id1 >= 1 and id1 <= 16,
       "AddPointLight returned invalid id: " .. tostring(id1))
assert(mod.GetLightCount() == 1, "GetLightCount should be 1")
pass("AddPointLight first id=" .. tostring(id1))

-- ============================================================
-- 5) AddSpotLight with full fields
-- ============================================================

local id2 = mod.AddSpotLight({
    x = 50, y = 50, dirX = 1, dirY = 0,
    color = {r=0, g=1, b=0}, range = 200,
    innerAngle = 15, outerAngle = 30, intensity = 2.0,
})
assert(type(id2) == "number", "AddSpotLight returned non-number: " .. tostring(id2))
assert(id2 ~= id1, "Add returned duplicate id")
assert(mod.GetLightCount() == 2, "GetLightCount should be 2")
pass("AddSpotLight id=" .. tostring(id2))

-- ============================================================
-- 6) UpdateLight partial: change only intensity, expect ok=true
-- ============================================================

local ok1 = mod.UpdateLight(id1, { intensity = 3.0 })
assert(ok1 == true, "UpdateLight valid id should return true, got " .. tostring(ok1))
pass("UpdateLight partial field ok")

-- Update with full table also returns true
local ok2 = mod.UpdateLight(id2, {
    x = 60, y = 70, dirX = 0, dirY = 1,
    color = {r=0.5, g=0.5, b=0.5}, range = 180,
    innerAngle = 10, outerAngle = 25, intensity = 1.0,
})
assert(ok2 == true, "UpdateLight full table failed")
pass("UpdateLight full table ok")

-- ============================================================
-- 7) UpdateLight invalid id -> false
-- ============================================================

assert(mod.UpdateLight(0,   {x=0}) == false, "UpdateLight(0) should return false")
assert(mod.UpdateLight(99,  {x=0}) == false, "UpdateLight(99) should return false")
assert(mod.UpdateLight(-1,  {x=0}) == false, "UpdateLight(-1) should return false")
pass("UpdateLight invalid id rejected (id=0/99/-1)")

-- ============================================================
-- 8) RemoveLight + idempotency
-- ============================================================

mod.RemoveLight(id1)
assert(mod.GetLightCount() == 1, "After Remove, count should be 1")
-- Idempotent: removing same id again is no-op
mod.RemoveLight(id1)
assert(mod.GetLightCount() == 1, "Double Remove should be no-op")
-- Removing invalid id is no-op
mod.RemoveLight(99)
mod.RemoveLight(0)
assert(mod.GetLightCount() == 1, "Remove invalid id should be no-op")
pass("RemoveLight + idempotency ok")

-- UpdateLight on removed slot -> false
assert(mod.UpdateLight(id1, {x=0}) == false,
       "UpdateLight on removed slot should return false")
pass("UpdateLight on removed slot rejected")

-- ============================================================
-- 9) 16-light cap: fill all slots, 17th returns nil
-- ============================================================

mod.ClearLights()
assert(mod.GetLightCount() == 0, "ClearLights reset failed")

local ids = {}
for i = 1, 16 do
    local id = mod.AddPointLight({ x = i * 10, y = i * 10 })
    assert(type(id) == "number" and id >= 1 and id <= 16,
           "AddPointLight #" .. i .. " failed: " .. tostring(id))
    ids[i] = id
end
assert(mod.GetLightCount() == 16, "After 16 adds, count should be 16")
pass("Filled all 16 slots")

-- 17th must fail with nil
local id17, err17 = mod.AddPointLight({ x = 0, y = 0 })
assert(id17 == nil, "17th AddPointLight should return nil, got " .. tostring(id17))
assert(type(err17) == "string", "17th AddPointLight should return error message")
pass("17th AddPointLight returns nil + err: " .. tostring(err17))

local id17s, err17s = mod.AddSpotLight({ x = 0, y = 0, dirX = 1, dirY = 0 })
assert(id17s == nil, "17th AddSpotLight should return nil")
pass("17th AddSpotLight returns nil")

-- ============================================================
-- 10) Remove + re-Add slot reuse
-- ============================================================

mod.RemoveLight(ids[5])
assert(mod.GetLightCount() == 15, "After Remove, count should be 15")
local newId = mod.AddPointLight({ x = 999, y = 999 })
assert(type(newId) == "number", "Add after Remove should succeed")
assert(mod.GetLightCount() == 16, "After re-Add, count should be 16 again")
pass("Slot reuse after Remove ok (new id=" .. tostring(newId) .. ")")

-- ============================================================
-- 11) ClearLights resets all slots, preserves ambient
-- ============================================================

mod.SetAmbient(0.4, 0.5, 0.6)
mod.ClearLights()
assert(mod.GetLightCount() == 0, "ClearLights should zero count")

local cr, cg, cb = mod.GetAmbient()
assert(approx(cr, 0.4) and approx(cg, 0.5) and approx(cb, 0.6),
       "ClearLights must NOT clear ambient, got " .. tostring(cr))
pass("ClearLights preserves ambient")

-- Can add again after Clear
local idAfter = mod.AddPointLight({ x = 1, y = 1 })
assert(type(idAfter) == "number", "Add after Clear should succeed")
pass("Add after Clear works (id=" .. tostring(idAfter) .. ")")

-- ============================================================
-- 12) Edge: AddPointLight with empty table uses defaults
-- ============================================================

mod.ClearLights()
local idDefault = mod.AddPointLight({})  -- all fields default
assert(type(idDefault) == "number", "AddPointLight({}) should succeed with defaults")
pass("AddPointLight with empty table uses defaults")

-- ============================================================
-- 13) Edge: AddSpotLight with zero direction -> normalized to (1,0)
-- ============================================================

local idZeroDir = mod.AddSpotLight({ x = 0, y = 0, dirX = 0, dirY = 0 })
assert(type(idZeroDir) == "number", "AddSpotLight zero-dir should still succeed (fallback)")
pass("AddSpotLight zero-direction handled (fallback to unit vector)")

-- ============================================================
-- 14) Phase E.1.5 — Light.Graphics.DrawLit / DrawLitQuad API surface
-- ============================================================
-- 仅检查函数存在; 真正的 GL 渲染验证留到 E.1.7 demo (需要 Window + GL context)

local ok_gfx, gfx = pcall(require, "Light.Graphics")
if ok_gfx and type(gfx) == "table" then
    assert(type(gfx.DrawLit)     == "function", "Light.Graphics.DrawLit must be a function")
    assert(type(gfx.DrawLitQuad) == "function", "Light.Graphics.DrawLitQuad must be a function")
    pass("Light.Graphics.DrawLit / DrawLitQuad function surface ok")

    -- 无 Window 时 g_render == nil 或 SupportsLit2D == false, 调用应直接返回 0 不崩
    local ok_call = pcall(gfx.DrawLit, nil, nil, 0, 0)
    assert(ok_call, "DrawLit(no-window) should not raise")
    local ok_call2 = pcall(gfx.DrawLitQuad, nil, nil, 0, 0, 0, 0, 0, 16, 16)
    assert(ok_call2, "DrawLitQuad(no-window) should not raise")
    pass("DrawLit / DrawLitQuad no-window guard ok (no crash)")
else
    print("SKIP: Light.Graphics not loadable (likely lightc -p syntax check)")
end

-- ============================================================
-- 15) Phase E.1.6 — ECS Light2D + LitSprite 集成
-- ============================================================
-- 通过 _UploadLights2D 内部方法直接测试 (不依赖完整 Render 流程):
--   - Light2D component 默认值 + 注册
--   - LitSprite component 默认值 + 注册
--   - _UploadLights2D 把 Light2D entity 转成 Lighting2D state
--   - parent chain world pos 累加
--   - enabled=false 跳过
--   - Point / Spot type 切换

local ok_ecs, ECS = pcall(require, "Light.ECS")
-- ECS.World 是 table 含 .new 字段 (调用方用 World.new() 而非 World:new())
if ok_ecs and type(ECS) == "table" and type(ECS.World) == "table" and type(ECS.World.new) == "function" then
    local World = ECS.World

    -- 15.1: Light2D / LitSprite component 自动注册
    local w = World.new()
    assert(w._components.Light2D,   "Light2D should be registered as builtin")
    assert(w._components.LitSprite, "LitSprite should be registered as builtin")
    assert(w._builtin_render_comps.Light2D, "Light2D builtin marker missing")
    assert(w._builtin_render_comps.LitSprite, "LitSprite builtin marker missing")
    assert(w._builtin_no_network and w._builtin_no_network.LitSprite,
           "LitSprite must be marked no_network (image/normalMap is userdata)")
    pass("ECS: Light2D + LitSprite builtin components registered")

    -- 15.2: 直接调 _UploadLights2D 测试 world->view 转换 + AddLight
    mod.ClearLights()
    local pt = w:CreateEntity()
    pt:Add("Transform2D", {x=100, y=200})
    pt:Add("Light2D",     {type=1, range=300, intensity=1.5,
                            color={r=1, g=0.5, b=0}})
    w:_UploadLights2D(nil)  -- 无 camera, 直接 world == view
    assert(mod.GetLightCount() == 1, "After 1 point Light2D, count=1, got " .. mod.GetLightCount())
    pass("ECS: _UploadLights2D adds 1 point light (count=1)")

    -- 15.3: Spot light type=2
    local sp = w:CreateEntity()
    sp:Add("Transform2D", {x=50, y=50})
    sp:Add("Light2D",     {type=2, dirX=0, dirY=1,
                            innerAngle=10, outerAngle=25,
                            color={r=0, g=1, b=0}, range=400})
    w:_UploadLights2D(nil)
    assert(mod.GetLightCount() == 2, "After Spot Light2D, count=2, got " .. mod.GetLightCount())
    pass("ECS: _UploadLights2D adds 1 spot light (count=2)")

    -- 15.4: enabled=false 跳过
    local off = w:CreateEntity()
    off:Add("Transform2D", {x=0, y=0})
    off:Add("Light2D",     {enabled=false, type=1})
    w:_UploadLights2D(nil)
    assert(mod.GetLightCount() == 2, "Light2D enabled=false should be skipped, got " .. mod.GetLightCount())
    pass("ECS: Light2D enabled=false skipped (count still 2)")

    -- 15.5: parent chain world pos
    local parent = w:CreateEntity():Add("Transform2D", {x=500, y=600})
    local child = w:CreateEntity()
    child:Add("Transform2D", {x=10, y=20, parent=parent})
    child:Add("Light2D",     {type=1, range=100})
    w:_UploadLights2D(nil)
    assert(mod.GetLightCount() == 3, "Child light should be added (count=3)")
    pass("ECS: child Light2D under parent transform added (count=3)")

    -- 15.6: camera view space 转换 (zoom + translate)
    mod.ClearLights()
    local w2 = World.new()
    local cam = w2:CreateEntity()
    cam:Add("Transform2D", {x=100, y=200})
    cam:Add("Camera2D",    {active=true, zoom=2.0})

    local pt2 = w2:CreateEntity()
    pt2:Add("Transform2D", {x=150, y=250})  -- world (150, 250), camera (100, 200), zoom=2
    pt2:Add("Light2D",     {type=1, range=100})
    -- 期望: view_x = (150-100)*2 = 100, view_y = (250-200)*2 = 100, range=100*2=200
    w2:_UploadLights2D(cam)
    assert(mod.GetLightCount() == 1, "Camera view-space upload count=1")
    pass("ECS: camera view-space transform applied (count=1)")

    -- 15.7: LitSprite default fields
    local ls = w2:CreateEntity()
    ls:Add("Transform2D", {x=300, y=300})
    ls:Add("LitSprite",   {image=nil})
    local comp = ls._comps.LitSprite
    assert(comp.normalMap == nil,  "LitSprite default normalMap == nil")
    assert(comp.visible == true,   "LitSprite default visible == true")
    assert(type(comp.color) == "table" and comp.color.r == 1, "LitSprite default color.r = 1")
    pass("ECS: LitSprite default fields correct")

    -- cleanup
    mod.ClearLights()
else
    print("SKIP: Light.ECS not available, ECS integration tests skipped")
end

-- ============================================================
-- 16) Phase E.2.1 — dirty bit version 单调递增
-- ============================================================
-- 间接验证 GL33Backend::UploadLighting2D 能否通过 state.version 相等跳过 uniform 上传:
--   - version 初始 > 0 (state 默认值为 1)
--   - 每个 mutator 调用后 version 严格递增 (Get* 不递增)
--   - 对幂等 mutator (Remove 已空 slot, Update 越界 id) 不递增 (version 仅在真正修改时 ++)

assert(type(mod.GetVersion) == "function", "GetVersion should exist after E.2.1")

-- 16.1: initial version > 0 (struct default = 1; §1-15 的 mutator 已把它推得更高)
local v0 = mod.GetVersion()
assert(type(v0) == "number" and v0 > 0, "initial version should be > 0, got " .. tostring(v0))
pass("E.2.1: GetVersion initial > 0 (v=" .. v0 .. ")")

-- 16.2: Get* 不递增
local vg1 = mod.GetVersion()
local _ = mod.GetLightCount()
local _ = mod.IsEnabled()
local _ = mod.GetAmbient()
local _ = mod.GetMaxLights()
local vg2 = mod.GetVersion()
assert(vg1 == vg2, "Get* must NOT bump version (v before=" .. vg1 .. ", after=" .. vg2 .. ")")
pass("E.2.1: Get* queries do not bump version")

-- 16.3: 每个 mutator 都递增
local function expectBump(desc, fn)
    local before = mod.GetVersion()
    fn()
    local after = mod.GetVersion()
    assert(after > before, desc .. " should bump version (before=" .. before .. " after=" .. after .. ")")
end

mod.ClearLights()                                          -- baseline
expectBump("SetEnabled",  function() mod.SetEnabled(false) end)
expectBump("SetEnabled2", function() mod.SetEnabled(true)  end)  -- 即使值相同也递增 (简化语义)
expectBump("SetAmbient",  function() mod.SetAmbient(0.2, 0.3, 0.4) end)

local idP
expectBump("AddPointLight", function()
    idP = mod.AddPointLight({x=100, y=100, range=200})
    assert(idP, "AddPointLight should succeed")
end)
expectBump("UpdateLight",   function() mod.UpdateLight(idP, {intensity=2.0}) end)
expectBump("RemoveLight",   function() mod.RemoveLight(idP) end)

local idS
expectBump("AddSpotLight",  function()
    idS = mod.AddSpotLight({x=0, y=0, dirX=1, dirY=0, range=100})
    assert(idS, "AddSpotLight should succeed")
end)

expectBump("ClearLights",   function() mod.ClearLights() end)
pass("E.2.1: all 6 mutators (SetEnabled/SetAmbient/Add/Update/Remove/Clear) bump version")

-- 16.4: 幂等 mutator 不递增 (Remove 已空 slot, Update 越界 id)
local vIdem1 = mod.GetVersion()
mod.RemoveLight(99)          -- 越界, no-op
mod.RemoveLight(-1)          -- 越界
mod.RemoveLight(0)           -- 保留, no-op
local ok_upd = mod.UpdateLight(99, {intensity=1.0})   -- 越界 id
assert(ok_upd == false, "UpdateLight with invalid id should return false")
local vIdem2 = mod.GetVersion()
assert(vIdem1 == vIdem2, "idempotent no-op mutators must NOT bump version (before=" ..
       vIdem1 .. " after=" .. vIdem2 .. ")")
pass("E.2.1: idempotent mutators (no-op Remove/Update) do not bump version")

-- 16.5: 多次 Add 到 16 slot 满后, 第 17 次 Add 失败, version 不递增
mod.ClearLights()
for i = 1, 16 do mod.AddPointLight({x=i, y=i}) end
local vFull1 = mod.GetVersion()
local ret = mod.AddPointLight({x=0, y=0})
assert(ret == nil, "17th AddPointLight should fail")
local vFull2 = mod.GetVersion()
assert(vFull1 == vFull2, "failed Add (16 full) must NOT bump version")
pass("E.2.1: failed Add (16 slot full) does not bump version")

mod.ClearLights()

-- ============================================================
-- 17) Phase E.2.2 — ECS _UploadLights2D 加 bounds 参数 (AABB-Circle cull)
-- ============================================================
-- 验证:
--   - bounds == nil 向后兼容: 所有 light 上传 (culled=0)
--   - 中心在 bounds 外, 影响圆也不触及 bounds → culled
--   - 中心在 bounds 外但 range 足够穿入 bounds → uploaded (不过度 cull)
--   - 中心在 bounds 内 → uploaded

if ok_ecs and type(ECS) == "table" and type(ECS.World) == "table" and type(ECS.World.new) == "function" then
    local World = ECS.World
    local w = World.new()

    -- bounds = viewport (-100, -100) ~ (100, 100) (world space, 200x200 窗口)
    local bounds = {minX = -100, maxX = 100, minY = -100, maxY = 100}

    -- L1: 视口内 (0, 0), range=50 → uploaded
    local e1 = w:CreateEntity()
    e1:Add("Transform2D", {x=0, y=0})
    e1:Add("Light2D",     {type=1, range=50})

    -- L2: 远处 (1000, 0), range=50 → culled (视口右 100, light 中心在 1000, 影响圆 950~1050 与 bounds 不交)
    local e2 = w:CreateEntity()
    e2:Add("Transform2D", {x=1000, y=0})
    e2:Add("Light2D",     {type=1, range=50})

    -- L3: 远处 (1000, 0), range=1500 → uploaded (影响圆覆盖 bounds)
    local e3 = w:CreateEntity()
    e3:Add("Transform2D", {x=1000, y=0})
    e3:Add("Light2D",     {type=1, range=1500})

    -- L4: 视口正下方远处 (0, 2000), range=100 → culled (y=2000-100=1900 > maxY=100)
    local e4 = w:CreateEntity()
    e4:Add("Transform2D", {x=0, y=2000})
    e4:Add("Light2D",     {type=1, range=100})

    -- 17.1: 传入 bounds, 应 uploaded=2 (L1+L3), culled=2 (L2+L4)
    mod.ClearLights()
    w:_UploadLights2D(nil, bounds)
    local stats = w._light2d_stats
    assert(stats and stats.uploaded == 2 and stats.culled == 2,
           "bounds cull: expected uploaded=2, culled=2, got uploaded=" ..
           tostring(stats and stats.uploaded) .. ", culled=" ..
           tostring(stats and stats.culled))
    assert(mod.GetLightCount() == 2, "light count should match uploaded=2")
    pass("E.2.2: AABB-Circle cull (uploaded=2 / culled=2)")

    -- 17.2: bounds == nil 向后兼容: 全部上传, culled=0
    mod.ClearLights()
    w:_UploadLights2D(nil, nil)
    stats = w._light2d_stats
    assert(stats and stats.uploaded == 4 and stats.culled == 0,
           "bounds=nil: all 4 uploaded, culled=0, got uploaded=" ..
           tostring(stats and stats.uploaded) .. ", culled=" ..
           tostring(stats and stats.culled))
    assert(mod.GetLightCount() == 4, "bounds=nil: light count = 4")
    pass("E.2.2: bounds == nil backward compat (all 4 uploaded)")

    -- 17.3: 收紧 bounds 到 L1 也被 cull (L1 在 (0,0), range=50; bounds 移到 200 外)
    local bounds_far = {minX=200, maxX=400, minY=200, maxY=400}
    mod.ClearLights()
    w:_UploadLights2D(nil, bounds_far)
    stats = w._light2d_stats
    -- 只 L3 (range=1500 从 (1000, 0), 影响范围 x=-500~2500, y=-1500~1500) 覆盖 bounds_far
    assert(stats.uploaded == 1 and stats.culled == 3,
           "far bounds: only L3 uploaded (range=1500 covers bounds_far), got uploaded=" ..
           stats.uploaded .. ", culled=" .. stats.culled)
    pass("E.2.2: tighter bounds culls more lights (uploaded=1)")

    mod.ClearLights()
else
    print("SKIP: E.2.2 cull smoke (Light.ECS unavailable)")
end

-- ============================================================
-- 18) Phase E.2.3 — LitBatchRenderer API surface + 无窗口 guard
-- ============================================================
-- 验证:
--   - Light.Graphics.FlushLitBatch 存在且可调用 (无窗口环境也安全)
--   - DrawLit/DrawLitQuad 仍可调用 (内部 SupportsLit2D=false 时静默 return 0)
--   - 真正的 batch 行为 (drawCalls / 合批数) 需视觉验收, 留给 demo_2d_lighting

local ok_gfx, gfx = pcall(require, "Light.Graphics")
if ok_gfx and type(gfx) == "table" then
    assert(type(gfx.FlushLitBatch) == "function", "Light.Graphics.FlushLitBatch should exist (E.2.3)")
    pass("E.2.3: Light.Graphics.FlushLitBatch API exists")

    local ok_flush = pcall(gfx.FlushLitBatch)
    assert(ok_flush, "FlushLitBatch should be callable in headless (no-op when LitBatchRenderer not init)")
    pass("E.2.3: FlushLitBatch headless no-window guard ok (no crash)")

    -- DrawLit / DrawLitQuad 在 headless 已在 §14 验证, 这里仅再确认改造后未崩
    if type(gfx.DrawLit) == "function" then
        local ok_dl = pcall(gfx.DrawLit, nil, nil, 0, 0, 0, 0, 0, 45, 2, 2, 1, 32, 32, 0)
        assert(ok_dl, "DrawLit with full 14 params (transform) should not crash")
        pass("E.2.3: DrawLit with rot/scale/origin params ok (no crash)")
    end

    if type(gfx.DrawLitQuad) == "function" then
        local ok_dlq = pcall(gfx.DrawLitQuad, nil, nil, 0, 0, 0, 0, 0, 32, 32,
                              0, 0, 30, 1.5, 1.5, 1, 16, 16, 0)
        assert(ok_dlq, "DrawLitQuad with full 18 params should not crash")
        pass("E.2.3: DrawLitQuad with quad+transform params ok (no crash)")
    end
else
    print("SKIP: E.2.3 surface smoke (Light.Graphics unavailable)")
end

-- ============================================================
-- Final cleanup
-- ============================================================

mod.ClearLights()
mod.SetAmbient(0, 0, 0)
mod.SetEnabled(true)
assert(mod.GetLightCount() == 0, "Final clear failed")

print("==== Light.Lighting2D smoke DONE ====")
