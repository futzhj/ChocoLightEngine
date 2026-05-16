# Phase F.0.10.8.5 — HDR LUT 完整 TODO

---

## 1. 强制

### 1.1 ⏳ CI 6/6 验证

**等**: GitHub Actions 完成 6 平台 (Linux/macOS/Web/iOS/Android/Windows).

**风险评估**:
- ⚠️ **中等** — GL_RGB16F + GL_FLOAT 跨平台格式兼容:
  - Linux desktop: ✅ 标准 GL3.3
  - macOS: ✅ GL_RGB16F 内置
  - Android (GLES3): ⚠️ 需 `EXT_color_buffer_half_float`, 大多数 GLES3 设备已支持; 极端情况 backend fallback 到 RGB8
  - WebGL2: ⚠️ 类似 GLES3, 需 `EXT_color_buffer_half_float` 扩展
  - iOS Metal: ⚠️ 当前 iOS 走 GL (Metal backend 未实现 LUT3D)
- ✅ stbi_is_16_bit / stbi_load_16 — stb_image 标准 API (15+ 年稳定)
- ✅ 浮点 vector 跨平台

**用户操作**: 检查 [GitHub Actions](https://github.com/futzhj/ChocoLightEngine/actions); 若某平台 fail, 自动 fallback 到 RGB8 路径已就位 (warn log).

---

## 2. 可选扩展

### 2.1 demo live HDR LUT 演示 (~30 min, 推荐)

**目标**: demo_taa_split2 主循环加 HDR LUT 加载 + 切换:
- 准备 1 个 LDR cube + 1 个 HDR cube (DOMAIN_MAX 4 4 4)
- 键盘切换查看高光保留效果

### 2.2 ACES 1.3 内建 LUT (~2h)

**目标**: 内置 ACES 1.3 HDR LUT (32³, DOMAIN_MAX 16 16 16), 供 Lua 一行调用:
```lua
local aces_id = HDR.LoadBuiltinACESLUT()
HDR.SetGradingLUT(aces_id, 1.0)
```

**实施**: 嵌入 .cube 数据到 C 数组 (header 文件), 内部走 LoadCubeLUTFromString.

**优先级**: 低. ACES tonemap 已通过 SetTonemap("aces") 提供; LUT 路径只用于美术自定义 grading.

### 2.3 RGB32F 全精度 (~1h)

**目标**: 当前 RGB16F 半浮点 (5e + 10m, 范围 [-65k, 65k]); 加 RGB32F 走 GL_RGB32F.

**评估**: 仅高端桌面 GPU 普及; 移动端不支持. ROI 低.

### 2.4 backend 加 SupportsLUT3DFloat() 探测 (~0.5h)

**目标**: 在 LoadCube/Hald 调用前知道 backend 能否支持 RGB16F.

**当前**: CreateLUT3DFloat 失败时已 fallback + warn log; 用户感知不到.

**评估**: 可加 `bool SupportsHDRLUT()` 接口给 Lua, 美术可在 UI 显示 "HDR LUT supported / not supported".

---

## 3. 用户支持

### 3.1 何时走 HDR 路径

| .cube DOMAIN_MAX | HALD 位深 | 路径 | 内部格式 |
|------------------|-----------|------|---------|
| 无 / 全 ≤ 1.0 | 8-bit | LDR (传统) | GL_RGB8 |
| 任一 > 1.0 | (cube only) | **HDR** | **GL_RGB16F** |
| (HALD only) | 16-bit PNG | **HDR** | **GL_RGB16F** |

### 3.2 准备 HDR LUT 文件

**Resolve / DaVinci 导出 ACES HDR LUT**:
- 项目 → 色彩 → LUT → 生成 LUT
- 选 `Cube` 格式, 数据范围 `[0, 4]` 或 `[0, 16]`
- 导出 .cube 文件, ChocoLight 自动识别

**Photoshop 16-bit HALD PNG**:
- 文件 → 新建 → 8×8×8 (HALD level 2) / 64×64×64 (level 4)
- 模式: 16 位/通道
- 编辑 → 调色板 → 应用
- 保存为 PNG (16-bit), ChocoLight 自动识别

### 3.3 性能注意

- RGB16F LUT 内存 = RGB8 × 2 (HALF) / × 4 (FLOAT): 32³ HDR LUT = 32×32×32 × 6 byte = 192 KB
- sampler3D RGB16F 采样性能 与 RGB8 相当 (硬件直接支持半浮点纹理)
- 不建议用 64³ HDR LUT (1.5 MB), 32³ 已足够

### 3.4 LDR ↔ HDR 切换

LDR LUT 应用强度 1.0, HDR LUT 应用强度可以 < 1.0:
```lua
HDR.SetGradingLUT(hdr_id, 0.7)  -- 70% HDR LUT, 30% original
```

shader 内 mix(original, lut_result, strength), HDR 值与 LDR 平滑过渡.

---

## 4. 文档导航

| 文档 | 用途 |
|------|------|
| `PLAN_PhaseF_0_10_8_5.md` | 6A 简化 (10 决策 + 数据流图 + 6 任务) |
| `FINAL_PhaseF_0_10_8_5.md` | 项目总结 + 10 决策落地 + ACES workflow 闭环图 |
| **`TODO_PhaseF_0_10_8_5.md`** | 本文件 |
