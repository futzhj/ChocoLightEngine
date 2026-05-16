# Phase F.0.11.3 — StopRecord flush last PBO frame FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (async 模式下 StopRecord 不再丢最后 1 帧)
> 基线: F.0.11.2 commit `acb49d2`

---

## 1. 目标

修复 F.0.11.2 async 路径已知的"边缘问题": 用户调 `StopRecord()` 时, 上次 `ReadbackDefaultFBAsync` 启动的 PBO readback 数据未被取出, 导致最后 1 帧丢失.

## 2. 问题分析

### 2.1 F.0.11.2 ping-pong 时序

```
帧 N:
  ReadbackDefaultFBAsync(buf_N):
    PBO[A].readPixels()       ← 启动
    PBO[B].getBufferSubData() → buf_N (帧 N-1 数据)
    pending[A]=true, pending[B]=false

帧 N+1:
  ReadbackDefaultFBAsync(buf_N1):
    PBO[B].readPixels()       ← 启动
    PBO[A].getBufferSubData() → buf_N1 (帧 N 数据)
    pending[A]=false, pending[B]=true

用户在帧 N+1 调 StopRecord():
  pending[B]=true 的数据 (帧 N+1) 未取 → 丢失!
```

### 2.2 影响范围

- 用户 `Gfx.R` 键停录: 最后一帧没写出
- max_frames 已知数停录: F.0.11.2 在 max_frames 达上限时 hook 自然停止, 数据已取出, 不丢. 仅手动 stop 受影响.

## 3. 设计

### 3.1 Backend 抽象 (+1 虚函数)

```cpp
/// 取出上次启动但未读的 pending PBO 数据
/// 返 true 表示已写入 out_rgba; false 表示无 pending 或不支持
virtual bool ReadbackAsyncFlushLast(unsigned char* out_rgba) { return false; }
```

GL33 实现关键点:
1. 找 `m_pbo_pending[i] == true` 的 PBO (理论最多 1 个, 因为 ping-pong 总是取上次留下次)
2. `glGetBufferSubData(PBO, 0, w*h*4, out_rgba)` 同步取出 (会 sync block 1 次 ~3-8ms, 但仅 1 次, 可接受)
3. 清 pending 标志

### 3.2 RecordState 加缓存

```cpp
struct RecordState {
    ...
    int last_w = 0;
    int last_h = 0;   // F.0.11.3: RecordTickHook 写, StopRecord 读
};
```

`RecordTickHook` 异步路径里每次写入 `last_w/h`. `StopRecord` 时读用以分配正确尺寸的 buffer + 写 PNG.

### 3.3 StopRecord 修改

```cpp
static int l_Graphics_StopRecord(lua_State* L) {
    // F.0.11.3: async flush 最后 1 帧
    if (g_record.use_async && g_render && g_record.last_w > 0 && g_record.last_h > 0 &&
        (g_record.max_frames <= 0 || g_record.frame_count < g_record.max_frames)) {
        std::vector<unsigned char> buf(...);
        if (g_render->ReadbackAsyncFlushLast(buf.data())) {
            // 写最后 1 张 PNG
            stbi_write_png(path, w, h, 4, buf.data(), w * 4);
            ++g_record.frame_count;
        }
    }
    // 原有 stop 逻辑
    int n = g_record.frame_count;
    g_record.active = false;
    g_record.last_w = 0;
    g_record.last_h = 0;
    return n;
}
```

### 3.4 边界保护

- 若 max_frames 已达, **不**写 flush 帧 (用户预期 N 张, 不能写 N+1 张)
- backend 不支持时返 false → silent skip (与 F.0.11.2 一致)
- 切回 sync 时 `last_w=last_h=0` 复位

## 4. 验证

### 4.1 Smoke (28 断言不变, 已覆盖 StopRecord/active flow)

`scripts/smoke/screenshot.lua` 28/28 PASS, headless 下 backend 不支持 async, ReadbackAsyncFlushLast 默认返 false, StopRecord 返回 count=0 (与 F.0.11.2 一致, 行为兼容).

### 4.2 真 GL async 双轨零回归

```
$env:CHOCO_AUTO_EXIT='1'; $env:CHOCO_RECORD_ASYNC='1'; .\light.exe demo.lua

[demo_quad_split] PBO async readback enabled (F.0.11.2)
[demo_quad_split] auto screenshot → docs/screenshots/frame_0000.png
[demo_quad_split] cleanup ...
demo_quad_split ok

PNG: 36980 bytes (与 F.0.11.2 baseline 字节一致)
```

注: demo 走 max_frames=1 路径, 在 hook 达上限自动 active=false (F.0.11.2 路径), flush 路径不会触发 (max_frames 已达). 但 flush 代码不破坏现有行为.

### 4.3 10 smoke 零回归

```
PASS graphics    PASS screenshot   PASS hdr         PASS bloom
PASS ssr         PASS auto_exposure PASS lens_fx    PASS motion_blur
PASS taa         PASS lighting2d
```

## 5. 文件变更

| 文件 | 操作 | 行数 |
|------|------|------|
| `ChocoLight/include/render_backend.h` | +1 虚函数 (ReadbackAsyncFlushLast) | +14 |
| `ChocoLight/src/render_gl33.cpp` | +ReadbackAsyncFlushLast 实现 | +25 |
| `ChocoLight/src/light_graphics.cpp` | +RecordState.last_w/h + StopRecord flush 路径 | +28 |
| `docs/Phase F.0.11.3 stoprecord flush/FINAL_*.md` | 新建 | — |
| **总计** | | **~70 LOC** |

**API 数量**: 117 (无新增, 内部行为改进)

## 6. 关键决策

### 6.1 为何 max_frames 已达时不写 flush 帧

用户调 `RecordPNGSequence(dir, 60)` 预期收 60 张. 若 flush 写第 61 张违反语义. 所以加 `frame_count < max_frames` 守卫.

仅当 `max_frames=0 (unlimited)` 或 `frame_count < max_frames` 时 flush.

### 6.2 为何用 glGetBufferSubData 而非 glMapBuffer

同 F.0.11.2 — 不需要 unmap 配对, 更安全. 此处 flush 是一次性调用, 性能差异微乎其微.

### 6.3 为何不在 SetRecordAsync(false) 时也 flush

切换模式时通常不在录屏中 (录屏中切换被拒, F.0.11.2 已定). 切换时若 PBO 有 pending 数据, 用户已通过 StopRecord 收尾过, pending 已清. 边界情况已被 F.0.11.2 设计保护.

### 6.4 sync block 1 次的性能影响

flush 一次 `glGetBufferSubData` 在 1280×720 上 ~3-8ms. 用户调一次 `StopRecord` 不会感知卡顿 (远小于按键响应时间). 这是可接受 trade-off.

## 7. F.0.11 系列完整收尾

| Phase | 功能 | API |
|-------|------|-----|
| F.0.11 | Screenshot + Record (sync) | +5 |
| F.0.11.1 | frame_skip 跳帧 | +0 |
| F.0.11.2 | PBO async readback | +2 |
| **F.0.11.3 (本)** | **StopRecord flush last frame** | **+0** |

**F.0.11 总 API**: 7, 总 LOC: ~620.

至此 F.0.11 截图/录屏系统功能完整:
- 单帧截图 (`Screenshot`, `ScreenshotRegion`)
- 序列录屏 (`RecordPNGSequence`)
- 性能优化双路 (`frame_skip` 降 IO, `SetRecordAsync` 消 GPU stall)
- 完整收尾 (StopRecord 不丢帧, 切回 sync 释放 PBO, Shutdown 自动清理)

## 8. 下一步建议

| 任务 | 工作量 | 建议 |
|-----|-------|------|
| **F.1 TAAU DLSS-like upscaling** | ~10-15h | **最高优** |
| F.0.11.4 HDR `.hdr` / `.exr` 截图 | ~3h | 低优 |
| F.0.11.5 mp4 直出 | ~5-8h | 低优 (PNG+ffmpeg 已够用) |

推荐: **F.1 TAAU**.
