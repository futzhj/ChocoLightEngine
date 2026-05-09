/**
 * @file light_physics3d.cpp
 * @brief Light.Physics3D — 3D 物理 (基于 Bullet 3.25)
 *
 * Phase AU — 引入 Bullet Physics 3D, 与现有 Box2D 2D 物理 (Light.Physics) 平行.
 *
 * Lua API:
 *   形状 (静态构造):
 *     Light.Physics3D.NewBox(hx, hy, hz)              -- 半边长
 *     Light.Physics3D.NewSphere(radius)
 *     Light.Physics3D.NewCylinder(radius, halfHeight) -- Y 轴对齐
 *     Light.Physics3D.NewCapsule(radius, halfHeight)
 *     Light.Physics3D.NewCone(radius, height)
 *     Light.Physics3D.NewStaticPlane(nx, ny, nz, dist)
 *
 *   World:
 *     Light.Physics3D.NewWorld([gx=0, gy=-9.81, gz=0]) -> World
 *     world:SetGravity(x, y, z)
 *     world:GetGravity() -> x, y, z
 *     world:Step(dt, [maxSubSteps=10], [fixedTimeStep=1/60])
 *     world:CreateBody({type="dynamic"|"static"|"kinematic", mass=1, x=0, y=0, z=0, shape=...,
 *                       friction=0.5, restitution=0, linDamp=0, angDamp=0}) -> Body
 *     world:DestroyBody(body)
 *     world:GetBodyCount() -> int
 *     world:RayCast(x1,y1,z1, x2,y2,z2) -> body|nil, hx,hy,hz, nx,ny,nz, fraction
 *     world:OnContact(fn|nil) -- fn(bodyA, bodyB) 在 Step 内同步触发
 *     world:Delete() / __gc
 *
 *   Body:
 *     body:Get/SetPosition() / Rotation (quaternion: w,x,y,z)
 *     body:Get/SetLinearVelocity / AngularVelocity
 *     body:ApplyForce(fx, fy, fz, [relX, relY, relZ])  -- relative point in body local space
 *     body:ApplyCentralForce/Impulse/Torque/TorqueImpulse(x, y, z)
 *     body:Get/SetMass / Friction / Restitution / LinearDamping / AngularDamping
 *     body:SetGravity(gx, gy, gz) / GetGravity()  -- per-body override
 *     body:SetCcdMotionThreshold(t) / SetCcdSweptSphereRadius(r)  -- 连续碰撞检测
 *     body:Activate([forceActivation]) / IsActive()
 *     body:SetKinematic(bool) / IsKinematic()
 *     body:GetTransform() -> 16 floats (column-major 4x4 matrix)
 *     body:Delete() / __gc / __tostring
 *
 * 内部:
 *   - 单线程, Lua 主线程内 step
 *   - btRigidBody userdata 由 World 持有 (registry ref), DestroyBody / __gc 时移除
 *   - Body 的 selfRef 用于 OnContact 回调中查 body Lua object
 *   - Shape userdata 不持有 btCollisionShape, 由 Body 共享; CreateBody 时引用计数 +1, body 销毁时 -1
 *     -- 简化: 暂不做共享, Body 持有 shape userdata 的 Lua ref 防 GC, body 销毁时 unref
 */

#include "light.h"

#if CHOCO_HAS_BULLET
#include <btBulletCollisionCommon.h>
#include <btBulletDynamicsCommon.h>
#endif

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <new>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#if CHOCO_HAS_BULLET

// ==================== userdata 元表名 ====================

static const char* WORLD_MT = "Light.Physics3D.World";
static const char* BODY_MT  = "Light.Physics3D.Body";
static const char* SHAPE_MT = "Light.Physics3D.Shape";

// ==================== userdata 结构 ====================

struct World3D;
struct Body3D;

struct Shape3D {
    btCollisionShape* shape;
    Shape3D() : shape(nullptr) {}
};

struct Body3D {
    btRigidBody*     body;
    btMotionState*   motion;
    World3D*         owner;
    int              shapeRef;   // 防 shape userdata GC
    int              selfRef;    // OnContact 回调里反查 Lua body
    bool             alive;
    Body3D() : body(nullptr), motion(nullptr), owner(nullptr),
               shapeRef(LUA_NOREF), selfRef(LUA_NOREF), alive(false) {}
};

struct World3D {
    btDefaultCollisionConfiguration*     config;
    btCollisionDispatcher*               dispatcher;
    btBroadphaseInterface*               broadphase;
    btSequentialImpulseConstraintSolver* solver;
    btDiscreteDynamicsWorld*             world;
    std::vector<Body3D*>                 bodies;
    int                                  contactRef;  // OnContact callback in registry
    lua_State*                           L;
    bool                                 alive;
    World3D() : config(nullptr), dispatcher(nullptr), broadphase(nullptr),
                solver(nullptr), world(nullptr), contactRef(LUA_NOREF),
                L(nullptr), alive(false) {}
};

// ==================== Helpers ====================

static Shape3D* CheckShape(lua_State* L, int idx) {
    return (Shape3D*)luaL_checkudata(L, idx, SHAPE_MT);
}
static Body3D* CheckBody(lua_State* L, int idx) {
    return (Body3D*)luaL_checkudata(L, idx, BODY_MT);
}
static World3D* CheckWorld(lua_State* L, int idx) {
    return (World3D*)luaL_checkudata(L, idx, WORLD_MT);
}

static void PushVec3(lua_State* L, const btVector3& v) {
    lua_pushnumber(L, v.x());
    lua_pushnumber(L, v.y());
    lua_pushnumber(L, v.z());
}

static btVector3 ReadVec3(lua_State* L, int idx) {
    float x = (float)luaL_checknumber(L, idx);
    float y = (float)luaL_checknumber(L, idx + 1);
    float z = (float)luaL_checknumber(L, idx + 2);
    return btVector3(x, y, z);
}

// ==================== Shape 工厂 ====================

static int PushShapeUserdata(lua_State* L, btCollisionShape* shape) {
    Shape3D* ud = (Shape3D*)lua_newuserdata(L, sizeof(Shape3D));
    new (ud) Shape3D();
    ud->shape = shape;
    luaL_getmetatable(L, SHAPE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_NewBox(lua_State* L) {
    float hx = (float)luaL_checknumber(L, 1);
    float hy = (float)luaL_checknumber(L, 2);
    float hz = (float)luaL_checknumber(L, 3);
    return PushShapeUserdata(L, new btBoxShape(btVector3(hx, hy, hz)));
}

static int l_NewSphere(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    return PushShapeUserdata(L, new btSphereShape(r));
}

static int l_NewCylinder(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float hh = (float)luaL_checknumber(L, 2);
    return PushShapeUserdata(L, new btCylinderShape(btVector3(r, hh, r)));
}

static int l_NewCapsule(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float hh = (float)luaL_checknumber(L, 2);
    return PushShapeUserdata(L, new btCapsuleShape(r, hh * 2.0f));
}

static int l_NewCone(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float h = (float)luaL_checknumber(L, 2);
    return PushShapeUserdata(L, new btConeShape(r, h));
}

static int l_NewStaticPlane(lua_State* L) {
    float nx = (float)luaL_checknumber(L, 1);
    float ny = (float)luaL_checknumber(L, 2);
    float nz = (float)luaL_checknumber(L, 3);
    float d  = (float)luaL_optnumber(L, 4, 0.0);
    return PushShapeUserdata(L, new btStaticPlaneShape(btVector3(nx, ny, nz), d));
}

static int l_Shape_GC(lua_State* L) {
    Shape3D* s = CheckShape(L, 1);
    if (s->shape) {
        delete s->shape;
        s->shape = nullptr;
    }
    return 0;
}

static int l_Shape_Tostring(lua_State* L) {
    Shape3D* s = CheckShape(L, 1);
    lua_pushfstring(L, "Light.Physics3D.Shape(%p)", s->shape);
    return 1;
}

// ==================== Body 实现 ====================

static int l_Body_GetPosition(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) { lua_pushnumber(L,0); lua_pushnumber(L,0); lua_pushnumber(L,0); return 3; }
    btTransform t;
    b->motion->getWorldTransform(t);
    PushVec3(L, t.getOrigin());
    return 3;
}

static int l_Body_SetPosition(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    btVector3 p = ReadVec3(L, 2);
    btTransform t;
    b->motion->getWorldTransform(t);
    t.setOrigin(p);
    b->motion->setWorldTransform(t);
    b->body->setWorldTransform(t);
    b->body->activate(true);
    return 0;
}

static int l_Body_GetRotation(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) {
        lua_pushnumber(L, 1); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 4;
    }
    btTransform t;
    b->motion->getWorldTransform(t);
    btQuaternion q = t.getRotation();
    lua_pushnumber(L, q.w());
    lua_pushnumber(L, q.x());
    lua_pushnumber(L, q.y());
    lua_pushnumber(L, q.z());
    return 4;
}

static int l_Body_SetRotation(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    float qw = (float)luaL_checknumber(L, 2);
    float qx = (float)luaL_checknumber(L, 3);
    float qy = (float)luaL_checknumber(L, 4);
    float qz = (float)luaL_checknumber(L, 5);
    btTransform t;
    b->motion->getWorldTransform(t);
    t.setRotation(btQuaternion(qx, qy, qz, qw));
    b->motion->setWorldTransform(t);
    b->body->setWorldTransform(t);
    b->body->activate(true);
    return 0;
}

static int l_Body_GetTransform(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) {
        for (int i = 0; i < 16; i++) {
            lua_pushnumber(L, (i % 5 == 0) ? 1.0 : 0.0);  // identity
        }
        return 16;
    }
    btTransform t;
    b->motion->getWorldTransform(t);
    btScalar m[16];
    t.getOpenGLMatrix(m);  // column-major OpenGL 4x4
    for (int i = 0; i < 16; i++) lua_pushnumber(L, m[i]);
    return 16;
}

static int l_Body_GetLinearVelocity(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) { lua_pushnumber(L,0); lua_pushnumber(L,0); lua_pushnumber(L,0); return 3; }
    PushVec3(L, b->body->getLinearVelocity());
    return 3;
}

static int l_Body_SetLinearVelocity(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setLinearVelocity(ReadVec3(L, 2));
    b->body->activate(true);
    return 0;
}

static int l_Body_GetAngularVelocity(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) { lua_pushnumber(L,0); lua_pushnumber(L,0); lua_pushnumber(L,0); return 3; }
    PushVec3(L, b->body->getAngularVelocity());
    return 3;
}

static int l_Body_SetAngularVelocity(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setAngularVelocity(ReadVec3(L, 2));
    b->body->activate(true);
    return 0;
}

// ApplyForce(fx,fy,fz, [relX,relY,relZ])
// - 3 参: 相当于 ApplyCentralForce
// - 6 参: 在 body local space relPos 处施加
static int l_Body_ApplyForce(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    btVector3 f = ReadVec3(L, 2);
    if (lua_gettop(L) >= 7) {
        btVector3 rel = ReadVec3(L, 5);
        b->body->applyForce(f, rel);
    } else {
        b->body->applyCentralForce(f);
    }
    b->body->activate(true);
    return 0;
}

static int l_Body_ApplyCentralForce(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->applyCentralForce(ReadVec3(L, 2));
    b->body->activate(true);
    return 0;
}

static int l_Body_ApplyImpulse(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    btVector3 imp = ReadVec3(L, 2);
    if (lua_gettop(L) >= 7) {
        btVector3 rel = ReadVec3(L, 5);
        b->body->applyImpulse(imp, rel);
    } else {
        b->body->applyCentralImpulse(imp);
    }
    b->body->activate(true);
    return 0;
}

static int l_Body_ApplyCentralImpulse(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->applyCentralImpulse(ReadVec3(L, 2));
    b->body->activate(true);
    return 0;
}

static int l_Body_ApplyTorque(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->applyTorque(ReadVec3(L, 2));
    b->body->activate(true);
    return 0;
}

static int l_Body_ApplyTorqueImpulse(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->applyTorqueImpulse(ReadVec3(L, 2));
    b->body->activate(true);
    return 0;
}

static int l_Body_GetMass(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) { lua_pushnumber(L, 0); return 1; }
    btScalar inv = b->body->getInvMass();
    lua_pushnumber(L, inv == 0 ? 0 : 1.0 / inv);  // 0 = static
    return 1;
}

static int l_Body_SetMass(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    float m = (float)luaL_checknumber(L, 2);
    btVector3 inertia(0, 0, 0);
    if (m > 0 && b->body->getCollisionShape()) {
        b->body->getCollisionShape()->calculateLocalInertia(m, inertia);
    }
    b->body->setMassProps(m, inertia);
    b->body->updateInertiaTensor();
    return 0;
}

static int l_Body_SetFriction(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setFriction((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Body_GetFriction(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    lua_pushnumber(L, b->alive ? b->body->getFriction() : 0.0);
    return 1;
}
static int l_Body_SetRestitution(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setRestitution((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Body_GetRestitution(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    lua_pushnumber(L, b->alive ? b->body->getRestitution() : 0.0);
    return 1;
}
static int l_Body_SetDamping(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    float lin = (float)luaL_checknumber(L, 2);
    float ang = (float)luaL_checknumber(L, 3);
    b->body->setDamping(lin, ang);
    return 0;
}
static int l_Body_GetLinearDamping(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    lua_pushnumber(L, b->alive ? b->body->getLinearDamping() : 0.0);
    return 1;
}
static int l_Body_GetAngularDamping(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    lua_pushnumber(L, b->alive ? b->body->getAngularDamping() : 0.0);
    return 1;
}

static int l_Body_SetGravity(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setGravity(ReadVec3(L, 2));
    return 0;
}
static int l_Body_GetGravity(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) { lua_pushnumber(L,0); lua_pushnumber(L,0); lua_pushnumber(L,0); return 3; }
    PushVec3(L, b->body->getGravity());
    return 3;
}

static int l_Body_SetCcdMotionThreshold(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setCcdMotionThreshold((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Body_SetCcdSweptSphereRadius(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->setCcdSweptSphereRadius((float)luaL_checknumber(L, 2));
    return 0;
}

static int l_Body_Activate(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    b->body->activate(lua_toboolean(L, 2) != 0);
    return 0;
}
static int l_Body_IsActive(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    lua_pushboolean(L, b->alive ? b->body->isActive() : 0);
    return 1;
}

static int l_Body_SetKinematic(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    if (!b->alive) return 0;
    int flags = b->body->getCollisionFlags();
    if (lua_toboolean(L, 2)) {
        flags |= btCollisionObject::CF_KINEMATIC_OBJECT;
        b->body->setActivationState(DISABLE_DEACTIVATION);
    } else {
        flags &= ~btCollisionObject::CF_KINEMATIC_OBJECT;
    }
    b->body->setCollisionFlags(flags);
    return 0;
}
static int l_Body_IsKinematic(lua_State* L) {
    Body3D* b = CheckBody(L, 1);
    lua_pushboolean(L, b->alive && b->body->isKinematicObject());
    return 1;
}

static int l_Body_IsAlive(lua_State* L) {
    Body3D* b = (Body3D*)luaL_checkudata(L, 1, BODY_MT);
    lua_pushboolean(L, b->alive);
    return 1;
}

// ==================== Body 销毁逻辑 ====================

static void InvalidateBody(lua_State* L, Body3D* b) {
    if (!b || !b->alive) return;
    if (b->body && b->owner && b->owner->world) {
        b->owner->world->removeRigidBody(b->body);
    }
    delete b->body;
    delete b->motion;
    b->body = nullptr;
    b->motion = nullptr;
    if (b->shapeRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, b->shapeRef);
        b->shapeRef = LUA_NOREF;
    }
    if (b->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, b->selfRef);
        b->selfRef = LUA_NOREF;
    }
    b->alive = false;
}

static int l_Body_Delete(lua_State* L) {
    Body3D* b = (Body3D*)luaL_checkudata(L, 1, BODY_MT);
    if (!b->alive) return 0;
    // 从 world 的 bodies 列表移除
    if (b->owner) {
        auto& v = b->owner->bodies;
        v.erase(std::remove(v.begin(), v.end(), b), v.end());
    }
    InvalidateBody(L, b);
    return 0;
}

static int l_Body_Tostring(lua_State* L) {
    Body3D* b = (Body3D*)luaL_checkudata(L, 1, BODY_MT);
    if (!b->alive) {
        lua_pushstring(L, "Light.Physics3D.Body(dead)");
        return 1;
    }
    btTransform t;
    b->motion->getWorldTransform(t);
    btVector3 p = t.getOrigin();
    lua_pushfstring(L, "Light.Physics3D.Body(%p, pos=%.2f,%.2f,%.2f)",
                    b->body, (double)p.x(), (double)p.y(), (double)p.z());
    return 1;
}

// ==================== World 实现 ====================

static int l_NewWorld(lua_State* L) {
    float gx = (float)luaL_optnumber(L, 1, 0.0);
    float gy = (float)luaL_optnumber(L, 2, -9.81);
    float gz = (float)luaL_optnumber(L, 3, 0.0);

    World3D* w = (World3D*)lua_newuserdata(L, sizeof(World3D));
    new (w) World3D();

    w->config = new btDefaultCollisionConfiguration();
    w->dispatcher = new btCollisionDispatcher(w->config);
    w->broadphase = new btDbvtBroadphase();
    w->solver = new btSequentialImpulseConstraintSolver();
    w->world = new btDiscreteDynamicsWorld(w->dispatcher, w->broadphase, w->solver, w->config);
    w->world->setGravity(btVector3(gx, gy, gz));
    w->L = L;
    w->alive = true;

    luaL_getmetatable(L, WORLD_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_World_SetGravity(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) return 0;
    w->world->setGravity(ReadVec3(L, 2));
    return 0;
}

static int l_World_GetGravity(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) { lua_pushnumber(L,0); lua_pushnumber(L,0); lua_pushnumber(L,0); return 3; }
    PushVec3(L, w->world->getGravity());
    return 3;
}

// 在 Step 之后扫 manifold dispatcher 触发 OnContact 回调
static void DispatchContacts(lua_State* L, World3D* w) {
    if (w->contactRef == LUA_NOREF) return;
    int n = w->dispatcher->getNumManifolds();
    for (int i = 0; i < n; i++) {
        btPersistentManifold* m = w->dispatcher->getManifoldByIndexInternal(i);
        if (m->getNumContacts() == 0) continue;
        const btCollisionObject* a = m->getBody0();
        const btCollisionObject* b = m->getBody1();
        Body3D* bodyA = (Body3D*)a->getUserPointer();
        Body3D* bodyB = (Body3D*)b->getUserPointer();
        if (!bodyA || !bodyB || !bodyA->alive || !bodyB->alive) continue;

        lua_rawgeti(L, LUA_REGISTRYINDEX, w->contactRef);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
        // push 2 bodies via selfRef (确保是同一个 Lua object)
        lua_rawgeti(L, LUA_REGISTRYINDEX, bodyA->selfRef);
        lua_rawgeti(L, LUA_REGISTRYINDEX, bodyB->selfRef);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            // 错误吞掉, 继续
            CC::Log(CC::LOG_WARN, "Light.Physics3D OnContact callback error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

static int l_World_Step(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) return 0;
    float dt = (float)luaL_checknumber(L, 2);
    int maxSubSteps = (int)luaL_optinteger(L, 3, 10);
    float fixedTimeStep = (float)luaL_optnumber(L, 4, 1.0 / 60.0);
    w->world->stepSimulation(dt, maxSubSteps, fixedTimeStep);
    DispatchContacts(L, w);
    return 0;
}

static int l_World_CreateBody(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) { lua_pushnil(L); return 1; }
    luaL_checktype(L, 2, LUA_TTABLE);

    // 读 type
    lua_getfield(L, 2, "type");
    const char* typeStr = lua_isstring(L, -1) ? lua_tostring(L, -1) : "dynamic";
    bool isStatic = strcmp(typeStr, "static") == 0;
    bool isKinematic = strcmp(typeStr, "kinematic") == 0;
    lua_pop(L, 1);

    // mass
    lua_getfield(L, 2, "mass");
    float mass = (lua_isnumber(L, -1)) ? (float)lua_tonumber(L, -1) : 1.0f;
    lua_pop(L, 1);
    if (isStatic) mass = 0;

    // pos
    lua_getfield(L, 2, "x"); float x = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0; lua_pop(L, 1);
    lua_getfield(L, 2, "y"); float y = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0; lua_pop(L, 1);
    lua_getfield(L, 2, "z"); float z = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0; lua_pop(L, 1);

    // shape
    lua_getfield(L, 2, "shape");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "shape required");
        return 2;
    }
    Shape3D* shape = (Shape3D*)luaL_testudata(L, -1, SHAPE_MT);
    if (!shape || !shape->shape) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "invalid shape");
        return 2;
    }
    int shapeStackIdx = lua_gettop(L);  // -1
    btCollisionShape* btshape = shape->shape;

    // 计算惯性 (仅 dynamic)
    btVector3 inertia(0, 0, 0);
    if (mass > 0 && !isKinematic) {
        btshape->calculateLocalInertia(mass, inertia);
    }

    // 创建 body
    btTransform tr;
    tr.setIdentity();
    tr.setOrigin(btVector3(x, y, z));
    btDefaultMotionState* motion = new btDefaultMotionState(tr);
    btRigidBody::btRigidBodyConstructionInfo ci(mass, motion, btshape, inertia);
    btRigidBody* rb = new btRigidBody(ci);
    if (isKinematic) {
        rb->setCollisionFlags(rb->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        rb->setActivationState(DISABLE_DEACTIVATION);
    }

    // 可选属性
    lua_getfield(L, 2, "friction");
    if (lua_isnumber(L, -1)) rb->setFriction((float)lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 2, "restitution");
    if (lua_isnumber(L, -1)) rb->setRestitution((float)lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 2, "linDamp");
    float linDamp = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;
    lua_pop(L, 1);
    lua_getfield(L, 2, "angDamp");
    float angDamp = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;
    lua_pop(L, 1);
    rb->setDamping(linDamp, angDamp);

    w->world->addRigidBody(rb);

    // 创建 Body3D userdata
    Body3D* b = (Body3D*)lua_newuserdata(L, sizeof(Body3D));
    new (b) Body3D();
    b->body = rb;
    b->motion = motion;
    b->owner = w;
    b->alive = true;

    // 持有 shape 引用 (防 GC)
    lua_pushvalue(L, shapeStackIdx);
    b->shapeRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // 持有自身引用 (用于 OnContact 回调)
    lua_pushvalue(L, -1);  // 复制 body userdata
    b->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

    luaL_getmetatable(L, BODY_MT);
    lua_setmetatable(L, -2);

    rb->setUserPointer(b);  // OnContact 反查
    w->bodies.push_back(b);
    return 1;
}

static int l_World_DestroyBody(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) return 0;
    Body3D* b = (Body3D*)luaL_checkudata(L, 2, BODY_MT);
    if (b->owner != w || !b->alive) return 0;
    auto& v = w->bodies;
    v.erase(std::remove(v.begin(), v.end(), b), v.end());
    InvalidateBody(L, b);
    return 0;
}

static int l_World_GetBodyCount(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) { lua_pushinteger(L, 0); return 1; }
    int count = 0;
    for (auto* b : w->bodies) if (b && b->alive) count++;
    lua_pushinteger(L, count);
    return 1;
}

static int l_World_RayCast(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) {
        lua_pushnil(L);
        for (int i = 0; i < 7; i++) lua_pushnumber(L, 0);
        return 8;
    }
    btVector3 from = ReadVec3(L, 2);
    btVector3 to   = ReadVec3(L, 5);
    btCollisionWorld::ClosestRayResultCallback cb(from, to);
    w->world->rayTest(from, to, cb);
    if (!cb.hasHit()) {
        lua_pushnil(L);
        for (int i = 0; i < 7; i++) lua_pushnumber(L, 0);
        return 8;
    }
    Body3D* hit = (Body3D*)cb.m_collisionObject->getUserPointer();
    if (hit && hit->alive && hit->selfRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, hit->selfRef);
    } else {
        lua_pushnil(L);
    }
    PushVec3(L, cb.m_hitPointWorld);
    PushVec3(L, cb.m_hitNormalWorld);
    lua_pushnumber(L, cb.m_closestHitFraction);
    return 8;
}

static int l_World_OnContact(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) return 0;
    if (w->contactRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, w->contactRef);
        w->contactRef = LUA_NOREF;
    }
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        w->contactRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// 释放 Bullet 资源, 可重复调用 (幂等)
static void World_ReleaseBullet(lua_State* L, World3D* w) {
    if (!w->alive) return;
    // 销毁所有 body 引用
    for (auto* b : w->bodies) {
        InvalidateBody(L, b);
        if (b) b->owner = nullptr;
    }
    w->bodies.clear();
    if (w->contactRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, w->contactRef);
        w->contactRef = LUA_NOREF;
    }
    delete w->world;       w->world = nullptr;
    delete w->solver;      w->solver = nullptr;
    delete w->broadphase;  w->broadphase = nullptr;
    delete w->dispatcher;  w->dispatcher = nullptr;
    delete w->config;      w->config = nullptr;
    w->alive = false;
}

// World:Delete() — 显式释放 Bullet 资源 (不析构 userdata, 仍由 Lua GC 管理)
static int l_World_Delete(lua_State* L) {
    World3D* w = (World3D*)luaL_checkudata(L, 1, WORLD_MT);
    World_ReleaseBullet(L, w);
    return 0;
}

// __gc — 释放 Bullet 资源 + 调用 placement-delete (vector 等需要显式析构)
static int l_World_GC(lua_State* L) {
    World3D* w = (World3D*)luaL_checkudata(L, 1, WORLD_MT);
    World_ReleaseBullet(L, w);  // alive==false 时内部会早返
    w->~World3D();
    return 0;
}

static int l_World_Tostring(lua_State* L) {
    World3D* w = (World3D*)luaL_checkudata(L, 1, WORLD_MT);
    int n = 0;
    if (w->alive) for (auto* b : w->bodies) if (b && b->alive) n++;
    lua_pushfstring(L, "Light.Physics3D.World(%d bodies)", n);
    return 1;
}

// ==================== Module registration ====================

static const luaL_Reg kPhysics3DFns[] = {
    { "NewBox",         l_NewBox },
    { "NewSphere",      l_NewSphere },
    { "NewCylinder",    l_NewCylinder },
    { "NewCapsule",     l_NewCapsule },
    { "NewCone",        l_NewCone },
    { "NewStaticPlane", l_NewStaticPlane },
    { "NewWorld",       l_NewWorld },
    { nullptr, nullptr }
};

static const luaL_Reg kWorldMethods[] = {
    { "SetGravity",     l_World_SetGravity },
    { "GetGravity",     l_World_GetGravity },
    { "Step",           l_World_Step },
    { "CreateBody",     l_World_CreateBody },
    { "DestroyBody",    l_World_DestroyBody },
    { "GetBodyCount",   l_World_GetBodyCount },
    { "RayCast",        l_World_RayCast },
    { "OnContact",      l_World_OnContact },
    { "Delete",         l_World_Delete },
    { "__gc",           l_World_GC },
    { "__tostring",     l_World_Tostring },
    { nullptr, nullptr }
};

static const luaL_Reg kBodyMethods[] = {
    { "GetPosition",            l_Body_GetPosition },
    { "SetPosition",            l_Body_SetPosition },
    { "GetRotation",            l_Body_GetRotation },
    { "SetRotation",            l_Body_SetRotation },
    { "GetTransform",           l_Body_GetTransform },
    { "GetLinearVelocity",      l_Body_GetLinearVelocity },
    { "SetLinearVelocity",      l_Body_SetLinearVelocity },
    { "GetAngularVelocity",     l_Body_GetAngularVelocity },
    { "SetAngularVelocity",     l_Body_SetAngularVelocity },
    { "ApplyForce",             l_Body_ApplyForce },
    { "ApplyCentralForce",      l_Body_ApplyCentralForce },
    { "ApplyImpulse",           l_Body_ApplyImpulse },
    { "ApplyCentralImpulse",    l_Body_ApplyCentralImpulse },
    { "ApplyTorque",            l_Body_ApplyTorque },
    { "ApplyTorqueImpulse",     l_Body_ApplyTorqueImpulse },
    { "GetMass",                l_Body_GetMass },
    { "SetMass",                l_Body_SetMass },
    { "GetFriction",            l_Body_GetFriction },
    { "SetFriction",            l_Body_SetFriction },
    { "GetRestitution",         l_Body_GetRestitution },
    { "SetRestitution",         l_Body_SetRestitution },
    { "SetDamping",             l_Body_SetDamping },
    { "GetLinearDamping",       l_Body_GetLinearDamping },
    { "GetAngularDamping",      l_Body_GetAngularDamping },
    { "SetGravity",             l_Body_SetGravity },
    { "GetGravity",             l_Body_GetGravity },
    { "SetCcdMotionThreshold",  l_Body_SetCcdMotionThreshold },
    { "SetCcdSweptSphereRadius",l_Body_SetCcdSweptSphereRadius },
    { "Activate",               l_Body_Activate },
    { "IsActive",               l_Body_IsActive },
    { "SetKinematic",           l_Body_SetKinematic },
    { "IsKinematic",            l_Body_IsKinematic },
    { "IsAlive",                l_Body_IsAlive },
    { "Delete",                 l_Body_Delete },
    { "__gc",                   l_Body_Delete },
    { "__tostring",             l_Body_Tostring },
    { nullptr, nullptr }
};

static const luaL_Reg kShapeMethods[] = {
    { "__gc",       l_Shape_GC },
    { "__tostring", l_Shape_Tostring },
    { nullptr, nullptr }
};

extern "C" LIGHT_API int luaopen_Light_Physics3D(lua_State* L) {
    // World 元表
    if (luaL_newmetatable(L, WORLD_MT)) {
        luaL_setfuncs(L, kWorldMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    // Body 元表
    if (luaL_newmetatable(L, BODY_MT)) {
        luaL_setfuncs(L, kBodyMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    // Shape 元表 (无方法, 仅 __gc + __tostring)
    if (luaL_newmetatable(L, SHAPE_MT)) {
        luaL_setfuncs(L, kShapeMethods, 0);
    }
    lua_pop(L, 1);

    // 模块表
    lua_newtable(L);
    luaL_setfuncs(L, kPhysics3DFns, 0);
    return 1;
}

#else  // CHOCO_HAS_BULLET=0 — stub for build with Bullet disabled

static int l_Physics3D_Unavailable(lua_State* L) {
    return luaL_error(L, "Light.Physics3D requires Bullet 3 (CHOCO_HAS_BULLET=0)");
}

extern "C" LIGHT_API int luaopen_Light_Physics3D(lua_State* L) {
    lua_newtable(L);
    static const luaL_Reg kStub[] = {
        { "NewBox",         l_Physics3D_Unavailable },
        { "NewSphere",      l_Physics3D_Unavailable },
        { "NewCylinder",    l_Physics3D_Unavailable },
        { "NewCapsule",     l_Physics3D_Unavailable },
        { "NewCone",        l_Physics3D_Unavailable },
        { "NewStaticPlane", l_Physics3D_Unavailable },
        { "NewWorld",       l_Physics3D_Unavailable },
        { nullptr, nullptr }
    };
    luaL_setfuncs(L, kStub, 0);
    return 1;
}

#endif  // CHOCO_HAS_BULLET
