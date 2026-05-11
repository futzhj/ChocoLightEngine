-- Phase D smoke: Light.ECS render integration (D-T1 ~ D-T7)
-- ASCII-only.
--
-- 单进程验证内置渲染 component 注册 + world:Render 调度 + Sprite/Mesh 绘制.
-- 用 mock Light.Graphics 替代真实图形模块, 验证调用顺序和参数.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end
local function eq(a, b, msg) if a ~= b then fail(msg .. " expected="..tostring(b).." got="..tostring(a)) end end

-- 1) require Light.ECS
local ok, ECS = pcall(require, "Light.ECS")
if not ok then fail("require(Light.ECS): " .. tostring(ECS)) end
pass("require(Light.ECS) ok")
local World = ECS.World

-- ============================================================
-- D-T1: 6 内置 component 自动注册
-- ============================================================
do
    local w = World.new()
    local need = {"Transform2D", "Sprite", "Camera2D",
                  "Transform3D", "MeshRenderer", "Camera3D"}
    for _, name in ipairs(need) do
        if not w._components[name] then
            fail("D-T1: builtin "..name.." not registered")
        end
        if not w._builtin_render_comps[name] then
            fail("D-T1: builtin marker missing for "..name)
        end
    end
    pass("D-T1: all 6 builtin render components registered")

    -- 默认 networked=false
    for _, name in ipairs(need) do
        if w._networked_comps[name] then
            fail("D-T1: "..name.." should default to networked=false")
        end
    end
    pass("D-T1: builtin defaults networked=false")

    -- 用户已注册同名则跳过 (用户优先)
    local w2 = World.new()
    -- 重新 new 一个 world (注意: 内置已注册过, 但用户可覆盖)
    -- 验证: 若用户在 new 前替换 _RegisterBuiltinRenderComponents? 跳过该 case
    -- 这里只验证一次 new 不重复注册
    eq(type(w2._components.Sprite), "table", "D-T1: w2 has Sprite")
    pass("D-T1: repeated new() does not error")
end

-- ============================================================
-- D-T2: world:Render 在 Light.Graphics 不可用时静默 no-op
-- ============================================================
do
    local w = World.new()
    local e = w:CreateEntity()
    e:Add("Transform2D", {x=10, y=20})
    e:Add("Sprite", {image={GetWidth=function() return 32 end, GetHeight=function() return 32 end}})

    -- 备份并清掉 Light.Graphics
    local origLight = Light
    Light = {}  -- 没 Graphics 子字段
    local okR = pcall(function() w:Render() end)
    Light = origLight
    if not okR then fail("D-T2: Render() should be no-op when Light.Graphics missing") end
    pass("D-T2: Render no-op when Light.Graphics missing")
end

-- ============================================================
-- 准备 mock Light.Graphics, 后续 D-T3~D-T6 共用
-- ============================================================
local function makeMockGraphics()
    local calls = {}
    local gfx = {}
    local fnNames = {"Push", "Pop", "Translate", "Rotate", "Scale", "SetColor",
                     "Draw", "DrawQuad", "SetPerspective", "SetCamera", "SetDepthTest"}
    for _, fn in ipairs(fnNames) do
        gfx[fn] = function(...)
            calls[#calls + 1] = {fn=fn, args={...}}
        end
    end
    return gfx, calls
end

local function installMockLight(gfx)
    if type(Light) ~= "table" then Light = {} end
    Light.Graphics = gfx
end

-- 简易 mock image (table with GetWidth/GetHeight)
local function makeMockImage(w, h)
    return {GetWidth = function(self) return w end,
            GetHeight = function(self) return h end}
end

-- 计数特定函数被调用次数
local function countFn(calls, fnName)
    local n = 0
    for _, c in ipairs(calls) do if c.fn == fnName then n = n + 1 end end
    return n
end

-- 取第 N 次特定函数调用 (1-based). 找不到直接 fail (调用方无需判空)
local function nthCall(calls, fnName, n)
    local i = 0
    for _, c in ipairs(calls) do
        if c.fn == fnName then
            i = i + 1
            if i == n then return c end
        end
    end
    fail("missing call #"..n.." of "..fnName)
    return {args={}}  -- never reach, 满足静态分析器
end

-- ============================================================
-- D-T3: Sprite Draw + visible=false 不画 + z 升序
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(40, 60)

    -- 3 个 sprite, z = 5, 1, 3 (期望绘制顺序 1, 3, 5)
    local e1 = w:CreateEntity():Add("Transform2D", {x=100, y=200, z=5})
                                :Add("Sprite",      {image=img})
    local e2 = w:CreateEntity():Add("Transform2D", {x=10, y=20, z=1})
                                :Add("Sprite",      {image=img})
    local e3 = w:CreateEntity():Add("Transform2D", {x=50, y=60, z=3})
                                :Add("Sprite",      {image=img})

    -- 一个不可见 sprite (不应被画)
    local eHide = w:CreateEntity():Add("Transform2D", {x=0,y=0,z=99})
                                   :Add("Sprite",      {image=img, visible=false})

    w:Render()

    -- Draw 调用 3 次 (3 个 visible sprite)
    eq(countFn(calls, "Draw"), 3, "D-T3: should Draw 3 visible sprites")
    pass("D-T3: Draw called for each visible sprite")

    -- Translate 调用第 1 次 (z=1) 参数应为 e2 的 x,y=(10, 20)
    -- (每个 sprite 内 Push -> Translate -> [Rotate?] -> [Scale?] -> SetColor -> Draw -> Pop)
    -- 但 Translate 还可能有 anchor 引起? 不, anchor 在 drawX/drawY 中. 单次 Translate(tf.x, tf.y, 0)
    local t1 = nthCall(calls, "Translate", 1)
    if not t1 then fail("D-T3: missing first Translate") end
    eq(t1.args[1], 10, "D-T3: first sprite by z should be e2(x=10)")
    eq(t1.args[2], 20, "D-T3: first sprite y=20")

    local t2 = nthCall(calls, "Translate", 2)
    eq(t2.args[1], 50, "D-T3: second sprite by z=3 should be e3(x=50)")

    local t3 = nthCall(calls, "Translate", 3)
    eq(t3.args[1], 100, "D-T3: third sprite by z=5 should be e1(x=100)")
    pass("D-T3: sprites drawn in ascending z order")
end

-- ============================================================
-- D-T3+: anchor 偏移正确 (anchor=(0.5,0.5) 时 drawX=-iw/2)
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(80, 40)
    w:CreateEntity():Add("Transform2D", {x=200, y=300})
                    :Add("Sprite",      {image=img, anchor={ax=0.5, ay=0.5}})

    w:Render()

    -- Draw 参数: img, drawX, drawY, 0
    local drawCall = nthCall(calls, "Draw", 1)
    if not drawCall then fail("D-T3+: missing Draw") end
    eq(drawCall.args[2], -40, "D-T3+: anchor(0.5,_) drawX = -0.5*80 = -40")
    eq(drawCall.args[3], -20, "D-T3+: anchor(_,0.5) drawY = -0.5*40 = -20")
    pass("D-T3+: anchor offset applied correctly")
end

-- ============================================================
-- D-T3+: flipX 通过负 scale 实现
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(64, 64)
    w:CreateEntity():Add("Transform2D", {x=0,y=0,sx=2,sy=2})
                    :Add("Sprite",      {image=img, flipX=true})

    w:Render()

    local scaleCall = nthCall(calls, "Scale", 1)
    if not scaleCall then fail("D-T3+: missing Scale") end
    eq(scaleCall.args[1], -2, "D-T3+: flipX makes sx negative (-2)")
    eq(scaleCall.args[2], 2, "D-T3+: sy stays positive")
    pass("D-T3+: flipX applied via negative scale")
end

-- ============================================================
-- D-T3+: quad.qw>0 走 DrawQuad
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(128, 128)
    w:CreateEntity():Add("Transform2D", {x=0,y=0})
                    :Add("Sprite",      {image=img, quad={qx=32, qy=0, qw=32, qh=64}})

    w:Render()

    eq(countFn(calls, "Draw"), 0, "D-T3+: quad path should not call Draw")
    eq(countFn(calls, "DrawQuad"), 1, "D-T3+: should call DrawQuad once")
    local dq = nthCall(calls, "DrawQuad", 1)
    eq(dq.args[5], 32, "D-T3+: DrawQuad qx=32")
    eq(dq.args[7], 32, "D-T3+: DrawQuad qw=32")
    pass("D-T3+: quad triggers DrawQuad with correct subregion")
end

-- ============================================================
-- D-T4: Camera2D active 时应用视图变换 (Push -> Scale -> Translate(-cam))
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(32, 32)

    -- Camera2D entity: 位置(100,200), zoom=2
    w:CreateEntity():Add("Transform2D", {x=100, y=200})
                    :Add("Camera2D",    {active=true, zoom=2.0})

    -- 1 个 sprite 在世界 (300, 400)
    w:CreateEntity():Add("Transform2D", {x=300, y=400})
                    :Add("Sprite",      {image=img})

    w:Render()

    -- Push 应该有 2 次 (相机一次 + sprite 一次)
    eq(countFn(calls, "Push"), 2, "D-T4: Push called twice (camera+sprite)")
    eq(countFn(calls, "Pop"), 2, "D-T4: Pop called twice")

    -- 第 1 次 Translate 应是相机 Translate(-100, -200, 0)
    local t1 = nthCall(calls, "Translate", 1)
    eq(t1.args[1], -100, "D-T4: camera Translate dx = -camX")
    eq(t1.args[2], -200, "D-T4: camera Translate dy = -camY")
    pass("D-T4: Camera2D applies view transform correctly")
end

-- ============================================================
-- D-T4: 无 active camera 时跳过 setup (但仍画 sprite)
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(32, 32)
    w:CreateEntity():Add("Transform2D", {x=10,y=20})
                    :Add("Sprite",      {image=img})

    -- 注意: 这里不创建任何 Camera2D entity, 所以 _FindActiveCamera 返回 nil
    w:Render()

    -- 只有 1 个 sprite Push/Pop, 相机不参与
    eq(countFn(calls, "Push"), 1, "D-T4: no camera => only sprite Push")
    eq(countFn(calls, "Draw"), 1, "D-T4: sprite still drawn")
    pass("D-T4: no active camera => sprite rendering still works")
end

-- ============================================================
-- D-T5+D-T6: 3D mesh + Camera3D
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    -- mock mesh
    local mockMesh = {}
    function mockMesh:Draw(matOrTexId)
        calls[#calls + 1] = {fn="mesh:Draw", args={matOrTexId}}
    end

    -- Camera3D
    w:CreateEntity():Add("Transform3D", {x=0,y=5,z=10})
                    :Add("Camera3D",    {active=true, fovY=75, aspect=1.5,
                                          nearZ=0.5, farZ=500,
                                          targetX=0, targetY=0, targetZ=0,
                                          upX=0, upY=1, upZ=0})

    -- MeshRenderer entity
    w:CreateEntity():Add("Transform3D", {x=1,y=2,z=3, ry=45})
                    :Add("MeshRenderer", {mesh=mockMesh, material=nil})

    -- 一个 visible=false 的 mesh entity (不应被画)
    w:CreateEntity():Add("Transform3D", {x=100,y=0,z=0})
                    :Add("MeshRenderer", {mesh=mockMesh, visible=false})

    w:Render()

    -- 验证 SetPerspective 调用一次, fovY=75
    eq(countFn(calls, "SetPerspective"), 1, "D-T6: SetPerspective called")
    local sp = nthCall(calls, "SetPerspective", 1)
    eq(sp.args[1], 75, "D-T6: SetPerspective fovY=75")
    eq(sp.args[2], 1.5, "D-T6: SetPerspective aspect=1.5")

    -- 验证 SetCamera 调用一次, eye=(0,5,10)
    eq(countFn(calls, "SetCamera"), 1, "D-T6: SetCamera called")
    local sc = nthCall(calls, "SetCamera", 1)
    eq(sc.args[1], 0, "D-T6: SetCamera ex=0")
    eq(sc.args[2], 5, "D-T6: SetCamera ey=5")
    eq(sc.args[3], 10, "D-T6: SetCamera ez=10")

    -- SetDepthTest(true) + SetDepthTest(false)
    eq(countFn(calls, "SetDepthTest"), 2, "D-T6: SetDepthTest called twice")
    local d1 = nthCall(calls, "SetDepthTest", 1)
    local d2 = nthCall(calls, "SetDepthTest", 2)
    eq(d1.args[1], true, "D-T6: first SetDepthTest enable=true")
    eq(d2.args[1], false, "D-T6: second SetDepthTest enable=false (cleanup)")

    -- mesh:Draw 只调一次 (visible=false 的不调)
    eq(countFn(calls, "mesh:Draw"), 1, "D-T5: mesh:Draw called once (visible=false skipped)")
    local md = nthCall(calls, "mesh:Draw", 1)
    eq(md.args[1], 0, "D-T5: material=nil => mesh:Draw(0) fallback")

    pass("D-T5/D-T6: 3D mesh + Camera3D rendering correct")
end

-- ============================================================
-- D-T5: mesh + material 时 mesh:Draw 收到 material
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local mockMesh = {}
    function mockMesh:Draw(m) calls[#calls+1] = {fn="mesh:Draw", args={m}} end
    local fakeMat = {kind="material"}

    w:CreateEntity():Add("Transform3D", {})
                    :Add("Camera3D",    {active=true})
    w:CreateEntity():Add("Transform3D", {})
                    :Add("MeshRenderer", {mesh=mockMesh, material=fakeMat})

    w:Render()

    local md = nthCall(calls, "mesh:Draw", 1)
    if not md then fail("D-T5: mesh:Draw missing") end
    eq(md.args[1], fakeMat, "D-T5: mesh:Draw receives material userdata")
    pass("D-T5: material passed to mesh:Draw")
end

-- ============================================================
-- D-T7: MarkRenderNetworked 把内置 component 标 networked
-- ============================================================
do
    local w = World.new()

    -- 调用前: 内置 component 都不 networked
    eq(w._networked_comps.Transform2D, nil, "D-T7: Transform2D not networked before mark")
    eq(w._networked_comps.Sprite, nil, "D-T7: Sprite not networked before mark")

    w:MarkRenderNetworked()

    -- 调用后: 6 个内置都标 networked
    for _, name in ipairs({"Transform2D","Sprite","Camera2D",
                            "Transform3D","MeshRenderer","Camera3D"}) do
        if not w._networked_comps[name] then
            fail("D-T7: "..name.." should be networked after MarkRenderNetworked")
        end
    end
    pass("D-T7: MarkRenderNetworked promoted all 6 builtin components")

    -- 用户自定义 component 不受影响
    w:RegisterComponent("UserComp", {n=0})
    eq(w._networked_comps.UserComp, nil, "D-T7: user component NOT affected by MarkRenderNetworked")
    pass("D-T7: user components untouched")
end

-- ============================================================
-- 兼容性验证: Phase C/C.x.1 现有 API 不被破坏
-- ============================================================
do
    local w = World.new()
    -- v1 API
    w:RegisterComponent("Position", {x=0,y=0}, {networked=true})
    if type(w.NetworkSync) ~= "function" then fail("Compat: NetworkSync missing") end
    if type(w.Update) ~= "function" then fail("Compat: Update missing") end
    if type(w.MarkFullResync) ~= "function" then fail("Compat: MarkFullResync missing") end
    pass("Compat: Phase C/C.x.1 World methods preserved")

    if type(ECS.MirrorFromRoom) ~= "function" then fail("Compat: MirrorFromRoom missing") end
    pass("Compat: Phase C MirrorFromRoom preserved")
end

-- ============================================================
-- Phase D.x.1: parent 层级 (递归 Push parent 变换)
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local img = makeMockImage(32, 32)

    -- root: position (100, 200), 无 parent
    local root = w:CreateEntity():Add('Transform2D', {x=100, y=200})
                                  :Add('Sprite',      {image=img})

    -- child: position (10, 20), parent=root
    local child = w:CreateEntity():Add('Transform2D', {x=10, y=20, parent=root})
                                   :Add('Sprite',      {image=img})

    w:Render()

    -- Push 次数:
    --   root: 1 (self) + 0 (no parent) = 1
    --   child: 1 (parent chain root) + 1 (self) = 2
    --   合计 3
    eq(countFn(calls, "Push"), 3, "Dx1: total Push = 3 (root self + child parent + child self)")
    eq(countFn(calls, "Pop"),  3, "Dx1: Pop balances Push")
    pass("Dx1: parent chain Push/Pop balance ok")

    -- 第 1 个 Translate 应是 root self (100, 200)
    local t1 = nthCall(calls, "Translate", 1)
    eq(t1.args[1], 100, "Dx1: first Translate is root x=100")
    eq(t1.args[2], 200, "Dx1: first Translate is root y=200")

    -- 第 2 个 Translate 是 child's parent chain (root 再 Translate 一次), 也是 (100, 200)
    local t2 = nthCall(calls, "Translate", 2)
    eq(t2.args[1], 100, "Dx1: second Translate is parent (root again) x=100")
    eq(t2.args[2], 200, "Dx1: second Translate is parent y=200")

    -- 第 3 个 Translate 是 child self (10, 20)
    local t3 = nthCall(calls, "Translate", 3)
    eq(t3.args[1], 10, "Dx1: third Translate is child self x=10")
    eq(t3.args[2], 20, "Dx1: third Translate is child self y=20")
    pass("Dx1: child renders with parent chain applied")
end

-- ============================================================
-- Phase D.x.1.2: 3D MeshRenderer parent chain (gfx stack 模式)
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)

    local w = World.new()
    local mockMesh = {}
    function mockMesh:Draw(m) calls[#calls+1] = {fn='mesh:Draw'} end

    -- root 3D entity at (10, 0, 0)
    local root = w:CreateEntity():Add('Transform3D', {x=10, y=0, z=0})
    -- child 3D entity 平移 (5, 0, 0), parent=root, 带 MeshRenderer
    local child = w:CreateEntity():Add('Transform3D', {x=5, y=0, z=0, parent=root})
                                   :Add('MeshRenderer', {mesh=mockMesh})

    -- Camera3D
    w:CreateEntity():Add('Transform3D', {})
                    :Add('Camera3D',    {active=true})

    w:Render()

    -- Push 次数: child 有 1 个 parent (root), 加上 _DrawMesh self push = 2
    eq(countFn(calls, "Push"), 2, "Dx1.2: total Push = 2 (parent + self)")
    eq(countFn(calls, "Pop"),  2, "Dx1.2: Pop balances Push")

    -- 第 1 个 Translate 是 parent (root, x=10)
    local t1 = nthCall(calls, "Translate", 1)
    eq(t1.args[1], 10, "Dx1.2: first Translate is parent x=10")

    -- 第 2 个 Translate 是 self (child, x=5)
    local t2 = nthCall(calls, "Translate", 2)
    eq(t2.args[1], 5, "Dx1.2: second Translate is self x=5")
    pass("Dx1.2: 3D MeshRenderer parent chain ok")
end

-- ============================================================
-- Phase D.x.5.1: _CollectSprites cache 行为
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)
    local w = World.new()
    local img = makeMockImage(32, 32)
    w:CreateEntity():Add('Transform2D', {x=0, y=0}):Add('Sprite', {image=img})
    w:CreateEntity():Add('Transform2D', {x=10, y=0}):Add('Sprite', {image=img})

    -- 第一次: 构建 cache
    local list1 = w:_CollectSprites()
    eq(#list1, 2, "Dx5.1: first call collects 2 sprites")

    -- 第二次: cache 命中, 应返回同一 table 引用
    local list2 = w:_CollectSprites()
    if list1 ~= list2 then fail("Dx5.1: cache hit should return same table reference") end
    pass("Dx5.1: cache hit returns same reference (no rebuild)")

    -- 用户改 z, 必须显式 invalidate
    list1[1].entity._comps.Transform2D.z = 999
    -- 不调 MarkSpriteListDirty: cache 仍 hit (stale)
    local list3 = w:_CollectSprites()
    if list3 ~= list1 then fail("Dx5.1: without MarkSpriteListDirty cache should still hit") end

    -- 显式 invalidate
    w:MarkSpriteListDirty()
    local list4 = w:_CollectSprites()
    if list4 == list1 then fail("Dx5.1: after MarkSpriteListDirty cache should rebuild") end
    pass("Dx5.1: MarkSpriteListDirty triggers rebuild")

    -- entity:Add('Sprite') 自动 invalidate
    w:CreateEntity():Add('Transform2D', {}):Add('Sprite', {image=img})
    local list5 = w:_CollectSprites()
    eq(#list5, 3, "Dx5.1: auto-invalidate after entity:Add(Sprite)")
    pass("Dx5.1: auto-invalidate on Sprite Add")
end

-- ============================================================
-- Phase D.x.1: 循环引用保护 (max 32 depth)
-- ============================================================
do
    local gfx, calls = makeMockGraphics()
    installMockLight(gfx)
    local w = World.new()
    local img = makeMockImage(32, 32)
    -- 故意创造 a.parent = b, b.parent = a (循环)
    local a = w:CreateEntity():Add('Transform2D', {})
                               :Add('Sprite',      {image=img})
    local b = w:CreateEntity():Add('Transform2D', {})
    a._comps.Transform2D.parent = b
    b._comps.Transform2D.parent = a
    -- Render 不应崩 (visited set + depth limit)
    local ok = pcall(function() w:Render() end)
    if not ok then fail("Dx1: parent cycle should not crash Render") end
    pass("Dx1: parent cycle protection ok")
end

print("")
print("Phase D ECS render smoke: ALL PASS")
