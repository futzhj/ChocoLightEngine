# LÖVE Physics P0/P1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current Box2D v3 desktop-only Physics binding with vendored Box2D v2.4.1 and implement the P0/P1 LÖVE-style Physics core for Windows/Linux/macOS/Android/iOS.

**Architecture:** Box2D becomes a vendored static dependency under `ChocoLight/third_party/box2d`. `light_physics.cpp` is rewritten around Box2D v2 C++ objects while preserving current `World`/`Body` Lua API and adding `Shape`/`Fixture`/basic contact callbacks. Web keeps exported Physics symbols through a clear `CHOCO_HAS_BOX2D=0` stub path.

**Tech Stack:** C++17, Lua 5.1 C API, Box2D v2.4.1, CMake, Android NDK/Gradle, ChocoLight module registration helpers.

---

## Scope

This plan implements only P0/P1 from `docs/superpowers/specs/2026-05-02-love-physics-design.md`:

- P0: vendored Box2D v2.4.1, cross-platform CMake restoration, old Physics API compatibility.
- P1: core `World` / `Body` / `Shape` / `Fixture` objects plus queued `BeginContact` / `EndContact` callbacks and legacy `OnCollision`.

The following are deliberately outside this plan:

- Joint API family.
- RayCast and QueryAABB.
- advanced `PreSolve` / `PostSolve` mutation support.
- visual debug draw.

## File Structure

- Create: `ChocoLight/third_party/box2d/`
  - Vendored Box2D v2.4.1 source tree.
- Modify: `ChocoLight/CMakeLists.txt`
  - Remove Box2D v3 FetchContent.
  - Add vendored Box2D for all non-Emscripten platforms.
  - Compile `light_physics.cpp` on all platforms.
  - Define `CHOCO_HAS_BOX2D=1` or `0` explicitly.
- Replace: `ChocoLight/src/light_physics.cpp`
  - Reimplement the module using Box2D v2 C++ API.
  - Preserve existing Lua API and add P1 `Shape` / `Fixture` APIs.
- Create: `scripts/smoke/physics_p0_p1.lua`
  - Lua smoke script that exercises compatibility and P1 features.
- Modify: `docs/PROJECT_SUMMARY.md`
  - Update Physics dependency note from Box2D v3 desktop-only to Box2D v2.4.1 cross-platform.
- Modify: `ENGINE_EVALUATION.md`
  - Mark Android Physics capability as restored after build validation.
- Modify: `docs/Phase2游戏能力/TASK_Phase2.md`
  - Update T9/T10 wording from Box2D v3 FetchContent to vendored Box2D v2.4.1.

---

### Task 1: Vendor Box2D v2.4.1

**Files:**
- Create: `ChocoLight/third_party/box2d/`

- [ ] **Step 1: Verify current Box2D source is not vendored**

Run:

```powershell
git status --short
git ls-files ChocoLight/third_party/box2d
Test-Path ChocoLight\third_party\box2d
```

Expected:

```text
False
```

If `git ls-files` prints paths, stop and inspect because the vendor directory already exists.

- [ ] **Step 2: Fetch Box2D v2.4.1 source**

Run from repository root:

```powershell
git clone --depth 1 --branch v2.4.1 https://github.com/erincatto/box2d.git ChocoLight/third_party/box2d
```

Expected:

```text
Cloning into 'ChocoLight/third_party/box2d'...
```

- [ ] **Step 3: Remove nested git metadata**

Run:

```powershell
Remove-Item -Recurse -Force ChocoLight\third_party\box2d\.git
Test-Path ChocoLight\third_party\box2d\.git
```

Expected:

```text
False
```

- [ ] **Step 4: Verify expected Box2D v2 headers exist**

Run:

```powershell
Test-Path ChocoLight\third_party\box2d\include\box2d\box2d.h
Select-String -Path ChocoLight\third_party\box2d\include\box2d\b2_world.h -Pattern "class b2World"
```

Expected:

```text
True
```

and one match containing `class b2World`.

- [ ] **Step 5: Commit vendored dependency**

Run:

```powershell
git add ChocoLight/third_party/box2d
git commit -m "vendor: add Box2D v2.4.1"
```

Expected: commit succeeds and does not report `adding embedded git repository`.

---

### Task 2: Switch CMake to vendored Box2D

**Files:**
- Modify: `ChocoLight/CMakeLists.txt`

- [ ] **Step 1: Capture current CMake failure condition for Android Physics**

Run:

```powershell
Select-String -Path ChocoLight\CMakeLists.txt -Pattern "Box2D v3|SKIPPED on Android|Physics 源文件"
```

Expected: output shows the old Box2D v3 FetchContent block and Android skip condition.

- [ ] **Step 2: Replace the Box2D v3 FetchContent block**

In `ChocoLight/CMakeLists.txt`, replace lines around the current Box2D v3 block with:

```cmake
# ==================== Box2D v2.4.1 (vendored, non-Web) ====================
if(NOT EMSCRIPTEN)
    set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/box2d ${CMAKE_CURRENT_BINARY_DIR}/box2d EXCLUDE_FROM_ALL)
    message(STATUS "Box2D: integrated via vendored source (v2.4.1)")
else()
    message(STATUS "Box2D: disabled on Web; Physics exports stubbed")
endif()
```

- [ ] **Step 3: Compile Physics source on every platform**

Replace the current conditional Physics source section:

```cmake
# Physics 源文件 (非 Android, 需要 Box2D)
if(NOT ANDROID)
    list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_physics.cpp)
endif()
```

with:

```cmake
# Physics 源文件: 非 Web 使用 Box2D, Web 保留导出空壳
list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_physics.cpp)
```

- [ ] **Step 4: Replace the Box2D link section**

Replace the current non-Android Box2D link block:

```cmake
# Box2D (非 Android)
if(NOT ANDROID)
    target_link_libraries(Light PRIVATE box2d)
    target_include_directories(Light PRIVATE ${box2d_SOURCE_DIR}/include)
    target_compile_definitions(Light PRIVATE CHOCO_HAS_BOX2D=1)
endif()
```

with:

```cmake
# Box2D: desktop/mobile enabled, Web stubbed
if(NOT EMSCRIPTEN)
    target_link_libraries(Light PRIVATE box2d)
    target_compile_definitions(Light PRIVATE CHOCO_HAS_BOX2D=1)
else()
    target_compile_definitions(Light PRIVATE CHOCO_HAS_BOX2D=0)
endif()
```

- [ ] **Step 5: Configure Windows build**

Run:

```powershell
cmake -S ChocoLight -B ChocoLight/build-physics-plan -A x64
```

Expected output contains:

```text
Box2D: integrated via vendored source (v2.4.1)
Configuring done
Generating done
```

- [ ] **Step 6: Configure Android build**

Run:

```powershell
cmake -S ChocoLight -B ChocoLight/build-android-physics-plan -DCMAKE_SYSTEM_NAME=Android -DCMAKE_ANDROID_NDK=$env:ANDROID_NDK_ROOT -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a -DCMAKE_ANDROID_STL_TYPE=c++_shared -DCMAKE_BUILD_TYPE=Release
```

Expected output contains:

```text
Box2D: integrated via vendored source (v2.4.1)
Configuring done
Generating done
```

If `$env:ANDROID_NDK_ROOT` is empty on the current machine, record that Android local configure cannot run and rely on GitHub Actions after implementation.

- [ ] **Step 7: Commit CMake migration**

Run:

```powershell
git add ChocoLight/CMakeLists.txt
git commit -m "build: switch Physics to vendored Box2D v2"
```

---

### Task 3: Add Lua smoke coverage for P0/P1

**Files:**
- Create: `scripts/smoke/physics_p0_p1.lua`

- [ ] **Step 1: Create smoke test script**

Create `scripts/smoke/physics_p0_p1.lua` with:

```lua
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
```

- [ ] **Step 2: Verify script has no syntax error with Lua parser if available**

Run:

```powershell
if (Test-Path lumen-master\build\src\lightc\Release\lightc.exe) { lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\physics_p0_p1.lua } else { Write-Output "lightc parser unavailable; syntax check skipped until runtime smoke" }
```

Expected: either parser succeeds or prints the skipped-check message.

- [ ] **Step 3: Commit smoke script**

Run:

```powershell
git add scripts/smoke/physics_p0_p1.lua
git commit -m "test: add Physics P0/P1 smoke script"
```

---

### Task 4: Rewrite Physics wrappers for Box2D v2 core compatibility

**Files:**
- Replace: `ChocoLight/src/light_physics.cpp`

- [ ] **Step 1: Run Windows build to capture current v3/v2 mismatch**

Run after Task 2:

```powershell
cmake --build ChocoLight/build-physics-plan --config Release --target Light
```

Expected: build fails in `light_physics.cpp` with missing v3 symbols such as `b2WorldId`, `b2CreateWorld`, or `b2BodyId`. This confirms the test is failing for the expected reason.

- [ ] **Step 2: Replace v3 includes and object model**

At the top of `ChocoLight/src/light_physics.cpp`, use this structure:

```cpp
#include "light.h"

#if CHOCO_HAS_BOX2D
#include <box2d/box2d.h>
#endif

#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

static constexpr float PTM = 32.0f;

#if CHOCO_HAS_BOX2D
struct PhysicsWorld;
struct PhysicsBody;
struct PhysicsShape;
struct PhysicsFixture;
struct PhysicsContactEvent;

struct PhysicsBody {
    b2Body* body;
    PhysicsWorld* owner;
    int selfRef;
    bool alive;
};

struct PhysicsFixture {
    b2Fixture* fixture;
    PhysicsBody* owner;
    int selfRef;
    int userRef;
    bool alive;
};

enum PhysicsShapeType {
    PHYSICS_SHAPE_CIRCLE,
    PHYSICS_SHAPE_BOX,
    PHYSICS_SHAPE_POLYGON,
    PHYSICS_SHAPE_EDGE,
    PHYSICS_SHAPE_CHAIN,
};

struct PhysicsShape {
    PhysicsShapeType type;
    b2CircleShape circle;
    b2PolygonShape polygon;
    b2EdgeShape edge;
    b2ChainShape* chain;
};

struct PhysicsContactEvent {
    bool begin;
    b2Fixture* fixtureA;
    b2Fixture* fixtureB;
};

class PhysicsContactListener : public b2ContactListener {
public:
    explicit PhysicsContactListener(PhysicsWorld* world) : world_(world) {}
    void BeginContact(b2Contact* contact) override;
    void EndContact(b2Contact* contact) override;
private:
    PhysicsWorld* world_;
};

struct PhysicsWorld {
    b2World* world;
    PhysicsContactListener* listener;
    std::vector<PhysicsBody*> bodies;
    std::vector<PhysicsFixture*> fixtures;
    std::vector<PhysicsContactEvent> contactEvents;
    int selfRef;
    int legacyCollisionRef;
    int beginContactRef;
    int endContactRef;
    lua_State* L;
    bool alive;
};
#endif
```

- [ ] **Step 3: Add object check helpers**

Implement helper functions with these exact names and semantics:

```cpp
#if CHOCO_HAS_BOX2D
static PhysicsWorld* CheckWorld(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* w = (PhysicsWorld*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (w && w->alive) ? w : nullptr;
}

static PhysicsBody* CheckBody(lua_State* L, int idx) {
    lua_getfield(L, idx, "__body");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* b = (PhysicsBody*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (b && b->alive && b->body) ? b : nullptr;
}

static PhysicsShape* CheckShape(lua_State* L, int idx) {
    lua_getfield(L, idx, "__shape");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* s = (PhysicsShape*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return s;
}

static PhysicsFixture* CheckFixture(lua_State* L, int idx) {
    lua_getfield(L, idx, "__fixture");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* f = (PhysicsFixture*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (f && f->alive && f->fixture) ? f : nullptr;
}
#endif
```

- [ ] **Step 4: Implement `World.__call`, `SetGravity`, `GetGravity`, `Step`, `ClearForces`, and `GetBodyCount`**

Use these exact Box2D v2 calls:

```cpp
// Constructor core
b2Vec2 gravity(0.0f, 10.0f);
w->world = new b2World(gravity);
w->listener = new PhysicsContactListener(w);
w->world->SetContactListener(w->listener);

// Gravity setters/getters
w->world->SetGravity(b2Vec2(gx / PTM, gy / PTM));
b2Vec2 g = w->world->GetGravity();
lua_pushnumber(L, g.x * PTM);
lua_pushnumber(L, g.y * PTM);

// Step
w->world->Step(dt, 8, 3);
// After Step, dispatch queued contactEvents and then clear the vector.

// ClearForces
w->world->ClearForces();
```

- [ ] **Step 5: Implement body compatibility methods**

Replace v3 body calls with Box2D v2 equivalents:

```cpp
// CreateBody
b2BodyDef bodyDef;
bodyDef.position.Set(px / PTM, py / PTM);
if (strcmp(typeStr, "dynamic") == 0) bodyDef.type = b2_dynamicBody;
else if (strcmp(typeStr, "kinematic") == 0) bodyDef.type = b2_kinematicBody;
else bodyDef.type = b2_staticBody;
b2Body* body = w->world->CreateBody(&bodyDef);

// Position
b2Vec2 pos = b->body->GetPosition();
b->body->SetTransform(b2Vec2(x / PTM, y / PTM), b->body->GetAngle());

// Velocity
b2Vec2 v = b->body->GetLinearVelocity();
b->body->SetLinearVelocity(b2Vec2(vx / PTM, vy / PTM));

// Force and impulse
b->body->ApplyForceToCenter(b2Vec2(fx / PTM, fy / PTM), true);
b->body->ApplyLinearImpulseToCenter(b2Vec2(ix / PTM, iy / PTM), true);
```

- [ ] **Step 6: Add `World.__gc`**

`World.__gc` must:

```cpp
static int l_World_GC(lua_State* L) {
#if CHOCO_HAS_BOX2D
    auto* w = (PhysicsWorld*)lua_touserdata(L, 1);
    if (!w || !w->alive) return 0;
    for (auto* body : w->bodies) {
        if (body) {
            body->alive = false;
            body->body = nullptr;
            if (body->selfRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, body->selfRef);
            body->selfRef = LUA_NOREF;
        }
    }
    for (auto* fixture : w->fixtures) {
        if (fixture) {
            fixture->alive = false;
            fixture->fixture = nullptr;
            if (fixture->selfRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, fixture->selfRef);
            if (fixture->userRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, fixture->userRef);
            fixture->selfRef = LUA_NOREF;
            fixture->userRef = LUA_NOREF;
        }
    }
    if (w->legacyCollisionRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->legacyCollisionRef);
    if (w->beginContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->beginContactRef);
    if (w->endContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->endContactRef);
    if (w->selfRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->selfRef);
    delete w->listener;
    delete w->world;
    w->alive = false;
#endif
    return 0;
}
```

- [ ] **Step 7: Verify Windows build passes core compatibility**

Run:

```powershell
cmake --build ChocoLight/build-physics-plan --config Release --target Light
```

Expected: no `light_physics.cpp` compile errors.

- [ ] **Step 8: Commit core rewrite**

Run:

```powershell
git add ChocoLight/src/light_physics.cpp
git commit -m "feat: migrate Physics core to Box2D v2"
```

---

### Task 5: Add Shape and Fixture P1 APIs

**Files:**
- Modify: `ChocoLight/src/light_physics.cpp`

- [ ] **Step 1: Add static shape constructors to `Light.Physics`**

Register these functions on the `Physics` module table:

```cpp
static const luaL_Reg physics_funcs[] = {
    {"NewCircleShape",    l_Physics_NewCircleShape},
    {"NewRectangleShape", l_Physics_NewRectangleShape},
    {"NewPolygonShape",   l_Physics_NewPolygonShape},
    {"NewEdgeShape",      l_Physics_NewEdgeShape},
    {"NewChainShape",     l_Physics_NewChainShape},
    {NULL, NULL}
};
```

`luaopen_Light_Physics` must call:

```cpp
LT::RegisterModule(L, "Physics", physics_funcs);
return 1;
```

- [ ] **Step 2: Implement circle and rectangle shape constructors first**

Use this behavior:

```cpp
static int l_Physics_NewCircleShape(lua_State* L) {
#if CHOCO_HAS_BOX2D
    float radius = (float)luaL_checknumber(L, 1) / PTM;
    lua_createtable(L, 0, 1);
    auto* s = (PhysicsShape*)lua_newuserdata(L, sizeof(PhysicsShape));
    memset(s, 0, sizeof(PhysicsShape));
    s->type = PHYSICS_SHAPE_CIRCLE;
    s->chain = nullptr;
    s->circle.m_radius = radius;
    s->circle.m_p.Set(0.0f, 0.0f);
    lua_setfield(L, -2, "__shape");
    return 1;
#else
    return luaL_error(L, "Light.Physics.NewCircleShape requires Box2D");
#endif
}

static int l_Physics_NewRectangleShape(lua_State* L) {
#if CHOCO_HAS_BOX2D
    float w = (float)luaL_checknumber(L, 1) / PTM;
    float h = (float)luaL_checknumber(L, 2) / PTM;
    lua_createtable(L, 0, 1);
    auto* s = (PhysicsShape*)lua_newuserdata(L, sizeof(PhysicsShape));
    memset(s, 0, sizeof(PhysicsShape));
    s->type = PHYSICS_SHAPE_BOX;
    s->chain = nullptr;
    s->polygon.SetAsBox(w * 0.5f, h * 0.5f);
    lua_setfield(L, -2, "__shape");
    return 1;
#else
    return luaL_error(L, "Light.Physics.NewRectangleShape requires Box2D");
#endif
}
```

- [ ] **Step 3: Implement `Body:CreateFixture(shape, density)`**

Use this Box2D v2 fixture creation logic:

```cpp
b2FixtureDef def;
def.density = (float)luaL_optnumber(L, 3, 1.0);
def.friction = 0.2f;
def.restitution = 0.0f;

switch (shape->type) {
    case PHYSICS_SHAPE_CIRCLE: def.shape = &shape->circle; break;
    case PHYSICS_SHAPE_BOX: def.shape = &shape->polygon; break;
    case PHYSICS_SHAPE_POLYGON: def.shape = &shape->polygon; break;
    case PHYSICS_SHAPE_EDGE: def.shape = &shape->edge; break;
    case PHYSICS_SHAPE_CHAIN: def.shape = shape->chain; break;
}

b2Fixture* fixture = body->body->CreateFixture(&def);
fixture->GetUserData().pointer = reinterpret_cast<uintptr_t>(wrapper);
```

The returned Lua table must contain `__fixture` and fixture methods registered by `luaL_setfuncs`.

- [ ] **Step 4: Make old `Body:AddBox` and `Body:AddCircle` return Fixture**

`Body:AddBox(w, h)` must internally create a temporary `PhysicsShape` rectangle and return the created Fixture table.

`Body:AddCircle(r)` must internally create a temporary `PhysicsShape` circle and return the created Fixture table.

Old Lua code that ignores the return value remains valid.

- [ ] **Step 5: Add Fixture methods**

Register these methods:

```cpp
static const luaL_Reg fixture_funcs[] = {
    {"GetBody",        l_Fixture_GetBody},
    {"SetDensity",     l_Fixture_SetDensity},
    {"GetDensity",     l_Fixture_GetDensity},
    {"SetFriction",    l_Fixture_SetFriction},
    {"GetFriction",    l_Fixture_GetFriction},
    {"SetRestitution", l_Fixture_SetRestitution},
    {"GetRestitution", l_Fixture_GetRestitution},
    {"SetSensor",      l_Fixture_SetSensor},
    {"IsSensor",       l_Fixture_IsSensor},
    {"SetFilterData",  l_Fixture_SetFilterData},
    {"GetFilterData",  l_Fixture_GetFilterData},
    {"SetUserData",    l_Fixture_SetUserData},
    {"GetUserData",    l_Fixture_GetUserData},
    {NULL, NULL}
};
```

Use these Box2D v2 calls:

```cpp
fixture->fixture->SetDensity(value);
fixture->fixture->GetDensity();
fixture->fixture->SetFriction(value);
fixture->fixture->GetFriction();
fixture->fixture->SetRestitution(value);
fixture->fixture->GetRestitution();
fixture->fixture->SetSensor(enabled);
fixture->fixture->IsSensor();
fixture->fixture->SetFilterData(filter);
fixture->fixture->GetFilterData();
```

After density changes, call:

```cpp
fixture->owner->body->ResetMassData();
```

- [ ] **Step 6: Verify smoke script reaches P1 API at runtime**

After building a runnable template, run the smoke script through the engine. On Windows, use the available local template executable if present:

```powershell
if (Test-Path Light-0.2.3\light.exe) { Copy-Item ChocoLight\build-physics-plan\bin\Release\Light.dll Light-0.2.3\ -Force; Light-0.2.3\light.exe scripts\smoke\physics_p0_p1.lua } else { Write-Output "runtime smoke skipped: Light-0.2.3/light.exe not present" }
```

Expected if executable exists:

```text
physics_p0_p1 smoke ok
```

- [ ] **Step 7: Commit P1 Shape/Fixture APIs**

Run:

```powershell
git add ChocoLight/src/light_physics.cpp
git commit -m "feat: add Physics Shape and Fixture APIs"
```

---

### Task 6: Add queued contact callbacks and compatibility `OnCollision`

**Files:**
- Modify: `ChocoLight/src/light_physics.cpp`

- [ ] **Step 1: Implement listener event capture**

Use this listener behavior:

```cpp
void PhysicsContactListener::BeginContact(b2Contact* contact) {
    if (!world_ || !world_->alive) return;
    world_->contactEvents.push_back({true, contact->GetFixtureA(), contact->GetFixtureB()});
}

void PhysicsContactListener::EndContact(b2Contact* contact) {
    if (!world_ || !world_->alive) return;
    world_->contactEvents.push_back({false, contact->GetFixtureA(), contact->GetFixtureB()});
}
```

- [ ] **Step 2: Implement `World:BeginContact` and `World:EndContact` registration**

Use the same registry pattern as `World:OnCollision`:

```cpp
static int l_World_BeginContact(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (w->beginContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->beginContactRef);
    lua_pushvalue(L, 2);
    w->beginContactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_World_EndContact(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (w->endContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->endContactRef);
    lua_pushvalue(L, 2);
    w->endContactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}
```

- [ ] **Step 3: Add Contact table creation and lookup helpers**

A contact table must expose:

```cpp
static const luaL_Reg contact_funcs[] = {
    {"GetBodyA",    l_Contact_GetBodyA},
    {"GetBodyB",    l_Contact_GetBodyB},
    {"GetFixtureA", l_Contact_GetFixtureA},
    {"GetFixtureB", l_Contact_GetFixtureB},
    {NULL, NULL}
};
```

The contact table can store wrapper references as fields:

```cpp
static PhysicsFixture* FixtureFromB2(b2Fixture* fixture) {
    if (!fixture) return nullptr;
    uintptr_t ptr = fixture->GetUserData().pointer;
    if (ptr == 0) return nullptr;
    auto* wrapper = reinterpret_cast<PhysicsFixture*>(ptr);
    if (!wrapper || !wrapper->alive || wrapper->fixture != fixture) return nullptr;
    return wrapper;
}

static bool PushBodySelf(lua_State* L, PhysicsBody* body) {
    if (!body || !body->alive || body->selfRef == LUA_NOREF) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, body->selfRef);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static bool PushFixtureSelf(lua_State* L, PhysicsFixture* fixture) {
    if (!fixture || !fixture->alive || fixture->selfRef == LUA_NOREF) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, fixture->selfRef);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static int l_Contact_GetBodyA(lua_State* L) {
    lua_getfield(L, 1, "__bodyA");
    return 1;
}

static int l_Contact_GetBodyB(lua_State* L) {
    lua_getfield(L, 1, "__bodyB");
    return 1;
}

static int l_Contact_GetFixtureA(lua_State* L) {
    lua_getfield(L, 1, "__fixtureA");
    return 1;
}

static int l_Contact_GetFixtureB(lua_State* L) {
    lua_getfield(L, 1, "__fixtureB");
    return 1;
}

static void PushContactTable(lua_State* L, PhysicsBody* bodyA, PhysicsBody* bodyB,
                             PhysicsFixture* fixtureA, PhysicsFixture* fixtureB) {
    lua_createtable(L, 0, 8);
    if (PushBodySelf(L, bodyA)) lua_setfield(L, -2, "__bodyA");
    else { lua_pushnil(L); lua_setfield(L, -2, "__bodyA"); }
    if (PushBodySelf(L, bodyB)) lua_setfield(L, -2, "__bodyB");
    else { lua_pushnil(L); lua_setfield(L, -2, "__bodyB"); }
    if (PushFixtureSelf(L, fixtureA)) lua_setfield(L, -2, "__fixtureA");
    else { lua_pushnil(L); lua_setfield(L, -2, "__fixtureA"); }
    if (PushFixtureSelf(L, fixtureB)) lua_setfield(L, -2, "__fixtureB");
    else { lua_pushnil(L); lua_setfield(L, -2, "__fixtureB"); }
    luaL_setfuncs(L, contact_funcs, 0);
}
```

- [ ] **Step 4: Dispatch queued contacts after `world->Step`**

After `w->world->Step(dt, 8, 3)`, call this helper:

```cpp
static void CallLuaNoReturn(lua_State* L, int nargs) {
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Physics contact callback error: %s", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

static void DispatchContactEvents(lua_State* L, PhysicsWorld* w) {
    if (!w || !w->alive) return;
    std::vector<PhysicsContactEvent> events = w->contactEvents;
    w->contactEvents.clear();

    for (const auto& ev : events) {
        PhysicsFixture* fixtureA = FixtureFromB2(ev.fixtureA);
        PhysicsFixture* fixtureB = FixtureFromB2(ev.fixtureB);
        if (!fixtureA || !fixtureB || !fixtureA->owner || !fixtureB->owner) continue;
        PhysicsBody* bodyA = fixtureA->owner;
        PhysicsBody* bodyB = fixtureB->owner;
        if (!bodyA->alive || !bodyB->alive) continue;

        if (ev.begin && w->legacyCollisionRef != LUA_NOREF) {
            int baseTop = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, w->legacyCollisionRef);
            bool canCall = lua_isfunction(L, -1) && PushBodySelf(L, bodyA) && PushBodySelf(L, bodyB);
            if (canCall) {
                CallLuaNoReturn(L, 2);
            } else {
                lua_settop(L, baseTop);
            }
        }

        int ref = ev.begin ? w->beginContactRef : w->endContactRef;
        if (ref == LUA_NOREF) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        PushContactTable(L, bodyA, bodyB, fixtureA, fixtureB);
        CallLuaNoReturn(L, 1);
    }
}
```

- [ ] **Step 5: Verify smoke test contact assertions**

Run the runtime smoke command from Task 5 again.

Expected if executable exists:

```text
physics_p0_p1 smoke ok
```

- [ ] **Step 6: Commit contact callback support**

Run:

```powershell
git add ChocoLight/src/light_physics.cpp
git commit -m "feat: queue Physics contact callbacks"
```

---

### Task 7: Documentation and verification

**Files:**
- Modify: `docs/PROJECT_SUMMARY.md`
- Modify: `ENGINE_EVALUATION.md`
- Modify: `docs/Phase2游戏能力/TASK_Phase2.md`
- Modify: `ChocoLight/src/light_physics.cpp`
- Generated: `docs/api/Light_Physics.md` and related files, if annotation parser emits them.

- [ ] **Step 1: Add `@lua_api` annotations for new Physics API**

Add annotations above each Lua binding function in `light_physics.cpp`. Use this format:

```cpp
/// @lua_api Light.Physics.NewCircleShape
/// @brief 创建圆形碰撞形状
/// @param radius number 半径, 单位为像素
/// @return table Shape 对象
static int l_Physics_NewCircleShape(lua_State* L) {
```

For methods, use names such as:

```cpp
/// @lua_api Light.Physics.World.BeginContact
/// @lua_api Light.Physics.Body.CreateFixture
/// @lua_api Light.Physics.Fixture.SetDensity
```

- [ ] **Step 2: Regenerate API docs**

Run:

```powershell
python tools\gen_api_doc.py
```

Expected output includes generated or updated Physics API docs under `docs/api`.

- [ ] **Step 3: Update project summary**

Change `docs/PROJECT_SUMMARY.md` Physics row from desktop Box2D v3 wording to:

```markdown
| `Light.Physics` | `luaopen_Light_Physics` | Box2D v2.4.1 物理引擎 (桌面/移动) | C++ |
```

- [ ] **Step 4: Update engine evaluation**

In `ENGINE_EVALUATION.md`, replace the Physics note:

```markdown
> Box2D v3.0 | 桌面平台
```

with:

```markdown
> Box2D v2.4.1 | 桌面 + Android/iOS
```

Replace:

```markdown
- Android 暂不支持 (Box2D v3 CPU 限制)
```

with:

```markdown
- Android/iOS 通过 vendored Box2D v2.4.1 启用
```

- [ ] **Step 5: Update Phase2 task wording**

In `docs/Phase2游戏能力/TASK_Phase2.md`, change:

```markdown
## T9: Box2D v3.x FetchContent
```

into:

```markdown
## T9: Box2D v2.4.1 vendored source
```

Change the output line to:

```markdown
**输出**: `third_party/box2d` vendored source + CMake integration
```

- [ ] **Step 6: Clean Windows build**

Run:

```powershell
Remove-Item -Recurse -Force ChocoLight\build-physics-final -ErrorAction SilentlyContinue
cmake -S ChocoLight -B ChocoLight/build-physics-final -A x64
cmake --build ChocoLight/build-physics-final --config Release --target Light
```

Expected: `Light.dll` is produced under `ChocoLight/build-physics-final/bin/Release/`.

- [ ] **Step 7: Android configure/build validation**

If Android SDK/NDK are configured locally, run:

```powershell
cmake -S ChocoLight -B ChocoLight/build-android-physics-final -DCMAKE_SYSTEM_NAME=Android -DCMAKE_ANDROID_NDK=$env:ANDROID_NDK_ROOT -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a -DCMAKE_ANDROID_STL_TYPE=c++_shared -DCMAKE_BUILD_TYPE=Release
cmake --build ChocoLight/build-android-physics-final --config Release --target Light
```

Expected: static `Light` library builds without missing Box2D headers or symbols.

- [ ] **Step 8: Commit docs and generated API docs**

Run:

```powershell
git add ChocoLight/src/light_physics.cpp docs/api docs/PROJECT_SUMMARY.md ENGINE_EVALUATION.md docs/Phase2游戏能力/TASK_Phase2.md
git commit -m "docs: update Physics API and platform status"
```

---

## Final Verification Checklist

- [ ] `git status --short` is clean.
- [ ] Windows clean build passes.
- [ ] Android configure succeeds, or the local environment reason is recorded.
- [ ] `scripts/smoke/physics_p0_p1.lua` runs if a local executable is available.
- [ ] Existing old-style Physics calls still work:

```lua
local world = Light(Light.Physics.World):New()
local body = world:CreateBody("dynamic", 100, 100)
body:AddBox(32, 32)
world:Step(1 / 60)
```

- [ ] New P1-style calls work:

```lua
local shape = Light.Physics.NewCircleShape(16)
local fixture = body:CreateFixture(shape, 1.0)
fixture:SetSensor(false)
fixture:SetFriction(0.5)
```

## Plan Self-Review

- Spec coverage: P0 and P1 requirements are covered by Tasks 1 through 7.
- Out-of-scope features are listed explicitly and assigned to separate plans.
- Type names are consistent: `PhysicsWorld`, `PhysicsBody`, `PhysicsShape`, `PhysicsFixture`, `PhysicsContactEvent`.
- CMake definitions are explicit: `CHOCO_HAS_BOX2D=1` for non-Web, `CHOCO_HAS_BOX2D=0` for Web.
- No implementation step depends on Box2D v3 symbols.
