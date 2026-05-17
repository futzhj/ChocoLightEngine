# Phase F.1 TAAU — TODO 文档（剩余增量与监控点）

> **阶段**：6A Workflow — 阶段 6 Adapt（迭代）
> **基线**：FINAL_PhaseF_1.md (v1.0, 2026-05-17 F.1.0 实施完结)
> **更新日期**：2026-05-17

---

## 1. 后续 Phase 计划

### F.1.0.1 — Multi-Instance × TAAU 扩展
**优先级**: 中 (split-screen 多 player 不同 renderScale 场景需求)

**改动**:
- TAARenderer::SetTAAUEnabled 移除 `g_active == 0` 限制
- HDRRenderer::OnTAAURenderScaleChanged 接受 instance id 参数 (而非作用 g_active)
- 4 instance × 不同 renderScale 测试 (smoke + demo_taa_split2 集成)
- 各 instance 独立维护 outputSceneFbo/Tex

**估时**: 1-2 天

### F.1.1 — Mipmap LOD Bias 调整
**优先级**: 中-高 (TAAU 视觉质量提升关键)

**改动**:
- 3D mesh shader (PBR / Unlit / Skinned 等约 10 个) 增 `uMipBias` uniform
- TAARenderer 自动计算 `bias = log2(renderScale) - 0.7` (FSR2 标准, UE5 同)
- 内部 hook: TAA::SetRenderScale 时同步 backend SetMipBias
- 涉及 shader 约 6-10 个 + backend 接口 1-2 个

**视觉收益**: 0.667 scale 下纹理细节锐度提升明显, 接近 native (FSR2 Quality 经验)

**估时**: 2-3 天 (含真机视觉验收)

### F.1.2 — Velocity Nearest-Filter 选项
**优先级**: 低 (仅在真机测试显示 ghost 严重时再启用)

**改动**:
- TAARenderer::SetVelocityNearestSample(bool) API 新增
- backend velocityTex GL_TEXTURE_MIN_FILTER 切换 GL_NEAREST / GL_LINEAR
- 默认保持 LINEAR (零回归), 用户按需 opt-in

**估时**: 0.5 天

### F.1.3 — Custom Render Scale (FSR2 Custom 档)
**优先级**: 低 (4 档预设已覆盖 95% 用例)

**改动**: 让 SetRenderScale 接受 [0.33, 1.0] (而非 [0.5, 1.0]), 增加 FSR2 Ultra Performance (0.33) 档预设

**风险**: 0.33 下 ghost 显著, 需更精细 history clip + 可选 LOD bias 加大

**估时**: 1 天

### F.1.4 — Dynamic Resolution Scaling (DRS)
**优先级**: 低-中 (主机/移动端帧率自适应需求)

**改动**:
- TAARenderer::SetDynamicTarget(targetFPS) API 新增
- 内部维护帧时间滑动窗口, 当超过预算时自动降 renderScale, 低于预算时升
- DRS 时 history 重建有抖动, 需平滑过渡逻辑

**估时**: 3-5 天

---

## 2. 监控点 (运维 / 真机)

### 2.1 移动端兼容性
- [ ] **Mali GPU (ARM)**: 在 renderW=非 8 倍数尺寸 (e.g. 854) 测 FBO 创建是否成功
- [ ] **Adreno GPU (Qualcomm)**: TAA shader 编译 + GLES3 path 在 Android 真机
- [ ] **Apple Silicon (Metal via SDL)**: 通过 SDL3 GL ES3 包装层是否兼容

### 2.2 极端配置
- [ ] **dilation halfRes + TAAU 0.5**: 等效 1/4 res velocity, 测试 history clip 是否仍可吸收
- [ ] **TAAU + SSR Temporal**: 双 temporal 共存时是否 over-smooth (建议 demo_ssr Y+T 同时切)
- [ ] **TAAU + MotionBlur 高速运动**: motion-adaptive sharpness 是否在 TAAU 下仍有效

### 2.3 性能 profiler
- [ ] **NSight Graphics / RenderDoc**: 抓帧验证 GPU 时间各 pass 分布与预期一致
- [ ] **GPU Bound vs CPU Bound**: TAAU 应让 GPU bound 场景帧率提升, CPU bound 场景无变化

---

## 3. 文档遗留

### 3.1 待补充
- [ ] `docs/api/Light_Graphics.md` TAA 子表新增 8 函数 API 说明 (字段表 + 示例)
- [ ] 主 `README.md` 特性表添加 "TAAU (DLSS/FSR2 风格上采样)"
- [ ] `docs/Phase F.0 TAA Master Pipeline/PLAN_PhaseF_0.md` 末尾加 "F.1 后续路线" 链接

### 3.2 可选改进
- [ ] DESIGN_PhaseF_1.md 更新: 标注 "FS_TAA shader 0 新 uniform" 修订 (相对原计划 4 个)
- [ ] 实施过程修复的 bug (`SetVelocityFormat` / `Resize` 双尺寸感知) 写入 FINAL §5.2 (已完成)

---

## 4. 验收回顾会议项

(用户真机验收后回填)

- [ ] 视觉质量是否符合预期 (4 档对比)
- [ ] 性能收益是否符合预期 (GPU 时间 -30~60%)
- [ ] Q5 仲裁是否对用户透明 (HalfResHistory 自动关 + log)
- [ ] HUD 信息是否清晰 (Render/Output 分辨率展示)
- [ ] demo_taau 键位是否易用

---

## 5. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 初稿 — F.1.0.1 / F.1.1 / F.1.2 / F.1.3 / F.1.4 路线图 + 真机监控点 + 文档遗留 |
