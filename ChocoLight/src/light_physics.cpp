/**
 * @file light_physics.cpp
 * @brief Light.Physics — Box2D v3 物理引擎绑定
 *
 * Lua API:
 *   Light.Physics.World
 *     world = Light(Light.Physics.World):New()
 *     world:SetGravity(gx, gy)
 *     world:Step(dt)
 *     body = world:CreateBody(type, x, y)    -- type: "static"/"dynamic"/"kinematic"
 *     world:DestroyBody(body)
 *     world:OnCollision(callback)
 *     world:GetBodyCount() → int
 *
 *   body (table with __instance):
 *     body:AddBox(hw, hh)                     -- 半宽/半高
 *     body:AddCircle(radius)
 *     body:GetPosition() → x, y
 *     body:GetAngle() → radians
 *     body:SetPosition(x, y)
 *     body:SetLinearVelocity(vx, vy)
 *     body:GetLinearVelocity() → vx, vy
 *     body:SetRestitution(r)
 *     body:SetFriction(f)
 *     body:ApplyForce(fx, fy)
 *     body:ApplyImpulse(ix, iy)
 *
 * 坐标系: Lua 像素坐标, 内部用 1 pixel = 1/PTM 米
 */

#include "light.h"
#include <box2d/box2d.h>
#include <cstring>
#include <cstdlib>
#include <vector>

static constexpr float PTM = 32.0f;  // 像素/米 转换

// ==================== World 上下文 ====================

struct PhysicsBody {
    b2BodyId bodyId;
    int selfRef;             // Lua 注册表引用
    bool alive;
};

struct PhysicsWorld {
    b2WorldId worldId;
    std::vector<PhysicsBody*> bodies;
    int collisionRef;        // Lua 碰撞回调引用
    lua_State* L;
    int selfRef;
};

static PhysicsWorld* CheckWorld(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* w = (PhysicsWorld*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return w;
}

static PhysicsBody* CheckBody(lua_State* L, int idx) {
    lua_getfield(L, idx, "__body");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* b = (PhysicsBody*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return b;
}

// ==================== World 函数 ====================

/// World.__call — 构造
static int l_World_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    auto* w = (PhysicsWorld*)lua_newuserdata(L, sizeof(PhysicsWorld));
    memset(w, 0, sizeof(PhysicsWorld));

    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = (b2Vec2){0.0f, 10.0f};  // 默认向下重力 (像素 y 轴向下)
    w->worldId = b2CreateWorld(&worldDef);
    w->collisionRef = LUA_NOREF;
    w->L = L;

    lua_setfield(L, 1, "__instance");

    // 保存 self 引用
    lua_pushvalue(L, 1);
    w->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

    return 0;
}

/// World:SetGravity(gx, gy) — 像素/秒²
static int l_World_SetGravity(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w) return 0;
    float gx = (float)luaL_checknumber(L, 2) / PTM;
    float gy = (float)luaL_checknumber(L, 3) / PTM;
    b2World_SetGravity(w->worldId, (b2Vec2){gx, gy});
    return 0;
}

/// World:Step(dt)
static int l_World_Step(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w) return 0;
    float dt = (float)luaL_checknumber(L, 2);
    int subSteps = 4;
    b2World_Step(w->worldId, dt, subSteps);

    // 碰撞事件
    if (w->collisionRef != LUA_NOREF) {
        b2ContactEvents events = b2World_GetContactEvents(w->worldId);
        for (int i = 0; i < events.beginCount; i++) {
            const b2ContactBeginTouchEvent& ev = events.beginEvents[i];
            // 查找对应的 body
            b2BodyId bodyA = b2Shape_GetBody(ev.shapeIdA);
            b2BodyId bodyB = b2Shape_GetBody(ev.shapeIdB);
            // 调用 Lua 回调
            lua_rawgeti(L, LUA_REGISTRYINDEX, w->collisionRef);
            if (lua_isfunction(L, -1)) {
                // 查找 body 的 Lua 引用
                bool foundA = false, foundB = false;
                for (auto* pb : w->bodies) {
                    if (!pb->alive) continue;
                    if (B2_ID_EQUALS(pb->bodyId, bodyA) && pb->selfRef != LUA_NOREF) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, pb->selfRef);
                        foundA = true;
                    }
                    if (B2_ID_EQUALS(pb->bodyId, bodyB) && pb->selfRef != LUA_NOREF) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, pb->selfRef);
                        foundB = true;
                    }
                    if (foundA && foundB) break;
                }
                if (!foundA) lua_pushnil(L);
                if (!foundB) lua_pushnil(L);
                lua_pcall(L, 2, 0, 0);
            } else {
                lua_pop(L, 1);
            }
        }
    }
    return 0;
}

/// World:CreateBody(type, x, y)
static int l_World_CreateBody(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w) return 0;
    const char* typeStr = luaL_checkstring(L, 2);
    float px = (float)luaL_checknumber(L, 3) / PTM;
    float py = (float)luaL_checknumber(L, 4) / PTM;

    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.position = (b2Vec2){px, py};

    if (strcmp(typeStr, "dynamic") == 0) bodyDef.type = b2_dynamicBody;
    else if (strcmp(typeStr, "kinematic") == 0) bodyDef.type = b2_kinematicBody;
    else bodyDef.type = b2_staticBody;

    b2BodyId bodyId = b2CreateBody(w->worldId, &bodyDef);

    // 创建 Lua body 表
    lua_createtable(L, 0, 8);

    auto* pb = (PhysicsBody*)lua_newuserdata(L, sizeof(PhysicsBody));
    pb->bodyId = bodyId;
    pb->alive = true;
    pb->selfRef = LUA_NOREF;
    lua_setfield(L, -2, "__body");

    // 注册 body 方法
    static const luaL_Reg body_funcs[] = {
        {"AddBox", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) return 0;
            float hw = (float)luaL_checknumber(Ls, 2) / PTM * 0.5f;
            float hh = (float)luaL_checknumber(Ls, 3) / PTM * 0.5f;
            b2ShapeDef shapeDef = b2DefaultShapeDef();
            b2Polygon box = b2MakeBox(hw, hh);
            b2CreatePolygonShape(b->bodyId, &shapeDef, &box);
            return 0;
        }},
        {"AddCircle", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) return 0;
            float r = (float)luaL_checknumber(Ls, 2) / PTM;
            b2ShapeDef shapeDef = b2DefaultShapeDef();
            b2Circle circle = {{0, 0}, r};
            b2CreateCircleShape(b->bodyId, &shapeDef, &circle);
            return 0;
        }},
        {"GetPosition", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) { lua_pushnumber(Ls, 0); lua_pushnumber(Ls, 0); return 2; }
            b2Vec2 pos = b2Body_GetPosition(b->bodyId);
            lua_pushnumber(Ls, pos.x * PTM);
            lua_pushnumber(Ls, pos.y * PTM);
            return 2;
        }},
        {"GetAngle", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            lua_pushnumber(Ls, b ? b2Body_GetAngle(b->bodyId) : 0);
            return 1;
        }},
        {"SetPosition", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) return 0;
            float x = (float)luaL_checknumber(Ls, 2) / PTM;
            float y = (float)luaL_checknumber(Ls, 3) / PTM;
            b2Body_SetTransform(b->bodyId, (b2Vec2){x, y}, b2Body_GetRotation(b->bodyId));
            return 0;
        }},
        {"SetLinearVelocity", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) return 0;
            float vx = (float)luaL_checknumber(Ls, 2) / PTM;
            float vy = (float)luaL_checknumber(Ls, 3) / PTM;
            b2Body_SetLinearVelocity(b->bodyId, (b2Vec2){vx, vy});
            return 0;
        }},
        {"GetLinearVelocity", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) { lua_pushnumber(Ls, 0); lua_pushnumber(Ls, 0); return 2; }
            b2Vec2 v = b2Body_GetLinearVelocity(b->bodyId);
            lua_pushnumber(Ls, v.x * PTM);
            lua_pushnumber(Ls, v.y * PTM);
            return 2;
        }},
        {"ApplyForce", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) return 0;
            float fx = (float)luaL_checknumber(Ls, 2) / PTM;
            float fy = (float)luaL_checknumber(Ls, 3) / PTM;
            b2Body_ApplyForceToCenter(b->bodyId, (b2Vec2){fx, fy}, true);
            return 0;
        }},
        {"ApplyImpulse", [](lua_State* Ls) -> int {
            auto* b = CheckBody(Ls, 1);
            if (!b) return 0;
            float ix = (float)luaL_checknumber(Ls, 2) / PTM;
            float iy = (float)luaL_checknumber(Ls, 3) / PTM;
            b2Body_ApplyLinearImpulseToCenter(b->bodyId, (b2Vec2){ix, iy}, true);
            return 0;
        }},
        {NULL, NULL}
    };
    luaL_setfuncs(L, body_funcs, 0);

    // 保存 body Lua 表引用
    lua_pushvalue(L, -1);
    pb->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

    w->bodies.push_back(pb);
    return 1;
}

/// World:DestroyBody(bodyTable)
static int l_World_DestroyBody(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* b = CheckBody(L, 2);
    if (!w || !b || !b->alive) return 0;
    b2DestroyBody(b->bodyId);
    b->alive = false;
    if (b->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, b->selfRef);
        b->selfRef = LUA_NOREF;
    }
    return 0;
}

/// World:OnCollision(callback)
static int l_World_OnCollision(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w) return 0;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (w->collisionRef != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, w->collisionRef);
    lua_pushvalue(L, 2);
    w->collisionRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

/// World:GetBodyCount()
static int l_World_GetBodyCount(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    int count = 0;
    if (w) {
        for (auto* b : w->bodies)
            if (b->alive) count++;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_World_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Physics.World");
    return 1;
}

// ==================== luaopen 注册 ====================

int luaopen_Light_Physics(lua_State* L) {
    LT::RegisterModule(L, "Physics", nullptr);
    return 1;
}

static const luaL_Reg world_funcs[] = {
    {"SetGravity",   l_World_SetGravity},
    {"Step",         l_World_Step},
    {"CreateBody",   l_World_CreateBody},
    {"DestroyBody",  l_World_DestroyBody},
    {"OnCollision",  l_World_OnCollision},
    {"GetBodyCount", l_World_GetBodyCount},
    {"__call",       l_World_Call},
    {"__tostring",   l_World_Tostring},
    {NULL, NULL}
};

int luaopen_Light_Physics_World(lua_State* L) {
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Physics");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, "World");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "World");
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, world_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "World");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}
