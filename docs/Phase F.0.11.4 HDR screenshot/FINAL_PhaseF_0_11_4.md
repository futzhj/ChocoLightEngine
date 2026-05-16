# Phase F.0.11.4 — HDR `.hdr` 截图 FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (美术 HDR 工作流, Radiance RGBE 32-bit 输出)
> 基线: F.0.11.3 commit `09a4754`

---

## 1. 目标

为 F.0.11 截图系统加 **HDR 浮点截图**: 读取 `HDRRenderer` 的 sceneTex (RGBA16F, tonemap 之前的浮点数据) → 写入 `.hdr` (Radiance RGBE 32-bit-per-pixel) 格式.

**用途**:
- 美术 HDR 工作流: Photoshop / Krita / Affinity Photo 可直接读 `.hdr` 做后期
- 调试 HDR 数据: 检查 tonemap 前的 raw radiance, 排查曝光/bloom/lens fx 问题
- 输出参考: 用于训练 ML 模型 / 对比不同 tonemap 算法

## 2. 技术方案

### 2.1 数据流

```
HDR sceneTex (RGBA16F GL_TEXTURE_2D)
    ↓ glReadPixels(GL_RGBA, GL_FLOAT) (通过临时 FBO)
RGBA float buf (w*h*4)
    ↓ 紧凑化, 丢 alpha
RGB float buf (w*h*3)
    ↓ stbi_flip_vertically_on_write(1) + stbi_write_hdr
.hdr (Radiance RGBE, 4 bytes per pixel)
```

### 2.2 跨 GL 版本兼容的纹理 readback

GL3.3 / GLES3 都不支持 `glGetTexImage` (GLES 完全没, GL3.3 已 deprecated). 替代方案:

```cpp
// 临时 FBO 绑 tex 为 COLOR_ATTACHMENT0, glReadPixels 读
GLuint tmpFbo;
glGenFramebuffers(1, &tmpFbo);
glBindFramebuffer(GL_FRAMEBUFFER, tmpFbo);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, out);
glDeleteFramebuffers(1, &tmpFbo);
```

封装在 `RenderBackend::ReadbackTextureRGBAFloat(tex, w, h, out)`.

### 2.3 Radiance RGBE 编码 (stbi_write_hdr)

stb_image_write 已支持 (与 stb_image 同源, 已在 F.0.11 引入). 编码:
- 每像素 4 bytes: RGB + 共享指数 E (浮点 -> RGB * 2^(E-128))
- 文件头: `#?RADIANCE\n# Written by stb_image_write.h\n...`
- 不需新依赖, 不引入 OpenEXR 大库

`.exr` (OpenEXR) 留未来 F.0.11.5 加 tinyexr.

## 3. API 设计

### 3.1 Backend 抽象 (+1 虚函数)

```cpp
// render_backend.h
virtual bool ReadbackTextureRGBAFloat(uint32_t tex, int w, int h,
                                       float* out_rgba) { return false; }
```

GL33 重写, Legacy/headless 保持默认 false.

### 3.2 Lua API (+1 函数)

```lua
local ok, err = Light.Graphics.ScreenshotHDR('scene.hdr')
-- ok=true: 写盘成功
-- ok=nil + err string: HDR 未启用 / readback 失败 / 写盘失败
```

**API 总计**: 117 → **118** (+1)

### 3.3 Lua 端校验链

```cpp
1. g_render 非 null   → "render backend not ready"
2. HDR.IsEnabled()    → "HDR not enabled (call HDR.Enable first)"
3. tex/w/h 有效       → "invalid HDR scene (tex=%u, %dx%d)"
4. ReadbackTextureRGBAFloat → "ReadbackTextureRGBAFloat failed (backend support?)"
5. stbi_write_hdr     → "stbi_write_hdr failed for '%s'"
```

每步失败都返 `nil + err`, 用户能精确诊断.

## 4. 验证

### 4.1 Smoke (30 断言, 28 → 30)

```
PASS ScreenshotHDR exists
PASS ScreenshotHDR no HDR → nil+err   (headless: HDR not enabled, 友好失败)
screenshot smoke: 30 pass / 0 fail
```

### 4.2 真 GL 验证 (1280×720 demo_quad_split)

```bash
$env:CHOCO_AUTO_EXIT='1'; $env:CHOCO_SCREENSHOT_HDR='1'
.\light.exe samples\demo_quad_split\main.lua
```

输出:
```
[demo_quad_split] auto screenshot → docs/screenshots/frame_0000.png
[demo_quad_split] HDR screenshot → docs/screenshots/scene_hdr.hdr (F.0.11.4)
demo_quad_split ok
```

生成文件:
- `frame_0000.png`: 36980 bytes (LDR baseline, 与 F.0.11.2 一致)
- `scene_hdr.hdr`: **18835 bytes** (Radiance RGBE)

### 4.3 文件格式校验

```
> dd if=docs/screenshots/scene_hdr.hdr bs=1 count=40 | hexdump -C

#?RADIANCE
# Written by stb_image_write.h
```

完全合规 Radiance 文件头.

### 4.4 10 smoke 零回归

```
PASS graphics    PASS screenshot   PASS hdr         PASS bloom
PASS ssr         PASS auto_exposure PASS lens_fx    PASS motion_blur
PASS taa         PASS lighting2d
```

## 5. 边界处理

### 5.1 HDR 未启用 → 友好失败

```lua
local ok, err = Gfx.ScreenshotHDR('out.hdr')
-- ok=nil
-- err='ScreenshotHDR: HDR not enabled (call HDR.Enable first)'
```

调用方可据此自动 fallback:
```lua
if not Gfx.ScreenshotHDR('out.hdr') then
    Gfx.Screenshot('out.png')   -- LDR fallback
end
```

### 5.2 RGBA → RGB 紧凑化 (丢 alpha)

stbi_write_hdr 仅接受 3-channel float. RGBA 转 RGB 时丢 alpha (Radiance 格式无 alpha 概念).

实际 HDR scene 几乎不用 alpha (compositing 多在 LDR 段做), 丢失可接受.

### 5.3 Y 翻转 (与 PNG 一致)

GL 左下原点 vs `.hdr` 文件格式左上原点 (与 PNG/EXR 同). `stbi_flip_vertically_on_write(1)` 处理.

### 5.4 active HDR instance

读 `HDRRenderer::GetSceneTexture()` (当前 active instance). multi-instance 场景下用户先 `HDR.SetActiveInstance(id)` 切到目标 instance 再调 `ScreenshotHDR`.

### 5.5 Backend 不支持纹理 readback

Legacy backend 默认返 false → `ReadbackTextureRGBAFloat failed (backend support?)`. 用户应只在 GL33+ backend 下使用 `ScreenshotHDR`.

## 6. 文件变更

| 文件 | 操作 | 行数 |
|------|------|------|
| `ChocoLight/include/render_backend.h` | +1 虚函数 ReadbackTextureRGBAFloat | +18 |
| `ChocoLight/src/render_gl33.cpp` | +ReadbackTextureRGBAFloat 实现 (临时 FBO + glReadPixels) | +42 |
| `ChocoLight/src/light_graphics.cpp` | +l_Graphics_ScreenshotHDR + 注册 | +52 |
| `scripts/smoke/screenshot.lua` | +2 断言 (28→30) | +5 |
| `samples/demo_quad_split/main.lua` | +CHOCO_SCREENSHOT_HDR=1 env var 测试路径 | +9 |
| `docs/Phase F.0.11.4 HDR screenshot/FINAL_*.md` | 新建 | — |
| `docs/screenshots/quad_split_F0_11_4_hdr.hdr` | 真 GL 验证产物 | 18835 |
| **总计** | | **~126 LOC** |

## 7. 关键决策

### 7.1 为何 `.hdr` (Radiance) 而非 `.exr` (OpenEXR)

| 维度 | `.hdr` (Radiance) | `.exr` (OpenEXR) |
|------|------------------|------------------|
| 依赖 | stb_image_write (已用, 0 新增) | tinyexr / OpenEXR (~3000+ LOC) |
| 文件大小 | 小 (RGBE 32bpp) | 大 (RGB float96 / RGB half48) |
| 精度 | 8.8 位有效 (RGBE 共享指数) | 16-bit half (Mantissa 完整) |
| 主流工具 | Photoshop / Affinity / Krita | Nuke / DaVinci / Houdini |
| 美术接受度 | 普及 (HDRI 制作首选) | 影视后期 (compositing 首选) |

**结论**: `.hdr` 满足游戏美术需求, 不增加依赖. `.exr` 是后续 F.0.11.5 候选 (留扩展点).

### 7.2 为何走临时 FBO 而非 glGetTexImage

| API | 兼容性 | 性能 |
|-----|--------|------|
| glGetTexImage | GL ≥ 1.0, **GLES 完全不支持** | 与 FBO 法相当 |
| 临时 FBO + glReadPixels | GL3.3+ + GLES3+ ✓ | 同 ✓ |

ChocoLight 走 GL3.3 backend (现) + 未来可能 GLES3 (移动平台), 选 FBO 法更面向未来.

### 7.3 为何不在录屏路径加 HDR

录屏 (`RecordPNGSequence`) 每帧 60+ 张, 输出 LDR 8-bit 是合理选择 (压缩比高, 工具链友好). 如要 HDR 序列, 后续可加 `RecordHDRSequence` 单独 API. 目前 `ScreenshotHDR` 是单帧美术用例.

## 8. F.0.11 系列完整收尾 (2nd round)

| Phase | 功能 | API |
|-------|------|-----|
| F.0.11 | Screenshot + Record (sync) | +5 |
| F.0.11.1 | frame_skip 跳帧 | +0 |
| F.0.11.2 | PBO async readback | +2 |
| F.0.11.3 | StopRecord flush last frame | +0 |
| **F.0.11.4 (本)** | **HDR `.hdr` 截图** | **+1** |

**F.0.11 总 API**: 8 (Screenshot, ScreenshotRegion, RecordPNGSequence, StopRecord, IsRecording, SetRecordAsync, IsRecordAsync, ScreenshotHDR).

至此 F.0.11 截图/录屏系统功能 + 性能 + 美术 HDR 工作流全部就绪.

## 9. 下一步

| 任务 | 工作量 | 建议 |
|-----|-------|------|
| **F.1 TAAU DLSS-like upscaling** | ~10-15h | **最高优, 重头戏** |
| F.0.11.5 `.exr` 输出 (tinyexr) | ~4-5h | 中优 (影视后期工作流) |
| F.0.11.6 multi-instance HDR 截图 (id 参数) | ~1h | 低优 |

推荐: **F.1 TAAU**.
