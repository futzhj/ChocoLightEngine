local world = Light(Light.Physics.World):New()
world:SetGravity(0, 980)

local ground = world:CreateBody("static", 320, 460)
local groundFixture = ground:AddBox(640, 40)
groundFixture:SetFriction(0.8)
groundFixture:SetRestitution(0.1)

local ball = world:CreateBody("dynamic", 320, 120)
local circle = Light.Physics.NewCircleShape(16)
local ballFixture = ball:CreateFixture(circle, 1.0)
ballFixture:SetFriction(0.4)
ballFixture:SetRestitution(0.6)

local beginCount = 0
local legacyCount = 0

world:BeginContact(function(contact)
  beginCount = beginCount + 1
  assert(contact:GetBodyA() ~= nil, "contact body A missing")
  assert(contact:GetBodyB() ~= nil, "contact body B missing")
end)

world:OnCollision(function(a, b)
  legacyCount = legacyCount + 1
  assert(a ~= nil, "legacy collision body A missing")
  assert(b ~= nil, "legacy collision body B missing")
end)

for i = 1, 180 do
  world:Step(1 / 60)
end

local x, y = ball:GetPosition()
assert(y > 120, "dynamic body did not move down")
assert(world:GetBodyCount() == 2, "body count mismatch")
assert(beginCount > 0, "BeginContact did not fire")
assert(legacyCount > 0, "OnCollision did not fire")

world:DestroyBody(ball)
assert(world:GetBodyCount() == 1, "DestroyBody did not reduce count")

print("physics_p0_p1 smoke ok", x, y, beginCount, legacyCount)
