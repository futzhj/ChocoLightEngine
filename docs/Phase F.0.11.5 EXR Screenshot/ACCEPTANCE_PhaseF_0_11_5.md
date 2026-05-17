# Phase F.0.11.5 EXR Screenshot — ACCEPTANCE 文档

> **阶段**：6A Workflow — 阶段 5 Acceptance（验收）
> **基线**：PLAN_PhaseF_0_11_5.md
> **实施记录**：FINAL_PhaseF_0_11_5.md
> **验收日期**：2026-05-17

---

## 1. 功能验收

### 1.1 tinyexr 集成
- [x] `ChocoLight/third_party/tinyexr.h` (v1.0.7, 369KB) 下载到位
- [x] `ChocoLight/third_party/tinyexr_impl.cpp` 单实现单元 (TINYEXR_USE_STB_ZLIB=1 复用 stb zlib, 不引入 miniz)
- [x] `CMakeLists.txt` 加入 sources 列表 (与 stb_impl.c / cgltf_impl.c 同模式)
- [x] Release build 编译无错 (warn 已通过 `#pragma warning push` 屏蔽 MSVC `strcpy/strncpy_s` deprecation)

### 1.2 Lua API
- [x] `Light.Graphics.ScreenshotEXR(path[, opts])` — opts 表支持 `bit_depth` (16/32) 与 `compression` (none/rle/zips/zip/piz)
- [x] 默认 16-bit half + ZIP 压缩 (FSR2 / DLSS / Nuke 主流配置)
- [x] 注册到 graphics_funcs[] ([light_graphics.cpp:5489](../../ChocoLight/src/light_graphics.cpp#L5489))
- [x] 错误处理 6 路径: HDR 未启用 / sceneTex 无效 / readback 失败 / bit_depth 非法 / compression 非法 / SaveEXR 失败

### 1.3 Smoke 增量 (4 检查点)
- [x] ScreenshotEXR 在 fn_names 列表
- [x] HDR 未启用时返 nil + err
- [x] bit_depth=24 → nil + err (含 "bit_depth")
- [x] compression="lzma" → nil + err (含 "compression")

**Smoke 结果** (screenshot.lua): **34 PASS / 0 FAIL** (旧 30 + 4 新)

### 1.4 Demo 集成
- [x] `demo_taau` 增 **E 键**: `Gfx.ScreenshotEXR('taau_screenshot.exr')` 一键截图
- [x] HUD 键位提示更新

---

## 2. 兼容性验收 (零回归)

| Demo | 启动 | warn/error/fail/undef |
|---|---|---|
| `demo_ssr` | ✅ | 0 |
| `demo_taa_split2` | ✅ | 0 |
| `demo_taau` | ✅ | 0 |
| `demo_multi_hdr_pip` | ✅ | 0 |

| Smoke | PASS | FAIL |
|---|---|---|
| TAA smoke | 171 | 0 |
| Screenshot smoke | 34 (+4 新) | 0 |

---

## 3. 设计决策回顾

### 3.1 tinyexr 版本选择: v1.0.7 (vs latest release branch)
- 最新 `release` 分支已拆分为多文件 (引入 `exr_reader.hh` 头), 不是单文件库
- v1.0.7 是稳定单文件版本, 与 stb 同模式, 集成简单
- 后续若 v2.x 仍保留单文件路径可升级

### 3.2 STB_ZLIB 模式 vs MINIZ
- TINYEXR_USE_STB_ZLIB=1 复用 stb_image.h 现有的 `stbi_zlib_decode_buffer` + stb_image_write.h 的 `stbi_zlib_compress`
- 避免再带一份 miniz 引入 ~3000 行代码 + 符号冲突风险
- ZIP 压缩性能与 miniz 等价 (因为本质都是 deflate)

### 3.3 EXR channel 顺序: A B G R (字母序)
- OpenEXR 规范要求 channels 按字母序排列
- 习惯 BGR 顺序 (Adobe / Maya) 已 deprecated
- tinyexr 内部按 `header.channels[i].name` 字典序对应 `image.images[i]` 数据

### 3.4 Half (16-bit) vs Float (32-bit) 默认
- 默认 half: 体积小一半, 精度 ~3 位小数, 满足 99% HDR 调色需求
- 32-bit float 仅在科研 / 显示 IBL 数据时需要
- 与 FSR2 / DLSS 输出格式一致 (FP16)

### 3.5 Y 翻转方式
- ScreenshotHDR 用 `stbi_flip_vertically_on_write(1)` 调整 stbi 全局状态
- ScreenshotEXR 在 channel 拆分循环内手动翻转 `srcY = h - 1 - y` (拆分时一次到位, 无额外 pass)

---

## 4. 文档验收

- [x] `docs/Phase F.0.11.5 EXR Screenshot/PLAN_PhaseF_0_11_5.md`
- [x] `docs/Phase F.0.11.5 EXR Screenshot/ACCEPTANCE_PhaseF_0_11_5.md` (本文)
- [x] `docs/Phase F.0.11.5 EXR Screenshot/FINAL_PhaseF_0_11_5.md`
- [x] `docs/HANDOFF_REMAINING_TASKS.md` 更新
- [x] `samples/demo_taau/README.md` 增 E 键说明

---

## 5. 验收结论

**核心交付**: tinyexr v1.0.7 单文件集成 + `Light.Graphics.ScreenshotEXR` Lua API (含 bit_depth + compression options) + 4 smoke 检查点 + demo_taau E 键集成.

**验收级别**:
- ✅ **代码层**: PASS (Release build clean, smoke 34 PASS / 0 FAIL)
- ✅ **兼容性**: PASS (4 demo 全零回归, TAA smoke 171/0 不受影响)
- ⏳ **真机功能**: 待用户在 demo_taau 按 E 生成 `.exr` 文件后用 Nuke / Krita / Photoshop 验证读取正常

**结论**: F.0.11.5 **代码层通过验收**, 进入用户真机验证阶段。

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 验收提交 — 代码层 PASS, 真机验证 PENDING |
