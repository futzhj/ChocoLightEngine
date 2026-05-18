#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7.3 — 类型安全 helpers + magic

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

/// Phase G.1.7.3: 首字段 magic 防止 type-confusion
struct PhysicsBody {
    uint32_t magic;          // 必须 = LT_MAGIC_BODY
    b2Body* body;
    PhysicsWorld* owner;
    int selfRef;
    bool alive;

    PhysicsBody() : body(nullptr), owner(nullptr), selfRef(LUA_NOREF), alive(false) {}
};

/// Phase G.1.7.3: 首字段 magic 防止 type-confusion
struct PhysicsFixture {
    uint32_t magic;          // 必须 = LT_MAGIC_FIXTURE
    b2Fixture* fixture;
    PhysicsBody* owner;
    int selfRef;
    int userRef;
    bool alive;

    PhysicsFixture() : fixture(nullptr), owner(nullptr), selfRef(LUA_NOREF), userRef(LUA_NOREF), alive(false) {}
};

// Phase AO: Joint wrapper. Joint 类型识别用 b2Joint::GetType() 即可,无需在 wrapper 内冗余存储。
/// Phase G.1.7.3: 首字段 magic 防止 type-confusion
struct PhysicsJoint {
    uint32_t magic;          // 必须 = LT_MAGIC_JOINT
    b2Joint* joint;
    PhysicsWorld* owner;
    int selfRef;
    bool alive;

    PhysicsJoint() : joint(nullptr), owner(nullptr), selfRef(LUA_NOREF), alive(false) {}
};

/// Phase G.1.7.3: 首字段 magic 防止 type-confusion
struct PhysicsShape {
    uint32_t magic;          // 必须 = LT_MAGIC_SHAPE
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

// Phase AP: Contact userdata view — Lua 侧通过它调用 SetEnabled/SetFriction 等
// 仅在 PreSolve/PostSolve 回调内有效, 回调返回后 alive=false, 后续方法安全 no-op
struct PhysicsContactView {
    b2Contact* contact;
    bool alive;
};

class PhysicsContactListener : public b2ContactListener {
public:
    explicit PhysicsContactListener(PhysicsWorld* world) : world_(world) {}
    void BeginContact(b2Contact* contact) override;
    void EndContact(b2Contact* contact) override;
    void PreSolve(b2Contact* contact, const b2Manifold* oldManifold) override;           // Phase AP
    void PostSolve(b2Contact* contact, const b2ContactImpulse* impulse) override;        // Phase AP

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

/// Phase G.1.7.3: 首字段 magic 防止 type-confusion
struct PhysicsWorld {
    uint32_t magic;          // 必须 = LT_MAGIC_WORLD (首字段)
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
    int preSolveRef;          // Phase AP
    int postSolveRef;         // Phase AP
    lua_State* L;
    bool alive;

    PhysicsWorld()
        : world(nullptr), listener(nullptr), destructionListener(nullptr),
          legacyCollisionRef(LUA_NOREF), beginContactRef(LUA_NOREF),
          endContactRef(LUA_NOREF),
          preSolveRef(LUA_NOREF), postSolveRef(LUA_NOREF),
          L(nullptr), alive(false) {}
};

static b2Vec2 ToMeters(float x, float y) {
    return b2Vec2(x / PTM, y / PTM);
}

static void PushPixels(lua_State* L, const b2Vec2& value) {
    lua_pushnumber(L, value.x * PTM);
    lua_pushnumber(L, value.y * PTM);
}

/// Phase G.1.7.3: magic 双保险
static PhysicsWorld* CheckWorld(lua_State* L, int idx) {
    auto* world = LT::TryCheckInstance<PhysicsWorld>(L, idx, LT::LT_MAGIC_WORLD);
    return (world && world->alive && world->world) ? world : nullptr;
}

/// Phase G.1.7.3: magic 双保险; 字段名不变 (__body), 手动读取
static PhysicsBody* CheckBody(lua_State* L, int idx) {
    lua_getfield(L, idx, "__body");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* body = (PhysicsBody*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!body || body->magic != LT::LT_MAGIC_BODY) return nullptr;
    return (body->alive && body->body) ? body : nullptr;
}

/// Phase G.1.7.3: magic 双保险
static PhysicsShape* CheckShape(lua_State* L, int idx) {
    lua_getfield(L, idx, "__shape");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* shape = (PhysicsShape*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!shape || shape->magic != LT::LT_MAGIC_SHAPE) return nullptr;
    return shape;
}

/// Phase G.1.7.3: magic 双保险
static PhysicsFixture* CheckFixture(lua_State* L, int idx) {
    lua_getfield(L, idx, "__fixture");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* fixture = (PhysicsFixture*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!fixture || fixture->magic != LT::LT_MAGIC_FIXTURE) return nullptr;
    return (fixture->alive && fixture->fixture) ? fixture : nullptr;
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
/// Phase G.1.7.3: magic 双保险
static PhysicsJoint* CheckJoint(lua_State* L, int idx) {
    lua_getfield(L, idx, "__joint");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    auto* joint = (PhysicsJoint*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!joint || joint->magic != LT::LT_MAGIC_JOINT) return nullptr;
    return (joint->alive && joint->joint) ? joint : nullptr;
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

// ============================================================
// Phase AP: Contact "live" 方法 (PreSolve/PostSolve 内有效)
// ============================================================

// 取出 contact 对应的 PhysicsContactView (alive 时返回非空)
static PhysicsContactView* CheckContactView(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_getfield(L, idx, "__contact");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* view = (PhysicsContactView*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!view || !view->alive || !view->contact) return nullptr;
    return view;
}

/// @lua_api Light.Physics.Contact.IsTouching
/// @brief Phase AP: Whether the contact is currently touching (PreSolve/PostSolve only)
static int l_Contact_IsTouching(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    lua_pushboolean(L, view && view->contact->IsTouching());
    return 1;
}

/// @lua_api Light.Physics.Contact.IsEnabled
/// @brief Phase AP: Whether the contact is enabled (PreSolve only useful)
static int l_Contact_IsEnabled(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    lua_pushboolean(L, view && view->contact->IsEnabled());
    return 1;
}

/// @lua_api Light.Physics.Contact.SetEnabled
/// @brief Phase AP: Enable/disable contact for this single time step (must be called in PreSolve)
/// @param enabled boolean Enabled flag
static int l_Contact_SetEnabled(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    if (!view) return 0;
    view->contact->SetEnabled(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Contact.SetFriction
/// @brief Phase AP: Override friction for this contact (must be called in PreSolve)
/// @param friction number Friction coefficient
static int l_Contact_SetFriction(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    if (!view) return 0;
    view->contact->SetFriction((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Contact.GetFriction
/// @brief Phase AP: Current friction coefficient
static int l_Contact_GetFriction(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    lua_pushnumber(L, view ? view->contact->GetFriction() : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Contact.SetRestitution
/// @brief Phase AP: Override restitution (bounciness) for this contact (must be called in PreSolve)
/// @param restitution number Restitution coefficient
static int l_Contact_SetRestitution(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    if (!view) return 0;
    view->contact->SetRestitution((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Contact.GetRestitution
/// @brief Phase AP: Current restitution coefficient
static int l_Contact_GetRestitution(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    lua_pushnumber(L, view ? view->contact->GetRestitution() : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Contact.ResetFriction
/// @brief Phase AP: Reset friction to per-fixture mixed value
static int l_Contact_ResetFriction(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    if (view) view->contact->ResetFriction();
    return 0;
}

/// @lua_api Light.Physics.Contact.ResetRestitution
/// @brief Phase AP: Reset restitution to per-fixture mixed value
static int l_Contact_ResetRestitution(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    if (view) view->contact->ResetRestitution();
    return 0;
}

/// @lua_api Light.Physics.Contact.GetManifoldPointCount
/// @brief Phase AP: Number of contact points (0/1/2)
static int l_Contact_GetManifoldPointCount(lua_State* L) {
    auto* view = CheckContactView(L, 1);
    if (!view) { lua_pushinteger(L, 0); return 1; }
    const b2Manifold* m = view->contact->GetManifold();
    lua_pushinteger(L, m ? m->pointCount : 0);
    return 1;
}

static const luaL_Reg g_contact_funcs[] = {
    {"GetBodyA",    l_Contact_GetBodyA},
    {"GetBodyB",    l_Contact_GetBodyB},
    {"GetFixtureA", l_Contact_GetFixtureA},
    {"GetFixtureB", l_Contact_GetFixtureB},
    // Phase AP: live-contact methods
    {"IsTouching",              l_Contact_IsTouching},
    {"IsEnabled",               l_Contact_IsEnabled},
    {"SetEnabled",              l_Contact_SetEnabled},
    {"SetFriction",             l_Contact_SetFriction},
    {"GetFriction",             l_Contact_GetFriction},
    {"SetRestitution",          l_Contact_SetRestitution},
    {"GetRestitution",          l_Contact_GetRestitution},
    {"ResetFriction",           l_Contact_ResetFriction},
    {"ResetRestitution",        l_Contact_ResetRestitution},
    {"GetManifoldPointCount",   l_Contact_GetManifoldPointCount},
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

// Phase AP: 推一个携带 __contact userdata 的 Contact table (live) 用于 PreSolve/PostSolve
// 返回栈顶 view 指针, 调用方在回调返回后置 view->alive = false
static PhysicsContactView* PushLiveContactTable(lua_State* L, b2Contact* contact) {
    PhysicsFixture* fa = contact ? FixtureFromB2(contact->GetFixtureA()) : nullptr;
    PhysicsFixture* fb = contact ? FixtureFromB2(contact->GetFixtureB()) : nullptr;
    PhysicsBody* ba = fa ? fa->owner : nullptr;
    PhysicsBody* bb = fb ? fb->owner : nullptr;
    PushContactTable(L, ba, bb, fa, fb);  // 在栈顶留 contact table
    // 附加 __contact userdata
    auto* view = (PhysicsContactView*)lua_newuserdata(L, sizeof(PhysicsContactView));
    view->contact = contact;
    view->alive = true;
    lua_setfield(L, -2, "__contact");
    return view;
}

void PhysicsContactListener::PreSolve(b2Contact* contact, const b2Manifold* oldManifold) {
    (void)oldManifold;
    if (!world_ || !world_->alive || !contact) return;
    if (world_->preSolveRef == LUA_NOREF) return;
    lua_State* L = world_->L;
    if (!L) return;

    int baseTop = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, world_->preSolveRef);
    if (!lua_isfunction(L, -1)) { lua_settop(L, baseTop); return; }
    PhysicsContactView* view = PushLiveContactTable(L, contact);
    // pcall 保护: PreSolve 异常不应中断 Step
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        if (err) CC::Log(CC::LOG_ERROR, "PreSolve error: %s", err);
    }
    view->alive = false;
    lua_settop(L, baseTop);
}

void PhysicsContactListener::PostSolve(b2Contact* contact, const b2ContactImpulse* impulse) {
    if (!world_ || !world_->alive || !contact) return;
    if (world_->postSolveRef == LUA_NOREF) return;
    lua_State* L = world_->L;
    if (!L) return;

    int baseTop = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, world_->postSolveRef);
    if (!lua_isfunction(L, -1)) { lua_settop(L, baseTop); return; }
    PhysicsContactView* view = PushLiveContactTable(L, contact);
    // 复制 normal/tangent impulses 到 Lua 数组
    int n = impulse ? impulse->count : 0;
    if (n < 0) n = 0;
    if (n > b2_maxManifoldPoints) n = b2_maxManifoldPoints;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushnumber(L, impulse->normalImpulses[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushnumber(L, impulse->tangentImpulses[i]);
        lua_rawseti(L, -2, i + 1);
    }
    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        if (err) CC::Log(CC::LOG_ERROR, "PostSolve error: %s", err);
    }
    view->alive = false;
    lua_settop(L, baseTop);
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
    shape->magic = LT::LT_MAGIC_SHAPE;  // Phase G.1.7.3 — type tag (placement-new 后设)
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

/// @lua_api Light.Physics.Fixture.RayCast
/// @brief Phase AP: Cast a ray against only this single fixture.
/// @param x1 number Start X in pixels
/// @param y1 number Start Y in pixels
/// @param x2 number End X in pixels
/// @param y2 number End Y in pixels
/// @param childIndex number Child shape index (chain/polygon usually 0), optional
/// @return number, number, number, number, number Hit X (px), Hit Y (px), Normal X, Normal Y, fraction — or nil if miss
static int l_Fixture_RayCast(lua_State* L) {
    auto* fixture = CheckFixture(L, 1);
    if (!fixture) { lua_pushnil(L); return 1; }
    float x1 = (float)luaL_checknumber(L, 2);
    float y1 = (float)luaL_checknumber(L, 3);
    float x2 = (float)luaL_checknumber(L, 4);
    float y2 = (float)luaL_checknumber(L, 5);
    int childIndex = (int)luaL_optinteger(L, 6, 0);

    b2RayCastInput input;
    input.p1 = ToMeters(x1, y1);
    input.p2 = ToMeters(x2, y2);
    input.maxFraction = 1.0f;

    b2RayCastOutput output;
    bool hit = fixture->fixture->RayCast(&output, input, childIndex);
    if (!hit) { lua_pushnil(L); return 1; }

    b2Vec2 hitPoint = input.p1 + output.fraction * (input.p2 - input.p1);
    lua_pushnumber(L, hitPoint.x * PTM);
    lua_pushnumber(L, hitPoint.y * PTM);
    lua_pushnumber(L, output.normal.x);
    lua_pushnumber(L, output.normal.y);
    lua_pushnumber(L, output.fraction);
    return 5;
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
    {"RayCast",        l_Fixture_RayCast},      // Phase AP
    {NULL, NULL}
};

static PhysicsFixture* PushFixtureTable(lua_State* L, PhysicsBody* body, b2Fixture* fixture) {
    lua_createtable(L, 0, 16);
    void* storage = lua_newuserdata(L, sizeof(PhysicsFixture));
    auto* wrapper = new (storage) PhysicsFixture();
    wrapper->magic = LT::LT_MAGIC_FIXTURE;  // Phase G.1.7.3 — type tag
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

// ============================================================
// Phase AP: Body 高级属性
// ============================================================

/// @lua_api Light.Physics.Body.GetGravityScale
/// @brief Phase AP: Get this Body's gravity scale multiplier (default 1.0)
/// @return number Gravity scale
static int l_Body_GetGravityScale(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushnumber(L, body ? body->body->GetGravityScale() : 1.0);
    return 1;
}

/// @lua_api Light.Physics.Body.SetGravityScale
/// @brief Phase AP: Set this Body's gravity scale (0 = no gravity, 2 = double)
/// @param scale number Gravity multiplier
static int l_Body_SetGravityScale(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetGravityScale((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Body.IsFixedRotation
/// @brief Phase AP: Query whether this Body has fixed (zero) rotation
/// @return boolean Fixed-rotation flag
static int l_Body_IsFixedRotation(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsFixedRotation());
    return 1;
}

/// @lua_api Light.Physics.Body.SetFixedRotation
/// @brief Phase AP: Lock rotation. Useful for character controllers.
/// @param enabled boolean Fixed-rotation flag
static int l_Body_SetFixedRotation(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetFixedRotation(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Body.IsSleepingAllowed
/// @brief Phase AP: Query whether this Body may be put to sleep automatically
/// @return boolean Sleeping-allowed flag
static int l_Body_IsSleepingAllowed(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_pushboolean(L, body && body->body->IsSleepingAllowed());
    return 1;
}

/// @lua_api Light.Physics.Body.SetSleepingAllowed
/// @brief Phase AP: Toggle whether this Body may auto-sleep.
/// @param enabled boolean Allow sleep
static int l_Body_SetSleepingAllowed(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->SetSleepingAllowed(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.Body.ResetMassData
/// @brief Phase AP: Recompute mass from fixtures. Call after changing fixture density.
static int l_Body_ResetMassData(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (body) body->body->ResetMassData();
    return 0;
}

/// @lua_api Light.Physics.Body.GetMassData
/// @brief Phase AP: Get mass data (mass, centerX_px, centerY_px, inertia)
/// @return number, number, number, number mass, center x (px), center y (px), rotational inertia
static int l_Body_GetMassData(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) {
        lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 4;
    }
    b2MassData md;
    body->body->GetMassData(&md);
    lua_pushnumber(L, md.mass);
    lua_pushnumber(L, md.center.x * PTM);
    lua_pushnumber(L, md.center.y * PTM);
    lua_pushnumber(L, md.I);
    return 4;
}

/// @lua_api Light.Physics.Body.SetMassData
/// @brief Phase AP: Manually override mass data.
/// @param mass number Mass in kg
/// @param cx number Center X in pixels
/// @param cy number Center Y in pixels
/// @param inertia number Rotational inertia
static int l_Body_SetMassData(lua_State* L) {
    auto* body = CheckBody(L, 1);
    if (!body) return 0;
    b2MassData md;
    md.mass   = (float)luaL_checknumber(L, 2);
    md.center = ToMeters((float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
    md.I      = (float)luaL_optnumber(L, 5, 0.0);
    body->body->SetMassData(&md);
    return 0;
}

// ============================================================
// Phase AP: Body 邻居遍历
// ============================================================

// 辅助: 从 b2Body* 反查 Lua body table 并压栈
static void PushBodyFromRaw(lua_State* L, b2Body* raw) {
    if (!raw) { lua_pushnil(L); return; }
    PhysicsBody* wrapper = reinterpret_cast<PhysicsBody*>(raw->GetUserData().pointer);
    if (!wrapper || !wrapper->alive || wrapper->selfRef == LUA_NOREF) {
        lua_pushnil(L);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, wrapper->selfRef);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

/// @lua_api Light.Physics.Body.GetJointList
/// @brief Phase AP: Array of all joints attached to this Body (alive only).
/// @return table Array of joint tables
static int l_Body_GetJointList(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_newtable(L);
    if (!body) return 1;
    int idx = 1;
    for (b2JointEdge* edge = body->body->GetJointList(); edge; edge = edge->next) {
        if (!edge->joint) continue;
        PhysicsJoint* jw = reinterpret_cast<PhysicsJoint*>(edge->joint->GetUserData().pointer);
        if (!jw || !jw->alive || jw->selfRef == LUA_NOREF) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, jw->selfRef);
        if (lua_istable(L, -1)) {
            lua_rawseti(L, -2, idx++);
        } else {
            lua_pop(L, 1);
        }
    }
    return 1;
}

/// @lua_api Light.Physics.Body.GetContactList
/// @brief Phase AP: Array of currently-touching contacts. Each item is a contact table with fixtureA/B, bodyA/B.
/// @return table Array of contact tables (only "touching" contacts included)
static int l_Body_GetContactList(lua_State* L) {
    auto* body = CheckBody(L, 1);
    lua_newtable(L);
    if (!body) return 1;
    int idx = 1;
    for (b2ContactEdge* edge = body->body->GetContactList(); edge; edge = edge->next) {
        if (!edge->contact || !edge->contact->IsTouching()) continue;
        b2Contact* c = edge->contact;
        b2Fixture* fa = c->GetFixtureA();
        b2Fixture* fb = c->GetFixtureB();
        if (!fa || !fb) continue;
        lua_newtable(L);
        // fixtureA / fixtureB
        PhysicsFixture* fwa = FixtureFromB2(fa);
        PhysicsFixture* fwb = FixtureFromB2(fb);
        if (fwa && PushFixtureSelf(L, fwa)) {
            lua_setfield(L, -2, "fixtureA");
        }
        if (fwb && PushFixtureSelf(L, fwb)) {
            lua_setfield(L, -2, "fixtureB");
        }
        // bodyA / bodyB
        PushBodyFromRaw(L, fa->GetBody());
        lua_setfield(L, -2, "bodyA");
        PushBodyFromRaw(L, fb->GetBody());
        lua_setfield(L, -2, "bodyB");
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
    // Phase AP: Body 高级属性
    {"GetGravityScale",               l_Body_GetGravityScale},
    {"SetGravityScale",               l_Body_SetGravityScale},
    {"IsFixedRotation",               l_Body_IsFixedRotation},
    {"SetFixedRotation",              l_Body_SetFixedRotation},
    {"IsSleepingAllowed",             l_Body_IsSleepingAllowed},
    {"SetSleepingAllowed",            l_Body_SetSleepingAllowed},
    {"ResetMassData",                 l_Body_ResetMassData},
    {"GetMassData",                   l_Body_GetMassData},
    {"SetMassData",                   l_Body_SetMassData},
    // Phase AP: 邻居遍历
    {"GetJointList",                  l_Body_GetJointList},
    {"GetContactList",                l_Body_GetContactList},
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
    if (world->preSolveRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->preSolveRef);    // Phase AP
    if (world->postSolveRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, world->postSolveRef);  // Phase AP
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
    world->magic = LT::LT_MAGIC_WORLD;  // Phase G.1.7.3 — type tag
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

// ============================================================
// Phase AP: World 仿真控制
// ============================================================

/// @lua_api Light.Physics.World.SetSubStepping
/// @brief Phase AP: Enable sub-stepping within continuous physics (small perf cost, better CCD)
/// @param enabled boolean Sub-stepping flag
static int l_World_SetSubStepping(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetSubStepping(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.World.GetSubStepping
/// @brief Phase AP: Query sub-stepping flag
/// @return boolean Sub-stepping flag
static int l_World_GetSubStepping(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    lua_pushboolean(L, world && world->world->GetSubStepping());
    return 1;
}

/// @lua_api Light.Physics.World.SetWarmStarting
/// @brief Phase AP: Enable solver warm-starting for better stacking stability (default on)
/// @param enabled boolean Warm-start flag
static int l_World_SetWarmStarting(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (world) world->world->SetWarmStarting(lua_toboolean(L, 2) != 0);
    return 0;
}

/// @lua_api Light.Physics.World.GetWarmStarting
/// @brief Phase AP: Query warm-starting flag
/// @return boolean Warm-start flag
static int l_World_GetWarmStarting(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    lua_pushboolean(L, world && world->world->GetWarmStarting());
    return 1;
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
    wrapper->magic = LT::LT_MAGIC_BODY;  // Phase G.1.7.3 — type tag
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

/// @lua_api Light.Physics.World.PreSolve
/// @brief Phase AP: Register PreSolve callback (called synchronously inside Step)
/// @note Callback receives (contact). Use contact:SetEnabled(false) to disable for this step.
/// @note WARNING: Do NOT call CreateBody/DestroyBody/etc inside the callback (Box2D world is locked).
/// @param callback function Callback function or nil to clear
static int l_World_PreSolve(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    if (world->preSolveRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, world->preSolveRef);
        world->preSolveRef = LUA_NOREF;
    }
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        world->preSolveRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

/// @lua_api Light.Physics.World.PostSolve
/// @brief Phase AP: Register PostSolve callback (called synchronously inside Step)
/// @note Callback receives (contact, normalImpulses, tangentImpulses) — useful for impact audio/destruction
/// @note WARNING: Do NOT modify the world inside the callback.
/// @param callback function Callback function or nil to clear
static int l_World_PostSolve(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) return 0;
    if (world->postSolveRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, world->postSolveRef);
        world->postSolveRef = LUA_NOREF;
    }
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        world->postSolveRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
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
/// @brief Current angle in radians. Revolute/Wheel (Phase AP extended)
static int l_Joint_GetJointAngle(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint: v = static_cast<b2RevoluteJoint*>(joint->joint)->GetJointAngle(); break;
    case e_wheelJoint:    v = static_cast<b2WheelJoint*>(joint->joint)->GetJointAngle(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetJointSpeed
/// @brief Current angular speed in rad/s. Revolute/Wheel (Phase AP extended)
static int l_Joint_GetJointSpeed(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint: v = static_cast<b2RevoluteJoint*>(joint->joint)->GetJointSpeed(); break;
    case e_wheelJoint:    v = static_cast<b2WheelJoint*>(joint->joint)->GetJointAngularSpeed(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetJointTranslation
/// @brief Current translation in pixels. Prismatic/Wheel (Phase AP extended)
static int l_Joint_GetJointTranslation(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->GetJointTranslation() * PTM; break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->GetJointTranslation() * PTM; break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetJointLinearSpeed
/// @brief Phase AP: Current linear speed in px/s. Prismatic/Wheel.
static int l_Joint_GetJointLinearSpeed(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->GetJointSpeed() * PTM; break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->GetJointLinearSpeed() * PTM; break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetMotorSpeed
/// @brief Set motor speed. Revolute/Prismatic/Wheel (Phase AP extended)
/// @param speed number Motor speed
static int l_Joint_SetMotorSpeed(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float speed = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  static_cast<b2RevoluteJoint*>(joint->joint)->SetMotorSpeed(speed); break;
    case e_prismaticJoint: static_cast<b2PrismaticJoint*>(joint->joint)->SetMotorSpeed(speed); break;
    case e_wheelJoint:     static_cast<b2WheelJoint*>(joint->joint)->SetMotorSpeed(speed); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.EnableMotor
/// @brief Enable/disable motor. Revolute/Prismatic/Wheel (Phase AP extended)
/// @param enabled boolean Enabled flag
static int l_Joint_EnableMotor(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    bool enabled = lua_toboolean(L, 2) != 0;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  static_cast<b2RevoluteJoint*>(joint->joint)->EnableMotor(enabled); break;
    case e_prismaticJoint: static_cast<b2PrismaticJoint*>(joint->joint)->EnableMotor(enabled); break;
    case e_wheelJoint:     static_cast<b2WheelJoint*>(joint->joint)->EnableMotor(enabled); break;
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

// ============================================================
// Phase AP: MaxForce/MaxTorque (Mouse/Motor/Friction)
// ============================================================

/// @lua_api Light.Physics.Joint.SetMaxForce
/// @brief Phase AP: Set max force. Mouse/Motor/Friction joints only.
/// @param force number Max force
static int l_Joint_SetMaxForce(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float f = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_mouseJoint:    static_cast<b2MouseJoint*>(joint->joint)->SetMaxForce(f); break;
    case e_motorJoint:    static_cast<b2MotorJoint*>(joint->joint)->SetMaxForce(f); break;
    case e_frictionJoint: static_cast<b2FrictionJoint*>(joint->joint)->SetMaxForce(f); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.GetMaxForce
/// @brief Phase AP: Get max force. Mouse/Motor/Friction joints only.
static int l_Joint_GetMaxForce(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_mouseJoint:    v = static_cast<b2MouseJoint*>(joint->joint)->GetMaxForce(); break;
    case e_motorJoint:    v = static_cast<b2MotorJoint*>(joint->joint)->GetMaxForce(); break;
    case e_frictionJoint: v = static_cast<b2FrictionJoint*>(joint->joint)->GetMaxForce(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetMaxTorque
/// @brief Phase AP: Set max torque. Motor/Friction joints only.
/// @param torque number Max torque
static int l_Joint_SetMaxTorque(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float t = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_motorJoint:    static_cast<b2MotorJoint*>(joint->joint)->SetMaxTorque(t); break;
    case e_frictionJoint: static_cast<b2FrictionJoint*>(joint->joint)->SetMaxTorque(t); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.GetMaxTorque
/// @brief Phase AP: Get max torque. Motor/Friction joints only.
static int l_Joint_GetMaxTorque(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_motorJoint:    v = static_cast<b2MotorJoint*>(joint->joint)->GetMaxTorque(); break;
    case e_frictionJoint: v = static_cast<b2FrictionJoint*>(joint->joint)->GetMaxTorque(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

// ============================================================
// Phase AP: MotorJoint 专属 (Linear/Angular offset, correction)
// ============================================================

static b2MotorJoint* CheckMotorJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_motorJoint) return nullptr;
    return static_cast<b2MotorJoint*>(joint->joint);
}

/// @lua_api Light.Physics.Joint.SetLinearOffset
/// @brief Phase AP: MotorJoint only. Set target linear offset in pixels.
/// @param x number X offset in pixels
/// @param y number Y offset in pixels
static int l_Joint_SetLinearOffset(lua_State* L) {
    auto* mj = CheckMotorJoint(L, 1);
    if (!mj) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    mj->SetLinearOffset(ToMeters(x, y));
    return 0;
}

/// @lua_api Light.Physics.Joint.GetLinearOffset
/// @brief Phase AP: MotorJoint only. Returns (x, y) in pixels.
/// @return number, number Offset x, y in pixels
static int l_Joint_GetLinearOffset(lua_State* L) {
    auto* mj = CheckMotorJoint(L, 1);
    if (!mj) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    PushPixels(L, mj->GetLinearOffset());
    return 2;
}

/// @lua_api Light.Physics.Joint.SetAngularOffset
/// @brief Phase AP: MotorJoint only. Set target angular offset in radians.
/// @param angle number Target angle in radians
static int l_Joint_SetAngularOffset(lua_State* L) {
    auto* mj = CheckMotorJoint(L, 1);
    if (!mj) return 0;
    mj->SetAngularOffset((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Joint.GetAngularOffset
/// @brief Phase AP: MotorJoint only. Returns angular offset in radians.
static int l_Joint_GetAngularOffset(lua_State* L) {
    auto* mj = CheckMotorJoint(L, 1);
    lua_pushnumber(L, mj ? mj->GetAngularOffset() : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetCorrectionFactor
/// @brief Phase AP: MotorJoint only. Set position correction factor (0..1).
/// @param factor number Correction factor
static int l_Joint_SetCorrectionFactor(lua_State* L) {
    auto* mj = CheckMotorJoint(L, 1);
    if (!mj) return 0;
    mj->SetCorrectionFactor((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Joint.GetCorrectionFactor
/// @brief Phase AP: MotorJoint only. Returns correction factor.
static int l_Joint_GetCorrectionFactor(lua_State* L) {
    auto* mj = CheckMotorJoint(L, 1);
    lua_pushnumber(L, mj ? mj->GetCorrectionFactor() : 0.0);
    return 1;
}

// ============================================================
// Phase AP: PulleyJoint 专属
// ============================================================

static b2PulleyJoint* CheckPulleyJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_pulleyJoint) return nullptr;
    return static_cast<b2PulleyJoint*>(joint->joint);
}

/// @lua_api Light.Physics.Joint.GetGroundAnchorA
/// @brief Phase AP: PulleyJoint only. Returns ground anchor A (x, y) in pixels.
static int l_Joint_GetGroundAnchorA(lua_State* L) {
    auto* pj = CheckPulleyJoint(L, 1);
    if (!pj) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    PushPixels(L, pj->GetGroundAnchorA());
    return 2;
}

/// @lua_api Light.Physics.Joint.GetGroundAnchorB
/// @brief Phase AP: PulleyJoint only. Returns ground anchor B (x, y) in pixels.
static int l_Joint_GetGroundAnchorB(lua_State* L) {
    auto* pj = CheckPulleyJoint(L, 1);
    if (!pj) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    PushPixels(L, pj->GetGroundAnchorB());
    return 2;
}

/// @lua_api Light.Physics.Joint.GetLengthA
/// @brief Phase AP: PulleyJoint only. Returns reference length A in pixels.
static int l_Joint_GetLengthA(lua_State* L) {
    auto* pj = CheckPulleyJoint(L, 1);
    lua_pushnumber(L, pj ? (pj->GetLengthA() * PTM) : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetLengthB
/// @brief Phase AP: PulleyJoint only. Returns reference length B in pixels.
static int l_Joint_GetLengthB(lua_State* L) {
    auto* pj = CheckPulleyJoint(L, 1);
    lua_pushnumber(L, pj ? (pj->GetLengthB() * PTM) : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetCurrentLengthA
/// @brief Phase AP: PulleyJoint only. Returns current length A in pixels.
static int l_Joint_GetCurrentLengthA(lua_State* L) {
    auto* pj = CheckPulleyJoint(L, 1);
    lua_pushnumber(L, pj ? (pj->GetCurrentLengthA() * PTM) : 0.0);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetCurrentLengthB
/// @brief Phase AP: PulleyJoint only. Returns current length B in pixels.
static int l_Joint_GetCurrentLengthB(lua_State* L) {
    auto* pj = CheckPulleyJoint(L, 1);
    lua_pushnumber(L, pj ? (pj->GetCurrentLengthB() * PTM) : 0.0);
    return 1;
}

// ============================================================
// Phase AP: GearJoint 专属
// ============================================================

static b2GearJoint* CheckGearJoint(lua_State* L, int idx) {
    auto* joint = CheckJoint(L, idx);
    if (!joint || joint->joint->GetType() != e_gearJoint) return nullptr;
    return static_cast<b2GearJoint*>(joint->joint);
}

/// @lua_api Light.Physics.Joint.SetRatio
/// @brief Phase AP: GearJoint/PulleyJoint ratio cannot be changed (Pulley). For Gear: set ratio.
/// @param ratio number New ratio
static int l_Joint_SetRatio(lua_State* L) {
    auto* gj = CheckGearJoint(L, 1);
    if (!gj) return 0;
    gj->SetRatio((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Joint.GetRatio
/// @brief Phase AP: Returns ratio. GearJoint or PulleyJoint.
static int l_Joint_GetRatio(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_gearJoint:   v = static_cast<b2GearJoint*>(joint->joint)->GetRatio(); break;
    case e_pulleyJoint: v = static_cast<b2PulleyJoint*>(joint->joint)->GetRatio(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

// 辅助: 从 b2Joint* 反查其 Lua joint table 并压栈。未命中或已销毁 push nil
static void PushJointFromRaw(lua_State* L, PhysicsWorld* world, b2Joint* raw) {
    if (!raw || !world) { lua_pushnil(L); return; }
    PhysicsJoint* wrapper = reinterpret_cast<PhysicsJoint*>(raw->GetUserData().pointer);
    if (!wrapper || !wrapper->alive || wrapper->selfRef == LUA_NOREF) {
        lua_pushnil(L);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, wrapper->selfRef);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

/// @lua_api Light.Physics.Joint.GetJoint1
/// @brief Phase AP: GearJoint only. Returns the first coupled joint.
static int l_Joint_GetJoint1(lua_State* L) {
    auto* gj = CheckGearJoint(L, 1);
    auto* wrapper = CheckJoint(L, 1);
    if (!gj || !wrapper) { lua_pushnil(L); return 1; }
    PushJointFromRaw(L, wrapper->owner, gj->GetJoint1());
    return 1;
}

/// @lua_api Light.Physics.Joint.GetJoint2
/// @brief Phase AP: GearJoint only. Returns the second coupled joint.
static int l_Joint_GetJoint2(lua_State* L) {
    auto* gj = CheckGearJoint(L, 1);
    auto* wrapper = CheckJoint(L, 1);
    if (!gj || !wrapper) { lua_pushnil(L); return 1; }
    PushJointFromRaw(L, wrapper->owner, gj->GetJoint2());
    return 1;
}

// ============================================================
// Phase AP: Joint Limits (Revolute + Prismatic + Wheel)
// ============================================================

/// @lua_api Light.Physics.Joint.EnableLimit
/// @brief Phase AP: Enable/disable joint motion limits (Revolute/Prismatic/Wheel)
/// @param enabled boolean Enabled flag
static int l_Joint_EnableLimit(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    bool enabled = lua_toboolean(L, 2) != 0;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  static_cast<b2RevoluteJoint*>(joint->joint)->EnableLimit(enabled); break;
    case e_prismaticJoint: static_cast<b2PrismaticJoint*>(joint->joint)->EnableLimit(enabled); break;
    case e_wheelJoint:     static_cast<b2WheelJoint*>(joint->joint)->EnableLimit(enabled); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.IsLimitEnabled
/// @brief Phase AP: Query whether motion limits are enabled
/// @return boolean Enabled flag
static int l_Joint_IsLimitEnabled(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushboolean(L, 0); return 1; }
    bool v = false;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  v = static_cast<b2RevoluteJoint*>(joint->joint)->IsLimitEnabled(); break;
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->IsLimitEnabled(); break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->IsLimitEnabled(); break;
    default: break;
    }
    lua_pushboolean(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetLimits
/// @brief Phase AP: Set joint motion limits. Revolute: radians. Prismatic/Wheel: pixels.
/// @param lower number Lower limit (radians or pixels depending on joint type)
/// @param upper number Upper limit
static int l_Joint_SetLimits(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float lo = (float)luaL_checknumber(L, 2);
    float hi = (float)luaL_checknumber(L, 3);
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:
        static_cast<b2RevoluteJoint*>(joint->joint)->SetLimits(lo, hi);
        break;
    case e_prismaticJoint:
        static_cast<b2PrismaticJoint*>(joint->joint)->SetLimits(lo / PTM, hi / PTM);
        break;
    case e_wheelJoint:
        static_cast<b2WheelJoint*>(joint->joint)->SetLimits(lo / PTM, hi / PTM);
        break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.GetLowerLimit
/// @brief Phase AP: Get lower motion limit (radians or pixels)
/// @return number Lower limit
static int l_Joint_GetLowerLimit(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  v = static_cast<b2RevoluteJoint*>(joint->joint)->GetLowerLimit(); break;
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->GetLowerLimit() * PTM; break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->GetLowerLimit() * PTM; break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetUpperLimit
/// @brief Phase AP: Get upper motion limit (radians or pixels)
/// @return number Upper limit
static int l_Joint_GetUpperLimit(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  v = static_cast<b2RevoluteJoint*>(joint->joint)->GetUpperLimit(); break;
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->GetUpperLimit() * PTM; break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->GetUpperLimit() * PTM; break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

// ============================================================
// Phase AP: Motor 扩展 (SetMaxMotorTorque/Force + GetMotor*)
// ============================================================

/// @lua_api Light.Physics.Joint.IsMotorEnabled
/// @brief Phase AP: Query motor enabled state (Revolute/Prismatic/Wheel)
/// @return boolean Enabled flag
static int l_Joint_IsMotorEnabled(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushboolean(L, 0); return 1; }
    bool v = false;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  v = static_cast<b2RevoluteJoint*>(joint->joint)->IsMotorEnabled(); break;
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->IsMotorEnabled(); break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->IsMotorEnabled(); break;
    default: break;
    }
    lua_pushboolean(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.GetMotorSpeed
/// @brief Phase AP: Get current motor speed setting (Revolute/Prismatic/Wheel)
/// @return number Motor speed
static int l_Joint_GetMotorSpeed(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint:  v = static_cast<b2RevoluteJoint*>(joint->joint)->GetMotorSpeed(); break;
    case e_prismaticJoint: v = static_cast<b2PrismaticJoint*>(joint->joint)->GetMotorSpeed(); break;
    case e_wheelJoint:     v = static_cast<b2WheelJoint*>(joint->joint)->GetMotorSpeed(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetMaxMotorTorque
/// @brief Phase AP: Set max motor torque (Revolute/Wheel only)
/// @param torque number Max motor torque
static int l_Joint_SetMaxMotorTorque(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float t = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_revoluteJoint: static_cast<b2RevoluteJoint*>(joint->joint)->SetMaxMotorTorque(t); break;
    case e_wheelJoint:    static_cast<b2WheelJoint*>(joint->joint)->SetMaxMotorTorque(t); break;
    default: break;
    }
    return 0;
}

/// @lua_api Light.Physics.Joint.GetMotorTorque
/// @brief Phase AP: Get current motor torque (Revolute/Wheel only)
/// @param invDt number Inverse delta time
/// @return number Motor torque
static int l_Joint_GetMotorTorque(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float invDt = (float)luaL_optnumber(L, 2, 60.0);
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_revoluteJoint: v = static_cast<b2RevoluteJoint*>(joint->joint)->GetMotorTorque(invDt); break;
    case e_wheelJoint:    v = static_cast<b2WheelJoint*>(joint->joint)->GetMotorTorque(invDt); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetMaxMotorForce
/// @brief Phase AP: Set max motor force (Prismatic only)
/// @param force number Max motor force
static int l_Joint_SetMaxMotorForce(lua_State* L) {
    auto* pj = CheckPrismaticJoint(L, 1);
    if (!pj) return 0;
    pj->SetMaxMotorForce((float)luaL_checknumber(L, 2));
    return 0;
}

/// @lua_api Light.Physics.Joint.GetMotorForce
/// @brief Phase AP: Get current motor force (Prismatic only)
/// @param invDt number Inverse delta time
/// @return number Motor force
static int l_Joint_GetMotorForce(lua_State* L) {
    auto* pj = CheckPrismaticJoint(L, 1);
    if (!pj) { lua_pushnumber(L, 0); return 1; }
    float invDt = (float)luaL_optnumber(L, 2, 60.0);
    lua_pushnumber(L, pj->GetMotorForce(invDt));
    return 1;
}

// ============================================================
// Phase AP: Spring API (Distance/Weld/Mouse/Wheel)
// ============================================================

// 辅助: 对带 SetStiffness 的 4 种 joint 类型做分派
static int l_Joint_SetStiffness(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float s = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_distanceJoint: static_cast<b2DistanceJoint*>(joint->joint)->SetStiffness(s); break;
    case e_weldJoint:     static_cast<b2WeldJoint*>(joint->joint)->SetStiffness(s); break;
    case e_mouseJoint:    static_cast<b2MouseJoint*>(joint->joint)->SetStiffness(s); break;
    case e_wheelJoint:    static_cast<b2WheelJoint*>(joint->joint)->SetStiffness(s); break;
    default: break;
    }
    return 0;
}

static int l_Joint_GetStiffness(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_distanceJoint: v = static_cast<b2DistanceJoint*>(joint->joint)->GetStiffness(); break;
    case e_weldJoint:     v = static_cast<b2WeldJoint*>(joint->joint)->GetStiffness(); break;
    case e_mouseJoint:    v = static_cast<b2MouseJoint*>(joint->joint)->GetStiffness(); break;
    case e_wheelJoint:    v = static_cast<b2WheelJoint*>(joint->joint)->GetStiffness(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

static int l_Joint_SetDamping(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float d = (float)luaL_checknumber(L, 2);
    switch (joint->joint->GetType()) {
    case e_distanceJoint: static_cast<b2DistanceJoint*>(joint->joint)->SetDamping(d); break;
    case e_weldJoint:     static_cast<b2WeldJoint*>(joint->joint)->SetDamping(d); break;
    case e_mouseJoint:    static_cast<b2MouseJoint*>(joint->joint)->SetDamping(d); break;
    case e_wheelJoint:    static_cast<b2WheelJoint*>(joint->joint)->SetDamping(d); break;
    default: break;
    }
    return 0;
}

static int l_Joint_GetDamping(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) { lua_pushnumber(L, 0); return 1; }
    float v = 0.0f;
    switch (joint->joint->GetType()) {
    case e_distanceJoint: v = static_cast<b2DistanceJoint*>(joint->joint)->GetDamping(); break;
    case e_weldJoint:     v = static_cast<b2WeldJoint*>(joint->joint)->GetDamping(); break;
    case e_mouseJoint:    v = static_cast<b2MouseJoint*>(joint->joint)->GetDamping(); break;
    case e_wheelJoint:    v = static_cast<b2WheelJoint*>(joint->joint)->GetDamping(); break;
    default: break;
    }
    lua_pushnumber(L, v);
    return 1;
}

/// @lua_api Light.Physics.Joint.SetSpring
/// @brief Phase AP: Convenience helper. Convert (frequencyHz, dampingRatio) to stiffness+damping via b2LinearStiffness.
/// @note Only applies to Distance/Weld/Mouse/Wheel joints. No-op for others.
/// @param frequencyHz number Natural frequency in Hz (e.g. 4.0)
/// @param dampingRatio number Damping ratio (0 = undamped, 1 = critically damped)
static int l_Joint_SetSpring(lua_State* L) {
    auto* joint = CheckJoint(L, 1);
    if (!joint) return 0;
    float hz = (float)luaL_checknumber(L, 2);
    float ratio = (float)luaL_checknumber(L, 3);
    b2Body* a = joint->joint->GetBodyA();
    b2Body* b = joint->joint->GetBodyB();
    if (!a || !b) return 0;
    float stiffness = 0.0f, damping = 0.0f;
    b2LinearStiffness(stiffness, damping, hz, ratio, a, b);
    switch (joint->joint->GetType()) {
    case e_distanceJoint: {
        auto* dj = static_cast<b2DistanceJoint*>(joint->joint);
        dj->SetStiffness(stiffness);
        dj->SetDamping(damping);
        break;
    }
    case e_weldJoint: {
        auto* wj = static_cast<b2WeldJoint*>(joint->joint);
        wj->SetStiffness(stiffness);
        wj->SetDamping(damping);
        break;
    }
    case e_mouseJoint: {
        auto* mj = static_cast<b2MouseJoint*>(joint->joint);
        mj->SetStiffness(stiffness);
        mj->SetDamping(damping);
        break;
    }
    case e_wheelJoint: {
        auto* whj = static_cast<b2WheelJoint*>(joint->joint);
        whj->SetStiffness(stiffness);
        whj->SetDamping(damping);
        break;
    }
    default: break;
    }
    return 0;
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
    // Phase AP: Limits
    {"EnableLimit",         l_Joint_EnableLimit},
    {"IsLimitEnabled",      l_Joint_IsLimitEnabled},
    {"SetLimits",           l_Joint_SetLimits},
    {"GetLowerLimit",       l_Joint_GetLowerLimit},
    {"GetUpperLimit",       l_Joint_GetUpperLimit},
    // Phase AP: Motor 扩展
    {"IsMotorEnabled",      l_Joint_IsMotorEnabled},
    {"GetMotorSpeed",       l_Joint_GetMotorSpeed},
    {"SetMaxMotorTorque",   l_Joint_SetMaxMotorTorque},
    {"GetMotorTorque",      l_Joint_GetMotorTorque},
    {"SetMaxMotorForce",    l_Joint_SetMaxMotorForce},
    {"GetMotorForce",       l_Joint_GetMotorForce},
    // Phase AP: Spring (Distance/Weld/Mouse/Wheel)
    {"SetStiffness",        l_Joint_SetStiffness},
    {"GetStiffness",        l_Joint_GetStiffness},
    {"SetDamping",          l_Joint_SetDamping},
    {"GetDamping",          l_Joint_GetDamping},
    {"SetSpring",           l_Joint_SetSpring},
    // Phase AP: MaxForce/MaxTorque (Mouse/Motor/Friction)
    {"SetMaxForce",         l_Joint_SetMaxForce},
    {"GetMaxForce",         l_Joint_GetMaxForce},
    {"SetMaxTorque",        l_Joint_SetMaxTorque},
    {"GetMaxTorque",        l_Joint_GetMaxTorque},
    // Phase AP: Wheel/Prismatic
    {"GetJointLinearSpeed", l_Joint_GetJointLinearSpeed},
    // Phase AP: MotorJoint 专属
    {"SetLinearOffset",     l_Joint_SetLinearOffset},
    {"GetLinearOffset",     l_Joint_GetLinearOffset},
    {"SetAngularOffset",    l_Joint_SetAngularOffset},
    {"GetAngularOffset",    l_Joint_GetAngularOffset},
    {"SetCorrectionFactor", l_Joint_SetCorrectionFactor},
    {"GetCorrectionFactor", l_Joint_GetCorrectionFactor},
    // Phase AP: PulleyJoint 专属
    {"GetGroundAnchorA",    l_Joint_GetGroundAnchorA},
    {"GetGroundAnchorB",    l_Joint_GetGroundAnchorB},
    {"GetLengthA",          l_Joint_GetLengthA},
    {"GetLengthB",          l_Joint_GetLengthB},
    {"GetCurrentLengthA",   l_Joint_GetCurrentLengthA},
    {"GetCurrentLengthB",   l_Joint_GetCurrentLengthB},
    // Phase AP: GearJoint 专属
    {"SetRatio",            l_Joint_SetRatio},
    {"GetRatio",            l_Joint_GetRatio},
    {"GetJoint1",           l_Joint_GetJoint1},
    {"GetJoint2",           l_Joint_GetJoint2},
    {"__tostring",          l_Joint_Tostring},
    {NULL, NULL}
};

static PhysicsJoint* PushJointTable(lua_State* L, PhysicsWorld* world, b2Joint* joint) {
    lua_createtable(L, 0, 8);
    void* storage = lua_newuserdata(L, sizeof(PhysicsJoint));
    auto* wrapper = new (storage) PhysicsJoint();
    wrapper->magic = LT::LT_MAGIC_JOINT;  // Phase G.1.7.3 — type tag
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

// ============================================================
// Phase AP: 5 种新 Joint 创建函数
// ============================================================

/// @lua_api Light.Physics.World.CreateWheelJoint
/// @brief Phase AP: Create a wheel joint — translating along an axis + limited rotation (vehicles)
/// @param bodyA table Body A (chassis)
/// @param bodyB table Body B (wheel)
/// @param ax number Anchor world X in pixels
/// @param ay number Anchor world Y in pixels
/// @param axisX number Axis direction X (suspension direction, unitless)
/// @param axisY number Axis direction Y
/// @param collideConnected boolean Default false
/// @return table Joint
static int l_World_CreateWheelJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2Vec2 axis((float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7));
    axis.Normalize();
    b2WheelJointDef def;
    def.Initialize(a, b, ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)), axis);
    def.collideConnected = lua_toboolean(L, 8) != 0;
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreateMotorJoint
/// @brief Phase AP: Create a motor joint to control relative offset+angle between two bodies (no anchor)
/// @param bodyA table Body A (reference)
/// @param bodyB table Body B (driven)
/// @param correctionFactor number Position correction factor (0..1, default 0.3)
/// @return table Joint
static int l_World_CreateMotorJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2MotorJointDef def;
    def.Initialize(a, b);
    def.correctionFactor = (float)luaL_optnumber(L, 4, 0.3);
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreateFrictionJoint
/// @brief Phase AP: Create a friction joint to simulate top-down friction between two bodies
/// @param bodyA table Body A
/// @param bodyB table Body B
/// @param ax number Anchor world X in pixels
/// @param ay number Anchor world Y in pixels
/// @param collideConnected boolean Default false
/// @return table Joint
static int l_World_CreateFrictionJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2FrictionJointDef def;
    def.Initialize(a, b, ToMeters((float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)));
    def.collideConnected = lua_toboolean(L, 6) != 0;
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreatePulleyJoint
/// @brief Phase AP: Create a pulley joint. lengthA + ratio*lengthB remains constant.
/// @param bodyA table Body A
/// @param bodyB table Body B
/// @param groundAx number Ground anchor A world X in pixels
/// @param groundAy number Ground anchor A world Y in pixels
/// @param groundBx number Ground anchor B world X in pixels
/// @param groundBy number Ground anchor B world Y in pixels
/// @param anchorAx number Anchor on body A world X in pixels
/// @param anchorAy number Anchor on body A world Y in pixels
/// @param anchorBx number Anchor on body B world X in pixels
/// @param anchorBy number Anchor on body B world Y in pixels
/// @param ratio number Ratio > 0
/// @return table Joint
static int l_World_CreatePulleyJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    b2Body *a = nullptr, *b = nullptr;
    if (!ResolveBodyPair(L, 2, 3, world, &a, &b)) { lua_pushnil(L); return 1; }
    b2Vec2 gA = ToMeters((float)luaL_checknumber(L, 4),  (float)luaL_checknumber(L, 5));
    b2Vec2 gB = ToMeters((float)luaL_checknumber(L, 6),  (float)luaL_checknumber(L, 7));
    b2Vec2 anA = ToMeters((float)luaL_checknumber(L, 8),  (float)luaL_checknumber(L, 9));
    b2Vec2 anB = ToMeters((float)luaL_checknumber(L, 10), (float)luaL_checknumber(L, 11));
    float ratio = (float)luaL_optnumber(L, 12, 1.0);
    if (ratio <= 0.0f) { lua_pushnil(L); return 1; }  // Box2D 要求 > 0
    b2PulleyJointDef def;
    def.Initialize(a, b, gA, gB, anA, anB, ratio);
    b2Joint* joint = world->world->CreateJoint(&def);
    if (!joint) { lua_pushnil(L); return 1; }
    PushJointTable(L, world, joint);
    return 1;
}

/// @lua_api Light.Physics.World.CreateGearJoint
/// @brief Phase AP: Couple two revolute/prismatic joints with a ratio (gear)
/// @note joint1 and joint2 MUST be either revolute or prismatic; must outlive the gear joint.
/// @param joint1 table A revolute or prismatic joint
/// @param joint2 table A revolute or prismatic joint
/// @param ratio number Gear ratio (default 1.0)
/// @return table Gear joint, or nil if joint types invalid
static int l_World_CreateGearJoint(lua_State* L) {
    auto* world = CheckWorld(L, 1);
    if (!world) { lua_pushnil(L); return 1; }
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    auto* j1 = CheckJoint(L, 2);
    auto* j2 = CheckJoint(L, 3);
    if (!j1 || !j2 || j1->owner != world || j2->owner != world) { lua_pushnil(L); return 1; }
    // 只支持 revolute/prismatic
    auto valid_type = [](b2JointType t) { return t == e_revoluteJoint || t == e_prismaticJoint; };
    if (!valid_type(j1->joint->GetType()) || !valid_type(j2->joint->GetType())) {
        lua_pushnil(L);
        return 1;
    }
    b2GearJointDef def;
    def.bodyA = j1->joint->GetBodyB();   // Box2D 约定: gear 作用 body 是 joint.bodyB
    def.bodyB = j2->joint->GetBodyB();
    def.joint1 = j1->joint;
    def.joint2 = j2->joint;
    def.ratio = (float)luaL_optnumber(L, 4, 1.0);
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
    {"PreSolve",             l_World_PreSolve},   // Phase AP
    {"PostSolve",            l_World_PostSolve},  // Phase AP
    // Phase AO: joints + queries
    {"CreateDistanceJoint",  l_World_CreateDistanceJoint},
    {"CreateRevoluteJoint",  l_World_CreateRevoluteJoint},
    {"CreatePrismaticJoint", l_World_CreatePrismaticJoint},
    {"CreateWeldJoint",      l_World_CreateWeldJoint},
    {"CreateMouseJoint",     l_World_CreateMouseJoint},
    // Phase AP
    {"CreateWheelJoint",     l_World_CreateWheelJoint},
    {"CreateMotorJoint",     l_World_CreateMotorJoint},
    {"CreateFrictionJoint",  l_World_CreateFrictionJoint},
    {"CreatePulleyJoint",    l_World_CreatePulleyJoint},
    {"CreateGearJoint",      l_World_CreateGearJoint},
    {"DestroyJoint",         l_World_DestroyJoint},
    {"GetJointCount",        l_World_GetJointCount},
    {"RayCast",              l_World_RayCast},
    {"QueryAABB",            l_World_QueryAABB},
    // Phase AP: 仿真控制
    {"SetSubStepping",       l_World_SetSubStepping},
    {"GetSubStepping",       l_World_GetSubStepping},
    {"SetWarmStarting",      l_World_SetWarmStarting},
    {"GetWarmStarting",      l_World_GetWarmStarting},
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
