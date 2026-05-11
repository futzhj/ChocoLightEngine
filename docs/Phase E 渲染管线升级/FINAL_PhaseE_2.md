# FINAL — Phase E.2 · 2D Lit 渲染性能优化

> 项目总结报告 · 6A 工作流 · 阶段 6 · Assess

Phase E.2 完整交付：在 Phase E.1 forward 多光照系统基础上，三项性能优化连续推进——dirty bit 跳过冗余 uniform、ECS bounds cull 跳过视口外 light、LitBatchRenderer 合批同纹理 lit sprite。

---

## 1. 交付总览

| 维度 | 数据 |
|------|------|
| **原子任务** | 3 个全部完成（E.2.1 / E.2.2 / E.2.3） |
| **代码行数** | C++ +400 行（新建 `lit_batch_renderer.h/cpp` 250 行 + 6 文件改动 150 行）； Lua/Smoke +180 行（ECS 60 行 + smoke 120 行） |
| **新增文件** | 6 个（2 C++ 源/头 + 4 文档 ACCEPTANCE/FINAL/ALIGNMENT） |
| **修改文件** | 8 个（include/light_lighting2d.h, src/light_lighting2d.cpp, include/render_backend.h, src/render_gl33.cpp, src/light_ui.cpp, src/light_graphics.cpp, src/light_ecs.cpp, scripts/smoke/lighting2d.lua, CMakeLists.txt） |
| **Commits** | 3 次本地（`0937c61` E.2.1 / `2d2ee09` E.2.2 / 待 E.2.3）+ `FINAL` 待 commit |
| **本地 smoke** | `lighting2d.lua` 40 PASS + DONE / `ecs_render.lua` ALL PASS / `graphics.lua` 11/11 |
| **CI 状态** | ⏸ 待 push（git 凭据问题未解，3 commit 本地积攒） |

---

## 2. 三项优化协同效果

```
                 ┌─────────────────────────────────────────────┐
                 │  ECS Render() 2D 阶段每帧执行流程              │
                 └─────────────────────────────────────────────┘
                                       │
                          ┌────────────▼────────────┐
                          │  E.2.2: bounds cull      │
                          │  AABB-Circle 测试         │
                          │  跳过视口外 Light2D       │
                          └────────────┬────────────┘
                                       │
                          ┌────────────▼────────────┐
                          │  Lighting2D::Add*        │
                          │  → ++state.version       │  ← E.2.1 dirty 触发
                          └────────────┬────────────┘
                                       │
                          ┌────────────▼────────────┐
                          │  LitSprite 循环          │
                          │  CPU 烘焙 transform      │
                          │  SubmitQuad 累积         │  ← E.2.3 batch
                          └────────────┬────────────┘
                                       │
                          ┌────────────▼────────────┐
                          │  FlushLitBatch()         │
                          │  → DrawLit2DBatch        │
                          │    → UploadLighting2D    │
                          │      (dirty 跳过 uniform) │  ← E.2.1 dirty 护航
                          │    → glDrawElements      │
                          │      (N quad / 1 call)   │  ← E.2.3 batch 收益
                          └─────────────────────────┘
```

**实测组合效果**（理论估算，待 demo_2d_lighting + 帧分析验证）：

| 场景 | E.1.5 baseline | E.2.1 only | E.2.3 only | E.2.1 + E.2.3 |
|------|----------------|------------|------------|----------------|
| 48 LitSprite × 同纹理 × 同 state | 48 draw + 48× upload | 48 draw + 1× upload | 1 draw + 1× upload | **1 draw + 1× upload** |
| 1000 LitSprite × 同纹理 | 1000 draw + 1000× upload | 1000 draw + 1× upload | 1 draw + 1× upload | **1 draw + 1× upload** |
| 100 LitSprite × 5 种纹理 | 100 draw + 100× upload | 100 draw + 1× upload | 5 draw + 5× upload | **5 draw + 1× upload** |
| 100 LitSprite, 视口外 80 | 100 draw + N× upload | 100 draw + 1× upload | 1 draw + 1× upload | **1 draw + 1× upload** (E.2.2 减灯数也减) |

---

## 3. 任务交付明细

| 任务 | 核心改动 | smoke 验证 | ACCEPTANCE |
|------|----------|-----------|------------|
| **E.2.1** | `Lighting2D::State` 加 `uint32_t version`；6 mutator `++version`；GL33 `lastUploadedLighting2DVersion` 缓存；`UploadLighting2D` 入口 version 相等早返回 | §16 5 段断言：initial / Get* / 6 mutator / idempotent / 17th 失败 | `ACCEPTANCE_PhaseE_2_1.md` |
| **E.2.2** | ECS `_UploadLights2D(cam2d, bounds)` 加 AABB-Circle cull；`Render()` 把 `bounds = _FrustumCull2D` 提前；`self._light2d_stats = {uploaded, culled}` 诊断 | §17 3 段断言：bounds 内外混合 / nil 向后兼容 / 紧 bounds 极端 cull | `ACCEPTANCE_PhaseE_2_2.md` |
| **E.2.3** | 新建 `LitBatchRenderer` (95+155 行)；`RenderBackend::DrawLit2DBatch` 虚接口 + GL33 实现（动态 EBO 临时切换）；`l_DrawLit*` CPU 烘焙 transform 走 batch；`l_FlushLitBatch` Lua API；5 处 matrix stack + `SubmitOrDraw` 入口 Flush；ECS `_DrawLitSprite` 不用 matrix stack | §18 API surface + 14/18 参数版本不崩 | `ACCEPTANCE_PhaseE_2_3.md` |

---

## 4. 关键技术决策

| # | 决策 | 选择 | 替代方案 / 原因 |
|---|------|------|-----------------|
| 1 | dirty bit 计数器类型 | `uint32_t` (溢出周期 ~497 天) | `uint64_t` 无必要；`bool` dirty flag 无法区分 "我看过的 v" vs "新的 v" |
| 2 | dirty version 递增"值相同时" | **递增**（如 `SetEnabled(true)` 两次都 ++） | 比较旧值复杂；性能影响可忽略；语义"任何 mutator 调用 = 状态变" 简单 |
| 3 | dirty 哨兵值 | State 初值 1 / Backend 初值 0 | 首次 upload 必 mismatch；后续溢出后 0 极少撞 |
| 4 | bounds cull 算法 | AABB-vs-enclosing-square（保守） | AABB-vs-Circle 距离测试要 sqrt；保守过度（少 cull 不破视觉，错 cull 才破） |
| 5 | bounds 坐标空间 | World space（与 `_FrustumCull2D` 同） | view space 要每次转换；world space 与 `_GetWorldPos2D` 天然对齐 |
| 6 | LitBatchRenderer 与 BatchRenderer 关系 | **独立实例**，互不感知 | 顶点格式不同（32B vs 64B）；shader/VAO 都不同；合并会破坏 BatchRenderer 简洁 |
| 7 | GL33 动态 EBO 策略 | 新增 `eboLit2DBatch`，与静态 `eboLit2D` 分离 | 共用一个 EBO 会让 `DrawLit2DQuad` 静态索引被污染；分离 + draw 后恢复 |
| 8 | EBO 生成时机 | `glBindVertexArray(0)` **之后** | 避免 vaoLit2D 误"记忆" 动态 EBO |
| 9 | CPU 烘焙 transform 范围 | 平移 + z 轴旋转 + 缩放 + origin | 2D sprite 主流；rx/ry 接受但忽略 |
| 10 | 双向 Flush 策略 | matrix stack 操作 + 普通 SubmitOrDraw 都 Flush LitBatch | 简单可靠；性能损失只在切换瞬间 |
| 11 | EndFrame 顺序 | LitBatch 先，普通后 | Lit shader 状态先清干净，普通后续不受 Lit uniform 影响 |
| 12 | ECS `_DrawLitSprite` 改造 | 完全去掉 matrix stack（fallback 路径除外） | 关键 — 否则每个 LitSprite 6 次 Flush 完全无 batch；现在 N 同纹理 1 draw call |

---

## 5. 本地 smoke 验证轨迹

```
E.2.1 完成后:
  light.exe lighting2d.lua → 33 PASS + DONE
  ecs_render.lua / graphics.lua 全 PASS

E.2.2 完成后:
  light.exe lighting2d.lua → 36 PASS + DONE
  ecs_render.lua / graphics.lua 全 PASS

E.2.3 完成后:
  light.exe lighting2d.lua → 40 PASS + DONE
  ecs_render.lua → "Phase D ECS render smoke: ALL PASS"
  graphics.lua → "通过 11 / 失败 0"
```

每个任务后既有 smoke **零回归**。

---

## 6. CI 验证（待 push）

| Commit | 状态 | 备注 |
|--------|------|------|
| `0937c61` E.2.1 | ⏸ 本地待 push | git 凭据 `iosndesign` 冲突 |
| `2d2ee09` E.2.2 | ⏸ 本地待 push | 等 push 后看 CI |
| E.2.3 + FINAL | ⏸ 待 commit + push | 本节点最后一个 |

**操作指引**（用户解决凭据后一次 push 三个 commit）：

```powershell
# 清除 github.com 旧凭据
cmdkey /delete:LegacyGeneric:target=git:https://github.com

# Push（会弹窗让重新登录 / 用 PAT）
git push origin main
```

---

## 7. 已知限制（留给后续）

| 限制 | 优先级 | 建议方案 |
|------|--------|----------|
| **ECS parent chain LitSprite 不合批** | 中 | 把 parent chain transform 也 CPU 烘焙到 `_DrawLitSprite` 的 transform 参数；要求 parent chain 无 rotation（或加复合矩阵参数） |
| **`LitBatchRenderer::Stats` 未 Lua 暴露** | 低 | demo_2d_lighting HUD 想显示 batch 数时再加 |
| **`normal map` 的 scale TBN 处理** | 低 | 当前 `BakeLit2DQuad` 只转 tangent 旋转；非均匀 scale 时 TBN 需求逆转置矩阵 |
| **视觉验收依赖 demo 跑** | 低 | demo_2d_lighting 48 sprite → 期望帧分析显示 batch=1（含 lighting upload 也 1 次） |
| **Phase E.3 HDR + 后处理** | 高 | 独立 Phase，依赖 E.2 的 batch baseline |

---

## 8. 后续推荐路径

| 选项 | 工作量 | 价值 |
|------|--------|------|
| **运行 demo + benchmark 验证 batch** | 0.5 天 | 实测确认 1 draw call/N sprites + dirty 跳过 upload；写 perf report |
| **Phase E.3 HDR + Tonemapping** | 4-5 天 | 视觉天花板大幅提升（ACES + offscreen RT） |
| **Phase E.4 Bloom + 后处理** | 3-4 天 | 与 HDR 同步 |
| **parent chain LitSprite 合批扩展** | 1 天 | 进一步压榨 lit batch 效率 |
| **fixes 杂项** | 0.5 天 | 修 `_DrawSkinnedMesh::Light.Animation` 同陷阱 + 加 normalMap demo 资源 |

---

## 9. 致谢

- **Phase E.1 baseline**：8 任务 forward 多光照系统提供了优化的"地基"
- **BatchRenderer 既有架构**：LitBatchRenderer 几乎全量复用其接口风格、状态机、性能统计模板
- **6A 工作流**：每个原子任务在 ALIGNMENT → ACCEPTANCE 的闭环让"性能优化"这种容易陷入"过度抽象"的任务保持节奏
