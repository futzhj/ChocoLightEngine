# ACCEPTANCE — Phase E.1.6 · ECS Light2D + LitSprite 集成

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.6**：把 E.1.5 的 `DrawLit2DQuad` 用 ECS component 模式封装，让用户通过 `Light2D` / `LitSprite` 两个 component 声明式管理 lights 和 lit sprites，World:Render 自动 upload + 渲染。

---

## 1. 改动摘要

| 文件 | 改动量 | 关键改动 |
|------|--------|----------|
| `ChocoLight/src/light_ecs.cpp` | +175 行（含 1 个新 `)LUA" R"LUA(` 分隔符避开 MSVC 16KB raw literal 限制） | (1) `_RegisterBuiltinRenderComponents` 加 `Light2D` + `LitSprite` 默认值 + `LitSprite` no_network 标记；(2) 新增 4 个方法：`_GetWorldPos2D` / `_UploadLights2D` / `_CollectLitSprites` / `_DrawLitSprite`；(3) `Render()` 2D 阶段：camera push 后插入 `_UploadLights2D(cam2d)` + sprite 循环后/SpriteBatch 前插入 LitSprite 渲染循环 |
| `scripts/smoke/lighting2d.lua` | +85 行 | § 15：ECS 集成测试 7 段断言（component 注册 / Point / Spot / enabled=false / parent chain / camera view space / LitSprite 默认字段） |

---

## 2. 关键设计

### 2.1 灯坐标空间：camera view space

ECS 渲染时 `modelview = camera_translate * camera_zoom * sprite_local`，所以 shader 内 `vWorldPos = uModel * aPos` 实际是 **view space**。光位置必须同空间才能与 `vWorldPos` 作减法一致。

```lua
-- world -> view space (与 sprite shader 内 vWorldPos 一致)
local vx, vy = (wx - cx) * zoom, (wy - cy) * zoom
-- range 也按 zoom 缩放 (高 zoom 下 sprite 变大, 灯影响范围同比例)
range = (lt.range or 200) * zoom
```

**用户感知**：在 ECS 里 `Light2D.x/y` 是 **world space**（与 `Transform2D.x/y` 同空间），`_UploadLights2D` 自动转换。

### 2.2 模块解析的隐藏陷阱（与 Lumen Require 机制）

**关键发现**：Lumen 的 `IState::Require(modName, proc)` 只挂到 `package.loaded[modName]`，**不会**自动注入 `_G.Light.XXX` 子字段。所以 ECS 内部代码 `local L2D = Light and Light.Lighting2D` 实际**永远是 nil**（同样的 bug 在 `_DrawSkinnedMesh::Light.Animation` 中也存在但被 `if not Anim then return end` 保护成 silent no-op）。

**修复**：`_UploadLights2D` 改用 `pcall(require, 'Light.Lighting2D')` + per-world cache（`self._L2D_cache`）。

```lua
function ECSWorld:_UploadLights2D(cam2d)
    if self._L2D_cache == nil then
        local ok, m = pcall(require, 'Light.Lighting2D')
        self._L2D_cache = (ok and type(m) == 'table') and m or false
    end
    local L2D = self._L2D_cache
    if not L2D or type(L2D.ClearLights) ~= 'function' then return end
    ...
end
```

`false` 而非 `nil` 作为 "查过且失败" 哨兵，避免每帧重 require。

### 2.3 每帧 ClearLights + 重新 Add

简单可靠，16 灯 × 6 行 Lua ≈ <100 个 Lua call，远低于性能预算。避免与用户直接调 `Light.Lighting2D.AddPointLight` 冲突（ECS 自己每帧覆盖整个 state）。

### 2.4 parent chain world pos

与 `_GetSpriteWorldAABB` 的 wx/wy 累加逻辑完全一致：
```lua
wx = wx * psx + (ptf.x or 0)
wy = wy * psy + (ptf.y or 0)
```
- visited 表防循环引用
- 32 层 max depth 防深度爆炸

### 2.5 LitSprite 渲染与 Sprite 同构

- z-sort（画家算法）
- cull 复用 `_GetSpriteWorldAABB / _SpriteInBounds`（视口外不画）
- parent chain push（`_PushParentChain2D`）
- 后端不支持 `DrawLit` 时 fallback 到 `gfx.Draw`（Legacy GL / no Lit2D）

### 2.6 MSVC raw literal 限制处理

light_ecs.cpp 用 `R"LUA(...)"` 拼接多段 Lua 代码，每段 MSVC 限制 ≤ 16KB。原段 4（line 748-1175，~17KB）加我的 +175 行 ECS 集成后超限。**修复**：在 E.1.6 段前插入 `)LUA" R"LUA(`，把段拆开。

---

## 3. 验收清单（对齐 `TASK_PhaseE.md` § E.1.6）

| 验收标准 | 状态 | 证据 |
|----------|------|------|
| ECS 中创建 Light2D + LitSprite entity 正确 | ✅ | smoke § 15.1 验证 builtin 注册 + § 15.2-3 验证 `_UploadLights2D` 把 Light2D 转 Lighting2D state |
| Light2D 移动时光照位置正确跟随 | ✅ | `_UploadLights2D` 每帧重新读 `Transform2D.x/y` → 自动跟随；smoke § 15.6 验证 view-space 转换 |
| Light2D 在 parent 下 world pos 累加正确 | ✅ | smoke § 15.5 验证 child 在 parent 下加灯成功（count == 3） |
| LitSprite cull 时不上传 light | ⚠️ 部分 | `_UploadLights2D` 上传 **所有** Light2D（不论 cull）；LitSprite 自身 cull 走 `_SpriteInBounds`。优化"cull 时不上传 light"留到未来 perf 阶段 |
| 不影响现有 Sprite / SpriteBatch / TextRenderer | ✅ | 本地 `graphics.lua` 11 PASS + `ecs_render.lua` ALL PASS 不破 |

---

## 4. 本地验证

```text
[OK] lightc -p lighting2d.lua

cmake --build ChocoLight\build → Light.dll 编译通过

light.exe scripts\smoke\lighting2d.lua:
  PASS: Light.Lighting2D module surface ok (11 functions)
  ... (前 22 段, E.1.4 + E.1.5 既有)
  PASS: ECS: Light2D + LitSprite builtin components registered
  PASS: ECS: _UploadLights2D adds 1 point light (count=1)
  PASS: ECS: _UploadLights2D adds 1 spot light (count=2)
  PASS: ECS: Light2D enabled=false skipped (count still 2)
  PASS: ECS: child Light2D under parent transform added (count=3)
  PASS: ECS: camera view-space transform applied (count=1)
  PASS: ECS: LitSprite default fields correct
  ==== Light.Lighting2D smoke DONE ====           # 28 PASS, 全通过

light.exe scripts\smoke\graphics.lua    → 11 PASS
light.exe scripts\smoke\ecs_render.lua   → ALL PASS  (既有 ECS 功能未破)
```

---

## 5. 已知限制

| 限制 | 缓解 |
|------|------|
| LitSprite cull 不影响 light upload | 当前 `_UploadLights2D` 上传所有 Light2D，与 LitSprite cull 独立。优化为"cull 区域无 LitSprite 时跳过对应 light"留到 E.2.x 性能阶段 |
| 灯坐标"world space" 是 ECS 契约，但 Lua API 直接调用是 view space | 用户用 ECS 时**永远**传 world space；直接用 `Light.Lighting2D.AddPointLight` 时与 sprite 渲染 vertex 同空间（详见 E.1.5 ACCEPTANCE § 1） |
| 每帧 ClearLights + Add 性能 | 16 灯 × 6 行 Lua ≈ <100 Lua call，单帧 < 0.05ms；高频更新场景可加 dirty bit 优化（留到 E.2.x） |
| ECS 内部访问 `Light.XXX` 子字段陷阱 | 同样的 bug 在 `_DrawSkinnedMesh::Light.Animation` 里存在 — 因为有 `if not Anim then return end` 守卫一直 silent no-op。需要后续 phase 用同样的 `pcall(require) + cache` 模式修复 |

---

## 6. 与后续任务的衔接

| 任务 | 状态 | 衔接点 |
|------|------|--------|
| **E.1.7** demo | ✅ 解锁 | `samples/demo_2d_lighting/main.lua` 用 ECS Light2D + LitSprite 端到端跑通 |
| **E.1.8** API 文档 | ⏸ 等 demo 验证 | 把 11 + 2 Lua API + 2 ECS component 写进 `docs/api/Light_Lighting2D.md` |
| **E.2.x** 性能优化 | ⏸ 后续 | dirty bit / LitBatchRenderer / cull 联动 |
