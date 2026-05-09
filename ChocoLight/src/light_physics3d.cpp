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
// Heightfield 头文件未被 btBulletCollisionCommon.h 自动包含, 显式引入
#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>
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
static const char* JOINT_MT = "Light.Physics3D.Joint";   // Phase AU Step 3.2

// ==================== userdata 结构 ====================

struct World3D;
struct Body3D;

struct Shape3D {
    btCollisionShape* shape;
    // Heightfield 必须持有 height array (Bullet 内部只存指针不拷贝)
    float* heightData;
    // TriangleMesh 必须持有 vertex/index buffer + btTriangleIndexVertexArray
    btTriangleIndexVertexArray* triMesh;
    float* triVertices;
    int*   triIndices;
    Shape3D() : shape(nullptr), heightData(nullptr),
                triMesh(nullptr), triVertices(nullptr), triIndices(nullptr) {}
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

// Phase AU Step 3.2: Joint 前向声明
struct Joint3D;

struct World3D {
    btDefaultCollisionConfiguration*     config;
    btCollisionDispatcher*               dispatcher;
    btBroadphaseInterface*               broadphase;
    btSequentialImpulseConstraintSolver* solver;
    btDiscreteDynamicsWorld*             world;
    std::vector<Body3D*>                 bodies;
    std::vector<Joint3D*>                joints;      // Phase AU Step 3.2
    int                                  contactRef;  // OnContact callback in registry
    lua_State*                           L;
    bool                                 alive;
    World3D() : config(nullptr), dispatcher(nullptr), broadphase(nullptr),
                solver(nullptr), world(nullptr), contactRef(LUA_NOREF),
                L(nullptr), alive(false) {}
};

// Phase AU Step 3.2: Joint userdata
enum JointType {
    JT_P2P       = 0,
    JT_HINGE     = 1,
    JT_SLIDER    = 2,
    JT_CONETWIST = 3,
    JT_6DOF      = 4,
};
struct Joint3D {
    btTypedConstraint* constraint;
    World3D*           owner;
    int                bodyARef;   // 防 bodyA userdata GC (registry ref)
    int                bodyBRef;
    int                selfRef;    // 防 joint 自身 GC (用 vector 持有)
    JointType          type;
    bool               alive;
    Joint3D() : constraint(nullptr), owner(nullptr),
                bodyARef(LUA_NOREF), bodyBRef(LUA_NOREF), selfRef(LUA_NOREF),
                type(JT_P2P), alive(false) {}
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

// Phase AU Step 3.1: NewConvexHull(verts:array)
// verts: flat number array {x1,y1,z1, x2,y2,z2, ...}; Bullet 内部 copy 顶点, 无需 extraData
static int l_NewConvexHull(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)lua_objlen(L, 1);
    if (n < 9 || (n % 3) != 0) {
        return luaL_error(L, "NewConvexHull: vertex array length must be >=9 and multiple of 3 (got %d)", n);
    }
    btConvexHullShape* hull = new btConvexHullShape();
    int triCount = n / 3;
    for (int i = 0; i < triCount; i++) {
        lua_rawgeti(L, 1, i*3 + 1); float vx = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, 1, i*3 + 2); float vy = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, 1, i*3 + 3); float vz = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        hull->addPoint(btVector3(vx, vy, vz), false);
    }
    hull->recalcLocalAabb();
    return PushShapeUserdata(L, hull);
}

// Phase AU Step 3.1: NewHeightfield(width, length, heights:array [, scaleY=1, minH=auto, maxH=auto])
// heights: row-major flat array, length = width*length
// Bullet 仅引用指针, 必须由 Shape3D 持有 heightData 直到 shape 销毁
static int l_NewHeightfield(lua_State* L) {
    int width  = (int)luaL_checkinteger(L, 1);
    int length = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    float scaleY = (float)luaL_optnumber(L, 4, 1.0);
    int totalCells = width * length;
    int n = (int)lua_objlen(L, 3);
    if (width < 2 || length < 2 || totalCells != n) {
        return luaL_error(L, "NewHeightfield: heights count %d != width*length %d (or width/length < 2)", n, totalCells);
    }
    float* data = (float*)malloc(sizeof(float) * totalCells);
    if (!data) return luaL_error(L, "NewHeightfield: out of memory");
    float minH = 1e30f, maxH = -1e30f;
    for (int i = 0; i < totalCells; i++) {
        lua_rawgeti(L, 3, i + 1);
        float h = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        data[i] = h;
        if (h < minH) minH = h;
        if (h > maxH) maxH = h;
    }
    if (lua_isnumber(L, 5)) minH = (float)lua_tonumber(L, 5);
    if (lua_isnumber(L, 6)) maxH = (float)lua_tonumber(L, 6);
    if (maxH <= minH) maxH = minH + 1.0f;

    // upAxis = 1 (Y up), heightScale=1 (data 已是 float), flipQuadEdges = false
    btHeightfieldTerrainShape* hf = new btHeightfieldTerrainShape(
        width, length, data, scaleY, minH, maxH, 1 /*Y up*/, PHY_FLOAT, false);
    hf->setLocalScaling(btVector3(1, 1, 1));

    // Push shape userdata 并写 extraData
    Shape3D* ud = (Shape3D*)lua_newuserdata(L, sizeof(Shape3D));
    new (ud) Shape3D();
    ud->shape = hf;
    ud->heightData = data;  // Shape GC 时 free
    luaL_getmetatable(L, SHAPE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// Phase AU Step 3.1: NewTriangleMesh(vertices:array, indices:array)
// vertices: {x,y,z, x,y,z, ...} flat
// indices : {i1,i2,i3, ...} 3 per triangle (0-based)
// 适合静态环境 (地形/建筑); 不能给 dynamic body 用
static int l_NewTriangleMesh(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    int vn = (int)lua_objlen(L, 1);
    int in_ = (int)lua_objlen(L, 2);
    if (vn < 9 || (vn % 3) != 0) {
        return luaL_error(L, "NewTriangleMesh: vertices length must be >=9 and multiple of 3 (got %d)", vn);
    }
    if (in_ < 3 || (in_ % 3) != 0) {
        return luaL_error(L, "NewTriangleMesh: indices length must be multiple of 3 (got %d)", in_);
    }
    int vCount = vn / 3;
    int triCount = in_ / 3;

    float* verts = (float*)malloc(sizeof(float) * vn);
    int*   inds  = (int*)  malloc(sizeof(int)   * in_);
    if (!verts || !inds) {
        free(verts); free(inds);
        return luaL_error(L, "NewTriangleMesh: out of memory");
    }
    for (int i = 0; i < vn; i++) {
        lua_rawgeti(L, 1, i + 1); verts[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    }
    for (int i = 0; i < in_; i++) {
        lua_rawgeti(L, 2, i + 1);
        int idx = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (idx < 0 || idx >= vCount) {
            free(verts); free(inds);
            return luaL_error(L, "NewTriangleMesh: index %d out of range [0,%d)", idx, vCount);
        }
        inds[i] = idx;
    }

    btTriangleIndexVertexArray* meshIface = new btTriangleIndexVertexArray(
        triCount, inds, sizeof(int) * 3,
        vCount, verts, sizeof(float) * 3);
    btBvhTriangleMeshShape* tm = new btBvhTriangleMeshShape(meshIface, true /*BVH 加速*/);

    Shape3D* ud = (Shape3D*)lua_newuserdata(L, sizeof(Shape3D));
    new (ud) Shape3D();
    ud->shape       = tm;
    ud->triMesh     = meshIface;
    ud->triVertices = verts;
    ud->triIndices  = inds;
    luaL_getmetatable(L, SHAPE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_Shape_GC(lua_State* L) {
    Shape3D* s = CheckShape(L, 1);
    if (s->shape) {
        delete s->shape;
        s->shape = nullptr;
    }
    if (s->triMesh) {
        delete s->triMesh;
        s->triMesh = nullptr;
    }
    if (s->heightData) {
        free(s->heightData);
        s->heightData = nullptr;
    }
    if (s->triVertices) {
        free(s->triVertices);
        s->triVertices = nullptr;
    }
    if (s->triIndices) {
        free(s->triIndices);
        s->triIndices = nullptr;
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

// ==================== Joint 销毁逻辑 (Phase AU Step 3.2) ====================

static void InvalidateJoint(lua_State* L, Joint3D* j) {
    if (!j || !j->alive) return;
    if (j->constraint && j->owner && j->owner->world) {
        j->owner->world->removeConstraint(j->constraint);
    }
    delete j->constraint;
    j->constraint = nullptr;
    if (j->bodyARef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, j->bodyARef);
        j->bodyARef = LUA_NOREF;
    }
    if (j->bodyBRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, j->bodyBRef);
        j->bodyBRef = LUA_NOREF;
    }
    if (j->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, j->selfRef);
        j->selfRef = LUA_NOREF;
    }
    j->alive = false;
}

static Joint3D* CheckJoint(lua_State* L, int idx) {
    return (Joint3D*)luaL_checkudata(L, idx, JOINT_MT);
}

static Joint3D* CheckLiveJoint(lua_State* L, int idx) {
    Joint3D* j = CheckJoint(L, idx);
    if (!j->alive) return nullptr;
    return j;
}

// 解析 Lua 表中的 vec3 字段, 默认 (0,0,0)
static btVector3 ReadVec3Field(lua_State* L, int idx, const char* key, btVector3 dflt = btVector3(0,0,0)) {
    lua_getfield(L, idx, key);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1); float x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); float y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); float z = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_pop(L, 1);
        return btVector3(x, y, z);
    }
    lua_pop(L, 1);
    return dflt;
}

// 解析 frame: {x,y,z, qw,qx,qy,qz} -> btTransform; 缺省单位变换
static btTransform ReadTransformField(lua_State* L, int idx, const char* key) {
    btTransform tr; tr.setIdentity();
    lua_getfield(L, idx, key);
    if (lua_istable(L, -1)) {
        int n = (int)lua_objlen(L, -1);
        if (n >= 3) {
            lua_rawgeti(L, -1, 1); float x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 2); float y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 3); float z = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            tr.setOrigin(btVector3(x, y, z));
        }
        if (n >= 7) {
            lua_rawgeti(L, -1, 4); float qw = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 5); float qx = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 6); float qy = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 7); float qz = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            tr.setRotation(btQuaternion(qx, qy, qz, qw));
        }
    }
    lua_pop(L, 1);
    return tr;
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

// ==================== Phase AU Step 3.2: World:CreateJoint ====================

static int l_World_CreateJoint(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) { lua_pushnil(L); lua_pushstring(L, "world dead"); return 2; }
    luaL_checktype(L, 2, LUA_TTABLE);

    // type
    lua_getfield(L, 2, "type");
    const char* typeStr = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!typeStr) {
        lua_pushnil(L);
        lua_pushstring(L, "CreateJoint: missing 'type'");
        return 2;
    }
    JointType jt;
    if      (!strcmp(typeStr, "p2p"))       jt = JT_P2P;
    else if (!strcmp(typeStr, "hinge"))     jt = JT_HINGE;
    else if (!strcmp(typeStr, "slider"))    jt = JT_SLIDER;
    else if (!strcmp(typeStr, "conetwist")) jt = JT_CONETWIST;
    else if (!strcmp(typeStr, "6dof"))      jt = JT_6DOF;
    else {
        lua_pushnil(L);
        lua_pushfstring(L, "CreateJoint: unknown type '%s'", typeStr);
        return 2;
    }

    // bodyA / bodyB (Both required for all 5 constraint types we expose)
    lua_getfield(L, 2, "bodyA");
    Body3D* bodyA = (Body3D*)luaL_testudata(L, -1, BODY_MT);
    int bodyAStackIdx = lua_gettop(L);
    lua_getfield(L, 2, "bodyB");
    Body3D* bodyB = (Body3D*)luaL_testudata(L, -1, BODY_MT);
    int bodyBStackIdx = lua_gettop(L);
    if (!bodyA || !bodyA->alive || !bodyA->body) {
        lua_pop(L, 2);
        lua_pushnil(L);
        lua_pushstring(L, "CreateJoint: bodyA missing/dead");
        return 2;
    }
    if (!bodyB || !bodyB->alive || !bodyB->body) {
        lua_pop(L, 2);
        lua_pushnil(L);
        lua_pushstring(L, "CreateJoint: bodyB missing/dead");
        return 2;
    }
    if (bodyA->owner != w || bodyB->owner != w) {
        lua_pop(L, 2);
        lua_pushnil(L);
        lua_pushstring(L, "CreateJoint: body not in this world");
        return 2;
    }

    btTypedConstraint* constraint = nullptr;
    switch (jt) {
        case JT_P2P: {
            btVector3 pivotA = ReadVec3Field(L, 2, "pivotA");
            btVector3 pivotB = ReadVec3Field(L, 2, "pivotB");
            constraint = new btPoint2PointConstraint(*bodyA->body, *bodyB->body, pivotA, pivotB);
            break;
        }
        case JT_HINGE: {
            btVector3 pivotA = ReadVec3Field(L, 2, "pivotA");
            btVector3 pivotB = ReadVec3Field(L, 2, "pivotB");
            btVector3 axisA  = ReadVec3Field(L, 2, "axisA", btVector3(0, 1, 0));
            btVector3 axisB  = ReadVec3Field(L, 2, "axisB", btVector3(0, 1, 0));
            constraint = new btHingeConstraint(*bodyA->body, *bodyB->body, pivotA, pivotB, axisA, axisB);
            break;
        }
        case JT_SLIDER: {
            btTransform fA = ReadTransformField(L, 2, "frameA");
            btTransform fB = ReadTransformField(L, 2, "frameB");
            constraint = new btSliderConstraint(*bodyA->body, *bodyB->body, fA, fB, true);
            break;
        }
        case JT_CONETWIST: {
            btTransform fA = ReadTransformField(L, 2, "frameA");
            btTransform fB = ReadTransformField(L, 2, "frameB");
            constraint = new btConeTwistConstraint(*bodyA->body, *bodyB->body, fA, fB);
            break;
        }
        case JT_6DOF: {
            btTransform fA = ReadTransformField(L, 2, "frameA");
            btTransform fB = ReadTransformField(L, 2, "frameB");
            constraint = new btGeneric6DofConstraint(*bodyA->body, *bodyB->body, fA, fB, true);
            break;
        }
    }
    if (!constraint) {
        lua_pop(L, 2);
        lua_pushnil(L);
        lua_pushstring(L, "CreateJoint: failed to construct");
        return 2;
    }

    // disableCollisions option
    lua_getfield(L, 2, "disableCollisions");
    bool disableCol = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    w->world->addConstraint(constraint, disableCol);

    // 创建 Joint userdata
    Joint3D* j = (Joint3D*)lua_newuserdata(L, sizeof(Joint3D));
    new (j) Joint3D();
    j->constraint = constraint;
    j->owner      = w;
    j->type       = jt;
    j->alive      = true;
    luaL_getmetatable(L, JOINT_MT);
    lua_setmetatable(L, -2);

    // 防 bodyA/bodyB GC: registry 引用
    lua_pushvalue(L, bodyAStackIdx);
    j->bodyARef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, bodyBStackIdx);
    j->bodyBRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // 防 joint userdata 自身 GC: 暂存到 registry, World 持 vector 也对应
    lua_pushvalue(L, -1);  // duplicate joint udata on top of stack
    j->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

    w->joints.push_back(j);

    // 清理 bodyA/bodyB 临时栈值,只留 joint udata
    lua_remove(L, bodyAStackIdx);
    // 注意 bodyB 因为 bodyA 已被 remove, 它的索引已经 -1 了; 直接用原 idx-1
    lua_remove(L, bodyAStackIdx);
    return 1;  // 只返回 joint
}

static int l_World_DestroyJoint(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) return 0;
    Joint3D* j = CheckJoint(L, 2);
    if (j->owner != w || !j->alive) return 0;
    auto& v = w->joints;
    v.erase(std::remove(v.begin(), v.end(), j), v.end());
    InvalidateJoint(L, j);
    return 0;
}

static int l_World_GetJointCount(lua_State* L) {
    World3D* w = CheckWorld(L, 1);
    if (!w->alive) { lua_pushinteger(L, 0); return 1; }
    int count = 0;
    for (auto* j : w->joints) if (j && j->alive) count++;
    lua_pushinteger(L, count);
    return 1;
}

// ==================== Phase AU Step 3.2: Joint 方法 ====================

static int l_Joint_GetType(lua_State* L) {
    Joint3D* j = CheckJoint(L, 1);
    const char* s = "unknown";
    switch (j->type) {
        case JT_P2P:       s = "p2p"; break;
        case JT_HINGE:     s = "hinge"; break;
        case JT_SLIDER:    s = "slider"; break;
        case JT_CONETWIST: s = "conetwist"; break;
        case JT_6DOF:      s = "6dof"; break;
    }
    lua_pushstring(L, s);
    return 1;
}

static int l_Joint_IsAlive(lua_State* L) {
    Joint3D* j = CheckJoint(L, 1);
    lua_pushboolean(L, j->alive);
    return 1;
}

static int l_Joint_SetEnabled(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j) return 0;
    bool e = lua_toboolean(L, 2) != 0;
    j->constraint->setEnabled(e);
    return 0;
}

static int l_Joint_IsEnabled(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1);
    lua_pushboolean(L, j ? j->constraint->isEnabled() : 0);
    return 1;
}

static int l_Joint_Delete(lua_State* L) {
    Joint3D* j = CheckJoint(L, 1);
    if (!j->alive) return 0;
    if (j->owner) {
        auto& v = j->owner->joints;
        v.erase(std::remove(v.begin(), v.end(), j), v.end());
    }
    InvalidateJoint(L, j);
    return 0;
}

static int l_Joint_GC(lua_State* L) {
    Joint3D* j = CheckJoint(L, 1);
    if (j->alive) {
        if (j->owner) {
            auto& v = j->owner->joints;
            v.erase(std::remove(v.begin(), v.end(), j), v.end());
        }
        InvalidateJoint(L, j);
    }
    j->~Joint3D();
    return 0;
}

static int l_Joint_Tostring(lua_State* L) {
    Joint3D* j = CheckJoint(L, 1);
    const char* s = "?";
    switch (j->type) {
        case JT_P2P: s = "p2p"; break;
        case JT_HINGE: s = "hinge"; break;
        case JT_SLIDER: s = "slider"; break;
        case JT_CONETWIST: s = "conetwist"; break;
        case JT_6DOF: s = "6dof"; break;
    }
    lua_pushfstring(L, "Light.Physics3D.Joint(%s, %s)", s, j->alive ? "alive" : "dead");
    return 1;
}

// ---- Hinge 方法 ----

static int l_Joint_Hinge_SetLimit(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_HINGE) return 0;
    btHingeConstraint* h = static_cast<btHingeConstraint*>(j->constraint);
    float low  = (float)luaL_checknumber(L, 2);
    float high = (float)luaL_checknumber(L, 3);
    float softness    = (float)luaL_optnumber(L, 4, 0.9);
    float biasFactor  = (float)luaL_optnumber(L, 5, 0.3);
    float relaxFactor = (float)luaL_optnumber(L, 6, 1.0);
    h->setLimit(low, high, softness, biasFactor, relaxFactor);
    return 0;
}

static int l_Joint_Hinge_GetAngle(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1);
    if (!j || j->type != JT_HINGE) { lua_pushnumber(L, 0); return 1; }
    btHingeConstraint* h = static_cast<btHingeConstraint*>(j->constraint);
    lua_pushnumber(L, h->getHingeAngle());
    return 1;
}

static int l_Joint_Hinge_EnableMotor(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_HINGE) return 0;
    btHingeConstraint* h = static_cast<btHingeConstraint*>(j->constraint);
    bool enable = lua_toboolean(L, 2) != 0;
    float targetVel  = (float)luaL_optnumber(L, 3, 0.0);
    float maxImpulse = (float)luaL_optnumber(L, 4, 1.0);
    h->enableAngularMotor(enable, targetVel, maxImpulse);
    return 0;
}

// ---- Slider 方法 ----

static int l_Joint_Slider_SetLowerLin(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_SLIDER) return 0;
    static_cast<btSliderConstraint*>(j->constraint)->setLowerLinLimit((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Joint_Slider_SetUpperLin(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_SLIDER) return 0;
    static_cast<btSliderConstraint*>(j->constraint)->setUpperLinLimit((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Joint_Slider_SetLowerAng(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_SLIDER) return 0;
    static_cast<btSliderConstraint*>(j->constraint)->setLowerAngLimit((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Joint_Slider_SetUpperAng(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_SLIDER) return 0;
    static_cast<btSliderConstraint*>(j->constraint)->setUpperAngLimit((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Joint_Slider_GetLinearPos(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1);
    if (!j || j->type != JT_SLIDER) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, static_cast<btSliderConstraint*>(j->constraint)->getLinearPos());
    return 1;
}

// ---- ConeTwist 方法 ----

static int l_Joint_ConeTwist_SetLimit(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_CONETWIST) return 0;
    btConeTwistConstraint* c = static_cast<btConeTwistConstraint*>(j->constraint);
    float swing1 = (float)luaL_checknumber(L, 2);
    float swing2 = (float)luaL_checknumber(L, 3);
    float twist  = (float)luaL_checknumber(L, 4);
    c->setLimit(swing1, swing2, twist);
    return 0;
}

// ---- Generic6DOF 方法 ----

static int l_Joint_6DOF_SetLinearLower(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_6DOF) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    static_cast<btGeneric6DofConstraint*>(j->constraint)->setLinearLowerLimit(btVector3(x, y, z));
    return 0;
}
static int l_Joint_6DOF_SetLinearUpper(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_6DOF) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    static_cast<btGeneric6DofConstraint*>(j->constraint)->setLinearUpperLimit(btVector3(x, y, z));
    return 0;
}
static int l_Joint_6DOF_SetAngularLower(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_6DOF) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    static_cast<btGeneric6DofConstraint*>(j->constraint)->setAngularLowerLimit(btVector3(x, y, z));
    return 0;
}
static int l_Joint_6DOF_SetAngularUpper(lua_State* L) {
    Joint3D* j = CheckLiveJoint(L, 1); if (!j || j->type != JT_6DOF) return 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    static_cast<btGeneric6DofConstraint*>(j->constraint)->setAngularUpperLimit(btVector3(x, y, z));
    return 0;
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
    // Phase AU Step 3.2: joints 必须先于 bodies 销毁 (Bullet constraint 引用 body)
    for (auto* j : w->joints) {
        InvalidateJoint(L, j);
        if (j) j->owner = nullptr;
    }
    w->joints.clear();
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
    { "NewBox",          l_NewBox },
    { "NewSphere",       l_NewSphere },
    { "NewCylinder",     l_NewCylinder },
    { "NewCapsule",      l_NewCapsule },
    { "NewCone",         l_NewCone },
    { "NewStaticPlane",  l_NewStaticPlane },
    // Phase AU Step 3.1
    { "NewConvexHull",   l_NewConvexHull },
    { "NewHeightfield",  l_NewHeightfield },
    { "NewTriangleMesh", l_NewTriangleMesh },
    { "NewWorld",        l_NewWorld },
    { nullptr, nullptr }
};

static const luaL_Reg kWorldMethods[] = {
    { "SetGravity",     l_World_SetGravity },
    { "GetGravity",     l_World_GetGravity },
    { "Step",           l_World_Step },
    { "CreateBody",     l_World_CreateBody },
    { "DestroyBody",    l_World_DestroyBody },
    { "GetBodyCount",   l_World_GetBodyCount },
    // Phase AU Step 3.2 — Joint
    { "CreateJoint",    l_World_CreateJoint },
    { "DestroyJoint",   l_World_DestroyJoint },
    { "GetJointCount",  l_World_GetJointCount },
    { "RayCast",        l_World_RayCast },
    { "OnContact",      l_World_OnContact },
    { "Delete",         l_World_Delete },
    { "__gc",           l_World_GC },
    { "__tostring",     l_World_Tostring },
    { nullptr, nullptr }
};

// Phase AU Step 3.2: Joint 元表方法
static const luaL_Reg kJointMethods[] = {
    { "GetType",            l_Joint_GetType },
    { "IsAlive",            l_Joint_IsAlive },
    { "SetEnabled",         l_Joint_SetEnabled },
    { "IsEnabled",          l_Joint_IsEnabled },
    // Hinge
    { "SetLimit",           l_Joint_Hinge_SetLimit },          // hinge: low/high; conetwist: 3 params (override based on type)
    { "GetHingeAngle",      l_Joint_Hinge_GetAngle },
    { "EnableMotor",        l_Joint_Hinge_EnableMotor },
    // Slider
    { "SetLowerLinLimit",   l_Joint_Slider_SetLowerLin },
    { "SetUpperLinLimit",   l_Joint_Slider_SetUpperLin },
    { "SetLowerAngLimit",   l_Joint_Slider_SetLowerAng },
    { "SetUpperAngLimit",   l_Joint_Slider_SetUpperAng },
    { "GetLinearPos",       l_Joint_Slider_GetLinearPos },
    // ConeTwist (注意: 与 hinge 的 SetLimit 同名, 但 hinge 接收 2-5 参, conetwist 接收 3 参; 为避免冲突, conetwist 用 SetConeTwistLimit)
    { "SetConeTwistLimit",  l_Joint_ConeTwist_SetLimit },
    // Generic 6DOF
    { "SetLinearLowerLimit",  l_Joint_6DOF_SetLinearLower },
    { "SetLinearUpperLimit",  l_Joint_6DOF_SetLinearUpper },
    { "SetAngularLowerLimit", l_Joint_6DOF_SetAngularLower },
    { "SetAngularUpperLimit", l_Joint_6DOF_SetAngularUpper },
    // 生命周期
    { "Delete",             l_Joint_Delete },
    { "__gc",               l_Joint_GC },
    { "__tostring",         l_Joint_Tostring },
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

    // Joint 元表 (Phase AU Step 3.2)
    if (luaL_newmetatable(L, JOINT_MT)) {
        luaL_setfuncs(L, kJointMethods, 0);
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
        { "NewBox",          l_Physics3D_Unavailable },
        { "NewSphere",       l_Physics3D_Unavailable },
        { "NewCylinder",     l_Physics3D_Unavailable },
        { "NewCapsule",      l_Physics3D_Unavailable },
        { "NewCone",         l_Physics3D_Unavailable },
        { "NewStaticPlane",  l_Physics3D_Unavailable },
        { "NewConvexHull",   l_Physics3D_Unavailable },
        { "NewHeightfield",  l_Physics3D_Unavailable },
        { "NewTriangleMesh", l_Physics3D_Unavailable },
        { "NewWorld",        l_Physics3D_Unavailable },
        { nullptr, nullptr }
    };
    luaL_setfuncs(L, kStub, 0);
    return 1;
}

#endif  // CHOCO_HAS_BULLET
