/**
 * @file light_data.cpp
 * @brief Light.Data 模块 — 二进制数据缓冲区管理
 * @note 还原自 Light.dll 地址范围 0x1800A5A20-0x1800A6950
 * 
 * Data 模块提供 Lua 层的二进制数据操作能力:
 * - Push/Pop: 栈式数据追加/弹出
 * - Insert/Delete: 随机位置插入/删除
 * - Shift/Unshift: 头部弹出/头部插入
 * - Count: 获取缓冲区大小
 * - GetPointer: 获取原始指针 (供与 C 交互)
 * - At/__index: 按字节索引读取
 * - __newindex: 按字节索引写入
 * - __call: 创建新缓冲区实例
 * - __tostring: 转换为字符串表示
 */

#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7 — 类型安全 helpers + magic

// ==================== 内部数据结构 ====================

/// @brief 缓冲区用户数据 — 对应 CC::Safe<CC::Safe<unsigned char[]>>
/// Phase G.1.7: 首字段 magic 防止 type-confusion
struct DataBuffer {
    uint32_t magic;          // 必须 = LT_MAGIC_DATABUF
    std::vector<uint8_t> data;
};

/// @brief 从 Lua 表中获取 Buffer 用户数据
static DataBuffer* CheckBuffer(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_pushstring(L, "__instance");
    lua_rawget(L, idx);
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    DataBuffer* buf = (DataBuffer*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return buf;
}

// ==================== 模块函数实现 ====================

/// Data.At / Data.__index — 按字节索引读取
static int l_Data_At(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_Integer idx = luaL_checkinteger(L, 2);
    if (idx < 0 || (size_t)idx >= buf->data.size()) {
        return luaL_error(L, "Index out of range");
    }
    lua_pushinteger(L, buf->data[idx]);
    return 1;
}

/// Data.__newindex — 按字节索引写入
static int l_Data_NewIndex(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_Integer idx = luaL_checkinteger(L, 2);
    lua_Integer val = luaL_checkinteger(L, 3);
    if (idx >= 0 && (size_t)idx < buf->data.size()) {
        buf->data[idx] = (uint8_t)val;
    }
    return 0;
}

/// Data.Count — 获取缓冲区大小
static int l_Data_Count(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_pushinteger(L, (lua_Integer)buf->data.size());
    return 1;
}

/// Data.GetPointer — 获取原始数据指针 (lightuserdata)
static int l_Data_GetPointer(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_pushlightuserdata(L, buf->data.data());
    return 1;
}

/// Data.Push — 追加数据 (支持多种类型标识符)
static int l_Data_Push(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    int argc = lua_gettop(L);
    
    if (argc == 3) {
        // Push(buf, type_string, value)
        const char* typeStr = luaL_checkstring(L, 2);
        uint32_t typeId = *(uint32_t*)typeStr;
        
        // 根据类型标识符推入不同大小的数据
        // si8/si16/si32/si64 — 有符号整数
        // ui8/ui16/ui32/ui64 — 无符号整数
        // fl32 — 浮点数
        // byte/char — 单字节
        // dbl — 双精度
        
        if (typeId == 0x38316973 || typeId == 0x38316975) {  // "si8" / "ui8"
            uint16_t val = (uint16_t)luaL_checkinteger(L, 3);
            buf->data.push_back(val & 0xFF);
            buf->data.push_back((val >> 8) & 0xFF);
        } else if (typeId == 0x32336973 || typeId == 0x32336975) {  // "si32" / "ui32"
            uint32_t val = (uint32_t)luaL_checkinteger(L, 3);
            for (int i = 0; i < 4; ++i) {
                buf->data.push_back((val >> (i * 8)) & 0xFF);
            }
        } else if (typeId == 0x34366973 || typeId == 0x34366975) {  // "si64" / "ui64"
            uint64_t val = (uint64_t)luaL_checkinteger(L, 3);
            for (int i = 0; i < 8; ++i) {
                buf->data.push_back((val >> (i * 8)) & 0xFF);
            }
        } else if (typeId == 0x36316C66) {  // "fl32"
            float val = (float)luaL_checknumber(L, 3);
            uint8_t* bytes = (uint8_t*)&val;
            buf->data.insert(buf->data.end(), bytes, bytes + 4);
        } else if (typeId == 0x65747962 || typeId == 0x72616863) {  // "byte" / "char"
            uint8_t val = (uint8_t)luaL_checkinteger(L, 3);
            buf->data.push_back(val);
        } else {
            // 默认: dbl (double, 8字节)
            double val = luaL_checknumber(L, 3);
            uint8_t* bytes = (uint8_t*)&val;
            buf->data.insert(buf->data.end(), bytes, bytes + 8);
        }
    } else if (argc == 4) {
        // Push(buf, "void", data_source, length)
        size_t len = (size_t)luaL_checkinteger(L, 4);
        
        if (lua_isstring(L, 3)) {
            const char* src = luaL_checkstring(L, 3);
            buf->data.insert(buf->data.end(), (uint8_t*)src, (uint8_t*)src + len);
        } else if (lua_isuserdata(L, 3)) {
            void* ptr = lua_touserdata(L, 3);
            buf->data.insert(buf->data.end(), (uint8_t*)ptr, (uint8_t*)ptr + len);
        } else {
            return luaL_error(L, "Buffer should be cdata or userdata, but give: %s",
                            lua_typename(L, lua_type(L, 3)));
        }
    } else {
        // Push(buf, raw_string)
        size_t len;
        const char* src = luaL_checklstring(L, 2, &len);
        buf->data.insert(buf->data.end(), (uint8_t*)src, (uint8_t*)src + len);
    }
    
    lua_pushvalue(L, 1);
    return 1;
}

/// Data.Pop — 弹出尾部数据
static int l_Data_Pop(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_Integer count = luaL_optinteger(L, 2, 1);
    for (lua_Integer i = 0; i < count && !buf->data.empty(); ++i) {
        buf->data.pop_back();
    }
    lua_pushvalue(L, 1);
    return 1;
}

/// Data.Insert — 在指定位置插入数据
static int l_Data_Insert(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_Integer pos = luaL_checkinteger(L, 2);
    lua_Integer val = luaL_checkinteger(L, 3);
    
    if (pos >= 0 && (size_t)pos <= buf->data.size()) {
        buf->data.insert(buf->data.begin() + pos, (uint8_t)val);
    }
    lua_pushvalue(L, 1);
    return 1;
}

/// Data.Delete — 删除指定位置的数据
static int l_Data_Delete(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_Integer pos = luaL_checkinteger(L, 2);
    if (pos >= 0 && (size_t)pos < buf->data.size()) {
        buf->data.erase(buf->data.begin() + pos);
    }
    lua_pushvalue(L, 1);
    return 1;
}

/// Data.Shift — 弹出头部数据
static int l_Data_Shift(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    if (!buf->data.empty()) {
        lua_pushinteger(L, buf->data.front());
        buf->data.erase(buf->data.begin());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// Data.Unshift — 在头部插入数据
static int l_Data_Unshift(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_Integer val = luaL_checkinteger(L, 2);
    buf->data.insert(buf->data.begin(), (uint8_t)val);
    lua_pushvalue(L, 1);
    return 1;
}

/// Data.__gc — GC 回收
static int l_Data_GC(lua_State* L) {
    DataBuffer* buf = (DataBuffer*)lua_touserdata(L, 1);
    if (buf) {
        buf->~DataBuffer();
    }
    return 0;
}

/// Data.__call — 构造新缓冲区
static int l_Data_Call(lua_State* L) {
    int argc = lua_gettop(L);
    
    luaL_checktype(L, 1, LUA_TTABLE);
    
    // 创建 userdata 存储 DataBuffer
    lua_pushstring(L, "__instance");
    DataBuffer* buf = (DataBuffer*)lua_newuserdata(L, sizeof(DataBuffer));
    new (buf) DataBuffer();  // placement new
    buf->magic = LT::LT_MAGIC_DATABUF;  // Phase G.1.7 — type tag (placement-new 后设)
    
    // 设置 __gc 元表
    lua_createtable(L, 0, 0);
    lua_pushcfunction(L, l_Data_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    
    lua_rawset(L, -3);
    
    // 根据参数初始化缓冲区
    if (argc >= 3) {
        // Data(size) 或 Data(source, size)
        if (lua_isstring(L, 2)) {
            size_t len;
            const char* src = lua_tolstring(L, 2, &len);
            lua_Integer size = luaL_checkinteger(L, 3);
            buf->data.assign((uint8_t*)src, (uint8_t*)src + std::min(len, (size_t)size));
        } else if (lua_isuserdata(L, 2)) {
            void* ptr = lua_touserdata(L, 2);
            lua_Integer size = luaL_checkinteger(L, 3);
            buf->data.assign((uint8_t*)ptr, (uint8_t*)ptr + size);
        }
    } else if (argc == 2) {
        lua_Integer size = luaL_checkinteger(L, 2);
        buf->data.resize((size_t)size, 0);
    }
    
    return 0;
}

/// Data.__tostring — 转字符串表示
static int l_Data_ToString(lua_State* L) {
    DataBuffer* buf = CheckBuffer(L, 1);
    if (!buf) return luaL_error(L, "Illegal type");
    
    lua_pushlstring(L, (const char*)buf->data.data(), buf->data.size());
    return 1;
}

// ==================== luaopen_Light_Data ====================
// 原始地址: sub_1800A6640 (导出)

static const luaL_Reg data_funcs[] = {
    {"Push",        l_Data_Push},
    {"Pop",         l_Data_Pop},
    {"Insert",      l_Data_Insert},
    {"Delete",      l_Data_Delete},
    {"Shift",       l_Data_Shift},
    {"Unshift",     l_Data_Unshift},
    {"Count",       l_Data_Count},
    {"GetPointer",  l_Data_GetPointer},
    {"At",          l_Data_At},
    {"__index",     l_Data_At},
    {"__newindex",  l_Data_NewIndex},
    {"__call",      l_Data_Call},
    {"__tostring",  l_Data_ToString},
    {NULL, NULL}
};

int luaopen_Light_Data(lua_State* L) {
    LT::RegisterModule(L, "Data", data_funcs);
    return 1;
}
