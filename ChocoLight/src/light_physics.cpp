#include "light.h"

#if CHOCO_HAS_BOX2D
#include <box2d/box2d.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static constexpr float PTM = 32.0f;

#if CHOCO_HAS_BOX2D

struct PhysicsWorld;
struct PhysicsBody;
struct PhysicsShape;
struct PhysicsFixture;
struct PhysicsJoint;  // Phase AO

enum PhysicsShapeType {
    PHYSICS_SHAPE_CIRCLE,
    PHYSICS_SHAPE_BOX,
    PHYSICS_SHAPE_POLYGON,
    PHYSICS_SHAPE_EDGE,
    PHYSICS_SHAPE_CHAIN,
};

struct PhysicsBody {
    b2Body* body;
    PhysicsWorld* owner;
    int selfRef;
    bool alive;

    PhysicsBody() : body(nullptr), owner(nullptr), selfRef(LUA_NOREF), alive(false) {}
};

struct PhysicsFixture {
    b2Fixture* fixture;
    PhysicsBody* owner;
    int selfRef;
    int userRef;
    bool alive;

    PhysicsFixture() : fixture(nullptr), owner(nullptr), selfRef(LUA_NOREF), userRef(LUA_NOREF), alive(false) {}
};

// Phase AO: Joint wrapper. Joint 类型识别用 b2Joint::GetType() 即可,无需在 wrapper 内冗余存储。
struct PhysicsJoint {
    b2Joint* joint;
    PhysicsWorld* owner;
    int selfRef;
    bool alive;

    PhysicsJoint() : joint(nullptr), owner(nullptr), selfRef(LUA_NOREF), alive(false) {}
};

struct PhysicsShape {
    PhysicsShapeType type;
    b2CircleShape circle;
    b2PolygonShape polygon;
    b2EdgeShape edge;
    b2ChainShape* chain;

    PhysicsShape() : type(PHYSICS_SHAPE_CIRCLE), chain(nullptr) {}
    ~PhysicsShape() { delete chain; }
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

// Phase AO: DestructionListener 监听 Box2D 自动销毁的 joint/fixture 事件
// 当用户调 DestroyBody 时, Box2D 自动 destroy 所有 attached joint, 这时通过
// SayGoodbye(b2Joint*) 通知, 我们标记 wrapper 为 not alive 以避免 dangling 访问。
class PhysicsDestructionListener : public b2DestructionListener {
public:
    explicit PhysicsDestructionListener(PhysicsWorld* world) : world_(world) {}
    void SayGoodbye(b2Joint* joint) override;
    void SayGoodbye(b2Fixture* fixture) override { (void)fixture; }  // fixture 由 InvalidateBody 处理

private:
    PhysicsWorld* world_;
};

struct PhysicsWorld {
    b2World* world;
    PhysicsContactListener* listener;
    PhysicsDestructionListener* destructionListener;  // Phase AO
    std::vector<PhysicsBody*> bodies;
    std::vector<PhysicsFixture*> fixtures;
    std::vector<PhysicsJoint*> joints;        // Phase AO
    std::vector<PhysicsContactEvent> contactEvents;
    int legacyCollisionRef;
    int beginContactRef;
    int endContactRef;
    lua_State* L;
    bool alive;

    PhysicsWorld()
        : world(nullptr), listener(nullptr), destructionListener(nullptr),
          legacyCollisionRef(LUA_NOREF), beginContactRef(LUA_NOREF),
          endContactRef(LUA_NOREF), L(nullptr), alive(false) {}
};

static b2Vec2 ToMeters(float x, float y) {
    return b2Vec2(x / PTM, y / PTM);
}

static void PushPixels(lua_State* L, const b2Vec2& value) {
    lua_pushnumber(L, value.x * PTM);
    lua_pushnumber(L, value.y * PTM);
}

static PhysicsWorld* CheckWorld(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* world = (PhysicsWorld*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (world && world->alive && world->world) ? world : nullptr;
}

static PhysicsBody* CheckBody(lua_State* L, int idx) {
    lua_getfield(L, idx, "__body");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* body = (PhysicsBody*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (body && body->alive && body->body) ? body : nullptr;
}

static PhysicsShape* CheckShape(lua_State* L, int idx) {
    lua_getfield(L, idx, "__shape");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* shape = (PhysicsShape*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return shape;
}

static PhysicsFixture* CheckFixture(lua_State* L, int idx) {
    lua_getfield(L, idx, "__fixture");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* fixture = (PhysicsFixture*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (fixture && fixture->alive && fixture->fixture) ? fixture : nullptr;
}

static PhysicsFixture* FixtureFromB2(b2Fixture* fixture) {
    if (!fixture) return nullptr;
    uintptr_t ptr = fixture->GetUserData().pointer;
    if (ptr == 0) return nullptr;
    auto* wrapper = reinterpret_cast<PhysicsFixture*>(ptr);
    if (!wrapper || !wrapper->alive || wrapper->fixture != fixture) return nullptr;
    return wrapper;
}

// Phase AO: Joint helpers
static PhysicsJoint* CheckJoint(lua_State* L, int idx) {
    lua_getfield(L, idx, "__joint");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* joint = (PhysicsJoint*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return (joint && joint->alive && joint->joint) ? joint : nullptr;
}

// 通过 b2Joint 反查 wrapper, 利用 b2Joint::GetUserData().pointer
static PhysicsJoint* JointFromB2(b2Joint* joint) {
    if (!joint) return nullptr;
    uintptr_t ptr = joint->GetUserData().pointer;
    if (ptr == 0) return nullptr;
    auto* wrapper = reinterpret_cast<PhysicsJoint*>(ptr);
    if (!wrapper || !wrapper->alive || wrapper->joint != joint) return nullptr;
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

static void CallLuaNoReturn(lua_State* L, int nargs) {
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Physics contact callback error: %s", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

static void InvalidateFixture(lua_State* L, PhysicsFixture* fixture, bool releaseRefs) {
    if (!fixture) return;
    if (fixture->fixture) fixture->fixture->GetUserData().pointer = 0;
    fixture->alive = false;
    fixture->fixture = nullptr;
    if (releaseRefs && fixture->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, fixture->selfRef);
        fixture->selfRef = LUA_NOREF;
    }
    if (releaseRefs && fixture->userRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, fixture->userRef);
        fixture->userRef = LUA_NOREF;
    }
}

static void InvalidateBody(lua_State* L, PhysicsBody* body, bool releaseRefs) {
    if (!body) return;
    PhysicsWorld* world = body->owner;
    if (world) {
        for (auto* fixture : world->fixtures) {
            if (fixture && fixture->owner == body) {
                InvalidateFixture(L, fixture, releaseRefs);
            }
        }
    }
    if (body->body) body->body->GetUserData().pointer = 0;  // Phase AO
    body->alive = false;
    body->body = nullptr;
    if (releaseRefs && body->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, body->selfRef);
        body->selfRef = LUA_NOREF;
    }
}

// Phase AO: Joint 失效辅助; releaseRefs 用于 GC 路径释放 Lua registry 引用
static void InvalidateJoint(lua_State* L, PhysicsJoint* joint, bool releaseRefs) {
    if (!joint) return;
    if (joint->joint) joint->joint->GetUserData().pointer = 0;
    joint->alive = false;
    joint->joint = nullptr;
    if (releaseRefs && joint->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, joint->selfRef);
        joint->selfRef = LUA_NOREF;
    }
}

// Phase AO: PhysicsDestructionListener 实现 (在所有 helper 定义后)
void PhysicsDestructionListener::SayGoodbye(b2Joint* joint) {
    if (!world_ || !world_->alive || !joint) return;
    PhysicsJoint* wrapper = JointFromB2(joint);
    if (!wrapper) return;
    // 注: Box2D 自动销毁路径; 不再手动调 b2World::DestroyJoint (会 double-free)
    // 不能在此释放 Lua registry ref (Lua 不可重入), 仅置 alive=false 并断开指针
    InvalidateJoint(world_->L, wrapper, false);
}

void PhysicsContactListener::BeginContact(b2Contact* contact) {
    if (!world_ || !world_->alive || !contact) return;
    world_->contactEvents.push_back({true, contact->GetFixtureA(), contact->GetFixtureB()});
}

void PhysicsContactListener::EndContact(b2Contact* contact) {
    if (!world_ || !world_->alive || !contact) return;
    world_->contactEvents.push_back({false, contact->GetFixtureA(), contact->GetFixtureB()});
}

/// @lua_api Light.Physics.Contact.GetBodyA
/// @brief Get the first Body in a contact event
/// @return table Body object or nil
static int l_Contact_GetBodyA(lua_State* L) {
    lua_getfield(L, 1, "__bodyA");
    return 1;
}

/// @lua_api Light.Physics.Contact.GetBodyB
/// @brief Get the second Body in a contact event
/// @return table Body object or nil
static int l_Contact_GetBodyB(lua_State* L) {
    lua_getfield(L, 1, "__bodyB");
    return 1;
}

/// @lua_api Light.Physics.Contact.GetFixtureA
/// @brief Get the first Fixture in a contact event
/// @return table Fixture object or nil
static int l_Contact_GetFixtureA(lua_State* L) {
    lua_getfield(L, 1, "__fixtureA");
    return 1;
}

/// @lua_api Light.Physics.Contact.GetFixtureB
/// @brief Get the second Fixture in a contact event
/// @return table Fixture object or nil
static int l_Contact_GetFixtureB(lua_State* L) {
    lua_getfield(L, 1, "__fixtureB");
    return 1;
}

static const luaL_Reg g_contact_funcs[] = {
    {"GetBodyA",    l_Contact_GetBodyA},
    {"GetBodyB",    l_Contact_GetBodyB},
    {"GetFixtureA", l_Contact_GetFixtureA},
    {"GetFixtureB", l_Contact_GetFixtureB},
    {NULL, NULL}
};

static void PushContactTable(lua_State* L, PhysicsBody* bodyA, PhysicsBody* bodyB, PhysicsFixture* fixtureA, PhysicsFixture* fixtureB) {
    lua_createtable(L, 0, 8);
    if (PushBodySelf(L, bodyA)) lua_setfield(L, -2, "__bodyA");
    else { lua_pushnil(L); lua_setfield(L, -2, "__bodyA"); }
    if (PushBodySelf(L, bodyB)) lua_setfield(L, -2, "__bodyB");
    else { lua_pushnil(L); lua_setfield(L, -2, "__bodyB"); }
    if (PushFixtureSelf(L, fixtureA)) lua_setfield(L, -2, "__fixtureA");
    else { lua_pushnil(L); lua_setfield(L, -2, "__fixtureA"); }
    if (PushFixtureSelf(L, fixtureB)) lua_setfield(L, -2, "__fixtureB");
    else { lua_pushnil(L); lua_setfield(L, -2, "__fixtureB"); }
    luaL_setfuncs(L, g_contact_funcs, 0);
}

static void DispatchContactEvents(lua_State* L, PhysicsWorld* world) {
    if (!world || !world->alive) return;
    std::vector<PhysicsContactEvent> events = world->contactEvents;
    world->contactEvents.clear();

    for (const auto& event : events) {
        PhysicsFixture* fixtureA = FixtureFromB2(event.fixtureA);
        PhysicsFixture* fixtureB = FixtureFromB2(event.fixtureB);
        if (!fixtureA || !fixtureB || !fixtureA->owner || !fixtureB->owner) continue;
        PhysicsBody* bodyA = fixtureA->owner;
        PhysicsBody* bodyB = fixtureB->owner;
        if (!bodyA->alive || !bodyB->alive) continue;

        if (event.begin && world->legacyCollisionRef != LUA_NOREF) {
            int baseTop = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, world->legacyCollisionRef);
            bool canCall = lua_isfunction(L, -1) && PushBodySelf(L, bodyA) && PushBodySelf(L, bodyB);
            if (canCall) CallLuaNoReturn(L, 2);
            else lua_settop(L, baseTop);
        }

        int ref = event.begin ? world->beginContactRef : world->endContactRef;
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

static int l_Shape_GC(lua_State* L) {
    auto* shape = (PhysicsShape*)lua_touserdata(L, 1);
    if (shape) shape->~PhysicsShape();
    return 0;
}

static void SetShapeGC(lua_State* L) {
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_Shape_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
}

static PhysicsShape* PushShapeTable(lua_State* L, PhysicsShapeType type) {
    lua_createtable(L, 0, 1);
    void* storage = lua_newuserdata(L, sizeof(PhysicsShape));
    auto* shape = new (storage) PhysicsShape();
    shape->type = type;
    SetShapeGC(L);
    lua_setfield(L, -2, "__shape");
    return shape;
}

/// @lua_api Light.Physics.Fixture.GetBody
/// @brief Get the Body that owns this Fixture
/// @return table Body object or nil
static int l_Fixture_GetBody(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture && PushBodySelf(L, fixture->owner)) return 1;
    lua_pushnil(L);
    return 1;
}

/// @lua_api Light.Physics.Fixture.SetDensity
/// @brief Set Fixture density and refresh mass data
/// @param value number Density
static int l_Fixture_SetDensity(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) {
        fixture->fixture->SetDensity((float)luaL_checknumber(L, 2));
        if (fixture->owner && fixture->owner->body) fixture->owner->body->ResetMassData();
    }
    return 0;
}

/// @lua_api Light.Physics.Fixture.GetDensity
/// @brief Get Fixture density
/// @return number Density
static int l_Fixture_GetDensity(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushnumber(L, fixture ? fixture->fixture->GetDensity() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Fixture.SetFriction
/// @brief Set Fixture friction
/// @param value number Friction
static int l_Fixture_SetFriction(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) fixture->fixture->SetFriction((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Fixture.GetFriction
/// @brief Get Fixture friction
/// @return number Friction
static int l_Fixture_GetFriction(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushnumber(L, fixture ? fixture->fixture->GetFriction() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Fixture.SetRestitution
/// @brief Set Fixture restitution
/// @param value number Restitution
static int l_Fixture_SetRestitution(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) fixture->fixture->SetRestitution((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Fixture.GetRestitution
/// @brief Get Fixture restitution
/// @return number Restitution
static int l_Fixture_GetRestitution(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushnumber(L, fixture ? fixture->fixture->GetRestitution() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Fixture.SetSensor
/// @brief Set whether this Fixture is a sensor
/// @param enabled boolean Sensor flag
static int l_Fixture_SetSensor(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) fixture->fixture->SetSensor(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Fixture.IsSensor
/// @brief Check whether this Fixture is a sensor
/// @return boolean Sensor flag
static int l_Fixture_IsSensor(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushboolean(L, fixture && fixture->fixture->IsSensor());
    return 1;
}

/// @lua_api Light.Physics.Fixture.SetFilterData
/// @brief Set collision filter data
/// @param categoryBits number Category bits
/// @param maskBits number Mask bits
/// @param groupIndex number Group index
static int l_Fixture_SetFilterData(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) return 0;
    b2Filter filter;
    filter.categoryBits = (uint16)luaL_checkinteger(L, 2);
    filter.maskBits = (uint16)luaL_checkinteger(L, 3);
    filter.groupIndex = (int16)luaL_optinteger(L, 4, 0);
    fixture->fixture->SetFilterData(filter);
    return 0;
}

/// @lua_api Light.Physics.Fixture.GetFilterData
/// @brief Get collision filter data
/// @return number categoryBits, maskBits, groupIndex
static int l_Fixture_GetFilterData(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) {
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
        return 3;
    }
    b2Filter filter = fixture->fixture->GetFilterData();
    lua_pushinteger(L, filter.categoryBits);
    lua_pushinteger(L, filter.maskBits);
    lua_pushinteger(L, filter.groupIndex);
    return 3;
}

/// @lua_api Light.Physics.Fixture.SetUserData
/// @brief Attach Lua user data to this Fixture
/// @param value any User data, nil clears it
static int l_Fixture_SetUserData(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) return 0;
    if (fixture->userRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, fixture->userRef);
        fixture->userRef = LUA_NOREF;
    }
    if (!lua_isnil(L, 2)) {
        lua_pushvalue(L, 2);
        fixture->userRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

/// @lua_api Light.Physics.Fixture.GetUserData
/// @brief Get Lua user data attached to this Fixture
/// @return any User data or nil
static int l_Fixture_GetUserData(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture || fixture->userRef == LUA_NOREF) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, fixture->userRef);
    return 1;
}

/// @lua_api Light.Physics.Fixture.TestPoint
/// @brief Phase AO: Test whether a world-space point (pixels) is inside the Fixture
/// @param px number World X in pixels
/// @param py number World Y in pixels
/// @return boolean True when inside
static int l_Fixture_TestPoint(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) { lua_pushboolean(L, 0); return 1; }
    float px = (float)luaL_checknumber(L, 2);
    float py = (float)luaL_checknumber(L, 3);
    lua_pushboolean(L, fixture->fixture->TestPoint(ToMeters(px, py)));
    return 1;
}

/// @lua_api Light.Physics.Fixture.GetAABB
/// @brief Phase AO: Get fixture AABB for first proxy, in pixels
/// @return number x, y, w, h in pixels
static int l_Fixture_GetAABB(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 4;
    }
    const b2AABB& aabb = fixture->fixture->GetAABB(0);
    lua_pushnumber(L, aabb.lowerBound.x * PTM);
    lua_pushnumber(L, aabb.lowerBound.y * PTM);
    lua_pushnumber(L, (aabb.upperBound.x - aabb.lowerBound.x) * PTM);
    lua_pushnumber(L, (aabb.upperBound.y - aabb.lowerBound.y) * PTM);
    return 4;
}

/// @lua_api Light.Physics.Fixture.GetShapeType
/// @brief Phase AO: Get underlying shape type string: "circle"|"polygon"|"edge"|"chain"
/// @return string Shape type
static int l_Fixture_GetShapeType(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) { lua_pushnil(L); return 1; }
    b2Shape* shape = fixture->fixture->GetShape();
    if (!shape) { lua_pushnil(L); return 1; }
    switch (shape->GetType()) {
    case b2Shape::e_circle:  lua_pushstring(L, "circle"); break;
    case b2Shape::e_polygon: lua_pushstring(L, "polygon"); break;
    case b2Shape::e_edge:    lua_pushstring(L, "edge"); break;
    case b2Shape::e_chain:   lua_pushstring(L, "chain"); break;
    default:                 lua_pushstring(L, "unknown"); break;
    }
    return 1;
}

static const luaL_Reg g_fixture_funcs[] = {
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
    {"TestPoint",      l_Fixture_TestPoint},    // Phase AO
    {"GetAABB",        l_Fixture_GetAABB},      // Phase AO
    {"GetShapeType",   l_Fixture_GetShapeType}, // Phase AO
    {NULL, NULL}
};

static PhysicsFixture* PushFixtureTable(lua_State* L, PhysicsBody* body, b2Fixture* fixture) {
    lua_createtable(L, 0, 16);
    void* storage = lua_newuserdata(L, sizeof(PhysicsFixture));
    auto* wrapper = new (storage) PhysicsFixture();
    wrapper->fixture = fixture;
    wrapper->owner = body;
    wrapper->alive = true;
    lua_setfield(L, -2, "__fixture");
    luaL_setfuncs(L, g_fixture_funcs, 0);
    lua_pushvalue(L, -1);
    wrapper->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    fixture->GetUserData().pointer = reinterpret_cast<uintptr_t>(wrapper);
    body->owner->fixtures.push_back(wrapper);
    return wrapper;
}

static b2Shape* ShapePointer(PhysicsShape* shape) {
    if (!shape) return nullptr;
    switch (shape->type) {
    case PHYSICS_SHAPE_CIRCLE: return &shape->circle;
    case PHYSICS_SHAPE_BOX:
    case PHYSICS_SHAPE_POLYGON: return &shape->polygon;
    case PHYSICS_SHAPE_EDGE: return &shape->edge;
    case PHYSICS_SHAPE_CHAIN: return shape->chain;
    default: return nullptr;
    }
}

static int CreateFixtureFromShape(lua_State* L, PhysicsBody* body, PhysicsShape* shape, float density) {
    if (!body || !shape) return 0;
    b2Shape* rawShape = ShapePointer(shape);
    if (!rawShape) {
        lua_pushnil(L);
        return 1;
    }

    b2FixtureDef fixtureDef;
    fixtureDef.shape = rawShape;
    fixtureDef.density = density;
    fixtureDef.friction = 0.2f;
    fixtureDef.restitution = 0.0f;

    b2Fixture* fixture = body->body->CreateFixture(&fixtureDef);
    if (!fixture) {
        lua_pushnil(L);
        return 1;
    }
    PushFixtureTable(L, body, fixture);
    return 1;
}

/// @lua_api Light.Physics.Body.CreateFixture
/// @brief Create a Fixture from a Shape
/// @param shape table Shape object
/// @param density number Density, defaults to 1.0
/// @return table Fixture object
static int l_Body_CreateFixture(lua_State* L) {
    auto* body = CheckBody(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* shape = CheckShape(L, 2);
    float density = (float)luaL_optnumber(L, 3, 1.0);
    if (!body || !shape) return 0;
    return CreateFixtureFromShape(L, body, shape, density);
}

/// @lua_api Light.Physics.Body.DestroyFixture
/// @brief Destroy a Fixture on this Body
/// @param fixture table Fixture object
static int l_Body_DestroyFixture(lua_State* L) {
    auto* body = CheckBody(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* fixture = CheckFixture(L, 2);
    if (!body || !fixture || fixture->owner != body) return 0;
    b2Fixture* raw = fixture->fixture;
    InvalidateFixture(L, fixture, false);
    if (raw) body->body->DestroyFixture(raw);
    body->body->ResetMassData();
    return 0;
}

/// @lua_api Light.Physics.Body.AddBox
/// @brief Compatibility API that adds a rectangle Fixture
/// @param width number Width in pixels
/// @param height number Height in pixels
/// @return table Fixture object
static int l_Body_AddBox(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    PhysicsShape shape;
    shape.type = PHYSICS_SHAPE_BOX;
    float width = (float)luaL_checknumber(L, 2) / PTM;
    float height = (float)luaL_checknumber(L, 3) / PTM;
    shape.polygon.SetAsBox(width * 0.5f, height * 0.5f);
    return CreateFixtureFromShape(L, body, &shape, 1.0f);
}

/// @lua_api Light.Physics.Body.AddCircle
/// @brief Compatibility API that adds a circle Fixture
/// @param radius number Radius in pixels
/// @return table Fixture object
static int l_Body_AddCircle(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    PhysicsShape shape;
    shape.type = PHYSICS_SHAPE_CIRCLE;
    shape.circle.m_radius = (float)luaL_checknumber(L, 2) / PTM;
    shape.circle.m_p.Set(0.0f, 0.0f);
    return CreateFixtureFromShape(L, body, &shape, 1.0f);
}

static b2BodyType ParseBodyType(const char* typeStr) {
    if (strcmp(typeStr, "dynamic") == 0) return b2_dynamicBody;
    if (strcmp(typeStr, "kinematic") == 0) return b2_kinematicBody;
    return b2_staticBody;
}

static const char* BodyTypeName(b2BodyType type) {
    switch (type) {
    case b2_dynamicBody: return "dynamic";
    case b2_kinematicBody: return "kinematic";
    default: return "static";
    }
}

/// @lua_api Light.Physics.Body.GetType
/// @brief Get Body type
/// @return string static, dynamic, or kinematic
static int l_Body_GetType(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushstring(L, body ? BodyTypeName(body->body->GetType()) : "static");
    return 1;
}

/// @lua_api Light.Physics.Body.SetType
/// @brief Set Body type
/// @param type string static, dynamic, or kinematic
static int l_Body_SetType(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetType(ParseBodyType(luaL_checkstring(L, 2)));
    return 0;
}

/// @lua_api Light.Physics.Body.GetPosition
/// @brief Get Body position in pixels
/// @return number x, y
static int l_Body_GetPosition(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    PushPixels(L, body->body->GetPosition());
    return 2;
}

/// @lua_api Light.Physics.Body.SetPosition
/// @brief Set Body position in pixels
/// @param x number X coordinate
/// @param y number Y coordinate
static int l_Body_SetPosition(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    body->body->SetTransform(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)), body->body->GetAngle());
    return 0;
}

/// @lua_api Light.Physics.Body.GetAngle
/// @brief Get Body angle
/// @return number Angle in radians
static int l_Body_GetAngle(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetAngle() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Body.SetAngle
/// @brief Set Body angle
/// @param angle number Angle in radians
static int l_Body_SetAngle(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetTransform(body->body->GetPosition(), (float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Body.GetLinearVelocity
/// @brief Get linear velocity
/// @return number vx, vy in pixels per second
static int l_Body_GetLinearVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    PushPixels(L, body->body->GetLinearVelocity());
    return 2;
}

/// @lua_api Light.Physics.Body.SetLinearVelocity
/// @brief Set linear velocity
/// @param vx number X velocity in pixels per second
/// @param vy number Y velocity in pixels per second
static int l_Body_SetLinearVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    body->body->SetLinearVelocity(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)));
    return 0;
}

/// @lua_api Light.Physics.Body.GetAngularVelocity
/// @brief Get angular velocity
/// @return number Angular velocity
static int l_Body_GetAngularVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetAngularVelocity() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Body.SetAngularVelocity
/// @brief Set angular velocity
/// @param value number Angular velocity
static int l_Body_SetAngularVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetAngularVelocity((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Body.ApplyForce
/// @brief Apply force to the Body center
/// @param fx number X force
/// @param fy number Y force
static int l_Body_ApplyForce(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ApplyForceToCenter(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)), true);
    return 0;
}

/// @lua_api Light.Physics.Body.ApplyImpulse
/// @brief Apply linear impulse to the Body center
/// @param ix number X impulse
/// @param iy number Y impulse
static int l_Body_ApplyImpulse(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ApplyLinearImpulseToCenter(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)), true);
    return 0;
}

/// @lua_api Light.Physics.Body.ApplyTorque
/// @brief Apply torque to this Body
/// @param torque number Torque
static int l_Body_ApplyTorque(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ApplyTorque((float)luaL_checknumber(L, 2), true);
    return 0;
}

/// @lua_api Light.Physics.Body.SetAwake
/// @brief Set whether this Body is awake
/// @param enabled boolean Awake flag
static int l_Body_SetAwake(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetAwake(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Body.IsAwake
/// @brief Check whether this Body is awake
/// @return boolean Awake flag
static int l_Body_IsAwake(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsAwake());
    return 1;
}

/// @lua_api Light.Physics.Body.SetActive
/// @brief Set whether this Body is enabled
/// @param enabled boolean Enabled flag
static int l_Body_SetActive(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetEnabled(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Body.IsActive
/// @brief Check whether this Body is enabled
/// @return boolean Enabled flag
static int l_Body_IsActive(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsEnabled());
    return 1;
}

/// @lua_api Light.Physics.Body.SetBullet
/// @brief Enable or disable bullet continuous collision mode
/// @param enabled boolean Bullet flag
static int l_Body_SetBullet(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetBullet(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Body.IsBullet
/// @brief Check whether this Body uses bullet mode
/// @return boolean Bullet flag
static int l_Body_IsBullet(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsBullet());
    return 1;
}

/// @lua_api Light.Physics.Body.SetLinearDamping
/// @brief Set linear damping
/// @param value number Linear damping
static int l_Body_SetLinearDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetLinearDamping((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Body.GetLinearDamping
/// @brief Get linear damping
/// @return number Linear damping
static int l_Body_GetLinearDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetLinearDamping() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Body.SetAngularDamping
/// @brief Set angular damping
/// @param value number Angular damping
static int l_Body_SetAngularDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetAngularDamping((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Body.GetAngularDamping
/// @brief Get angular damping
/// @return number Angular damping
static int l_Body_GetAngularDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetAngularDamping() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Body.GetMass
/// @brief Get Body mass
/// @return number Mass
static int l_Body_GetMass(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetMass() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Body.GetInertia
/// @brief Get Body rotational inertia
/// @return number Rotational inertia
static int l_Body_GetInertia(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetInertia() : 0.0f);
    return 1;
}

/// @lua_api Light.Physics.Body.GetWorldCenter
/// @brief Get Body world center
/// @return number x, y in pixels
static int l_Body_GetWorldCenter(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    PushPixels(L, body->body->GetWorldCenter());
    return 2;
}

/// @lua_api Light.Physics.Body.GetLocalCenter
/// @brief Get Body local center
/// @return number x, y in pixels
static int l_Body_GetLocalCenter(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    PushPixels(L, body->body->GetLocalCenter());
    return 2;
}

/// @lua_api Light.Physics.Body.SetRestitution
/// @brief Set restitution for all current Fixtures on this Body
/// @param value number Restitution
static int l_Body_SetRestitution(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    float value = (float)luaL_checknumber(L, 2);
    for (b2Fixture* fixture = body->body->GetFixtureList(); fixture; fixture = fixture->GetNext()) fixture->SetRestitution(value);
    return 0;
}

/// @lua_api Light.Physics.Body.SetFriction
/// @brief Set friction for all current Fixtures on this Body
/// @param value number Friction
static int l_Body_SetFriction(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    float value = (float)luaL_checknumber(L, 2);
    for (b2Fixture* fixture = body->body->GetFixtureList(); fixture; fixture = fixture->GetNext()) fixture->SetFriction(value);
    return 0;
}

/// @lua_api Light.Physics.Body.GetWorldPoint
/// @brief Phase AO: Convert local-space point (pixels) to world-space (pixels)
/// @param lx number Local X in pixels
/// @param ly number Local Y in pixels
/// @return number wx, wy in pixels
static int l_Body_GetWorldPoint(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    float lx = (float)luaL_checknumber(L, 2);
    float ly = (float)luaL_checknumber(L, 3);
    PushPixels(L, body->body->GetWorldPoint(ToMeters(lx, ly)));
    return 2;
}

/// @lua_api Light.Physics.Body.GetLocalPoint
/// @brief Phase AO: Convert world-space point (pixels) to local-space (pixels)
/// @param wx number World X in pixels
/// @param wy number World Y in pixels
/// @return number lx, ly in pixels
static int l_Body_GetLocalPoint(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    float wx = (float)luaL_checknumber(L, 2);
    float wy = (float)luaL_checknumber(L, 3);
    PushPixels(L, body->body->GetLocalPoint(ToMeters(wx, wy)));
    return 2;
}

/// @lua_api Light.Physics.Body.GetWorldVector
/// @brief Phase AO: Rotate a local-space vector (pixels) into world-space (pixels)
/// @param lx number Local X in pixels
/// @param ly number Local Y in pixels
/// @return number wx, wy in pixels
static int l_Body_GetWorldVector(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    float lx = (float)luaL_checknumber(L, 2);
    float ly = (float)luaL_checknumber(L, 3);
    PushPixels(L, body->body->GetWorldVector(ToMeters(lx, ly)));
    return 2;
}

/// @lua_api Light.Physics.Body.GetLocalVector
/// @brief Phase AO: Rotate a world-space vector (pixels) into local-space (pixels)
/// @param wx number World X in pixels
/// @param wy number World Y in pixels
/// @return number lx, ly in pixels
static int l_Body_GetLocalVector(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    float wx = (float)luaL_checknumber(L, 2);
    float wy = (float)luaL_checknumber(L, 3);
    PushPixels(L, body->body->GetLocalVector(ToMeters(wx, wy)));
    return 2;
}

/// @lua_api Light.Physics.Body.ApplyForceAtWorldPoint
/// @brief Phase AO: Apply a world-space force at a world-space point (pixels)
/// @param fx number Force X (Newtons-like)
/// @param fy number Force Y
/// @param px number World X in pixels
/// @param py number World Y in pixels
/// @param wake boolean Wake up the body (defaults true)
static int l_Body_ApplyForceAtWorldPoint(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    float fx = (float)luaL_checknumber(L, 2);
    float fy = (float)luaL_checknumber(L, 3);
    float px = (float)luaL_checknumber(L, 4);
    float py = (float)luaL_checknumber(L, 5);
    bool wake = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    // 注: Box2D 对力的单位不做像素换算, 仅对位置
    body->body->ApplyForce(b2Vec2(fx, fy), ToMeters(px, py), wake);
    return 0;
}

/// @lua_api Light.Physics.Body.ApplyLinearImpulseAtPoint
/// @brief Phase AO: Apply impulse at a world-space point (pixels)
/// @param ix number Impulse X
/// @param iy number Impulse Y
/// @param px number World X in pixels
/// @param py number World Y in pixels
/// @param wake boolean Wake up the body (defaults true)
static int l_Body_ApplyLinearImpulseAtPoint(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    float ix = (float)luaL_checknumber(L, 2);
    float iy = (float)luaL_checknumber(L, 3);
    float px = (float)luaL_checknumber(L, 4);
    float py = (float)luaL_checknumber(L, 5);
    bool wake = lua_isnoneornil(L, 6) ? true : (lua_toboolean(L, 6) != 0);
    body->body->ApplyLinearImpulse(b2Vec2(ix, iy), ToMeters(px, py), wake);
    return 0;
}

/// @lua_api Light.Physics.Body.GetFixtures
/// @brief Phase AO: Return an array of all Fixture tables attached to this Body (alive only)
/// @return table Array of Fixture tables
static int l_Body_GetFixtures(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_newtable(L);
    if (!body) return 1;
    int idx = 1;
    for (b2Fixture* raw = body->body->GetFixtureList(); raw; raw = raw->GetNext()) {
        PhysicsFixture* wrapper = FixtureFromB2(raw);
        if (!wrapper) continue;
        if (!PushFixtureSelf(L, wrapper)) continue;
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static const luaL_Reg g_body_funcs[] = {
    {"AddBox",             l_Body_AddBox},
    {"AddCircle",          l_Body_AddCircle},
    {"GetType",            l_Body_GetType},
    {"SetType",            l_Body_SetType},
    {"GetPosition",        l_Body_GetPosition},
    {"SetPosition",        l_Body_SetPosition},
    {"GetAngle",           l_Body_GetAngle},
    {"SetAngle",           l_Body_SetAngle},
    {"GetLinearVelocity",  l_Body_GetLinearVelocity},
    {"SetLinearVelocity",  l_Body_SetLinearVelocity},
    {"GetAngularVelocity", l_Body_GetAngularVelocity},
    {"SetAngularVelocity", l_Body_SetAngularVelocity},
    {"ApplyForce",         l_Body_ApplyForce},
    {"ApplyImpulse",       l_Body_ApplyImpulse},
    {"ApplyTorque",        l_Body_ApplyTorque},
    {"SetAwake",           l_Body_SetAwake},
    {"IsAwake",            l_Body_IsAwake},
    {"SetActive",          l_Body_SetActive},
    {"IsActive",           l_Body_IsActive},
    {"SetBullet",          l_Body_SetBullet},
    {"IsBullet",           l_Body_IsBullet},
    {"SetLinearDamping",   l_Body_SetLinearDamping},
    {"GetLinearDamping",   l_Body_GetLinearDamping},
    {"SetAngularDamping",  l_Body_SetAngularDamping},
    {"GetAngularDamping",  l_Body_GetAngularDamping},
    {"CreateFixture",      l_Body_CreateFixture},
    {"DestroyFixture",     l_Body_DestroyFixture},
    {"GetMass",            l_Body_GetMass},
    {"GetInertia",         l_Body_GetInertia},
    {"GetWorldCenter",     l_Body_GetWorldCenter},
    {"GetLocalCenter",     l_Body_GetLocalCenter},
    {"SetRestitution",     l_Body_SetRestitution},
    {"SetFriction",        l_Body_SetFriction},
    // Phase AO
    {"GetWorldPoint",                 l_Body_GetWorldPoint},
    {"GetLocalPoint",                 l_Body_GetLocalPoint},
    {"GetWorldVector",                l_Body_GetWorldVector},
    {"GetLocalVector",                l_Body_GetLocalVector},
    {"ApplyForceAtWorldPoint",        l_Body_ApplyForceAtWorldPoint},
    {"ApplyLinearImpulseAtPoint",     l_Body_ApplyLinearImpulseAtPoint},
    {"GetFixtures",                   l_Body_GetFixtures},
    {NULL, NULL}
};

static int l_World_GC(lua_State* L) {
    auto* world = (PhysicsWorld*)lua_touserdata(L, 1);
    if (!world || !world->alive) return 0;

    // Phase AO: 先失效 joints (在 body/world 销毁前, 释放 registry ref)
    for (auto* joint : world->joints) InvalidateJoint(L, joint, true);
    for (auto* body : world->bodies) InvalidateBody(L, body, true);
    if (world->legacyCollisionRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->legacyCollisionRef);
    if (world->beginContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->beginContactRef);
    if (world->endContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->endContactRef);
    delete world->listener;
    delete world->destructionListener;  // Phase AO
    delete world->world;
    world->listener = nullptr;
    world->destructionListener = nullptr;
    world->world = nullptr;
    world->alive = false;
    world->~PhysicsWorld();
    return 0;
}

static void SetWorldGC(lua_State* L) {
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_World_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
}

/// @lua_api Light.Physics.World.New
/// @brief Create a Box2D physics World instance
static int l_World_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    void* storage = lua_newuserdata(L, sizeof(PhysicsWorld));
    auto* world = new (storage) PhysicsWorld();
    world->world = new b2World(b2Vec2(0.0f, 10.0f));
    world->listener = new PhysicsContactListener(world);
    world->destructionListener = new PhysicsDestructionListener(world);  // Phase AO
    world->world->SetContactListener(world->listener);
    world->world->SetDestructionListener(world->destructionListener);
    world->L = L;
    world->alive = true;
    SetWorldGC(L);
    lua_setfield(L, 1, "__instance");
    return 0;
}

/// @lua_api Light.Physics.World.SetGravity
/// @brief Set world gravity
/// @param gx number X gravity in pixels per second squared
/// @param gy number Y gravity in pixels per second squared
static int l_World_SetGravity(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetGravity(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)));
    return 0;
}

/// @lua_api Light.Physics.World.GetGravity
/// @brief Get world gravity
/// @return number gx, gy in pixels per second squared
static int l_World_GetGravity(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    PushPixels(L, world->world->GetGravity());
    return 2;
}

/// @lua_api Light.Physics.World.Step
/// @brief Step the physics world and dispatch queued contact events
/// @param dt number Time step in seconds
static int l_World_Step(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    world->world->Step((float)luaL_checknumber(L, 2), 8, 3);
    DispatchContactEvents(L, world);
    return 0;
}

/// @lua_api Light.Physics.World.ClearForces
/// @brief Clear accumulated world forces
static int l_World_ClearForces(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->ClearForces();
    return 0;
}

/// @lua_api Light.Physics.World.SetAllowSleeping
/// @brief Set whether bodies may sleep
/// @param enabled boolean Allow-sleep flag
static int l_World_SetAllowSleeping(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetAllowSleeping(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.World.SetContinuousPhysics
/// @brief Set whether continuous physics is enabled
/// @param enabled boolean Continuous-physics flag
static int l_World_SetContinuousPhysics(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetContinuousPhysics(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.World.CreateBody
/// @brief Create a Body
/// @param type string static, dynamic, or kinematic
/// @param x number X position in pixels
/// @param y number Y position in pixels
/// @return table Body object
static int l_World_CreateBody(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    b2BodyDef bodyDef;
    bodyDef.type = ParseBodyType(luaL_checkstring(L, 2));
    bodyDef.position = ToMeters((float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    b2Body* body = world->world->CreateBody(&bodyDef);
    if (!body) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 16);
    void* storage = lua_newuserdata(L, sizeof(PhysicsBody));
    auto* wrapper = new (storage) PhysicsBody();
    wrapper->body = body;
    wrapper->owner = world;
    wrapper->alive = true;
    lua_setfield(L, -2, "__body");
    luaL_setfuncs(L, g_body_funcs, 0);
    lua_pushvalue(L, -1);
    wrapper->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    // Phase AO: 反向链接 b2Body -> wrapper, 用于 Joint.GetBodyA/B
    body->GetUserData().pointer = reinterpret_cast<uintptr_t>(wrapper);
    world->bodies.push_back(wrapper);
    return 1;
}

/// @lua_api Light.Physics.World.DestroyBody
/// @brief Destroy a Body
/// @param body table Body object
static int l_World_DestroyBody(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* body = CheckBody(L, 2);
    if (!world || !body || body->owner != world) return 0;
    b2Body* raw = body->body;
    InvalidateBody(L, body, false);
    if (raw) world->world->DestroyBody(raw);
    return 0;
}

/// @lua_api Light.Physics.World.GetBodyCount
/// @brief Get the current number of live bodies
/// @return number Body count
static int l_World_GetBodyCount(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    int count = 0;
    if (world) {
        for (auto* body : world->bodies) if (body && body->alive) count++;
    }
    lua_pushinteger(L, count);
    return 1;
}

/// @lua_api Light.Physics.World.OnCollision
/// @brief Register the legacy collision callback
/// @param callback function Callback with bodyA, bodyB arguments
static int l_World_OnCollision(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (world->legacyCollisionRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->legacyCollisionRef);
    lua_pushvalue(L, 2);
    world->legacyCollisionRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

/// @lua_api Light.Physics.World.BeginContact
/// @brief Register the BeginContact callback
/// @param callback function Callback with a Contact object
static int l_World_BeginContact(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (world->beginContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->beginContactRef);
    lua_pushvalue(L, 2);
    world->beginContactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

/// @lua_api Light.Physics.World.EndContact
/// @brief Register the EndContact callback
/// @param callback function Callback with a Contact object
static int l_World_EndContact(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (world->endContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->endContactRef);
    lua_pushvalue(L, 2);
    world->endContactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

// ============================================================
// Phase AO: Joint bindings, PushJointTable + common ops
// ============================================================

static bool PushJointSelf(lua_State* L, PhysicsJoint* joint) {
    if (!joint || !joint->alive || joint->selfRef == LUA_NOREF) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, joint->selfRef);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

/// @lua_api Light.Physics.Joint.IsAlive
/// @brief Phase AO: True when the underlying b2Joint has not been destroyed
/// @return boolean Alive flag
static int l_Joint_IsAlive(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    lua_pushboolean(L, joint != nullptr);
    return 1;
}

static const char* JointTypeName(b2JointType type) {
    switch (type) {
    case e_revoluteJoint:  return "revolute";
    case e_prismaticJoint: return "prismatic";
    case e_distanceJoint:  return "distance";
    case e_weldJoint:      return "weld";
    case e_mouseJoint:     return "mouse";
    case e_ropeJoint:      return "rope";
    case e_pulleyJoint:    return "pulley";
    case e_wheelJoint:     return "wheel";
    case e_frictionJoint:  return "friction";
    case e_motorJoint:     return "motor";
    case e_gearJoint:      return "gear";
    default:               return "unknown";
    }
}

/// @lua_api Light.Physics.Joint.GetType
/// @brief Phase AO: Joint kind string
/// @return string Type name: "distance"|"revolute"|"prismatic"|"weld"|"mouse"|...
static int l_Joint_GetType(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnil(L); return 1; }
    lua_pushstring(L, JointTypeName(joint->joint->GetType()));
    return 1;
}

/// @lua_api Light.Physics.Joint.GetBodyA
/// @brief Phase AO: Get the first Body attached to this joint (or nil)
static int l_Joint_GetBodyA(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnil(L); return 1; }
    b2Body* body = joint->joint->GetBodyA();
    if (!body) { lua_pushnil(L); return 1; }
    auto* wrapper = reinterpret_cast<PhysicsBody*>(body->GetUserData().pointer);
    if (!PushBodySelf(L, wrapper)) lua_pushnil(L);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetBodyB
/// @brief Phase AO: Get the second Body attached to this joint (or nil)
static int l_Joint_GetBodyB(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnil(L); return 1; }
    b2Body* body = joint->joint->GetBodyB();
    if (!body) { lua_pushnil(L); return 1; }
    auto* wrapper = reinterpret_cast<PhysicsBody*>(body->GetUserData().pointer);
    if (!PushBodySelf(L, wrapper)) lua_pushnil(L);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetAnchorA
/// @brief Phase AO: World-space anchor on Body A in pixels
/// @return number x, y in pixels
static int l_Joint_GetAnchorA(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    PushPixels(L, joint->joint->GetAnchorA());
    return 2;
}

/// @lua_api Light.Physics.Joint.GetAnchorB
/// @brief Phase AO: World-space anchor on Body B in pixels
/// @return number x, y in pixels
static int l_Joint_GetAnchorB(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    PushPixels(L, joint->joint->GetAnchorB());
    return 2;
}

/// @lua_api Light.Physics.Joint.GetReactionForce
/// @brief Phase AO: Reaction force from the last time step scaled by 1/dt
/// @param invDt number Inverse delta time (1/dt)
/// @return number fx, fy
static int l_Joint_GetReactionForce(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    float invDt = (float)luaL_optnumber(L, 2, 60.0);
    b2Vec2 f = joint->joint->GetReactionForce(invDt);
    lua_pushnumber(L, f.x);
    lua_pushnumber(L, f.y);
    return 2;
}

/// @lua_api Light.Physics.Joint.GetReactionTorque
/// @brief Phase AO: Reaction torque from the last time step scaled by 1/dt
/// @param invDt number Inverse delta time (1/dt)
/// @return number Torque value
static int l_Joint_GetReactionTorque(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float invDt = (float)luaL_optnumber(L, 2, 60.0);
    lua_pushnumber(L, joint->joint->GetReactionTorque(invDt));
    return 1;
}

/// @lua_api Light.Physics.Joint.IsEnabled
/// @brief Phase AO: True when both attached bodies are enabled
/// @return boolean Enabled flag
static int l_Joint_IsEnabled(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    lua_pushboolean(L, joint && joint->joint->IsEnabled());
    return 1;
}

/// @lua_api Light.Physics.Joint.Destroy
/// @brief Phase AO: Manually destroy this joint (equivalent to World:DestroyJoint)
static int l_Joint_Destroy(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    PhysicsWorld* world = joint->owner;
    if (!world || !world->alive || !world->world) return 0;
    b2Joint* raw = joint->joint;
    InvalidateJoint(L, joint, true);   // 先 invalidate 防止 SayGoodbye 再次触发
    world->world->DestroyJoint(raw);
    return 0;
}

// ---------- Type-specific Joint getters ----------

static b2RevoluteJoint* CheckRevoluteJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_revoluteJoint) return nullptr;
    return static_cast<b2RevoluteJoint*>(joint->joint);
}

static b2PrismaticJoint* CheckPrismaticJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_prismaticJoint) return nullptr;
    return static_cast<b2PrismaticJoint*>(joint->joint);
}

static b2DistanceJoint* CheckDistanceJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_distanceJoint) return nullptr;
    return static_cast<b2DistanceJoint*>(joint->joint);
}

static b2MouseJoint* CheckMouseJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_mouseJoint) return nullptr;
    return static_cast<b2MouseJoint*>(joint->joint);
}

/// @lua_api Light.Physics.Joint.GetJointAngle
/// @brief Phase AO: Revolute joint: current angle in radians (returns 0 for other types)
static int l_Joint_GetJointAngle(lua_State* L) {
    auto* rj = CheckRevoluteJoint(L, 1);
    lua_pushnumber(L, rj ? rj->GetJointAngle() : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetJointSpeed
/// @brief Phase AO: Revolute joint: current angular speed in rad/s (returns 0 for other types)
static int l_Joint_GetJointSpeed(lua_State* L) {
    auto* rj = CheckRevoluteJoint(L, 1);
    lua_pushnumber(L, rj ? rj->GetJointSpeed() : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetJointTranslation
/// @brief Phase AO: Prismatic joint: current translation in pixels (returns 0 for other types)
static int l_Joint_GetJointTranslation(lua_State* L) {
    auto* pj = CheckPrismaticJoint(L, 1);
    lua_pushnumber(L, pj ? (pj->GetJointTranslation() * PTM) : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetMotorSpeed
/// @brief Phase AO: Set motor speed (revolute/prismatic joints only)
/// @param speed number Motor speed
static int l_Joint_SetMotorSpeed(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float speed = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  static_cast<b2RevoluteJoint*>(joint->joint)->SetMotorSpeed(speed); break;
    case e_prismaticJoint: static_cast<b2PrismaticJoint*>(joint->joint)->SetMotorSpeed(speed); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.EnableMotor
/// @brief Phase AO: Enable/disable motor (revolute/prismatic joints only)
/// @param enabled boolean Enabled flag
static int l_Joint_EnableMotor(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    bool enabled = lua_toboolean(L, 2) != 0;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  static_cast<b2RevoluteJoint*>(joint->joint)->EnableMotor(enabled); break;
    case e_prismaticJoint: static_cast<b2PrismaticJoint*>(joint->joint)->EnableMotor(enabled); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.SetTarget
/// @brief Phase AO: Mouse joint only: set target world-space point (pixels)
/// @param x number World X in pixels
/// @param y number World Y in pixels
static int l_Joint_SetTarget(lua_State* L) {
    auto* mj = CheckMouseJoint(L, 1);
    if (!mj) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    mj->SetTarget(ToMeters(x, y));
    return 0;
}

/// @lua_api Light.Physics.Joint.SetLength
/// @brief Phase AO: Distance joint only: set rest length in pixels
/// @param length number New length in pixels
static int l_Joint_SetLength(lua_State* L) {
    auto* dj = CheckDistanceJoint(L, 1);
    if (!dj) return 0;
    dj->SetLength((float)luaL_checknumber(L, 2) / PTM);
    return 0;
}

/// @lua_api Light.Physics.Joint.GetLength
/// @brief Phase AO: Distance joint only: get rest length in pixels (returns 0 for other types)
static int l_Joint_GetLength(lua_State* L) {
    auto* dj = CheckDistanceJoint(L, 1);
    lua_pushnumber(L, dj ? (dj->GetLength() * PTM) : 0.0);
    return 1;
}

static int l_Joint_Tostring(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushstring(L, "Light.Physics.Joint(dead)"); return 1; }
    lua_pushfstring(L, "Light.Physics.Joint(%s)", JointTypeName(joint->joint->GetType()));
    return 1;
}

static const luaL_Reg g_joint_funcs[] = {
    {"IsAlive",           l_Joint_IsAlive},
    {"GetType",           l_Joint_GetType},
    {"GetBodyA",          l_Joint_GetBodyA},
    {"GetBodyB",          l_Joint_GetBodyB},
    {"GetAnchorA",        l_Joint_GetAnchorA},
    {"GetAnchorB",        l_Joint_GetAnchorB},
    {"GetReactionForce",  l_Joint_GetReactionForce},
    {"GetReactionTorque", l_Joint_GetReactionTorque},
    {"IsEnabled",         l_Joint_IsEnabled},
    {"Destroy",           l_Joint_Destroy},
    // Type-specific (safe no-op when wrong type)
    {"GetJointAngle",       l_Joint_GetJointAngle},
    {"GetJointSpeed",       l_Joint_GetJointSpeed},
    {"GetJointTranslation", l_Joint_GetJointTranslation},
    {"SetMotorSpeed",       l_Joint_SetMotorSpeed},
    {"EnableMotor",         l_Joint_EnableMotor},
    {"SetTarget",           l_Joint_SetTarget},
    {"SetLength",           l_Joint_SetLength},
    {"GetLength",           l_Joint_GetLength},
    {"__tostring",          l_Joint_Tostring},
    {NULL, NULL}
};

static PhysicsJoint* PushJointTable(lua_State* L, PhysicsWorld* world, b2Joint* joint) {
    lua_createtable(L, 0, 8);
    void* storage = lua_newuserdata(L, sizeof(PhysicsJoint));
    auto* wrapper = new (storage) PhysicsJoint();
    wrapper->joint = joint;
    wrapper->owner = world;
    wrapper->alive = true;
    lua_setfield(L, -2, "__joint");
    luaL_setfuncs(L, g_joint_funcs, 0);
    lua_pushvalue(L, -1);
    wrapper->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    joint->GetUserData().pointer = reinterpret_cast<uintptr_t>(wrapper);
    world->joints.push_back(wrapper);
    return wrapper;
}

// ============================================================
// Phase AO: World Create*Joint / DestroyJoint / GetJointCount
// ============================================================

static bool ResolveBodyPair(lua_State* L, int idxA, int idxB, PhysicsWorld* world,
                            b2Body** outA, b2Body** outB) {
    auto* bodyA = CheckBody(L, idxA);
    auto* bodyB = CheckBody(L, idxB);
    if (!bodyA || !bodyB) return false;
    if (bodyA->owner != world || bodyB->owner != world) return false;
    *outA = bodyA->body;
    *outB = bodyB->body;
    return true;
}

/// @lua_api Light.Physics.World.CreateDistanceJoint
/// @brief Phase AO: Create a distance joint between two Bodies
/// @param bodyA table Body A
/// @param bodyB table Body B
/// @param ax number Anchor A world X in pixels
/// @param ay number Anchor A world Y in pixels
/// @param bx number Anchor B world X in pixels
/// @param by number Anchor B world Y in pixels
/// @param collideConnected boolean Allow collision between connected bodies (default false)
/// @return table Joint
static int l_World_CreateDistanceJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2DistanceJointDef def;
    def.Initialize(a, b,
                   ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)),
                   ToMeters((float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7)));
    def.collideConnected = lua_toboolean(L, 8) != 0;
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreateRevoluteJoint
/// @brief Phase AO: Create a revolute joint between two Bodies
/// @param bodyA table Body A
/// @param bodyB table Body B
/// @param ax number Anchor world X in pixels
/// @param ay number Anchor world Y in pixels
/// @param collideConnected boolean Allow collision between connected bodies (default false)
/// @return table Joint
static int l_World_CreateRevoluteJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2RevoluteJointDef def;
    def.Initialize(a, b, ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)));
    def.collideConnected = lua_toboolean(L, 6) != 0;
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreatePrismaticJoint
/// @brief Phase AO: Create a prismatic joint between two Bodies
/// @param bodyA table Body A
/// @param bodyB table Body B
/// @param ax number Anchor world X in pixels
/// @param ay number Anchor world Y in pixels
/// @param axisX number Axis direction X (unitless, normalized internally)
/// @param axisY number Axis direction Y
/// @param collideConnected boolean Default false
/// @return table Joint
static int l_World_CreatePrismaticJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2PrismaticJointDef def;
    b2Vec2 axis((float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7));
    axis.Normalize();
    def.Initialize(a, b, ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)), axis);
    def.collideConnected = lua_toboolean(L, 8) != 0;
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreateWeldJoint
/// @brief Phase AO: Create a weld joint (rigid connection) between two Bodies
/// @param bodyA table Body A
/// @param bodyB table Body B
/// @param ax number Anchor world X in pixels
/// @param ay number Anchor world Y in pixels
/// @param collideConnected boolean Default false
/// @return table Joint
static int l_World_CreateWeldJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2WeldJointDef def;
    def.Initialize(a, b, ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)));
    def.collideConnected = lua_toboolean(L, 6) != 0;
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreateMouseJoint
/// @brief Phase AO: Create a mouse joint to drag a dynamic Body toward a target point
/// @param bodyA table A static/kinematic Body (usually an anchor)
/// @param bodyB table The draggable dynamic Body
/// @param tx number Target world X in pixels
/// @param ty number Target world Y in pixels
/// @param maxForce number Maximum force (optional, default 1000 * mass of bodyB)
/// @return table Joint
static int l_World_CreateMouseJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2MouseJointDef def;
    def.bodyA = a;
    def.bodyB = b;
    def.target = ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5));
    def.maxForce = (float)luaL_optnumber(L, 6, 1000.0 * b->GetMass());
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.DestroyJoint
/// @brief Phase AO: Destroy a joint (safe when already dead)
/// @param joint table Joint object
static int l_World_DestroyJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* joint = CheckJoint(L, 2);
    if (!world || !joint || joint->owner != world) return 0;
    b2Joint* raw = joint->joint;
    InvalidateJoint(L, joint, true);
    world->world->DestroyJoint(raw);
    return 0;
}

/// @lua_api Light.Physics.World.GetJointCount
/// @brief Phase AO: Count currently live joints in this world
/// @return number Joint count
static int l_World_GetJointCount(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    int count = 0;
    if (world) {
        for (auto* joint : world->joints) if (joint && joint->alive) count++;
    }
    lua_pushinteger(L, count);
    return 1;
}

// ============================================================
// Phase AO: RayCast + QueryAABB
// ============================================================

// RayCast callback 需要返回 fraction (0..1) 或特殊值 -1 / 0 / 1
// Lua 回调签名: function(fixture, hitX, hitY, normalX, normalY, fraction)
//   return nil     -> 用 fraction (继续但裁剪)
//   return -1      -> 忽略此 fixture
//   return 0       -> 终止 raycast
//   return 1       -> 继续但不裁剪 (clipping disabled)
//   return <num>   -> 作为新的最大 fraction
class PhysicsRayCastCallback : public b2RayCastCallback {
public:
    PhysicsRayCastCallback(lua_State* L, int cbRef) : L_(L), cbRef_(cbRef), aborted_(false) {}

    float ReportFixture(b2Fixture* fixture, const b2Vec2& point,
                        const b2Vec2& normal, float fraction) override {
        if (aborted_) return 0.0f;
        PhysicsFixture* wrapper = FixtureFromB2(fixture);
        if (!wrapper) return -1.0f;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cbRef_);
        if (!PushFixtureSelf(L_, wrapper)) {
            lua_pop(L_, 1);
            return -1.0f;
        }
        lua_pushnumber(L_, point.x * PTM);
        lua_pushnumber(L_, point.y * PTM);
        lua_pushnumber(L_, normal.x);
        lua_pushnumber(L_, normal.y);
        lua_pushnumber(L_, fraction);
        if (lua_pcall(L_, 6, 1, 0) != 0) {
            const char* err = lua_tostring(L_, -1);
            CC::Log(CC::LOG_WARN, "Physics raycast callback error: %s", err ? err : "(unknown)");
            lua_pop(L_, 1);
            aborted_ = true;
            return 0.0f;  // terminate
        }
        float ret;
        if (lua_isnumber(L_, -1)) ret = (float)lua_tonumber(L_, -1);
        else ret = fraction;  // nil: 使用 fraction
        lua_pop(L_, 1);
        return ret;
    }

private:
    lua_State* L_;
    int cbRef_;
    bool aborted_;
};

/// @lua_api Light.Physics.World.RayCast
/// @brief Phase AO: Cast a ray in the world and invoke the Lua callback per hit
/// @param x1 number Start X in pixels
/// @param y1 number Start Y in pixels
/// @param x2 number End X in pixels
/// @param y2 number End Y in pixels
/// @param callback function function(fixture, hitX, hitY, nx, ny, fraction) return nil|-1|0|1|fraction
static int l_World_RayCast(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    float x1 = (float)luaL_checknumber(L, 2);
    float y1 = (float)luaL_checknumber(L, 3);
    float x2 = (float)luaL_checknumber(L, 4);
    float y2 = (float)luaL_checknumber(L, 5);
    luaL_checktype(L, 6, LUA_TFUNCTION);
    lua_pushvalue(L, 6);
    int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    {
        PhysicsRayCastCallback cb(L, cbRef);
        b2Vec2 p1 = ToMeters(x1, y1), p2 = ToMeters(x2, y2);
        if (!(p1.x == p2.x && p1.y == p2.y)) world->world->RayCast(&cb, p1, p2);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
    return 0;
}

class PhysicsQueryCallback : public b2QueryCallback {
public:
    PhysicsQueryCallback(lua_State* L, int cbRef) : L_(L), cbRef_(cbRef), aborted_(false) {}

    bool ReportFixture(b2Fixture* fixture) override {
        if (aborted_) return false;
        PhysicsFixture* wrapper = FixtureFromB2(fixture);
        if (!wrapper) return true;   // 继续但跳过 invalid wrapper
        lua_rawgeti(L_, LUA_REGISTRYINDEX, cbRef_);
        if (!PushFixtureSelf(L_, wrapper)) {
            lua_pop(L_, 1);
            return true;
        }
        if (lua_pcall(L_, 1, 1, 0) != 0) {
            const char* err = lua_tostring(L_, -1);
            CC::Log(CC::LOG_WARN, "Physics query callback error: %s", err ? err : "(unknown)");
            lua_pop(L_, 1);
            aborted_ = true;
            return false;
        }
        bool cont = lua_isnil(L_, -1) ? true : (lua_toboolean(L_, -1) != 0);
        lua_pop(L_, 1);
        return cont;
    }

private:
    lua_State* L_;
    int cbRef_;
    bool aborted_;
};

/// @lua_api Light.Physics.World.QueryAABB
/// @brief Phase AO: Query fixtures overlapping an AABB, invoking the Lua callback
/// @param x number AABB X in pixels
/// @param y number AABB Y in pixels
/// @param w number AABB width in pixels
/// @param h number AABB height in pixels
/// @param callback function function(fixture) return true to continue, false/nil to stop
static int l_World_QueryAABB(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4);
    float h = (float)luaL_checknumber(L, 5);
    luaL_checktype(L, 6, LUA_TFUNCTION);
    lua_pushvalue(L, 6);
    int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    {
        PhysicsQueryCallback cb(L, cbRef);
        b2AABB aabb;
        aabb.lowerBound = ToMeters(x, y);
        aabb.upperBound = ToMeters(x + w, y + h);
        world->world->QueryAABB(&cb, aabb);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
    return 0;
}

static int l_World_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Physics.World");
    return 1;
}

/// @lua_api Light.Physics.NewCircleShape
/// @brief Create a circle Shape
/// @param radius number Radius in pixels
/// @return table Shape object
static int l_Physics_NewCircleShape(lua_State* L) {
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_CIRCLE);
    shape->circle.m_radius = (float)luaL_checknumber(L, 1) / PTM;
    shape->circle.m_p.Set(0.0f, 0.0f);
    return 1;
}

/// @lua_api Light.Physics.NewRectangleShape
/// @brief Create a rectangle Shape
/// @param width number Width in pixels
/// @param height number Height in pixels
/// @return table Shape object
static int l_Physics_NewRectangleShape(lua_State* L) {
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_BOX);
    float width = (float)luaL_checknumber(L, 1) / PTM;
    float height = (float)luaL_checknumber(L, 2) / PTM;
    shape->polygon.SetAsBox(width * 0.5f, height * 0.5f);
    return 1;
}

static bool ReadVertices(lua_State* L, int idx, std::vector<b2Vec2>& vertices, int maxCount) {
    luaL_checktype(L, idx, LUA_TTABLE);
    int len = (int)lua_objlen(L, idx);
    if (len < 4 || (len % 2) != 0) return false;
    int count = len / 2;
    if (maxCount > 0 && count > maxCount) count = maxCount;
    vertices.clear();
    vertices.reserve(count);
    for (int i = 0; i < count; ++i) {
        lua_rawgeti(L, idx, i * 2 + 1);
        lua_rawgeti(L, idx, i * 2 + 2);
        float x = (float)lua_tonumber(L, -2);
        float y = (float)lua_tonumber(L, -1);
        lua_pop(L, 2);
        vertices.push_back(ToMeters(x, y));
    }
    return true;
}

/// @lua_api Light.Physics.NewPolygonShape
/// @brief Create a polygon Shape
/// @param vertices table Vertex array {x1,y1,x2,y2,...}
/// @return table Shape object
static int l_Physics_NewPolygonShape(lua_State* L) {
    std::vector<b2Vec2> vertices;
    if (!ReadVertices(L, 1, vertices, b2_maxPolygonVertices) || vertices.size() < 3) {
        return luaL_error(L, "Light.Physics.NewPolygonShape expects at least 3 points");
    }
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_POLYGON);
    shape->polygon.Set(vertices.data(), (int32)vertices.size());
    return 1;
}

/// @lua_api Light.Physics.NewEdgeShape
/// @brief Create an edge Shape
/// @param x1 number Start X position
/// @param y1 number Start Y position
/// @param x2 number End X position
/// @param y2 number End Y position
/// @return table Shape object
static int l_Physics_NewEdgeShape(lua_State* L) {
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_EDGE);
    shape->edge.SetTwoSided(ToMeters((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2)),
                            ToMeters((float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)));
    return 1;
}

/// @lua_api Light.Physics.NewChainShape
/// @brief Create a chain Shape
/// @param vertices table Vertex array {x1,y1,x2,y2,...}
/// @param loop boolean Whether the chain is closed
/// @return table Shape object
static int l_Physics_NewChainShape(lua_State* L) {
    std::vector<b2Vec2> vertices;
    if (!ReadVertices(L, 1, vertices, 0) || vertices.size() < 2) {
        return luaL_error(L, "Light.Physics.NewChainShape expects at least 2 points");
    }
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_CHAIN);
    shape->chain = new b2ChainShape();
    if (lua_toboolean(L, 2)) shape->chain->CreateLoop(vertices.data(), (int32)vertices.size());
    else shape->chain->CreateChain(vertices.data(), (int32)vertices.size(), vertices.front(), vertices.back());
    return 1;
}

static const luaL_Reg g_physics_funcs[] = {
    {"NewCircleShape",    l_Physics_NewCircleShape},
    {"NewRectangleShape", l_Physics_NewRectangleShape},
    {"NewPolygonShape",   l_Physics_NewPolygonShape},
    {"NewEdgeShape",      l_Physics_NewEdgeShape},
    {"NewChainShape",     l_Physics_NewChainShape},
    {NULL, NULL}
};

static const luaL_Reg g_world_funcs[] = {
    {"SetGravity",           l_World_SetGravity},
    {"GetGravity",           l_World_GetGravity},
    {"Step",                 l_World_Step},
    {"ClearForces",          l_World_ClearForces},
    {"SetAllowSleeping",     l_World_SetAllowSleeping},
    {"SetContinuousPhysics", l_World_SetContinuousPhysics},
    {"CreateBody",           l_World_CreateBody},
    {"DestroyBody",          l_World_DestroyBody},
    {"GetBodyCount",         l_World_GetBodyCount},
    {"OnCollision",          l_World_OnCollision},
    {"BeginContact",         l_World_BeginContact},
    {"EndContact",           l_World_EndContact},
    // Phase AO: joints + queries
    {"CreateDistanceJoint",  l_World_CreateDistanceJoint},
    {"CreateRevoluteJoint",  l_World_CreateRevoluteJoint},
    {"CreatePrismaticJoint", l_World_CreatePrismaticJoint},
    {"CreateWeldJoint",      l_World_CreateWeldJoint},
    {"CreateMouseJoint",     l_World_CreateMouseJoint},
    {"DestroyJoint",         l_World_DestroyJoint},
    {"GetJointCount",        l_World_GetJointCount},
    {"RayCast",              l_World_RayCast},
    {"QueryAABB",            l_World_QueryAABB},
    {"__call",               l_World_Call},
    {"__tostring",           l_World_Tostring},
    {NULL, NULL}
};

#else

static int l_Physics_Box2DUnavailable(lua_State* L) {
    return luaL_error(L, "Light.Physics requires Box2D on this platform");
}

static const luaL_Reg g_physics_funcs[] = {
    {"NewCircleShape",    l_Physics_Box2DUnavailable},
    {"NewRectangleShape", l_Physics_Box2DUnavailable},
    {"NewPolygonShape",   l_Physics_Box2DUnavailable},
    {"NewEdgeShape",      l_Physics_Box2DUnavailable},
    {"NewChainShape",     l_Physics_Box2DUnavailable},
    {NULL, NULL}
};

static const luaL_Reg g_world_funcs[] = {
    {"__call", l_Physics_Box2DUnavailable},
    {NULL, NULL}
};

#endif

int luaopen_Light_Physics(lua_State* L) {
    LT::RegisterModule(L, "Physics", g_physics_funcs);
    luaL_setfuncs(L, g_physics_funcs, 0);
    return 1;
}

int luaopen_Light_Physics_World(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "Physics");
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, g_physics_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
    } else {
        luaL_setfuncs(L, g_physics_funcs, 0);
    }

    lua_pushstring(L, "World");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "World");
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, g_world_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "World");
        lua_rawget(L, -2);
    } else {
        luaL_setfuncs(L, g_world_funcs, 0);
    }

    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}
