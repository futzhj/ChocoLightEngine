# Light.Network.Http

## `Light.Network.Http.Open`

建立 TCP 连接

### 返回值

`void`

---

## `Light.Network.Http.Close`

关闭 TCP 连接

### 返回值

`void`

---

## `Light.Network.Http.SendRequest`

发送 HTTP 请求

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `method` | `string` | HTTP 方法 (GET/POST/PUT/DELETE) |
| `path` | `string` | 请求路径 |
| `body` | `string?` | 请求体 |

### 返回值

`void`

---

## `Light.Network.Http.SendMessage`

发送 WebSocket 消息

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `msg` | `string` | 消息内容 |
| `opcode` | `number?` | 操作码 (1=文本, 2=二进制, 默认 1) |

### 返回值

`void`

---

## `Light.Network.Http.Upgrade`

HTTP 升级到 WebSocket

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string?` | 升级路径 (默认 "/") |

### 返回值

`void`

---

## `Light.Network.Http.GetFD`

获取 socket 文件描述符

### 返回值

`number`

---

## `Light.Network.Http.__call`

构造函数, 创建 HTTP 客户端实例

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `ip` | `string` | 服务器 IP/域名 |
| `port` | `number` | 端口 |

### 返回值

`void`

---
