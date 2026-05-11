# ALIGNMENT — Phase E.4 · Bloom 后处理

> 6A 工作流 · 阶段 1 · Align
> 目标：模糊需求 → 精确规范

---

## 1. 项目上下文分析

### 1.1 前置基础（已完成）

- **Phase E.3 (HDR + Tonemap)**：HDR RT (RGBA16F) + ACES tonemap 已上线 4 子任务（`9ce4431` → `cf90d43` → `d6eae5d` → `4e0501f`）
- **集成点**：`HDRRenderer::EndScene` 在 SwapBuffers 前做 `UnbindFBO + DrawTonemapFullscreen`
- **Lua 暴露**：`Light.Graphics.HDR.*` 12 函数 + `Light.Graphics.FlushLitBatch` 同模式
- **架构模式**：
  - Namespace module (非 class) 模式（BatchRenderer / LitBatchRenderer / HDRRenderer 同风格）
  - 显式 Enable / Disable + `IsSupported()` 后端能力查询 + `IsEnabled()` 运行时状态
  - 所有 GL 操作经 `RenderBackend` 虚接口 → Legacy 后端 no-op

### 1.2 现有类似子系统

- `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` — 模块级生命周期 / RT 管理参考
- `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:InitTonemap` — 全屏 quad VBO + shader + uniform location 缓存模式
- `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:CreateHDRFBO` — FBO 创建 / FBO↔DepthRBO map 管理参考

### 1.3 技术栈

- C++ → GL33 + GLES3 shader（双份实现，`#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)` 分支）
- Lua 5.1 绑定 → `light_graphics.cpp` `Bloom` 子表
- CI：Windows runtime smoke via `scripts/smoke/bloom.lua`（新建）

---

## 2. 需求理解

### 2.1 什么是 Bloom

Bloom（泛光）是 HDR 管线中最经典的后处理：亮度超过阈值的像素在屏幕上"发光扩散"，模拟真实相机的镜头散射 + 人眼眩光效应。常见于游戏（所有 AAA 引擎都有），视觉提升显著且 HDR 受益最大。

### 2.2 用户期望的体验

```lua
local Gfx = require("Light.Graphics")
Gfx.HDR.Enable(1920, 1080)        -- 先启用 HDR
Gfx.HDR.SetExposure(1.0)

Gfx.Bloom.Enable()                 -- 一键启用（复用 HDR RT 大小）
Gfx.Bloom.SetThreshold(1.0)        -- 只有亮度 > 1.0 的像素参与 bloom
Gfx.Bloom.SetIntensity(0.8)        -- 合成强度 80%
Gfx.Bloom.SetRadius(0.7)           -- 扩散半径 70%
-- 正常绘制 sprite / lit；带光源/高亮的像素自动发光
```

### 2.3 边界 / 必做 vs 不做

#### 必做（In Scope）

| 能力 | 说明 |
|------|------|
| **Bright Pass** | 按亮度阈值提取高亮像素 (Luminance threshold + soft knee) |
| **Downsample pyramid** | 5 级（可配 2..8）逐级 1/2 下采样 + 13-tap filter |
| **Upsample pyramid** | 反向逐级 ×2 上采样 + tent 3x3 filter |
| **Composite** | 合成回 HDR RT（加法 blend，在 tonemap 前） |
| **参数控制** | Threshold / Intensity / Radius / Levels |
| **GL33 后端实现** | 桌面 GL 3.3 + GL ES 3.0 shader |
| **Legacy fallback** | 1.x 后端 `IsSupported() = false`，所有 API no-op |
| **Resize 跟随** | HDR RT Resize 时 Bloom pyramid 同步重建 |
| **Lua API** | Light.Graphics.Bloom 子表（12+ 函数）|
| **smoke 覆盖** | scripts/smoke/bloom.lua（≥ 20 PASS） |
| **demo_bloom** | 交互式调参（threshold/intensity/radius） |
| **6 平台 CI 绿** | Windows / macOS / Linux / Android / iOS / Web |

#### 不做（Out of Scope）

| 排除 | 原因 |
|------|------|
| Lens flare / starburst / dirt texture | 与 Bloom 正交，独立 Phase |
| Anamorphic bloom（方向性拉长） | 电影感强但 2D 场景受益弱 |
| HDR 相机自动曝光（auto-exposure） | 属 Phase E.5（eye adaption）范畴 |
| Physically-based bloom LUT / energy conservation | 过度设计；简单 additive 足够 |
| 与 Batch renderer 的深度合成 | Bloom 是 full-screen pass，不需要深度 |

### 2.4 性能目标

- 1080p / 5 级 pyramid / RTX 级 GPU：< 0.6ms（整个 Bloom pipeline）
- 1080p / 5 级 pyramid / Intel UHD：< 2.5ms
- 移动端 720p / 4 级 pyramid / 中端 GPU：< 3ms

以上均基于 RGBA16F 存储（与 HDR RT 一致）。

---

## 3. 智能决策 — 歧义消解

### 3.1 Bloom 算法选型

| 方案 | 质量 | 复杂度 | 推荐度 |
|------|------|--------|--------|
| **COD AW (Sledgehammer 2014)** | 高（好莱坞标准） | 中 | ✅ 采用 |
| Kawase Bloom（单通道多 pass） | 中 | 低 | 已过时 |
| Gaussian 分离（H + V blur） | 中偏低（有光环） | 中 | 不推荐 |
| Compute shader mipchain | 最高 | 高 | GL33 无 CS，跳过 |

**决策**：采用 COD AW 方案 — 13-tap downsample + tent upsample + additive composite。GL3.3 / GLES3.0 通吃，质量近乎 AAA 游戏水平。

### 3.2 Pyramid 层数

| Levels | 1080p 视觉 | 性能 | 推荐场景 |
|--------|-----------|------|----------|
| 2 | 小范围 glow | 最快 | 手游 / 调试 |
| **5**（默认） | 平衡 | 1.2ms | 默认；覆盖 ≈ 32px blur radius |
| 7 | 大范围柔和 | 2ms | AAA 游戏标准 |
| 8 | 影视级 | 3ms+ | 过度；溢出 2K 屏 |

**决策**：默认 5 级，可配 2..8。

### 3.3 Bright Pass 曲线

| 方案 | 公式 | 缺点 |
|------|------|------|
| Hard threshold | `L > T ? color : 0` | 阈值附近有阶跃，瞬时闪烁 |
| **Soft knee**（采用） | Unity 风：knee-blend + 平滑过渡 | 无 |

**决策**：Soft knee 方案（knee 宽度 = threshold * 0.5 常量，不做可配）。

### 3.4 Bloom RT 像素格式

| 格式 | 优势 | 劣势 |
|------|------|------|
| RGBA16F（与 HDR 一致） | 保留 HDR 能量 | 4 bytes/texel × 5 级 |
| R11G11B10F | 比 RGBA16F 少 50% 存储 | 某些 GL 驱动不支持 |
| RGBA8 | 最小 | clip 到 LDR，bloom 失真 |

**决策**：RGBA16F（与 HDR RT 一致，内存可控）。

### 3.5 合成方式

| 方案 | 实现 | 特点 |
|------|------|------|
| **GL 硬件 blend**（采用） | `glBlendFunc(GL_ONE, GL_ONE)` 直接 add | 1 pass，最快 |
| Composite shader | 手动 sample 再 blend | 支持 soft mix，但 overhead |

**决策**：硬件 blend。简单 additive 效果已好，未来若需 soft mix 可升级。

### 3.6 集成点

**选 A**（HDRRenderer 内部集成）：`HDRRenderer::EndScene()` 前调 `BloomRenderer::Process(sceneFbo, sceneTex, w, h)` → bloom 管线输出合成回 sceneTex → 继续 tonemap。

**选 B**（独立 Lua 调用）：Lua 层手动调 `Bloom.Process()`，HDR 不感知 Bloom 存在。

**决策**：**选 A**。Bloom 逻辑应默认挂在 HDR 流水线上，用户只需 `Bloom.Enable()`，无需关心调用顺序。模块解耦仍通过 `BloomRenderer::IsEnabled()` 查询。

### 3.7 Lua API 粒度

Lua 用户需要调参但不需要底层控制（如 GL 细节 / FBO）。API 围绕 **"如何调光效"** 设计，不暴露 pyramid 细节。

```lua
Bloom.Enable()          -- 无参数，复用 HDR 设置
Bloom.Disable()
Bloom.IsEnabled()
Bloom.IsSupported()
Bloom.Resize(w, h)      -- 手动重建（通常自动跟随 HDR.Resize）

-- 参数
Bloom.SetThreshold(t)   -- 默认 1.0
Bloom.GetThreshold()
Bloom.SetIntensity(i)   -- 默认 0.8
Bloom.GetIntensity()
Bloom.SetRadius(r)      -- 默认 0.7，范围 0..1
Bloom.GetRadius()
Bloom.SetLevels(n)      -- 默认 5，范围 2..8
Bloom.GetLevels()
```

共 **11 函数**（对齐 HDR 模块的 12 函数密度）。

---

## 4. 疑问澄清 / 需用户确认

以下 2 点留给用户决策，不基于现有项目默认假设：

### 4.1 是否需要默认 enable 开关

选项 A：`Bloom.Enable()` 必须显式调（与 HDR 对齐）。默认关闭。
选项 B：`Bloom.AutoEnable = true` 注册后自动随 HDR 启用。

**推荐 A**（与 HDR 一致，显式优于隐式）。

### 4.2 是否要在 demo_hdr 中加 Bloom 交互（B 键切换）

选项 A：demo_hdr 不改，单独做 `samples/demo_bloom/`。
选项 B：demo_hdr 加 B 键切 Bloom，保持一份"HDR 全家桶演示"。

**推荐 B**（单一入口，用户可一次看完 HDR+Tonemap+Bloom 全部效果）。同时单独做 `demo_bloom/` 展示参数调节。

---

## 5. 验收标准（可测）

| # | 标准 | 测试方式 |
|---|------|---------|
| 1 | 6 平台 build 通过 | CI 绿 |
| 2 | Bloom.IsSupported 在 GL33 下 `true`，Legacy `false` | smoke.lua |
| 3 | Enable/Disable/Resize 生命周期无 leak | smoke.lua + valgrind（可选） |
| 4 | 所有 11 Lua 函数 signature 正确 | smoke.lua module surface |
| 5 | 参数边界：threshold < 0 → clamp 0；intensity < 0 → clamp 0 | smoke.lua |
| 6 | Levels 边界：`SetLevels(0/1)` → 回退 2；`>8` → clamp 8 | smoke.lua |
| 7 | HDR disabled 时 `Bloom.Enable()` 返回 false + warn | smoke.lua |
| 8 | Tonemap 前合成：输出像素在 HDR RT 上 | 运行时目测（demo） |
| 9 | demo_bloom 交互 threshold/intensity/radius 参数生效 | 视觉验收（用户） |

---

## 6. 项目特性对齐

| 维度 | E.4 Bloom 策略 |
|------|---------------|
| 命名风格 | BloomRenderer 命名空间（同 HDRRenderer） |
| Lua 子表 | Light.Graphics.Bloom（同 Light.Graphics.HDR） |
| Legacy fallback | 所有 API 静默 no-op（同 HDR） |
| CI smoke | 加入 Windows runtime smoke 列表（同 hdr.lua） |
| 文档风格 | ALIGNMENT + DESIGN + TASK + ACCEPTANCE × N + FINAL + TODO（同 E.3） |
| Commit 粒度 | 3 个原子 commit（E.4.1 Backend / E.4.2 Module / E.4.3 Lua API）+ 1 docs commit |

---

## 7. 最终共识（用户确认）

- ✅ Bloom 算法：COD AW 风格（bright pass + 5 级 pyramid + tent upsample + additive）
- ✅ RT 格式：RGBA16F（与 HDR 一致）
- ✅ 参数：Threshold / Intensity / Radius / Levels（4 个可调）
- ✅ 集成点：HDRRenderer::EndScene 内部调用，Lua 层解耦
- ✅ **默认启用开关**：**选 B — 自动启用**（HDR.Enable 后自动拉起 Bloom）
  - 内部加 `BloomRenderer::SetAutoEnable(bool)` flag，默认 true
  - HDRRenderer::Enable 成功 → 自动调 BloomRenderer::Enable(w, h)
  - HDRRenderer::Disable → 自动调 BloomRenderer::Disable
  - HDRRenderer::Resize → 自动调 BloomRenderer::Resize(w, h)
  - Lua 层暴露 `Bloom.SetAutoEnable(false)` 供进阶用户关闭联动
  - 即便联动启用，Lua 仍可 `Bloom.Disable()` 单独关 Bloom（保留 HDR）
- ✅ **demo_hdr B 键 + 独立 demo_bloom**：都做
  - demo_hdr 加 B 键切换 Bloom ON/OFF（全家桶一站式演示）
  - 独立 `samples/demo_bloom/` 专注 threshold / intensity / radius / levels 交互调参
- ✅ 拆 3 个原子任务（E.4.1 / E.4.2 / E.4.3）

### 因"自动启用"引入的额外 API

Lua 子表追加 2 函数（原 11 → 13 函数）：

```lua
Bloom.SetAutoEnable(flag)   -- 设置 HDR.Enable 时是否自动拉起 Bloom (默认 true)
Bloom.GetAutoEnable()       -- 查询当前自动启用 flag
```

Align 完成。进入 Design 阶段。
