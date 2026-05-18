/**
 * @file light_network_room.cpp
 * @brief Phase BC T8 — Light.Network.Room Lua 模块
 *
 * 房间模型 (room) 抽象 — 一个 server 端权威的小规模实时多人会话:
 *   - Server (Room.Host) 维护 state 表 + peer 列表
 *   - Client (Room.Join) 同步 state, 发送 input, 接收 event broadcast
 *
 * 包类型 (NetProto):
 *   PKT_ROOM_HELLO       6  C→S  {name, meta}            join 请求
 *   PKT_ROOM_KICK        7  S→C  {reason}                踢人 / 拒绝 join
 *   PKT_ROOM_STATE       3  S→C  {rev, data}             全量 state
 *   PKT_ROOM_STATE_PATCH 8  S→C  {rev, set, delete}      增量 state (Phase BC v2)
 *   PKT_ROOM_EVENT       4  S→C  {name, args}            广播事件
 *   PKT_ROOM_INPUT       5  C→S  {kind, data}            用户输入
 *
 * Channel 分配 (DESIGN §3.4):
 *   0  reliable ordered   — HELLO, STATE 全量, KICK
 *   1  unreliable seq     — EVENT, INPUT (低延迟优先, 丢失可接受)
 *
 * Lua API:
 *   local Room = require('Light.Network.Room')
 *
 *   -- server
 *   local room = Room.Host('0.0.0.0', 9000, 32)
 *   room:OnJoin(function(peer_id, hello)
 *       -- hello = {name, meta}
 *       return true                 -- accept
 *       -- or: return false, 'reason'
 *   end)
 *   room:OnLeave(function(peer_id) end)
 *   room:OnInput(function(peer_id, kind, data) end)
 *   room:SetState(state_table)       -- 全量替换, 自动 bump rev + broadcast
 *   room:Broadcast(name, args)       -- channel 1 unreliable seq
 *   room:Kick(peer_id, reason)
 *   room:Close()
 *
 *   -- client
 *   local c = Room.Join('127.0.0.1', 9000, {name='alice', meta={...}})
 *   c:OnReady(function() end)
 *   c:OnState(function(state, rev) end)
 *   c:OnEvent(function(name, args) end)
 *   c:OnKick(function(reason) end)
 *   c:SendInput(kind, data)
 *   c:Leave()
 */

#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7.2 — 类型安全 helpers + magic
#include "light_platform_net.h"
#include "light_network_packet.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

// ==================== 元表名 ====================

static const char* ROOM_HOST_MT   = "Light.Network.Room.Host";
static const char* ROOM_CLIENT_MT = "Light.Network.Room.Client";

// ==================== JSON ↔ Lua (从 T7 借用思路, 这里独立实现) ====================

static void PushCJsonAsLua(lua_State* L, cJSON* node);
static cJSON* PushLuaAsCJson(lua_State* L, int idx);

static void PushCJsonAsLua(lua_State* L, cJSON* node) {
    if (!node) { lua_pushnil(L); return; }
    if (cJSON_IsNull(node))    { lua_pushnil(L); return; }
    if (cJSON_IsBool(node))    { lua_pushboolean(L, cJSON_IsTrue(node)); return; }
    if (cJSON_IsNumber(node))  { lua_pushnumber(L, node->valuedouble); return; }
    if (cJSON_IsString(node))  { lua_pushstring(L, node->valuestring ? node->valuestring : ""); return; }
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

static cJSON* PushLuaAsCJson(lua_State* L, int idx) {
    int absIdx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
    switch (lua_type(L, absIdx)) {
        case LUA_TNIL:      return cJSON_CreateNull();
        case LUA_TBOOLEAN:  return cJSON_CreateBool(lua_toboolean(L, absIdx) != 0);
        case LUA_TNUMBER:   return cJSON_CreateNumber(lua_tonumber(L, absIdx));
        case LUA_TSTRING:   return cJSON_CreateString(lua_tostring(L, absIdx));
        case LUA_TTABLE: {
            int len = (int)lua_objlen(L, absIdx);     // Lua 5.1 (lumen) API
            bool isArray = (len > 0);
            if (isArray) {
                lua_pushnil(L);
                while (lua_next(L, absIdx) != 0) {
                    if (lua_type(L, -2) != LUA_TNUMBER) {
                        isArray = false;
                        lua_pop(L, 2);
                        break;
                    }
                    lua_pop(L, 1);
                }
            }
            if (isArray) {
                cJSON* arr = cJSON_CreateArray();
                for (int i = 1; i <= len; ++i) {
                    lua_rawgeti(L, absIdx, i);
                    cJSON* item = PushLuaAsCJson(L, -1);
                    lua_pop(L, 1);
                    if (item) cJSON_AddItemToArray(arr, item);
                }
                return arr;
            }
            cJSON* obj = cJSON_CreateObject();
            lua_pushnil(L);
            while (lua_next(L, absIdx) != 0) {
                lua_pushvalue(L, -2);
                const char* key = lua_tostring(L, -1);
                cJSON* val = PushLuaAsCJson(L, -2);
                if (key && val) cJSON_AddItemToObject(obj, key, val);
                else if (val) cJSON_Delete(val);
                lua_pop(L, 2);
            }
            return obj;
        }
        default: return cJSON_CreateNull();
    }
}

// ==================== userdata ====================

/// Phase G.1.7.2: 首字段 magic 防止 type-confusion (host 与 client 共 magic, Check 函数靠 MT 区分)
struct RoomHost {
    uint32_t  magic;            // = LT_MAGIC_NET_ROOM
    PlatformNet::EnetHost* host;
    int       onJoinRef;       // function(peer_id, hello) -> bool[, reason]
    int       onLeaveRef;      // function(peer_id)
    int       onInputRef;      // function(peer_id, kind, data)
    int       stateRef;        // Lua table registry-ref (server state snapshot)
    uint32_t  stateRev;        // 单调递增 revision
    lua_State* L;
};

/// Phase G.1.7.2: 首字段 magic 防止 type-confusion
struct RoomClient {
    uint32_t  magic;            // = LT_MAGIC_NET_ROOM
    PlatformNet::EnetHost* host;
    PlatformNet::EnetPeer* peer;
    int       onReadyRef;      // function()
    int       onStateRef;      // function(state, rev)
    int       onEventRef;      // function(name, args)
    int       onKickRef;       // function(reason)
    int       helloRef;        // 缓存的 hello table registry-ref (重连用)
    int       stateCacheRef;   // 客户端本地 state 副本 (registry ref). 收 STATE 全量替换,
                               // 收 STATE_PATCH 增量应用. OnState cb 拿到的是这个 cache.
    bool      ready;
    lua_State* L;
};

/// Phase G.1.7.2: magic 双保险
static RoomHost* CheckHost(lua_State* L, int idx) {
    auto* h = (RoomHost*)luaL_checkudata(L, idx, ROOM_HOST_MT);
    if (h && h->magic != LT::LT_MAGIC_NET_ROOM) luaL_error(L, "Light.Network.Room.Host: type confusion at arg #%d", idx);
    return h;
}
static RoomClient* CheckClient(lua_State* L, int idx) {
    auto* c = (RoomClient*)luaL_checkudata(L, idx, ROOM_CLIENT_MT);
    if (c && c->magic != LT::LT_MAGIC_NET_ROOM) luaL_error(L, "Light.Network.Room.Client: type confusion at arg #%d", idx);
    return c;
}

// ==================== 共用 ====================

static bool SendPacket(PlatformNet::EnetPeer* peer, NetProto::PacketType type,
                       cJSON* root, int channel, bool reliable) {
    if (!peer || !root) return false;
    char* serialized = cJSON_PrintUnformatted(root);
    if (!serialized) return false;
    std::string pkt = NetProto::Pack(type, serialized, std::strlen(serialized));
    cJSON_free(serialized);
    if (pkt.empty()) return false;
    return PlatformNet::EnetSend(peer, channel, pkt.data(), (int)pkt.size(), reliable);
}

static void CallLuaCbWithArgs(lua_State* L, int cbRef, int nargs) {
    if (cbRef == LUA_NOREF || cbRef == LUA_REFNIL) {
        lua_pop(L, nargs);
        return;
    }
    int base = lua_gettop(L) - nargs;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1 + nargs);
        return;
    }
    lua_insert(L, base + 1);
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Light.Network.Room callback error: %s", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

// ==================== Host (server) 事件 ====================

// 把 host 当前 state 序列化并发给 1 个 peer (全量同步)
static void SendStateToPeer(RoomHost* h, PlatformNet::EnetPeer* peer) {
    if (!h || !peer || h->stateRef == LUA_NOREF) return;
    lua_State* L = h->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->stateRef);
    cJSON* dataNode = PushLuaAsCJson(L, -1);
    lua_pop(L, 1);

    NetProto::JsonScope obj(cJSON_CreateObject());
    cJSON_AddNumberToObject(obj.get(), "rev", (double)h->stateRev);
    cJSON_AddItemToObject(obj.get(), "data", dataNode ? dataNode : cJSON_CreateNull());
    SendPacket(peer, NetProto::PKT_ROOM_STATE, obj.get(), /*ch=*/0, /*reliable=*/true);
}

// 广播 state 到所有连接的 peer (在 SetState 后调用)
//
// ENet 没有直接的 broadcast peer-iteration API, 但 enet_host_broadcast
// 走全部 peer. 这里通过 PlatformNet::EnetHost 暴露的是封装, 我们没暴露
// broadcast — 简化做法: 复用 SendStateToPeer + peer 列表跟踪.
// 实际我们让 ENet 自身的连接广播能力工作: 在 OnConnect 处的 peer 记录.
// Phase BC v1 简化: 在 EnetHost 内部维护 peer list, 全量同步时遍历.
//
// 由于 PlatformNet API 不直接暴露 peer iteration, 我们让 server side 收到
// SetState 后用 EnetSend 给每个已知 peer. peer 列表通过事件回调动态维护.

static void HostHandleConnect(RoomHost* h, PlatformNet::EnetPeer* peer) {
    // 等待 HELLO; 暂不主动 reply
    (void)h; (void)peer;
}

static void HostHandleHello(RoomHost* h, PlatformNet::EnetPeer* peer, cJSON* body) {
    lua_State* L = h->L;
    uint32_t peerId = PlatformNet::EnetPeerID(peer);

    // 调 OnJoin(peer_id, hello_table) -> ok[, reason]
    bool accept = true;
    std::string reason;

    if (h->onJoinRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->onJoinRef);
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, (lua_Integer)peerId);
            PushCJsonAsLua(L, body);
            if (lua_pcall(L, 2, 2, 0) == 0) {
                // ret1 = bool, ret2 = string (reason)
                if (lua_isboolean(L, -2)) accept = lua_toboolean(L, -2) != 0;
                if (!accept && lua_isstring(L, -1)) reason = lua_tostring(L, -1);
                lua_pop(L, 2);
            } else {
                const char* err = lua_tostring(L, -1);
                CC::Log(CC::LOG_WARN, "Light.Network.Room OnJoin error: %s",
                        err ? err : "(unknown)");
                lua_pop(L, 1);
                accept = false;
                reason = "OnJoin error";
            }
        } else {
            lua_pop(L, 1);
        }
    }

    if (!accept) {
        // 发 KICK + 主动 disconnect
        NetProto::JsonScope obj(cJSON_CreateObject());
        cJSON_AddStringToObject(obj.get(), "reason",
                                reason.empty() ? "rejected" : reason.c_str());
        SendPacket(peer, NetProto::PKT_ROOM_KICK, obj.get(), 0, true);
        PlatformNet::EnetDisconnect(peer, 0);
        return;
    }
    // 接受 → 立即发当前 state 全量
    SendStateToPeer(h, peer);
}

static void HostHandleInput(RoomHost* h, PlatformNet::EnetPeer* peer, cJSON* body) {
    if (h->onInputRef == LUA_NOREF) return;
    lua_State* L = h->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->onInputRef);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }

    cJSON* kindNode = cJSON_GetObjectItem(body, "kind");
    cJSON* dataNode = cJSON_GetObjectItem(body, "data");

    lua_pushinteger(L, (lua_Integer)PlatformNet::EnetPeerID(peer));
    lua_pushstring(L, cJSON_IsString(kindNode) ? kindNode->valuestring : "");
    PushCJsonAsLua(L, dataNode);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Light.Network.Room OnInput error: %s", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

static void HostHandleDisconnect(RoomHost* h, PlatformNet::EnetPeer* peer) {
    if (h->onLeaveRef == LUA_NOREF) return;
    lua_State* L = h->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->onLeaveRef);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushinteger(L, (lua_Integer)PlatformNet::EnetPeerID(peer));
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Light.Network.Room OnLeave error: %s", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

static void HostHandleEvent(RoomHost* h, const PlatformNet::EnetEvent& ev) {
    using ET = PlatformNet::EnetEventType;
    switch (ev.type) {
        case ET::CONNECT:
            HostHandleConnect(h, ev.peer);
            break;
        case ET::DISCONNECT:
            HostHandleDisconnect(h, ev.peer);
            break;
        case ET::RECEIVE: {
            NetProto::PacketType ptype;
            std::string json;
            if (!NetProto::Unpack(ev.data, (size_t)ev.len, ptype, json)) break;
            NetProto::JsonScope body(cJSON_Parse(json.c_str()));
            if (!body) break;
            if (ptype == NetProto::PKT_ROOM_HELLO) {
                HostHandleHello(h, ev.peer, body.get());
            } else if (ptype == NetProto::PKT_ROOM_INPUT) {
                HostHandleInput(h, ev.peer, body.get());
            }
            // 其他 type 静默丢
            break;
        }
        default: break;
    }
}

// ==================== Client 事件 ====================

static void ClientSendHello(RoomClient* c) {
    if (!c->peer || c->helloRef == LUA_NOREF) {
        // 无 hello data, 发空 HELLO
        NetProto::JsonScope obj(cJSON_CreateObject());
        SendPacket(c->peer, NetProto::PKT_ROOM_HELLO, obj.get(), 0, true);
        return;
    }
    lua_rawgeti(c->L, LUA_REGISTRYINDEX, c->helloRef);
    cJSON* body = PushLuaAsCJson(c->L, -1);
    lua_pop(c->L, 1);
    NetProto::JsonScope obj(body ? body : cJSON_CreateObject());
    SendPacket(c->peer, NetProto::PKT_ROOM_HELLO, obj.get(), 0, true);
}

static void ClientHandleEvent(RoomClient* c, const PlatformNet::EnetEvent& ev) {
    using ET = PlatformNet::EnetEventType;
    switch (ev.type) {
        case ET::CONNECT:
            ClientSendHello(c);
            break;
        case ET::DISCONNECT:
            c->peer = nullptr;
            c->ready = false;
            if (c->onKickRef != LUA_NOREF) {
                lua_State* L = c->L;
                lua_rawgeti(L, LUA_REGISTRYINDEX, c->onKickRef);
                if (lua_isfunction(L, -1)) {
                    lua_pushstring(L, "disconnected");
                    if (lua_pcall(L, 1, 0, 0) != 0) {
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
            }
            break;
        case ET::RECEIVE: {
            NetProto::PacketType ptype;
            std::string json;
            if (!NetProto::Unpack(ev.data, (size_t)ev.len, ptype, json)) break;
            NetProto::JsonScope body(cJSON_Parse(json.c_str()));
            if (!body) break;

            lua_State* L = c->L;
            if (ptype == NetProto::PKT_ROOM_STATE) {
                // 首次 STATE 视为 ready
                if (!c->ready) {
                    c->ready = true;
                    if (c->onReadyRef != LUA_NOREF) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, c->onReadyRef);
                        if (lua_isfunction(L, -1)) {
                            if (lua_pcall(L, 0, 0, 0) != 0) lua_pop(L, 1);
                        } else { lua_pop(L, 1); }
                    }
                }
                // 全量 state: 替换本地 cache
                cJSON* rev  = cJSON_GetObjectItem(body.get(), "rev");
                cJSON* data = cJSON_GetObjectItem(body.get(), "data");
                if (c->stateCacheRef != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, c->stateCacheRef);
                }
                PushCJsonAsLua(L, data);                       // [..., new_state]
                if (!lua_istable(L, -1)) {
                    // 服务端 data 不是 table (理论上罕见), 用空 table 代替
                    lua_pop(L, 1);
                    lua_newtable(L);
                }
                c->stateCacheRef = luaL_ref(L, LUA_REGISTRYINDEX);
                // 调 OnState(stateCache, rev)
                if (c->onStateRef != LUA_NOREF) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, c->onStateRef);
                    if (lua_isfunction(L, -1)) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, c->stateCacheRef);
                        lua_pushnumber(L, cJSON_IsNumber(rev) ? rev->valuedouble : 0);
                        if (lua_pcall(L, 2, 0, 0) != 0) lua_pop(L, 1);
                    } else { lua_pop(L, 1); }
                }
            } else if (ptype == NetProto::PKT_ROOM_STATE_PATCH) {
                // 增量 patch: 应用 set + delete 到 stateCacheRef
                if (c->stateCacheRef == LUA_NOREF) {
                    // 尚未收到全量 STATE, 不应用 patch (避免误差)
                    break;
                }
                cJSON* rev    = cJSON_GetObjectItem(body.get(), "rev");
                cJSON* setNd  = cJSON_GetObjectItem(body.get(), "set");
                cJSON* delNd  = cJSON_GetObjectItem(body.get(), "delete");

                lua_rawgeti(L, LUA_REGISTRYINDEX, c->stateCacheRef);   // [..., cache]
                int cacheIdx = lua_gettop(L);

                // apply set: cache[k] = v
                if (cJSON_IsObject(setNd)) {
                    for (cJSON* item = setNd->child; item; item = item->next) {
                        if (!item->string) continue;
                        lua_pushstring(L, item->string);
                        PushCJsonAsLua(L, item);
                        lua_rawset(L, cacheIdx);
                    }
                }
                // apply delete: cache[k] = nil
                if (cJSON_IsArray(delNd)) {
                    int n = cJSON_GetArraySize(delNd);
                    for (int i = 0; i < n; ++i) {
                        cJSON* k = cJSON_GetArrayItem(delNd, i);
                        if (cJSON_IsString(k) && k->valuestring) {
                            lua_pushstring(L, k->valuestring);
                            lua_pushnil(L);
                            lua_rawset(L, cacheIdx);
                        }
                    }
                }
                lua_pop(L, 1);                                          // pop cache

                // 调 OnState(stateCache, rev)
                if (c->onStateRef != LUA_NOREF) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, c->onStateRef);
                    if (lua_isfunction(L, -1)) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, c->stateCacheRef);
                        lua_pushnumber(L, cJSON_IsNumber(rev) ? rev->valuedouble : 0);
                        if (lua_pcall(L, 2, 0, 0) != 0) lua_pop(L, 1);
                    } else { lua_pop(L, 1); }
                }
            } else if (ptype == NetProto::PKT_ROOM_EVENT) {
                if (c->onEventRef != LUA_NOREF) {
                    cJSON* name = cJSON_GetObjectItem(body.get(), "name");
                    cJSON* args = cJSON_GetObjectItem(body.get(), "args");
                    lua_rawgeti(L, LUA_REGISTRYINDEX, c->onEventRef);
                    if (lua_isfunction(L, -1)) {
                        lua_pushstring(L, cJSON_IsString(name) ? name->valuestring : "");
                        PushCJsonAsLua(L, args);
                        if (lua_pcall(L, 2, 0, 0) != 0) lua_pop(L, 1);
                    } else { lua_pop(L, 1); }
                }
            } else if (ptype == NetProto::PKT_ROOM_KICK) {
                cJSON* reason = cJSON_GetObjectItem(body.get(), "reason");
                if (c->onKickRef != LUA_NOREF) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, c->onKickRef);
                    if (lua_isfunction(L, -1)) {
                        lua_pushstring(L, cJSON_IsString(reason) ? reason->valuestring : "kicked");
                        if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
                    } else { lua_pop(L, 1); }
                }
                // 自动 disconnect
                if (c->peer) {
                    PlatformNet::EnetDisconnect(c->peer, 0);
                }
            }
            break;
        }
        default: break;
    }
}

// ==================== Host Lua API ====================

// Room.Host(ip, port, maxPeers=32) -> host | nil, err
static int l_Room_Host(lua_State* L) {
    const char* ip = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int maxPeers = (int)luaL_optinteger(L, 3, 32);
    if (port <= 0 || port > 65535) {
        lua_pushnil(L); lua_pushstring(L, "port out of range"); return 2;
    }
    if (maxPeers <= 0 || maxPeers > 4096) maxPeers = 32;

    auto* eh = PlatformNet::EnetCreateHost(ip, (uint16_t)port, maxPeers, 2);
    if (!eh) {
        lua_pushnil(L);
        lua_pushfstring(L, "EnetCreateHost failed (port=%d)", port);
        return 2;
    }

    auto* h = (RoomHost*)lua_newuserdata(L, sizeof(RoomHost));
    h->magic      = LT::LT_MAGIC_NET_ROOM;  // Phase G.1.7.2 — type tag
    h->host       = eh;
    h->onJoinRef  = LUA_NOREF;
    h->onLeaveRef = LUA_NOREF;
    h->onInputRef = LUA_NOREF;
    h->stateRev   = 0;
    h->L          = L;
    // 初始 state = 空 table
    lua_newtable(L);
    h->stateRef = luaL_ref(L, LUA_REGISTRYINDEX);

    luaL_getmetatable(L, ROOM_HOST_MT);
    lua_setmetatable(L, -2);

    PlatformNet::EnetSetEventCb(eh,
        [h](const PlatformNet::EnetEvent& ev) { HostHandleEvent(h, ev); });
    return 1;
}

// host:OnJoin(cb), OnLeave, OnInput — 三个 setter 模式相同, 用宏简化
#define ROOM_HOST_ON_SETTER(field, lname)                              \
    static int l_RoomHost_##field(lua_State* L) {                      \
        auto* h = CheckHost(L, 1);                                     \
        if (h->field##Ref != LUA_NOREF) {                              \
            luaL_unref(L, LUA_REGISTRYINDEX, h->field##Ref);           \
            h->field##Ref = LUA_NOREF;                                 \
        }                                                              \
        if (lua_isfunction(L, 2)) {                                    \
            lua_pushvalue(L, 2);                                       \
            h->field##Ref = luaL_ref(L, LUA_REGISTRYINDEX);            \
        }                                                              \
        return 0;                                                      \
    }

ROOM_HOST_ON_SETTER(onJoin,  "OnJoin")
ROOM_HOST_ON_SETTER(onLeave, "OnLeave")
ROOM_HOST_ON_SETTER(onInput, "OnInput")

// 内部: 把当前 h->state 全量序列化 + 广播到所有已连接 peer
static void BroadcastState(RoomHost* h) {
    if (!h || !h->host || h->stateRef == LUA_NOREF) return;
    lua_State* L = h->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->stateRef);
    cJSON* dataNode = PushLuaAsCJson(L, -1);
    lua_pop(L, 1);

    NetProto::JsonScope obj(cJSON_CreateObject());
    cJSON_AddNumberToObject(obj.get(), "rev", (double)h->stateRev);
    cJSON_AddItemToObject(obj.get(), "data", dataNode ? dataNode : cJSON_CreateNull());

    char* serialized = cJSON_PrintUnformatted(obj.get());
    if (!serialized) return;
    std::string pkt = NetProto::Pack(NetProto::PKT_ROOM_STATE,
                                      serialized, std::strlen(serialized));
    cJSON_free(serialized);
    if (!pkt.empty()) {
        PlatformNet::EnetBroadcast(h->host, /*ch=*/0,
                                    pkt.data(), (int)pkt.size(), /*reliable=*/true);
    }
}

// host:SetState(tbl) — 全量替换, bump rev, 立即 broadcast
static int l_RoomHost_SetState(lua_State* L) {
    auto* h = CheckHost(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    if (h->stateRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, h->stateRef);
    }
    lua_pushvalue(L, 2);
    h->stateRef = luaL_ref(L, LUA_REGISTRYINDEX);
    h->stateRev++;

    BroadcastState(h);
    return 0;
}

// host:PatchState(set_table[, delete_keys_array]) — 增量更新 + 广播 patch
//
// 语义:
//   set_table     {key=value, ...}    顶层 key 直接 set 到 server state (浅替换)
//   delete_keys   {"k1", "k2", ...}   server state[ki] = nil (删除)
//
// 限制 (Phase BC v2 简化):
//   - 仅支持顶层 key, 不做 deep merge. 子表整个替换.
//   - delete_keys 必须是字符串数组. 顶层 key 是其它类型时不能删除.
//   - 实际状态被修改后才 bump rev + broadcast (空 patch 是 no-op)
//
// Wire format (PKT_ROOM_STATE_PATCH):
//   {"rev":N, "set":{...}, "delete":["k1","k2"]}
static int l_RoomHost_PatchState(lua_State* L) {
    auto* h = CheckHost(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    bool hasDelete = lua_istable(L, 3);

    if (h->stateRef == LUA_NOREF || !h->host) {
        return 0;
    }

    // 1. 应用 patch 到 server state
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->stateRef);     // [..., set, del?, state]
    int stateIdx = lua_gettop(L);
    bool changed = false;

    // 1a. set: 遍历 set_table, server_state[k] = v
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        // [..., state, key, value]
        lua_pushvalue(L, -2);                            // dup key
        lua_pushvalue(L, -2);                            // dup value
        lua_rawset(L, stateIdx);                         // state[key] = value
        lua_pop(L, 1);                                   // pop value, keep key
        changed = true;
    }

    // 1b. delete: 遍历 delete_keys 数组, server_state[k] = nil
    if (hasDelete) {
        int n = (int)lua_objlen(L, 3);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 3, i);                        // push key string
            if (lua_isstring(L, -1)) {
                lua_pushvalue(L, -1);                    // dup
                lua_pushnil(L);
                lua_rawset(L, stateIdx);                 // state[key] = nil
                changed = true;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);                                        // pop state

    if (!changed) return 0;
    h->stateRev++;

    // 2. 序列化 patch 包 + 广播
    NetProto::JsonScope obj(cJSON_CreateObject());
    cJSON_AddNumberToObject(obj.get(), "rev", (double)h->stateRev);
    cJSON* setNode = PushLuaAsCJson(L, 2);
    cJSON_AddItemToObject(obj.get(), "set", setNode ? setNode : cJSON_CreateObject());
    if (hasDelete) {
        cJSON* delNode = PushLuaAsCJson(L, 3);
        cJSON_AddItemToObject(obj.get(), "delete",
                              delNode ? delNode : cJSON_CreateArray());
    }

    char* serialized = cJSON_PrintUnformatted(obj.get());
    if (serialized) {
        std::string pkt = NetProto::Pack(NetProto::PKT_ROOM_STATE_PATCH,
                                          serialized, std::strlen(serialized));
        cJSON_free(serialized);
        if (!pkt.empty()) {
            PlatformNet::EnetBroadcast(h->host, /*ch=*/0,
                                        pkt.data(), (int)pkt.size(),
                                        /*reliable=*/true);
        }
    }
    return 0;
}

// host:Broadcast(name, args) — 广播事件 (channel 1 unreliable seq)
static int l_RoomHost_Broadcast(lua_State* L) {
    auto* h = CheckHost(L, 1);
    if (!h->host) { lua_pushboolean(L, 0); return 1; }
    const char* name = luaL_checkstring(L, 2);

    NetProto::JsonScope body(cJSON_CreateObject());
    cJSON_AddStringToObject(body.get(), "name", name);
    cJSON* args = PushLuaAsCJson(L, 3);
    cJSON_AddItemToObject(body.get(), "args", args ? args : cJSON_CreateNull());

    char* serialized = cJSON_PrintUnformatted(body.get());
    if (!serialized) { lua_pushboolean(L, 0); return 1; }
    std::string pkt = NetProto::Pack(NetProto::PKT_ROOM_EVENT,
                                      serialized, std::strlen(serialized));
    cJSON_free(serialized);
    if (pkt.empty()) { lua_pushboolean(L, 0); return 1; }

    int n = PlatformNet::EnetBroadcast(h->host, /*ch=*/1,
                                        pkt.data(), (int)pkt.size(),
                                        /*reliable=*/false);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

// host:Kick(peer_id [, reason]) -> bool
//
// Phase BC v2: 先通过 EnetSendByPeerId 发 PKT_ROOM_KICK {reason}, 再 disconnect.
// 客户端 OnKick(reason) 会拿到友好原因, 然后 ENet 自动推送 DISCONNECT 事件.
//
// 返回值: disconnect 是否成功发出 (即 peer 是否存在且未已断). KICK 包发送失败
// 不影响 disconnect — peer 不存在时整个调用返回 false.
static int l_RoomHost_Kick(lua_State* L) {
    auto* h = CheckHost(L, 1);
    if (!h->host) { lua_pushboolean(L, 0); return 1; }
    uint32_t peerId = (uint32_t)luaL_checkinteger(L, 2);
    const char* reason = luaL_optstring(L, 3, "kicked");

    // 1. 尝试发 KICK 包 (channel 0 reliable). 失败也继续 disconnect.
    NetProto::JsonScope body(cJSON_CreateObject());
    cJSON_AddStringToObject(body.get(), "reason", reason);
    char* serialized = cJSON_PrintUnformatted(body.get());
    if (serialized) {
        std::string pkt = NetProto::Pack(NetProto::PKT_ROOM_KICK,
                                          serialized, std::strlen(serialized));
        cJSON_free(serialized);
        if (!pkt.empty()) {
            PlatformNet::EnetSendByPeerId(h->host, peerId, /*ch=*/0,
                                           pkt.data(), (int)pkt.size(),
                                           /*reliable=*/true);
        }
    }

    // 2. disconnect (即使 KICK 包发送失败也尝试)
    bool ok = PlatformNet::EnetDisconnectPeerById(h->host, peerId, 0);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int l_RoomHost_Close(lua_State* L) {
    auto* h = CheckHost(L, 1);
    if (h->host) {
        PlatformNet::EnetDestroyHost(h->host);
        h->host = nullptr;
    }
    auto unref = [&](int& r) {
        if (r != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, r); r = LUA_NOREF; }
    };
    unref(h->onJoinRef); unref(h->onLeaveRef); unref(h->onInputRef); unref(h->stateRef);
    return 0;
}

static int l_RoomHost_Gc(lua_State* L) { return l_RoomHost_Close(L); }

static int l_RoomHost_Tostring(lua_State* L) {
    auto* h = CheckHost(L, 1);
    lua_pushfstring(L, "Light.Network.Room.Host(%s, rev=%d)",
                    h->host ? "open" : "closed", (int)h->stateRev);
    return 1;
}

// ==================== Client Lua API ====================

// Room.Join(host, port, hello?) -> client | nil, err
static int l_Room_Join(lua_State* L) {
    const char* host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    // arg 3 = hello table (可选)
    if (port <= 0 || port > 65535) {
        lua_pushnil(L); lua_pushstring(L, "port out of range"); return 2;
    }

    auto* eh = PlatformNet::EnetCreateHost(nullptr, 0, 1, 2);
    if (!eh) {
        lua_pushnil(L); lua_pushstring(L, "EnetCreateHost failed"); return 2;
    }
    auto* peer = PlatformNet::EnetConnect(eh, host, (uint16_t)port, 2);
    if (!peer) {
        PlatformNet::EnetDestroyHost(eh);
        lua_pushnil(L);
        lua_pushfstring(L, "EnetConnect failed (%s:%d)", host, port);
        return 2;
    }

    auto* c = (RoomClient*)lua_newuserdata(L, sizeof(RoomClient));
    c->magic         = LT::LT_MAGIC_NET_ROOM;  // Phase G.1.7.2 — type tag
    c->host          = eh;
    c->peer          = peer;
    c->onReadyRef    = LUA_NOREF;
    c->onStateRef    = LUA_NOREF;
    c->onEventRef    = LUA_NOREF;
    c->onKickRef     = LUA_NOREF;
    c->helloRef      = LUA_NOREF;
    c->stateCacheRef = LUA_NOREF;       // 首次 PKT_ROOM_STATE 时实际分配
    c->ready      = false;
    c->L          = L;

    if (lua_istable(L, 3)) {
        lua_pushvalue(L, 3);
        c->helloRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    luaL_getmetatable(L, ROOM_CLIENT_MT);
    lua_setmetatable(L, -2);

    PlatformNet::EnetSetEventCb(eh,
        [c](const PlatformNet::EnetEvent& ev) { ClientHandleEvent(c, ev); });
    return 1;
}

#define ROOM_CLIENT_ON_SETTER(field)                                   \
    static int l_RoomClient_##field(lua_State* L) {                    \
        auto* c = CheckClient(L, 1);                                   \
        if (c->field##Ref != LUA_NOREF) {                              \
            luaL_unref(L, LUA_REGISTRYINDEX, c->field##Ref);           \
            c->field##Ref = LUA_NOREF;                                 \
        }                                                              \
        if (lua_isfunction(L, 2)) {                                    \
            lua_pushvalue(L, 2);                                       \
            c->field##Ref = luaL_ref(L, LUA_REGISTRYINDEX);            \
        }                                                              \
        return 0;                                                      \
    }

ROOM_CLIENT_ON_SETTER(onReady)
ROOM_CLIENT_ON_SETTER(onState)
ROOM_CLIENT_ON_SETTER(onEvent)
ROOM_CLIENT_ON_SETTER(onKick)

// client:SendInput(kind, data)
static int l_RoomClient_SendInput(lua_State* L) {
    auto* c = CheckClient(L, 1);
    if (!c->peer) { lua_pushboolean(L, 0); return 1; }
    const char* kind = luaL_checkstring(L, 2);
    NetProto::JsonScope body(cJSON_CreateObject());
    cJSON_AddStringToObject(body.get(), "kind", kind);
    cJSON* data = PushLuaAsCJson(L, 3);
    cJSON_AddItemToObject(body.get(), "data", data ? data : cJSON_CreateNull());

    bool ok = SendPacket(c->peer, NetProto::PKT_ROOM_INPUT, body.get(),
                          /*ch=*/1, /*reliable=*/false);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// client:Leave()
static int l_RoomClient_Leave(lua_State* L) {
    auto* c = CheckClient(L, 1);
    if (c->peer) {
        PlatformNet::EnetDisconnect(c->peer, 0);
        c->peer = nullptr;
    }
    if (c->host) {
        PlatformNet::EnetDestroyHost(c->host);
        c->host = nullptr;
    }
    auto unref = [&](int& r) {
        if (r != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, r); r = LUA_NOREF; }
    };
    unref(c->onReadyRef); unref(c->onStateRef); unref(c->onEventRef);
    unref(c->onKickRef); unref(c->helloRef); unref(c->stateCacheRef);
    return 0;
}

static int l_RoomClient_Gc(lua_State* L) { return l_RoomClient_Leave(L); }

static int l_RoomClient_Tostring(lua_State* L) {
    auto* c = CheckClient(L, 1);
    lua_pushfstring(L, "Light.Network.Room.Client(%s)",
                    c->peer ? (c->ready ? "ready" : "joining") : "closed");
    return 1;
}

// ==================== 元表注册 ====================

static void RegisterHostMt(lua_State* L) {
    luaL_newmetatable(L, ROOM_HOST_MT);
    static const luaL_Reg methods[] = {
        { "OnJoin",     l_RoomHost_onJoin     },
        { "OnLeave",    l_RoomHost_onLeave    },
        { "OnInput",    l_RoomHost_onInput    },
        { "SetState",   l_RoomHost_SetState   },
        { "PatchState", l_RoomHost_PatchState },
        { "Broadcast",  l_RoomHost_Broadcast  },
        { "Kick",       l_RoomHost_Kick       },
        { "Close",      l_RoomHost_Close      },
        { nullptr, nullptr },
    };
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pushcfunction(L, l_RoomHost_Gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_RoomHost_Tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

static void RegisterClientMt(lua_State* L) {
    luaL_newmetatable(L, ROOM_CLIENT_MT);
    static const luaL_Reg methods[] = {
        { "OnReady",   l_RoomClient_onReady   },
        { "OnState",   l_RoomClient_onState   },
        { "OnEvent",   l_RoomClient_onEvent   },
        { "OnKick",    l_RoomClient_onKick    },
        { "SendInput", l_RoomClient_SendInput },
        { "Leave",     l_RoomClient_Leave     },
        { nullptr, nullptr },
    };
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pushcfunction(L, l_RoomClient_Gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_RoomClient_Tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

// ==================== luaopen_Light_Network_Room ====================

extern "C" int luaopen_Light_Network(lua_State* L);

extern "C" LIGHT_API int luaopen_Light_Network_Room(lua_State* L) {
    RegisterHostMt(L);
    RegisterClientMt(L);

    luaopen_Light_Network(L);

    lua_pushstring(L, "Room");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Room");
        lua_createtable(L, 0, 0);

        const luaL_Reg fns[] = {
            { "Host", l_Room_Host },
            { "Join", l_Room_Join },
            { nullptr, nullptr   },
        };
        luaL_setfuncs(L, fns, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Room");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
