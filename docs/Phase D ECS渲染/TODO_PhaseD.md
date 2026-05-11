# TODO — Phase D ECS 渲染系统 待办清单

> **6A 工作流 · Stage 6 Assess 交付物 (3/3)**
> 精简列出待办事宜、技术债务、用户操作指引.

---

## 1. 用户立即关注 (需决策或操作)

### 1.1 手工验证 server/client 联动 (D-AC9)

**背景**: Phase D demo 已实现 server/client 模式, 但 CI 无法跨进程验证 ECS 网络化的完整链路 (server `MarkRenderNetworked + NetworkSync` → broadcast → client `MirrorFromRoom + Render`).

**操作**:

```bash
# 终端 A
lumen-master\build\src\light\Release\light.exe samples\demo_ecs_render\main.lua server 9111

# 终端 B
lumen-master\build\src\light\Release\light.exe samples\demo_ecs_render\main.lua client 127.0.0.1 9111
```

**预期**: 终端 B 开窗显示 20 个彩色矩形跟随 server 模拟移动, 持续 ~10s 后 server 自动结束.

---

### 1.2 是否启动 Phase D.x.4 (SkeletalAnim ECS 化)

**背景**: Phase AW 完成的骨骼动画当前是独立 API (`Light.Animation.NewAnimator + DrawSkinnedMesh`), 没接入 ECS. 3D 角色游戏的核心刚需是 "ECS 实体 + 骨骼动画状态机".

**选项**:
- **A**: 立即启动 Phase D.x.4, 加 `SkinnedMeshRenderer` component + `AnimationState` component (4-6h)
- **B**: 推迟到有真实 3D 角色 demo 需求时

**推荐**: A (立即). 与"业务功能核心"对齐, 价值高.

---

### 1.3 是否补充 `Light.Graphics.Image.FromBytes(rgba)` API (Phase D.x.7)

**背景**: 当前 demo_ecs_render 没现成 PNG, 用 fallback Rectangle. 引擎缺**程序化生成 Image** 的 API, 限制 demo / 测试自给自足.

**选项**:
- **A**: 加 1h 实现 `Image.FromBytes(w, h, rgbaTable)` 接受 4*w*h 字节的 RGBA
- **B**: 让用户自己 cmake build stb_image + 提供文件路径

**推荐**: A. 1h 投入, 长期收益 (所有未来 demo 都受益).

---

## 2. 已知技术债务

### 2.1 [性能] `_FindActiveCamera` O(n) 线性扫描

**严重度**: 低 (当前规模无感)
**位置**: `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` 内嵌 Lua `ECSWorld:_FindActiveCamera`
**触发**: 每次 `world:Render` 调 2 次 (2D + 3D), 每次扫 `self._entities` 全表
**修复方案**: 维护 `_active_camera_2d` / `_active_camera_3d` 缓存, 在 `Camera2D/Camera3D` 的 Add/Set 时更新
**预计工作量**: 0.5h (留 Phase D.x.5 与 `_entities_by_id` 索引一起做)

---

### 2.2 [性能] `_CollectSprites` 每帧重建 list

**严重度**: 低 (< 1000 entity 无感)
**位置**: 同上
**触发**: 每帧 Render 时遍历所有 entity, 过滤 + 排序
**修复方案**: 维护 dirty list, 仅当 Transform2D.z 改时重新排序; entity Add/Remove Sprite 时更新 list
**预计工作量**: 1-2h

---

### 2.3 [API] Camera2D 的 `viewportW/viewportH` 字段定义了但未使用

**严重度**: 极低 (字段存在不破坏 API, 仅占内存)
**位置**: `_RegisterBuiltinRenderComponents` 中 Camera2D defaults
**理由**: 留接口给未来 viewport 裁切 / split-screen 实现, 但本 Phase 未实现
**操作**: 用户可暂时忽略这两个字段, 或自己写 system 利用. 引擎未读取它们

---

### 2.4 [demo] 缺自动化 e2e CI 验证

**严重度**: 中 (验收 D-AC9 依赖手工双终端)
**位置**: `@e:\jinyiNew\Light\samples\demo_ecs_render`
**触发**: CI 无法跑跨进程, 因此 server/client 同步未在 CI 自动验证
**修复方案**:
- 选项 A: `scripts/smoke/ecs_render_e2e.lua` 单进程 mock Room 端到端模拟双进程 ECS 同步
- 选项 B: GitHub Actions 配置 service container 跑真实双进程
**预计工作量**: 1-2h (选 A 更现实)

---

### 2.5 [文档] API_REFERENCE.md 缺 Phase D 章节

**严重度**: 中 (用户使用必要文档缺失)
**位置**: 项目根目录 `docs/API_REFERENCE.md` 应增加 `Light.ECS.World:Render / MarkRenderNetworked` 章节 + 6 个内置 component schema 说明
**预计工作量**: 1h

---

### 2.6 [继承] Phase C.x.1 的 4 项技术债务仍在

详见 `@e:\jinyiNew\Light\docs\Phase C.x.1 per-component dirty\TODO_PhaseCx1.md`:
- MSVC raw string + 行注释拼接 bug (规避方案稳定)
- `_FindById` O(n) (与 D.x.5 同批做)
- 不支持 component 字段级增量 (优先级低)
- demo_ecs_network 缺 e2e CI (与 D-T10 demo_ecs_render 一并加 §2.4)

---

## 3. 后续可启动的相关 Phase

| 候选 Phase | 主题 | 预计耗时 | 优先级 |
|-----------|------|----------|--------|
| **Phase D.x.1** | parent 层级 + 递归世界矩阵 | 2-3h | 中 |
| **Phase D.x.2** | ECS PointLight component | 1-2h | 中 |
| **Phase D.x.3** | Material as ECS component + handle 同步 | 4-6h | 低 |
| **Phase D.x.4** | SkeletalAnim ECS 化 | 4-6h | **高** |
| **Phase D.x.5** | 视锥剔除 + 索引缓存 | 2-3h | 低 |
| **Phase D.x.6** | demo_ecs_render_3d (含 glTF) | 2-3h | 中 |
| **Phase D.x.7** | `Image.FromBytes(rgba)` | 1h | **中** |
| **Phase E** | ECS 序列化 + replay | 6-8h | 中 |
| **Phase F** | ECS Particle 系统 | 4-6h | 中 |

---

## 4. 缺少的配置 / 资源

**无新增配置**. Phase D 完全是源码内部扩展, 无新外部依赖、无新环境变量、无新 .env 字段、无新 CMake 参数.

**缺资源** (非阻塞):
- `samples/demo_ecs_render/hero.png` — demo 可选, 用户提供后自动切换到 Sprite 渲染 (README §"升级到真实 Sprite 渲染" 章节有指引)

---

## 5. 文档 / Tooling 改进

### 5.1 用户文档

- [ ] `@e:\jinyiNew\Light\docs\API_REFERENCE.md` 增加 "Light.ECS Phase D 渲染 API" 章节 (含 `world:Render`, `world:MarkRenderNetworked`, 6 个内置 component schema)
- [ ] `@e:\jinyiNew\Light\samples\demo_ecs_render\README.md` 增加 server/client 端到端联调截图 (待用户实际跑后补图)

**预计工作量**: 1h

### 5.2 Tooling

- [ ] `scripts/tools/extract_embedded_lua.ps1` (Phase C.x.1 TODO 提及, 仍待做) — 把当前 PowerShell one-liner 固化为可重用工具
- [ ] CI 加 raw string 字节数 lint (cpp 中 `R"..."` 单段 < 14KB 安全阈值预警)

**预计工作量**: 2h

---

## 6. 待办速查表

| 优先级 | 项 | 工作量 | 负责模块 |
|--------|----|--------|----------|
| ⭐⭐ | 手工验证 server/client 联动 (§1.1) | 5min | 用户操作 |
| ⭐⭐ | API_REFERENCE Phase D 章节 (§5.1) | 1h | docs |
| ⭐⭐ | `Image.FromBytes` API (§1.3 / §3 D.x.7) | 1h | light_graphics_image.cpp |
| ⭐ | SkinnedMeshRenderer ECS 化 (§3 D.x.4) | 4-6h | light_ecs.cpp + light_animation 桥接 |
| ⭐ | parent 层级 (§3 D.x.1) | 2-3h | light_ecs.cpp |
| ⭐ | demo e2e mock CI (§2.4) | 1-2h | smoke |
| ◯ | `_FindActiveCamera` 缓存 (§2.1) | 0.5h | light_ecs.cpp |
| ◯ | `_CollectSprites` dirty list (§2.2) | 1-2h | light_ecs.cpp |
| ◯ | viewportW/H 实际使用 (§2.3) | 留 Phase D.x | light_ecs.cpp |
| ◯ | extract_embedded_lua 工具化 (§5.2) | 0.5h | scripts/tools |

⭐⭐ = 建议近期处理 / ⭐ = 中期 / ◯ = 实测瓶颈或具体需求触发再做

---

## 7. 未解决的开放问题

### 7.1 Sprite + 真实 Image 的网络化同步

**问题**: `Sprite.image` 是 `Light.Graphics.Image` userdata, 含 GPU texture handle. 网络化时不能直接发 userdata.

**当前行为**: `MarkRenderNetworked` 把 Sprite 标 networked, `_BuildEntityState` 浅拷贝 Sprite table 时, `image` 字段被一起序列化 — 但 userdata 在 cjson encode 时变 nil. 结果 client mirror 收到 `Sprite { image=nil, color=..., ... }`, 显示 fallback Rectangle (没 image 跳过).

**临时方案**: 网络场景用户自己映射 image 资源:
- server 端 `Sprite.imageId` (字符串 ID) 同步
- client 端在 `mirror:OnEvent('ecs_delta')` 后, 用 imageId 查本地资源表绑 image

**长期方案**: Phase D.x.3 设计 GPU 资源 handle 同步协议.

---

### 7.2 多 active=true Camera 时的行为

**问题**: `_FindActiveCamera` 找**首个** `active=true` 的相机. 若用户错配 2 个 Camera2D 都 active, 行为不确定 (取决于 entity 创建顺序).

**操作指引**: 文档中说明"同时只能有 1 个 active 相机, 切换时改 `active` 字段". 引擎不主动检测冲突.

**长期方案**: 加 smoke 警告或 viewport 路由系统 (split-screen 才需要).

---

### 7.3 Render 阶段是否该 hook 物理刷新

**问题**: 物理引擎 (`Light.Physics3D` Bullet) 每帧 `Step` 后更新 rigid body world matrix. 是否应该自动写回 `Transform3D`?

**当前**: 不自动. 用户得自己写 system 把 body matrix → Transform3D.

**操作指引**: 后续若做 Phase 物理 × ECS 整合, 加 `PhysicsBody` component + 内置 sync system.

---

## 8. 操作指引 (FAQ)

### Q1: Phase D 集成到我的现有项目要改什么?

**答**: 兼容现有 Phase C/C.x.1 代码, 无 breaking change. 直接享受:
- 6 个内置 component 注册可立即用 (但用户已注册同名时, 引擎跳过, 用户优先)
- 调用一次 `world:Render()` 触发内置渲染

可选优化:
```lua
world:MarkRenderNetworked()  -- 多人模式开启网络同步内置 component
```

### Q2: 如何在 demo 里替换 fallback Rectangle 为真实 Sprite?

**答**:
1. 把任意 PNG 命名 `hero.png` 放 `samples/demo_ecs_render/`
2. 修改 demo 的 spawnEntities 处:
   ```lua
   local heroImg = Light(Light.Graphics.Image):New('samples/demo_ecs_render/hero.png')
   e:Add('Sprite', { image = heroImg, ... })  -- ← 加 image 字段
   ```
3. fallback Rectangle 段会自动 skip (因为 `not sp.image` 不成立)

### Q3: 我自己定义了 Transform2D component 会冲突吗?

**答**: 不会. `_RegisterBuiltinRenderComponents` 检测到 `_components[name]` 已存在则跳过该项. 用户优先. 但请保证字段语义与内置一致 (x/y/z/rot/sx/sy/ox/oy), 否则 `_DrawSprite` 内部读字段会拿到 nil → 用默认值.

### Q4: 我想要 60fps 固定步长怎么办?

**答**: 自己实现 fixed timestep:
```lua
local ACCUM = 0
function Game:Update(dt)
    ACCUM = ACCUM + dt
    while ACCUM >= 1/60 do
        world:Update(1/60)
        ACCUM = ACCUM - 1/60
    end
end
```

---

## 9. 文档版本

| 版本 | 日期 | 备注 |
|------|------|------|
| 1.0 | 2026-05-11 | Phase D Stage 6 Assess TODO 初版 |

文档维护: 每次启动 Phase D.x.N (N ≥ 1) 时, 在 §3 "后续 Phase" 表中划掉完成项; 完成后在 §6 速查表标 ✓.
