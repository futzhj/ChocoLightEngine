# Phase F.2 渲染架构补齐 — ACCEPTANCE (验收) 文档

> **阶段**：6A Workflow — 阶段 5 Assess
> **创建日期**：2026-05-17

---

## 1. 编译验收

| 检查项 | 期望 | 结果 |
|--------|------|------|
| Release 增量构建 | 0 error 0 warning regression | ✅ Pass |
| Light.dll 已同步到 examples/editor | 时间戳新鲜 | ✅ Pass |

```
[I][2026-05-17][build] Light.vcxproj -> Light.dll  (无 warning regression)
```

## 2. Smoke 验收

| 套件 | 期望 | 结果 |
|------|------|------|
| phase_f2_multi_instance.lua | 30 个 binding + 6 类行为通过 | ✅ Pass |
| taa.lua (56/53 fn) | F.0~F.0.14 + F.1.0.1 + F.1.1 全部通过 | ✅ Pass |
| hdr.lua (57 fn) | E.3 + E.14 + E.18 + F.0.10.x 全部通过 | ✅ Pass |
| bloom.lua | E.4 + F.0.10.3 + F.0.10.9.x.2/x.3/x.4 全部通过 | ✅ Pass |
| ssao.lua | E.8 全部通过 | ✅ Pass |
| auto_exposure.lua | E.5 全部通过 | ✅ Pass |
| lens_fx.lua | E.6 全部通过 | ✅ Pass |
| lens_flare.lua | E.7 全部通过 | ✅ Pass |

## 3. G1 (P0) 行为验收

代码可见性检查 (hdr_renderer.cpp:786-805):

```cpp
g.taauActive = true;
// Phase F.2.0 — TAAU 切换通知下游后处理重建到 render-res
BloomRenderer::OnHDRResized(renderW, renderH);
AutoExposureRenderer::OnHDRResized(renderW, renderH);
LensDirtRenderer::OnHDRResized(renderW, renderH);
StreakRenderer::OnHDRResized(renderW, renderH);
LensFlareRenderer::OnHDRResized(renderW, renderH);
SSAORenderer::OnHDRResized(renderW, renderH);
SSRRenderer::OnHDRResized(renderW, renderH);
MotionBlurRenderer::OnHDRResized(renderW, renderH);
```

OnTAAUDisabled 镜像调用 (renderW → outputW)：✅

## 4. G2 (P1) 行为验收

phase_f2_multi_instance.lua 输出片段:

```
[PASS] AutoExposure has 6 multi-instance functions
[PASS] LensDirt has 6 multi-instance functions
[PASS] Streak has 6 multi-instance functions
[PASS] LensFlare has 6 multi-instance functions
[PASS] SSAO has 6 multi-instance functions
[PASS] All 5 modules: default count=1, active=0
[PASS] All 5 modules: Create/SetActive/Destroy round-trip ok
[PASS] LensDirt: 4 instance 上限
[PASS] LensDirt: cleanup back to count=1
[PASS] LensFlare: Clone 继承参数 ok
[PASS] SSAO: 边界检查 (不能销毁 default, 不能切到未分配 id)
[OK] Phase F.2 multi-instance smoke 全部通过
```

## 5. G3 (P1) 行为验收

| 检查项 | 结果 |
|--------|------|
| lit_batch_renderer.h 公开 OnHDREnabled/Disabled/Resized 三声明 | ✅ |
| lit_batch_renderer.cpp 三个 stub 实现编译通过 | ✅ |

## 6. 真机验收口径 (留用户)

需要用户在真实场景下验证:
1. 启用 TAAU 后, Bloom 视觉无错位 / 无暗角扩张
2. split-screen 4 player 场景 SSAO/LensFlare/LensDirt/Streak/AE 各自参数独立
3. 多 instance Clone 路径无显存泄漏 (任务结束后 GetMemoryStats 回到基线)
