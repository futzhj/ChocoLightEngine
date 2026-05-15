# Phase E.16 Camera-only Motion Blur — ALIGNMENT

> 6A 工作流 · 阶段 1 · Align
> 基线：Phase E.15 commit `4eb22c7`（CI run 25894807417 6/6 success @ 524s）

---

## 1. 项目上下文（已分析）

### 1.1 当前 velocity 系统（Phase E.13~E.15 累积成果）

```
3D shader VS (4 份: unlit / PBR / skin / morph)
  ├ uniform: uMVP, uModel, uPrevViewProj, uPrevModel
  ├ vCurClip  = gl_Position             = curVP × curM × pos
  └ vPrevClip = uPrevViewProj × uPrevModel × pos
    ↓ varying
3D shader FS (4 份)
  ├ uVelocityFormat / uVelocityScale (Phase E.14)
  ├ delta = (curUV - prevUV) = (vCurClip.xy/w - vPrevClip.xy/w)
  └ FragVelocity (MRT slot 2) = encode(delta) ← combined velocity
    ↓
HDR FBO MRT
  ├ slot 0: RGBA16F sceneTex
  ├ slot 1: RG16F   normalTex (Phase E.8)
  ├ slot 2: RG16F/RG8 velocityTex (Phase E.13/E.14) ★ combined
  └ depth RBO
    ↓
消费者
  ├ SSR Temporal (E.12+E.14): sample velocityTex → reproject prevUV
  └ MotionBlur   (E.15):       sample velocityTex → blur 沿运动方向
```

### 1.2 关键代码位置（已 grep 锁定）

| 模块 | 文件 | 关键 |
|------|------|------|
| RenderBackend 接口 | `render_backend.h:130-271/1142-1183` | VelocityFormat enum + Get/Set + Phase E.15 4 个 MotionBlur 虚接口 |
| GL33 backend | `render_gl33.cpp:124-265 / 477-614 / 2549-2918 / 3902-4079` | 4 VS×2 (GLES3+GL33) + State 字段 + CreateHDRFBO MRT 设置 + UploadVelocityUniforms |
| HDRRenderer | `hdr_renderer.cpp:26-120 / 308-310` | velocityFormat 状态 + EndScene 后处理链 |
| SSRRenderer (Temporal 消费) | `ssr_renderer.cpp:351-465` | DrawSSRTemporal(... GetHDRVelocityTex(hdrFbo) ...) |
| MotionBlurRenderer | `motion_blur_renderer.cpp` | Process(hdrFbo, hdrTex) → DrawMotionBlur |

### 1.3 Phase E.15 给我们留下的扩展空间

- ✅ MRT slot 0/1/2 已用，**slot 3 仍空闲**（GL3.3 / GLES3 都支持 ≥ 4）
- ✅ shader 编/解码框架（Decode/Encode + Format/Scale）已就位 → 第二张 velocity RT 可零成本复用
- ✅ MotionBlurRenderer 是「纯壳模式」 — 加 mode 字段只改 state，无渲染管线改动
- ✅ Phase E.15 ACCEPTANCE §5 已列「相机+物体合一 motion blur（无 camera-only 模式）」为已知限制

---

## 2. 原始需求理解

### 2.1 用户表述（从交互中提炼）

> 「拆分 camera velocity 与 object velocity 到两路（GBuffer 扩展或两通道单 RT），让用户可选『相机静止物体动 / 物体静止相机动 / 全开』三档。预计 1.5~2 天。架构上是 Phase E.15 的自然进化，需要 3D shader velocity 写入路径分两路。」

### 2.2 任务边界

**In Scope**:
- 3D shader VS×4 加 `vPrevClipCameraOnly` varying；FS 加第 2 个 velocity 输出
- HDR FBO MRT 增 slot 3 = cameraVelocityTex（与 slot 2 同格式 RG16F/RG8）
- MotionBlur Lua API 增 SetMode/GetMode（mode = 0/1/2 三档）
- shader: object_only 模式做 `v_combined - v_camera`
- 默认 mode=0 = combined（兼容 Phase E.15 行为，零回归）

**Out of Scope**:
- ❌ 不修改 SSR Temporal（数学上 reproject 用 combined velocity 是正确的，无需改）
- ❌ 不暴露 cameraVelocity 给 Lua 或其他模块（仅 MotionBlur 内部使用）
- ❌ 不做 dynamic skinned/morph 物体的特殊 camera-only 处理（公式自然推导）
- ❌ 不变更 Phase E.13 prevModel / prevViewProj 的 CPU 端管理逻辑

### 2.3 验收口径

- 默认 mode=0 时：用户感知与 Phase E.15 完全一致（视觉、性能、API 都无差异）
- mode=1（camera_only）：物体相对屏幕静止时 blur 消失；相机旋转/平移时仍然 blur
- mode=2（object_only）：相机静止物体动时 blur 仍在；相机旋转物体相对屏幕静止时 blur 消失
- VRAM 1080p 增量：RG16F +4MB / RG8 +1MB（cameraVelocityTex）
- 现有 16 个 phase smoke 全部通过（零回归）

---

## 3. 关键决策矩阵

> 优先级：⭐⭐⭐ = 用户必须拍板；⭐⭐ = 已自动决策但需用户确认；⭐ = 工程惯例自动决策

### 决策 1 ⭐⭐⭐ — 拆分实现方案

| 方案 | 说明 | VRAM | shader 改动 | 精度 | 兼容性 |
|------|------|------|-------------|------|--------|
| **A1 双 RT** | HDR FBO MRT slot 3 加 cameraVelocityTex | +1~4MB | VS×4 加 1 varying，FS×4 加 1 输出 | 无损 | 完全 |
| **A2 单 RGBA16F** | 升级 slot 2 为 RGBA16F：xy=combined, zw=camera | +0（替换） | 改动同 A1 + 改 DecodeVelocity 返回 vec4 | 无损 | **破坏 Phase E.13/14 兼容** |
| **A3 推导式** | shader 用 prevViewProj + invCurViewProj + depth 反算 cameraVelocity | 0 | MotionBlur shader 加矩阵 uniform | 远景 depth 累积误差风险 | 完全 |

**推荐**：**A1 双 RT**。理由：
1. VRAM 1~4MB 增量在桌面/主机可接受（移动端 motion blur 默认不开）
2. shader 改动量最小、最直接（4 个 VS 加 1 行；4 个 FS 加 1 行）
3. 精度无损（VS 算 + FS 直存，没有反向解算误差）
4. 向后兼容：mode=0 时 cameraVelocityTex 创建但不读，零行为变化
5. 一致：与 Phase E.13/14 框架（编/解码 + dilation）完全一致

### 决策 2 ⭐⭐ — 第二张 RT 存什么

| 选项 | 优势 | 劣势 |
|------|------|------|
| **存 camera_only** ✅推荐 | 能算 object = combined − camera；camera_only 模式直接采样 | 需要 1 次额外采样（object_only 模式时） |
| 存 object_only | object_only 模式直接采样 | 反推 camera = combined − object，camera_only 模式同样 1 次额外采样 |

**推荐**：**存 camera_only**。理由：
1. camera 运动通常更连续（每帧只小变化），对 RG8 编码更友好
2. object 运动可能很大（爆炸碎片、快速 sprite），更适合从 combined 中减出（防 RG8 截断）
3. 与「camera-only motion blur」业界惯例（赛车、FPS 游戏）一致

### 决策 3 ⭐⭐ — Mode Lua API 表达

| 选项 | 示例 | 优劣 |
|------|------|------|
| **int (0/1/2)** ✅推荐 | `MB.SetMode(1)` | 与 SSR `SetRejectionMode(0/1)`、HDR `SetTonemapper(0/1/2)` 一致 |
| string enum | `MB.SetMode("camera_only")` | 可读但慢（字符串比较）；与现有 API 风格不一致 |
| 双 bool | `EnableCameraBlur(true) + EnableObjectBlur(true)` | 4 状态不直观；与单一概念 mode 不符 |

**推荐**：**int 0/1/2**。

### 决策 4 ⭐⭐ — Mode 数值映射

| 值 | 含义 | 默认 |
|---|------|------|
| 0 | combined（camera + object 合一） | ★默认 |
| 1 | camera_only（仅相机运动） | |
| 2 | object_only（仅物体运动） | |

**推荐**：**默认 0 = combined**。理由：完全兼容 Phase E.15 行为；用户不显式 SetMode 时零变化。

### 决策 5 ⭐⭐ — 影响范围

| 范围 | 决策 |
|------|------|
| **只 MotionBlur** ✅推荐 | SSR Temporal 维持现状（reproject 用 combined velocity 数学正确） |
| 同步影响 SSR | 让 SSR 选择性用 camera_only velocity（高速物体下反射更稳） |

**推荐**：**只 MotionBlur**。理由：
1. SSR Temporal 数学要求是「上一帧屏幕该像素的位置」，combined velocity 才正确
2. 物体反射在屏幕空间确实应该跟着物体动（视觉直觉）
3. 如未来需要 roughness-aware temporal SSR（Phase E.14 TODO §3），再单独迭代

### 决策 6 ⭐ — RT 格式跟随

| 选项 | 决策 |
|------|------|
| 与 combined velocity 同格式（RG16F/RG8 跟随用户 SetVelocityFormat） | ✅推荐 |
| 强制 RG16F | 翻倍 VRAM 增量（4MB → 8MB），无收益 |

**推荐**：**跟随 combined velocity 格式**。复用现有 Phase E.14 编/解码框架。

### 决策 7 ⭐ — RT 创建时机

| 选项 | 决策 |
|------|------|
| **HDR.Enable 时总是创建** ✅推荐 | mode 切换零开销；生命周期与现有 velocityTex 平行 |
| 仅 mode≠0 时创建 | 切换 mode 需要重建 FBO，复杂 |

**推荐**：**总是创建**。VRAM 增量可控；简化生命周期。

### 决策 8 ⭐ — Lua API 函数命名

| 选项 | 决策 |
|------|------|
| **`SetMode(int) / GetMode() → int`** ✅推荐 | 与 SSR `SetRejectionMode` 一致 |
| `SetVelocitySource(int)` | 不直观，Velocity 不是 user-facing 概念 |

**推荐**：`SetMode / GetMode`。

### 决策 9 ⭐ — Mode 常量字符串映射（用于 demo HUD）

| Mode | 显示名 |
|------|--------|
| 0 | "combined" |
| 1 | "camera_only" |
| 2 | "object_only" |

### 决策 10 ⭐ — Backend 接口扩展

新增 1 个 RenderBackend 虚接口：

```cpp
/// Phase E.16 — 查询 HDR FBO 关联的 cameraVelocityTex (slot 3)
/// mode=0 (combined) 时此 tex 创建但不读取
virtual uint32_t GetHDRCameraVelocityTex(uint32_t /*fbo*/) const { return 0; }
```

`CreateHDRFBO` 签名扩展（兼容性：默认参数）：
```cpp
virtual uint32_t CreateHDRFBO(int w, int h,
                               uint32_t* outTex,
                               uint32_t* outNormalTex = nullptr,
                               uint32_t* outVelocityTex = nullptr,
                               VelocityFormat velocityFormat = VelocityFormat::RG16F,
                               uint32_t* outCameraVelocityTex = nullptr);  // ★ E.16
```

`DrawMotionBlur` 签名扩展：
```cpp
virtual void DrawMotionBlur(uint32_t sceneTex,
                            uint32_t velocityTex,            // combined
                            uint32_t cameraVelocityTex,      // ★ E.16: mode 1/2 用
                            uint32_t motionBlurFbo,
                            uint32_t motionBlurTex,
                            uint32_t dstFbo,
                            int w, int h,
                            float strength, int sampleCount,
                            int mode);                       // ★ E.16: 0/1/2
```

---

## 4. 疑问澄清（**唯一需要用户拍板**）

虽然 §3 已自动给出 10 个决策的推荐方案，但有 **1 个关键决策点** 影响整体架构，建议用户确认：

### Q1 ⭐⭐⭐ — 是否接受 A1 双 RT 方案 + camera_only 存储？

**等价问法**：

> Phase E.16 在 HDR FBO 上**新增 1 张 velocity RT（slot 3, 与 slot 2 同格式 RG16F/RG8）专存 camera-only velocity**。1080p 下 +1~4MB VRAM。换取：MotionBlur 模式切换零额外计算、shader 改动最小、精度无损、与 Phase E.13/14 框架完全对齐。

**对比方案**（如果用户拒绝）：

> A3 推导式：shader 内用 invCurViewProj + prevViewProj + depth 反推 camera velocity。0 VRAM 增量，但远景 depth ≈ 1.0 时浮点累积误差可能造成 1~2 像素偏差。

**预估**：用户大概率接受 A1（VRAM 增量小且方案最干净）。

---

## 5. 自动决策汇总（无需用户确认，记录备查）

| # | 决策 | 选择 | 备注 |
|---|------|------|------|
| 1 | 拆分方案 | **A1 双 RT** | 见 §3 决策 1 |
| 2 | 第二张 RT 存什么 | **camera_only** | 见 §3 决策 2 |
| 3 | Lua mode 类型 | **int 0/1/2** | §3 决策 3 |
| 4 | mode 数值含义 | 0=combined / 1=camera / 2=object | §3 决策 4 |
| 5 | 默认 mode | **0 (combined)** | 零回归 |
| 6 | 影响范围 | **只 MotionBlur** | SSR 不动 |
| 7 | RT 格式 | **跟随 combined velocity** | RG16F or RG8 |
| 8 | RT 创建时机 | **HDR.Enable 时总是创建** | 简化生命周期 |
| 9 | Lua API 命名 | `SetMode / GetMode` | 与 SSR 一致 |
| 10 | Backend 接口 | 1 个新虚接口 + 2 个签名扩展 | 见 §3 决策 10 |
| 11 | shader 改动 | 4 VS×2 (GLES3+GL33) 加 1 varying，4 FS×2 加 1 输出 + 1 layout | 共 16 处一致改动 |

---

## 6. 任务范围估算（提前给）

- 实施量：**8~10 子任务（T1~T?）**，预计 **1.5~2 天**
- 文件改动：约 **8 个文件 +400 行代码**（render_backend.h 接口、render_gl33.cpp 8 shader 改 + 2 函数扩展、hdr_renderer.cpp 加 cameraVelocityTex 状态、motion_blur_renderer.cpp 加 mode 字段、light_graphics.cpp 加 2 个 Lua API、smoke 加 1 段、demo 加按键）
- 文档：6A 5 件套 + Light_Graphics.md MotionBlur 段更新 mode

---

## 7. 验收标准

| 项 | 期望 |
|---|------|
| Lua API: `MB.SetMode(0/1/2) / GetMode() → int` | ✅ |
| 默认 mode=0 时与 Phase E.15 行为完全一致 | ✅（视觉、性能、所有 API 不变） |
| smoke 加 mode round-trip + clamp + 默认值检查 ≥ 5 个 PASS | ✅ |
| demo_ssr 按 N 切换 mode（M 仍是 enable，N 切 mode） | ✅ |
| HUD 显示 `MotionBlur: ON|OFF \| mode=1 (camera_only) \| ...` | ✅ |
| GitHub Actions CI 6/6 全绿 | ✅ |
| Lua syntax check (lightc -p) | ✅ |
| 现有 16 个 phase smoke 零回归 | ✅ |

---

## 8. 推进确认

如用户确认 §4 Q1 = 接受 A1 双 RT + camera_only 存储 → 进入 **Architect** 阶段，编写 `DESIGN_PhaseE_16.md`（含 RT 拓扑图、shader 完整 GLSL 伪码、UploadVelocityUniforms 扩展、MotionBlur shader sample 路径切换、ping-pong 时序图等）。
