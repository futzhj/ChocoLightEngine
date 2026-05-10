# demo_udp_echo

Phase BC 网络层端到端 demo. 同一 `main.lua` 通过 `arg[1]` 切换 server / client 模式, 验证以下功能:

| 功能 | 阶段 | 验证点 |
|------|------|--------|
| `PlatformNet` ENet | T5 | 双进程 UDP 联通 |
| `Rpc.Listen / Rpc.Connect` + `client:Call` | T6 | RPC `Echo` 请求-响应 |
| `Room.Host / Room.Join` | T8 | Hello 握手 + State 同步 |
| `client:Call(..., timeout_ms)` | **v2** | `Sleep` 故意慢响应, 100ms 触发 `-32001` |
| `room:PatchState` | **v2** | 中途 set / set+delete patch, 客户端 OnState 触发 |
| `room:Kick(peer_id, reason)` | **v2** | 5s 后 kick, 客户端 OnKick 收到 reason |

## 一键启动

### Windows (PowerShell)

```powershell
# 终端 A — server
./light.exe samples/demo_udp_echo/main.lua server

# 终端 B — client
./light.exe samples/demo_udp_echo/main.lua client
```

### Linux/macOS

```bash
# 终端 A
./light samples/demo_udp_echo/main.lua server &

# 终端 B
./light samples/demo_udp_echo/main.lua client
```

## 自定义端口 / 远程 host

```
server: light samples/demo_udp_echo/main.lua server 9100
client: light samples/demo_udp_echo/main.lua client 192.168.1.10 9100
```

端口 P 用于 Room, P+1 用于 RPC. 默认 9001 / 9002.

## 预期输出

### Server
```
[server] starting on port=9001 (RPC 9002)
[server] ready, running for ~10s...
[server room] join pid=0 name=demo_user
[server rpc] connect peer=0
[server] PatchState set: score=10, lastEvent=first_patch
[server] PatchState set+del: score=20, -lastEvent
[server] Broadcast round_start to 1 peer(s)
[server] Kick pid=0 ok=true
[server room] leave pid=0
[server] done
```

### Client
```
[client] connecting to 127.0.0.1:9001 (RPC 9002)
[client rpc] connect
[client room] ready
[client room] state rev=1 score=0 lastEvent=nil round=1
[client] Echo result: echoed=hello phase BC server_time=...
[client] Sleep TIMEOUT fired correctly (-32001)
[client room] state rev=2 score=10 lastEvent=first_patch round=1
[client room] state rev=3 score=20 lastEvent=nil round=1
[client room] event round_start args=2
[client room] KICKED with reason: demo finished, bye
[client] done
```

## 已知限制

- 单机回环测试. 跨网络要保证 UDP 双向可达 (NAT 穿透不在此 demo 范畴, 见 Phase BC TODO 2.4 WebRTC 桥)
- 不支持 Web 平台 (浏览器无 raw UDP)
- 双进程必须按顺序启动: 先 server 后 client (默认 client 启动 200ms 内必须收到 connect)
