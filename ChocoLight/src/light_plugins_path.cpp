#include "light.h"
#include "light_utils_core.h"

#include <string>

namespace {

static bool IsSep(char c) {
    return c == '/' || c == '\\';
}

static char Sep() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

static std::string TrimTrailingSeparators(const std::string& path) {
    if (path.empty()) return path;
    size_t end = path.size();
    while (end > 1 && IsSep(path[end - 1])) {
        if (end == 3 && path[1] == ':') break;
        --end;
    }
    return path.substr(0, end);
}

static size_t LastSep(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t backslash = path.find_last_of('\\');
    if (slash == std::string::npos) return backslash;
    if (backslash == std::string::npos) return slash;
    return slash > backslash ? slash : backslash;
}

static bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
    if (IsSep(path[0])) return true;
    return path.size() >= 3 && path[1] == ':' && IsSep(path[2]);
}

static std::string BaseNameOf(const std::string& input) {
    std::string path = TrimTrailingSeparators(input);
    size_t pos = LastSep(path);
    if (pos == std::string::npos) return path;
    if (pos + 1 >= path.size()) return path;
    return path.substr(pos + 1);
}

static int l_Join(lua_State* L) {
    int argc = lua_gettop(L);
    std::string out = LT::Utils::JoinPathParts(L, 1, argc);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Normalize(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    std::string out = LT::Utils::NormalizePath(std::string(path, len), Sep());
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Basename(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    std::string out = BaseNameOf(std::string(path, len));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Dirname(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    std::string path = TrimTrailingSeparators(std::string(raw, len));
    size_t pos = LastSep(path);
    if (pos == std::string::npos) {
        lua_pushliteral(L, ".");
        return 1;
    }
    if (pos == 0) {
        char root[2] = {path[0], '\0'};
        lua_pushstring(L, root);
        return 1;
    }
    std::string out = path.substr(0, pos);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Extname(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    std::string base = BaseNameOf(std::string(raw, len));
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    std::string out = base.substr(dot);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Stem(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    std::string base = BaseNameOf(std::string(raw, len));
    size_t dot = base.find_last_of('.');
    std::string out = (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_IsAbsolute(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, IsAbsolutePath(std::string(raw, len)) ? 1 : 0);
    return 1;
}

static int l_Split(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    std::string normalized = LT::Utils::NormalizePath(std::string(raw, len), '/');
    lua_newtable(L);
    int index = 1;
    size_t pos = 0;
    while (pos < normalized.size()) {
        while (pos < normalized.size() && IsSep(normalized[pos])) ++pos;
        size_t start = pos;
        while (pos < normalized.size() && !IsSep(normalized[pos])) ++pos;
        if (pos > start) {
            std::string part = normalized.substr(start, pos - start);
            lua_pushinteger(L, index++);
            lua_pushlstring(L, part.data(), part.size());
            lua_rawset(L, -3);
        }
    }
    return 1;
}

static int l_Separator(lua_State* L) {
    char sep[2] = {Sep(), '\0'};
    lua_pushstring(L, sep);
    return 1;
}

}

extern "C" LIGHT_API int luaopen_Light_Plugins_Path(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"Join",       l_Join},
        {"Normalize",  l_Normalize},
        {"Basename",   l_Basename},
        {"Dirname",    l_Dirname},
        {"Extname",    l_Extname},
        {"Stem",       l_Stem},
        {"IsAbsolute", l_IsAbsolute},
        {"Split",      l_Split},
        {"Separator",  l_Separator},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPluginsSubmodule(L, "Path", funcs);
    return 1;
}
