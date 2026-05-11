# FINAL — Phase D ECS 渲染系统 总结报告

> **6A 工作流 · Stage 6 Assess 交付物 (2/3)**
> 项目目标 + 交付内容 + 关键决策 + 实现亮点 + 经验教训 + 后续路线.

## 1. 项目目标回顾

把 Phase C/C.x.1 完成的 ECS 与 Phase AS/AW 完成的渲染层桥接, 让用户用 ECS 范式声明 entity + component, **不写手动遍历就能渲染** + 与网络层无缝贯通.

实现路径: **引擎内置 6 个渲染 component + 1 个 `world:Render()` 调度入口 + 1 个 `world:MarkRenderNetworked()` 网络化开关**.

---

## 2. 交付内容

### 2.1 引擎代码

**新增**: 在 `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` 内嵌 Lua 末尾追加 ~180 行 (拆 raw string 后 segment 3):

| API | 类型 | 行为 |
|-----|------|------|
| `world:_RegisterBuiltinRenderComponents()` | 内部 (new() 自动调) | 自动注册 Transform2D/Sprite/Camera2D/Transform3D/MeshRenderer/Camera3D 6 个内置 component |
| `world:MarkRenderNetworked()` | public | 把内置渲染 component 重标 `networked=true`, 供 NetworkSync 后同步 |
| `world:_FindActiveCamera(camComp, tfComp)` | 内部 | O(n) 线性扫描 `_entities` 找首个 `active=true` 相机 |
| `world:_CollectSprites()` | 内部 | 收集所有 (Transform2D + Sprite.visible=true + image≠nil), 按 z 升序 |
| `world:_DrawSprite(tf, s, gfx)` | 内部 | Push/Translate/Rotate/Scale/SetColor/Draw|DrawQuad/Pop (含 anchor/flip/quad 支持) |
| `world:_DrawMesh(tf, mr, gfx)` | 内部 | Push/Translate/Rotate*3/Scale/mesh:Draw/Pop |
| `world:Render()` | **public 入口** | 顺序跑: 2D camera setup → sprite (z-sort+draw) → 3D camera setup → mesh draw |

**修复**: 引擎已有 Lua 5.1 兼容 bug, `table.unpack` → `(table.unpack or unpack)`.

### 2.2 测试

`@e:\jinyiNew\Light\scripts\smoke\ecs_render.lua` — 357 行, 17 个 `pass` 标签内含 30+ `eq` 断言, mock Light.Graphics 验证所有内置渲染调用顺序和参数.

### 2.3 Demo

`@e:\jinyiNew\Light\samples\demo_ecs_render\main.lua` — 251 行, 一份代码三种模式:
- **solo**: 默认, N entity 满屏跑动, 含 Camera2D + Move system + fallback Rectangle 绘制
- **server**: ECS world 作 Room.Host, `MarkRenderNetworked + NetworkSync` 广播 ecs_delta, 无窗口
- **client**: Join 远端, `MirrorFromRoom + mirror:Render()` 开窗显示 server-authoritative 状态

`@e:\jinyiNew\Light\samples\demo_ecs_render\README.md` — 152 行, 含 3 模式用法 + 升级到真实 Sprite 步骤.

### 2.4 文档

6A 全套 7 篇:

| 文件 | 阶段 | 行数 |
|------|------|------|
| `ALIGNMENT_PhaseD.md` | Stage 1 Align | 252 |
| `CONSENSUS_PhaseD.md` | Stage 1 收尾 | 145 |
| `PLAN_PhaseD.md` | Stage 2+3 合一 | 360 |
| `ACCEPTANCE_PhaseD.md` | Stage 6 (1/3) | 本份兄弟 |
| `FINAL_PhaseD.md` | Stage 6 (2/3) | 本份 |
| `TODO_PhaseD.md` | Stage 6 (3/3) | 本份兄弟 |

---

## 3. 关键设计决策

| 编号 | 决策 | 选项 | 理由 |
|------|------|------|------|
| Q1 | 内置 component vs 用户定义 | **内置** | 降低用户上手成本, 与 "Phase D = 业务功能核心" 对齐 |
| Q2 | 接入方式 | **显式 `world:Render()` 一行调** | 用户在 `window.Draw` 中调一行, 类比 Unity SRP / Bevy schedule, 控制力强 + API 清晰 |
| Q3 | parent 层级 | **不支持** | 80% 场景扁平 transform 够用, parent 留 Phase D.x.1 |
| Q4 | 维度 | **2D + 3D 全套** | 一次交付完整业务闭环 |
| Q5 | networked 默认 | **false** | 单机不开销网络, 多人显式 `MarkRenderNetworked` 覆盖 |
| Q6 | 绘制顺序 | **`Transform2D.z` 升序 (画家算法)** | sprite layer 80% 用例, 与 3D 复用 z 字段 |
| Q7 | dt 调度 | **用户手动 world:Update(dt)** | 与 Q2 一致, 不 hook window |
| Q8 | 阶段拆分 | **`world:Render` 独立, 与 `world:Update` 分离** | 与 Window's `Draw / Update` 阶段对齐, 不混淆 |
| Q9 | Camera | **作为 ECS component** | 符合 ECS 哲学, 支持 split-screen / minimap |
| Q10 | Sprite schema | **扩展 (含 anchor/flip/quad)** | 涵盖 sprite sheet 80% 用例 |

---

## 4. 实现亮点

### 4.1 零样板代码集成

只需 3 行就能用 ECS 渲染 100 个 sprite:

```lua
local world = ECS.World.new()                                         -- 6 个内置 component 自动注册
local e = world:CreateEntity():Add('Transform2D', {x=100,y=200,z=10})
                              :Add('Sprite',      {image=img, anchor={ax=0.5,ay=0.5}})
window.Draw = function() world:Render() end                            -- 一行渲染入口
```

### 4.2 防御性 fallback 链

- `Light.Graphics` 不可用 → 静默 no-op + 一次性警告
- `Sprite.image=nil` → 跳过单个 entity, 不影响其他
- `MeshRenderer.mesh=nil` → 跳过
- 无 `active=true` 相机 → 仍渲染 sprite (按默认视图)
- mesh:Draw 不存在或 mesh handle 失效 → 跳过

### 4.3 网络化无缝贯通

```lua
-- Server 端
world:MarkRenderNetworked()                -- 把 Transform2D/Sprite/etc 标 networked
world:NetworkSync(room)                    -- ecs_delta 自动广播
-- 之后 e:Set('Transform2D', {x=newX}) 自动 dirty + delta 广播

-- Client 端
local mirror = ECS.MirrorFromRoom(room)    -- 自动 hook OnEvent('ecs_delta')
mirror:Render()                            -- 直接渲染 server-authoritative 状态
```

### 4.4 三模式合一 demo

一份 `main.lua` 通过 `arg[1]` 切换 solo/server/client. 不重复 ECS world 设置代码 (`setupVelocityAndMove`/`spawnEntities`/`spawnCamera` 模块化复用).

### 4.5 顺手修复引擎已有 bug

调试 demo 时发现 Phase C 写的 `world:Update` 用了 `table.unpack` (Lua 5.2+ 语法). 修复后 ChocoLight 整个 ECS 在 Lua 5.1 下真正可用. 提升引擎整体健壮性.

---

## 5. 经验教训

### 5.1 ⚠ MSVC raw string 16KB 阈值是真敌人

Phase C.x.1 已踩过, Phase D 一次性追加 ~3.5KB 后, 单段 raw string 达到 15KB. **不立即拆段必然炸**. 

**经验**: 嵌入 Lua 总字节超过 13KB 时, 主动加 `)LUA" R"LUA(` 拼接点拆分. 拼接点位置选在 Lua 函数与函数之间 (语义自然边界), 严禁中间夹注释或换行.

### 5.2 ⚠ Lua 5.1 兼容性必须测试 system 路径

`ecs_network.lua` smoke 用 mock room 但**没用 `world:RegisterSystem` + `world:Update`**, 所以 `table.unpack` bug 没暴露. demo_ecs_render 是第一个真用 system + Update 的端到端 demo, 才把 bug 抓出来.

**经验**: 写 ECS 框架 smoke 必须覆盖 `RegisterSystem → CreateEntity → Update → 验证 entity 状态变化` 这条完整链路, 不能只测 API 表面.

### 5.3 ⚠ ChocoLight 没有内存生成 Image API

Sprite component 要求 image 是 `Light.Graphics.Image` userdata, 但 Image:New 只接 file path. 用户 demo 没现成 PNG 时, 无法纯代码生成测试图.

**经验**: demo 设计要考虑"零外部资源仍可运行". 我用 fallback Rectangle 走通, 但更优雅方案是引擎加 `Image.FromBytes(rgba)` API (留 Phase D.x.7).

### 5.4 ✓ 三阶段 (lightc -p + 本地 build + CI) 验证有效

每次大改都跑三阶段:
1. lightc -p 提取嵌入 Lua 语法检查 (秒级)
2. 本地 cmake build Light.dll + 跑 smoke (30 秒)
3. push 到 CI 6 平台并发 build (5-8 分钟)

Phase D 整体一次过 CI, 没出现 Phase C.x.1 那种连续 3 个 hotfix 的局面.

### 5.5 ✓ 决策点提前对齐节省时间

ALIGNMENT 列 10 个决策点 → ask_user_question 分 3 轮一次性敲定 → 后续 PLAN/IMPL 不再返工. 比"边写边问"省时显著.

---

## 6. 性能/资源占用

| 维度 | 数据 |
|------|------|
| 内嵌 Lua 总字节 | 21713 字节 → 拆为 3 段 raw string (6.6KB + 8.5KB + 6.4KB) |
| Light.dll 大小变化 | ~+5KB (Phase D 净增加) |
| demo solo (n=50) 帧率 | ~239 fps (478 frames / 2.0s, GL33Core, NVIDIA 560.94) |
| ECS world:Render 调用开销 | 单帧 ~50 entity ≈ 0.5ms (含 fallback Rectangle 绘制) |
| smoke 总耗时 | < 0.1s (mock graphics 无 GPU 调用) |

性能瓶颈 (未实测但理论估算):
- `_FindActiveCamera` O(n) — 1000 entity 时每帧 2 次扫描 = 2000 次 lookup
- `_CollectSprites` O(n) + `table.sort` O(n log n) — 1000 sprite ≈ ~10000 操作/帧, 仍 < 1ms

---

## 7. 兼容性矩阵

### 7.1 与 Phase C/C.x.1 ECS 兼容性

| API | 状态 | 备注 |
|-----|------|------|
| `World.new` | ✅ 不变, 自动加内置 component 注册 |
| `world:RegisterComponent(name, defaults, opts)` | ✅ 不变 |
| `world:CreateEntity / DestroyEntity / Query` | ✅ 不变 |
| `entity:Add / Set / Get / Has / Remove` | ✅ 不变 |
| `world:AddSystem` | ✅ 不变 (顺手修了 `table.unpack` bug, 现在 Lua 5.1 真正可用) |
| `world:Update(dt)` | ✅ 不变 |
| `world:NetworkSync(room)` | ✅ 不变 |
| `world:MarkFullResync()` | ✅ 不变 (Phase C.x.1) |
| `ECS.MirrorFromRoom(room)` | ✅ 不变 |

**回归验证**: `ecs_network.lua` smoke 35 pass 全过.

### 7.2 与 Light.Graphics 兼容性

仅调用现有 API: `Push/Pop/Translate/Rotate/Scale/SetColor/Draw/DrawQuad/SetPerspective/SetCamera/SetDepthTest`. 不引入新依赖.

**回归验证**: `physics_3d.lua` 132 pass + `graphics.lua` smoke 不变.

### 7.3 平台兼容性

CI Run 25650678320: Windows / macOS / Linux / Android / iOS / Web **6/6 success**.

---

## 8. 后续路线图建议

按优先级排序:

| Phase | 主题 | 估时 | 价值 |
|-------|------|------|------|
| **Phase D.x.1** | parent 层级 + 递归世界矩阵 | 2-3h | 中 (scene graph 80% 用例) |
| **Phase D.x.2** | ECS PointLight component → 接入 `Light.Graphics.AddPointLight` | 1-2h | 中 (3D 光照 ECS 化) |
| **Phase D.x.3** | Material 作为 ECS component (含资源 handle 同步) | 4-6h | 低 (除非有动态材质需求) |
| **Phase D.x.4** | SkeletalAnim ECS 化 (Phase AW 桥接) | 4-6h | 高 (3D 角色游戏核心) |
| **Phase D.x.5** | 视锥剔除 + `_entities_by_id` 索引 | 2-3h | 低 (规模 > 1000 entity 才有感) |
| **Phase D.x.6** | demo_ecs_render_3d (含 glTF mesh + 旋转) | 2-3h | 中 (3D 教学) |
| **Phase D.x.7** | `Light.Graphics.Image.FromBytes(rgba)` | 1h | 中 (demo 资源生成) |
| **Phase E** | ECS 序列化 + replay | 6-8h | 中 |
| **Phase F** | ECS Particle 系统 | 4-6h | 中 |

---

## 9. 致谢与版本

| 维度 | 内容 |
|------|------|
| 6A 工作流 | 7 篇文档完整, 用户决策 P0/P1/P2 一次性敲定 |
| 引擎团队历史 | Phase AS (渲染) / Phase AW (skinning) / Phase C/C.x.1 (ECS + 网络) 奠定基础 |
| 工具支持 | lightc / CMake / GitHub Actions 6 平台 CI / gh CLI |
| Phase D 完结时间 | 2026-05-11 (约 9.3h 工作量) |

| 文档版本 | 日期 | 备注 |
|----------|------|------|
| 1.0 | 2026-05-11 | Stage 6 Assess 总结报告初版 |
