# Phase F.0.10.8.1 — `.cube` LUT 文件解析 TODO

> 6A 工作流 · 阶段 6 (Assess) · 待办事项

---

## 1. 强制 (必须解决)

### 1.1 ⏳ CI 6/6 验证

**等**: GitHub Actions 完成所有平台 build (Linux / Windows / macOS / Emscripten / iOS / Android).

**风险点**: 
- `SDL_LoadFile` 在所有平台 SDL3 backend 已 ready
- `strtof / strtol / vsnprintf` 是 C 标准, 各平台无差异
- 无新 GPU / 渲染相关改动 — 风险极低

**用户操作**: 检查 https://github.com/futzhj/ChocoLightEngine/actions

---

## 2. 可选 (建议但非阻塞)

### 2.1 demo 实际渲染中应用 .cube LUT (推荐)

**目标**: 让 `luts/warm_red.cube` + `cool_blue.cube` 在 demo_taa_split2 主循环实际应用 (P1 暖调 vs P2 冷调).

**实施**: 在 demo Window.Open 后, main loop 前加:

```lua
-- 启动时加载 LUT (一次性)
local lut_p1 = hasF10_8_1 and HDR.LoadCubeLUT('samples/demo_taa_split2/luts/warm_red.cube')
local lut_p2 = hasF10_8_1 and HDR.LoadCubeLUT('samples/demo_taa_split2/luts/cool_blue.cube')
print('[demo] warm_red LUT id =', tostring(lut_p1))
print('[demo] cool_blue LUT id =', tostring(lut_p2))

-- 主循环 per-region apply
HDR.Tonemap(0,     0, W/2, H, {
    exposure=1.5, tonemap='aces',
    lut=lut_p1, lutStrength=0.8,
})
HDR.Tonemap(W/2,   0, W/2, H, {
    exposure=0.6, tonemap='uncharted2',
    lut=lut_p2, lutStrength=0.8,
})

-- demo 退出前
if lut_p1 then HDR.DeleteLUT3D(lut_p1) end
if lut_p2 then HDR.DeleteLUT3D(lut_p2) end
```

**工作量**: ~30 分钟

**注**: 路径相对 CWD (Lumen 启动 dir). 可能需要绝对路径 via Light.Filesystem.GetBasePath().

### 2.2 F.0.10.8.2 HALD / Stripe image → 3D LUT (~4h)

**目标**: 支持 PNG / JPG 图像形式的 LUT (Hald CLUT 标准, GIMP/Photoshop 输出).

**实施**: 在 HDRRenderer 加 `LoadHALDLUT(path)` C++ fn, 用 stb_image 解析 PNG, 像素 reshape 成 size^3 × 3 bytes 喂给 backend->CreateLUT3D.

### 2.3 F.0.10.8.3 LUT 热重载 (~2h)

**目标**: 美术修改 `.cube` 后自动检测 + reload.

**实施**: SDL_GetFileLastWriteTime 周期检查; 改动后调 LoadCubeLUT 重建. 加 `HDR.SetLUTHotReload(true)` 开关.

### 2.4 HDR LUT 完整支持 (~3h)

**目标**: 支持 DOMAIN > 1.0 的 HDR LUT (Resolve HDR project 输出).

**实施**:
- backend `CreateLUT3D` 加 `format` 参数 (RGB8 / RGB16F)
- parser 检查 DOMAIN_MAX > 1.0 → 选 RGB16F
- shader 加 HDR 路径 (无 clamp)

---

## 3. 用户支持需求

### 3.1 配置 / 环境

- **无新配置**: F.0.10.8.1 是 API 增量, 不引入新依赖
- **`.cube` 文件**: 用户从 Lightroom / DaVinci Resolve / Premiere 导出即可

### 3.2 美术工作流 (典型 .cube 来源)

| 工具 | 导出菜单 |
|------|---------|
| DaVinci Resolve | Color tab → 右键 LUT thumbnail → Export → Cube |
| Adobe Lightroom | Develop → Profile → Custom → Export as .cube |
| Adobe Premiere | Lumetri Color → Color Wheels → Export Looks (.cube) |
| Final Cut Pro X | Color Inspector → Save Effects Preset → Export Cube |

### 3.3 调试 / 排查

如 `LoadCubeLUT` 失败:

```lua
local id, err = HDR.LoadCubeLUT(path)
if not id then
    print('LUT load failed:', err)
    -- 常见错误:
    -- "file read failed: ..."        → 路径错或文件不存在
    -- "1D LUT not supported"         → 文件含 LUT_1D_SIZE (用 3D 版)
    -- "LUT_3D_SIZE N out of range"   → size 必须 [4, 64]
    -- "data row count <N> mismatch"  → 文件不完整 / 行数错
    -- "expected 3 floats, parse R failed" → 数据行格式错
    -- "backend CreateLUT3D failed"   → GL context 未 ready / VRAM OOM
end
```

### 3.4 性能建议

| LUT size | 文件大小 | parse 时间 | VRAM | 建议场景 |
|---------|---------|-----------|------|---------|
| 17³ | ~70KB | < 2ms | ~14KB | 移动 / Web |
| 33³ | ~600KB | < 10ms | ~104KB | 桌面 SDR |
| 64³ | ~5MB | < 40ms | ~768KB | 高质 SDR / HDR 备 |

- LUT 是**启动一次性加载**, 不要每帧 LoadCubeLUT
- 用完调 `HDR.DeleteLUT3D` 释放 VRAM
- 多 LUT 切换适合 cross-fade (调 strength) 而非 reload

---

## 4. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_8_1.md` | 需求 / .cube 格式标准 / 14 决策 |
| `DESIGN_PhaseF_0_10_8_1.md` | parser 算法 + 接口契约 + 测试矩阵 |
| `TASK_PhaseF_0_10_8_1.md` | 6 原子任务 + 依赖图 |
| `FINAL_PhaseF_0_10_8_1.md` | 项目总结 + 7 关键决策 + Bug 修复记录 |
| **`TODO_PhaseF_0_10_8_1.md`** | 本文件 (CI + demo 实战 + 后续 phase) |
