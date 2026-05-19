#include "light.h"
#include "light_utils_core.h"

#include "cJSON.h"

#include <string>

namespace {

static const char* kNullKey = "Light.Plugins.JSON.Null";

static int AbsIndex(lua_State* L, int idx) {
    return idx < 0 ? lua_gettop(L) + idx + 1 : idx;
}

static void PushNullSentinel(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kNullKey);
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, kNullKey);
}

static bool IsNullSentinel(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return false;
    int absIdx = AbsIndex(L, idx);
    PushNullSentinel(L);
    bool same = lua_rawequal(L, absIdx, -1) != 0;
    lua_pop(L, 1);
    return same;
}

static void PushCJsonAsLua(lua_State* L, cJSON* node) {
    if (!node || cJSON_IsNull(node)) {
        PushNullSentinel(L);
        return;
    }
    if (cJSON_IsBool(node)) {
        lua_pushboolean(L, cJSON_IsTrue(node) ? 1 : 0);
        return;
    }
    if (cJSON_IsNumber(node)) {
        lua_pushnumber(L, node->valuedouble);
        return;
    }
    if (cJSON_IsString(node)) {
        const char* value = cJSON_GetStringValue(node);
        lua_pushstring(L, value ? value : "");
        return;
    }
    if (cJSON_IsArray(node)) {
        int n = cJSON_GetArraySize(node);
        lua_createtable(L, n, 0);
        for (int i = 0; i < n; ++i) {
            PushCJsonAsLua(L, cJSON_GetArrayItem(node, i));
            lua_rawseti(L, -2, i + 1);
        }
        return;
    }
    if (cJSON_IsObject(node)) {
        lua_newtable(L);
        for (cJSON* child = node->child; child; child = child->next) {
            PushCJsonAsLua(L, child);
            lua_setfield(L, -2, child->string ? child->string : "");
        }
        return;
    }
    lua_pushnil(L);
}

static bool IsIntegerKey(lua_State* L, int idx, int* outKey) {
    if (lua_type(L, idx) != LUA_TNUMBER) return false;
    lua_Number n = lua_tonumber(L, idx);
    int i = static_cast<int>(n);
    if (n != static_cast<lua_Number>(i) || i < 1) return false;
    if (outKey) *outKey = i;
    return true;
}

static bool AnalyzeArray(lua_State* L, int tableIdx, int* outLen) {
    int count = 0;
    int maxKey = 0;
    lua_pushnil(L);
    while (lua_next(L, tableIdx) != 0) {
        int key = 0;
        bool integerKey = IsIntegerKey(L, -2, &key);
        lua_pop(L, 1);
        if (!integerKey) {
            lua_pop(L, 1);
            if (outLen) *outLen = 0;
            return false;
        }
        ++count;
        if (key > maxKey) maxKey = key;
    }
    if (outLen) *outLen = maxKey;
    return count > 0 && count == maxKey;
}

static cJSON* LuaToCJson(lua_State* L, int idx, int seenIdx, std::string& err, int depth) {
    if (depth > 64) {
        err = "Encode: table nesting is too deep";
        return nullptr;
    }

    int absIdx = AbsIndex(L, idx);
    int t = lua_type(L, absIdx);
    switch (t) {
        case LUA_TNIL:
            return cJSON_CreateNull();
        case LUA_TBOOLEAN:
            return cJSON_CreateBool(lua_toboolean(L, absIdx));
        case LUA_TNUMBER:
            return cJSON_CreateNumber(lua_tonumber(L, absIdx));
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, absIdx);
            return cJSON_CreateString(s ? s : "");
        }
        case LUA_TTABLE:
            break;
        default:
            err = "Encode: unsupported Lua value type";
            return nullptr;
    }

    if (IsNullSentinel(L, absIdx)) return cJSON_CreateNull();

    lua_pushvalue(L, absIdx);
    lua_rawget(L, seenIdx);
    bool seen = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (seen) {
        err = "Encode: table cycle detected";
        return nullptr;
    }

    lua_pushvalue(L, absIdx);
    lua_pushboolean(L, 1);
    lua_rawset(L, seenIdx);

    int arrayLen = 0;
    bool isArray = AnalyzeArray(L, absIdx, &arrayLen);
    cJSON* root = isArray ? cJSON_CreateArray() : cJSON_CreateObject();
    if (!root) {
        lua_pushvalue(L, absIdx);
        lua_pushnil(L);
        lua_rawset(L, seenIdx);
        return nullptr;
    }

    if (isArray) {
        for (int i = 1; i <= arrayLen; ++i) {
            lua_rawgeti(L, absIdx, i);
            cJSON* item = LuaToCJson(L, -1, seenIdx, err, depth + 1);
            lua_pop(L, 1);
            if (!item) {
                cJSON_Delete(root);
                lua_pushvalue(L, absIdx);
                lua_pushnil(L);
                lua_rawset(L, seenIdx);
                return nullptr;
            }
            cJSON_AddItemToArray(root, item);
        }
    } else {
        lua_pushnil(L);
        while (lua_next(L, absIdx) != 0) {
            lua_pushvalue(L, -2);
            size_t keyLen = 0;
            const char* key = lua_tolstring(L, -1, &keyLen);
            lua_pop(L, 1);
            if (!key) {
                lua_pop(L, 2);
                cJSON_Delete(root);
                lua_pushvalue(L, absIdx);
                lua_pushnil(L);
                lua_rawset(L, seenIdx);
                err = "Encode: object key is not stringable";
                return nullptr;
            }
            std::string keyCopy(key, keyLen);
            cJSON* item = LuaToCJson(L, -1, seenIdx, err, depth + 1);
            lua_pop(L, 1);
            if (!item) {
                lua_pop(L, 1);
                cJSON_Delete(root);
                lua_pushvalue(L, absIdx);
                lua_pushnil(L);
                lua_rawset(L, seenIdx);
                return nullptr;
            }
            cJSON_AddItemToObject(root, keyCopy.c_str(), item);
        }
    }

    lua_pushvalue(L, absIdx);
    lua_pushnil(L);
    lua_rawset(L, seenIdx);
    return root;
}

static int l_Encode(lua_State* L) {
    lua_newtable(L);
    int seenIdx = lua_gettop(L);
    std::string err;
    cJSON* root = LuaToCJson(L, 1, seenIdx, err, 0);
    lua_pop(L, 1);
    if (!root) {
        lua_pushnil(L);
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }

    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        lua_pushnil(L);
        lua_pushstring(L, "Encode: failed to serialize JSON");
        return 2;
    }

    lua_pushstring(L, text);
    cJSON_free(text);
    return 1;
}

static int l_Decode(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    cJSON* root = cJSON_Parse(text);
    if (!root) {
        lua_pushnil(L);
        lua_pushstring(L, "Decode: invalid JSON input");
        return 2;
    }
    PushCJsonAsLua(L, root);
    cJSON_Delete(root);
    return 1;
}

static int l_Null(lua_State* L) {
    PushNullSentinel(L);
    return 1;
}

static int l_IsNull(lua_State* L) {
    lua_pushboolean(L, IsNullSentinel(L, 1) ? 1 : 0);
    return 1;
}

}

extern "C" LIGHT_API int luaopen_Light_Plugins_JSON(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"Encode", l_Encode},
        {"Decode", l_Decode},
        {"Null",   l_Null},
        {"IsNull", l_IsNull},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPluginsSubmodule(L, "JSON", funcs);
    return 1;
}

extern "C" LIGHT_API int luaopen_json(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"encode", l_Encode},
        {"decode", l_Decode},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPlainTable(L, funcs);
    return 1;
}
