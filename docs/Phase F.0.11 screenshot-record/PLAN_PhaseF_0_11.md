# Phase F.0.11 — Screenshot / PNG Sequence Record (PLAN)

> 6A · Align + Architect + Atomize
> 基线: F.0.10.9.x.4 commit `1224494` (110 Lua API)
> 目标 Lua API: 110 → **115** (+5)

---

## 1. 目标

为 demo 视觉验收 + multi-instance 效果展示加 截图 / 录屏 能力:

1. 单帧截图 (PNG, full screen / region)
2. 录屏 (PNG sequence + 用户自行 ffmpeg 拼 mp4)
3. 录屏状态查询

## 2. 用户场景

```lua
-- 一: 单帧截图
Light.Graphics.Screenshot("docs/screenshots/quad_split.png")

-- 二: region 截图 (单 quad)
Light.Graphics.ScreenshotRegion("Q0_TL.png", 0, 360, 640, 360)

-- 三: 录屏 (PNG 序列, 后处理 ffmpeg 拼 mp4)
Light.Graphics.RecordPNGSequence("docs/recordings/quad/", 120)  -- 录 120 帧 (~2s @ 60fps)
-- 引擎自动在每帧 EndFrame 后 readback default fb → 写 frame_NNNN.png
-- 写到 120 帧自动 stop

-- 四: 主动 stop
Light.Graphics.StopRecord()

-- 五: 状态查询
local active, count = Light.Graphics.IsRecording()
```

后处理 (用户侧):
```bash
ffmpeg -framerate 60 -i frame_%04d.png -c:v libx264 -pix_fmt yuv420p out.mp4
```

## 3. 设计

### 3.1 backend 层 (1 新虚函数)

`render_backend.h` 加:
```cpp
/**
 * @brief Phase F.0.11 — 同步读 default framebuffer 到 RGBA8 buffer
 * @param x, y         读取起点 (左下原点, OpenGL 习惯)
 * @param w, h         读取宽高
 * @param out_rgba     调用方分配, 至少 w*h*4 字节
 * @return true=成功, false=不支持/参数非法/glGetError
 */
virtual bool ReadbackDefaultFB(int /*x*/, int /*y*/, int /*w*/, int /*h*/,
                                unsigned char* /*out_rgba*/) { return false; }
```

GL33 实现 (`render_gl33.cpp`):
```cpp
bool ReadbackDefaultFB(int x, int y, int w, int h, unsigned char* out_rgba) override {
    if (w <= 0 || h <= 0 || !out_rgba) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    return glGetError() == GL_NO_ERROR;
}
```

### 3.2 截图实现 (light_graphics.cpp)

```cpp
// 内部 helper (复用)
static int do_screenshot(const char* path, int x, int y, int w, int h) {
    if (!g_render || w <= 0 || h <= 0) return 0;
    std::vector<unsigned char> buf((size_t)w * h * 4);
    if (!g_render->ReadbackDefaultFB(x, y, w, h, buf.data())) return 0;
    // OpenGL 左下原点, PNG 左上 → 写时翻转
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path, w, h, 4, buf.data(), w * 4);
}

// Lua: Screenshot(path)
static int l_Graphics_Screenshot(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int w = 0, h = 0;
    PlatformWindow::GetSize(g_mainWindow, &w, &h);  // 全屏读
    if (do_screenshot(path, 0, 0, w, h)) {
        lua_pushboolean(L, 1); return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "Screenshot failed: %s", path);
    return 2;
}
```

### 3.3 录屏 (PNG 序列, 在 EndFrame 后 hook)

全局录屏状态 (single recorder, 多次 start 后续覆盖):
```cpp
struct RecordState {
    bool        active        = false;
    std::string dir_prefix;             // "frames/"
    int         frame_count   = 0;      // 已写帧数
    int         max_frames    = 0;      // 最大帧数 (0 = unlimited)
    int         start_frame   = 0;      // 起始帧编号 (允许多次 record 续编号)
};
static RecordState g_record;
```

在 `light_ui.cpp::l_Window_Call` 的 EndFrame 之后, SwapBuffers 之前 hook:
```cpp
if (g_record.active) {
    int w = 0, h = 0;
    PlatformWindow::GetSize(g_mainWindow, &w, &h);
    char path[512];
    snprintf(path, sizeof(path), "%sframe_%04d.png",
             g_record.dir_prefix.c_str(), g_record.frame_count);
    // 复用 Screenshot 路径
    do_screenshot_internal(path, 0, 0, w, h);
    ++g_record.frame_count;
    if (g_record.max_frames > 0 && g_record.frame_count >= g_record.max_frames) {
        g_record.active = false;   // auto-stop
    }
}
```

### 3.4 5 Lua API

| API | 签名 | 返回 |
|-----|------|------|
| `Screenshot(path)` | path:string | true / nil+err |
| `ScreenshotRegion(path, x, y, w, h)` | string + 4 int | true / nil+err |
| `RecordPNGSequence(dir, max_frames)` | string + int | true / nil+err |
| `StopRecord()` | — | int (写入帧数) |
| `IsRecording()` | — | bool, int |

## 4. 不在本 phase 范围

- mp4 直出 (复杂度高 + FFmpeg encoder 依赖, 留 F.0.11.1)
- 异步 readback (PBO double-buffer 优化, 留性能优化)
- HDR (RGBA16F) 截图 (.hdr / .exr 输出, 留 F.0.11.2)

## 5. Atomize (子任务)

| 子任务 | 工作量 | 依赖 |
|-------|-------|------|
| **A1** RenderBackend.h 加 ReadbackDefaultFB 虚函数 | 5min | 无 |
| **A2** GL33 实现 ReadbackDefaultFB | 10min | A1 |
| **A3** light_graphics.cpp 加 do_screenshot helper + 5 Lua wrap | 25min | A2 |
| **A4** light_ui.cpp 加 录屏 EndFrame hook | 15min | A3 |
| **A5** smoke 测试 (headless 不能真正 readback, 仅测 API surface 与错误返回) | 15min | A4 |
| **A6** demo_quad_split 加 F8 截图键 / R 录屏键演示 | 10min | A4 |
| **A7** 编译 + 8 smoke + 真 GL 截图实测 + commit + push + CI | 25min | A6 |
| **总计** | **~1.5h** | |

## 6. 验收标准

- 5 Lua API 全部实现
- 8 smoke 零回归
- 真 GL 模式 demo_quad_split: 按 F8 截图生成 PNG, 按 R 录屏 60 帧 PNG 序列
- 截图 PNG 用图像查看器打开正常 (color/orientation 正确)
- CI 6 平台全绿 (PNG 编码全平台 stb_image_write 跨平台)

## 7. 风险

1. **stb_image_write 在所有平台编译**: 已是公共领域代码, 跨平台兼容 (Win/Mac/Linux/iOS/Android/Web). 应无问题.
2. **PNG 写盘 stall**: 每帧 readback + PNG 编码 ~5-20ms, 60fps 时会掉帧. 用户已知 trade-off (录屏期 demo 会卡).
3. **目录不存在**: stbi_write_png 不会自动 mkdir, 用户需手动建目录. 文档中说明.
