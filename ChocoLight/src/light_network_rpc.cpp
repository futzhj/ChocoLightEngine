/**
 * @file light_network_rpc.cpp
 * @brief Phase BC T7 — Light.Network.Rpc Lua 模块 (JSON-RPC 2.0 over ENet)
 *
 * 提供 client / server 两端的远程过程调用. 协议格式遵循 JSON-RPC 2.0
 * (子集), 用 NetProto::Pack 封装为 ENet packet, 走 channel 0 (reliable).
 *
 * Lua API:
 *   local Rpc = require("Light.Network.Rpc")
 *
 *   -- 客户端
 *   local c = Rpc.Connect("127.0.0.1", 9000)
 *   c:Call("Add", {a=1, b=2}, function(err, result)
 *       if err then print(err.code, err.message)
 *       else print("result =", result) end
 *   end)
 *   c:Notify("log", "hello")            -- 不要响应, 一去无回
 *   c:OnEvent(function(evt) ... end)    -- evt.type ∈ {connect,disconnect,error}
 *   c:Close()
 *
 *   -- 服务端
 *   local s = Rpc.Listen("0.0.0.0", 9000, 32)   -- maxPeers
 *   s:RegisterMethod("Add", function(params, peer_id)
 *       return params.a + params.b              -- 返回值即 result
 *       -- 或 return nil, {code=-32602, message="bad params"}
 *   end)
 *   s:OnEvent(function(evt) ... end)            -- evt.type ∈ {peer_connect,peer_disconnect}
 *   s:Close()
 *
 * 协议:
 *   Request:      {"id":N, "method":"<name>", "params":<value>}
 *   Response OK:  {"id":N, "result":<value>}
 *   Response ERR: {"id":N, "error":{"code":int, "message":str}}
 *   Notification: {"method":"<name>", "params":<value>}   (无 id)
 *
 * 错误码 (JSON-RPC 2.0 标准):
 *   -32700 Parse error
 *   -32600 Invalid Request
 *   -32601 Method not found
 *   -32602 Invalid params
 *   -32603 Internal error
 *   网络层错误码 (扩展):
 *   -32000 Disconnected (call pending 时连接断开)
 *   -32001 Timeout      (预留, 当前未实现超时)
 */

#include "light.h"
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
#include <vector>

// ==================== 元表名 ====================

static const char* RPC_CLIENT_MT = "Light.Network.Rpc.Client";
static const char* RPC_SERVER_MT = "Light.Network.Rpc.Server";

// ==================== 错误码常量 ====================

namespace RpcErr {
    constexpr int PARSE_ERROR      = -32700;
    constexpr int INVALID_REQUEST  = -32600;
    constexpr int METHOD_NOT_FOUND = -32601;
    constexpr int INVALID_PARAMS   = -32602;
    constexpr int INTERNAL_ERROR   = -32603;
    constexpr int DISCONNECTED     = -32000;
}

// ==================== userdata ====================

struct RpcClient {
    PlatformNet::EnetHost* host;
    PlatformNet::EnetPeer* peer;
    int       onEventRef;                       // OnEvent cb registry ref
    int       pendingRef;                       // pending {id → cb_ref} table, registry ref
    uint32_t  nextId;                           // 单调递增 request id
    bool      connected;                        // CONNECT 事件后置 true
    lua_State* L;
};

struct RpcServer {
    PlatformNet::EnetHost* host;
    int       methodsRef;                       // {name → handler_fn} table, registry ref
    int       onEventRef;                       // OnEvent cb registry ref
    lua_State* L;
};

static RpcClient* CheckClient(lua_State* L, int idx) {
    return (RpcClient*)luaL_checkudata(L, idx, RPC_CLIENT_MT);
}
static RpcServer* CheckServer(lua_State* L, int idx) {
    return (RpcServer*)luaL_checkudata(L, idx, RPC_SERVER_MT);
}

// ==================== JSON ↔ Lua 转换 ====================

// 前向声明
static void PushCJsonAsLua(lua_State* L, cJSON* node);
static cJSON* PushLuaAsCJson(lua_State* L, int idx);

// cJSON 树 → Lua 值 (push 到栈顶)
static void PushCJsonAsLua(lua_State* L, cJSON* node) {
    if (!node) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsNull(node)) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(node)) {
        lua_pushboolean(L, cJSON_IsTrue(node) ? 1 : 0);
    } else if (cJSON_IsNumber(node)) {
        // 整数优先 (避免 float 误差)
        double v = node->valuedouble;
        lua_Integer iv = (lua_Integer)v;
        if ((double)iv == v) lua_pushinteger(L, iv);
        else                 lua_pushnumber(L, (lua_Number)v);
    } else if (cJSON_IsString(node)) {
        lua_pushstring(L, node->valuestring ? node->valuestring : "");
    } else if (cJSON_IsArray(node)) {
        int n = cJSON_GetArraySize(node);
        lua_createtable(L, n, 0);
        for (int i = 0; i < n; ++i) {
            PushCJsonAsLua(L, cJSON_GetArrayItem(node, i));
            lua_rawseti(L, -2, i + 1);          // Lua 1-based
        }
    } else if (cJSON_IsObject(node)) {
        lua_newtable(L);
        for (cJSON* child = node->child; child; child = child->next) {
            PushCJsonAsLua(L, child);
            lua_setfield(L, -2, child->string ? child->string : "");
        }
    } else {
        lua_pushnil(L);
    }
}

// Lua 值 → 新分配的 cJSON 节点 (调用方负责 cJSON_Delete / 挂到父节点)
//
// 表 → object 还是 array?
//   - 若 #t > 0 且 next(t, #t) == nil → array
//   - 否则 → object
static cJSON* PushLuaAsCJson(lua_State* L, int idx) {
    // 规范化负索引
    int absIdx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
    int t = lua_type(L, absIdx);

    switch (t) {
        case LUA_TNIL:
            return cJSON_CreateNull();
        case LUA_TBOOLEAN:
            return cJSON_CreateBool(lua_toboolean(L, absIdx) != 0);
        case LUA_TNUMBER:
            // lumen 不区分 int/float (Lua 5.1 fork), 统一用 lua_tonumber.
            // cJSON 内部以 double 存, integer 也无损 (≤ 2^53)
            return cJSON_CreateNumber(lua_tonumber(L, absIdx));
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, absIdx, &len);
            // cJSON 不支持嵌入 NUL, 用 CreateStringReference 也一样, 直接用 CreateString
            return cJSON_CreateString(s ? s : "");
        }
        case LUA_TTABLE: {
            // 判 array 还是 object: 用 lua_objlen (Lua 5.1 API) + 遍历检查 1..n 都是 number key
            int len = (int)lua_objlen(L, absIdx);
            bool isArray = (len > 0);
            if (isArray) {
                // 检查每个 1..len 是否都存在 + 是否有非数字 key
                for (int i = 1; i <= len && isArray; ++i) {
                    lua_rawgeti(L, absIdx, i);
                    if (lua_isnil(L, -1)) isArray = false;
                    lua_pop(L, 1);
                }
                if (isArray) {
                    // 还要确认无非数字 key
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
                // key 必须 string-able
                lua_pushvalue(L, -2);
                const char* key = lua_tostring(L, -1);
                cJSON* val = PushLuaAsCJson(L, -2);
                if (key && val) {
                    cJSON_AddItemToObject(obj, key, val);
                } else if (val) {
                    cJSON_Delete(val);
                }
                lua_pop(L, 2);                  // pop key-copy + value, 保留原 key 供 next
            }
            return obj;
        }
        default:
            return cJSON_CreateNull();
    }
}

// ==================== 共用辅助 ====================

// 把 cJSON 序列化成 string 并 Pack + Send 到 peer
//
// 返回 true 表示已交付 ENet 队列 (实际网络可达性由 ENet service 处理)
static bool SendJsonPacket(PlatformNet::EnetPeer* peer, cJSON* root) {
    if (!peer || !root) return false;
    char* serialized = cJSON_PrintUnformatted(root);
    if (!serialized) return false;
    size_t len = std::strlen(serialized);

    // 用 RPC_REQUEST type 涵盖 request/response/notification — Phase BC §3 简化:
    // 所有 RPC 流量都用 PKT_RPC_REQUEST 区分, 由 JSON 内字段决定身份.
    // (T8 Room 用不同 type 区分协议域)
    std::string pkt = NetProto::Pack(NetProto::PKT_RPC_REQUEST, serialized, len);
    cJSON_free(serialized);
    if (pkt.empty()) return false;

    return PlatformNet::EnetSend(peer, /*channel=*/0,
                                  pkt.data(), (int)pkt.size(), /*reliable=*/true);
}

// 在 obj 上构造 error 子对象: {"code": code, "message": msg}
static void AttachJsonError(cJSON* obj, int code, const char* msg) {
    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", (double)code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "");
    cJSON_AddItemToObject(obj, "error", err);
}

// 调 Lua callback (registry ref), 处理 pcall 错误并 LOG
static void CallLuaCb(lua_State* L, int cbRef, int nargs) {
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
    // 把 nargs 个参数从 base+1..top-1 移到函数下方
    lua_insert(L, base + 1);                    // 插入到 nargs 之前
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Light.Network.Rpc Lua callback error: %s",
                err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

// ==================== 事件分发: Client ====================
// (实现见后续片段)

// 前向声明 (在第二段定义)
static void ClientHandleEvent(RpcClient* c, const PlatformNet::EnetEvent& ev);
static void ServerHandleEvent(RpcServer* s, const PlatformNet::EnetEvent& ev);

// ==================== Client 事件 ====================

// 触发 client 的 OnEvent 回调
//   evt = {type="connect"|"disconnect"|"error", message="..."}
static void FireClientEvent(RpcClient* c, const char* type, const char* msg) {
    if (c->onEventRef == LUA_NOREF) return;
    lua_State* L = c->L;
    lua_createtable(L, 0, 2);
    lua_pushstring(L, type);
    lua_setfield(L, -2, "type");
    if (msg) {
        lua_pushstring(L, msg);
        lua_setfield(L, -2, "message");
    }
    CallLuaCb(L, c->onEventRef, 1);
}

// 处理客户端收到的 RECEIVE 事件 — 解析 RPC response, 按 id 分发到 pending callback
static void ClientHandleReceive(RpcClient* c, const PlatformNet::EnetEvent& ev) {
    if (!ev.data || ev.len <= 0) return;
    NetProto::PacketType ptype;
    std::string json;
    if (!NetProto::Unpack(ev.data, (size_t)ev.len, ptype, json)) return;
    if (ptype != NetProto::PKT_RPC_REQUEST) return;     // 非 RPC 流量丢弃

    NetProto::JsonScope root(cJSON_Parse(json.c_str()));
    if (!root) return;

    cJSON* idNode = cJSON_GetObjectItem(root.get(), "id");
    if (!cJSON_IsNumber(idNode)) {
        // 没 id 的入站包 = server 推过来的事件 (Phase BC §3 简化, 当前不处理)
        return;
    }
    int id = (int)idNode->valuedouble;

    // pending[id] → cb_ref
    lua_State* L = c->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->pendingRef);
    lua_rawgeti(L, -1, id);                     // pending[id]
    if (!lua_isfunction(L, -1)) {
        // 无匹配 callback (可能 timeout 后到达, 或重复 response)
        lua_pop(L, 2);
        return;
    }
    // pending[id] = nil (一次性)
    lua_pushnil(L);
    lua_rawseti(L, -3, id);

    // 准备调用: cb(err, result)
    cJSON* errNode = cJSON_GetObjectItem(root.get(), "error");
    cJSON* okNode  = cJSON_GetObjectItem(root.get(), "result");
    if (errNode && cJSON_IsObject(errNode)) {
        PushCJsonAsLua(L, errNode);              // err
        lua_pushnil(L);                           // result
    } else {
        lua_pushnil(L);                           // err
        PushCJsonAsLua(L, okNode);                // result (可能 nil)
    }
    // 此时栈: pending_table, cb_func, err, result
    // CallLuaCb 需要参数在 cb 后, 这里手工 pcall
    if (lua_pcall(L, 2, 0, 0) != 0) {
        const char* m = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Light.Network.Rpc Client cb error: %s",
                m ? m : "(unknown)");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);                               // pop pending_table
}

static void ClientHandleEvent(RpcClient* c, const PlatformNet::EnetEvent& ev) {
    using ET = PlatformNet::EnetEventType;
    switch (ev.type) {
        case ET::CONNECT:
            c->connected = true;
            FireClientEvent(c, "connect", nullptr);
            break;
        case ET::DISCONNECT: {
            c->connected = false;
            c->peer = nullptr;
            // 把所有 pending callback 用 disconnect error 触发
            if (c->pendingRef != LUA_NOREF) {
                lua_State* L = c->L;
                lua_rawgeti(L, LUA_REGISTRYINDEX, c->pendingRef);
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    if (lua_isfunction(L, -1)) {
                        lua_pushvalue(L, -1);          // dup cb
                        // err = {code=-32000, message="disconnected"}
                        lua_createtable(L, 0, 2);
                        lua_pushinteger(L, RpcErr::DISCONNECTED);
                        lua_setfield(L, -2, "code");
                        lua_pushstring(L, "disconnected");
                        lua_setfield(L, -2, "message");
                        lua_pushnil(L);                 // result
                        if (lua_pcall(L, 2, 0, 0) != 0) {
                            lua_pop(L, 1);
                        }
                    }
                    lua_pop(L, 1);                      // pop value
                }
                // 清空表
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pop(L, 1);
                    lua_pushvalue(L, -1);
                    lua_pushnil(L);
                    lua_rawset(L, -4);
                }
                lua_pop(L, 1);                          // pop pending_table
            }
            FireClientEvent(c, "disconnect", nullptr);
            break;
        }
        case ET::RECEIVE:
            ClientHandleReceive(c, ev);
            break;
        default: break;
    }
}

// ==================== Server 事件 ====================

static void FireServerEvent(RpcServer* s, const char* type, uint32_t peerId) {
    if (s->onEventRef == LUA_NOREF) return;
    lua_State* L = s->L;
    lua_createtable(L, 0, 2);
    lua_pushstring(L, type);
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)peerId);
    lua_setfield(L, -2, "peer_id");
    CallLuaCb(L, s->onEventRef, 1);
}

// 处理服务端收到的 RECEIVE — 解析 RPC request, 调 handler, 回包 response
static void ServerHandleReceive(RpcServer* s, const PlatformNet::EnetEvent& ev) {
    if (!ev.data || ev.len <= 0 || !ev.peer) return;
    NetProto::PacketType ptype;
    std::string json;
    if (!NetProto::Unpack(ev.data, (size_t)ev.len, ptype, json)) return;
    if (ptype != NetProto::PKT_RPC_REQUEST) return;

    NetProto::JsonScope root(cJSON_Parse(json.c_str()));
    if (!root) {
        // parse error response 没法发, 因为不知道 id; 静默丢
        return;
    }

    cJSON* idNode     = cJSON_GetObjectItem(root.get(), "id");
    cJSON* methodNode = cJSON_GetObjectItem(root.get(), "method");
    cJSON* paramsNode = cJSON_GetObjectItem(root.get(), "params");

    if (!cJSON_IsString(methodNode)) {
        // invalid request
        if (cJSON_IsNumber(idNode)) {
            NetProto::JsonScope resp(cJSON_CreateObject());
            cJSON_AddNumberToObject(resp.get(), "id", idNode->valuedouble);
            AttachJsonError(resp.get(), RpcErr::INVALID_REQUEST, "missing or invalid method");
            SendJsonPacket(ev.peer, resp.get());
        }
        return;
    }

    const char* method = methodNode->valuestring;
    bool isNotify = !cJSON_IsNumber(idNode);
    int  callId  = cJSON_IsNumber(idNode) ? (int)idNode->valuedouble : 0;

    lua_State* L = s->L;
    int entryTop = lua_gettop(L);              // 入口栈高度, 出口必须 settop(L, entryTop)

    // 查 method handler
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->methodsRef);
    lua_getfield(L, -1, method);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, entryTop);
        if (!isNotify) {
            NetProto::JsonScope resp(cJSON_CreateObject());
            cJSON_AddNumberToObject(resp.get(), "id", (double)callId);
            AttachJsonError(resp.get(), RpcErr::METHOD_NOT_FOUND, method);
            SendJsonPacket(ev.peer, resp.get());
        }
        return;
    }
    // 调 handler(params_lua, peer_id) → result | nil, err_table
    PushCJsonAsLua(L, paramsNode);
    lua_pushinteger(L, (lua_Integer)PlatformNet::EnetPeerID(ev.peer));
    // 此时栈: entry..., methods_tbl, handler, params, peer_id
    if (lua_pcall(L, 2, 2, 0) != 0) {
        const char* m = lua_tostring(L, -1);
        if (!isNotify) {
            NetProto::JsonScope resp(cJSON_CreateObject());
            cJSON_AddNumberToObject(resp.get(), "id", (double)callId);
            AttachJsonError(resp.get(), RpcErr::INTERNAL_ERROR, m ? m : "handler error");
            SendJsonPacket(ev.peer, resp.get());
        } else {
            CC::Log(CC::LOG_WARN, "Light.Network.Rpc method '%s' error: %s",
                    method, m ? m : "(unknown)");
        }
        lua_settop(L, entryTop);
        return;
    }
    // 栈: entry..., methods_tbl, ret1, ret2
    if (!isNotify) {
        NetProto::JsonScope resp(cJSON_CreateObject());
        cJSON_AddNumberToObject(resp.get(), "id", (double)callId);
        if (lua_isnil(L, -2) && lua_istable(L, -1)) {
            // handler 返回 nil, err_table
            cJSON* err = PushLuaAsCJson(L, -1);
            if (err) cJSON_AddItemToObject(resp.get(), "error", err);
            else AttachJsonError(resp.get(), RpcErr::INTERNAL_ERROR, "bad error table");
        } else {
            // handler 返回 result, nil (or result alone)
            cJSON* result = PushLuaAsCJson(L, -2);
            cJSON_AddItemToObject(resp.get(), "result",
                                  result ? result : cJSON_CreateNull());
        }
        SendJsonPacket(ev.peer, resp.get());
    }
    lua_settop(L, entryTop);
}

static void ServerHandleEvent(RpcServer* s, const PlatformNet::EnetEvent& ev) {
    using ET = PlatformNet::EnetEventType;
    switch (ev.type) {
        case ET::CONNECT:
            FireServerEvent(s, "peer_connect", PlatformNet::EnetPeerID(ev.peer));
            break;
        case ET::DISCONNECT:
            FireServerEvent(s, "peer_disconnect", PlatformNet::EnetPeerID(ev.peer));
            break;
        case ET::RECEIVE:
            ServerHandleReceive(s, ev);
            break;
        default: break;
    }
}

// ==================== Client Lua API ====================

// Rpc.Connect(host, port) -> client | nil, err
static int l_Rpc_Connect(lua_State* L) {
    const char* host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    if (port <= 0 || port > 65535) {
        lua_pushnil(L);
        lua_pushstring(L, "port out of range");
        return 2;
    }

    auto* eh = PlatformNet::EnetCreateHost(nullptr, 0, /*peers=*/1, /*ch=*/2);
    if (!eh) {
        lua_pushnil(L);
        lua_pushstring(L, "EnetCreateHost failed (Web platform?)");
        return 2;
    }
    auto* peer = PlatformNet::EnetConnect(eh, host, (uint16_t)port, 2);
    if (!peer) {
        PlatformNet::EnetDestroyHost(eh);
        lua_pushnil(L);
        lua_pushfstring(L, "EnetConnect failed (%s:%d)", host, port);
        return 2;
    }

    auto* c = (RpcClient*)lua_newuserdata(L, sizeof(RpcClient));
    c->host       = eh;
    c->peer       = peer;
    c->onEventRef = LUA_NOREF;
    c->nextId     = 1;
    c->connected  = false;
    c->L          = L;
    // pending = {}
    lua_newtable(L);
    c->pendingRef = luaL_ref(L, LUA_REGISTRYINDEX);

    luaL_getmetatable(L, RPC_CLIENT_MT);
    lua_setmetatable(L, -2);

    // 用 lambda 捕获 c 注册到 EnetSetEventCb
    // 注: 若 userdata 被 GC, EnetDestroyHost 已先在 __gc 调过, 此 cb 不会再触发
    PlatformNet::EnetSetEventCb(eh,
        [c](const PlatformNet::EnetEvent& ev) {
            ClientHandleEvent(c, ev);
        });
    return 1;
}

// client:Call(method, params, cb) -> bool
static int l_RpcClient_Call(lua_State* L) {
    auto* c = CheckClient(L, 1);
    if (!c->peer) return luaL_error(L, "client closed");
    const char* method = luaL_checkstring(L, 2);
    // params 在 3, cb 在 4 (cb 必须是 function)
    luaL_checktype(L, 4, LUA_TFUNCTION);

    uint32_t id = c->nextId++;

    // 构造 request JSON
    NetProto::JsonScope req(cJSON_CreateObject());
    cJSON_AddNumberToObject(req.get(), "id", (double)id);
    cJSON_AddStringToObject(req.get(), "method", method);
    cJSON* params = PushLuaAsCJson(L, 3);
    cJSON_AddItemToObject(req.get(), "params", params ? params : cJSON_CreateNull());

    if (!SendJsonPacket(c->peer, req.get())) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 注册 cb 到 pending[id]
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->pendingRef);
    lua_pushvalue(L, 4);
    lua_rawseti(L, -2, (int)id);
    lua_pop(L, 1);

    lua_pushboolean(L, 1);
    return 1;
}

// client:Notify(method, params) -> bool
static int l_RpcClient_Notify(lua_State* L) {
    auto* c = CheckClient(L, 1);
    if (!c->peer) return luaL_error(L, "client closed");
    const char* method = luaL_checkstring(L, 2);

    NetProto::JsonScope req(cJSON_CreateObject());
    cJSON_AddStringToObject(req.get(), "method", method);
    cJSON* params = PushLuaAsCJson(L, 3);
    cJSON_AddItemToObject(req.get(), "params", params ? params : cJSON_CreateNull());

    lua_pushboolean(L, SendJsonPacket(c->peer, req.get()) ? 1 : 0);
    return 1;
}

// client:OnEvent(cb) -> ()
static int l_RpcClient_OnEvent(lua_State* L) {
    auto* c = CheckClient(L, 1);
    if (c->onEventRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->onEventRef);
        c->onEventRef = LUA_NOREF;
    }
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        c->onEventRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// client:Close()
static int l_RpcClient_Close(lua_State* L) {
    auto* c = CheckClient(L, 1);
    if (c->peer) {
        PlatformNet::EnetDisconnect(c->peer, 0);
        c->peer = nullptr;
    }
    if (c->host) {
        PlatformNet::EnetDestroyHost(c->host);
        c->host = nullptr;
    }
    if (c->onEventRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->onEventRef);
        c->onEventRef = LUA_NOREF;
    }
    if (c->pendingRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->pendingRef);
        c->pendingRef = LUA_NOREF;
    }
    return 0;
}

static int l_RpcClient_Gc(lua_State* L) {
    return l_RpcClient_Close(L);
}

static int l_RpcClient_Tostring(lua_State* L) {
    auto* c = CheckClient(L, 1);
    lua_pushfstring(L, "Light.Network.Rpc.Client(%s)",
                    c->peer ? (c->connected ? "connected" : "connecting") : "closed");
    return 1;
}

// ==================== Server Lua API ====================

// Rpc.Listen(ip, port, maxPeers=32) -> server | nil, err
static int l_Rpc_Listen(lua_State* L) {
    const char* ip = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int maxPeers = (int)luaL_optinteger(L, 3, 32);
    if (port <= 0 || port > 65535) {
        lua_pushnil(L);
        lua_pushstring(L, "port out of range");
        return 2;
    }
    if (maxPeers <= 0 || maxPeers > 4096) maxPeers = 32;

    auto* eh = PlatformNet::EnetCreateHost(ip, (uint16_t)port, maxPeers, 2);
    if (!eh) {
        lua_pushnil(L);
        lua_pushfstring(L, "EnetCreateHost failed (port=%d)", port);
        return 2;
    }

    auto* s = (RpcServer*)lua_newuserdata(L, sizeof(RpcServer));
    s->host       = eh;
    s->onEventRef = LUA_NOREF;
    s->L          = L;
    lua_newtable(L);
    s->methodsRef = luaL_ref(L, LUA_REGISTRYINDEX);

    luaL_getmetatable(L, RPC_SERVER_MT);
    lua_setmetatable(L, -2);

    PlatformNet::EnetSetEventCb(eh,
        [s](const PlatformNet::EnetEvent& ev) {
            ServerHandleEvent(s, ev);
        });
    return 1;
}

// server:RegisterMethod(name, handler) -> ()
static int l_RpcServer_RegisterMethod(lua_State* L) {
    auto* s = CheckServer(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_rawgeti(L, LUA_REGISTRYINDEX, s->methodsRef);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

// server:UnregisterMethod(name) -> ()
static int l_RpcServer_UnregisterMethod(lua_State* L) {
    auto* s = CheckServer(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->methodsRef);
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

// server:OnEvent(cb)
static int l_RpcServer_OnEvent(lua_State* L) {
    auto* s = CheckServer(L, 1);
    if (s->onEventRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->onEventRef);
        s->onEventRef = LUA_NOREF;
    }
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        s->onEventRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// server:Close()
static int l_RpcServer_Close(lua_State* L) {
    auto* s = CheckServer(L, 1);
    if (s->host) {
        PlatformNet::EnetDestroyHost(s->host);
        s->host = nullptr;
    }
    if (s->onEventRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->onEventRef);
        s->onEventRef = LUA_NOREF;
    }
    if (s->methodsRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->methodsRef);
        s->methodsRef = LUA_NOREF;
    }
    return 0;
}

static int l_RpcServer_Gc(lua_State* L) {
    return l_RpcServer_Close(L);
}

static int l_RpcServer_Tostring(lua_State* L) {
    auto* s = CheckServer(L, 1);
    lua_pushfstring(L, "Light.Network.Rpc.Server(%s)",
                    s->host ? "listening" : "closed");
    return 1;
}

// ==================== 元表注册 ====================

static void RegisterClientMt(lua_State* L) {
    luaL_newmetatable(L, RPC_CLIENT_MT);
    static const luaL_Reg methods[] = {
        { "Call",    l_RpcClient_Call    },
        { "Notify",  l_RpcClient_Notify  },
        { "OnEvent", l_RpcClient_OnEvent },
        { "Close",   l_RpcClient_Close   },
        { nullptr, nullptr },
    };
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pushcfunction(L, l_RpcClient_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_RpcClient_Tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

static void RegisterServerMt(lua_State* L) {
    luaL_newmetatable(L, RPC_SERVER_MT);
    static const luaL_Reg methods[] = {
        { "RegisterMethod",   l_RpcServer_RegisterMethod   },
        { "UnregisterMethod", l_RpcServer_UnregisterMethod },
        { "OnEvent",          l_RpcServer_OnEvent          },
        { "Close",            l_RpcServer_Close            },
        { nullptr, nullptr },
    };
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pushcfunction(L, l_RpcServer_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_RpcServer_Tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

// ==================== luaopen_Light_Network_Rpc ====================

extern "C" int luaopen_Light_Network(lua_State* L);

extern "C" LIGHT_API int luaopen_Light_Network_Rpc(lua_State* L) {
    RegisterClientMt(L);
    RegisterServerMt(L);

    luaopen_Light_Network(L);

    lua_pushstring(L, "Rpc");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Rpc");
        lua_createtable(L, 0, 0);

        const luaL_Reg fns[] = {
            { "Connect", l_Rpc_Connect },
            { "Listen",  l_Rpc_Listen  },
            { nullptr, nullptr         },
        };
        luaL_setfuncs(L, fns, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Rpc");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
