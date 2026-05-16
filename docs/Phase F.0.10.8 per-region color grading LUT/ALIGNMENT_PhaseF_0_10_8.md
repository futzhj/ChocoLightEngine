# Phase F.0.10.8 — per-region color grading LUT ALIGNMENT

> 6A 工作流 · 阶段 1 (Align) · 模糊需求 → 精确规范

---

## 1. 原始需求

实现 **per-region color grading LUT** — split-screen 中每 region 可独立加载不同 3D LUT 实现差异化调色 (如 P1 黄昏暖色 LUT vs P2 冷夜蓝色 LUT). 是 UE5 `FViewInfo::FilmGrading` LUT 系统的最简对标实现.

业界对照: UE5 / Unity URP 默认色彩管线均含 LUT pass, 是 AAA 调色标配.

---

## 2. 项目上下文分析

### 2.1 当前 tonemap 管线 (F.0.10.6 后)

- **shader** (`render_gl33.cpp` GLES3 + GL33 双版本):
  - sampler2D uHDRTex, uExposure, uGamma, uTonemapMode (4 operator: ACES / Reinhard / Uncharted2 / Linear)
  - 流程: HDR → exposure → tonemap → gamma → output
- **backend**:
  - `DrawTonemapFullscreen(hdrTex, exp, gamma, mode)` — F.0.10.6 之前
  - `DrawTonemapRegion(hdrTex, exp, gamma, mode, x, y, w, h)` — F.0.10.6 新
- **HDRRenderer**:
  - `Tonemap(rgn)` 含 AE / `Tonemap(rgn, exp, gamma, mode)` 显式 — F.0.10.6 新
  - `autoTonemap` toggle (默认 true)
- **Lua**: `HDR.Tonemap(x,y,w,h [, params_table])` (`params.exposure/gamma/tonemap` 三字段)

### 2.2 3D texture 现状

- **零基础**: codebase 无 `glTexImage3D` / `sampler3D` / 任何 LUT 使用历史
- **glad 头**: 已暴露 `glTexImage3D` 等 3D 函数 (OpenGL 3.3 core 标准)
- **GLES 3.0**: 支持 3D texture (核心规格), 不需扩展

### 2.3 用户纹理传递模式 (现成模板)

`LensFlare.SetFlareTexture` 模式:
```lua
LensFlare.SetFlareTexture(image)  -- Image table with :GetTextureId() / number id / nil
LensFlare.GetFlareTextureId()     -- → uint32_t
```
内部: backend 端 `flareTexId=0` 时 fallback 到 1x1 白 (procedural). 这是本 phase LUT 的直接参考.

---

## 3. 边界确认 (任务范围)

### 3.1 In scope (本 phase 必做)

- [x] backend 3D texture create / delete API (`CreateLUT3D` / `DeleteLUT3D`)
- [x] backend tonemap shader 加 sampler3D + uLUT + uLUTStrength + uLUTEnabled uniform (GLES3 + GL33 双源)
- [x] backend `DrawTonemap{Fullscreen,Region}` 加 lutTex + lutStrength 参数 (默认 0/0 = no LUT, 零回归)
- [x] HDRRenderer: `SetGradingLUT(texId, strength)` 全局 + `GetGradingLUTId` / `GetGradingLUTStrength`
- [x] HDRRenderer: `Tonemap(rgn, params)` params 加 `lut` / `lutStrength` 字段
- [x] Lua API: `HDR.CreateLUT3D(size, data)` / `HDR.DeleteLUT3D(id)` / `HDR.SetGradingLUT(id, strength)` / `HDR.GetGradingLUTId` / `HDR.GetGradingLUTStrength`
- [x] Lua `HDR.Tonemap` 加 `lut` / `lutStrength` 字段解析 (含 nil-as-disable)
- [x] smoke 加 round-trip + create/delete + headless 路径
- [x] demo `demo_taa_split2` 加 procedural identity LUT 或简单调色 LUT 演示
- [x] 6A 文档 (ALIGN / DESIGN / TASK / ACCEPTANCE / FINAL / TODO)

### 3.2 Out of scope (留后续)

- ❌ `.cube` 文本格式解析 (LUT 标准格式) — 留 F.0.10.8.1
- ❌ HALD / stripe image 转 3D LUT — 留 F.0.10.8.2
- ❌ LUT 热重载 (file watch) — 留 F.0.10.8.3
- ❌ tetrahedral 插值 (默认 trilinear 即可, 视觉差异微小)
- ❌ per-LUT mipmap (3D texture mipmap 在 LUT 无意义)
- ❌ floating-point LUT (raw bytes RGB8 即可, 现代标准)
- ❌ LUT 序列化 / asset 系统 (留资产管线)

### 3.3 兼容性约束

- **零回归**: 用户不调任何 LUT API → tonemap 行为与 F.0.10.6 完全等价
- **6 平台支持**: GLES 3.0 (Web/iOS/Android) + GL 3.3 (Linux/macOS/Windows) — 均原生支持 3D texture
- **可选**: 用户不传 LUT 时 shader 跳过 LUT 采样 (单 if branch, 性能影响 < 5%)

---

## 4. 需求理解

### 4.1 LUT 工作原理 (业界标准)

3D LUT (Look-Up Table) 是色彩查找表, 输入 HDR 经 tonemap 后的 LDR RGB (∈[0,1]), 查表得到 graded RGB. 数学:

```
graded.rgb = sample3D(uLUT, vec3(ldr.r, ldr.g, ldr.b))
final.rgb = mix(ldr.rgb, graded.rgb, uLUTStrength)
```

`strength=0` 等价于不应用 LUT, `strength=1` 完全替换. 适合 cross-fade.

### 4.2 LUT 尺寸标准

| Size | 内存 | 业界用法 |
|-----|-----|---------|
| 16³ | 12 KB | UE3, 简单调色 |
| 17³ | 14 KB | Adobe Photoshop 标准 (尺寸奇导致插值不对称) |
| 32³ | 96 KB | UE4/5 默认, 高质 |
| 33³ | 105 KB | Adobe Premiere 标准 |
| 64³ | 768 KB | 顶级电影, 过设计 |

**本 phase 决策**: 默认 16, 支持任意 size (4..64). 用户负责数据完整性 (size³ × 3 bytes RGB).

### 4.3 LUT 数据格式

- **传递**: Lua 端传 size + raw bytes (string 或 number array)
- **GL upload**: `glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8, s, s, s, 0, GL_RGB, GL_UNSIGNED_BYTE, data)`
- **filter**: `GL_LINEAR` (trilinear, 硬件免费)
- **wrap**: `GL_CLAMP_TO_EDGE` (LUT 边界精确)

### 4.4 Identity LUT (no-op grading)

如果用户传 strength=0 或不传 LUT, shader 不采样 LUT (`uLUTEnabled=0` short circuit). 不需要内置 identity LUT.

---

## 5. 智能决策矩阵

| # | 决策点 | 选项 | **本 phase 决策** | 依据 |
|---|--------|-----|------------------|------|
| 1 | LUT 维度 | 1D / 3D / tetrahedral | **3D trilinear** | 业界标准, 硬件免费 |
| 2 | LUT size | 固定 16 / 任意 | **任意 (4..64), 默认 16** | 用户灵活, 默认低成本 |
| 3 | 数据传递 | string / array / file | **string + array 双支持** | string 高效 (binary), array 易用 (开发期) |
| 4 | strength 范围 | [0,1] / [0, ∞] | **[0, 1] clamp** | 业界标准, 防过曝 |
| 5 | per-region 维护 | 全局 / params 字段 | **params 字段 (lut + lutStrength)** | 与 F.0.10.6 一致 |
| 6 | 全局 SetGradingLUT | 同时支持 / 仅 params | **两者支持** | 兼容单视角场景 |
| 7 | shader 开关 | uniform branch / variant | **uniform `uLUTEnabled`** | 单 program 简化 |
| 8 | LUT 0 fallback | identity LUT / disabled | **disabled (短路)** | 省内存 + 省 1 fetch |
| 9 | DeleteLUT3D | 立即删除 / 引用计数 | **立即删除** | 用户自己管理生命周期 |
| 10 | API surface | 子模块 LUT / HDR 子表 | **HDR 子表内** | 与 SetTonemapper 同级 |
| 11 | Lua data 输入 | byte string / int array | **byte string + int array 双兼容** | string 高效 (loadfile binary), array 易测 |
| 12 | shader uniform | sampler3D 1 个 / 多 LUT | **1 个 sampler3D** | 单 LUT 已足够多数场景 |
| 13 | 默认 lutStrength | 0.0 / 1.0 | **1.0** | API 简化, 用户传 LUT 即生效 |
| 14 | LUT 在 tonemap 前/后 | LUT(hdr) / LUT(ldr) | **LUT(ldr)** | 业界标准, LUT 设计为 LDR 调色 |

---

## 6. 关键假设 (需用户确认或自动)

1. **a) LUT 应用时机** — 自动: tonemap 后 (LDR domain)
2. **a) 默认 lutStrength** — 自动: 1.0 (传 LUT 即生效)
3. **a) GL ES 3.0 兼容性** — 假设: 3D texture 是 GLES 3 核心, 不需扩展
4. **a) 用户数据格式** — 假设: 用户负责数据为 size³ × 3 字节 RGB (R 变化最快)
5. **a) demo scope** — 自动: 用 procedural LUT (Lua 生成简单红/蓝偏移), 不依赖外部 .cube 文件

---

## 7. 验收标准

### 7.1 功能验收

- [x] `HDR.CreateLUT3D(16, data)` 返 texId > 0; `DeleteLUT3D(id)` 返 true
- [x] `HDR.SetGradingLUT(id, 0.5)` 后 `Tonemap()` 与 strength=0 视觉不同 (需 GL context)
- [x] `HDR.Tonemap(rgn, {lut=id, lutStrength=0.8})` per-region 覆盖全局
- [x] strength=0 或 lut=0 时 shader 跳过 LUT (uLUTEnabled=0 验证)
- [x] CreateLUT3D 输入校验 (size 4..64, data 长度 = size³×3) → 失败返 0 + err

### 7.2 零回归

- [x] 用户不调任何 LUT API → 8 smoke 全 PASS (含原 hdr.lua smoke)
- [x] demo_taa_split2 不改 LUT 路径 → headless 11 PASS 不变
- [x] 老 `DrawTonemapFullscreen` / `DrawTonemapRegion` 兼容 (默认 lutTex=0, lutStrength=0)

### 7.3 文档完整

- [x] 6 个 6A 文档全
- [x] `Light_Graphics.md` 加 LUT API 段
- [x] smoke 加 5+ PASS (create / delete / round-trip / no-LUT 短路)

---

## 8. 工作量估

| 子阶段 | 内容 | 估时 |
|-------|------|-----|
| ALIGN/DESIGN/TASK 文档 | 已对齐 + 设计 + 任务拆分 | 0.5h |
| SP1 (backend) | GL33 LUT3D create/delete + tonemap shader 改 + Draw 接口扩参 | 2h |
| SP2 (HDRRenderer) | State + Set/Get + Tonemap params 加 lut/lutStrength | 1h |
| SP3 (Lua API) | 5 Lua fn + smoke + demo 探针 | 1.5h |
| SP4 (Assess) | 6A Assess 文档 | 0.5h |
| **合计** | | **~5.5h** (DESIGN 估 5-7h, 中位) |

---

## 9. 风险 / 缓解

| 风险 | 影响 | 缓解 |
|------|-----|------|
| GLES 3.0 3D texture 兼容性差异 | 移动端崩 | 用 GL_RGB8 (核心标准, 无扩展依赖) |
| Lua string 上传 binary 性能 | 大 LUT 慢 | 默认 size=16 (12KB) → 微秒级 |
| 内存泄漏 (用户忘 Delete) | VRAM 泄漏 | Lua 文档强调 + HDR.Shutdown 时不强清 (用户负责) |
| LUT 数据格式错 | shader 采样错 / 崩 | CreateLUT3D 校验 size + data 长度 |
| shader uniform 性能 | 多 1 if branch | 测量 < 5% (uniform branch 现代 GPU 零成本) |

---

## 10. 后续 phase 候选

- **F.0.10.8.1**: `.cube` 文件解析 (Adobe / Resolve LUT 标准) ~3h
- **F.0.10.8.2**: HALD / stripe image 转 3D LUT (PNG 加载) ~4h
- **F.0.10.8.3**: LUT 热重载 (file watch) ~2h
- **F.0.10.9**: 真多 HDR target (RT pool) ~8-10h
- **F.1**: DLSS-like TAAU ~10-15h
