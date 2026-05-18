# perf_async_gltf — Phase G.1.5 T6 真机性能测试

异步 GLTF 加载 (Mesh.LoadGLTFAsync withMaterial=true) 主线程帧时分布 benchmark。

## 用途

量化测量 worker shared GL ctx 路径下主线程帧时 P50/P95/P99/Max,
验证 G.1.5 设计性能目标: **主线程 P95 < 16.7ms (60fps budget)**。

与 `samples/perf_async_loader/` (单 PNG image) 区别:
- 走 GLTF 完整路径 (cgltf parse + 1-5 类 material texture + worker upload + fence)
- 走 `with_material=true` 双值返回 + Material userdata 生成
- 主线程 Tick 路径含 5 类 texture WriteSlots

## 用法

```powershell
# 默认: 用 G.1.5 fixture (1×1 PNG quad) 反复加载 100 次, 每帧最多 5 dispatch
.\lumen-master\build\src\light\Release\light.exe samples\perf_async_gltf\main.lua

# 自定义模型 + 配置
.\lumen-master\build\src\light\Release\light.exe samples\perf_async_gltf\main.lua "path/to/DamagedHelmet.glb" 50 1
```

## CLI 参数

| 位置 | 含义 | 默认 |
|------|------|------|
| `arg[1]` | 模型路径 (`.glb` / `.gltf`) | `scripts/smoke/assets_g1_5/test_box_textured.glb` |
| `arg[2]` | 重复加载次数 | `100` |
| `arg[3]` | 每帧最多 dispatch 数 | `5` |

## 输出示例

```
[perf_async_gltf] model=scripts/smoke/assets_g1_5/test_box_textured.glb repeat=100 loads_per_frame=5 max_frames=600
[perf_async_gltf] frames=18 avg=6.84ms P50=4.02ms P95=13.35ms P99=54.53ms Max=54.53ms
[perf_async_gltf] loaded 100/100 (errors=0) avg_load=7.8ms P95_load=13.4ms
[perf_async_gltf] PASS: P95 < 16.7ms (60fps budget held)
```

## 本地基线 (NVIDIA RTX, GL 3.3 Core, Windows 11)

测试模型: G.1.5 fixture (4 顶点 quad + 1×1 红色 PNG, 1192 bytes)
| 配置 | P50 | P95 | P99 | Max | avg_load |
|------|-----|-----|-----|-----|----------|
| `repeat=100, LPF=5` | 4.17ms | 4.79ms | 58.19ms | 58.19ms | 10.8ms |
| `repeat=100, LPF=1` | 4.15ms | 4.37ms | 4.58ms | 54.19ms | 4.7ms |
| `repeat=200, LPF=5` | 4.17ms | 4.48ms | 53.61ms | 53.61ms | 5.9ms |

观察:
- **P50/P95 都在 4-5ms** — 远低于 60fps budget (16.7ms),  worker 路径设计目标达成
- **LPF=1 最干净**: P99=4.58ms (几乎完美 60fps),  适合长生命周期应用
- **Max ~54-58ms** 都是最后一帧 (Future Ready 集中 Tick + WriteSlots 尾巴),  非 steady-state 行为

## 解读

### 成功路径 (Shared GL Context enabled)

- ✅ `P95 < 16.7ms` = worker 路径正确消除主线程尖峰
- ⚠️ `P95 ≥ 16.7ms` = 主线程仍有阻塞, 可能瓶颈点:
  - LPF 过大 (单帧 dispatch + Tick 压力高), 调小为 1-2 试试
  - 5 类 texture WriteSlots 在 Tick 内串行执行
  - Material userdata 创建涉及 Lua 栈管理

### 失败路径 (Fallback to main-thread upload)

- backend `Supports3D=false` (Legacy GL 2.x 或 CI 软件 GL): LoadGLTFAsync 立即返 Error
- sample 报 `loaded 0/N (errors=N)`, P95 数据无意义

## 后续扩展 (TODO)

- **真实大资源测试**: 引入 Khronos GLTF Sample Model (DamagedHelmet, FlightHelmet)
  测 1024×1024 - 4096×4096 PNG decode + upload 全链路性能
- **多模型并发**: 测同时加载不同模型的 worker queue 调度
- **内存峰值**: 测 100 次加载是否触发 image cache 设计需求 (G.1.5 TODO T5)
