# Phase E.12 Temporal SSR — ALIGNMENT 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）
> **目标**：模糊需求 → 精确规范
> **基线**：Phase E.11 Bilateral SSR Blur（commit `ebd069b`，CI 6/6 green，main HEAD）
> **主题**：在 Phase E.11 bilateral blur 基础上叠加 **时序累积降噪（temporal accumulation）**，进一步消除 SSR raw 的高频噪点和 ray march 阶梯感

---

## 1. 项目上下文分析

### 1.1 现有管线（Phase E.11 已交付）

```
HDR color (full-res, RGBA16F)    HDR depth (full-res)
    │                                    │
    ├──(MRT normal slot 1)               │
    │           │                        ▼
    │           │              BlitHDRDepthToSSAO
    ▼           ▼                        │
SSR raw  ◄── ssrDepthTex (full-res, NEAREST) ◄┘
    │ RGBA16F full-res (reflectTex)
    │
    ▼
[Phase E.10/E.11] half-res ping-pong blur
    │ Gaussian (E.10) / Bilateral (E.11) 双模式
    │ blurTexs[0] ⇄ blurTexs[1]
    ▼
SSR Composite (HDR += reflect.rgb * reflect.a * intensity)
    │ bilinear upscale 到 full-res
    ▼
HDR color
```

### 1.2 Phase E.11 后仍存在的问题

1. **ray march 阶梯感**：64 步线性步进在 thickness 边界附近会出现 1-pixel 锯齿，单帧静态可见
2. **粗糙表面噪点**：未来若引入 BRDF 重要性采样（roughness-aware），噪点会放大；temporal 是配套必需
3. **细小高光闪烁**：动态场景下 1-pixel 反射点在不同帧可能跳动（aliasing），bilateral blur 无法解决
4. **静态画面仍有视觉瑕疵**：blur 是空间维度平滑，不能替代时序累积

### 1.3 行业参考：Temporal SSR 标准做法

| 方案 | 优势 | 劣势 |
|---|---|---|
| **TAA-style velocity buffer** | 动静都能 reproject | 需要 G-buffer velocity 通道，影响整个 HDR pipeline |
| **Reverse reprojection from depth** | 无需 G-buffer 改动，仅 SSR 模块自包含 | 仅静态几何 reproject 准确，动态物体反射会 ghost |
| **Frame-to-frame blend without reproj** | 极简 1 行 mix(history, current, alpha) | 镜头转动立即模糊残影 |

**ChocoLight 适配选择**：**Reverse reprojection from depth + jitter + neighborhood clip**
- 不改 G-buffer / 不改 HDR pipeline
- 仅 SSRRenderer + render_backend.h + Lua API 局部增量
- 配合 jitter 让 ray march 在多帧之间累积不同采样位置

### 1.4 当前 backend 接口差距

```cpp
// 现状（@ChocoLight/include/render_backend.h:993-998）
virtual void DrawSSR(uint32_t depthTex, uint32_t normalTex, uint32_t hdrTex,
                     uint32_t dstFbo, int w, int h,
                     const float* projMat4, const float* invProjMat4,
                     int maxSteps, float stepSize, float thickness,
                     float maxDist, float edgeFade) {}
// 缺：jitter offset (2 floats)

// 完全缺失（Phase E.12 需新增）
virtual bool CreateSSRHistoryRT(...) { return false; }
virtual void DeleteSSRHistoryRT(...) {}
virtual void DrawSSRTemporal(uint32_t curReflectTex, uint32_t historyTex,
                             uint32_t depthTex, uint32_t prevDepthTex,
                             uint32_t dstFbo, int w, int h,
                             const float* curViewProj, const float* prevViewProj,
                             const float* invCurProj,
                             float blendAlpha, int rejectionMode) {}
```

### 1.5 SSRRenderer 现状状态字段

```@e:\jinyiNew\Light\ChocoLight\src\ssr_renderer.cpp:80-102
struct State {
    bool    enabled, supported, autoEnable;
    int     srcW, srcH;
    uint32_t depthFbo, depthTex;        // SSR 独立 depth (full-res)
    uint32_t reflectFbo, reflectTex;    // SSR raw output (RGBA16F full-res)
    int     maxSteps; float stepSize, thickness, maxDistance, intensity, edgeFade;
    bool    blurEnabled;                // Phase E.10
    uint32_t blurFbos[2], blurTexs[2];  // Phase E.10 half-res ping-pong
    int     blurW, blurH;               // Phase E.10
    float   blurRadius;                 // Phase E.10
    bool    bilateralEnabled;           // Phase E.11, 默认 true
    float   blurDepthSigma;             // Phase E.11, 默认 200
};
```

**Phase E.12 需追加**：
- `bool     temporalEnabled` (默认 ?)
- `float    temporalAlpha`   (history 权重, 默认 ?)
- `int      rejectionMode`   (0=depth-only, 1=neighborhood-clip, 默认 1)
- `int      jitterPattern`   (0=off, 1=Halton-2,3-8, 2=Halton-2,3-16, 默认 1)
- `uint64_t frameCounter`    (内部, 选 jitter 序列下标)
- `uint32_t historyFbo, historyTex`  (RGBA16F)
- `uint32_t prevDepthFbo, prevDepthTex` (用于 rejection；可选)
- `float    prevViewProj[16]` (上一帧矩阵缓存)
- `bool     hasPrevViewProj` (首帧标志)

### 1.6 资源开销估算（1080p）

| 资源 | 分辨率 | 格式 | 大小 |
|---|---|---|---|
| historyTex | full-res | RGBA16F | ~16 MB |
| historyTex | half-res | RGBA16F | ~4 MB |
| prevDepthTex | full-res | DEPTH24 | ~6 MB |
| prevDepthTex | 不存（用 hist alpha 通道判定） | — | 0 |
| jitter table | CPU 静态 | 2 floats × 16 | 128 B |

---

## 2. 用户原始需求

> "B — Temporal SSR<br>
> Phase E.12: jitter + reprojection 时序降噪。质量飞跃但需要 velocity buffer + history RT，3-5 天工时。复杂度高。"

**关键词解析**：
- **"jitter"** → 必须做（ray march 起点 jitter，让多帧采样位置错开）
- **"reprojection"** → 必须做（reverse reprojection from depth，复用 G-buffer 不引入 velocity）
- **"质量飞跃"** → 默认 Temporal=ON，与 Phase E.11 bilateral 默认 ON 风格一致
- **"复杂度高"** → 用户已知，接受 3-5 天工时

---

## 3. 边界确认

### 3.1 在范围内（Phase E.12 必交付）

| 项 | 内容 |
|---|---|
| Backend 接口 | `CreateSSRHistoryRT` / `DeleteSSRHistoryRT` / `DrawSSRTemporal` 3 函数 |
| Backend 接口扩展 | `DrawSSR` 增 2 参（jitterX, jitterY） |
| Shader | 新 `FS_SSR_TEMPORAL`（GLES3 + GL33 双 profile） |
| Shader 修改 | `FS_SSR` 增 jitter uniform，ray march 起点偏移 |
| SSRRenderer | history RT 管理 + prev viewproj 缓存 + Halton 序列 + Process 流水线插入 |
| Lua API | 新 4-6 函数（Set/Get × {TemporalEnabled, TemporalAlpha, RejectionMode}） |
| smoke | 60 → 75-80 检查点 |
| demo_ssr | T 键切 Temporal / U 键调 alpha / R 键切 rejection / J 键切 jitter pattern |
| docs | 7 件套（同 Phase E.11） |

### 3.2 不在范围（Phase E.12+ / 后续）

| 项 | 说明 |
|---|---|
| G-buffer velocity attachment | 留给完整 TAA Phase（HDR-wide），SSR 不引入 |
| 动态物体精准 reprojection | 反射动态物体 ghosting 是 reverse-reproj 已知限制，TODO 说明 |
| Roughness-aware temporal weight | 留给 Phase E.13 (roughness-aware blur) 联动 |
| Disocclusion fill via blur | 当前用 fallback to current 即可，不做 fancy fill |
| Variance-aware adaptive alpha | 留 Phase E.14 |
| Spatial-Temporal joint filter (SVGF / A-trous) | 留 Phase E.15+ |

### 3.3 兼容性

- **向后兼容**：默认 `TemporalEnabled = true`，但旧 demo 不调用任何 Temporal API 时行为完全不变（temporal 仅在 SSR.Enable 后启用）
- **Legacy backend**：`SupportsSSR=false` 时 Temporal 完全无效，不分配任何 history 资源
- **Phase E.10/E.11 联动**：Temporal 在 SSR raw 和 blur 之间工作，blur enabled/bilateral enabled 任意组合都正常

---

## 4. 需求理解（对现有项目）

### 4.1 SSR 管线插入点（推荐方案 A：raw → temporal → blur）

```
SSR raw (jittered)
   │
   ▼ DrawSSRTemporal(curRaw, history, depth, prevDepth, viewProj, prevViewProj, ...)
   │   ├─ reproject prev pixel via depth → prev UV
   │   ├─ history sample
   │   ├─ rejection (depth diff / neighborhood clip)
   │   └─ blend = mix(history, current, alpha) on accept; current on reject
   ▼
historyTex (本帧 = 下一帧的 history) ───┐
   │                                  │
   │ copy or alias 到下次              │（ping-pong 或 swap pointer）
   │                                  │
   ▼                                  │
SSR blur (bilateral / gaussian)       │
   │                                  │
   ▼                                  │
SSR composite ──────────► HDR     ────┘
```

**为什么 temporal 在 blur 之前**：
- temporal 输入信号需要保持高频细节供 reprojection 准确
- blur 之后再 temporal 会 over-smooth + 历史不准确
- 工业惯例（UE / Unity HDRP）也是 temporal 先于空间滤波

### 4.2 Reverse Reprojection 数学

```glsl
// 当前像素 → world → prev clip space
vec3 viewPos = ReconstructViewPos(uv, depth, invProj);  // 当前帧
vec4 worldPos = invView * vec4(viewPos, 1.0);
vec4 prevClip = prevProj * prevView * worldPos;
prevClip.xyz /= prevClip.w;
vec2 prevUV = prevClip.xy * 0.5 + 0.5;
```

**优化**：直接缓存 `prevViewProj * curViewProjInv`（单 4×4 矩阵），shader 内 1 次 matmul。

### 4.3 Jitter 序列：Halton-2,3 8-sample（推荐）

| 帧 | jitterX | jitterY |
|---|---|---|
| 0 | 0.0000 | 0.0000 |
| 1 | -0.5000 | 0.3333 |
| 2 | 0.2500 | -0.3333 |
| 3 | -0.2500 | 0.1111 |
| 4 | 0.3750 | -0.1111 |
| 5 | -0.3750 | 0.4444 |
| 6 | 0.1250 | -0.4444 |
| 7 | -0.1250 | 0.2222 |

range：±0.5 pixel（标准 TAA jitter 范围），通过 `gl_FragCoord.xy + jitter` 影响 ray march 起点 UV。

### 4.4 Rejection 策略对比

| 模式 | 实现 | 抗 ghost | 复杂度 |
|---|---|---|---|
| 0 = depth-only | `abs(curDepth - prevDepth) > threshold → reject` | 弱（颜色 ghost 仍存在） | 低 |
| 1 = neighborhood clip | `clip historyColor to neighborhood AABB of currentColor` | 强 | 中（需 9-tap min/max） |
| 2 = depth + neighborhood | 两者结合 | 最强 | 高 |

**推荐**：默认 mode=1（neighborhood clip）；mode 0 作为对比备选；mode 2 留 TODO 评估。

---

## 5. 用户视角期望

| 期望 | 验收手段 |
|---|---|
| SSR 静态画面更干净 | demo_ssr 截屏对比 Temporal ON/OFF（用户参与真机验收） |
| ray march 阶梯感减弱 | demo 中 V/T 键观察 |
| 动态镜头转动不糊 | 转动相机时 reproject 工作，画面跟手 |
| 反射物体移动有 ghost | 已知限制（TODO 说明，由 G-buffer velocity 解决，留 Phase E.13+） |
| API 简洁 | 默认开启即用，调参只在需要时 |
| smoke 全绿 | CI 6/6 green |

---

## 6. 算法设计预览

### 6.1 历史 RT 管理（ping-pong）

```cpp
uint32_t historyFbos[2], historyTexs[2];
int     historyIdx = 0;  // 当前帧写入 [historyIdx], 下帧读 [historyIdx]

// 每帧 Process:
int writeIdx = historyIdx;
int readIdx  = 1 - historyIdx;
DrawSSRTemporal(curReflect, historyTexs[readIdx], ..., historyFbos[writeIdx], ...);
// 把 writeIdx 输出当作下次 blur 输入（或直接进 composite）
historyIdx = 1 - historyIdx;  // swap
```

### 6.2 首帧处理

`hasPrevViewProj=false` 时，shader 强制 `outColor = currentColor`（disable temporal 直到第二帧）。

### 6.3 Resize 时

清空 history（重新分配 RT，标志 `hasPrevViewProj=false`），下一帧从头累积。

---

## 7. 集成期望

### 7.1 与 Phase E.10/E.11 联动

| 用户配置 | 行为 |
|---|---|
| Temporal=OFF | 完全是 Phase E.11 行为（blur + bilateral） |
| Temporal=ON, Blur=OFF | temporal 直接进 composite（无 blur） |
| Temporal=ON, Blur=ON, Bilateral=OFF | temporal → gaussian blur → composite |
| Temporal=ON, Blur=ON, Bilateral=ON | temporal → bilateral blur → composite（推荐组合） |

### 7.2 与 HDR 联动

- `HDRRenderer::Disable` → SSRRenderer.Disable → 释放所有 history RT
- `HDRRenderer::Resize` → SSRRenderer.Resize → 重建 history RT，清 prev 矩阵

### 7.3 性能预算

| 1080p 开销估算 | 增量 |
|---|---|
| historyRT 内存 | ~16 MB（full-res RGBA16F）/ ~4 MB（half-res） |
| GPU 时间 | +0.2~0.4 ms（temporal pass full-screen） |
| jitter | ~0 ms（CPU 计算 + 1 uniform） |

**目标**：总开销 < +0.5 ms，可接受。

---

## 8. 疑问澄清（关键决策点）

### Q1：history RT 分辨率

| 选项 | 优势 | 劣势 |
|---|---|---|
| **A（推荐）** full-res RGBA16F (~16 MB) | 最大保真度，未来 roughness-aware 直接对接 | 内存占用 |
| B half-res RGBA16F (~4 MB) | 与 blur RT 一致，轻量 | 反射细节损失，ghost 更明显 |

**推荐 A**：SSR 已是高端特性，full-res 保真合理。

### Q2：默认 TemporalEnabled

| 选项 | 推荐场景 |
|---|---|
| **A（推荐）** 默认 true | 与 E.11 BilateralEnabled=true 风格一致，质量优先 |
| B 默认 false | 与 BlurEnabled=false 风格一致，opt-in |

**推荐 A**：用户拍板 "B - Temporal SSR" 已表达质量倾向；与 Bilateral 默认 ON 一致。

### Q3：Rejection 模式默认

| 选项 | 默认值 |
|---|---|
| **A（推荐）** mode=1 neighborhood clip | 抗 ghost 强 + 实现成熟 |
| B mode=0 depth-only | 极简，但相机转动时 ghost 明显 |

**推荐 A**：业界标准做法，效果显著。

### Q4：Jitter pattern 默认

| 选项 | 帧数收敛 |
|---|---|
| **A（推荐）** Halton-2,3 8-sample | 8 帧覆盖完整 jitter，足够 SSR |
| B Halton-2,3 16-sample | 16 帧收敛，质量略好但启动响应慢 |
| C 关闭 jitter | 仅 temporal blend，无超采样收益 |

**推荐 A**：8-sample 是 TAA 标准，启动响应快（8 帧≈140ms @ 60fps）。

### Q5：API 函数数量

| 选项 | 函数 |
|---|---|
| **A（推荐）** 6 函数（3 setter + 3 getter） | Set/Get × {TemporalEnabled, TemporalAlpha, RejectionMode}；jitter pattern 内部硬编码 Halton-8 |
| B 8 函数 | 加 Set/GetJitterPattern |

**推荐 A**：jitter pattern 切换实际使用率低（用户调试场景），硬编码 Halton-8 更清爽，减少 API 噪音。

### Q6：默认 TemporalAlpha（history blend 权重）

| 选项 | 视觉效果 |
|---|---|
| **A（推荐）** 0.9 | 业界标准 TAA 默认，10 帧收敛主体 |
| B 0.92 | 更稳但 lag 略增 |
| C 0.85 | 响应快，但去噪弱 |

**推荐 A**：clamp [0.5, 0.99]，给用户调参空间。

---

## 9. 行业参考

| 引擎 / 论文 | temporal alpha | jitter | rejection |
|---|---|---|---|
| UE4 TAA | 0.95 | Halton-2,3 8 | neighborhood clip + depth |
| Unity HDRP | 0.92 | Halton-2,3 8 | neighborhood clip |
| Filament | 0.9 | Halton-2,3 8 | neighborhood clip |
| Frostbite SSR (Stochastic SSR, 2015) | 0.9 | Halton + blue noise | bbox clip + variance |

---

## 10. 约束

### 10.1 技术约束

- **GLES3 兼容**：所有 shader 必须双 profile（GL3.3 core + GLES3.0），无 compute shader / geometry shader
- **零 G-buffer 改动**：不引入 velocity attachment，影响范围限于 SSRRenderer
- **零 HDR pipeline 改动**：HDRRenderer / hdr_renderer.cpp 不改
- **后端无关**：所有 GL 操作经 RenderBackend 虚接口

### 10.2 代码约束

- 与 Phase E.10/E.11 风格一致：State 字段、setter/getter 模式、Process 内部自检模式
- shader 命名 `FS_SSR_TEMPORAL` 沿用 `FS_SSR_xxx` 系列
- Lua API 命名沿用 `SetTemporalXxx / GetTemporalXxx`

### 10.3 测试约束

- smoke 不能依赖真实反射结果（headless 渲染无图像 GT），仅测 API surface + 默认值 + clamp + round-trip + 联动
- demo 视觉验收由用户在真机执行

---

## 11. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| reprojection 矩阵反推数值精度 | 中 | 中 | 用 double 精度 CPU 计算 invProj，shader 内 float matmul |
| 首帧 history 全 0 导致 1-frame 黑屏 | 中 | 低 | hasPrevViewProj=false 时强制 outColor=current |
| neighborhood clip shader 性能 | 低 | 中 | 9-tap 可接受，跟 SSAO blur 同量级 |
| Resize 时 history 残留 | 低 | 中 | Resize 内强制释放重建 + reset hasPrevViewProj |
| Bilateral + Temporal 联动权重失配 | 低 | 低 | temporal 在 blur 前，blur 在累积后的稳定信号上工作 |

---

## 12. 验收标准（预案）

### 12.1 功能验收

- [ ] Backend 3 新接口 + 1 接口扩展实现，0 编译 error
- [ ] FS_SSR_TEMPORAL shader 双 profile 编译通过（CI 6/6 green）
- [ ] FS_SSR jitter 注入不破坏 Phase E.10/E.11 行为（temporal=off 时画面与 main 完全一致）
- [ ] SSRRenderer State 字段 + setter/getter + Process 流水线集成
- [ ] Lua API 6 新函数注册到 ssr_funcs[]
- [ ] smoke 75-80 检查点全过（CI Windows runtime smoke）
- [ ] demo_ssr T/U/R 键工作，HUD 显示 Temporal 状态

### 12.2 质量验收

- [ ] 默认 TemporalEnabled=true，TemporalAlpha=0.9，RejectionMode=1
- [ ] clamp 边界正常（alpha [0.5, 0.99]，rejection mode {0, 1}）
- [ ] round-trip 测试通过（set 后 get 一致）
- [ ] 联动测试：与 blurEnabled / bilateralEnabled 任意组合无 crash

### 12.3 文档验收

- [ ] ALIGNMENT / CONSENSUS / DESIGN / TASK / ACCEPTANCE / FINAL / TODO 全 7 件
- [ ] API_REFERENCE.md SSR 函数数 28 → 34
- [ ] demo_ssr/README.md 新键位 + Temporal 描述
- [ ] CHANGELOG / 主 README 入口

### 12.4 CI 验收

- [ ] 6 平台 build success（windows / linux / macos / android / ios / web）
- [ ] Windows runtime smoke `[Phase E.12] 通过 N / 失败 0`
- [ ] commit 简洁（单 PR 或 5-8 commits）

---

## 13. 需用户拍板的最终问题

**Q1**：history RT 分辨率 → **推荐 A：full-res**
**Q2**：默认 TemporalEnabled → **推荐 A：true**
**Q3**：Rejection 模式默认 → **推荐 A：neighborhood clip (mode=1)**
**Q4**：Jitter pattern → **推荐 A：Halton-2,3 8-sample 硬编码**
**Q5**：API 函数数量 → **推荐 A：6 函数，jitter 不暴露**
**Q6**：默认 TemporalAlpha → **推荐 A：0.9**

**全推荐 A 组合 = 行业标准 TAA-style temporal SSR，质量优先，最小 API 表面，可控复杂度**。

需用户拍板：**接受全 A？还是修改某项？**

---

## 14. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v0.1 | 2026-05-12 | 初稿 — 项目上下文 + 6 决策点 + 推荐组合 |

---

**下一步**：用户拍板 Q1-Q6 → 写 CONSENSUS_PhaseE_12.md 锁定方案 → 进入阶段 2 Architect。
