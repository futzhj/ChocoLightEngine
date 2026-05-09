-- ChocoLight Sample: Light.Physics3D (Phase AU)
--
-- 演示 3D 物理 — RigidBody / Vehicle / SoftBody (rope) 综合,
-- 含 Phase AU Step 4.4 新增的 GetWheelInfo / SetWindVelocity / GetLinkNodes 等。
-- 无 GUI/无资源,纯 console 输出物理状态,跨 6 平台可跑(Bullet 不可用时优雅降级)。

local ok, Phys = pcall(require, "Light.Physics3D")
if not ok or type(Phys) ~= "table" then
    print("Light.Physics3D 不可用,跳过 (Phys=" .. tostring(Phys) .. ")")
    print("\ndemo_physics3d ok (no physics)")
    return
end

print("==== Light.Physics3D demo ====")

-- ==================== 1. World + 地面 + 落体 ====================
local w = Phys.NewWorld()
local plane    = Phys.NewStaticPlane(0, 1, 0, 0)         -- y=0 上半空间
local boxShape = Phys.NewBox(0.5, 0.5, 0.5)

local ground = w:CreateBody({ type = "static", x = 0, y = -1, z = 0,
                              shape = plane, friction = 1.0 })
local box    = w:CreateBody({ type = "dynamic", x = 0, y = 8, z = 0,
                              shape = boxShape, mass = 1.0, restitution = 0.3 })
print(string.format("[1] body: ground=%s, box at y=%.2f",
    tostring(ground:IsAlive()), select(2, box:GetPosition())))

-- ==================== 2. Vehicle (4 轮) — Step 4.4 新接口 ====================
local chassisShape = Phys.NewBox(1.0, 0.3, 2.0)
local chassis = w:CreateBody({ type = "dynamic", x = 5, y = 1.5, z = 0,
                               shape = chassisShape, mass = 800 })
local vehicle = w:CreateVehicle({ chassis = chassis })

-- 4 轮(2 前 2 后),y=-0.6 处接地
local function add(x, z, isFront)
    return vehicle:AddWheel({ x = x, y = -0.6, z = z, dirX = 0, dirY = -1, dirZ = 0,
        axleX = -1, axleY = 0, axleZ = 0, restLen = 0.6, radius = 0.4, isFront = isFront })
end
add(-0.9, 1.4, true);  add(0.9, 1.4, true)   -- 前
add(-0.9, -1.4, false); add(0.9, -1.4, false) -- 后

-- Step 4.4: 用 GetWheelInfo 一次拿齐物理参数
local info = vehicle:GetWheelInfo(0)
print(string.format("[2] vehicle wheel0: r=%.2f friction=%.2f stiff=%.1f",
    info.radius, info.friction, info.suspension_stiffness))

-- 调高摩擦/悬挂以演示 set 接口
for i = 0, 3 do
    vehicle:SetWheelFriction(i, 1.5)
    vehicle:SetSuspensionStiffness(i, 25)
end
vehicle:ApplyEngineForce(800, 2)  -- 后左
vehicle:ApplyEngineForce(800, 3)  -- 后右
print("[2] 应用 800N 引擎力 + 摩擦 1.5 + 悬挂刚度 25")

-- ==================== 3. SoftBody Rope + 风 — Step 4.4 新接口 ====================
local rope = w:NewSoftBodyRope({
    x1 = -3, y1 = 6, z1 = 0,
    x2 = -3, y2 = 2, z2 = 0,
    segments = 8, fixed = 1, mass = 0.3,         -- 顶端固定
})
rope:SetWindVelocity(3, 0, 0)                    -- x 方向风
local nodes = rope:GetNodeCount()
local links = rope:GetLinkCount()
print(string.format("[3] rope: %d nodes / %d links, 顶端固定, 风 vx=3", nodes, links))

-- 拓扑展示
local n0a, n0b = rope:GetLinkNodes(0)
local nLa, nLb = rope:GetLinkNodes(links - 1)
print(string.format("[3] 拓扑: link[0]=(%d,%d), link[%d]=(%d,%d)",
    n0a, n0b, links - 1, nLa, nLb))

-- ==================== 4. 模拟 60 帧并采样 ====================
print("[4] 模拟 60 帧 (1s) ...")
for i = 1, 60 do w:Step(1.0 / 60) end

local _, by = box:GetPosition()
local _, ny = rope:GetNodePosition(math.floor(nodes / 2))     -- 中段下垂
local speed = vehicle:GetSpeed()
local in_contact_count = 0
for i = 0, 3 do if vehicle:IsWheelInContact(i) then in_contact_count = in_contact_count + 1 end end

print(string.format("[4] 1s 后: box.y=%.2f (从 8 落下), rope.mid.y=%.2f, vehicle %.1f km/h, 接地轮=%d/4",
    by, ny, speed, in_contact_count))

-- ==================== 5. 清理 ====================
rope:Delete()
vehicle:Delete()
box:Delete()
ground:Delete()
print(string.format("[5] 清理后: softbodies=%d, vehicles=%d, bodies=%d",
    w:GetSoftBodyCount(), w:GetVehicleCount(), w:GetBodyCount()))
w:Delete()

print("\ndemo_physics3d ok")
