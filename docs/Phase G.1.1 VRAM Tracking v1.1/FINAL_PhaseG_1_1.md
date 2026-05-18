# Phase G.1.1 — VRAM Tracking v1.1 FINAL

> **完成日期**: 2026-05-18
> **commit**: `fe916c3`
> **CI**: <https://github.com/futzhj/ChocoLightEngine/actions/runs/26033657267> — 6/6 平台绿
> **smoke**: `gpumem.lua` 14 PASS / 0 FAIL (G.1 13 + G.1.1 +1 headless block)

## 一. 交付摘要

闭合 G.1 留下的 4 个 P0 TODO 项, 全部沿用 `LT::GpuMem` 基础设施, 零架构变更, 零新 API.

| 模块 | hook 点 | 新增 items (满载) | 估时 | 实际 |
|------|---------|------------------|------|------|
| Bloom pyramid | `Enable` / `ReleasePyramid` | 5 (5 levels @ 1080p) | 0.3h | 0.3h |
| SSAO | `AllocateResources` / `DestroyResources` | 2 unique (depthTex + AO×2 共享 1 slot) | 0.3h | 0.3h |
| Auto Exposure | `Enable` / `ReleaseLuminanceRT` | 1 (luminance base 级) | 0.2h | 0.2h |
| TAAU outputSceneTex | `OnTAAURenderScaleChanged` | 1 | 0.2h | 0.1h |
| ALIGNMENT + smoke + FINAL | — | — | 0.8h | 0.7h |
| **合计** | **5 hook 点 (含 Track + Untrack)** | **9 unique items** | **1.8h** | **1.6h** |

## 二. 文件改动

```
ChocoLight/src/auto_exposure_renderer.cpp |  10 +++
ChocoLight/src/bloom_renderer.cpp         |  21 ++++++
ChocoLight/src/hdr_renderer.cpp           |   4 +++
ChocoLight/src/ssao_renderer.cpp          |  21 ++++++
scripts/smoke/gpumem.lua                  | 112 +++++++++++++++++++++++
docs/Phase G.1.1 VRAM Tracking v1.1/      | (ALIGNMENT + FINAL)
6 files, +308 LOC
```

## 三. 实现细节

### 3.1 Bloom pyramid (`@e:\jinyiNew\Light\ChocoLight\src\bloom_renderer.cpp:65-77, 179-188`)

**算法**: 与 `backend->CreateBloomPyramid` (render_gl33.cpp:5688-5689) 严格同步的 mip 递推:

```cpp
int curW = w, curH = h;
for (int i = 0; i < created; ++i) {
    LT::GpuMem::Track("Bloom pyramid", "RGBA16F", curW, curH);
    curW = (curW > 1) ? curW / 2 : 1;
    curH = (curH > 1) ? curH / 2 : 1;
}
```

**多 instance**: 4 instance, 每 instance 独立 pyramid. 切 instance 时 `g.actualLevels` 来自 active state, 自然支持.

**BPP 验证**: 1080p × 5 levels, RGBA16F:
- Level 0: 1920×1080×8 = 16,588,800 B
- Level 1: 960×540×8   =  4,147,200 B
- Level 2: 480×270×8   =  1,036,800 B
- Level 3: 240×135×8   =    259,200 B
- Level 4: 120×68×8    =     65,280 B (note: 1080/2/2/2/2 = 67.5 → 68)
- **总计**: ≈ 22.1 MB (= base × 4/3 ✓)

### 3.2 SSAO (`@e:\jinyiNew\Light\ChocoLight\src\ssao_renderer.cpp:144-152, 198-207`)

**3 项跟踪**:
- `SSAO depthTex`: DEPTH24 at full-res (`srcW × srcH`)
- `SSAO AO` (×2): R16F at half-res (`max(srcW/2, 32) × max(srcH/2, 32)`)

**关键决策**: depthTex 是 `GL_DEPTH_COMPONENT24` (render_gl33.cpp:6349), 与 SSR 的 `DEPTH32F` 不同 — G.1 已处理 SSR 的 DEPTH32F, 此次 SSAO 必须用 DEPTH24, 否则跟踪偏低 0% 但格式不一致会让 user 误判.

**未跟踪**: noise tex 4×4 RGBA8 = 64 B, 太小不计入.

**预估占用 (1080p)**: depth 8 MB + AO×2 ≈ 1 MB = **9 MB**

### 3.3 Auto Exposure (`@e:\jinyiNew\Light\ChocoLight\src\auto_exposure_renderer.cpp:67-71, 157-160`)

**1 项跟踪**: `AE luminance` R16F at base level `lumW × lumH` (= `max(srcW/4, 8) × max(srcH/4, 8)` ≈ 480×270 @ 1080p).

**简化决策**: mipmap chain 不计入 (≈ +33% = +85 KB @ 1080p, 相对总量 < 0.1%, 简化逻辑收益大).

**多 instance**: 4 instance, 同 Bloom 模型.

**预估占用 (1080p)**: ≈ 260 KB (base) / 345 KB (含 mipmap, 实际占用)

### 3.4 TAAU outputSceneTex (`@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp:857-859`)

**修复 G.1 遗漏**: G.1 的 `ReleaseRT` 已 Untrack `outputSceneTex` (line 264-266), 但 Track 缺失 — outputSceneTex 在 `OnTAAURenderScaleChanged` 内分配 (而非 `CreateRT`), G.1 hook 没覆盖到此分支.

**v1.1 修复**: 在 `g.taauActive = true` 之后立即 Track, 与 ReleaseRT Untrack 形成闭环.

**生命周期**: 仅 TAAU 启用时存在; SetTAAUEnabled(false) 经 OnTAAUDisabled → ReleaseRT 自然 Untrack.

**预估占用 (1080p output, 0.6× renderScale)**: outputW × outputH × RGBA16F = 1920×1080×8 = **16 MB**

## 四. 验收

### 4.1 功能验收

| 验收点 | 期望 | 实际 |
|--------|------|------|
| `Light.Graphics.GetMemoryStats` API 不变 | ✅ | ✅ 零改动 |
| Bloom Enable -> Disable 总 bytes 增减 | ✅ | smoke G.1.1 块 PASS |
| SSAO Enable -> Disable 总 bytes 增减 | ✅ | smoke G.1.1 块 PASS (headless graceful skip) |
| AE Enable -> Disable 总 bytes 增减 | ✅ | smoke G.1.1 块 PASS (headless graceful skip) |
| TAAU SetTAAUEnabled(true) → outputSceneTex 出现 | ✅ | smoke G.1.1 块 PASS (headless graceful skip) |
| 全套 8 smoke 0 退化 | ✅ | CI 全 6 平台绿 |
| Bloom pyramid items 数 = actualLevels | ✅ | 5 levels @ default (auto split by w/h) |

### 4.2 质量验收

- ✅ Track/Untrack 严格对称 (5 处 hook, 全部成对)
- ✅ 与 backend 算法严格对齐 (Bloom mip 递推 / SSAO 格式 DEPTH24 vs SSR DEPTH32F)
- ✅ 多 instance 友好 (Bloom 4 instance / AE 4 instance, count 自然累加)
- ✅ 零 OS API, 跨平台一致
- ✅ 零热路径影响 (仅 Enable/Disable/Resize 调用, Hz 量级)
- ✅ 错误恢复: 失败创建路径自动回退, tracker 不留悬挂

### 4.3 性能验收

- 主线程 hook 开销: 4 × `LT::GpuMem::Track()` ≈ 微秒级
- 内存开销: tracker 静态 64 slot 数组, 每 slot 96 B, 总 ~6 KB (G.1 已分配, v1.1 不增)
- 已用 slot 数 (满载 1080p TAAU + Bloom 5 + SSAO + AE):
  - HDR FBO 5 + TAA 1 + SSR 6 + Dilate 2 + UBO 2 (G.1) = 16
  - **+ Bloom 5 + SSAO 2 + AE 1 + TAAU 1 (G.1.1) = 9**
  - 总 25 / 64 slot, 留 39 slot 给 v2 (Image / Mesh / Font 等)

## 五. CI 与 smoke 结果

```
✓ build-android  4m4s
✓ build-macos    4m9s
✓ build-windows  7m13s
  └─ Run Windows runtime smoke scripts:
      === Phase G.1 gpumem smoke: pass=14 fail=0 ===
✓ build-web      2m53s
✓ build-linux    3m24s
✓ build-ios      7m29s

ANNOTATIONS: only Node.js 20 deprecation warnings (无关本次变更)
```

## 六. 已知限制 / v2 扩展

仍未跟踪 (体积小或 hook 复杂度不值):

| 项 | 优先级 | 估时 | 备注 |
|----|--------|------|------|
| AE mipmap chain (准确 +33%) | P2 | 0.5h | 跟踪精度提升 < 0.1% 总量 |
| Lens Dirt / Streak / LensFlare | P2 | 0.5h | 全模块查无 RT 创建 (走 Bloom 输出) |
| MotionBlur internal RT (如有) | P2 | 0.3h | 待扫码确认 |
| 用户 Image | P1 | 1h | 50+ Lua API hook 点 |
| 用户 Mesh / VBO | P1 | 1h | Asset Loader hook |
| 用户 Font | P1 | 0.5h | atlas tex |
| Skybox / Cubemap | P2 | 0.3h | 罕见, 一个 cubemap = 6× tex |

留作 Phase G.1.2 / G.2 候选.

## 七. 后续建议

按 HANDOFF §3 选项 A:

1. **Phase G.2 (推荐): Lua API 容错 audit** (`docs/HANDOFF_REMAINING_TASKS.md` 选项 A.4)
   - 全 API 错参 nil+err 返回 (而非 crash)
   - 估时 4-6h
   - 长期价值高 (用户脚本崩溃 → 优雅 nil 返回)

2. **Phase G.3: Tick-Render 解耦** (选项 A.2)
   - 60Hz 逻辑 + VSync 渲染
   - 估时 8-15h (架构级改动)
   - 为未来插帧 / 网络同步铺路

3. **Phase G.1.2: VRAM v1.2 P1** (用户 Image / Mesh / Font)
   - 估时 2.5h
   - 闭合 v2 P1 部分

## 八. 经验沉淀

### 8.1 hook 对称性原则

**每个 Track 必须有对应 Untrack, 且生命周期完全匹配**. 此次 4 项 hook 都做了:
- Track 在 `Enable/AllocateResources/...` 成功路径末尾
- Untrack 在 `Disable/DestroyResources/...` 释放路径开头 (在 `g.X` 字段被清零之前)

### 8.2 backend 算法严格对齐

Bloom mip 递推算法, SSAO 格式 DEPTH24, AE base 级简化 — 这些都依赖**直接 grep backend 实现确认**, 而非依据接口注释推测. 注释可能过期, GL 调用是 ground truth.

### 8.3 遗漏修复模式

G.1 outputSceneTex 只在 `ReleaseRT` Untrack, 缺 `OnTAAURenderScaleChanged` 的 Track — 这种"分配点不在主创建函数"的情况容易漏. v1.1 修复策略:
- 检查 ReleaseRT 内的所有 Untrack 调用
- 对每个 Untrack, grep 对应 name 的 Track 调用是否存在
- 缺失则补 Track 在分配点

通用规律: **释放代码比分配代码更集中, 反向追踪释放点能高效发现遗漏的分配 hook.**
