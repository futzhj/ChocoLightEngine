# Phase F.0.10.3 — Bloom/SSR/MB Region 化 ALIGNMENT (对齐)

> 6A 工作流 · 阶段 1 (Align) · 项目上下文 + 需求理解
> 关联: F.0.10.2 (已交付, 5/5 sub-phase × 6 平台 success)
> 工作量预估: 5-8h (3 子模块 + 文档 + smoke + demo)

---

## 1. 项目上下文分析

### 1.1 现有 EndScene 后处理 Pipeline (HDR.EndScene 内部, 13 阶段)

```
1. UnbindFBO                                — 切回 default fb
2. BloomRenderer::Process(g.fbo, sceneTex)  ← ❌ 待 region 化
3. AutoExposureRenderer::Process            (全局测光, 不 region)
4. LensDirtRenderer::Process                (附加项, 不在本 phase 范围)
5. StreakRenderer::Process                  (附加项, 不在本 phase 范围)
6. SSAORenderer::Process                    (附加项, 不在本 phase 范围)
7. velocity dilation pass                   (单 pass full-screen, 不 region)
8. SSRRenderer::Process(g.fbo, sceneTex)    ← ❌ 待 region 化
9. LensFlareRenderer::Process               (附加项, 不在本 phase 范围)
10. MotionBlurRenderer::Process             ← ❌ 待 region 化
11. TAARenderer::Process(g.fbo, sceneTex)   ✅ F.0.10.2 已 region 化
12. DrawTonemapFullscreen                   (单 pass, Gfx.SetViewport 控制)
13. CommitVelocityHistory                   (内部状态, 不影响 region)
```

### 1.2 各模块当前实现结构

#### A. BloomRenderer (Phase E.4)

| 资源 | 内容 |
|------|------|
| 模块状态 | `bloom_renderer.cpp::g` (单例 State) |
| pyramid | `g.fbos[8]` + `g.texs[8]` (RGBA16F, 2-8 级 mip) |
| 默认 levels | 5 (clamp [2, 8]) |
| Process pass 链 | BrightPass → Downsample × N → Upsample × (N-1) → Composite |

**Pass 详情 (4 个 backend 接口)**:
1. `DrawBloomBrightPass(sceneTex, pyramidFbo[0], w, h, threshold)`
   - sceneTex (full-res) → pyramid[0] (full-res)
2. `DrawBloomDownsample(prevTex, dstFbo, dstW, dstH)` × N
   - pyramid[i-1].tex → pyramid[i].fbo (尺寸 /2)
3. `DrawBloomUpsample(srcTex, dstFbo, dstW, dstH, radius)` × N-1
   - pyramid[i].tex → pyramid[i-1].fbo (尺寸 ×2, GL_BLEND ONE/ONE)
4. `DrawBloomComposite(bloomTex, hdrFbo, w, h, intensity)`
   - pyramid[0].tex → hdrFbo (full-res, additive blend)

**关键观察**:
- 每级 pyramid 的 viewport 当前是 `glViewport(0, 0, dstW, dstH)` 全级覆盖
- Composite 直接写 hdrFbo, 用 GL_BLEND ONE/ONE 加性合成
- Shader 是采样邻像素 (3-tap tent / 13-tap COD AW), 跨边界会采到另一区域

#### B. SSRRenderer (Phase E.9-E.18)

| 资源 | 内容 |
|------|------|
| 模块状态 | `ssr_renderer.cpp::g` (单例 State) |
| depth RT | `g.depthFbo` + `g.depthTex` (full-res, copy from HDR depth) |
| reflect RT | `g.reflectFbo` + `g.reflectTex` (full-res RGBA16F) |
| blur RT (E.10) | `g.blurFbos[2]` + `g.blurTexs[2]` (half-res ping-pong) |
| history RT (E.12) | `g.historyFbos[2]` + `g.historyTexs[2]` (full-res ping-pong) |
| Composite temp | backend 内部 `ssrCompTempFbo/Tex` (full-res, 解 feedback loop) |

**Pass 详情 (~5 个 backend 接口)**:
1. `BlitHDRDepthToSSAO(hdrFbo, depthFbo, w, h)` — depth blit (full-screen)
2. `DrawSSR(depthTex, normalTex, hdrTex, reflectFbo, w, h, ..., jitter)`
   - 反射 ray march, 写 reflectFbo
3. `DrawSSRTemporal(reflect, history[read], depth, velocity, history[write], ...)`
   - 时序累积 + reproject + clip
4. `DrawSSRBlur(src, depth, blurFbo, halfW, halfH, axis, radius, ...)` × 2 (H + V)
   - half-res Gaussian / Bilateral
5. `DrawSSRComposite(reflect/blur, hdrFbo, w, h, intensity)`
   - 加性 composite 到 hdrFbo (内部 blit hdrFbo → temp 解 feedback loop)

**关键观察**:
- SSR ray march 在屏幕空间, ray 可能 march 出 region 边界 (跨区域采样)
- depth blit 是 full-screen, 但 region 化时也要限制只 blit 子矩形
- history reproject 也跨边界 (prevUV 可能越界另一半)

#### C. MotionBlurRenderer (Phase E.15-E.17)

| 资源 | 内容 |
|------|------|
| 模块状态 | `motion_blur_renderer.cpp::g` (单例 State) |
| ping-pong RT | `g.fbo` + `g.tex` (RGBA16F, 与 sceneTex 同尺寸) |
| storage size (E.17) | half-res 可选 (存储空间优化) |

**Pass 详情 (1 个 backend 接口, 2-pass 内部)**:
1. `DrawMotionBlur(sceneTex, velocityTex, cameraVelocityTex, mbFbo, mbTex, dstFbo, w, h, strength, sampleCount, mode, rtW, rtH)`
   - **Pass 1 (shader)**: bind motionBlurFbo → 沿 velocity 多采样 → 写 motionBlurTex
   - **Pass 2 (blit)**: motionBlurTex → dstFbo (覆盖 sceneTex 内容, glBlitFramebuffer)

**关键观察**:
- Pass 1 的 viewport 当前是 `glViewport(0, 0, rtW, rtH)` 全屏
- Pass 2 是 glBlitFramebuffer (src 全屏 → dst 全屏), 需要 region 化为 sub-rect blit

---

## 2. 需求理解

### 2.1 任务范围 (本 phase 必做)

- [x] **Bloom region 化**: 4 个 backend pass + Process(region)
  - 默认参数 0/0/0/0 = 全屏 (零回归)
  - region 化时 pyramid 每级按比例缩 (mip-N 是 region/2^N)
  - 使用 GL_SCISSOR + viewport 限定写 (shader 不改)
- [x] **SSR region 化**: 5 个 backend pass + Process(region)
  - 默认参数 0/0/0/0 = 全屏 (零回归)
  - depth blit / reflect raster / temporal / blur / composite 都接受 region
  - SSR ray march 仍全屏运行 (跨边界采样合理, 不动 shader)
- [x] **MotionBlur region 化**: 1 个 backend pass + Process(region)
  - 默认参数 0/0/0/0 = 全屏 (零回归)
  - Pass 1 shader scissor + Pass 2 blit sub-rect

### 2.2 任务边界 (本 phase 不做)

- [ ] LensDirt / Streak / SSAO / LensFlare region 化 (附加项, 复杂度高, 留 F.0.10.4)
- [ ] AutoExposure (设计上是全局测光, 不需要 region)
- [ ] Bloom shader 改 uvOffset/uvScale (避免跨边界泄漏的"完美"方案, 留 F.0.10.5)
- [ ] HDR fbo 多实例化 (留 F.0.10.4)
- [ ] 4-player demo (用户立即可写, 留独立任务)

### 2.3 零回归刚性约束

- 现有 4 个 demo (demo_taa_compare / demo_ssr / demo_taa_split / demo_taa_split2) 行为完全等价
- 现有 8 个 backend pass 接口签名增加 region 默认 0 参数, 老调用方零改动
- 现有 Bloom 35 fn / SSR 13 fn / MotionBlur 11 fn Lua API 全保留
- BloomRenderer / SSRRenderer / MotionBlurRenderer 的 Process(hdrFbo, hdrTex) 签名保留, 仅加 overload

---

## 3. 关键技术决策点 (主动判定 + 自动决策)

| 决策点 | 备选 | 主动决策 + 理由 |
|--------|------|----------------|
| 1. Bloom region 化路径 | A. shader uvOffset / B. scissor + 全 size pyramid / C. 不做 | **B** — 与 F.0.10.2 TAA 同方案, 跨平台稳定, 工作量低 (~2h) |
| 2. SSR ray march 是否限 region | A. region 内 / B. 全屏 march | **B** — ray 跨区采样合理 (反射可看到邻区), shader 零改动 |
| 3. SSR history 写入策略 | A. region 写 / B. 全屏写 | **A** — region 写, scissor 限定 history 仅本 region 累积 |
| 4. SSR depth/reflect 写入 | A. region 写 / B. 全屏写 | **A** — scissor 限 region, 减重复计算 + 防写脏邻区 |
| 5. MotionBlur Pass 1 region | A. shader scissor / B. region viewport | **A** — scissor 限定写区域, viewport 仍 full-rt 保持 UV 映射 |
| 6. MotionBlur Pass 2 blit | A. sub-rect blit / B. shader copy | **A** — glBlitFramebuffer 支持 src/dst rect, 零 shader |
| 7. region 参数语义 | 4 个 int (rgnX/Y/W/H) | 与 F.0.10.2 TAA 一致, W=0 或 H=0 = 全屏 |
| 8. Bloom mip-N region 计算 | A. 按比例缩 / B. 每级独立传 region | **A** — Process 内部按 `>>i` 缩 region, 调用方一次传 |
| 9. region 边界小于 1px 处理 | A. 跳过该 mip / B. clamp 到 1×1 | **B** — 与现有 pyramid mip 1×1 兜底逻辑一致 |
| 10. Lua API 设计 | A. Bloom.Process(rgn) / SSR.Process(rgn) / MB.Process(rgn) Lua 直暴露 / B. 仅 C++ 调用方传 | **A** — 用户可手动控时序 (类似 F.0.10.2 TAA.Process), split-screen demo 必备 |
| 11. autoBloom / autoSSR / autoMB 开关 | A. 加 SetAutoXxx / B. 沿用现 IsEnabled | **A** — 与 HDR.SetAutoTAA 对称, split-screen 必备 |
| 12. 默认值 | autoBloom/autoSSR/autoMB 默认 true | 零回归 (老 EndScene 路径自动调) |

---

## 4. 疑问澄清

### 4.1 已自动决策 (无需用户介入)

- ✅ Region 化路径 (scissor + 全 size, 与 F.0.10.2 TAA 一致)
- ✅ Bloom mip 按比例缩 region
- ✅ SSR ray march 全屏 (反射借邻区合理)
- ✅ MotionBlur 2-pass: scissor + sub-rect blit
- ✅ Lua API + autoXxx 开关 (与 F.0.10.2 TAA 对称)

### 4.2 需用户拍板的剩余决策点

**唯一需用户拍板的**:

- **Q1: 4 sub-phase 实施顺序**: Bloom → SSR → MotionBlur → 收尾, 还是先做 demo 验证最简的 MotionBlur (1-2h 风险最低), 再 Bloom (2-3h), 再 SSR (2-3h, 最复杂)?
  - **推荐**: MotionBlur 先做 (作为最小可行验证), 因为它只有 1 pass + 1 blit, 容易验证 region 化是否破坏视觉. 通过后再做 Bloom 和 SSR.

- **Q2: 是否做 demo_taa_split3 演示真物理 split-screen with bloom + SSR + MB?
  - **推荐**: 是的. demo_taa_split2 仅演示 TAA region, 加这个 sub-phase 后 split-screen 才是真正"完整后处理".

---

## 5. 现有项目对齐

### 5.1 与 F.0.10.2 复用经验

| 复用点 | 内容 |
|--------|------|
| region 参数签名 | `int rgnX, int rgnY, int rgnW, int rgnH` 默认 0 |
| 全屏 fallback 判定 | `rgnW <= 0 \|\| rgnH <= 0` → 走老路径 |
| scissor 启用方式 | `glEnable(GL_SCISSOR_TEST)` + `glScissor(x, y, w, h)` |
| autoXxx 开关 | 与 `HDR.SetAutoTAA` 对称设计 |
| Lua API 验证 | luaL_checkinteger + 防御性 0/4 args + w/h<0 拒绝 |
| smoke 模式 | headless API probe 验 round-trip + nil + err |

### 5.2 与现有 backend 接口对齐

| 接口 | 现有签名 | 新签名 (加 region 默认 0) |
|------|---------|---------------------------|
| `DrawBloomBrightPass` | `(sceneTex, outFbo, w, h, threshold)` | `+ rgnX, rgnY, rgnW, rgnH = 0` |
| `DrawBloomDownsample` | `(srcTex, dstFbo, dstW, dstH)` | 同上 |
| `DrawBloomUpsample` | `(srcTex, dstFbo, dstW, dstH, radius)` | 同上 |
| `DrawBloomComposite` | `(bloomTex, hdrFbo, w, h, intensity)` | 同上 |
| `BlitHDRDepthToSSAO` | `(hdrFbo, depthFbo, w, h)` | 同上 (Phase E.8.x 也复用) |
| `DrawSSR` | `(depth, normal, hdr, dst, w, h, ..., jitter)` | 同上 |
| `DrawSSRTemporal` | `(...10 args...)` | 同上 |
| `DrawSSRBlur` | `(src, depth, dst, w, h, axis, radius, ...)` | 同上 |
| `DrawSSRComposite` | `(reflect, hdrFbo, w, h, intensity)` | 同上 |
| `DrawMotionBlur` | `(scene, vel, camVel, mbFbo, mbTex, dst, w, h, ..., rtW, rtH)` | 同上 |

总计 10 个 backend pass 加 region 参数. 默认 0 = 全屏 = 老路径, 零回归.

### 5.3 与现有 Lua 命名空间对齐

新增 Lua API (预估 9 fn):
- `HDR.SetAutoBloom(bool)` / `HDR.GetAutoBloom()` (默认 true)
- `HDR.SetAutoSSR(bool)` / `HDR.GetAutoSSR()` (默认 true)
- `HDR.SetAutoMotionBlur(bool)` / `HDR.GetAutoMotionBlur()` (默认 true)
- `Bloom.Process()` / `Bloom.Process(x, y, w, h)`
- `SSR.Process()` / `SSR.Process(x, y, w, h)`
- `MotionBlur.Process()` / `MotionBlur.Process(x, y, w, h)`

累计: 现有 45 fn + 9 fn = **54 fn**.

---

## 6. 验收标准

- [ ] 老 demo (demo_taa_compare / demo_ssr / demo_taa_split / demo_taa_split2) 视觉零差异
- [ ] 现有 smoke (taa.lua / hdr.lua / graphics.lua / ssr.lua / bloom.lua) 全 PASS
- [ ] 新增 smoke 段验证 9 个新 fn (默认值 / round-trip / 类型错 / 区域参数)
- [ ] CI 6/6 平台全 success
- [ ] 6A 文档完整: ALIGNMENT (本文) / DESIGN / TASK / ACCEPTANCE / FINAL / TODO

---

## 7. 共识 (用户拍板 2026-05-16)

**实施顺序 (按风险递增, 最低风险先行)**:
1. **Phase 1 — MotionBlur region 化** (~1.5h)
   - 1 pass shader + 1 pass blit, 改动量最小, 验证 region 化路径正确性
2. **Phase 2 — Bloom region 化** (~2.5h)
   - 4 pass + pyramid mip 计算 region 缩放
3. **Phase 3 — SSR region 化** (~3h, 复杂度最高)
   - 5 pass + history ping-pong + blur ping-pong + ray march 不动 shader
4. **Phase 4 — 6A docs 收尾** (~1h)
   - ACCEPTANCE / FINAL / TODO + CI 回填

**总计预估**: ~8h

**Demo 范围**: ❌ **不做 demo_taa_split3** (节省 1-2h, 仅靠 smoke + demo_taa_split2 验证零回归)

**理由**:
- demo_taa_split2 已是真物理 split-screen, 跑通后启用 bloom/ssr/mb 即视觉验证
- smoke 在 headless CI 验证 API 正确性 (类型错 / nil + err / round-trip)
- 用户后续如需视觉演示, 1-2h 内可独立补 demo (复用 demo_taa_split2 结构)

**累计 Lua API**:
- F.0 ~ F.0.14: 35 fn
- F.0.10 multi-instance: +5 fn (40)
- F.0.10.2 split-screen: +5 fn (45)
- **F.0.10.3 Bloom/SSR/MB region: +9 fn (54)**
