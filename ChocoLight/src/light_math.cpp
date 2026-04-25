/**
 * @file light_math.cpp
 * @brief Light.Math 模块 — 数学工具函数
 * @note 还原自 Light.dll 地址范围 0x1800A6C90-0x1800A6F30
 */

#include "light.h"

// ==================== CWHash 算法 ====================
// 原始地址: sub_1800A6CD0
// 自定义哈希算法 — 路径归一化后计算带魔数的混合乘法散列

static uint32_t CWHash(const char* source) {
    char buf[280] = {0};
    if (!source) return 0;
    
    strncpy(buf, source, 260);
    
    // 路径归一化: 大写转小写, '/' 转 '\\'
    for (char* p = buf; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = *p + 32;  // 转小写
        } else if (*p == '/') {
            *p = '\\';     // 统一分隔符
        }
    }
    
    // 计算有效长度 (按 uint32_t 对齐, 最多64个 DWORD)
    uint32_t len = 0;
    uint32_t* dwords = (uint32_t*)buf;
    while (len < 64 && dwords[len]) {
        if (!dwords[len + 1]) { len += 1; break; }
        if (!dwords[len + 2]) { len += 2; break; }
        if (!dwords[len + 3]) { len += 3; break; }
        len += 4;
    }
    
    // 追加魔数尾部
    dwords[len]     = 0x9BFE8468;  // 魔数1: -1679342520
    dwords[len + 1] = 0x66F40C58;  // 魔数2: 1727278152
    
    // 混合乘法散列
    int32_t rotVal   = (int32_t)0xF4F70C38;  // -184907480
    uint32_t accA    = 0x7759799B;            // 2002301995
    uint32_t accB    = 0x37A3170E;            // 933775118
    uint32_t count   = len + 2;
    
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t xA = dwords[i] ^ accA;
        uint32_t xB = dwords[i] ^ accB;
        
        // 左旋 rotVal
        rotVal = (rotVal << 1) | ((uint32_t)rotVal >> 31);
        
        uint32_t mask1 = ((rotVal ^ 0x267B0B11) + xA) & 0xBDEB77DE | 0x2040801;
        uint64_t prod1 = (uint64_t)xB * mask1;
        uint32_t lo1 = (uint32_t)prod1;
        uint32_t hi1 = (uint32_t)(prod1 >> 32);
        uint32_t v1 = hi1 + lo1;
        if (hi1 == 0) {
            accB = (uint32_t)prod1;
        } else {
            accB = v1 + 1;
            if (hi1 == 0) accB = v1;
        }
        
        uint32_t mask2 = ((rotVal ^ 0x267B0B11) + xB) & 0x7D7EBBDE | 0x804021;
        uint64_t prod2 = (uint64_t)xA * mask2 + 2 * ((uint64_t)(xA * (uint64_t)mask2) >> 32);
        accA = (uint32_t)prod2 + 2;
        if ((uint32_t)(prod2 >> 32) == 0) {
            accA = (uint32_t)prod2;
        }
    }
    
    return accA ^ accB;
}

// ==================== Lua 绑定: CWHash ====================
// 原始地址: sub_1800A6C90

static int l_CWHash(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    uint32_t hash = CWHash(str);
    lua_pushinteger(L, hash);
    return 1;
}

// ==================== luaopen_Light_Math ====================
// 原始地址: 0x1800A6F10 (导出)

static const luaL_Reg math_funcs[] = {
    {"CWHash", l_CWHash},
    {NULL, NULL}
};

int luaopen_Light_Math(lua_State* L) {
    LT::RegisterModule(L, "Math", math_funcs);
    return 1;
}
