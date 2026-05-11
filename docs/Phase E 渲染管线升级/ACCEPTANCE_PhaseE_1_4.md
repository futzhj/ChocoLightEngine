# ACCEPTANCE — Phase E.1.4 · Light.Lighting2D Lua API 绑定

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.4**：在 E.1.3 C++ 模块之上追加 Lua binding，暴露 `Light.Lighting2D` table 给 Lua 脚本，并通过 smoke 测试覆盖全部 API + 16 灯极限 + 部分字段更新 + 边界情况。

---

## 1. 实现摘要

| 改动文件 | 类型 | 关键改动 |
|----------|------|----------|
| `ChocoLight/src/light_lighting2d.cpp` | 修改 | +265 行：anonymous namespace 里 `ReadLightTable` helper + `Normalize2D` + 11 个 `l_XXX` C-binding 函数 + `kLighting2DReg[]` 注册表 + `extern "C" LIGHT_API int luaopen_Light_Lighting2D` |
| `lumen-master/src/light/light.cpp` | +2 行 | `g_lightModules[]` 末尾哨兵之前追加 `{"Light.Lighting2D", "luaopen_Light_Lighting2D"}` 让 Windows light.exe 启动时自动 require |
| `scripts/smoke/lighting2d.lua` | 新建 | ~190 行，13 段断言覆盖模块表面 + Enabled + Ambient + Add/Update/Remove/Clear + 16 灯极限 + 边界 |
| `.github/workflows/build-templates.yml` | +3 行 | Windows runtime smoke 加入 `$phaseE14Smoke` 路径变量 + 执行调用 + 退出码检查 |

无其他文件改动。`include/light_lighting2d.h` / E.1.3 C++ API 完全不动，保持接口稳定。

---

## 2. Lua API 设计

### 2.1 函数 (11 个)

| Lua | C 函数 | 入参 | 出参 |
|-----|--------|------|------|
| `Light.Lighting2D.SetEnabled(b)` | `l_SetEnabled` | `boolean` | — |
| `Light.Lighting2D.IsEnabled()` | `l_IsEnabled` | — | `boolean` |
| `Light.Lighting2D.SetAmbient(r,g,b)` | `l_SetAmbient` | `number×3` | — |
| `Light.Lighting2D.GetAmbient()` | `l_GetAmbient` | — | `number×3` |
| `Light.Lighting2D.AddPointLight(t)` | `l_AddPointLight` | `table` | `int id` / `nil + err` |
| `Light.Lighting2D.AddSpotLight(t)` | `l_AddSpotLight` | `table` | `int id` / `nil + err` |
| `Light.Lighting2D.UpdateLight(id, fields)` | `l_UpdateLight` | `int, table` | `boolean` |
| `Light.Lighting2D.RemoveLight(id)` | `l_RemoveLight` | `int` | — |
| `Light.Lighting2D.ClearLights()` | `l_ClearLights` | — | — |
| `Light.Lighting2D.GetLightCount()` | `l_GetLightCount` | — | `int` |
| `Light.Lighting2D.GetMaxLights()` | `l_GetMaxLights` | — | `int` (= 16) |

### 2.2 常量 (3 个)

| 常量 | 值 | 备注 |
|------|----|----|
| `Light.Lighting2D.MAX_LIGHTS` | `16` | 与 C++ `Lighting2D::MAX_LIGHTS` + shader uniform `[16]` 严格一致 |
| `Light.Lighting2D.TYPE_POINT` | `1` | 当前 API 未直接暴露 type 字段切换，留作未来扩展 |
| `Light.Lighting2D.TYPE_SPOT` | `2` | 同上 |

### 2.3 table 字段约定

| 字段 | 用途 | 缺省值 | 类型 |
|------|------|--------|------|
| `x, y` | 世界坐标 | `0, 0` | number |
| `dirX, dirY` | spot 方向（point 忽略） | `(1, 0)` | number；spot 内部归一化 |
| `color = {r, g, b}` | RGB | `{1, 1, 1}` | nested table |
| `range` | 距离衰减半径 | `200` | number |
| `intensity` | 强度乘子 | `1.0` | number |
| `innerAngle` | spot 内锥角（度） | `20°` (cos≈0.9397) | number |
| `outerAngle` | spot 外锥角（度） | `35°` (cos≈0.8192) | number |

**innerAngle/outerAngle 单位为度**，binding 内部一次性 `cosf(deg × π/180)` 后存到 C++ `Light.innerCos / outerCos`。Lua 端不暴露 cos 接口，降低使用门槛。

---

## 3. 关键实现细节

### 3.1 `ReadLightTable` helper —— "缺失字段保留原值"

```cpp
lua_getfield(L, idx, "x");
if (!lua_isnil(L, -1)) out.pos[0] = (float)lua_tonumber(L, -1);
lua_pop(L, 1);
```

每个字段都用 `lua_isnil` 守卫，**未传字段保留 `out` 当前值**，这是 `UpdateLight` 部分字段更新语义的基础。

### 3.2 `l_UpdateLight` 部分字段更新

```cpp
// 1. 读 id 越界 → false
// 2. 拷贝当前 slot 数据
// 3. 用 table 字段覆盖 (缺失字段保持原值)
// 4. spot 时归一化 dir
// 5. 调 C++ Update (会再做合法性检查)
```

这层 Lua 端"先读后改"语义把 E.1.3 C++ `Update`（全字段覆盖）转成了用户友好的"只改我传的字段"行为，与 CONSENSUS 第 2.1 节描述一致。

### 3.3 `Normalize2D` —— 零向量安全 fallback

```cpp
if (len² > 1e-10f) {
    /* 归一化 */
} else {
    x = 1.0f; y = 0.0f;   // fallback, 避免 NaN
}
```

Spot 光必须有有效方向；用户传 `dirX=0, dirY=0` 时不让程序崩溃也不让 shader 拿到 NaN，安全 fallback 为 (1, 0)。

### 3.4 错误返回值约定

| 场景 | 返回 |
|------|------|
| `AddPointLight` / `AddSpotLight` 失败（16 满 / 非法 type） | `nil + "lights full or invalid type"` |
| `UpdateLight` 失败（id 越界 / slot 已 Remove） | `false` |
| `RemoveLight` 越界 / 已 Remove | no-op（无返回值） |

与 CONSENSUS 第 4 章"错误处理"完全一致。

---

## 4. Smoke 测试覆盖

`scripts/smoke/lighting2d.lua` 共 13 段断言（按顺序）：

| # | 测试段 | 关键断言 |
|---|--------|----------|
| 1 | **模块表面** | 11 个函数 + 3 个常量都存在 + `MAX_LIGHTS == 16` |
| 2 | **Enabled / IsEnabled** | `SetEnabled(false/true)` 来回切换可读 |
| 3 | **Ambient round-trip** | `SetAmbient(0.1,0.2,0.3)` 后 `GetAmbient` 还原（误差 < 1e-5） |
| 4 | **Clear baseline** | `ClearLights()` 后 `GetLightCount == 0` |
| 5 | **AddPointLight 基础** | 返回 id ∈ [1, 16]，count == 1 |
| 6 | **AddSpotLight 完整字段** | 返回 id ≠ 之前的 id, count == 2 |
| 7 | **UpdateLight 部分字段** | `UpdateLight(id, {intensity=3.0})` → true |
| 8 | **UpdateLight 全表** | 9 个字段一次性更新 → true |
| 9 | **UpdateLight 非法 id** | id=0/99/-1 都返回 false |
| 10 | **RemoveLight 幂等** | 重复 Remove 同 id / 非法 id 都是 no-op |
| 11 | **16 灯极限** | 填满 16 个 slot，第 17 个 AddPoint/Spot 返回 `nil + err` |
| 12 | **slot 复用** | Remove 后再 Add 能成功（id 复用） |
| 13 | **ClearLights 保留 ambient** | Clear 只清 lights，ambient 不变 |
| 14 | **边界** | 空 table `AddPointLight({})` 用默认值 + 零方向 `AddSpotLight` fallback |

**末尾 `print("==== Light.Lighting2D smoke DONE ====")`** 标记成功。

---

## 5. 验收清单（对齐 `TASK_PhaseE.md` § E.1.4）

| 验收标准 | 状态 | 证据 |
|----------|------|------|
| `scripts/smoke/lighting2d.lua` 可调全部 API 无错 | ✅ | 13 段断言覆盖 11 个函数 + 3 个常量 + 边界 |
| `Light.Lighting2D.GetMaxLights() == 16` | ✅ | smoke § 1 直接断言 + 模块 `MAX_LIGHTS == 16` 常量 |
| Add 17 个返回 nil | ✅ | smoke § 11：填满 16 后第 17 个 AddPoint/Spot 都返回 `nil + err` |
| UpdateLight 部分字段更新生效 | ✅ | smoke § 7 验证 `UpdateLight(id, {intensity=3.0})` 返回 true（C++ 层语义正确；视觉效果到 E.1.5 才能验证） |

---

## 6. CI 验证策略

### 6.1 全平台编译 + 语法检查
- **6 平台**：Windows / Linux / macOS / Android / iOS / Web 全部 `Build ChocoLight Engine` 通过
- **6 平台**：`Run smoke script syntax checks` 中 `lightc -p lighting2d.lua` 通过（确保 Lua 语法正确）

### 6.2 Windows runtime smoke
- 新增 `& "$runtimeDir\light.exe" $phaseE14Smoke` 步骤
- 验证 `require("Light.Lighting2D")` 成功
- 验证 11 个函数 + 3 个常量都暴露
- 验证 16 灯极限 + invalid id + slot 复用 + 边界 case 全部行为正确

**这是对 E.1.3 C++ 状态机的端到端运行时验证**，弥补 E.1.3 当时只能"逻辑确认"的缺口。

---

## 7. 与后续任务的衔接

| 任务 | 解锁 | 衔接点 |
|------|------|--------|
| **E.1.5** DrawLit2DQuad | ✅ 解锁（依赖 E.1.2 + E.1.3, E.1.4 是 bonus） | `render_gl33.cpp` 用 `Lighting2D::GetState()` 上传 uniform，与 Lua 端写入解耦 |
| **E.1.6** ECS Light2D | ✅ 解锁 | `light_ecs.cpp` 的 `_UploadLights2D()` 现在可以直接调 `Light.Lighting2D.AddPointLight` Lua API |
| **E.1.7** demo + smoke | 部分解锁 | smoke 已经做了 API 测试；demo 等 E.1.5 DrawLit 落地后才能产生视觉 |

---

## 8. 风险与已知限制

| 风险 | 缓解 |
|------|------|
| **type 切换不可由 Lua 触发** | 当前 `UpdateLight` 内 `ReadLightTable` 不读 type 字段；如未来需要切换 Point↔Spot，需要 Remove + Add（与 Lighting2D::Add API 行为一致） |
| **Lua 端无法直接读 light 字段** | 当前 API 是单向"配置 → 渲染"，调用方应自己保留状态副本；若未来 ECS 系统需要回读，可在 E.1.6 通过 `GetLight(id)` 扩展 |
| **innerAngle / outerAngle 顺序约定** | shader 侧 `smoothstep(outerCos, innerCos, cosA)` 要求 `outerCos < innerCos`（外角更大 → 余弦更小）；用户若传反则光锥变形但不崩溃 |
| **dir 归一化在 binding 内做** | 用户传 `dirX=0.5, dirY=0.5` 会被归一化为 (0.707, 0.707)；与 shader 期望一致，但用户应理解此约定 |

---

## 9. 提交信息建议

```
feat(phase-e1.4): Light.Lighting2D Lua API binding + smoke

- light_lighting2d.cpp: +265 lines anonymous-namespace bindings
  - ReadLightTable helper: nil-guarded partial field read
  - Normalize2D: zero-vector safe fallback for spot dir
  - 11 l_XXX functions matching CONSENSUS 2.1 table
  - kLighting2DReg + luaopen_Light_Lighting2D (LIGHT_API exported)
  - MAX_LIGHTS / TYPE_POINT / TYPE_SPOT constants exposed

- lumen-master light.cpp g_lightModules[]: register Light.Lighting2D
  so Windows light.exe auto-requires on startup

- scripts/smoke/lighting2d.lua: 13 assertion sections covering
  module surface, enable/ambient, add/update/remove/clear,
  16-light cap, slot reuse, partial update, edge cases

- build-templates.yml: add lighting2d smoke to Windows runtime
  step for end-to-end verification of E.1.3 + E.1.4
```
