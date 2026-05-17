# perf_async_loader — Phase G.1.x 量化 Benchmark

> 测量 Shared GL Context 路径下主线程帧时分布（P50/P95/P99/Max），验证 G.1.1 / G.1.2 worker 上传路径是否成功消除主线程尖峰。

## 设计目的

G.1.1 / G.1.2 引入的 worker 直接 GL 上传 + fence 翻状态机制，其核心收益是**主线程从"GPU 同步阻塞"变为"非阻塞 fence check"**。`FINAL_PhaseG_1_1.md § 五` 仅给出了定性预期，本 sample 提供量化数据采集与对比框架。

## 资源准备

仓库不携带测试图（避免增加 git 仓库体积）。需用户自备：

```powershell
# Windows / PowerShell
mkdir samples\perf_async_loader\textures
# 然后复制 ~100 张 PNG / JPG 到该目录, 推荐:
# - 1024x1024 ~ 4096x4096 RGBA
# - 总大小 100MB ~ 1GB (足以让 worker 上传花费可观测量级时间)
```

```bash
# Linux / Mac
mkdir -p samples/perf_async_loader/textures
# 复制资源到该目录
```

**资源建议来源**：
- 任何公开的纹理素材集（如 Poly Haven CC0 textures）
- 项目自身的 game asset (确保版权允许)
- 程序化生成（用 ffmpeg 或 ImageMagick 一行命令造一组 random noise PNG）

例如用 ImageMagick 造 100 张 random PNG：
```bash
for i in $(seq 1 100); do
  convert -size 1024x1024 xc: +noise random samples/perf_async_loader/textures/perf_$i.png
done
```

## 运行

```powershell
# Windows
.\lumen-master\build\src\light\Release\light.exe samples\perf_async_loader\main.lua
```

```bash
# Linux / Mac
./lumen-master/build/src/light/light samples/perf_async_loader/main.lua
```

资源缺失时会打印 `[skip]` + 准备指引并退出。

## 报告示例

```
[perf_async_loader] discovered 5 texture files
[I] AssetLoader: Shared GL Context enabled (worker direct upload + fence)
[I] AssetLoader: worker upload ok (type=1, path=.../perf_1.png) x5
[perf_async_loader] frames=6  avg=2.57ms  P50=2.15ms  P95=4.80ms  P99=4.80ms  Max=4.80ms
[perf_async_loader] loaded 5/5 textures (errors=0)
[perf_async_loader] PASS: P95 < 16.7ms (60fps budget held)
```

> 上述为 NVIDIA RTX (GL 3.3 NVIDIA 560.94, Windows 10) + 5 张 64x64 测试 PNG 的实测数据。
> 真实场景中 N=100+ 张 1024x1024+ 资源时, frames 会显著增加, 但 P95 应保持远低于 16.7ms (60fps budget)。

## 退出行为 (Phase G.1.3 已修复)

之前历史版本在 sample 末尾会触发 `STATUS_STACK_BUFFER_OVERRUN` (exit code `-1073740791`), 根因是 `while UI.Loop() do UI.Resume() end` 写法跳过了引擎清理路径, worker thread 未 join → `std::terminate`。

**Phase G.1.3 修复**: `light_ui.cpp` 中抽 `PerformWindowShutdown_(L)` helper, `l_UI_Loop` 与 `l_UI_Resume` 在任一路径首次检测到 `ShouldClose` 时都调用 (幂等 guard)。现在 sample 干净退出 (exit=0)。

`io.stdout:setvbuf("no")` 仍保留以确保 benchmark 数据立即 flush, 不依赖进程退出时的 buffer flush。

## 解读

| 输出 | 含义 |
|----|----|
| `Shared GL Context enabled` 在启动日志 | G.1.1 probe 成功，走 worker 路径 |
| `fallback to main-thread upload` | probe 失败，走 G.1.0 同步路径（作为对比基线） |
| `P95 < 16.7ms` | 主线程持稳 60fps，worker 路径生效 |
| `P95 > 16.7ms` | 主线程仍有尖峰，可能：driver 共享 ctx 实现差 / GL state 切换开销 / 单帧 dispatch 数过多 |
| `Max` 异常大 | 检查是否有 GL state thrashing 或 fence 等待超时 |

## 对比 G.1.0 / G.1.1 / G.1.2

由于 G.1.1 / G.1.2 是连续演进，sample 默认走最新路径。如需对比：

1. **G.1.1 对比 G.1.0**：手动改 `light_ui.cpp` 把 `AssetLoader::Init(g_mainWindow, g_glContext)` 改成 `AssetLoader::Init(nullptr, nullptr)` 强制 probe 失败 → 重新构建 → 重跑 sample，对比 P95
2. **G.1.2 对比 G.1.1**：本 sample 仅加载 Image，未涉及 Mesh。要量化 Mesh 收益需新建 perf_async_mesh sample（加载 GLTF 而非 PNG）

## 局限性

- **不接入 CI**：需要 GPU + GL 3.3 Core，GitHub Actions windows-latest 无 GPU
- **资源依赖**：用户必须自备 PNG，无法在裸仓库直接跑
- **窗口必须可见**：headless 模式下退出
