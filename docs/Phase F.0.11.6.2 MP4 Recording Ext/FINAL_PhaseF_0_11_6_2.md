# Phase F.0.11.6.2 — MP4 录屏功能扩展 (FINAL)

> **交付日期**：2026-05-18
> **基线**：Phase F.0.11.6.1 (MP4 worker thread + A1~A7 性能闭环)
> **commits**：`2ef81ea` — Phase F.0.11.6.2 (A8 + A10 + A11 + A12)
> **author**: Cascade-assisted refactor

---

## 一. 目标

在 F.0.11.6.1 的 MP4 编码 worker 基础上, 补齐录屏的**可控性 / 可观测性**功能, 让 mp4 录屏从 "能录" 升级到 "可用于生产":

| 子任务 | 价值 | 用户场景 |
|--------|------|----------|
| **A8 关键帧间隔 (`gop_size`)** | 高 | 视频剪辑 seek 粒度 / 网络推流关键帧间隔 / 全 I 帧高质量归档 |
| **A10 Pause / Resume** | 中 | 调试时暂停录屏 (调试器命中断点不污染录像), 多段录制中间停顿 |
| **A11 max_size_bytes 上限** | 中 | 长时间录屏不爆盘, 脚本可决定何时切分文件 |
| **A12 GetRecordStats** | 高 | OSD 显示真实进度 (帧数/字节/编码器), 脚本判断录屏健康度 |

整体工作量 ~2h, 一个 commit 完成. 复杂的 A9 ROI 录屏 (要改 readback 接口) 和 A13 多轨音频 (跨平台 audio capture) 留后续 Phase.

---

## 二. 交付内容

### 2.1 A8 — GOP / 关键帧间隔 (`gop_size`)

#### 问题
F.0.11.6.1 codec_ctx 固定 `g = fps * 2` (2 秒 1 关键帧). 三类用户需求覆盖不了:
- **剪辑工作流**: 需要更密集关键帧, 才能在剪辑软件里逐帧 seek; 当前 60fps × 2s = 120 帧 GOP, 拖动时间轴会卡
- **网络推流**: RTMP / HLS 要求 1~2 秒 1 关键帧 (与 fps 无关, 固定秒数)
- **高质量归档**: 想要全 I 帧 (gop=1), 文件大但任意 seek

#### 方案
- `RecordMP4::Open` 增 `int gop_size = 0` 参数 (尾参数, 默认 0 兼容旧调用)
- `gop_size > 0` → 直接用; `gop_size <= 0` → fps × 2 (旧行为)
- `gop_size = 1` → 全 I 帧 (`avcodec_open2` 内部自动调整)
- Lua `RecordMP4(path, {gop_size = N})` 解析此字段

#### 代码
- `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h:35-42` (header 增参数 + 文档)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp:396-400` (effective_gop 计算)
- `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5503-5528` (Lua opts 解析)

#### Lua 示例
```lua
-- 默认行为 (fps×2 = 60 帧 @ 30fps)
Gfx.RecordMP4("default.mp4", { fps = 30 })

-- 高频关键帧 (15 帧 = 0.5 秒, 适合剪辑)
Gfx.RecordMP4("editing.mp4", { fps = 30, gop_size = 15 })

-- 全 I 帧 (适合无损归档, 文件大)
Gfx.RecordMP4("archive.mp4", { fps = 30, gop_size = 1, bitrate = 50000000 })
```

---

### 2.2 A10 — Pause / Resume 录制控制

#### 问题
- 调试时录屏: 命中断点 → 调试器卡几分钟 → mp4 时间线被污染 / 主线程 fps 骤降 → 视频卡顿
- 演示录屏: 想暂停一下回顾设置, 不希望录屏继续

#### 方案
- `State::paused` (`std::atomic<bool>`) 受**独立**于 `stop_flag`/`active` 的语义控制
- `PauseRecord()` 设 `paused = true`; `ResumeRecord()` 设 `false`
- 生效路径: `light_graphics::Light_Graphics_RecordTickHook` 顶部检查 `RecordMP4::IsPaused()`:
  ```cpp
  if (g_record.mode == 1 && RecordMP4::IsPaused()) return;
  ```
  → 暂停期间不调 `Acquire`/`Commit`, ring 不进新帧, pts 不前进, worker 编码线程仅 drain 已入队帧
- **pts 不前进 = 时间线无缝衔接**: 暂停 10 秒后恢复, mp4 内的时间戳是连续的, 类似剪辑里"跳过空白段"

#### 与 stop_flag 的独立性
| 状态 | active | paused | stop_flag | 行为 |
|------|--------|--------|-----------|------|
| 正常录 | true | false | false | RecordTickHook 走 Acquire/Commit |
| 暂停 | true | true | false | RecordTickHook 顶部 return, worker idle |
| 主线程要停 | true | * | true | worker drain 完后 join + Close |
| 已停 | false | false | false | RecordTickHook 顶部 return |

#### 代码
- `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h:84-95` (Pause/Resume/IsPaused 接口)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp:725-737` (实现, 3 个函数共 13 行)
- `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5260-5263` (RecordTickHook 顶部分支)
- `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5719-5756` (3 个 Lua API)

#### Lua 示例
```lua
Gfx.RecordMP4("demo.mp4", { fps = 60 })
-- ... 录 5 秒 ...
Gfx.PauseRecord()
-- 调试 / 设置场景 / 等用户输入 (这段时间不入 mp4)
Gfx.ResumeRecord()
-- ... 继续录 5 秒 ...
Gfx.StopRecord()
-- 输出 mp4 总时长 ≈ 10 秒 (暂停期不算)
```

---

### 2.3 A11 — `max_size_bytes` 文件大小上限

#### 问题
- 长时间录屏 (例如游戏 speedrun 直播 / 自动化测试) 可能录数小时
- 5 Mbps × 1 小时 = 2.25 GB; 没上限时容易把磁盘录爆
- 但**自动停可能丢精彩瞬间**, 让脚本决定何时切分更合理

#### 方案
- `SetMaxSizeBytes(int64_t)`: Open 后任何时刻可设, 0 = 无限 (默认)
- `bytes_written`: worker 在 `av_interleaved_write_frame` 后累加 `packet.size` (受 `mu` 保护, 与 `GetStats` 读路径同锁)
- **不自动停**: 仅暴露给脚本; 由脚本通过 `GetRecordStats()` 查 `bytes`, 自行决定 `StopRecord` + 重新 `RecordMP4` 切下一段
- Close 阶段的 flush packet 也累加 `bytes_written`, 让 `GetStats` 反映最终值

#### 为什么不自动停?
1. **业务复杂性**: 切到下一段时需要 fade-out / 接力 PNG / 通知 ImGui 等, 引擎层无法替业务决定
2. **死锁风险**: worker 自动停时主线程可能正在 `Acquire`, 双方互等
3. **可预测性**: 用户脚本看到 `bytes >= max_bytes` 自己处理, 比"突然 IsRecording=false"更可控

#### 代码
- `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h:97-100` (SetMaxSizeBytes 接口)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp:739-742` (实现, 4 行)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp:632-637` (worker EncodeFrameInternal_ 累加)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp:757-762` (Close flush 累加)
- `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5758-5769` (Lua API)

#### Lua 示例
```lua
Gfx.RecordMP4("seg_001.mp4", { fps = 30, bitrate = 5000000 })
Gfx.SetRecordMaxSize(500 * 1024 * 1024)  -- 500 MB

local segment = 1
while true do
    coroutine.yield()  -- 主循环 tick
    local s = Gfx.GetRecordStats()
    if s.max_bytes > 0 and s.bytes >= s.max_bytes then
        Gfx.StopRecord()
        segment = segment + 1
        Gfx.RecordMP4(string.format("seg_%03d.mp4", segment), { fps = 30, bitrate = 5000000 })
        Gfx.SetRecordMaxSize(500 * 1024 * 1024)
    end
end
```

---

### 2.4 A12 — `GetRecordStats` 录屏统计

#### 问题
- F.0.11.6.1.A7 OSD 只能显示 "REC" 闪烁点, 没有真实帧数/字节数
- 脚本想做自定义 OSD 或日志, 没渠道查 worker 内部状态

#### 方案
- C++ API: `GetStats(out_frames, out_bytes, out_max_bytes, out_enc_name, name_cap, out_paused)` 一次拿全
- Lua API: `Gfx.GetRecordStats()` 返 table:
  ```lua
  {
      mode = 1,           -- 0=PNG, 1=mp4
      active = true,
      tick_frame_count = 150,   -- 主线程 push 计数 (含暂停前)
      frames = 145,             -- worker 已编码并写盘 (B-frame flush 后)
      bytes = 18500000,         -- 已写入 mp4 的总字节
      max_bytes = 524288000,    -- SetRecordMaxSize 设定值 (0=无限)
      encoder = "h264_nvenc",   -- 实际打开的编码器名 (Open 时由 A3/A5 决定)
      paused = false,
  }
  ```
- 线程安全: `frames/bytes` 加锁读 (与 worker 写路径同 `mu`)
- 性能: 单次 GetStats 加锁 < 1μs, 每帧调用无压力

#### 代码
- `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h:102-111` (接口 + 文档)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp:744-772` (实现 29 行)
- `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5771-5810` (Lua 包装 + table 构造)

---

## 三. API 增量汇总

### Lua API (新增 6 个)
| API | 入参 | 返回 | 备注 |
|-----|------|------|------|
| `Gfx.RecordMP4(path, opts.gop_size)` | `integer?` | (透传 RecordMP4 返回) | A8: 0=默认 fps×2 |
| `Gfx.PauseRecord()` | — | `true` 或 `nil, err` | A10: 仅 mp4 模式生效 |
| `Gfx.ResumeRecord()` | — | `true` 或 `nil, err` | A10: 配对 |
| `Gfx.IsRecordPaused()` | — | `boolean` | A10: 未录 / PNG 模式都返 false |
| `Gfx.SetRecordMaxSize(bytes)` | `integer` | `true` | A11: 0=无限, 负值归 0 |
| `Gfx.GetRecordStats()` | — | `table` | A12: 详见上 |

### C++ API (新增 5 个 + 1 个签名扩展)
| API | 备注 |
|-----|------|
| `RecordMP4::Open(..., int gop_size = 0)` | **A8 签名扩展** (尾参数, 默认 0) |
| `RecordMP4::PauseRecord()` | A10 |
| `RecordMP4::ResumeRecord()` | A10 |
| `RecordMP4::IsPaused()` | A10 |
| `RecordMP4::SetMaxSizeBytes(int64_t)` | A11 |
| `RecordMP4::GetStats(...)` | A12 |

---

## 四. 验证矩阵

| 测试 | 状态 | 备注 |
|------|------|------|
| 编译 (Release, /W3) | ✅ 零 warning | record_mp4.cpp + light_graphics.cpp |
| smoke `screenshot` | ✅ **61 PASS / 0 FAIL** | 含 19 个新 F.0.11.6.2 用例 (API surface + 边界 + 默认行为) |
| smoke `asset_loader_async_gltf` | ✅ 14 PASS | G.1.5 全套不破坏 |
| smoke `hdr` | ✅ 141 PASS | 渲染管线无影响 |
| smoke `window_lifecycle` | ✅ 2 PASS | 窗口生命周期不影响 |
| smoke `asset_loader_async` | ✅ 2 PASS | — |
| smoke `mesh_3d` | ✅ 0 fail | — |
| smoke `asset_loader_async_probe` | ✅ 1 PASS | — |
| **真机录屏验证** | ⏳ 待用户验证 | 见 §五 |

### 新增 smoke 用例 (`@e:\jinyiNew\Light\scripts\smoke\screenshot.lua:121-161`)
- API surface 检查: 5 个新函数存在
- A10: 未录屏时 `PauseRecord` / `ResumeRecord` → nil+err
- A10: 未录屏时 `IsRecordPaused` = false
- A11: `SetRecordMaxSize` 接受 0 / 100MB / 负值 (归 0) 均成功
- A12: `GetRecordStats` 未录屏 → table 且 7 个字段齐全 (active=false, mode=0, frames=0, bytes=0, paused=false, encoder=string)
- A8: `RecordMP4(opts.gop_size=1)` 解析成功 (headless 仍正常返 nil+err 因为无窗口)

---

## 五. 真机验证步骤 (用户操作)

```powershell
# 1. 启动 demo_taau
e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe `
  e:\jinyiNew\Light\samples\demo_taau\main.lua

# 2. 在 Lua 控制台或 demo 脚本内测试 (举例):
local Gfx = Light.Graphics

-- A8: gop_size = 15 (高频关键帧)
Gfx.RecordMP4("test_gop.mp4", { fps = 30, gop_size = 15, bitrate = 5000000 })
-- 录 5 秒后:
Gfx.StopRecord()
-- 验证: ffprobe test_gop.mp4 | grep "key_frame" → 应该每 15 帧 1 关键帧

-- A10: Pause/Resume
Gfx.RecordMP4("test_pause.mp4", { fps = 30 })
-- 录 3 秒
Gfx.PauseRecord()
-- 等 5 秒 (mp4 时间不前进)
Gfx.ResumeRecord()
-- 再录 3 秒
Gfx.StopRecord()
-- 验证: mp4 总时长 ≈ 6 秒 (3+3, 不含 5 秒暂停)

-- A12: GetRecordStats 实时查
Gfx.RecordMP4("test_stats.mp4", { fps = 30, encoder = "libx264" })
local s = Gfx.GetRecordStats()
print("encoder:", s.encoder)   -- 应输出 "libx264"
-- 录几秒后再查
local s2 = Gfx.GetRecordStats()
print("frames:", s2.frames, "bytes:", s2.bytes)
Gfx.StopRecord()
```

---

## 六. 文件变更

| 文件 | 增 | 删 | 备注 |
|------|----|----|------|
| `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h` | +37 | -3 | gop_size 参数 + 5 个新 API |
| `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp` | +135 | -7 | State 字段 + 5 实现 + worker stats 累加 + 桩函数 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +125 | 0 | Lua opts gop_size + 5 个新 Lua API + RecordTickHook Paused 检查 |
| `@e:\jinyiNew\Light\scripts\smoke\screenshot.lua` | +44 | 0 | 19 个新 PASS 用例 |
| **总计** | **+341** | **-10** | 一个 commit |

**commit**: `2ef81ea` (in `main`)

---

## 七. 后续候选 (Phase F.0.11.6.3+)

### ✅ A9 — ROI 录屏 (已完成 2026-05-18, commit `3bb7d3b`)
- 需求: 录某个 UI 元素 / 局部 debug, 不录整屏
- 实现策略 (比预估简单, ~1.5h 完成):
  - `ReadbackDefaultFB` 接口已支持 x/y/w/h 参数, PBO 自适应 w/h → 不需改 backend
  - 仅在 `light_graphics::g_record` 加 `roi_x/y/w/h` + 抽 helper `calc_readback_region_`
  - Lua: `opts.roi = { x, y, w, h }` (屏幕左上原点, libx264 偶数对齐自动 round down)
  - Open 时 mp4 尺寸 = ROI 尺寸; Open 失败若 ROI 越界 (早失败比 silent skip 友好)
  - 运行中 ROI 越界 (窗口缩小) → CancelWriteSlot + warn, mp4 帧序不断
- 代码:
  - `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5241-5248` (RecordState ROI 字段)
  - `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5263-5289` (`calc_readback_region_` helper)
  - `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5309-5366` (RecordTickHook mp4 分支用 ROI)
  - `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5575-5587` (Lua opts.roi 解析)
  - `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:5633-5648` (Open ROI 越界检查 + mp4 尺寸用 ROI)
- smoke: `@e:\jinyiNew\Light\scripts\smoke\screenshot.lua:163-181` (+4 用例: 合法 / 非 table / 部分字段 / 不传 roi)
- Lua 示例:
  ```lua
  Gfx.RecordMP4("ui.mp4", { fps=30, roi = { x=100, y=50, w=640, h=480 } })
  -- mp4 输出尺寸 = 640x480, 仅录屏幕 [100,50 ~ 740,530] 区域
  -- 不传 roi = 录全屏 (兼容旧行为)
  ```

### A13 — 多轨音频 (高价值, 高工作量)
- 需求: 录屏带声音是用户最直接体感
- 跨平台 audio capture:
  - Windows: WASAPI loopback (从扬声器抓输出) + DirectSound (麦克风)
  - macOS: CoreAudio + ScreenCaptureKit (12.3+)
  - Linux: PulseAudio monitor / PipeWire
- FFmpeg 侧: 加 AAC encoder + audio AVStream + audio/video sync (DTS/PTS)
- 工作量: ~8h (含跨平台桥接)

### A14 — 录屏 GIF 输出 (低工作量, 中价值)
- 复用现有 PNG 序列, libgif-wrapper 转 GIF
- 适合做小型 demo / Slack 分享
- 工作量: ~2h

### F.1.2 — Velocity Nearest-Filter (条件性)
- 仅当 F.1.1 mipmap LOD bias 真机测试 ghost 严重时启用
- 当前未验证, 不推荐盲做

---

**author**: Cascade-assisted refactor
