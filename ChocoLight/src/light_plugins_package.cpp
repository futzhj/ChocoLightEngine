#include "resource_package.h"

#include <new>

namespace {

using LT::Resource::PackageHandle;

PackageHandle* CheckPackage(lua_State* L, int idx) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, idx, "Light.Plugins.Package.Handle"));
    if (!h || h->magic != LT::Resource::LT_MAGIC_PACKAGE || !h->package) {
        luaL_error(L, "Light.Plugins.Package: invalid or closed handle");
        return nullptr;
    }
    return h;
}

void PushInfo(lua_State* L, const LT::Resource::PackageInfo& info) {
    lua_createtable(L, 0, 6);
    lua_pushstring(L, info.kind.c_str());
    lua_setfield(L, -2, "kind");
    lua_pushstring(L, info.subtype.c_str());
    lua_setfield(L, -2, "subtype");
    lua_pushstring(L, info.path.c_str());
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, static_cast<lua_Integer>(info.count));
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, static_cast<lua_Integer>(info.indexOffset));
    lua_setfield(L, -2, "indexOffset");
    lua_pushboolean(L, info.supported ? 1 : 0);
    lua_setfield(L, -2, "supported");
}

LT::Resource::ReadOptions ReadOptionsFromLua(lua_State* L, int idx) {
    LT::Resource::ReadOptions opts;
    int type = lua_type(L, idx);
    if (type == LUA_TNONE || type == LUA_TNIL) return opts;
    if (type != LUA_TTABLE) {
        luaL_typerror(L, idx, "table or nil");
        return opts;
    }

    lua_getfield(L, idx, "raw");
    opts.raw = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, idx, "decode");
    if (!lua_isnil(L, -1)) opts.decode = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, idx, "maxBytes");
    if (lua_isnumber(L, -1)) opts.maxBytes = static_cast<uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    return opts;
}

std::string KeyFromLua(lua_State* L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        return std::to_string(static_cast<uint32_t>(luaL_checkinteger(L, idx)));
    }

    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    return std::string(s, len);
}

int l_Package_Probe(lua_State* L) {
    const char* path = LT::CheckStringStrict(L, 1);
    LT::Resource::PackageInfo info;
    std::string err;
    if (!LT::Resource::ProbePackageFile(path, info, err)) return LT::PushNilError(L, "%s", err.c_str());
    PushInfo(L, info);
    return 1;
}

int l_Package_Open(lua_State* L) {
    const char* path = LT::CheckStringStrict(L, 1);
    std::string err;
    auto pkg = LT::Resource::OpenPackageFile(path, err);
    if (!pkg) return LT::PushNilError(L, "%s", err.c_str());

    auto* h = static_cast<PackageHandle*>(lua_newuserdata(L, sizeof(PackageHandle)));
    new (h) PackageHandle();
    h->package = std::move(pkg);

    luaL_getmetatable(L, "Light.Plugins.Package.Handle");
    lua_setmetatable(L, -2);
    return 1;
}

int l_Handle_GetInfo(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    PushInfo(L, h->package->Info());
    return 1;
}

int l_Handle_List(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    const auto& entries = h->package->Entries();
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    int arr = lua_gettop(L);

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        lua_createtable(L, 0, 8);

        if (!e.keyText.empty() && e.md5.empty()) lua_pushinteger(L, static_cast<lua_Integer>(e.id));
        else lua_pushstring(L, e.keyText.c_str());
        lua_setfield(L, -2, "key");

        lua_pushinteger(L, static_cast<lua_Integer>(e.id));
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, static_cast<lua_Integer>(e.offset));
        lua_setfield(L, -2, "offset");
        lua_pushinteger(L, static_cast<lua_Integer>(e.size));
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, static_cast<lua_Integer>(e.packedSize));
        lua_setfield(L, -2, "packedSize");
        lua_pushinteger(L, static_cast<lua_Integer>(e.archive));
        lua_setfield(L, -2, "archive");
        if (!e.md5.empty()) {
            lua_pushstring(L, e.md5.c_str());
            lua_setfield(L, -2, "md5");
        }

        lua_rawseti(L, arr, static_cast<int>(i + 1));
    }
    return 1;
}

int l_Handle_Has(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    std::string key = KeyFromLua(L, 2);
    lua_pushboolean(L, h->package->Has(key) ? 1 : 0);
    return 1;
}

int l_Handle_GetData(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    std::string key = KeyFromLua(L, 2);
    LT::Resource::ReadOptions opts = ReadOptionsFromLua(L, 3);
    std::string data;
    std::string err;
    if (!h->package->Read(key, opts, data, err)) return LT::PushNilError(L, "%s", err.c_str());
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int l_Handle_Close(lua_State* L) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, 1, "Light.Plugins.Package.Handle"));
    if (h && h->magic == LT::Resource::LT_MAGIC_PACKAGE) {
        h->package.reset();
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_Handle_GC(lua_State* L) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, 1, "Light.Plugins.Package.Handle"));
    if (h && h->magic != LT::LT_MAGIC_DEAD) {
        h->package.reset();
        h->magic = LT::LT_MAGIC_DEAD;
        h->~PackageHandle();
    }
    return 0;
}

int l_Handle_Tostring(lua_State* L) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, 1, "Light.Plugins.Package.Handle"));
    lua_pushfstring(L, "Light.Plugins.Package.Handle: %p", h);
    return 1;
}

const luaL_Reg kPackageFns[] = {
    {"Probe", l_Package_Probe},
    {"Open", l_Package_Open},
    {nullptr, nullptr}
};

const luaL_Reg kHandleFns[] = {
    {"GetInfo", l_Handle_GetInfo},
    {"List", l_Handle_List},
    {"Has", l_Handle_Has},
    {"GetData", l_Handle_GetData},
    {"Close", l_Handle_Close},
    {"__gc", l_Handle_GC},
    {"__tostring", l_Handle_Tostring},
    {nullptr, nullptr}
};

} // namespace

extern "C" LIGHT_API int luaopen_Light_Plugins_Package(lua_State* L) {
    if (luaL_newmetatable(L, "Light.Plugins.Package.Handle")) {
        luaL_setfuncs(L, kHandleFns, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    LT::Resource::RegisterPluginsSubmodule(L, "Package", kPackageFns);
    return 1;
}
