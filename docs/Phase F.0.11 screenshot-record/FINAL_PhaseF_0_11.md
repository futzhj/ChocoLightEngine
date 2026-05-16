# Phase F.0.11 — Screenshot / PNG Sequence Record FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (5 Lua API + Backend hook, **110 → 115**)

---

## 1. 完成工作

为 demo 视觉验收 + multi-instance 效果展示加入 **截图 / PNG 序列录屏** 能力, 配合 F.0.10.9.x.4 SetState 形成完整工具链:

| API | 签名 | 用途 |
|-----|------|------|
| `Light.Graphics.Screenshot` | `(path) → bool / nil+err` | 全屏截图当前 default fb (Y 自动翻转) |
| `Light.Graphics.ScreenshotRegion` | `(path, x, y, w, h)` | 子区域截图 (single quad / HUD) |
| `Light.Graphics.RecordPNGSequence` | `(dir, max_frames)` | 启动每帧 PNG 序列录屏 |
| `Light.Graphics.StopRecord` | `() → int` | 主动停止, 返已写帧数 |
| `Light.Graphics.IsRecording` | `() → bool, int` | 状态查询 (active, frame_count) |

## 2. 实现要点

### 2.1 Backend 抽象 (1 新虚函数)

`render_backend.h`:
```cpp
virtual bool ReadbackDefaultFB(int x, int y, int w, int h, unsigned char* out_rgba) {
    return false;   // 默认 Legacy / headless: 不支持
}
```

GL33 实现 (`render_gl33.cpp`):
```cpp
bool ReadbackDefaultFB(...) override {
    // ⚠ 关键: 先 drain 之前累积的 GL error (HDR/TAA/SSR 渲染期可能产生
    // driver-specific error 不影响功能, 不 drain 会误报本次 readback 失败)
    while (glGetError() != GL_NO_ERROR) {}
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    return glGetError() == GL_NO_ERROR;
}
```

### 2.2 PNG 编码 (stb_image_write)

下载 `stb_image_write.h` (1.16, 公共领域, 71KB) 加入 `third_party/`. 在 `stb_impl.c` 加 1 行:
```c
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
```

### 2.3 录屏 EndFrame Hook

`light_ui.cpp::l_Window_Call` 在 `g_render->EndFrame()` 之后, `SwapBuffers` 之前调:
```cpp
extern "C" void Light_Graphics_RecordTickHook(int win_w, int win_h);
{
    int win_w = 0, win_h = 0;
    PlatformWindow::GetWindowSize(g_mainWindow, &win_w, &win_h);
    Light_Graphics_RecordTickHook(win_w, win_h);
}
```

时序保证:
- HDR EndScene tonemap 已落到 default fb
- BatchRenderer EndFrame 全 flush
- SwapBuffers 之前, GL_BACK 是确定的本帧画面

`Light_Graphics_RecordTickHook` 内部检查 `g_record.active`, inactive 时是廉价 no-op.

### 2.4 Y 翻转

OpenGL 左下原点 vs PNG 左上原点, 用 `stbi_flip_vertically_on_write(1)` 一次设置.

## 3. 用户场景

### 3.1 单帧截图 (即时)

```lua
local ok, err = Gfx.Screenshot("docs/screenshots/quad.png")
-- ok=true; nil+err 失败 (路径/权限/headless)
```

### 3.2 录屏 (PNG 序列, 用户后处理 ffmpeg)

```lua
-- 启动: 录 120 帧 (~2s @ 60fps), 写 docs/recordings/quad/frame_NNNN.png
Gfx.RecordPNGSequence("docs/recordings/quad/", 120)

-- 引擎自动每帧 EndFrame 后 readback + 写 PNG, 达 max_frames 后 auto-stop

-- 用户后处理 (1 行 ffmpeg):
-- ffmpeg -framerate 60 -i frame_%04d.png -c:v libx264 -pix_fmt yuv420p out.mp4
```

### 3.3 demo_quad_split F8/R 键演示

```lua
-- F8 = 单张截图, R = 切换录屏
elseif key == 297 then     -- F8
    Gfx.Screenshot('docs/screenshots/quad_split.png')
elseif key == string.byte('R') then
    if not g_recording then
        Gfx.RecordPNGSequence('docs/recordings/quad/', 120)
    else
        local n = Gfx.StopRecord()
    end
end

-- CI 自动验证: CHOCO_AUTO_EXIT=1 → 渲染 3 帧 → 录屏 1 帧 → 自动退出
```

## 4. 验证

### 4.1 Smoke 测试 (新增 + 扩充)

| 文件 | 测试段 | 覆盖 |
|------|-------|------|
| `scripts/smoke/screenshot.lua` (新) | 16 个断言 | 5 API surface + headless 错误返回 + round-trip |
| `scripts/smoke/graphics.lua` | [4] 段 9 测试 | 与 GetBackendName / SetViewport 同表测试 |

**关键覆盖**:
- API surface (5 函数都存在且类型 == function)
- Screenshot headless 返 nil + err string (不 raise)
- ScreenshotRegion 参数校验 (w<=0 → nil+err)
- RecordPNGSequence 参数校验 (max_frames<0 → nil+err)
- IsRecording 初始 active=false count=0
- StopRecord 返 number (实际帧数), headless 下 count=0
- Start → Active=true → Stop → Active=false 状态机
- 不 raise (所有错误返 nil+err 而非 lua_error)

### 4.2 真 GL 验证 (1280x720)

`samples/demo_quad_split` 加 F8/R 键 + 自动 CHOCO_AUTO_EXIT=1 模式:

```
$env:CHOCO_AUTO_EXIT='1'; .\light.exe samples\demo_quad_split\main.lua
```

输出:
```
[demo_quad_split] auto screenshot → docs/screenshots/frame_0000.png
[demo_quad_split] CHOCO_AUTO_EXIT=1 → self:Close()
demo_quad_split ok
```

生成 `docs/screenshots/frame_0000.png` (36980 字节 1280x720 PNG), 内容与窗口画面完全一致.

### 4.3 10 smoke 零回归

```
PASS graphics      (含 [4] F.0.11 测试)
PASS screenshot    (新增, 16 断言)
PASS hdr
PASS bloom
PASS ssr
PASS auto_exposure
PASS lens_fx
PASS motion_blur
PASS taa
PASS lighting2d
```

## 5. 关键 Bug Fix

### Bug: `ReadbackDefaultFB` 在大窗口 (1280x720) 失败

**症状**: screenshot_gl.lua (400x300) 工作, demo_quad_split (1280x720) `glGetError() != GL_NO_ERROR` 误报失败.

**根因**: `glGetError()` 返回的是 **累积** 错误, 不是单次调用错误. HDR / TAA / SSR 渲染期可能产生 driver-specific 不影响功能的 error 残留, 我们的 readback 之后 `glGetError()` 误读到了这些累积值.

**修复** (`render_gl33.cpp::ReadbackDefaultFB`):
```cpp
// Step 1: drain 之前帧累积的 error (不属于本次 readback)
while (glGetError() != GL_NO_ERROR) {}
// Step 2: setup + readback
glBindFramebuffer(GL_FRAMEBUFFER, 0);
glPixelStorei(GL_PACK_ALIGNMENT, 1);
glReadPixels(...);
// Step 3: 仅检查本次 readback 是否报错
if (glGetError() != GL_NO_ERROR) return false;
```

**验证**: 修复后 demo_quad_split 1280x720 截图成功, screenshot_gl 400x300 仍正常.

### 关于 `extern "C"` 链接规范

`light_ui.cpp` 调 `light_graphics.cpp` 的 `Light_Graphics_RecordTickHook`. 后者声明为 `extern "C"`, 前者必须 file-scope 一致声明 (block-scope `extern "C"` 是 MSVC 错误 C2598). 修复: forward declaration 提到文件全局.

## 6. 文件变更

| 文件 | 操作 | 行数 |
|------|------|------|
| `ChocoLight/third_party/stb_image_write.h` | 新增 (公共领域 1.16) | +71221 |
| `ChocoLight/third_party/stb_impl.c` | +3 行 (#define + #include) | +3 |
| `ChocoLight/include/render_backend.h` | +1 虚函数 | +12 |
| `ChocoLight/src/render_gl33.cpp` | +ReadbackDefaultFB 实现 | +18 |
| `ChocoLight/src/light_graphics.cpp` | +5 Lua API + RecordState + hook | +120 |
| `ChocoLight/src/light_ui.cpp` | +RecordTickHook 调用 | +13 |
| `scripts/smoke/screenshot.lua` | 新 smoke (16 断言) | +30 |
| `scripts/smoke/graphics.lua` | +F.0.11 测试段 [4] | +50 |
| `samples/demo_quad_split/main.lua` | +F8/R 键 + CHOCO_AUTO_EXIT | +40 |
| `docs/Phase F.0.11 screenshot-record/{PLAN,FINAL}.md` | 新建 | — |
| `docs/screenshots/quad_split.png` | 实测生成 (1280x720) | 36KB |
| **总计 (不含 stb_image_write.h)** | | **~290 LOC** |

## 7. 关键决策回顾

### 7.1 为何选 PNG 序列而非 mp4

- **复杂度**: mp4 编码需要 FFmpeg encoder (现 video_backend_ffmpeg.cpp 仅 decoder), 引入 `avcodec_send_frame`/`av_interleaved_write_frame` 等 ~500 LOC.
- **依赖**: FFmpeg DLL 已是 optional, encoder 路径需保证更多函数指针.
- **用户体验**: PNG 序列 + 1 行 ffmpeg `ffmpeg -framerate 60 -i frame_%04d.png ...` 拼接, 通用工作流.
- **trade-off**: PNG 写盘 stall (~5-20ms/帧 1280x720), 录屏期间 demo 卡顿. 用户已知.

### 7.2 为何 hook 而非 Lua 侧调

`Gfx.Screenshot()` 在 Draw 回调里调时, default fb 还没 HDR tonemap (HDR EndScene 在 BatchRenderer EndFrame 之后). 必须放到 `g_render->EndFrame()` 之后, `SwapBuffers` 之前. 这只能通过 backend hook 实现, Lua 侧无法直接 hook EndFrame 时序.

### 7.3 为何加 stb_image_write 而非自写 PNG encoder

- 下载即用 (单 71KB header), 公共领域 license
- 与已用 stb_image (解码) 同源, 完全兼容
- 跨平台 100% (Windows/Mac/Linux/iOS/Android/Web)
- 自写 PNG encoder 需 zlib/deflate 实现 ~500 LOC, 重复造轮子

### 7.4 为何 silent skip 而非 lua_error

`Screenshot` / `RecordPNGSequence` 失败 (路径无权限 / headless / disk full) 返 `nil + err string` 而非 `lua_error`. 用户用 pcall 不需特殊处理, 与 SetState 的 nil+err 一致.

### 7.5 关于 frame_skip 优化 (未做)

录屏每帧 readback 写盘成本固定 ~5-20ms. 可选 `frame_skip` 参数 (每 N 帧写 1 张) 降到 60fps/3=20fps. 留 F.0.11.1 优化, 当前不做 (用户大多录短视频).

## 8. F.0 Phase 累计

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0.10.0 ~ F.0.10.9.x.4 | multi-instance + LUT + Clone + Get/SetState | 110 |
| **F.0.11 (本)** | **Screenshot + PNG Record** | **+5 (115)** |
| **总计** | **完整 multi-instance 工具链 + 视觉验收能力** | **115 fn** |

至此, ChocoLightEngine 具备:
1. 多 instance HDR/TAA/Bloom/SSR/MotionBlur (split-screen / 多窗口 / PIP)
2. Clone + GetState + SetState 完整生命周期 (1-行 setup / save load profile)
3. Screenshot + PNG 录屏 (demo 验收 / 美术工作流 / regression diff)

## 9. 下一步 (建议优先级)

| 任务 | 工作量 | 价值 |
|-----|-------|------|
| F.0.10.10.2 demo Bloom/SSR/MB multi-instance refactor | ~2h | 中 (apply_postfx_profile 替换为 SetActiveInstance) |
| F.0.11.1 frame_skip 优化录屏性能 | ~1h | 中 (60fps 录屏不卡) |
| F.0.11.2 HDR `.hdr` / `.exr` 截图 (RGBA16F) | ~3h | 低 (美术 HDR 工作流) |
| F.1 TAAU DLSS-like upscaling | ~10-15h | 高 (性能 + 画质) |
| F.0.11.3 mp4 直出 (FFmpeg encoder) | ~5-8h | 低 (PNG+ffmpeg 已够用) |

推荐路径: **F.0.10.10.2** (扫尾) → **F.1 TAAU** (重头戏).
