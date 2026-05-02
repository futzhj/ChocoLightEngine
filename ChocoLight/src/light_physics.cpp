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

struct PhysicsWorld {
    b2World* world;
    PhysicsContactListener* listener;
    std::vector<PhysicsBody*> bodies;
    std::vector<PhysicsFixture*> fixtures;
    std::vector<PhysicsContactEvent> contactEvents;
    int legacyCollisionRef;
    int beginContactRef;
    int endContactRef;
    lua_State* L;
    bool alive;

    PhysicsWorld()
        : world(nullptr), listener(nullptr), legacyCollisionRef(LUA_NOREF), beginContactRef(LUA_NOREF),
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
    body->alive = false;
    body->body = nullptr;
    if (releaseRefs && body->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, body->selfRef);
        body->selfRef = LUA_NOREF;
    }
}

void PhysicsContactListener::BeginContact(b2Contact* contact) {
    if (!world_ || !world_->alive || !contact) return;
    world_->contactEvents.push_back({true, contact->GetFixtureA(), contact->GetFixtureB()});
}

void PhysicsContactListener::EndContact(b2Contact* contact) {
    if (!world_ || !world_->alive || !contact) return;
    world_->contactEvents.push_back({false, contact->GetFixtureA(), contact->GetFixtureB()});
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

static int l_Fixture_GetBody(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture && PushBodySelf(L, fixture->owner)) return 1;
    lua_pushnil(L);
    return 1;
}

static int l_Fixture_SetDensity(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) {
        fixture->fixture->SetDensity((float)luaL_checknumber(L, 2));
        if (fixture->owner && fixture->owner->body) fixture->owner->body->ResetMassData();
    }
    return 0;
}

static int l_Fixture_GetDensity(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushnumber(L, fixture ? fixture->fixture->GetDensity() : 0.0f);
    return 1;
}

static int l_Fixture_SetFriction(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) fixture->fixture->SetFriction((float)luaL_checknumber(L, 2));
    return 0;
}

static int l_Fixture_GetFriction(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushnumber(L, fixture ? fixture->fixture->GetFriction() : 0.0f);
    return 1;
}

static int l_Fixture_SetRestitution(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) fixture->fixture->SetRestitution((float)luaL_checknumber(L, 2));
    return 0;
}

static int l_Fixture_GetRestitution(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushnumber(L, fixture ? fixture->fixture->GetRestitution() : 0.0f);
    return 1;
}

static int l_Fixture_SetSensor(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (fixture) fixture->fixture->SetSensor(lua_toboolean(L, 2) != 0);
    return 0;
}

static int l_Fixture_IsSensor(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    lua_pushboolean(L, fixture && fixture->fixture->IsSensor());
    return 1;
}

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

static int l_Fixture_GetUserData(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture || fixture->userRef == LUA_NOREF) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, fixture->userRef);
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

static int l_Body_CreateFixture(lua_State* L) {
    auto* body = CheckBody(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* shape = CheckShape(L, 2);
    float density = (float)luaL_optnumber(L, 3, 1.0);
    if (!body || !shape) return 0;
    return CreateFixtureFromShape(L, body, shape, density);
}

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

static int l_Body_GetType(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushstring(L, body ? BodyTypeName(body->body->GetType()) : "static");
    return 1;
}

static int l_Body_SetType(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetType(ParseBodyType(luaL_checkstring(L, 2)));
    return 0;
}

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

static int l_Body_SetPosition(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    body->body->SetTransform(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)), body->body->GetAngle());
    return 0;
}

static int l_Body_GetAngle(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetAngle() : 0.0f);
    return 1;
}

static int l_Body_SetAngle(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetTransform(body->body->GetPosition(), (float)luaL_checknumber(L, 2));
    return 0;
}

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

static int l_Body_SetLinearVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    body->body->SetLinearVelocity(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)));
    return 0;
}

static int l_Body_GetAngularVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetAngularVelocity() : 0.0f);
    return 1;
}

static int l_Body_SetAngularVelocity(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetAngularVelocity((float)luaL_checknumber(L, 2));
    return 0;
}

static int l_Body_ApplyForce(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ApplyForceToCenter(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)), true);
    return 0;
}

static int l_Body_ApplyImpulse(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ApplyLinearImpulseToCenter(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)), true);
    return 0;
}

static int l_Body_ApplyTorque(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ApplyTorque((float)luaL_checknumber(L, 2), true);
    return 0;
}

static int l_Body_SetAwake(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetAwake(lua_toboolean(L, 2) != 0);
    return 0;
}

static int l_Body_IsAwake(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsAwake());
    return 1;
}

static int l_Body_SetActive(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetEnabled(lua_toboolean(L, 2) != 0);
    return 0;
}

static int l_Body_IsActive(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsEnabled());
    return 1;
}

static int l_Body_SetBullet(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetBullet(lua_toboolean(L, 2) != 0);
    return 0;
}

static int l_Body_IsBullet(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsBullet());
    return 1;
}

static int l_Body_SetLinearDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetLinearDamping((float)luaL_checknumber(L, 2));
    return 0;
}

static int l_Body_GetLinearDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetLinearDamping() : 0.0f);
    return 1;
}

static int l_Body_SetAngularDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetAngularDamping((float)luaL_checknumber(L, 2));
    return 0;
}

static int l_Body_GetAngularDamping(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetAngularDamping() : 0.0f);
    return 1;
}

static int l_Body_GetMass(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetMass() : 0.0f);
    return 1;
}

static int l_Body_GetInertia(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetInertia() : 0.0f);
    return 1;
}

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

static int l_Body_SetRestitution(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    float value = (float)luaL_checknumber(L, 2);
    for (b2Fixture* fixture = body->body->GetFixtureList(); fixture; fixture = fixture->GetNext()) fixture->SetRestitution(value);
    return 0;
}

static int l_Body_SetFriction(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    float value = (float)luaL_checknumber(L, 2);
    for (b2Fixture* fixture = body->body->GetFixtureList(); fixture; fixture = fixture->GetNext()) fixture->SetFriction(value);
    return 0;
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
    {NULL, NULL}
};

static int l_World_GC(lua_State* L) {
    auto* world = (PhysicsWorld*)lua_touserdata(L, 1);
    if (!world || !world->alive) return 0;

    for (auto* body : world->bodies) InvalidateBody(L, body, true);
    if (world->legacyCollisionRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->legacyCollisionRef);
    if (world->beginContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->beginContactRef);
    if (world->endContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->endContactRef);
    delete world->listener;
    delete world->world;
    world->listener = nullptr;
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

static int l_World_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    void* storage = lua_newuserdata(L, sizeof(PhysicsWorld));
    auto* world = new (storage) PhysicsWorld();
    world->world = new b2World(b2Vec2(0.0f, 10.0f));
    world->listener = new PhysicsContactListener(world);
    world->world->SetContactListener(world->listener);
    world->L = L;
    world->alive = true;
    SetWorldGC(L);
    lua_setfield(L, 1, "__instance");
    return 0;
}

static int l_World_SetGravity(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetGravity(ToMeters((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)));
    return 0;
}

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

static int l_World_Step(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    world->world->Step((float)luaL_checknumber(L, 2), 8, 3);
    DispatchContactEvents(L, world);
    return 0;
}

static int l_World_ClearForces(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->ClearForces();
    return 0;
}

static int l_World_SetAllowSleeping(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetAllowSleeping(lua_toboolean(L, 2) != 0);
    return 0;
}

static int l_World_SetContinuousPhysics(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetContinuousPhysics(lua_toboolean(L, 2) != 0);
    return 0;
}

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
    world->bodies.push_back(wrapper);
    return 1;
}

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

static int l_World_GetBodyCount(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    int count = 0;
    if (world) {
        for (auto* body : world->bodies) if (body && body->alive) count++;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_World_OnCollision(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (world->legacyCollisionRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->legacyCollisionRef);
    lua_pushvalue(L, 2);
    world->legacyCollisionRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_World_BeginContact(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (world->beginContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->beginContactRef);
    lua_pushvalue(L, 2);
    world->beginContactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_World_EndContact(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (world->endContactRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->endContactRef);
    lua_pushvalue(L, 2);
    world->endContactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_World_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Physics.World");
    return 1;
}

static int l_Physics_NewCircleShape(lua_State* L) {
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_CIRCLE);
    shape->circle.m_radius = (float)luaL_checknumber(L, 1) / PTM;
    shape->circle.m_p.Set(0.0f, 0.0f);
    return 1;
}

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

static int l_Physics_NewPolygonShape(lua_State* L) {
    std::vector<b2Vec2> vertices;
    if (!ReadVertices(L, 1, vertices, b2_maxPolygonVertices) || vertices.size() < 3) {
        return luaL_error(L, "Light.Physics.NewPolygonShape expects at least 3 points");
    }
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_POLYGON);
    shape->polygon.Set(vertices.data(), (int32)vertices.size());
    return 1;
}

static int l_Physics_NewEdgeShape(lua_State* L) {
    PhysicsShape* shape = PushShapeTable(L, PHYSICS_SHAPE_EDGE);
    shape->edge.SetTwoSided(ToMeters((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2)),
                            ToMeters((float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4)));
    return 1;
}

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
