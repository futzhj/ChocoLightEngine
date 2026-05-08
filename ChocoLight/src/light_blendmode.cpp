/**
 * @file light_blendmode.cpp
 * @brief Light.BlendMode module - SDL3 custom blend mode factory
 *
 * Lua API (1 fn):
 *
 *    Light.BlendMode.Compose(
 *        srcColorFactor, dstColorFactor, colorOperation,
 *        srcAlphaFactor, dstAlphaFactor, alphaOperation
 *    ) -> blend_mode (u32)
 *
 *    The six arguments are SDL_BlendFactor / SDL_BlendOperation enum
 *    values (see FACTOR_* / OP_* constants below). Returns a u32 that
 *    can be fed to any SDL renderer / texture SetBlendMode API.
 *    Returns INVALID (0x7FFFFFFF) if the combination is unsupported
 *    by the current renderer.
 *
 * Constants (23):
 *
 *  Preset blend modes (8):
 *    NONE                  0x00000000
 *    BLEND                 0x00000001    alpha blending
 *    BLEND_PREMULTIPLIED   0x00000010    pre-multiplied alpha
 *    ADD                   0x00000002    additive
 *    ADD_PREMULTIPLIED     0x00000020    additive pre-multiplied
 *    MOD                   0x00000004    color modulate
 *    MUL                   0x00000008    color multiply
 *    INVALID               0x7FFFFFFF    sentinel
 *
 *  Blend operations (5):
 *    OP_ADD, OP_SUBTRACT, OP_REV_SUBTRACT, OP_MINIMUM, OP_MAXIMUM
 *
 *  Blend factors (10):
 *    FACTOR_ZERO, FACTOR_ONE,
 *    FACTOR_SRC_COLOR, FACTOR_ONE_MINUS_SRC_COLOR,
 *    FACTOR_SRC_ALPHA, FACTOR_ONE_MINUS_SRC_ALPHA,
 *    FACTOR_DST_COLOR, FACTOR_ONE_MINUS_DST_COLOR,
 *    FACTOR_DST_ALPHA, FACTOR_ONE_MINUS_DST_ALPHA
 *
 * No SDL_Init dependency: the composition is a pure bit-pack operation
 * performed at call site.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int l_BlendMode_Compose(lua_State* L) {
    SDL_BlendFactor    srcColorF = (SDL_BlendFactor)luaL_checkinteger(L, 1);
    SDL_BlendFactor    dstColorF = (SDL_BlendFactor)luaL_checkinteger(L, 2);
    SDL_BlendOperation colorOp   = (SDL_BlendOperation)luaL_checkinteger(L, 3);
    SDL_BlendFactor    srcAlphaF = (SDL_BlendFactor)luaL_checkinteger(L, 4);
    SDL_BlendFactor    dstAlphaF = (SDL_BlendFactor)luaL_checkinteger(L, 5);
    SDL_BlendOperation alphaOp   = (SDL_BlendOperation)luaL_checkinteger(L, 6);

    SDL_BlendMode mode = SDL_ComposeCustomBlendMode(srcColorF, dstColorF, colorOp,
                                                    srcAlphaF, dstAlphaF, alphaOp);
    lua_pushnumber(L, (lua_Number)mode);
    return 1;
}

static const luaL_Reg kBlendModeReg[] = {
    { "Compose", l_BlendMode_Compose },
    { nullptr, nullptr },
};

#define LIGHT_PUSH_CONST(NAME, VALUE)       \
    lua_pushnumber(L, (lua_Number)(VALUE)); \
    lua_setfield(L, -2, NAME)

extern "C" LIGHT_API int luaopen_Light_BlendMode(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kBlendModeReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Preset blend modes (u32)
    LIGHT_PUSH_CONST("NONE",                 SDL_BLENDMODE_NONE);
    LIGHT_PUSH_CONST("BLEND",                SDL_BLENDMODE_BLEND);
    LIGHT_PUSH_CONST("BLEND_PREMULTIPLIED",  SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    LIGHT_PUSH_CONST("ADD",                  SDL_BLENDMODE_ADD);
    LIGHT_PUSH_CONST("ADD_PREMULTIPLIED",    SDL_BLENDMODE_ADD_PREMULTIPLIED);
    LIGHT_PUSH_CONST("MOD",                  SDL_BLENDMODE_MOD);
    LIGHT_PUSH_CONST("MUL",                  SDL_BLENDMODE_MUL);
    LIGHT_PUSH_CONST("INVALID",              SDL_BLENDMODE_INVALID);

    // Operations
    LIGHT_PUSH_CONST("OP_ADD",           SDL_BLENDOPERATION_ADD);
    LIGHT_PUSH_CONST("OP_SUBTRACT",      SDL_BLENDOPERATION_SUBTRACT);
    LIGHT_PUSH_CONST("OP_REV_SUBTRACT",  SDL_BLENDOPERATION_REV_SUBTRACT);
    LIGHT_PUSH_CONST("OP_MINIMUM",       SDL_BLENDOPERATION_MINIMUM);
    LIGHT_PUSH_CONST("OP_MAXIMUM",       SDL_BLENDOPERATION_MAXIMUM);

    // Factors
    LIGHT_PUSH_CONST("FACTOR_ZERO",                SDL_BLENDFACTOR_ZERO);
    LIGHT_PUSH_CONST("FACTOR_ONE",                 SDL_BLENDFACTOR_ONE);
    LIGHT_PUSH_CONST("FACTOR_SRC_COLOR",           SDL_BLENDFACTOR_SRC_COLOR);
    LIGHT_PUSH_CONST("FACTOR_ONE_MINUS_SRC_COLOR", SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR);
    LIGHT_PUSH_CONST("FACTOR_SRC_ALPHA",           SDL_BLENDFACTOR_SRC_ALPHA);
    LIGHT_PUSH_CONST("FACTOR_ONE_MINUS_SRC_ALPHA", SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
    LIGHT_PUSH_CONST("FACTOR_DST_COLOR",           SDL_BLENDFACTOR_DST_COLOR);
    LIGHT_PUSH_CONST("FACTOR_ONE_MINUS_DST_COLOR", SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR);
    LIGHT_PUSH_CONST("FACTOR_DST_ALPHA",           SDL_BLENDFACTOR_DST_ALPHA);
    LIGHT_PUSH_CONST("FACTOR_ONE_MINUS_DST_ALPHA", SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA);

    return 1;
}

#undef LIGHT_PUSH_CONST
