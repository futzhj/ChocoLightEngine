# ACCEPTANCE — Phase E.2.2 · ECS Light2D cull 联动

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.2.2**：ECS `_UploadLights2D(cam2d, bounds?)` 接受 world-space AABB，对每个 Light2D 做 AABB-Circle 相交测试，视口外的 light 跳过上传。

---

## 1. 改动摘要

| 文件 | 改动 | 关键点 |
|------|------|--------|
| `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` (Lua 内嵌) | `_UploadLights2D` 签名从 `(cam2d)` 改为 `(cam2d, bounds)`；新增 AABB-Circle cull 分支；新增 `self._light2d_stats = {uploaded, culled}` 诊断 | bounds 为 world-space（与 `_FrustumCull2D` 返回同 space）；`bounds == nil` 时保留全上传行为（向后兼容） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:1107-1113` | `Render()` 中 `bounds = _FrustumCull2D(cam2d)` 提前到 `_UploadLights2D` 之前 | bounds 在 light cull + sprite cull 间共享，不重复计算 |
| `@e:\jinyiNew\Light\scripts\smoke\lighting2d.lua` | §17 3 段断言：bounds 内外混合（uploaded=2/culled=2）/ bounds=nil 全上传 / 收紧 bounds 只留穿透 light | 用 4 个 Light2D（近/远 range=50/远 range=1500/远下方）覆盖四种边界情况 |

---

## 2. 关键设计

### 2.1 坐标空间

`_FrustumCull2D` 返回 **world space** AABB。`_UploadLights2D` 内 `wx/wy` 也是 world space（`_GetWorldPos2D` 产生）。所以 cull 测试 **不用空间转换**，直接：

```lua
if wx + range < bounds.minX or wx - range > bounds.maxX or
   wy + range < bounds.minY or wy - range > bounds.maxY then
    -- cull
end
```

只有保留下来的 light 才做 `(wx - cx) * zoom` 转 view space 并调 `AddPointLight/AddSpotLight`。

### 2.2 AABB-Circle 宽松测试

用 AABB-vs-enclosing-square 测试（保守）而非真正的 AABB-vs-Circle 最近距离测试：
- **正确率**：100% 不漏 cull（不存在"应渲染但被丢"的 light，视觉绝对安全）
- **过度保守**：少数"圆角不交、外接方形相交"的 light 仍上传（视觉无影响，仅性能极轻微损失）
- **不开平方根**：比真正 Circle 距离测试快且简单

### 2.3 `_light2d_stats` 诊断

每帧 `_UploadLights2D` 执行后，`self._light2d_stats = {uploaded=N, culled=M}`：
- smoke 层验证 cull 逻辑正确
- 用户可打 HUD 展示 `world._light2d_stats.culled` 观察帧间 cull 效果
- 不加网络同步（统计类，无意义）

### 2.4 `bounds == nil` 向后兼容

`_FrustumCull2D(cam2d)` 在 camera 无 `viewportW/viewportH` 时返回 nil（例如 smoke § 15.6 用 `w2:_UploadLights2D(cam)` 单参数 + camera 未设 viewport）。此时 `_UploadLights2D` 跳过所有 cull 分支，行为与 E.2.1 完全一致。

---

## 3. 验收清单

| 标准 | 状态 | 证据 |
|------|------|------|
| `_UploadLights2D(cam2d, bounds)` 新签名 | ✅ | smoke §17 全部用 `w:_UploadLights2D(nil, bounds)` |
| AABB-Circle cull 准确（边界情况 4 种） | ✅ | §17.1 混合测试 uploaded=2/culled=2 |
| `bounds == nil` 向后兼容 | ✅ | §17.2 全部上传 |
| `_light2d_stats.uploaded + culled == active Light2D 数` | ✅ | 暗含在 §17.1 的 2+2=4 / §17.2 的 4+0=4 |
| 既有 smoke §1-§16 全 PASS | ✅ | 33 段既有 + 3 段新增 = 36 PASS + DONE |
| `ecs_render.lua` 既有 smoke 不破 | ✅ | `Phase D ECS render smoke: ALL PASS` |
| 编译通过 Release | ✅ | cmake build success |

---

## 4. 本地验证

```
Light.vcxproj -> Light.dll (build success)
light.exe lighting2d.lua:
  ... (§1-§16 前 33 PASS)
  PASS: E.2.2: AABB-Circle cull (uploaded=2 / culled=2)
  PASS: E.2.2: bounds == nil backward compat (all 4 uploaded)
  PASS: E.2.2: tighter bounds culls more lights (uploaded=1)
  ==== Light.Lighting2D smoke DONE ====        # 36 PASS

light.exe ecs_render.lua     → Phase D ECS render smoke: ALL PASS
```

---

## 5. 性能收益（理论估算）

| 场景 | 之前 | E.2.2 后 | 备注 |
|------|------|----------|------|
| N 个 Light2D，均在视口内 | 全 N 上传 | 全 N 上传（AABB 命中） | 无变化 |
| N 个 Light2D，均在视口外 | 全 N 上传 | 0 上传（全部 AABB 拒绝） | 节省全部 uniform + shader 循环开销 |
| N 个 Light2D，K 个在视口内 | 全 N 上传 | K 上传，N-K 跳过 | 节省 (N-K) 次 Lua `AddPoint/Spot` + shader 循环 |
| N > 16 时（超硬上限） | 只能前 16 个 | 按 cull 优先保留视口内 | 间接提升"有效 light 覆盖率" |

**组合优势**：E.2.2 与 E.2.1 协同 — cull 后若活跃 light 集合不变，`Lighting2D::State::version` 不递增，GL33 backend 继续跳过 uniform upload。

---

## 6. 不在本任务范围

- spot light 方向感知的锥形 cull（优化：spot 在 bounds 外但锥向 bounds 时仍 cull — 当前按点光圆处理，保守）
- ECS 之外的直接 `Light.Lighting2D.AddPointLight` 调用不受影响（cull 是 ECS 层的附加能力）

---

## 7. 下一步

继续 E.2.3 — LitBatchRenderer + `RenderBackend::DrawLit2DBatch` + `l_DrawLit*` 改批提交。
