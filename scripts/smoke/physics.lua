-- Phase AO smoke: Light.Physics + Light.Physics.World
--
-- 覆盖:
--   1) 向后兼容: World/Body 原有 API
--   2) Phase AO: Body 坐标转换/扩展力学/GetFixtures
--   3) Phase AO: Fixture.TestPoint/GetAABB/GetShapeType
--   4) Phase AO: 5 种 Joint 创建 + Joint 公共/类型特定方法
--   5) Phase AO: DestroyJoint + GetJointCount
--   6) Phase AO: DestructionListener (DestroyBody 自动清理 joint)
--   7) Phase AO: World.RayCast + QueryAABB
--   8) 边界: dead joint, cross-world拒绝, wrong-type joint getter

local function fail(msg)
    print("FAIL: " .. tostring(msg))
    os.exit(1)
end

local function pass(msg) print(msg) end

local function approx_eq(a, b, eps)
    eps = eps or 0.5
    return math.abs(a - b) <= eps
end

local function assert_function(t, k)
    if type(t[k]) ~= "function" then fail(k .. " not a function") end
end

-- ==================== 1) module loads ====================

local ok, P = pcall(require, "Light.Physics")
if not ok then fail("require(Light.Physics) failed: " .. tostring(P)) end
if type(P) ~= "table" then fail("Light.Physics not a table") end

for _, k in ipairs({ "NewCircleShape", "NewRectangleShape", "NewPolygonShape", "NewEdgeShape", "NewChainShape" }) do
    assert_function(P, k)
end
pass("Light.Physics module ok (5 shape constructors)")

local ok2, W = pcall(require, "Light.Physics.World")
if not ok2 then fail("require(Light.Physics.World) failed: " .. tostring(W)) end
if type(W) ~= "table" then fail("Light.Physics.World not a table") end

-- 向后兼容的方法
local legacy_fns = {
    "SetGravity", "GetGravity", "Step", "ClearForces",
    "SetAllowSleeping", "SetContinuousPhysics",
    "CreateBody", "DestroyBody", "GetBodyCount",
    "OnCollision", "BeginContact", "EndContact",
}
for _, k in ipairs(legacy_fns) do assert_function(W, k) end
pass("Light.Physics.World legacy API ok")

-- Phase AO 新增方法
local phase_ao_fns = {
    "CreateDistanceJoint", "CreateRevoluteJoint", "CreatePrismaticJoint",
    "CreateWeldJoint", "CreateMouseJoint",
    "DestroyJoint", "GetJointCount",
    "RayCast", "QueryAABB",
}
for _, k in ipairs(phase_ao_fns) do assert_function(W, k) end
pass("Light.Physics.World Phase AO API ok (9 fns)")

-- ==================== 2) create a World ====================

local world = setmetatable({}, { __index = W })
W.__call(world)
world:SetGravity(0, 300)
local gx, gy = world:GetGravity()
if not approx_eq(gx, 0) or not approx_eq(gy, 300) then
    fail("SetGravity/GetGravity mismatch: " .. gx .. "," .. gy)
end
pass("World creation + gravity ok")

-- ==================== 3) Create Bodies + Fixtures ====================

local ground = world:CreateBody("static", 0, 400)
local ground_shape = P.NewRectangleShape(800, 40)
local ground_fx = ground:CreateFixture(ground_shape, 0)
if type(ground_fx) ~= "table" then fail("CreateFixture did not return table") end

local ball = world:CreateBody("dynamic", 100, 100)
local ball_shape = P.NewCircleShape(16)
local ball_fx = ball:CreateFixture(ball_shape, 1.0)

if world:GetBodyCount() ~= 2 then fail("BodyCount != 2") end
pass("Created 2 bodies + fixtures")

-- ==================== 4) Phase AO: Fixture new APIs ====================

for _, k in ipairs({ "TestPoint", "GetAABB", "GetShapeType" }) do
    assert_function(ball_fx, k)
end

-- Shape type 判定
local stype = ball_fx:GetShapeType()
if stype ~= "circle" then fail("ball_fx:GetShapeType() expected 'circle', got " .. tostring(stype)) end
local ground_stype = ground_fx:GetShapeType()
if ground_stype ~= "polygon" then
    fail("ground_fx:GetShapeType() expected 'polygon' (box), got " .. tostring(ground_stype))
end

-- TestPoint: (100,100) 在 ball 内, (500,500) 不在
local inside = ball_fx:TestPoint(100, 100)
local outside = ball_fx:TestPoint(500, 500)
if not inside then fail("TestPoint(inside) returned false") end
if outside then fail("TestPoint(outside) returned true") end

-- GetAABB: ball 约在 (84,84)-(116,116) 附近, 宽高 ~32
local ax, ay, aw, ah = ball_fx:GetAABB()
if not (aw > 20 and aw < 60 and ah > 20 and ah < 60) then
    fail(string.format("GetAABB unexpected size w=%.2f h=%.2f", aw, ah))
end
pass("Phase AO Fixture TestPoint/GetAABB/GetShapeType ok")

-- ==================== 5) Phase AO: Body coordinate helpers ====================

for _, k in ipairs({ "GetWorldPoint", "GetLocalPoint", "GetWorldVector", "GetLocalVector",
                     "ApplyForceAtWorldPoint", "ApplyLinearImpulseAtPoint", "GetFixtures" }) do
    assert_function(ball, k)
end

-- 局部原点对应 body 世界位置
local wx, wy = ball:GetWorldPoint(0, 0)
local bx, by = ball:GetPosition()
if not approx_eq(wx, bx) or not approx_eq(wy, by) then
    fail(string.format("GetWorldPoint(0,0) mismatch: got (%.2f,%.2f), expect (%.2f,%.2f)", wx, wy, bx, by))
end

-- round-trip: local -> world -> local
local lx0, ly0 = 10, 20
local wx1, wy1 = ball:GetWorldPoint(lx0, ly0)
local lx2, ly2 = ball:GetLocalPoint(wx1, wy1)
if not approx_eq(lx0, lx2, 0.5) or not approx_eq(ly0, ly2, 0.5) then
    fail(string.format("World<->Local point roundtrip failed: (%.2f,%.2f)", lx2, ly2))
end

-- 向量变换保角, 对 axis-aligned body 应等于输入
local vx1, vy1 = ball:GetWorldVector(1, 0)
if not approx_eq(vx1, 1, 0.05) or not approx_eq(vy1, 0, 0.05) then
    fail(string.format("GetWorldVector(1,0) = (%.2f,%.2f)", vx1, vy1))
end

-- ApplyForceAtWorldPoint / ApplyLinearImpulseAtPoint 不应崩溃
ball:ApplyForceAtWorldPoint(0, -500, bx, by, true)
ball:ApplyLinearImpulseAtPoint(0, -50, bx, by)  -- optional wake 省略

-- GetFixtures 返回含 1 个 fixture 的表
local fxs = ball:GetFixtures()
if type(fxs) ~= "table" or #fxs ~= 1 then
    fail("GetFixtures expected 1 fixture, got " .. tostring(#fxs))
end
pass("Phase AO Body coordinate/force helpers ok")

-- ==================== 6) Phase AO: Joint creation + common ops ====================

-- CreateDistanceJoint: ground.pos -> ball.pos
local j_dist = world:CreateDistanceJoint(ground, ball, 0, 400, 100, 100)
if type(j_dist) ~= "table" then fail("CreateDistanceJoint failed") end

-- 全部 joint methods
for _, k in ipairs({ "IsAlive", "GetType", "GetBodyA", "GetBodyB",
                     "GetAnchorA", "GetAnchorB", "GetReactionForce", "GetReactionTorque",
                     "IsEnabled", "Destroy",
                     "GetJointAngle", "GetJointSpeed", "GetJointTranslation",
                     "SetMotorSpeed", "EnableMotor", "SetTarget",
                     "SetLength", "GetLength" }) do
    assert_function(j_dist, k)
end

if not j_dist:IsAlive() then fail("new distance joint not alive") end
if j_dist:GetType() ~= "distance" then fail("Distance joint GetType wrong: " .. j_dist:GetType()) end

-- GetBodyA/B 反解
local a = j_dist:GetBodyA()
if type(a) ~= "table" then fail("GetBodyA returned non-table") end
local a_bx, a_by = a:GetPosition()
if not approx_eq(a_bx, 0) or not approx_eq(a_by, 400) then
    fail(string.format("GetBodyA position mismatch: (%.2f,%.2f)", a_bx, a_by))
end

-- GetAnchor 应该返回像素坐标
local an_x, an_y = j_dist:GetAnchorA()
if not approx_eq(an_x, 0, 2) or not approx_eq(an_y, 400, 2) then
    fail(string.format("GetAnchorA mismatch: (%.2f,%.2f)", an_x, an_y))
end

-- SetLength/GetLength
j_dist:SetLength(150)
local len = j_dist:GetLength()
if not approx_eq(len, 150, 2) then fail("SetLength/GetLength mismatch: " .. len) end

-- Wrong-type getter 安全返回 0
local ang = j_dist:GetJointAngle()
if ang ~= 0 then fail("GetJointAngle on distance joint should return 0, got " .. ang) end

-- CreateRevoluteJoint
local j_rev = world:CreateRevoluteJoint(ground, ball, 100, 100)
if j_rev:GetType() ~= "revolute" then fail("Revolute type mismatch") end
j_rev:EnableMotor(true)
j_rev:SetMotorSpeed(1.5)
-- revolute 上 GetJointAngle/Speed 合法
local _ = j_rev:GetJointAngle()
local _ = j_rev:GetJointSpeed()

-- CreatePrismaticJoint
local j_pri = world:CreatePrismaticJoint(ground, ball, 100, 100, 1, 0)
if j_pri:GetType() ~= "prismatic" then fail("Prismatic type mismatch") end
local _ = j_pri:GetJointTranslation()

-- CreateWeldJoint
local j_weld = world:CreateWeldJoint(ground, ball, 100, 100)
if j_weld:GetType() ~= "weld" then fail("Weld type mismatch") end

-- CreateMouseJoint
local j_mouse = world:CreateMouseJoint(ground, ball, 100, 100, 100.0)
if j_mouse:GetType() ~= "mouse" then fail("Mouse type mismatch") end
j_mouse:SetTarget(120, 120)

pass("Phase AO 5 joint types created; common/type-specific ops ok")

-- ==================== 7) GetJointCount + DestroyJoint ====================

local jc = world:GetJointCount()
if jc ~= 5 then fail("expected 5 joints, got " .. jc) end

-- 主动销毁一个
world:DestroyJoint(j_mouse)
if j_mouse:IsAlive() then fail("j_mouse should be dead after DestroyJoint") end

jc = world:GetJointCount()
if jc ~= 4 then fail("expected 4 joints after DestroyJoint, got " .. jc) end

-- Joint:Destroy 自我销毁
j_weld:Destroy()
if j_weld:IsAlive() then fail("j_weld should be dead after Destroy()") end
jc = world:GetJointCount()
if jc ~= 3 then fail("expected 3 joints after Joint:Destroy, got " .. jc) end

-- Double-destroy 安全
j_weld:Destroy()
world:DestroyJoint(j_mouse)
pass("DestroyJoint + Joint:Destroy + double-destroy safe")

-- ==================== 8) DestructionListener: DestroyBody -> joints go dead ====================

-- 此时剩下 j_dist, j_rev, j_pri 都连接 ball
if not j_dist:IsAlive() or not j_rev:IsAlive() or not j_pri:IsAlive() then
    fail("pre-DestroyBody: some joints already dead")
end

world:DestroyBody(ball)

if j_dist:IsAlive() or j_rev:IsAlive() or j_pri:IsAlive() then
    fail("DestroyBody(ball) did not auto-invalidate attached joints")
end
if world:GetJointCount() ~= 0 then
    fail("GetJointCount after DestroyBody expected 0, got " .. world:GetJointCount())
end
pass("DestructionListener: DestroyBody auto-invalidates joints ok")

-- ==================== 9) dead-joint safety ====================

-- dead joint 所有读取应该返回安全默认值, 不崩溃
local _ = j_dist:GetType()        -- nil
local _ = j_dist:GetBodyA()       -- nil
local _ = j_dist:GetAnchorA()     -- 0, 0
local _ = j_dist:GetReactionForce(60.0)
local _ = j_dist:GetReactionTorque(60.0)
local _ = j_dist:IsEnabled()      -- false
j_dist:SetMotorSpeed(1)           -- no-op
j_dist:EnableMotor(false)
j_dist:SetTarget(0, 0)
j_dist:SetLength(100)
pass("Dead joint methods are safe")

-- ==================== 10) Step + contact 不崩 ====================

-- 重新造一个 body 用于后续查询 (ball 已销毁)
local ball2 = world:CreateBody("dynamic", 200, 200)
local ball2_shape = P.NewCircleShape(20)
local ball2_fx = ball2:CreateFixture(ball2_shape, 1.0)

for _ = 1, 10 do world:Step(1 / 60) end
pass("Step x10 ok")

-- ==================== 11) Phase AO: RayCast ====================

-- 从 (200,0) 向下射到 (200,600), 应该命中 ball2 (200,200) 半径 20
local hit_count = 0
local hit_fx = nil
world:RayCast(200, 0, 200, 600, function(fixture, hx, hy, nx, ny, fraction)
    hit_count = hit_count + 1
    hit_fx = fixture
    return 0  -- 终止
end)
if hit_count == 0 then fail("RayCast expected at least 1 hit") end
pass("RayCast ok (hit_count=" .. hit_count .. ")")

-- RayCast 回调返回 -1 忽略: 持续扫描
hit_count = 0
world:RayCast(200, 0, 200, 600, function() hit_count = hit_count + 1; return -1 end)
pass("RayCast ignore path ok (hit_count=" .. hit_count .. ")")

-- RayCast 起终点相同安全
world:RayCast(0, 0, 0, 0, function() fail("zero-length ray callback fired") end)
pass("Zero-length raycast handled")

-- ==================== 12) Phase AO: QueryAABB ====================

-- 查询 (180,180)-(220,220), 应命中 ball2_fx 和 ground_fx 不在里面
local found = 0
world:QueryAABB(180, 180, 40, 40, function(fx)
    found = found + 1
    return true
end)
if found == 0 then fail("QueryAABB expected hits") end
pass("QueryAABB ok (found=" .. found .. ")")

-- 返回 false 立即停止
local first_only = 0
world:QueryAABB(0, 0, 800, 800, function(fx)
    first_only = first_only + 1
    return false  -- 停止
end)
if first_only ~= 1 then fail("QueryAABB stop-early expected 1, got " .. first_only) end
pass("QueryAABB early-stop ok")

-- ==================== 13) cross-world joint 拒绝 ====================

local world2 = setmetatable({}, { __index = W })
W.__call(world2)
local w2_body = world2:CreateBody("dynamic", 0, 0)

local cross_joint = world:CreateDistanceJoint(ground, w2_body, 0, 0, 0, 0)
if cross_joint ~= nil then fail("Cross-world joint should have been rejected, got: " .. tostring(cross_joint)) end
pass("Cross-world joint rejected")

-- ==================== summary ====================

pass("")
pass("=== Phase AO physics smoke passed ===")
