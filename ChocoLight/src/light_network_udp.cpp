/**
 * @file light_network_udp.cpp
 * @brief Phase BC T5 — Light.Network.Udp Lua 模块
 *
 * 暴露 raw UDP 数据报 send/recv API 给 Lua 脚本. 基于 PlatformNet T2/T3
 * 的跨平台 UDP 抽象 (桌面 libuv / 移动 POSIX / Web 空存根).
 *
 * Lua API:
 *   local Udp = require("Light.Network.Udp")
 *
 *   local sock = Udp.Open(port)              -- port=0 表示随机端口
 *   if sock then
 *       sock:OnReceive(function(host, port, data)
 *           print("recv from", host, port, #data)
 *       end)
 *       sock:Send("127.0.0.1", 9000, "hello")
 *       print(sock:GetLocalPort())
 *       -- 每帧 Light.Network.Resume() 驱动 PlatformNet::Poll()
 *       sock:Close()
 *   end
 *
 * 生命周期:
 *   - userdata GC 时自动 Close + 释放 callback ref
 *   - sock:Close() 显式关闭后, 再调任何方法返回 nil/false
 */

#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7.2 — 类型安全 helpers + magic
#include "light_platform_net.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cstring>
#include <string>

// ==================== userdata ====================

static const char* UDP_SOCKET_MT = "Light.Network.Udp.Socket";

/// Phase G.1.7.2: 首字段 magic 防止 type-confusion
struct UdpSocketUserdata {
    uint32_t  magic;       // 必须 = LT_MAGIC_NET_UDP
    uv_udp_s* sock;        // PlatformNet handle (nullptr 表示已关闭)
    int       cbRef;       // OnReceive 回调 registry ref (LUA_NOREF = 未设置)
    lua_State* L;          // 触发回调时所用 L (对 ChocoLight 单线程一致, 安全)
};

// 校验栈位置是 UDP socket userdata, 返回指针 (失败 lua_error)
// Phase G.1.7.2: magic 双保险
static UdpSocketUserdata* CheckUdpUd(lua_State* L, int idx) {
    auto* ud = (UdpSocketUserdata*)luaL_checkudata(L, idx, UDP_SOCKET_MT);
    if (ud && ud->magic != LT::LT_MAGIC_NET_UDP) {
        luaL_error(L, "Light.Network.Udp.Socket: type confusion at arg #%d", idx);
    }
    return ud;
}

// ==================== 内部回调桥 ====================

// PlatformNet 回调 → Lua callback ref
//
// 安全要点:
//   - 回调可能在 sock:Close 之后才到达 (PlatformNet 异步关闭)
//     ud->sock 已是 nullptr 时跳过, 保证不调用已 GC 的 Lua 状态
//   - cbRef 已被 unref 时 lua_rawgeti 返回 nil, pcall 跳过
//   - cb 中 Lua error: pcall 吞掉 + LOG_WARN, 不传染 PlatformNet
static void DispatchUdpRecv(UdpSocketUserdata* ud,
                             const char* host, uint16_t port,
                             const char* data, int len) {
    if (!ud || !ud->sock || !ud->L) return;
    if (ud->cbRef == LUA_NOREF || ud->cbRef == LUA_REFNIL) return;

    lua_State* L = ud->L;
    int top = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cbRef);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return;
    }
    lua_pushstring(L, host ? host : "");
    lua_pushinteger(L, (lua_Integer)port);
    lua_pushlstring(L, data ? data : "", (size_t)(len > 0 ? len : 0));

    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Light.Network.Udp.OnReceive callback error: %s",
                err ? err : "(unknown)");
    }
    lua_settop(L, top);
}

// ==================== Lua API ====================

// Light.Network.Udp.Open(port=0) -> sock | nil, err
//
// port=0 让系统分配, 用 sock:GetLocalPort() 查询实际端口.
// port>0 时 bind 到 0.0.0.0:port, 用于服务端监听.
static int l_Udp_Open(lua_State* L) {
    int port = (int)luaL_optinteger(L, 1, 0);
    if (port < 0 || port > 65535) {
        lua_pushnil(L);
        lua_pushstring(L, "port out of range (0..65535)");
        return 2;
    }

    uv_udp_s* sock = PlatformNet::CreateUdpSocket();
    if (!sock) {
        lua_pushnil(L);
        lua_pushstring(L, "CreateUdpSocket failed (PlatformNet not initialized?)");
        return 2;
    }
    // bind: 即使 port=0 也要 bind 到 0.0.0.0:0 才能接收 (libuv recv_start 需要 bind)
    if (!PlatformNet::BindUdp(sock, "0.0.0.0", (uint16_t)port)) {
        PlatformNet::CloseUdp(sock);
        lua_pushnil(L);
        lua_pushfstring(L, "BindUdp failed on port %d", port);
        return 2;
    }

    auto* ud = (UdpSocketUserdata*)lua_newuserdata(L, sizeof(UdpSocketUserdata));
    ud->magic = LT::LT_MAGIC_NET_UDP;  // Phase G.1.7.2 — type tag
    ud->sock  = sock;
    ud->cbRef = LUA_NOREF;
    ud->L     = L;
    luaL_getmetatable(L, UDP_SOCKET_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// sock:Send(host, port, data) -> bool, err
static int l_Udp_Send(lua_State* L) {
    auto* ud = CheckUdpUd(L, 1);
    if (!ud->sock) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "socket closed");
        return 2;
    }
    const char* host = luaL_checkstring(L, 2);
    int port = (int)luaL_checkinteger(L, 3);
    if (port <= 0 || port > 65535) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "destination port out of range");
        return 2;
    }
    size_t len = 0;
    const char* data = luaL_checklstring(L, 4, &len);
    if (len == 0) {
        // 允许空包 (KEEPALIVE 等), 但 PlatformNet 会拒绝, 直接成功
        lua_pushboolean(L, 1);
        return 1;
    }
    bool ok = PlatformNet::SendUdp(ud->sock, host, (uint16_t)port, data, len);
    if (!ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "SendUdp failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// sock:OnReceive(cb) -> ()
//   cb 签名: function(host:string, port:int, data:string)
//   传 nil 取消注册
static int l_Udp_OnReceive(lua_State* L) {
    auto* ud = CheckUdpUd(L, 1);
    if (!ud->sock) return 0;     // 已关闭, 静默忽略

    // 释放旧 ref
    if (ud->cbRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cbRef);
        ud->cbRef = LUA_NOREF;
    }

    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        ud->cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
        // 注册 PlatformNet 回调 (PlatformNet 内部 std::move 接管, 与现有 cb 替换)
        // ud 在 lambda 内通过指针 capture, lifetime 由 GC 管 — 见 __gc
        UdpSocketUserdata* udPtr = ud;
        PlatformNet::StartUdpRecv(ud->sock,
            [udPtr](const char* host, uint16_t port,
                     const char* data, int len) {
                DispatchUdpRecv(udPtr, host, port, data, len);
            });
    } else if (lua_isnil(L, 2) || lua_gettop(L) < 2) {
        // 取消接收
        PlatformNet::StopUdpRecv(ud->sock);
    } else {
        return luaL_error(L, "OnReceive expects function or nil");
    }
    return 0;
}

// sock:GetLocalPort() -> int
static int l_Udp_GetLocalPort(lua_State* L) {
    auto* ud = CheckUdpUd(L, 1);
    if (!ud->sock) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)PlatformNet::GetUdpLocalPort(ud->sock));
    return 1;
}

// sock:Close() -> ()
static int l_Udp_Close(lua_State* L) {
    auto* ud = CheckUdpUd(L, 1);
    if (ud->sock) {
        PlatformNet::CloseUdp(ud->sock);
        ud->sock = nullptr;
    }
    if (ud->cbRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cbRef);
        ud->cbRef = LUA_NOREF;
    }
    return 0;
}

// __gc: GC 时确保 Close (Phase AY T01 Http 同模式: 防 libuv 回调悬空)
static int l_Udp_Gc(lua_State* L) {
    auto* ud = (UdpSocketUserdata*)lua_touserdata(L, 1);
    if (!ud) return 0;
    if (ud->sock) {
        PlatformNet::CloseUdp(ud->sock);
        ud->sock = nullptr;
    }
    if (ud->cbRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cbRef);
        ud->cbRef = LUA_NOREF;
    }
    return 0;
}

// __tostring
static int l_Udp_Tostring(lua_State* L) {
    auto* ud = CheckUdpUd(L, 1);
    lua_pushfstring(L, "Light.Network.Udp.Socket(port=%d, %s)",
                    (int)(ud->sock ? PlatformNet::GetUdpLocalPort(ud->sock) : 0),
                    ud->sock ? "open" : "closed");
    return 1;
}

// ==================== 元表注册 ====================

static void RegisterUdpSocketMt(lua_State* L) {
    luaL_newmetatable(L, UDP_SOCKET_MT);

    static const luaL_Reg methods[] = {
        { "Send",         l_Udp_Send         },
        { "OnReceive",    l_Udp_OnReceive    },
        { "GetLocalPort", l_Udp_GetLocalPort },
        { "Close",        l_Udp_Close        },
        { nullptr, nullptr },
    };

    // __index 指向自身, 让方法可直接通过 sock:Method() 调用
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    luaL_setfuncs(L, methods, 0);

    lua_pushcfunction(L, l_Udp_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_Udp_Tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);  // pop metatable
}

// ==================== luaopen_Light_Network_Udp ====================

// 前向引用: light_network.cpp 中 Network 父表注册函数
extern "C" int luaopen_Light_Network(lua_State* L);

extern "C" LIGHT_API int luaopen_Light_Network_Udp(lua_State* L) {
    RegisterUdpSocketMt(L);

    // 确保 Light.Network 父表 (与 Phase AT Http/HttpServer 同模式)
    luaopen_Light_Network(L);

    lua_pushstring(L, "Udp");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        // 还未创建 Udp 子表
        lua_settop(L, -2);
        lua_pushstring(L, "Udp");
        lua_createtable(L, 0, 0);

        const luaL_Reg udp_funcs[] = {
            { "Open", l_Udp_Open },
            { nullptr, nullptr   },
        };
        luaL_setfuncs(L, udp_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Udp");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);   // 删 Network 父表, 留 Udp 子表
    return 1;
}
