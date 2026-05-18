/**
 * @file light_particles.cpp
 * @brief Light.Graphics.Particles — 2D 粒子系统
 *
 * Lua API:
 *   Particles:New()                → 创建发射器
 *   emitter:SetPosition(x, y)     → 设置发射位置
 *   emitter:SetEmitRate(n)        → 每秒发射数
 *   emitter:SetLifeRange(min,max) → 粒子生命周期范围
 *   emitter:SetSpeedRange(min,max)→ 初速范围
 *   emitter:SetAngleRange(min,max)→ 发射角度范围 (弧度)
 *   emitter:SetColorRange(c1, c2) → 颜色渐变 (RGBA 表)
 *   emitter:SetSizeRange(min,max) → 粒子尺寸范围
 *   emitter:SetGravity(gx, gy)   → 重力加速度
 *   emitter:Start() / Stop()
 *   emitter:Update(dt)
 *   emitter:Draw()
 */

#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7 — 类型安全 helpers + magic
#include "render_backend.h"
#include "batch_renderer.h"
#include <cmath>
#include <cstring>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern RenderBackend* g_render;

// ==================== 粒子数据 ====================

struct Particle {
    float x, y;
    float vx, vy;
    float r, g, b, a;
    float size;
    float life, maxLife;
    float rotation, rotSpeed;
    bool active;
};

/// Phase G.1.7: 首字段 magic 防止 type-confusion
struct ParticleEmitter {
    uint32_t magic;     // 必须 = LT_MAGIC_EMITTER
    Particle* pool;
    int capacity;
    int activeCount;
    // 发射参数
    float posX, posY;
    float emitRate;
    float emitAccum;       // 发射累积
    float minLife, maxLife;
    float minSpeed, maxSpeed;
    float minAngle, maxAngle;
    float startR, startG, startB, startA;
    float endR, endG, endB, endA;
    float minSize, maxSize;
    float gravX, gravY;
    bool  running;
};

// 随机 [0, 1]
static float randf() { return (float)rand() / (float)RAND_MAX; }
// 随机 [a, b]
static float randf(float a, float b) { return a + randf() * (b - a); }

/// Phase G.1.7: magic 校验防 type-confusion
static ParticleEmitter* CheckEmitter(lua_State* L, int idx) {
    return LT::TryCheckInstance<ParticleEmitter>(L, idx, LT::LT_MAGIC_EMITTER);
}

// ==================== 粒子核心逻辑 ====================

static void EmitterUpdate(ParticleEmitter* em, float dt) {
    if (!em) return;

    // 发射新粒子
    if (em->running) {
        em->emitAccum += em->emitRate * dt;
        while (em->emitAccum >= 1.0f) {
            em->emitAccum -= 1.0f;
            // 找空槽
            for (int i = 0; i < em->capacity; i++) {
                if (!em->pool[i].active) {
                    Particle& p = em->pool[i];
                    p.active = true;
                    p.x = em->posX;
                    p.y = em->posY;
                    float angle = randf(em->minAngle, em->maxAngle);
                    float speed = randf(em->minSpeed, em->maxSpeed);
                    p.vx = cosf(angle) * speed;
                    p.vy = sinf(angle) * speed;
                    p.maxLife = randf(em->minLife, em->maxLife);
                    p.life = p.maxLife;
                    p.size = randf(em->minSize, em->maxSize);
                    p.r = em->startR; p.g = em->startG;
                    p.b = em->startB; p.a = em->startA;
                    p.rotation = 0;
                    p.rotSpeed = randf(-3.0f, 3.0f);
                    em->activeCount++;
                    break;
                }
            }
        }
    }

    // 更新粒子
    for (int i = 0; i < em->capacity; i++) {
        Particle& p = em->pool[i];
        if (!p.active) continue;

        p.life -= dt;
        if (p.life <= 0) {
            p.active = false;
            em->activeCount--;
            continue;
        }

        // 物理
        p.vx += em->gravX * dt;
        p.vy += em->gravY * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.rotation += p.rotSpeed * dt;

        // 颜色插值
        float t = 1.0f - (p.life / p.maxLife);
        p.r = em->startR + (em->endR - em->startR) * t;
        p.g = em->startG + (em->endG - em->startG) * t;
        p.b = em->startB + (em->endB - em->startB) * t;
        p.a = em->startA + (em->endA - em->startA) * t;
    }
}

static void EmitterDraw(ParticleEmitter* em) {
    if (!em || !g_render) return;
    // Phase A7: 走 BatchRenderer 批渲染, 1024 粒子 1 draw call
    const bool useBatch = BatchRenderer::IsInited();
    for (int i = 0; i < em->capacity; i++) {
        const Particle& p = em->pool[i];
        if (!p.active) continue;
        float hs = p.size * 0.5f;
        // 构建 4 个 RenderVertex (矩形)
        RenderVertex verts[4];
        float offsets[4][2] = { {-hs,-hs}, {hs,-hs}, {hs,hs}, {-hs,hs} };
        for (int v = 0; v < 4; v++) {
            verts[v].x = p.x + offsets[v][0];
            verts[v].y = p.y + offsets[v][1];
            verts[v].z = 0;
            verts[v].u = 0; verts[v].v = 0;
            verts[v].r = p.r; verts[v].g = p.g;
            verts[v].b = p.b; verts[v].a = p.a;
        }
        if (useBatch) {
            // 纯色粒子, textureId=0
            BatchRenderer::SubmitQuad(verts, 0);
        } else {
            g_render->DrawArrays(DrawMode::Quads, verts, 4);
        }
    }
}

// ==================== Lua 绑定 ====================

static int l_Particles_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int cap = (int)luaL_optinteger(L, 2, 1024);

    auto* em = (ParticleEmitter*)lua_newuserdata(L, sizeof(ParticleEmitter));
    memset(em, 0, sizeof(ParticleEmitter));
    em->magic = LT::LT_MAGIC_EMITTER;  // Phase G.1.7 — type tag
    em->capacity = cap;
    em->pool = (Particle*)calloc(cap, sizeof(Particle));
    // 默认参数
    em->emitRate = 100.0f;
    em->minLife = 1.0f; em->maxLife = 2.0f;
    em->minSpeed = 50.0f; em->maxSpeed = 100.0f;
    em->minAngle = 0; em->maxAngle = (float)(2 * M_PI);
    em->startR = 1; em->startG = 1; em->startB = 1; em->startA = 1;
    em->endR = 1; em->endG = 1; em->endB = 1; em->endA = 0;
    em->minSize = 4; em->maxSize = 8;
    lua_setfield(L, 1, "__instance");

    // GC: 释放粒子池
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        auto* e = CheckEmitter(Ls, 1);
        if (e && e->pool) { free(e->pool); e->pool = nullptr; }
        return 0;
    });
    lua_setfield(L, 1, "__gc_release");

    return 0;
}

static int l_Particles_SetPosition(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) { em->posX = (float)luaL_checknumber(L, 2); em->posY = (float)luaL_checknumber(L, 3); }
    return 0;
}

static int l_Particles_SetEmitRate(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) em->emitRate = (float)luaL_checknumber(L, 2);
    return 0;
}

static int l_Particles_SetLifeRange(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) { em->minLife = (float)luaL_checknumber(L, 2); em->maxLife = (float)luaL_checknumber(L, 3); }
    return 0;
}

static int l_Particles_SetSpeedRange(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) { em->minSpeed = (float)luaL_checknumber(L, 2); em->maxSpeed = (float)luaL_checknumber(L, 3); }
    return 0;
}

static int l_Particles_SetAngleRange(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) { em->minAngle = (float)luaL_checknumber(L, 2); em->maxAngle = (float)luaL_checknumber(L, 3); }
    return 0;
}

static int l_Particles_SetColorRange(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (!em) return 0;
    // arg 2: start {r,g,b,a}, arg 3: end {r,g,b,a}
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_rawgeti(L, 2, 1); em->startR = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 2, 2); em->startG = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 2, 3); em->startB = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 2, 4); em->startA = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 3, 1); em->endR = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 3, 2); em->endG = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 3, 3); em->endB = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 3, 4); em->endA = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    return 0;
}

static int l_Particles_SetSizeRange(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) { em->minSize = (float)luaL_checknumber(L, 2); em->maxSize = (float)luaL_checknumber(L, 3); }
    return 0;
}

static int l_Particles_SetGravity(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) { em->gravX = (float)luaL_checknumber(L, 2); em->gravY = (float)luaL_checknumber(L, 3); }
    return 0;
}

static int l_Particles_Start(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) em->running = true;
    return 0;
}

static int l_Particles_Stop(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    if (em) em->running = false;
    return 0;
}

static int l_Particles_Update(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    float dt = (float)luaL_checknumber(L, 2);
    EmitterUpdate(em, dt);
    return 0;
}

static int l_Particles_Draw(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    EmitterDraw(em);
    return 0;
}

static int l_Particles_GetCount(lua_State* L) {
    auto* em = CheckEmitter(L, 1);
    lua_pushinteger(L, em ? em->activeCount : 0);
    return 1;
}

static int l_Particles_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics.Particles");
    return 1;
}

// ==================== luaopen 注册 ====================

static void EnsureGraphicsTable(lua_State* L) {
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Graphics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Graphics");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
}

static const luaL_Reg particles_funcs[] = {
    {"SetPosition",   l_Particles_SetPosition},
    {"SetEmitRate",   l_Particles_SetEmitRate},
    {"SetLifeRange",  l_Particles_SetLifeRange},
    {"SetSpeedRange", l_Particles_SetSpeedRange},
    {"SetAngleRange", l_Particles_SetAngleRange},
    {"SetColorRange", l_Particles_SetColorRange},
    {"SetSizeRange",  l_Particles_SetSizeRange},
    {"SetGravity",    l_Particles_SetGravity},
    {"Start",         l_Particles_Start},
    {"Stop",          l_Particles_Stop},
    {"Update",        l_Particles_Update},
    {"Draw",          l_Particles_Draw},
    {"GetCount",      l_Particles_GetCount},
    {"__call",        l_Particles_Call},
    {"__tostring",    l_Particles_Tostring},
    {NULL, NULL}
};

int luaopen_Light_Graphics_Particles(lua_State* L) {
    EnsureGraphicsTable(L);
    lua_pushstring(L, "Particles");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Particles");
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, particles_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Particles");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
