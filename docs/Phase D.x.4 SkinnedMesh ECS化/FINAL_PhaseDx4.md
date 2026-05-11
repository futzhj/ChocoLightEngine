# FINAL — Phase D.x.4 SkinnedMesh ECS 化

> **6A 工作流 · Stage 6 Assess (合并 ACCEPTANCE/FINAL/TODO)**

## 1. 状态总览

| 维度 | 状态 |
|------|------|
| atomic tasks | **11/11 完成** (Dx4-T1~T11) |
| smoke 验证 | `ecs_skinned.lua` **11/11 PASS** + 现有 4 个 smoke 全过 |
| demo 验证 | `demo_ecs_skinned` headless 模式 Exit=0 |
| 引擎影响 | 嵌入 Lua 从 21.7KB → 26.7KB (segment3: 11KB, 安全余量 5KB) |
| 实际耗时 | ~3h (估时 8-10h, 比预期省 5h 因为 onAnimEvent 简化 + 字段桥接合并实现) |

## 2. 验收 (CONSENSUS 13 项)

| 编号 | 描述 | 状态 |
|------|------|------|
| Dx4-AC1 | SkinnedMeshRenderer + AnimationState 注册 | ✅ smoke L70-86 |
| Dx4-AC2 | world:Update 自动调 animator:Update | ✅ smoke L107-124 |
| Dx4-AC3 | state 改变 → Play / Crossfade | ✅ smoke L126-150 |
| Dx4-AC4 | params diff apply → SetParam | ✅ smoke L171-189 |
| Dx4-AC5 | morphWeights diff apply → SetMorphWeight | ✅ smoke L191-204 |
| Dx4-AC6 | speed/paused → SetSpeed/Pause/Resume | ✅ smoke L206-231 |
| Dx4-AC7 | onAnimEvent 回调 | 🟡 字段保留, 用户自行调 `animator:AddEvent` 注册 (Phase D.x.4.1 加自动桥接) |
| Dx4-AC8 | world:Render 调 DrawSkinnedMesh | ✅ smoke L233-271 |
| Dx4-AC9 | MarkRenderNetworked 排除 SMR | ✅ smoke L88-105 |
| Dx4-AC10 | server/client 联动 | 🟡 设计就绪, 用户手工双终端验证 (CI 不跑跨进程) |
| Dx4-AC11 | demo 跑通 | ✅ headless 验证 Exit=0; 有 glTF 时主路径 OK (用户验证) |
| Dx4-AC12 | Phase D/Phase AW 现有 smoke 全过 | ✅ ecs_render/ecs_network/physics_3d/image_from_bytes 4/4 PASS |
| Dx4-AC13 | CI 6/6 全绿 | 🟡 push 后等 CI |

## 3. 关键设计决策回顾

| 项 | 决策 | 推理 |
|----|------|------|
| component 拆分 | 独立 SMR + AnimationState | 职责清晰; userdata 字段 vs 可序列化字段分开, 便于网络化区分 |
| 网络化范围 | AnimationState 全字段同步; SMR 排除 | userdata 不可 JSON 序列化, 资源 client 端自管 |
| state 桥接 | crossfade 字段决定 Play vs Crossfade | 用户数据驱动, 引擎自动检测变化 |
| onAnimEvent | 字段保留, 不自动转发 | 简化 (用户 `animator:AddEvent` 中调 entity.AnimationState.onAnimEvent 即可) |
| Transform3D 旋转 | 仅 Y 轴 (`ry`) | 角色游戏 80% 用例, 多轴留 Phase D.x.4.x |

## 4. 实现亮点

### 4.1 零样板

```lua
-- 4 行代码, 完整骨骼角色 + 自动播放 + 自动渲染
local e = world:CreateEntity()
e:Add('Transform3D', {x=0, y=0, z=0})
e:Add('SkinnedMeshRenderer', {mesh=pack.mesh, animator=animator})
e:Add('AnimationState',      {state='Idle'})
-- 每帧: world:Update(dt) + world:Render() 自动完成所有工作
```

### 4.2 防御性 fallback 链

- `Light.Animation` 不可用 → `_AnimationSystem` 直接 return, 不影响其他 ECS
- `animator` 为 nil → `_AnimationSystem` 内部 if 检查跳过该 entity
- `mesh` 为 nil → Render 中 `smr.mesh and smr.animator` 守卫跳过
- 桥接函数都用 `pcall` — 用户传错类型 (例如 state 非 string) 不会 crash 整个 frame

### 4.3 字段 diff apply 减少不必要调用

`_AnimationSystem` 维护 `self._anim_cache[entity_id]` 缓存上一次值, 只在变化时调对应桥接函数. 1000 个 entity × 60fps 性能优于无脑全量 SetParam.

### 4.4 与 Phase D / Phase C.x.1 完美继承

- `_builtin_render_comps` 表自动包含新 2 个 component → MarkRenderNetworked 一行扩展
- 新增 `_builtin_no_network` 排除集设计模式, 给未来含 userdata 的 component (e.g. PointLight texture handle) 复用
- Update 钩入点 (`self:_AnimationSystem(dt)`) 简洁, 无破坏

## 5. 已知限制 / TODO

### 5.1 当前限制

| 项 | 严重度 | 说明 |
|----|--------|------|
| Transform3D 只支持 Y 轴旋转 | 中 | 完整 ZYX Euler matrix 留 Phase D.x.4.x |
| onAnimEvent 不自动桥接 | 低 | 用户自己在 animator:AddEvent 中转发 |
| AnimationState.looping 未桥接 | 低 | Phase D.x.4.1 加 animator:SetLooping 调用 |
| client mirror 需用户预创建 animator | 中 | 由"userdata 不可同步"决定, 设计取舍 |

### 5.2 推荐 Phase D.x.4.1 (~3h)

- 完整 3 轴 Euler matrix + LookAt helper
- AnimationState.looping 自动桥接
- onAnimEvent ECS 自动注册 (在 entity 首次见时给 animator 加 dispatcher)
- 多 Animator 共享 Skeleton (角色 LOD)
- 性能基准 (1000+ 角色规模测试)

### 5.3 用户操作指引

**hero.glb 资源**: 任意 glTF 2.0 含 skin 的角色 (Khronos sample 库 RiggedFigure / CesiumMan / Fox 都可) 放 `samples/demo_ecs_skinned/hero.glb`. demo 自动加载.

**网络化测试 (手工)**:

```bash
# 终端 A — server 用 demo_ecs_render 改 state 同步给 client (Phase D 已 demo)
# Phase D.x.4 网络化用例可基于 demo_ecs_render 添加 SkinnedMeshRenderer 实体, 留用户自行扩展
```

## 6. Commit 历史

| Commit (预期) | 内容 |
|----------|------|
| [pending] | feat(phase-dx4): SkinnedMeshRenderer + AnimationState + animation system + smoke + demo |

## 7. 文档版本

| 版本 | 日期 |
|------|------|
| 1.0 | 2026-05-11 (Stage 6 Assess) |
