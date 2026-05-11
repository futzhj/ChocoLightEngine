# ACCEPTANCE — Phase E.2.3 · LitBatchRenderer

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.2.3**：新建 `LitBatchRenderer` 累积同 `(baseTex, normalMap)` 的 LitSprite 到一个 batch；新增 `RenderBackend::DrawLit2DBatch` 虚接口 + GL33 实现（动态 EBO + dirty bit 护航）；`Light.Graphics.DrawLit/DrawLitQuad` 改 CPU 烘焙 transform 后 submit；ECS 配合改造。

---

## 1. 改动摘要

| 文件 | 改动量 | 类型 | 关键点 |
|------|--------|------|--------|
| `@e:\jinyiNew\Light\ChocoLight\include\lit_batch_renderer.h` | ~95 行 | 新建 | 命名空间式 API（与 `BatchRenderer` 同风格）：`Init` / `Shutdown` / `BeginFrame` / `EndFrame` / `SubmitQuad(verts, baseTex, normTex)` / `Flush` / `Stats` |
| `@e:\jinyiNew\Light\ChocoLight\src\lit_batch_renderer.cpp` | ~155 行 | 新建 | 累积 `std::vector<RenderVertex2DLit>` + 动态 `std::vector<uint32_t>` 索引；状态切换条件 = `(baseTex, normTex)` 任一变化；容量满 / 手动 Flush 时调 `backend->DrawLit2DBatch` |
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +26 行 | 修改 | 新虚接口 `DrawLit2DBatch(verts, vertCount, indices, idxCount, baseTex, normalMap)` 默认 no-op |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +75 行 | 修改 | `GL33Backend` 加 `eboLit2DBatch` 动态 EBO + 容量；`DrawLit2DBatch` 实现：临时切动态 EBO → glDrawElements → 切回静态 eboLit2D（保护 vaoLit2D 的 EBO 绑定） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +12 行 | 修改 | 4 处 hook：`Init/BeginFrame/EndFrame/Shutdown` 与 BatchRenderer 同位置 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | ~155 行净变化 | 修改 | (1) `BakeLit2DQuad` CPU 烘焙 helper（transform 烘到 vertex.pos + tangent）；(2) `l_DrawLit`/`l_DrawLitQuad` 改走 `LitBatchRenderer::SubmitQuad`；(3) 新增 `l_FlushLitBatch` Lua API + 注册；(4) `l_Push/Pop/Translate/Rotate/Scale` 入口加 `LitBatchRenderer::Flush` 保护；(5) `SubmitOrDraw`（普通 sprite 入口）加 `LitBatchRenderer::Flush` 双向同步 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` (Lua 内嵌) | ~50 行 | 修改 | `_DrawLitSprite` 去掉 matrix stack，改用 `gfx.DrawLit/DrawLitQuad` 14/18 参数版本（含 transform + anchor 当 origin + flip 当负 scale）；Legacy fallback 保留 matrix stack；`Render()` LitSprite 循环末调 `gfx.FlushLitBatch()` 显式刷干净；插入 `)LUA" R"LUA(` 分隔符避 MSVC 16KB |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 行 | 修改 | 加 `lit_batch_renderer.cpp` 到 Light 目标 |
| `@e:\jinyiNew\Light\scripts\smoke\lighting2d.lua` | §18 ~30 行 | 修改 | API surface + 含 14/18 参数版本调用不崩 |

---

## 2. 关键设计

### 2.1 LitBatchRenderer 与 BatchRenderer 的关系

| 维度 | BatchRenderer | LitBatchRenderer |
|------|---------------|------------------|
| 顶点格式 | `RenderVertex` (32B) | `RenderVertex2DLit` (64B) |
| 状态键 | `textureId` | `(baseColorTex, normalMapTex)` 对 |
| 索引 | 静态 `[0,1,2,0,2,3,...]` 预生成 65536 | 动态 `std::vector<uint32_t>` 每帧重建 |
| Shader | `program`（2D unlit） | `programLit2D` + `Lighting2D` uniform |
| 容量 | 16384 quad | 8192 quad（顶点 2× 大小，减半） |
| 触发 Flush | 纹理切换 / 容量满 / 手动 | 同上 + `Lighting2D` state 变化（通过 E.2.1 dirty 间接护航） |

两者**独立实例**，互不感知。切换路径时调用方/共享代码（`SubmitOrDraw`、matrix stack 操作）显式互相 `Flush` 维持画家顺序。

### 2.2 GL33 动态 EBO 关键陷阱

`vaoLit2D` 在 `InitLit2D` 时绑定了静态 `eboLit2D` (索引 `[0,1,2,0,2,3]`)，供 `DrawLit2DQuad` 单 quad 用。`DrawLit2DBatch` 需要动态索引（N quad × 6 = N×6 索引）。

**陷阱**：VAO 会"记忆" `GL_ELEMENT_ARRAY_BUFFER` 的**最后一次绑定**。若 batch draw 后忘了切回 `eboLit2D`，下次 `DrawLit2DQuad` 会用错的索引（指向 batch 的过期数据）。

**解决**：
```cpp
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2DBatch);
glBufferSubData(...);
glDrawElements(...);
glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2D);  // ★ 必须恢复
```

同样在 `InitLit2D` 末尾，`glBindVertexArray(0)` **之后**才生成 `eboLit2DBatch`，避免被 vaoLit2D 误"吸收"。

### 2.3 CPU 烘焙 transform

batch 内 N 个 quad 共享同一 `modelview`（即 camera transform，调用方 push 进 GL stack 的部分），每个 quad 自己的 `(x, y, z, rz, sx, sy, ox, oy)` **必须**在 CPU 端烘焙到 `vertex.pos`。

`BakeLit2DQuad` 实现（`light_graphics.cpp:770`）：
```
local_pos = (lx, ly)                        # 4 个 quad 角点
step 1: pos -= (ox, oy, oz)                  # origin offset (anchor)
step 2: pos *= (sx, sy, sz)                  # scale (flip 用负 scale)
step 3: pos = R_z(rz) * pos                  # z 轴旋转 (2D 主流)
step 4: pos += (x, y, z)                     # 平移
tangent: (1,0,0,1) → R_z * tangent = (cs, sn, 0, 1)   # normalMap 旋转后仍正确
normal:  (0,0,1) 不变 (z 轴旋转不影响 z 分量)
```

**简化点**：`rx`、`ry` 接受但忽略（2D sprite 罕用）。

### 2.4 双向 Flush 同步

batch 内必须共享 modelview，所以**任何**改 matrix stack 的操作都强制 Flush LitBatch：

| 调用点 | Flush 行为 |
|--------|-----------|
| `gfx.Push/Pop` | Flush LitBatch（matrix stack 即将变） |
| `gfx.Translate/Rotate/Scale` | Flush LitBatch |
| `SubmitOrDraw`（普通 sprite/几何入口） | Flush LitBatch（切到不同 shader/VAO） |
| `gfx.DrawLit/DrawLitQuad` | Flush 普通 `BatchRenderer`（切到 Lit shader） |
| ECS `Render()` LitSprite 循环末 | 显式 `gfx.FlushLitBatch()` 刷干净 |
| `EndFrame` | LitBatch 先 EndFrame，普通 BatchRenderer 后 EndFrame |

这确保混用场景下顺序绝对正确。

### 2.5 ECS `_DrawLitSprite` 改造

**改造前**（matrix stack 风格）：
```lua
gfx.Push()
gfx.Translate(tf.x, tf.y, 0)
if tf.rot ~= 0 then gfx.Rotate(tf.rot, 0, 0, 1) end
gfx.Scale(sx, sy, 1)
gfx.DrawLit(img, normalMap, -ax*iw, -ay*ih, 0)  -- 调用前 4 个 Push/Translate/Rotate/Scale 都 Flush LitBatch
gfx.Pop()  -- 又 Flush
```
**每个 LitSprite = 6 次 Flush + 1 个 quad** → 完全无 batch。

**改造后**（参数风格）：
```lua
gfx.SetColor(...)
gfx.DrawLit(img, normalMap, tf.x, tf.y, 0,
            0, 0, tf.rot,  sx, sy, 1,  ax*iw, ay*iy, 0)
```
**N 个同 (img, normalMap) LitSprite = 1 batch + 1 draw call**。

flipX/flipY 通过负 `sx/sy` 表达，anchor 通过 `ox/oy` 当 origin 表达。

---

## 3. 验收清单

| 标准 | 状态 | 证据 |
|------|------|------|
| `LitBatchRenderer` 模块编译通过 | ✅ | Light.dll build success |
| `RenderBackend::DrawLit2DBatch` 接口 | ✅ | 默认 no-op (非 Lit2D 后端) + GL33 完整实现 |
| `l_DrawLit/l_DrawLitQuad` 走 LitBatchRenderer | ✅ | smoke §18 含 14/18 参数版本调用不崩 |
| `Light.Graphics.FlushLitBatch` Lua API | ✅ | smoke §18.1 API surface 验证 |
| ECS `Render()` LitSprite 循环走 batch | ✅ | `_DrawLitSprite` 不用 matrix stack；末调 `gfx.FlushLitBatch()` |
| matrix stack 操作正确 Flush LitBatch | ✅ | `l_Push/Pop/Translate/Rotate/Scale` 5 处入口加 Flush |
| 普通 sprite/几何 ↔ Lit 切换顺序正确 | ✅ | `SubmitOrDraw` 入口 Flush LitBatch |
| `EndFrame` 顺序：Lit 先 → 普通后 | ✅ | `light_ui.cpp:669-670` |
| `lighting2d.lua` 既有断言全 PASS | ✅ | §1-§17 36 段 + §18 4 段 = 40 PASS + DONE |
| `ecs_render.lua` 不破 | ✅ | `Phase D ECS render smoke: ALL PASS` |
| `graphics.lua` 不破 | ✅ | `通过 11 / 失败 0` |

---

## 4. 性能收益（理论估算）

| 场景 | 之前 (E.1.5) | E.2.3 后 | 节省 |
|------|--------------|----------|------|
| 48 个同 (img, normalMap) LitSprite（demo 8×6 网格） | 48 draw call | **1 draw call** | 47 draw call/帧 |
| 1000 同纹理 lit sprite | 1000 draw call | ~1 draw call（< 8192 容量） | 99.9% |
| 不同纹理混合：100 个 sprite × 4 种纹理 | 100 draw call | 4 draw call（每纹理 1 batch） | 96% |
| `UploadLighting2D` per draw（同帧 N 个） | N × 8 glUniform*v | **1 × 8 glUniform*v**（dirty 护航） | 与 E.2.1 协同 |

**组合**：
- E.2.1 dirty + E.2.3 batch → batch 内 N 个 quad **共享一次** lighting upload，零浪费
- E.2.2 cull → batch 前先扣掉视口外的 light，shader 内循环更短

---

## 5. 已知限制

| 限制 | 说明 |
|------|------|
| `RenderVertex2DLit::pos.z` 共享 modelview | batch 内每个 quad 不同 z 仍是合法的（z 烘焙到 vertex.pos.z），但调用方必须保证 batch 内 modelview 不变（matrix stack 改变会触发 Flush） |
| ECS parent chain LitSprite 不合批 | `_PushParentChain2D` 用 matrix stack，每个有 parent 的 LitSprite 进入前都 Flush。这是 batch 的天然代价；可在未来 phase 把 parent chain 也 CPU 烘焙到 transform |
| 用户 Lua 代码混用 matrix stack + `gfx.DrawLit` | 文档明确：matrix stack 操作会强制 Flush LitBatch；调用方负责合理使用以利用批渲染 |
| 视觉验收依赖人工 | CI 无显示环境只能验证 smoke API surface；真实合批数 (drawCalls 统计) 需 demo_2d_lighting 运行 + 帧分析 |

---

## 6. 不在本任务范围

- LitBatchRenderer 性能 benchmark report（独立任务）
- ECS parent chain transform CPU 烘焙（未来 phase）
- normal map TBN 矩阵的 scale 处理（当前只处理 rotation）
- `LitBatchRenderer::Stats` Lua 暴露（仅 C++ 内部，需要时再加）

---

## 7. 下一步

Phase E.2 完整收尾：写 `FINAL_PhaseE_2.md` 总结报告，commit + push + CI。
