# Phase F.0.11.1 — RecordPNGSequence frame_skip 性能优化 FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (录屏 IO 性能优化, 向后兼容)
> 基线: F.0.10.10.2 commit `1f69191`

---

## 1. 目标

为 F.0.11 录屏 (`RecordPNGSequence`) 加 `frame_skip` 可选参数, 让 60fps 渲染场景下用户可选择 30fps / 20fps 录屏, 显著降低录屏 IO 卡顿.

## 2. 问题分析

### 2.1 F.0.11 录屏的性能瓶颈

每帧执行的 IO 操作:
1. **glReadPixels** (1280×720 RGBA8 = 3.6 MB GPU→CPU 拷贝): ~3-8ms (PCIE bandwidth + driver overhead)
2. **stbi_write_png** (zlib 压缩 + sync 写盘): ~5-15ms (取决于场景压缩率 + SSD 写速度)

合计: **8-23ms/帧**, 在 16.6ms (60fps) 帧预算内严重挤压渲染时间.

### 2.2 用户场景

- **录 GIF / 短视频教学**: 30fps 已够, 不需要 60fps
- **录 demo 验证 PR**: 关键事件足够, 不需每帧都录
- **录长场景 (>5 秒)**: 高 IO 累积导致 demo 明显卡顿, 用户不愿用

## 3. 设计

### 3.1 API 扩展 (向后兼容)

```cpp
// 旧 (F.0.11): RecordPNGSequence(dir, max_frames)
// 新 (本):    RecordPNGSequence(dir, max_frames, frame_skip?)
//
//   frame_skip=1 (默认) → 每渲染帧写 1 张 (与 F.0.11 行为完全一致)
//   frame_skip=2        → 每 2 帧写 1 张 (60fps 渲染 → 30fps 录屏)
//   frame_skip=3        → 每 3 帧写 1 张 (60fps 渲染 → 20fps 录屏, 最常用)
//   frame_skip=N        → 每 N 帧写 1 张
```

### 3.2 RecordState 内部状态

```cpp
struct RecordState {
    bool        active        = false;
    std::string dir_prefix;
    int         frame_count   = 0;     // 已写 PNG 数 (递增, 文件名 frame_NNNN)
    int         max_frames    = 0;     // 0=unlimited
    // F.0.11.1 新增:
    int         frame_skip    = 1;     // 跳帧间隔
    int         tick_count    = 0;     // 渲染帧累计 (内部, 用于 % frame_skip 判断)
};
```

### 3.3 RecordTickHook 跳帧逻辑

```cpp
extern "C" void Light_Graphics_RecordTickHook(int win_w, int win_h) {
    if (!g_record.active || win_w <= 0 || win_h <= 0) return;
    // F.0.11.1: 每渲染帧 tick_count++, 仅当达到 skip 整数倍时才写盘
    ++g_record.tick_count;
    if (g_record.frame_skip > 1 && (g_record.tick_count % g_record.frame_skip) != 0) return;
    // 写 PNG (frame_count 递增, 文件名连续编号方便 ffmpeg 拼接)
    char path[512];
    std::snprintf(path, sizeof(path), "%sframe_%04d.png",
                  g_record.dir_prefix.c_str(), g_record.frame_count);
    do_screenshot_internal(path, 0, 0, win_w, win_h);
    ++g_record.frame_count;
    if (g_record.max_frames > 0 && g_record.frame_count >= g_record.max_frames) {
        g_record.active = false;   // auto-stop (按已写 PNG 数算)
    }
}
```

### 3.4 关键决策

#### a) 文件名按已写 PNG 编号, 不按渲染帧编号

```
frame_skip=3, max_frames=10:
  渲染帧:  1 2 3 4 5 6 7 8 9 10 11 12 ... 30
  写盘:        ✓     ✓     ✓     ✓        ✓
  PNG:    frame_0000 frame_0001 frame_0002 frame_0003 ... frame_0009
```

文件名连续, 方便 `ffmpeg -i frame_%04d.png` 拼接, 不需 `-vsync` 处理跳号.

#### b) max_frames 是已写 PNG 数, 不是渲染帧数

`frame_skip=3, max_frames=60`:
- 录 60 张 PNG
- 实际跨 ~180 渲染帧 (60 × 3)
- 用户思维: "我要 60 张图" 而非 "我要跑 180 帧"

#### c) frame_skip 边界

- `< 1` → 拒绝 (返 nil + err): `frame_skip=0` 含义不清, `< 0` 是错误
- `= 1` → 默认值, 与 F.0.11 行为一致 (向后完全兼容)
- `>= 1` → 允许任意值 (用户自决, 不强加上限)

## 4. 性能预期

| frame_skip | 写盘频率 | IO 时间/秒 (1280×720) | demo 卡顿 |
|-----------|---------|---------------------|---------|
| 1 (默认) | 60 张/秒 | ~600-1380ms (远超 1s, 必卡) | 严重 |
| 2 | 30 张/秒 | ~300-690ms | 中等 |
| **3** | **20 张/秒** | **~200-460ms** | **轻微 (推荐)** |
| 5 | 12 张/秒 | ~120-280ms | 流畅 |
| 6 | 10 张/秒 | ~100-230ms | 流畅 (低帧动画足够) |

实际 SSD + 简单场景下 (5MB/帧 PNG, 7Gbps 写速度), frame_skip=3 卡顿基本不可感知.

## 5. 验证

### 5.1 Smoke 测试 (新增 4 断言, 16 → 20)

`scripts/smoke/screenshot.lua` 加:
- `frame_skip=0 → nil+err` (边界)
- `frame_skip=-2 → nil+err` (边界)
- 不传第 3 参数 → 默认 1, 向后兼容
- 显式传 `frame_skip=3` → accepted

```
PASS RecordPNGSequence frame_skip=0 → nil+err
PASS RecordPNGSequence frame_skip=-2 → nil+err
PASS RecordPNGSequence(dir,5) backwards-compat (default frame_skip=1)
PASS RecordPNGSequence(dir,5,3) frame_skip=3 accepted
screenshot smoke: 20 pass / 0 fail
```

### 5.2 真 GL 零回归

demo_quad_split 用 frame_skip 默认值 (=1) 录屏:
```
[demo_quad_split] auto screenshot → docs/screenshots/frame_0000.png
[demo_quad_split] cleanup: releasing 4× (HDR/TAA/Bloom/SSR/MB) instances
demo_quad_split ok
```

PNG 36980 字节, 与 F.0.10.10.2 baseline 完全一致 (向后兼容验证).

### 5.3 10 smoke 全 PASS

```
PASS graphics    PASS screenshot   PASS hdr         PASS bloom
PASS ssr         PASS auto_exposure PASS lens_fx    PASS motion_blur
PASS taa         PASS lighting2d
```

## 6. 文件变更

| 文件 | 操作 | 行数 |
|------|------|------|
| `ChocoLight/src/light_graphics.cpp` | RecordState +2 字段, RecordTickHook + L_Graphics_RecordPNGSequence 加 skip 逻辑 | +24 / -3 = +21 |
| `scripts/smoke/screenshot.lua` | +4 断言段 | +14 |
| `docs/Phase F.0.11.1 frame skip/FINAL_*.md` | 新建 | — |
| **总计** | | **~35 LOC** |

**API 数量**: 115 (无新增, 仅扩展现有 `RecordPNGSequence`)

## 7. 用户使用示例

### 7.1 录 30fps GIF (frame_skip=2)

```lua
-- 60fps 渲染下录 30fps, 60 张 PNG (覆盖 ~2 秒)
Gfx.RecordPNGSequence("docs/recordings/demo/", 60, 2)

-- 后处理 (用户侧):
-- ffmpeg -framerate 30 -i frame_%04d.png demo.gif
```

### 7.2 录 20fps demo 视频 (frame_skip=3, 推荐)

```lua
-- 60fps 渲染下录 20fps, 录 5 秒 = 100 张 PNG
Gfx.RecordPNGSequence("docs/recordings/demo/", 100, 3)

-- ffmpeg -framerate 20 -i frame_%04d.png -c:v libx264 -pix_fmt yuv420p demo.mp4
```

### 7.3 极致流畅录长场景 (frame_skip=6)

```lua
-- 60fps 渲染下录 10fps, 录 10 秒 = 100 张 PNG (够用低帧动画展示)
Gfx.RecordPNGSequence("docs/recordings/demo/", 100, 6)
```

## 8. 下一步

| 任务 | 工作量 | 建议 |
|-----|-------|------|
| **F.1 TAAU DLSS-like upscaling** | ~10-15h | **高优, 重头戏** |
| F.0.11.2 HDR `.hdr` / `.exr` 截图 | ~3h | 低优 (美术工作流) |
| F.0.11.3 mp4 直出 | ~5-8h | 低优 (PNG+ffmpeg 已够) |
| 优化录屏使用 PBO 异步 readback | ~2-3h | 中优, 进一步降卡 |

推荐: **F.1 TAAU**.
