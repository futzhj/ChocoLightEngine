# Phase F.0.10.8.2 — HALD CLUT 图像 LUT 加载 TODO

> 6A · 阶段 6 (Assess) · 待办事项

---

## 1. 强制 (必须解决)

### 1.1 ⏳ CI 6/6 验证

**等**: GitHub Actions 完成所有平台 build (Linux / Windows / macOS / Emscripten / iOS / Android).

**风险点**:
- stb_image 已在项目稳定使用 (light_graphics_image.cpp / light_graphics_mesh.cpp / light_tray.cpp), 各平台 ready
- 无新 GPU / 渲染相关改动
- 风险**极低**

**用户操作**: 检查 https://github.com/futzhj/ChocoLightEngine/actions

---

## 2. 可选 (建议但非阻塞)

### 2.1 demo 真 HALD PNG 加载演示 (推荐)

**目标**: 让 demo_taa_split2 加载真 HALD PNG (用户自备 from Photoshop / GIMP), 在主循环视觉化.

**实施**:
```lua
-- 在 hasF10_8_2 路径下加:
local lut_id = HDR.LoadHaldLUT('samples/demo_taa_split2/luts/my_hald8.png')
if lut_id then
    HDR.SetGradingLUT(lut_id, 1.0)
    print('[demo] HALD LUT loaded id=' .. lut_id)
end
```

**用户操作**: 从 Photoshop "Image → Adjustments → Color Lookup" 或 GIMP "Tools → Color Picker → Hald CLUT" 导出 HALD level 8 PNG, 命名 `my_hald8.png` 放入 `samples/demo_taa_split2/luts/`.

**工作量**: ~20 min (代码) + 用户提供素材

### 2.2 F.0.10.8.3 LUT 热重载 (~2h)

**目标**: 美术修改 `.cube` 或 HALD PNG 后**自动检测 + reload**.

**实施**:
- C++ 加 SDL_GetFileLastWriteTime polling (per frame in HDR.EndScene)
- 检测 mtime 变化时 → 自动 reload + 替换 tex_id (旧 id 自动 Delete)
- Lua API: `HDR.SetLUTHotReload(true)` + 注册 callback (可选)

**收益**: 美术调色实时反馈 — 这是工业级工具链关键能力.

### 2.3 HDR LUT 完整支持 (DOMAIN > 1.0 + 16-bit) (~3h)

**目标**: 支持 HDR LUT (Resolve HDR project 输出 + ACES workflow).

**实施**:
- backend `CreateLUT3D` 加 `format` 参数 (RGB8 / RGB16F)
- `.cube` parser: 检测 DOMAIN_MAX > 1.0 → 选 RGB16F
- HALD PNG: 检测 16-bit PNG (stbi_load_16) → 选 RGB16F
- shader: 加 HDR 路径 (无 clamp)

### 2.4 Stripe LUT (1×N³ 长条 ImageMagick 变体) (~1h)

**目标**: 支持 stripe 形式 LUT (ImageMagick `-hald-clut` 变体, 较少用).

**实施**: HALD parser 多加一个 path: width = N⁶, height = 1 (stripe layout). 像素 reshape.

**优先级**: 低 (用户主流是 HALD 方阵).

---

## 3. 用户支持需求

### 3.1 HALD CLUT 生成 (典型工具)

| 工具 | 操作 |
|------|------|
| **Photoshop** | Image → Adjustments → Color Lookup → 用 .cube → File → Export → Save Color Lookup as Hald CLUT PNG (插件可能需) |
| **GIMP** | Filters → Generic → Hald CLUT identity (创建 level 8 identity PNG) → 修改 → Save |
| **ImageMagick** | `convert hald:8 hald8.png` (生成 level 8 identity) → 修改 → 直接用 |
| **Online** | https://www.colorimetric.com/ 等在线 HALD generator |

### 3.2 调试 / 排查

```lua
local id, err = HDR.LoadHaldLUT(path)
if not id then
    print('HALD LUT load failed:', err)
    -- 常见错误:
    -- "stbi_load failed: can't fopen"          → 路径错或文件不存在
    -- "stbi_load failed: unknown image type"   → 文件不是 PNG/JPG/BMP/TGA
    -- "HALD image not square: WxH"             → 必须方阵 (W == H)
    -- "HALD width N is not N^3 for any N..."   → 必须 N³ × N³ (N ∈ [2,8])
    --   expected: 8/27/64/125/216/343/512
    -- "HDR backend not initialized..."         → HDR 模块未 Enable / 无 GL context
    -- "backend CreateLUT3D failed..."          → VRAM OOM / GL driver bug
end
```

### 3.3 性能建议

| HALD level | 图像 | LUT size | parse 时间 | VRAM |
|-----------|------|----------|-----------|------|
| 2 | 8×8 | 4 | < 1ms | 192B |
| 4 | 64×64 | 16 | ~4ms | 12KB |
| **8** | **512×512** | **64** | **~55ms** | **786KB** |

**建议**:
- LUT 启动一次性加载, 不要每帧 LoadHaldLUT
- 美术工作: level 8 (Photoshop / Resolve / Lightroom 默认)
- 移动 / Web: level 4-6 (平衡质量 / VRAM)

---

## 4. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_8_2.md` | HALD CLUT 标准 + scope + 12 决策 |
| `DESIGN_PhaseF_0_10_8_2.md` | parser 算法 + 接口契约 + 测试矩阵 |
| `TASK_PhaseF_0_10_8_2.md` | 6 原子任务 + 依赖图 |
| `FINAL_PhaseF_0_10_8_2.md` | 项目总结 + 7 关键决策 + LUT 生态拓展 |
| **`TODO_PhaseF_0_10_8_2.md`** | 本文件 (CI + demo 真 HALD + 后续 phase) |
