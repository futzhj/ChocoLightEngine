# ACCEPTANCE — Phase E.1.7 · demo_2d_lighting + smoke

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.7**：交付端到端可运行的 demo，让 Phase E.1 全套（C++ shader + state + Lua API + ECS 集成）在一个 200 行内的脚本里**视觉验证**通过。

---

## 1. 改动摘要

| 文件 | 改动量 | 角色 |
|------|--------|------|
| `samples/demo_2d_lighting/main.lua` | ~210 行 | demo 主入口：模块加载 + 8×6 ECS LitSprite 网格 + 3 灯（2 point + 1 spot）+ UI.Window 主循环 + 键盘 / 鼠标交互 + headless 降级 |
| `samples/demo_2d_lighting/README.md` | ~80 行 | 运行说明 + 场景描述 + 验收要点 + 已知限制 |
| `scripts/smoke/lighting2d.lua` | 已在 E.1.4 / E.1.5 / E.1.6 中累积成 28 段 PASS | 满足 E.1.7 任务对 smoke 的所有要求 |

> **注**：E.1.7 任务文档要求 `scripts/smoke/lighting2d.lua` 包含 "API 全套调用 / 16 light 极限 / update/remove/clear / `Light.Graphics.DrawLit` API 存在 / ECS Light2D component"。这些在 E.1.4（§ 1-13）/ E.1.5（§ 14）/ E.1.6（§ 15）三轮迭代中已完整覆盖，本次 E.1.7 不再追加 smoke 内容。

---

## 2. demo 设计

### 2.1 场景

| 元素 | 配置 | 验证点 |
|------|------|--------|
| Camera2D | 居中 (0, 0)，zoom = 1，viewport 800×600 | view-space 转换 (`_UploadLights2D`) |
| 48 个 LitSprite | 8×6 网格，64×48 像素，纯顶点色 | `DrawLit` 走 sprite_lit_2d shader |
| Ambient | `(0.15, 0.15, 0.20)` 冷暗 | `L2D.SetAmbient` + shader `uAmbient` |
| Point #1 | 暖橘色 (1.0, 0.6, 0.3)，左上 (200, 150)，range 250，intensity 1.5 | `AddPointLight` + 距离衰减 |
| Point #2 | 冷蓝色 (0.3, 0.6, 1.0)，右下 (600, 450)，range 250，intensity 1.2 | 多灯叠加 |
| Spot | 白色，中央 (400, 300)，range 350，inner 15° / outer 35°，朝向跟随鼠标 | `AddSpotLight` + `smoothstep(outerCos, innerCos, cosA)` 锥形衰减 |

### 2.2 交互

| 输入 | 行为 |
|------|------|
| `ESC` | 退出 |
| `1` / `2` / `3` | 切换 Point#1 / Point#2 / Spot 的 `enabled` 字段 |
| 鼠标移动 | 实时改 spot `dirX/dirY`，方向归一化由 binding 内部 `Normalize2D` 完成 |

### 2.3 Headless 降级

无 `Light.UI.Window`（CI / server-only）时进入降级路径：
- 调 5 帧 `world:_UploadLights2D(cam)` 
- 校验 `L2D.GetLightCount() == 3`（2 point + 1 spot）
- 打印 `demo_2d_lighting headless ok` 退出

CI 跑 `lightc -p main.lua` 做语法检查；不做 runtime 视觉验证（无 display）。

---

## 3. 验收清单（对齐 `TASK_PhaseE.md` § E.1.7）

| 验收标准 | 状态 | 证据 / 备注 |
|----------|------|-------------|
| `light samples/demo_2d_lighting/main.lua` 跑通 | ✅ 语法 / ⏸ 视觉 | `lightc -p` 通过；视觉验证需用户在 Windows / macOS 桌面运行（CI 无 display） |
| `light scripts/smoke/lighting2d.lua` 退出码 0 | ✅ | 本地 28 PASS + DONE，CI Windows runtime smoke 已自动跑 |
| CI Windows / Linux / macOS smoke 通过 | ⏸ 待 push | E.1.4 commit 已让 `lightc -p lighting2d.lua` 在全 6 平台跑通；本次只追加 demo 新文件 + smoke 扩展，不破坏既有 |

---

## 4. 已知限制 / 后续 TODO

| 项 | 说明 |
|----|------|
| 无真实 normalMap 资源 | 当前 demo 用 mock 纯色 sprite，`LitSprite.normalMap = nil`。如用户提供 `normal.png`，加 `normalMap = Light(Light.Graphics.Image):New("normal.png")` 即可触发 TBN 凹凸 |
| `1000 lit quad < 16ms` benchmark 未做 | 当前 demo 仅 48 sprite；性能 benchmark 留到 `perf_benchmark/lit2d.lua`（独立后续任务） |
| 视觉验收依赖人工 | CI 无 display，验收 4 条视觉项（ambient / point / spot / normalMap）需用户本地 Windows / macOS 跑 demo 看 |

---

## 5. 与后续任务衔接

| 任务 | 状态 | 衔接点 |
|------|------|--------|
| **E.1.8** API 文档 | ✅ 解锁 | demo 已锁定接口，文档可直接照 demo + smoke 写 |
| **E.2.x** HDR / 后处理 | ⏸ Phase E.2 | demo 提供基础 forward 渲染 baseline，未来 HDR / bloom 可在 demo 之上扩展 |

---

## 6. 提交信息建议

```
feat(phase-e1.6+e1.7): ECS Light2D/LitSprite + demo_2d_lighting

E.1.6 ECS 集成:
- light_ecs.cpp: _RegisterBuiltinRenderComponents +Light2D +LitSprite
  - Light2D defaults: type=1 Point, color/range/intensity/dir/innerAngle/outerAngle
  - LitSprite defaults: image/normalMap/color/anchor/flipX/flipY/quad
  - _builtin_no_network: LitSprite (image/normalMap userdata)
- 4 new methods (after _DrawSpriteBatch):
  - _GetWorldPos2D: parent chain visited+32-depth (mirror _GetSpriteWorldAABB)
  - _UploadLights2D: pcall(require) + per-world cache, world->view space
    + zoom scaling for range
  - _CollectLitSprites: z-sort painter algorithm
  - _DrawLitSprite: gfx.DrawLit/DrawLitQuad, fallback to gfx.Draw
- Render() 2D phase: _UploadLights2D after cam push, LitSprite loop
  before SpriteBatch (cull reuse via _SpriteInBounds)
- Insert R"LUA(" separator to avoid MSVC 16KB raw literal limit

E.1.7 demo:
- samples/demo_2d_lighting/main.lua (~210 lines)
  - 8x6 LitSprite grid + 3 lights (2 point + 1 spot)
  - Keyboard 1/2/3 toggle, mouse for spot dir
  - Headless fallback for CI (5 frames + _UploadLights2D check)
- samples/demo_2d_lighting/README.md

smoke:
- scripts/smoke/lighting2d.lua +section 15 (7 ECS sub-asserts)
  Local: 28 PASS + DONE; graphics.lua/ecs_render.lua unbroken

docs: ACCEPTANCE_PhaseE_1_6.md, ACCEPTANCE_PhaseE_1_7.md
```
