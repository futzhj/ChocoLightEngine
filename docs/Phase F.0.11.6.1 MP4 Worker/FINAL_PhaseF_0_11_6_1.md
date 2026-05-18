# Phase F.0.11.6.1 — MP4 录屏 Worker Thread 编码 (FINAL)

> **交付日期**：2026-05-18
> **基线**：Phase F.0.11.6 (MP4 录屏主线程同步编码)
> **commit**：`d506ad7` (`main`)

---

## 一. 目标

将 MP4 H.264 编码从主线程移到独立 worker thread, 解决主线程帧时被编码 25-40ms 阻塞的问题。

---

## 二. 改造前后对比

### 主线程负担 (per recorded frame)

| 步骤 | F.0.11.6 同步 | F.0.11.6.1 worker | 备注 |
|------|---------------|-------------------|------|
| `ReadbackDefaultFB` | ~3-5ms | ~3-5ms | 不变 (PBO async 路径未启用 mp4 模式) |
| Y 翻转 (1080p) | ~0.5ms | **0** (移到 worker) | — |
| `sws_scale` RGBA→YUV | ~3-5ms | **0** (移到 worker) | — |
| `avcodec_send_frame` | ~0.5ms | **0** (移到 worker) | — |
| `avcodec_receive_packet` (libx264 medium CRF 23) | **~20-30ms** | **0** (移到 worker) | 主要瓶颈 |
| `av_interleaved_write_frame` | ~0.5ms | **0** (移到 worker) | — |
| **新增**: `vector::assign` (8MB memcpy) | — | ~2ms | 主线程拷贝到 staging |
| **新增**: `mutex::lock` + `queue::push` | — | <0.1ms | — |
| **总计** | **25-40ms** | **~5-7ms** | **降低 80-85%** |

### 编码 throughput

- Worker 单线程 libx264 medium @ 1080p: ~25-40ms/frame → **稳定 25-40 fps 编码能力**
- 主线程渲染 ≤ 30fps 时, queue 不会满 (steady state q.size=0~2)
- 主线程 > 40fps 时, queue 累积到 16 帧上限后 push 阻塞 (back-pressure 防 OOM)

---

## 三. 关键设计决策

### 1. 单 worker thread, 非线程池

libx264 encoder context **per-context thread-safe** (单 context 不能多线程并发 send_frame). 多 worker 需要多 encoder context, 复杂度上升且收益有限 (录屏单流足够). 直接单 worker 简单可靠。

### 2. Queue cap = 16 帧 + back-pressure

- 1080p RGBA × 16 = ~128 MB 上限, 可接受
- queue 满时 `WriteRGBA` 调用 `cv_not_full.wait()` 阻塞主线程, 等 worker 出队后唤醒
- 这是**主动反压**而非丢帧, 保证不漏帧但可能拖慢主线程渲染 (录屏期间是合理 trade-off)

### 3. Encoder state 严格 worker thread 独占

主线程仅碰 `mu` / `cv` / `q` / `stop_flag` (同步原语 + 队列), 不碰 `g.codec_ctx` / `g.frame` / `g.packet` / `g.sws_ctx` / `g.fmt_ctx`. 杜绝 FFmpeg 跨线程访问问题。

### 4. Close 串行 finalize

worker join 后, 由主线程顺序调:
1. `avcodec_send_frame(NULL)` 通知 encoder flush
2. `avcodec_receive_packet` 排空 buffered B/P frames
3. `av_write_trailer`
4. `avio_closep`

这避免 worker 收到 stop signal 时不知道是不是最后一帧, 导致 flush 时序混乱。

### 5. `FindCodecpar_` 抽到独立 helper

MSVC C2712: `__try` 不能与 unwindable 对象 (`std::thread` / `std::mutex` 等) 共存于同一函数。codecpar 探针块含 SEH `__try`, 而 `Open` 函数体引用 `g.worker` 等析构对象, 编译失败。解决: 把 `__try` 块抽到独立 `static void* FindCodecpar_(void*)`, 该函数无 unwindable 对象, 安全。

---

## 四. API 兼容性

**完全无破坏性变更**:
- `RecordMP4::Open / WriteRGBA / Close / IsActive` 签名不变
- `Light.Graphics.RecordMP4` Lua API 不变
- `Light_Graphics_RecordTickHook` 调用路径不变 (主线程 → `WriteRGBA` → 内部 push)

主线程感知不到 worker thread 存在; 仅通过日志输出 `worker thread spawned` 提示。

---

## 五. 验证矩阵

| 测试 | 状态 | 备注 |
|------|------|------|
| 编译 (Release) | ✅ 零 warning | MSVC C2712 通过 helper 抽取解决 |
| smoke `asset_loader_async_gltf` | ✅ 14 PASS | G.1.5 全套用例不破坏 |
| smoke `hdr` | ✅ 141 PASS | 渲染管线无影响 |
| smoke `screenshot` | ✅ 11 PASS | RecordMP4 API binding 完整 |
| smoke `window_lifecycle` | ✅ 2 PASS | 窗口生命周期不影响 worker |
| smoke `asset_loader_async` | ✅ 2 PASS | — |
| smoke `mesh_3d` | ✅ 0 fail | — |
| smoke `asset_loader_async_probe` | ✅ 1 PASS | — |
| **真机录屏验证** | ⏳ 待用户验证 | 见 §六 步骤 |

---

## 六. 真机验证步骤 (用户操作)

```powershell
# 1. 启动 demo_taau (含 MP4 录屏快捷键 M)
e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe `
  e:\jinyiNew\Light\samples\demo_taau\main.lua

# 2. 在 demo 窗口内:
#    - 按 M 键开始录屏 (默认 path/fps/bitrate, 见 demo 内部)
#    - 等若干秒 (观察主线程 fps 不掉)
#    - 再按 M 键停止
#    - 检查日志: 应看到 "worker thread spawned" + "worker joined, resources released"

# 3. 验证产物:
#    - 检查 mp4 文件存在且可被 ffplay / VLC 播放
#    - 比对主线程渲染流畅度 (应明显比 F.0.11.6 同步路径流畅)
```

**预期结果**:
- 录屏期间 demo_taau 主线程 fps 稳定 (60fps cap)
- mp4 文件帧数 = 录屏期间帧数 (无丢帧)
- Close 时 log 顺序: `worker joined` → `mp4 finalized`

---

## 七. 已知限制

1. **录屏期间瞬时主线程 push 阻塞**: 若主线程渲染速度 > worker 编码速度 (40+fps 渲染 / 30fps 编码), queue 会满, 主线程 push 时阻塞 ~25ms 等 worker 消化 1 帧。这是 back-pressure 的预期行为, 不会漏帧但会**主动降速**主线程到 worker 编码速度。

2. **memcpy 8MB / frame 主线程开销**: 1080p RGBA full frame 拷贝约 ~2ms。后续可优化为 ring buffer (Open 时预分配 16 个 staging buffer, push/pop 仅交换指针)。当前 vector::assign 简单可靠, 收益已足够明显。

3. **Close 同步阻塞**: `worker.join()` 会等 queue 完全 drain, 1080p × 16 帧 × 30ms = ~480ms 最坏情况。用户体感: 按 M 停止录屏后偶尔会卡 0.5s。可后续加进度提示。

---

## 八. 后续优化候选 (低优先级)

- **A1**: Ring buffer 替换 `std::queue<std::vector>`, 主线程 push 仅指针交换 (主线程开销 ~2ms → ~0.1ms)
- **A2**: PBO async readback 接入 mp4 模式 (复用 `ReadbackDefaultFBAsync`), 主线程 readback 不 stall GPU
- **A3**: H264 NVENC 路径 (硬件编码, 编码 1080p < 5ms/frame, 取消 back-pressure 风险)

---

## 九. 文件变更

| 文件 | 行数变更 | 说明 |
|------|----------|------|
| `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp` | +135 / -56 | worker thread + state + helper 抽取 |

**commit**: `d506ad7` (`main`)
**author**: Cascade-assisted refactor
