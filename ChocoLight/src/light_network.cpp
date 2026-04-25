/**
 * @file light_network.cpp
 * @brief Light.Network + Http + HttpServer + Web 模块
 * @note 深度还原自 Light.dll IDA 反编译
 *
 * Network API:
 *   Resume       — 恢复网络轮询
 *   __lightHttp  — 内部 HTTP 连接池表
 *
 * Http API (8 函数, 还原自 sub_1800B10A0):
 *   Open()               — 建立 TCP 连接
 *   Close()              — 关闭连接
 *   SendRequest(m,p,b)   — 发送 HTTP 请求 (method, path, body)
 *   SendMessage(msg,op)  — 发送 WebSocket 消息
 *   Upgrade(path)        — HTTP → WebSocket 升级
 *   GetFD()              — 获取 socket 文件描述符
 *   __call(ip,port)      — 构造函数
 *   __tostring()         — "Light.Network.Http"
 *
 * HttpServer API (4 函数, 还原自 sub_1800B2560):
 *   Open()               — ���动服务器监听
 *   Close()              — 关闭服务器
 *   __call(ip,port)      — 构造函数
 *   __tostring()         — "Light.Network.HttpServer"
 *
 * Web — 纯 Lua Web 框架 (还原自 luaopen_Light_Network_Web 嵌入脚本):
 *   Open(ip,port)        — 启动 Web 服务器
 *   Close()              — 关闭 Web 服务器
 *   Get/Post/Put/Delete  — HTTP 路由绑定
 *   Head                 — HEAD 路由绑定
 *   Message/Join/Leave   — WebSocket 事件绑定
 *   Send(uId,msg,isBin)  — 向 WebSocket 客户端发送消息
 *   Session              — HTTP 客户端会话类
 *   Chat                 — WebSocket 聊天客户端
 */

#include "light.h"

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
// Web/Android: 网络模块不可用 (libuv 不支持), 提供空存根
int luaopen_Light_Network(lua_State* L) { LT::EnsureLightTable(L); lua_pushstring(L, "Network"); lua_createtable(L, 0, 0); lua_rawset(L, -3); lua_pushstring(L, "Network"); lua_rawget(L, -2); lua_remove(L, -2); return 1; }
int luaopen_Light_Network_Http(lua_State* L) { return luaopen_Light_Network(L); }
int luaopen_Light_Network_HttpServer(lua_State* L) { return luaopen_Light_Network(L); }
int luaopen_Light_Network_Web(lua_State* L) { return luaopen_Light_Network(L); }
#else

#include "light_platform_net.h"
#include <uv.h>
#include <cstring>
#include <cstdlib>
#include <string>

// ==================== Network 内部状态 ====================

struct HttpContext {
    uv_tcp_s*   handle;       // libuv TCP 句柄
    char        host[256];
    uint16_t    port;
    bool        connected;
    bool        isWebSocket;
    int         selfRef;      // Lua 注册表引用 (self 表)
    lua_State*  L;            // 回调用 Lua 状态
    std::string recvBuf;      // 接收缓冲区
};

// 辅助: 从 __instance 取上下文
static HttpContext* GetHttpCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    HttpContext* ctx = (HttpContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

// ==================== Network 函数 ====================

/// 辅助: 解析 HTTP 响应, 提取 status/headers/body
/// 简化解析: 找 \r\n\r\n 分割头/体
static void ParseAndDispatchHttp(lua_State* L, int selfIdx,
                                 const char* data, int len) {
    // 查找头尾分隔 \r\n\r\n
    const char* sep = nullptr;
    for (int i = 0; i < len - 3; ++i) {
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n') {
            sep = data + i;
            break;
        }
    }
    if (!sep) return;  // 未收完

    // 提取 status code ("HTTP/1.x NNN")
    int status = 0;
    const char* sp = strchr(data, ' ');
    if (sp) status = atoi(sp + 1);

    // 构建 headers 表
    lua_createtable(L, 0, 8);
    const char* hdrStart = strchr(data, '\n');
    if (hdrStart) {
        hdrStart++;
        while (hdrStart < sep) {
            const char* lineEnd = strstr(hdrStart, "\r\n");
            if (!lineEnd || lineEnd >= sep) break;
            const char* colon = (const char*)memchr(hdrStart, ':', lineEnd - hdrStart);
            if (colon) {
                lua_pushlstring(L, hdrStart, colon - hdrStart);
                // 跳过 ": "
                const char* valStart = colon + 1;
                while (valStart < lineEnd && *valStart == ' ') valStart++;
                lua_pushlstring(L, valStart, lineEnd - valStart);
                lua_rawset(L, -3);
            }
            hdrStart = lineEnd + 2;
        }
    }

    // body
    const char* body = sep + 4;
    int bodyLen = len - (int)(body - data);

    // 调用 self:OnHttp(status, headers, body)
    lua_getfield(L, selfIdx, "OnHttp");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, selfIdx);
        lua_pushinteger(L, status);
        lua_pushvalue(L, -5);  // headers 表
        lua_pushlstring(L, body, bodyLen > 0 ? bodyLen : 0);
        lua_call(L, 4, 0);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // pop headers 表
}

// 辅助: 分发接收到的数据到 Lua 回调 (在 PlatformNet::Poll 期间触发)
static void DispatchRecvData(HttpContext* ctx) {
    if (!ctx || !ctx->L || ctx->selfRef == LUA_NOREF || ctx->recvBuf.empty()) return;
    lua_State* L = ctx->L;

    // 取出 self 表
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->selfRef);
    int selfIdx = lua_gettop(L);

    const char* data = ctx->recvBuf.c_str();
    int len = (int)ctx->recvBuf.size();

    if (ctx->isWebSocket) {
        // WebSocket 帧解析
        if (len >= 2) {
            uint8_t opcode = (uint8_t)(data[0] & 0x0F);
            int payloadLen = (uint8_t)data[1] & 0x7F;
            int offset = 2;
            if (payloadLen == 126 && len >= 4) {
                payloadLen = ((uint8_t)data[2]<<8) | (uint8_t)data[3];
                offset = 4;
            }
            lua_getfield(L, selfIdx, "OnWS");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, selfIdx);
                lua_pushinteger(L, opcode);
                lua_pushlstring(L, data + offset, len - offset > 0 ? len - offset : 0);
                lua_call(L, 3, 0);
            } else lua_pop(L, 1);
        }
    } else {
        // HTTP 响应
        ParseAndDispatchHttp(L, selfIdx, data, len);
    }
    lua_pop(L, 1);  // pop self
    ctx->recvBuf.clear();
}

// libuv 读取回调: 将数据累积到 recvBuf 并分发
static void OnHttpRead(HttpContext* ctx, const char* data, int len) {
    if (len > 0 && data) {
        ctx->recvBuf.append(data, (size_t)len);
        DispatchRecvData(ctx);
    } else if (len < 0) {
        // 连接关闭/错误
        ctx->connected = false;
    }
}

/// @lua_api Light.Network.Resume
/// @brief 驱动网络 IO 事件循环 (基于 libuv, 每帧调用)
/// @return number 始终返回 1
static int l_Network_Resume(lua_State* L) {
    PlatformNet::Poll();
    lua_pushinteger(L, 1);
    return 1;
}

// ==================== Http 函数 ====================

/// @lua_api Light.Network.Http.Open
/// @brief 建立 TCP 连接 (异步, libuv 驱动)
/// @return void
static int l_Http_Open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    HttpContext* ctx = GetHttpCtx(L, 1);
    if (!ctx) return 0;

    // 创建 libuv TCP 句柄
    ctx->handle = PlatformNet::CreateClient();
    if (!ctx->handle) {
        CC::Log(CC::LOG_ERROR, "Http.Open: CreateClient failed");
        return 0;
    }

    // 保存 Lua 状态和 self 引用 (用于异步回调)
    ctx->L = L;
    lua_pushvalue(L, 1);
    ctx->selfRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // 异步连接 (DNS 解析 + TCP 连接)
    PlatformNet::Connect(ctx->handle, ctx->host, ctx->port,
        [ctx](int status) {
            if (status == 0) {
                ctx->connected = true;
                // 开始读取数据
                PlatformNet::StartRead(ctx->handle,
                    [ctx](const char* data, int len) {
                        OnHttpRead(ctx, data, len);
                    });
                // 触发 OnConnect 回调
                if (ctx->L && ctx->selfRef != LUA_NOREF) {
                    lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->selfRef);
                    lua_getfield(ctx->L, -1, "OnConnect");
                    if (lua_isfunction(ctx->L, -1)) {
                        lua_pushvalue(ctx->L, -2);
                        lua_call(ctx->L, 1, 0);
                    } else lua_pop(ctx->L, 1);
                    lua_pop(ctx->L, 1);
                }
            } else {
                CC::Log(CC::LOG_WARN, "Http.Open: connect to %s:%u failed (%d)",
                        ctx->host, ctx->port, status);
            }
        });
    return 0;
}

/// @lua_api Light.Network.Http.Close
/// @brief 关闭 TCP 连接
/// @return void
static int l_Http_Close(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    HttpContext* ctx = GetHttpCtx(L, 1);
    if (ctx) {
        if (ctx->handle) {
            PlatformNet::Close(ctx->handle);
            ctx->handle = nullptr;
        }
        if (ctx->selfRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ctx->selfRef);
            ctx->selfRef = LUA_NOREF;
        }
        ctx->connected = false;
        ctx->recvBuf.clear();
    }
    return 0;
}

/// @lua_api Light.Network.Http.SendRequest
/// @brief 发送 HTTP 请求
/// @param method string HTTP 方法 (GET/POST/PUT/DELETE)
/// @param path string 请求路径
/// @param body string? 请求体
/// @return void
static int l_Http_SendRequest(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int method = (int)luaL_checkinteger(L, 2);
    const char* path = luaL_checkstring(L, 3);
    const char* body = luaL_optstring(L, 4, "");

    HttpContext* ctx = GetHttpCtx(L, 1);
    if (!ctx || !ctx->connected) return 0;

    // 方法映射: 1=HEAD, 2=GET, 3=POST, 4=PUT, 5=DELETE
    const char* methods[] = { "", "HEAD", "GET", "POST", "PUT", "DELETE" };
    const char* m = (method >= 1 && method <= 5) ? methods[method] : "GET";

    // 构建 HTTP 请求
    char buf[4096];
    int len = sprintf(buf, 
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        m, path, ctx->host, ctx->port, strlen(body), body);

    PlatformNet::Write(ctx->handle, buf, (size_t)len);
    return 0;
}

/// @lua_api Light.Network.Http.SendMessage
/// @brief 发送 WebSocket 消息
/// @param msg string 消息内容
/// @param opcode number? 操作码 (1=文本, 2=二进制, 默认 1)
/// @return void
static int l_Http_SendMessage(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    size_t msgLen = 0;
    const char* msg = luaL_checklstring(L, 2, &msgLen);
    int opcode = (int)luaL_optinteger(L, 3, 1);  // 1=text, 2=binary

    HttpContext* ctx = GetHttpCtx(L, 1);
    if (!ctx || !ctx->connected || !ctx->isWebSocket) return 0;

    // 构建 WebSocket 帧 (客户端发送必须 MASK)
    unsigned char header[14];  // 最大帧头 14 字节
    int headerLen = 0;

    // byte 0: FIN(1) + RSV(000) + opcode(4)
    header[headerLen++] = (unsigned char)(0x80 | (opcode & 0x0F));

    // byte 1: MASK(1) + payload length
    if (msgLen <= 125) {
        header[headerLen++] = (unsigned char)(0x80 | msgLen);
    } else if (msgLen <= 65535) {
        header[headerLen++] = (unsigned char)(0x80 | 126);
        header[headerLen++] = (unsigned char)((msgLen >> 8) & 0xFF);
        header[headerLen++] = (unsigned char)(msgLen & 0xFF);
    } else {
        header[headerLen++] = (unsigned char)(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            header[headerLen++] = (unsigned char)((msgLen >> (i * 8)) & 0xFF);
    }

    // masking key (4 字节随机)
    unsigned char maskKey[4];
    unsigned int seed = (unsigned int)uv_hrtime();
    for (int i = 0; i < 4; ++i) {
        seed = seed * 1103515245 + 12345;
        maskKey[i] = (unsigned char)(seed >> 16);
    }
    memcpy(&header[headerLen], maskKey, 4);
    headerLen += 4;

    // 发送帧头
    PlatformNet::Write(ctx->handle, (const char*)header, (size_t)headerLen);

    // 发送经 XOR mask 的 payload
    if (msgLen > 0) {
        char* masked = (char*)malloc(msgLen);
        for (size_t i = 0; i < msgLen; ++i)
            masked[i] = msg[i] ^ maskKey[i & 3];
        PlatformNet::Write(ctx->handle, masked, msgLen);
        free(masked);
    }
    return 0;
}

/// @lua_api Light.Network.Http.Upgrade
/// @brief HTTP 升级到 WebSocket
/// @param path string? 升级路径 (默认 "/")
/// @return void
static int l_Http_Upgrade(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);
    HttpContext* ctx = GetHttpCtx(L, 1);
    if (!ctx || !ctx->connected) return 0;

    // 发送 WebSocket 握手请求
    char buf[1024];
    int len = sprintf(buf,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, ctx->host, ctx->port);
    PlatformNet::Write(ctx->handle, buf, (size_t)len);
    ctx->isWebSocket = true;
    return 0;
}

/// @lua_api Light.Network.Http.GetFD
/// @brief 获取底层句柄指针 (兼容旧接口)
/// @return number
static int l_Http_GetFD(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    HttpContext* ctx = GetHttpCtx(L, 1);
    lua_pushinteger(L, ctx && ctx->handle ? (lua_Integer)(intptr_t)ctx->handle : -1);
    return 1;
}

/// @lua_api Light.Network.Http.__call
/// @brief 构造函数, 创建 HTTP 客户端实例
/// @param ip string 服务器 IP/域名
/// @param port number 端口
/// @return void
static int l_Http_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* ip = luaL_checkstring(L, 2);
    int port = (int)luaL_checkinteger(L, 3);

    HttpContext* ctx = (HttpContext*)lua_newuserdata(L, sizeof(HttpContext));
    memset(ctx, 0, sizeof(HttpContext));
    strncpy(ctx->host, ip, sizeof(ctx->host) - 1);
    ctx->port = (uint16_t)port;
    ctx->handle = nullptr;
    ctx->selfRef = LUA_NOREF;
    ctx->L = nullptr;
    // 就地构造 std::string (userdata 内存已 memset 0, 手动初始化)
    new (&ctx->recvBuf) std::string();

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// Http.__tostring
/// 还原自 sub_1800B1710
static int l_Http_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Network.Http");
    return 1;
}

// ==================== HttpServer 函数 ====================

/// @lua_api Light.Network.HttpServer.Open
/// @brief 启动服务器监听 (基于 libuv, 新连接通过 OnAccept 回调分发)
/// @return boolean 监听是否成功
static int l_HttpServer_Open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__server_handle");
    if (lua_islightuserdata(L, -1)) {
        uv_tcp_s* server = (uv_tcp_s*)lua_touserdata(L, -1);
        lua_pop(L, 1);

        // 保存 Lua 状态和 self 引用
        lua_pushvalue(L, 1);
        int selfRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_State* Lref = L;

        bool ok = PlatformNet::Listen(server, 128,
            [Lref, selfRef](uv_tcp_s* client) {
                // 新连接回调
                lua_rawgeti(Lref, LUA_REGISTRYINDEX, selfRef);
                lua_getfield(Lref, -1, "OnAccept");
                if (lua_isfunction(Lref, -1)) {
                    lua_pushvalue(Lref, -2);
                    lua_pushlightuserdata(Lref, client);
                    lua_call(Lref, 2, 0);
                } else {
                    lua_pop(Lref, 1);
                    PlatformNet::Close(client);
                }
                lua_pop(Lref, 1);
            });
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    }
    lua_pop(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

/// @lua_api Light.Network.HttpServer.Close
/// @brief 关闭服务器
/// @return void
static int l_HttpServer_Close(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__server_handle");
    if (lua_islightuserdata(L, -1)) {
        uv_tcp_s* server = (uv_tcp_s*)lua_touserdata(L, -1);
        if (server) {
            PlatformNet::Close(server);
            CC::Log(CC::LOG_INFO, "HttpServer: closed");
        }
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setfield(L, 1, "__server_handle");
    return 0;
}

/// @lua_api Light.Network.HttpServer.__call
/// @brief 构造函数, 创建 HTTP 服务器 (基于 libuv)
/// @param ip string 监听 IP (如 "0.0.0.0")
/// @param port number 监听端口
/// @return void
static int l_HttpServer_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* ip = luaL_checkstring(L, 2);
    int port = (int)luaL_checkinteger(L, 3);

    uv_tcp_s* server = PlatformNet::CreateServer(ip, (uint16_t)port);
    if (!server) {
        CC::Log(CC::LOG_ERROR, "HttpServer: CreateServer(%s:%d) failed", ip, port);
        return 0;
    }

    // 存入 Lua 表
    lua_pushlightuserdata(L, server);
    lua_setfield(L, 1, "__server_handle");

    CC::Log(CC::LOG_INFO, "HttpServer: bound on %s:%d", ip, port);
    return 0;
}

/// HttpServer.__tostring
/// 还原自 sub_1800B26B0
static int l_HttpServer_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Network.HttpServer");
    return 1;
}

// ==================== Web 嵌入 Lua 脚本 ====================
// 还原自 luaopen_Light_Network_Web 中的嵌入脚本

static const char s_WebLuaScript[] = R"LUA(
local function defaultAction(header, body)
  local defaultResp = [[
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Hello ChocoLight</title>
</head>
<body>
<h1>It Works!</h1>
<br/>
<br/>
<p><i>Power by ChocoLight<i/></p>
</body>
</html> 
]]
  return 200, defaultResp
end

local Web = {
  clients = {},
  routes = {{}, {}, {}, {}, {}, {}},
}

Web.routes[1]['/'] = defaultAction
Web.routes[2]['/'] = defaultAction
Web.routes[3]['/'] = defaultAction
Web.routes[4]['/'] = defaultAction
Web.routes[5]['/'] = defaultAction

local function contains(table, value)
  for _, v in pairs(table) do
    if v == value then
      return true
    end
  end
  return false
end

local function bind(routes, method, path, callback)
  if type(method) ~= 'number' then
    error('method should be number')
  end
  if not contains({1, 2, 3, 4, 5, 6}, method) then
    error('unknown method')
  end
  if type(callback) ~= 'function' then
    error('callback should be function')
  end
  if routes[method] == nil then
    routes[method] = {}
  end
  routes[method][path] = callback
end

function Web.Open(self, ip, port)
  local server = Light(Light.Network.HttpServer):New(ip, port)
  local clients = self.clients
  local routes = self.routes

  function server:OnConnect(client)
    clients[client:GetFD()] = client
    local route = routes[6]['/join']
    if route == nil then return end
    route(client:GetFD())
  end

  function server:OnClose(client)
    local route = routes[6]['/leave']
    if route == nil then return end
    route(client:GetFD())
    clients[client:GetFD()] = nil
  end

  function server:OnHttp(client, method, url, headers, body)
    if headers["Upgrade"] or headers["upgrade"] then
      return 101, ""
    end
    local route = routes[method]
    if route == nil then return 404, "Page not found!" end
    route = route[url]
    if route == nil then return 404, "Page not found!" end
    return route(headers, body)
  end

  function server:OnWS(client, opCode, message)
    local route = routes[6]
    if route == nil then return end
    route = route['/message']
    if route == nil then return end
    if opCode == 1 then
      route(message, client:GetFD(), false)
    elseif opCode == 2 then
      route(message, client:GetFD(), true)
    end
  end

  self.server = server
  self.clients = clients
  self.routes = routes
  self.server:Open()
end

function Web.Close(self)
  if self.server then
    self.server:Close()
  end
end

function Web.Head(self, path, callback)
  if self ~= Web then error('Call Web:Get instead of Web.Get') end
  bind(self.routes, 1, path, callback)
end

function Web.Get(self, path, callback)
  if self ~= Web then error('Call Web:Get instead of Web.Get') end
  bind(self.routes, 2, path, callback)
end

function Web.Post(self, path, callback)
  if self ~= Web then error('Call Web:Get instead of Web.Get') end
  bind(self.routes, 3, path, callback)
end

function Web.Put(self, path, callback)
  if self ~= Web then error('Call Web:Get instead of Web.Get') end
  bind(self.routes, 4, path, callback)
end

function Web.Delete(self, path, callback)
  if self ~= Web then error('Call Web:Get instead of Web.Get') end
  bind(self.routes, 5, path, callback)
end

function Web.Message(self, callback)
  bind(self.routes, 6, '/message', callback)
end

function Web.Join(self, callback)
  bind(self.routes, 6, '/join', callback)
end

function Web.Leave(self, callback)
  bind(self.routes, 6, '/leave', callback)
end

function Web.Send(self, uId, message, isBin)
  local client = self.clients[uId]
  if client == nil then return end
  if isBin then
    client:SendMessage(message, 2)
  else
    client:SendMessage(message)
  end
end

local Session = {
  tasks = {},
}

local function checkParam(ip, port, path, headers, body, callback)
  if type(ip) ~= 'string' then error('ip should be string') end
  if type(port) ~= 'number' then error('port should be number') end
  if type(headers) ~= 'table' then error('headers should be table') end
  if body ~= nil then
    if type(body) ~= 'string' then error('body should be string') end
  end
  if type(callback) ~= 'function' then error('callback should be function') end
end

function Session.Head(self, ip, port, path, headers, callback)
  checkParam(ip, port, path, headers, nil, callback)
  local task = Light(Light.Network.Http):New(ip, port)
  self.tasks[task:GetFD()] = task
  function task:OnConnect() task:SendRequest(1, path, "") end
  function task:OnHttp(s, h, b)
    callback(s, h, b)
    Session.tasks[self:GetFD()] = nil
    task:Close()
  end
  task:Open()
end

function Session.Get(self, ip, port, path, headers, callback)
  checkParam(ip, port, path, headers, nil, callback)
  local task = Light(Light.Network.Http):New(ip, port)
  self.tasks[task:GetFD()] = task
  function task:OnConnect() task:SendRequest(2, path, "") end
  function task:OnHttp(s, h, b)
    callback(s, h, b)
    Session.tasks[self:GetFD()] = nil
    task:Close()
  end
  task:Open()
end

function Session.Post(self, ip, port, path, headers, body, callback)
  checkParam(ip, port, path, headers, body, callback)
  local task = Light(Light.Network.Http):New(ip, port)
  self.tasks[task:GetFD()] = task
  function task:OnConnect() task:SendRequest(3, path, body) end
  function task:OnHttp(s, h, b)
    callback(s, h, b)
    Session.tasks[self:GetFD()] = nil
    task:Close()
  end
  task:Open()
end

function Session.Put(self, ip, port, path, headers, body, callback)
  checkParam(ip, port, path, headers, body, callback)
  local task = Light(Light.Network.Http):New(ip, port)
  self.tasks[task:GetFD()] = task
  function task:OnConnect() task:SendRequest(4, path, body) end
  function task:OnHttp(s, h, b)
    callback(s, h, b)
    Session.tasks[self:GetFD()] = nil
    task:Close()
  end
  task:Open()
end

function Session.Delete(self, ip, port, path, headers, callback)
  checkParam(ip, port, path, headers, nil, callback)
  local task = Light(Light.Network.Http):New(ip, port)
  self.tasks[task:GetFD()] = task
  function task:OnConnect() task:SendRequest(5, path, "") end
  function task:OnHttp(s, h, b)
    callback(s, h, b)
    Session.tasks[self:GetFD()] = nil
    task:Close()
  end
  task:Open()
end

function Chat(ip, port, path, onJoin, onLeave, onMessage)
  local session = {
    onJoin = onJoin,
    onLeave = onLeave,
    onMessage = onMessage
  }
  local chat = Light(Light.Network.Http):New(ip, port)
  function chat:OnConnect() self:Upgrade(path) end
  function chat:OnClose() onLeave(session) end
  function chat:OnHttp(status, headers, body)
    if headers['Upgrade'] or headers['upgrade'] then
      onJoin(session)
    end
  end
  function chat:OnWS(opCode, message)
    if opCode == 1 then
      onMessage(session, message, false)
    elseif opCode == 2 then
      onMessage(session, message, true)
    end
  end
  table.insert(session, chat)
  function session:Send(message, isBin)
    if isBin then
      chat:SendMessage(message, 2, 1)
    else
      chat:SendMessage(message, 1, 1)
    end
  end
  chat:Open()
  return session
end

Web.Session = Session
Web.Chat = Chat

return Web
)LUA";

// ==================== luaopen 注册 ====================

int luaopen_Light_Network(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Network");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Network");
        lua_createtable(L, 0, 0);

        // __lightHttp 内部连接池 (还原自 sub_1800B0510)
        lua_createtable(L, 0, 0);
        lua_setfield(L, LUA_REGISTRYINDEX, "__lightHttp");

        const luaL_Reg net_funcs[] = {
            {"Resume", l_Network_Resume},
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

// Http 注册 — 8 函数精确匹配 sub_1800B10A0
int luaopen_Light_Network_Http(lua_State* L) {
    // 确保 Network 父表
    luaopen_Light_Network(L);

    lua_pushstring(L, "Http");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Http");
        lua_createtable(L, 0, 0);

        const luaL_Reg http_funcs[] = {
            {"Open",        l_Http_Open},
            {"Close",       l_Http_Close},
            {"SendRequest", l_Http_SendRequest},
            {"SendMessage", l_Http_SendMessage},
            {"Upgrade",     l_Http_Upgrade},
            {"GetFD",       l_Http_GetFD},
            {"__call",      l_Http_Call},
            {"__tostring",  l_Http_Tostring},
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

// HttpServer 注册 — 4 函数精确匹配 sub_1800B2560
int luaopen_Light_Network_HttpServer(lua_State* L) {
    luaopen_Light_Network(L);

    lua_pushstring(L, "HttpServer");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "HttpServer");
        lua_createtable(L, 0, 0);

        const luaL_Reg server_funcs[] = {
            {"Open",       l_HttpServer_Open},
            {"Close",      l_HttpServer_Close},
            {"__call",     l_HttpServer_Call},
            {"__tostring", l_HttpServer_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, server_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "HttpServer");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// Web — 纯 Lua 框架, 嵌入脚本加载
int luaopen_Light_Network_Web(lua_State* L) {
    luaopen_Light_Network(L);

    lua_pushstring(L, "Web");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Web");

        if (!luaL_loadstring(L, s_WebLuaScript) && !lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_rawset(L, -3);
            lua_pushstring(L, "Web");
            lua_rawget(L, -2);
        } else {
            lua_error(L);
        }
    }
    lua_remove(L, -2);
    return 1;
}

#endif // !__EMSCRIPTEN__ && !__ANDROID__
