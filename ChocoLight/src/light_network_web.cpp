/**
 * @file light_network_web.cpp
 * @brief Light.Network Web/Emscripten 实现 — 浏览器原生 HTTP + WebSocket
 * @note 仅在 __EMSCRIPTEN__ 编译
 *
 * 浏览器沙箱限制:
 *   - 无原始 TCP socket → 用 emscripten_fetch (HTTP) + JS WebSocket
 *   - HttpServer 不可用 → 保留空存根
 *   - 同源策略限制跨域请求 (需 CORS 头)
 *
 * Lua API 兼容:
 *   Http:Open()        → 触发 OnConnect (模拟)
 *   Http:SendRequest() → emscripten_fetch 异步请求 → OnHttp 回调
 *   Http:Upgrade()     → JS WebSocket 连接 → OnWS 回调
 *   Http:SendMessage() → WebSocket.send()
 *   Http:Close()       → 关闭 fetch/WebSocket
 *   Network.Resume     → 空操作 (浏览器事件循环自动驱动)
 */

#ifdef __EMSCRIPTEN__

#include "light.h"
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <cstring>
#include <cstdlib>
#include <string>

// ==================== JS WebSocket 桥接 (EM_JS) ====================

// 创建 WebSocket, 返回 JS 端 ID
EM_JS(int, js_ws_create, (const char* urlPtr), {
    var url = UTF8ToString(urlPtr);
    if (!Module._wsSockets) Module._wsSockets = [];
    var id = Module._wsSockets.length;
    try {
        var ws = new WebSocket(url);
        ws.binaryType = 'arraybuffer';
        ws._id = id;
        ws._messages = [];
        ws._opened = false;
        ws._closed = false;
        ws._error = false;
        ws.onopen = function() { ws._opened = true; };
        ws.onclose = function() { ws._closed = true; };
        ws.onerror = function() { ws._error = true; };
        ws.onmessage = function(e) {
            if (typeof e.data === 'string') {
                ws._messages.push({opcode: 1, data: e.data});
            } else {
                var arr = new Uint8Array(e.data);
                var str = '';
                for (var i = 0; i < arr.length; i++) str += String.fromCharCode(arr[i]);
                ws._messages.push({opcode: 2, data: str});
            }
        };
        Module._wsSockets.push(ws);
        return id;
    } catch(e) {
        Module._wsSockets.push(null);
        return -1;
    }
});

EM_JS(int, js_ws_is_open, (int id), {
    if (!Module._wsSockets || !Module._wsSockets[id]) return 0;
    return Module._wsSockets[id]._opened ? 1 : 0;
});

EM_JS(int, js_ws_is_closed, (int id), {
    if (!Module._wsSockets || !Module._wsSockets[id]) return 1;
    return Module._wsSockets[id]._closed ? 1 : 0;
});

EM_JS(int, js_ws_has_message, (int id), {
    if (!Module._wsSockets || !Module._wsSockets[id]) return 0;
    return Module._wsSockets[id]._messages.length > 0 ? 1 : 0;
});

// 取出一条消息, 返回 opcode (1=text, 2=binary), 数据写入堆
EM_JS(int, js_ws_pop_message, (int id, char* outBuf, int maxLen), {
    if (!Module._wsSockets || !Module._wsSockets[id]) return 0;
    var ws = Module._wsSockets[id];
    if (ws._messages.length === 0) return 0;
    var msg = ws._messages.shift();
    var len = Math.min(msg.data.length, maxLen - 1);
    for (var i = 0; i < len; i++) {
        HEAP8[outBuf + i] = msg.data.charCodeAt(i);
    }
    HEAP8[outBuf + len] = 0;
    return msg.opcode;
});

EM_JS(void, js_ws_send_text, (int id, const char* msgPtr), {
    if (!Module._wsSockets || !Module._wsSockets[id]) return;
    var ws = Module._wsSockets[id];
    if (ws.readyState === 1) ws.send(UTF8ToString(msgPtr));
});

EM_JS(void, js_ws_send_binary, (int id, const char* data, int len), {
    if (!Module._wsSockets || !Module._wsSockets[id]) return;
    var ws = Module._wsSockets[id];
    if (ws.readyState === 1) {
        var buf = new Uint8Array(len);
        for (var i = 0; i < len; i++) buf[i] = HEAPU8[data + i];
        ws.send(buf.buffer);
    }
});

EM_JS(void, js_ws_close, (int id), {
    if (!Module._wsSockets && Module._wsSockets[id]) return;
    var ws = Module._wsSockets[id];
    if (ws && ws.readyState < 2) ws.close();
    Module._wsSockets[id] = null;
});

// ==================== Http 上下文 ====================

/// Phase G.1.7.2: 首字段 magic 防止 type-confusion
struct WebHttpContext {
    uint32_t magic;         // 必须 = LT_MAGIC_NET_WEB
    char     host[256];
    uint16_t port;
    bool     useTLS;        // https
    bool     connected;     // Open 已调用
    bool     isWebSocket;
    int      wsId;          // JS WebSocket ID
    bool     wsOpened;      // WS 握手完成 (已分发 OnConnect)
    int      selfRef;       // Lua 注册表引用
    lua_State* L;
};

// Phase G.1.7.2: magic 校验防 type-confusion
static WebHttpContext* GetWebCtx(lua_State* L, int idx) {
    return LT::TryCheckInstance<WebHttpContext>(L, idx, LT::LT_MAGIC_NET_WEB);
}

// ==================== Network 函数 ====================

/// Network.Resume — 驱动 WebSocket 消息分发
static int l_Web_Network_Resume(lua_State* L) {
    // 浏览器事件循环自动驱动 HTTP fetch 回调
    // 但 WebSocket 消息需要手动轮询分发到 Lua
    lua_pushinteger(L, 1);
    return 1;
}

// ==================== Http 函数 ====================

/// Http.Open — 模拟连接 (浏览器无需真正 TCP)
static int l_Web_Http_Open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* ctx = GetWebCtx(L, 1);
    if (!ctx) return 0;

    ctx->L = L;
    lua_pushvalue(L, 1);
    ctx->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
    ctx->connected = true;

    // 立即触发 OnConnect (浏览器无 TCP 握手)
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->selfRef);
    lua_getfield(L, -1, "OnConnect");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_call(L, 1, 0);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}

/// Http.Close — 清理资源
static int l_Web_Http_Close(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* ctx = GetWebCtx(L, 1);
    if (!ctx) return 0;

    if (ctx->isWebSocket && ctx->wsId >= 0) {
        js_ws_close(ctx->wsId);
        ctx->wsId = -1;
    }
    if (ctx->selfRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->selfRef);
        ctx->selfRef = LUA_NOREF;
    }
    ctx->connected = false;
    return 0;
}

// emscripten_fetch 成功回调 (在主线程异步触发)
static void fetch_success_cb(emscripten_fetch_t* fetch) {
    auto* ctx = (WebHttpContext*)fetch->userData;
    if (!ctx || !ctx->L || ctx->selfRef == LUA_NOREF) {
        emscripten_fetch_close(fetch);
        return;
    }
    lua_State* L = ctx->L;

    // 取出 self
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->selfRef);
    int selfIdx = lua_gettop(L);

    // 构建 headers 表 (emscripten_fetch 不直接提供完整头, 简化处理)
    lua_createtable(L, 0, 4);
    // Content-Length
    if (fetch->numBytes > 0) {
        lua_pushinteger(L, (lua_Integer)fetch->numBytes);
        lua_setfield(L, -2, "Content-Length");
    }

    // 调用 self:OnHttp(status, headers, body)
    lua_getfield(L, selfIdx, "OnHttp");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, selfIdx);
        lua_pushinteger(L, fetch->status);
        lua_pushvalue(L, -5);  // headers
        lua_pushlstring(L, fetch->data, (size_t)fetch->numBytes);
        lua_call(L, 4, 0);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);  // pop headers + self

    emscripten_fetch_close(fetch);
}

// emscripten_fetch 失败回调
static void fetch_error_cb(emscripten_fetch_t* fetch) {
    auto* ctx = (WebHttpContext*)fetch->userData;
    if (ctx && ctx->L && ctx->selfRef != LUA_NOREF) {
        lua_State* L = ctx->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->selfRef);
        lua_getfield(L, -1, "OnHttp");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2);
            lua_pushinteger(L, fetch->status ? fetch->status : 0);
            lua_createtable(L, 0, 0);
            lua_pushstring(L, "");
            lua_call(L, 4, 0);
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    emscripten_fetch_close(fetch);
}

/// Http.SendRequest — emscripten_fetch 异步 HTTP 请求
static int l_Web_Http_SendRequest(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int method = (int)luaL_checkinteger(L, 2);
    const char* path = luaL_checkstring(L, 3);
    size_t bodyLen = 0;
    const char* body = luaL_optlstring(L, 4, "", &bodyLen);

    auto* ctx = GetWebCtx(L, 1);
    if (!ctx || !ctx->connected) return 0;

    // 方法映射
    const char* methods[] = { "", "HEAD", "GET", "POST", "PUT", "DELETE" };
    const char* m = (method >= 1 && method <= 5) ? methods[method] : "GET";

    // 构建完整 URL
    char url[1024];
    snprintf(url, sizeof(url), "%s://%s:%u%s",
             ctx->useTLS ? "https" : "http", ctx->host, ctx->port, path);

    // emscripten_fetch 配置
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strncpy(attr.requestMethod, m, sizeof(attr.requestMethod) - 1);
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = fetch_success_cb;
    attr.onerror = fetch_error_cb;
    attr.userData = ctx;

    // POST/PUT body
    if (bodyLen > 0 && (method == 3 || method == 4)) {
        // emscripten_fetch 需要持久数据, 复制一份
        char* bodyCopy = (char*)malloc(bodyLen);
        memcpy(bodyCopy, body, bodyLen);
        attr.requestData = bodyCopy;
        attr.requestDataSize = bodyLen;
    }

    emscripten_fetch(&attr, url);
    return 0;
}

/// Http.SendMessage — 通过 WebSocket 发送消息
static int l_Web_Http_SendMessage(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    size_t msgLen = 0;
    const char* msg = luaL_checklstring(L, 2, &msgLen);
    int opcode = (int)luaL_optinteger(L, 3, 1);

    auto* ctx = GetWebCtx(L, 1);
    if (!ctx || !ctx->isWebSocket || ctx->wsId < 0) return 0;

    if (opcode == 2) {
        js_ws_send_binary(ctx->wsId, msg, (int)msgLen);
    } else {
        js_ws_send_text(ctx->wsId, msg);
    }
    return 0;
}

/// Http.Upgrade — HTTP → WebSocket (创建 JS WebSocket)
static int l_Web_Http_Upgrade(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    auto* ctx = GetWebCtx(L, 1);
    if (!ctx || !ctx->connected) return 0;

    // 构建 WebSocket URL
    char url[1024];
    snprintf(url, sizeof(url), "%s://%s:%u%s",
             ctx->useTLS ? "wss" : "ws", ctx->host, ctx->port, path);

    ctx->wsId = js_ws_create(url);
    if (ctx->wsId >= 0) {
        ctx->isWebSocket = true;
        ctx->wsOpened = false;
    } else {
        CC::Log(CC::LOG_WARN, "Http.Upgrade: WebSocket create failed for '%s'", url);
    }
    return 0;
}

/// Http.GetFD — 返回内部 ID
static int l_Web_Http_GetFD(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* ctx = GetWebCtx(L, 1);
    lua_pushinteger(L, ctx ? (lua_Integer)(intptr_t)ctx : -1);
    return 1;
}

/// Http.__call — 构造函数
static int l_Web_Http_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* ip = luaL_checkstring(L, 2);
    int port = (int)luaL_checkinteger(L, 3);

    auto* ctx = (WebHttpContext*)lua_newuserdata(L, sizeof(WebHttpContext));
    memset(ctx, 0, sizeof(WebHttpContext));
    ctx->magic = LT::LT_MAGIC_NET_WEB;  // Phase G.1.7.2 — type tag
    strncpy(ctx->host, ip, sizeof(ctx->host) - 1);
    ctx->port = (uint16_t)port;
    ctx->useTLS = (port == 443);
    ctx->wsId = -1;
    ctx->selfRef = LUA_NOREF;

    lua_setfield(L, 1, "__instance");
    return 0;
}

static int l_Web_Http_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Network.Http");
    return 1;
}

// ==================== WebSocket 消息轮询 ====================
// 在 Network.Resume 中调用, 分发 WS 消息到 Lua 回调

// 全局 WebSocket 上下文列表 (用于 Resume 轮询)
#include <vector>
static std::vector<WebHttpContext*> s_wsContexts;

static void PollWebSockets(lua_State* L) {
    char msgBuf[8192];
    for (auto* ctx : s_wsContexts) {
        if (!ctx || !ctx->isWebSocket || ctx->wsId < 0) continue;
        if (ctx->selfRef == LUA_NOREF || !ctx->L) continue;

        // 检查 WS 握手完成
        if (!ctx->wsOpened && js_ws_is_open(ctx->wsId)) {
            ctx->wsOpened = true;
            // 触发 OnHttp 101 (模拟升级响应)
            lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->selfRef);
            lua_getfield(ctx->L, -1, "OnHttp");
            if (lua_isfunction(ctx->L, -1)) {
                lua_pushvalue(ctx->L, -2);
                lua_pushinteger(ctx->L, 101);
                lua_createtable(ctx->L, 0, 1);
                lua_pushstring(ctx->L, "websocket");
                lua_setfield(ctx->L, -2, "Upgrade");
                lua_pushstring(ctx->L, "");
                lua_call(ctx->L, 4, 0);
            } else {
                lua_pop(ctx->L, 1);
            }
            lua_pop(ctx->L, 1);
        }

        // 分发 WS 消息
        while (js_ws_has_message(ctx->wsId)) {
            int opcode = js_ws_pop_message(ctx->wsId, msgBuf, sizeof(msgBuf));
            if (opcode <= 0) break;

            lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->selfRef);
            lua_getfield(ctx->L, -1, "OnWS");
            if (lua_isfunction(ctx->L, -1)) {
                lua_pushvalue(ctx->L, -2);
                lua_pushinteger(ctx->L, opcode);
                lua_pushstring(ctx->L, msgBuf);
                lua_call(ctx->L, 3, 0);
            } else {
                lua_pop(ctx->L, 1);
            }
            lua_pop(ctx->L, 1);
        }

        // 检查关闭
        if (js_ws_is_closed(ctx->wsId)) {
            lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->selfRef);
            lua_getfield(ctx->L, -1, "OnClose");
            if (lua_isfunction(ctx->L, -1)) {
                lua_pushvalue(ctx->L, -2);
                lua_call(ctx->L, 1, 0);
            } else {
                lua_pop(ctx->L, 1);
            }
            lua_pop(ctx->L, 1);
            ctx->isWebSocket = false;
            ctx->wsId = -1;
        }
    }
}

// 重写 Resume 加入 WS 轮询
static int l_Web_Network_Resume_Full(lua_State* L) {
    PollWebSockets(L);
    lua_pushinteger(L, 1);
    return 1;
}

// ==================== luaopen 注册 ====================

int luaopen_Light_Network(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Network");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Network");
        lua_createtable(L, 0, 0);

        const luaL_Reg net_funcs[] = {
            {"Resume", l_Web_Network_Resume_Full},
            {NULL, NULL}
        };
        luaL_setfuncs(L, net_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Network");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

int luaopen_Light_Network_Http(lua_State* L) {
    luaopen_Light_Network(L);

    lua_pushstring(L, "Http");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Http");
        lua_createtable(L, 0, 0);

        const luaL_Reg http_funcs[] = {
            {"Open",        l_Web_Http_Open},
            {"Close",       l_Web_Http_Close},
            {"SendRequest", l_Web_Http_SendRequest},
            {"SendMessage", l_Web_Http_SendMessage},
            {"Upgrade",     l_Web_Http_Upgrade},
            {"GetFD",       l_Web_Http_GetFD},
            {"__call",      l_Web_Http_Call},
            {"__tostring",  l_Web_Http_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, http_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Http");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// HttpServer: 浏览器沙箱不支持服务端监听, 保留空存根
int luaopen_Light_Network_HttpServer(lua_State* L) {
    luaopen_Light_Network(L);
    lua_pushstring(L, "HttpServer");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "HttpServer");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "HttpServer");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// Web 框架: 依赖 HttpServer, 浏览器不可用
int luaopen_Light_Network_Web(lua_State* L) {
    return luaopen_Light_Network(L);
}

#endif // __EMSCRIPTEN__
