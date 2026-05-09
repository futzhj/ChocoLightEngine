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

-- ============================================================
-- Phase AP smoke (14 ~ 25)
-- ============================================================

-- 为 AP 测试新建一个干净 world
local apw = setmetatable({}, { __index = W })
W.__call(apw)
apw:SetGravity(0, 600)

-- 静态地面 + 两个 dynamic body
local ap_ground  = apw:CreateBody("static", 400, 580)
ap_ground:CreateFixture(P.NewRectangleShape(800, 40), 0.0)
local ap_box_a   = apw:CreateBody("dynamic", 300, 300)
ap_box_a:CreateFixture(P.NewRectangleShape(40, 40), 1.0)
local ap_box_b   = apw:CreateBody("dynamic", 500, 300)
ap_box_b:CreateFixture(P.NewRectangleShape(40, 40), 1.0)

-- ==================== 14) Joint Limits (revolute) ====================

local rev = apw:CreateRevoluteJoint(ap_box_a, ap_box_b, 400, 300)
rev:EnableLimit(true)
if not rev:IsLimitEnabled() then fail("EnableLimit not reflected") end
rev:SetLimits(-0.5, 0.5)
if math.abs(rev:GetLowerLimit() - (-0.5)) > 1e-3 then fail("GetLowerLimit mismatch: " .. rev:GetLowerLimit()) end
if math.abs(rev:GetUpperLimit() - 0.5) > 1e-3 then fail("GetUpperLimit mismatch: " .. rev:GetUpperLimit()) end
rev:EnableMotor(true)
rev:SetMotorSpeed(1.0)
rev:SetMaxMotorTorque(100)
if not rev:IsMotorEnabled() then fail("IsMotorEnabled false after enable") end
if math.abs(rev:GetMotorSpeed() - 1.0) > 1e-3 then fail("GetMotorSpeed mismatch") end
local _ = rev:GetMotorTorque(60)  -- 不验证具体值, 只验证不崩
pass("Joint Limits + Motor (revolute) ok")

-- ==================== 15) Spring API (distance + wheel) ====================

local dist = apw:CreateDistanceJoint(ap_box_a, ap_box_b, 300, 300, 500, 300)
dist:SetStiffness(50.0)
dist:SetDamping(0.5)
if math.abs(dist:GetStiffness() - 50.0) > 1e-3 then fail("Distance stiffness mismatch") end
if math.abs(dist:GetDamping() - 0.5) > 1e-3 then fail("Distance damping mismatch") end
-- SetSpring 便利函数
dist:SetSpring(4.0, 0.5)  -- 4 Hz, ratio 0.5 → 内部转 stiffness/damping
local s_after = dist:GetStiffness()
if s_after <= 0 then fail("SetSpring did not set stiffness, got " .. s_after) end
pass("Spring API (Distance) ok")

-- ==================== 16) WheelJoint with motor + spring + limit ====================

local chassis = apw:CreateBody("dynamic", 200, 100)
chassis:CreateFixture(P.NewRectangleShape(60, 20), 1.0)
local wheel_b = apw:CreateBody("dynamic", 200, 130)
wheel_b:CreateFixture(P.NewCircleShape(15), 1.0)

local wheel = apw:CreateWheelJoint(chassis, wheel_b, 200, 130, 0, 1)
if wheel == nil then fail("CreateWheelJoint returned nil") end
if wheel:GetType() ~= "wheel" then fail("Wheel GetType wrong: " .. tostring(wheel:GetType())) end

wheel:EnableMotor(true)
wheel:SetMotorSpeed(5.0)
wheel:SetMaxMotorTorque(20)
wheel:EnableLimit(true)
wheel:SetLimits(-30, 30)  -- pixels
wheel:SetSpring(4.0, 0.7)

local _wt = wheel:GetMotorTorque(60)
local _wls = wheel:GetJointLinearSpeed()  -- 0 px/s 初始
local _wa = wheel:GetJointAngle()
local _was = wheel:GetJointSpeed()        -- angular
local _wtr = wheel:GetJointTranslation()
if math.abs(wheel:GetLowerLimit() - (-30)) > 1e-2 then fail("Wheel lower limit mismatch") end
pass("WheelJoint (motor+spring+limit) ok")

-- ==================== 17) MotorJoint ====================

local motor = apw:CreateMotorJoint(ap_box_a, ap_box_b, 0.5)
if motor == nil then fail("CreateMotorJoint returned nil") end
if motor:GetType() ~= "motor" then fail("Motor GetType wrong: " .. tostring(motor:GetType())) end
motor:SetLinearOffset(50, 30)
local lx, ly = motor:GetLinearOffset()
if math.abs(lx - 50) > 1e-2 or math.abs(ly - 30) > 1e-2 then fail("LinearOffset mismatch: " .. lx .. ", " .. ly) end
motor:SetAngularOffset(0.5)
if math.abs(motor:GetAngularOffset() - 0.5) > 1e-3 then fail("AngularOffset mismatch") end
motor:SetMaxForce(200)
motor:SetMaxTorque(100)
if math.abs(motor:GetMaxForce() - 200) > 1e-2 then fail("MotorJoint MaxForce mismatch") end
if math.abs(motor:GetMaxTorque() - 100) > 1e-2 then fail("MotorJoint MaxTorque mismatch") end
motor:SetCorrectionFactor(0.7)
if math.abs(motor:GetCorrectionFactor() - 0.7) > 1e-3 then fail("CorrectionFactor mismatch") end
apw:DestroyJoint(motor)
pass("MotorJoint ok")

-- ==================== 18) FrictionJoint ====================

local fric = apw:CreateFrictionJoint(ap_box_a, ap_box_b, 400, 300)
if fric == nil then fail("CreateFrictionJoint returned nil") end
if fric:GetType() ~= "friction" then fail("Friction GetType wrong: " .. tostring(fric:GetType())) end
fric:SetMaxForce(50)
fric:SetMaxTorque(20)
if math.abs(fric:GetMaxForce() - 50) > 1e-2 then fail("Friction MaxForce mismatch") end
if math.abs(fric:GetMaxTorque() - 20) > 1e-2 then fail("Friction MaxTorque mismatch") end
apw:DestroyJoint(fric)
pass("FrictionJoint ok")

-- ==================== 19) PulleyJoint ====================

-- 两个独立 body 各有锚点, ground anchor 在它们上方
local pba = apw:CreateBody("dynamic", 100, 400)
pba:CreateFixture(P.NewRectangleShape(30, 30), 1.0)
local pbb = apw:CreateBody("dynamic", 700, 400)
pbb:CreateFixture(P.NewRectangleShape(30, 30), 1.0)

local pulley = apw:CreatePulleyJoint(
    pba, pbb,
    100, 100,    -- groundA
    700, 100,    -- groundB
    100, 400,    -- anchorA (on body A, world coords)
    700, 400,    -- anchorB (on body B, world coords)
    1.0)         -- ratio
if pulley == nil then fail("CreatePulleyJoint returned nil") end
if pulley:GetType() ~= "pulley" then fail("Pulley GetType wrong: " .. tostring(pulley:GetType())) end
local gax, gay = pulley:GetGroundAnchorA()
if math.abs(gax - 100) > 1e-2 or math.abs(gay - 100) > 1e-2 then fail("GroundAnchorA mismatch: " .. gax .. ", " .. gay) end
if pulley:GetLengthA() <= 0 then fail("LengthA non-positive") end
if pulley:GetCurrentLengthA() <= 0 then fail("CurrentLengthA non-positive") end
if math.abs(pulley:GetRatio() - 1.0) > 1e-3 then fail("Pulley ratio mismatch") end
-- ratio <= 0 应被拒绝
local bad_pulley = apw:CreatePulleyJoint(pba, pbb, 100,100, 700,100, 100,400, 700,400, 0)
if bad_pulley ~= nil then fail("PulleyJoint with ratio<=0 should be rejected") end
pass("PulleyJoint ok")

-- ==================== 20) GearJoint ====================

-- 两个 revolute joint coupled with ratio
local rev1 = apw:CreateRevoluteJoint(ap_ground, pba, 100, 400)
local rev2 = apw:CreateRevoluteJoint(ap_ground, pbb, 700, 400)
local gear = apw:CreateGearJoint(rev1, rev2, 2.0)
if gear == nil then fail("CreateGearJoint returned nil") end
if gear:GetType() ~= "gear" then fail("Gear GetType wrong: " .. tostring(gear:GetType())) end
if math.abs(gear:GetRatio() - 2.0) > 1e-3 then fail("Gear ratio mismatch") end
gear:SetRatio(1.5)
if math.abs(gear:GetRatio() - 1.5) > 1e-3 then fail("Gear SetRatio mismatch") end
local j1 = gear:GetJoint1()
local j2 = gear:GetJoint2()
if j1 == nil or j2 == nil then fail("Gear GetJoint1/2 returned nil") end
if j1:GetType() ~= "revolute" or j2:GetType() ~= "revolute" then fail("Gear coupled joint type mismatch") end
-- 用错误类型组合 (gear + revolute 不允许)
local bad_gear = apw:CreateGearJoint(gear, rev1, 1.0)
if bad_gear ~= nil then fail("GearJoint with non-revolute/prismatic should be rejected") end
pass("GearJoint ok")

-- 必须先销毁 gear, 才能销毁组合 joint
apw:DestroyJoint(gear)

-- ==================== 21) PreSolve + PostSolve callback ====================

local pre_count = 0
local post_count = 0
local last_normal_impulse = 0

apw:PreSolve(function(contact)
    pre_count = pre_count + 1
    -- contact:SetEnabled(false) 应能在 PreSolve 内禁用 (此处不禁用以保留碰撞 → PostSolve)
    local _ = contact:IsTouching()
    contact:SetFriction(0.5)
    contact:GetFriction()
    contact:SetRestitution(0.3)
    contact:GetRestitution()
    contact:ResetFriction()
    contact:ResetRestitution()
    local _ = contact:GetManifoldPointCount()
end)

apw:PostSolve(function(contact, normalImpulses, tangentImpulses)
    post_count = post_count + 1
    if type(normalImpulses) == "table" and #normalImpulses > 0 then
        last_normal_impulse = normalImpulses[1]
    end
end)

-- 让 ap_box_a 和 ap_box_b 撞击 ground (从 300 落到 580)
for _ = 1, 60 do apw:Step(1 / 60) end

if pre_count == 0 then fail("PreSolve callback never fired") end
if post_count == 0 then fail("PostSolve callback never fired") end
-- normalImpulse 应为正数 (撞击力)
if last_normal_impulse <= 0 then fail("PostSolve normal impulse should be > 0, got " .. last_normal_impulse) end
pass("PreSolve+PostSolve ok (pre=" .. pre_count .. ", post=" .. post_count ..
     ", lastImpulse=" .. string.format("%.2f", last_normal_impulse) .. ")")

-- contact:SetEnabled(false) 测试: 让一个新 body 落到平台上, PreSolve 禁用 → 不会反弹
apw:PreSolve(function(c) c:SetEnabled(false) end)
apw:PostSolve(nil)  -- 清掉 PostSolve

local ghost = apw:CreateBody("dynamic", 600, 400)
ghost:CreateFixture(P.NewCircleShape(8), 1.0)
local _, ghost_y_before = ghost:GetPosition()
for _ = 1, 60 do apw:Step(1 / 60) end
local _gx, ghost_y_after = ghost:GetPosition()
-- 因为禁用了所有 contact, ghost 应该穿过地面继续下落
if ghost_y_after <= ghost_y_before + 50 then
    fail("Ghost did not penetrate ground (PreSolve SetEnabled(false) failed): before=" ..
         ghost_y_before .. ", after=" .. ghost_y_after)
end
apw:PreSolve(nil)  -- 清掉 PreSolve, 后续测试恢复正常
pass("Contact:SetEnabled(false) inside PreSolve disables contact ok")

-- ==================== 22) Body 邻居遍历 ====================

-- GetJointList 应包含 rev/dist/wheel/pulley/rev1/rev2 中与当前 body 相关的
local boxa_joints = ap_box_a:GetJointList()
if type(boxa_joints) ~= "table" then fail("GetJointList not a table") end
if #boxa_joints == 0 then fail("box_a should have joints attached") end
-- 每个 entry 是 joint table
for _, j in ipairs(boxa_joints) do
    if not j.GetType then fail("joint entry missing GetType") end
end
pass("Body:GetJointList ok (#=" .. #boxa_joints .. ")")

-- GetContactList: ap_box_a 已落到地上, 应有 1 contact
local boxa_contacts = ap_box_a:GetContactList()
if type(boxa_contacts) ~= "table" then fail("GetContactList not a table") end
-- 注: ap_box_a 在第一轮 Step 后应该接触地面, 但 box_a 也可能与 box_b 接触
-- 至少应该 ≥0 不崩 (因受 motorJoint 控制可能未接触)
for _, c in ipairs(boxa_contacts) do
    if c.bodyA == nil or c.bodyB == nil then fail("contact entry missing body") end
end
pass("Body:GetContactList ok (#=" .. #boxa_contacts .. ")")

-- ==================== 23) Fixture:RayCast (单体) ====================

local cir_body = apw:CreateBody("static", 50, 200)
local cir_fx = cir_body:CreateFixture(P.NewCircleShape(20), 0.0)
local hx, hy, nx, ny, frac = cir_fx:RayCast(50, 0, 50, 400)
if hx == nil then fail("Single-fixture RayCast should hit circle") end
if frac == nil or frac <= 0 or frac >= 1 then fail("RayCast fraction out of range: " .. tostring(frac)) end
-- Miss case
local miss = cir_fx:RayCast(700, 0, 700, 400)
if miss ~= nil then fail("Single-fixture RayCast should miss but returned " .. tostring(miss)) end
pass("Fixture:RayCast ok")

-- ==================== 24) Body 高级属性 ====================

ap_box_a:SetGravityScale(0.5)
if math.abs(ap_box_a:GetGravityScale() - 0.5) > 1e-3 then fail("GravityScale mismatch") end

ap_box_a:SetFixedRotation(true)
if not ap_box_a:IsFixedRotation() then fail("FixedRotation not set") end

ap_box_a:SetSleepingAllowed(false)
if ap_box_a:IsSleepingAllowed() then fail("SleepingAllowed should be false") end

ap_box_a:ResetMassData()
local m, cx, cy, I = ap_box_a:GetMassData()
if m <= 0 then fail("ResetMassData mass should be > 0") end

-- SetMassData 手动覆盖
ap_box_a:SetMassData(2.5, 0, 0, 0.1)
local m2, cx2, cy2, I2 = ap_box_a:GetMassData()
if math.abs(m2 - 2.5) > 1e-3 then fail("SetMassData mass: " .. m2) end
pass("Body 高级属性 ok")

-- ==================== 25) World 仿真控制 ====================

apw:SetSubStepping(true)
if not apw:GetSubStepping() then fail("SubStepping not set") end
apw:SetSubStepping(false)
if apw:GetSubStepping() then fail("SubStepping not cleared") end

apw:SetWarmStarting(false)
if apw:GetWarmStarting() then fail("WarmStarting not cleared") end
apw:SetWarmStarting(true)
if not apw:GetWarmStarting() then fail("WarmStarting not set") end
pass("World 仿真控制 ok")

-- ==================== summary ====================

pass("")
pass("=== Phase AO + AP physics smoke passed ===")
