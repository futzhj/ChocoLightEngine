# Phase G.1.7 — Lua API 容错 audit (ALIGNMENT)

> **创建日期**: 2026-05-19
> **状态**: ✅ 决策完成 (方案 C — 全面重构)
> **关联 HANDOFF**: §3 选项 A.4 "Lua API 容错 audit"
> **前置依赖**: 无
> **预估工时**: 30-40h+, 拆为 5-6 个子阶段多会话推进

---

## 一. 项目上下文调研结果

### 1.1 binding 文件总规模

```
ChocoLight/src/light_*.cpp = 79 个文件
其中:
  - 总 luaopen_ 入口: ~74 个 (个别文件如 light_network 是父表注册)
  - 总 Lua API 数量估算: 600-800 个 (按平均 8-10 API/文件)
```

**结论**: 全 79 文件 audit 远超 4-6h 估时, **必须聚焦**。

### 1.2 现有错参处理风格 (实证抽样 3 文件)

| 文件 | 风格 | 评价 |
|------|------|------|
| `light_filesystem.cpp` | 全 `luaL_checkstring/checkinteger/optinteger` + nil+err 返回 | ✅ 标杆 |
| `light_animation.cpp` | `luaL_checkudata` + double-deref check + magic 模式 | ✅ 标杆 (强类型 userdata) |
| `light_graphics.cpp` | 大多用 `luaL_check`, 但 5 处用 `lua_isuserdata + lua_touserdata` (无 metatable 校验) | ⚠️ 混合 |
| `light_asset.cpp` (G.1.6) | luaL_newmetatable + luaL_testudata + lua_type 严格 | ✅ 新标杆 |

### 1.3 crash-prone pattern 分布 (grep 实测)

| Pattern | 命中数 | 文件数 | 风险 |
|---------|--------|-------|------|
| `lua_touserdata` (含安全和不安全) | 74 | 29 | 中 |
| `lua_isuserdata` (无 metatable 校验的 fast path) | **26** | **13** | **高** — type confusion |
| `lua_tostring` (无 isstring 前置校验) | 100 | 26 | 中 — nullptr 解引用 |
| `lua_isuserdata` + `lua_touserdata` + 直接 cast | **20+** | **10+** | **高** — 已识别 crash 路径 |

---

## 二. 关键发现 — Lumen OOP 框架的设计约束

### 2.1 `__instance` userdata pattern (10+ 文件)

```cpp
// 现行 OOP 模式 (light_db.cpp / light_av.cpp / light_network*.cpp / etc)
static SQLiteContext* GetSQLiteCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    SQLiteContext* ctx = (SQLiteContext*)lua_touserdata(L, -1);  // ⚠️ 无 type tag 校验
    lua_pop(L, 1);
    return ctx;
}
```

```lua
-- Lua 侧: 用户 OOP table
local db = setmetatable({__instance = userdata}, DB_MT)
db:Query("SELECT ...")  -- C++: getfield "__instance" → cast to SQLiteContext*
```

**这是 lumen Light.NewClass 的标准模式**. raw userdata 不挂 metatable, 类型信息靠外层 OOP table 维护. 设计上**有意为之**, 但存在 type-confusion 攻击面:

```lua
-- Type-confusion 示例 (导致 crash)
local img = Light.Graphics.NewImage("a.png")           -- ImageContext userdata
local fakeDb = setmetatable({__instance = img.__instance}, getmetatable(realDb))
fakeDb:Query("...")                                     -- crash: cast 到 SQLiteContext*
```

### 2.2 强类型 metatable pattern (新代码)

```cpp
// light_asset.cpp (G.1.6) / light_animation.cpp pattern
static AssetBatchUd* CheckBatchUd(lua_State* L, int idx) {
    void* p = luaL_checkudata(L, idx, "Light.AssetLoader._BatchHandle");
    return static_cast<AssetBatchUd*>(p);
}
```

**优势**: Lua 5.1 标准类型校验, 类型安全 by design.
**限制**: 不兼容 OOP `__instance` 模式, 重构成本高.

---

## 三. A.4 任务范围歧义 — 需用户决策

### 3.1 任务定义模糊

HANDOFF 原文:
> **Lua API 容错 audit**: 错误传参不应 crash, 全 API audit + nil+err 返回 (估时 4-6h)

歧义点:
- "全 API audit" 是字面意义还是高频 API 子集?
- "不应 crash" 是 type-confusion 也修, 还是仅 nullptr 解引用?
- "nil+err 返回" 是新写法的统一规范, 还是改造现存 luaL_error 风格?

### 3.2 三种范围方案

| 方案 | 描述 | 估时 | 收益 |
|------|------|------|------|
| **A. 焦点修复** | 修 26 处 `lua_isuserdata` 的 type-confusion + 关键 lua_tostring nullptr 风险 | 4-6h | 消除 5-10 个真实 crash 路径 |
| **B. 标杆推广** | 抽取 helpers (lt_check_instance_with_magic) + 改造 5-10 高频文件 | 8-12h | 统一规范, 中长期收益 |
| **C. 全面重构** | 79 文件全 audit + OOP magic header 强制 + nil+err 统一规范 | 30-40h+ | 完全消除 crash 面 |

### 3.3 我的实证推荐: **方案 A (焦点修复)**

**理由**:
1. **HANDOFF 标注 4-6h** → 方案 B/C 严重超时
2. **实际 crash 数量有限** (10-30 处), 全 audit ROI 低
3. **OOP 框架已工作多年**, 全局重构有回归风险
4. **Phase G.1.6 已建立强类型 metatable 标杆**, 后续新代码自然采用即可
5. **验收门槛清晰**: 添加 fuzz smoke 用 type-confusion 攻击, 全 PASS = 完成

### 3.4 方案 A 具体范围

**3.4.1 必修 (P0)** — 已 grep 定位
- 26 处 `lua_isuserdata + lua_touserdata` 加 magic header 验证
- light_graphics.cpp:286 / 571 / 1061 / 1512 等 inline cast 抽提为 helper

**3.4.2 加固 (P1)** — instance ctx struct 改造
- 选 5-10 个高频 ctx 结构体, 首字段加 `uint32_t magic` 常量
- helper `LT::CheckInstance<T>(L, idx, T_MAGIC)` 验证 magic
- 影响文件: light_graphics(canvas/image/font), light_db, light_av(video/avctx), light_network, light_data

**3.4.3 smoke (P0)**
- `scripts/smoke/lua_api_robustness.lua`: 30+ fuzz 用例
  - nil 参数 → luaL_argerror (不 crash)
  - 错类型 (number 当 string) → luaL_argerror
  - 错 userdata (Image 当 Canvas) → magic 检查 → luaL_error
  - 错 OOP table (fake `__instance`) → magic 检查 → luaL_error
  - 越界 index → 范围检查
  - 缺失字段 → luaL_optX 默认值 / luaL_error

---

## 四. 决策点 (D1-D6) — 待用户拍板

### D1. 范围方案选择

- **D1.A**: 焦点修复 (4-6h) — **我推荐**
- D1.B: 标杆推广 (8-12h)
- D1.C: 全面重构 (30-40h+)

### D2. magic header 引入策略

- **D2.A**: 给关键 ctx struct 加 magic, 首字段 `static constexpr uint32_t MAGIC = 'XXXX'` — **我推荐**
- D2.B: 不加 magic, 仅做 nullptr 检查 (保留 type-confusion 风险)
- D2.C: 改用 luaL_newmetatable 重构 OOP (重大架构变更)

### D3. nil+err 返回 vs luaL_error 抛错

现有混合:
- `light_filesystem.cpp` 用 `lua_pushboolean(0); lua_pushstring(err); return 2;`
- `light_graphics.cpp` 大多 `luaL_error` (longjmp)

- **D3.A**: 不强制统一, 保留各文件现风格 — **我推荐** (避免回归)
- D3.B: 全部改 nil+err 风格 (大量改动)
- D3.C: 全部改 luaL_error 风格 (大量改动)

### D4. lua_tostring 不安全 pattern 处理

- **D4.A**: 仅修复**真实 nullptr 解引用**点 (e.g. 直接 strcpy/strstr 无前置 isstring) — **我推荐**
- D4.B: 全 100 处加 isstring 前置检查 (大量改动, 大多已有上下文保护)

### D5. smoke 覆盖深度

- **D5.A**: 30 用例覆盖 5-10 个高频 module (Graphics / Image / Canvas / Camera / Asset / Audio / Font / Animation / Physics / DB) — **我推荐**
- D5.B: 100 用例覆盖全 79 模块
- D5.C: 仅 10 用例 sanity test

### D6. 引入新 helpers 是否扩 light.h public API

- **D6.A**: helpers 内置在新 `light_lua_helpers.h` 私有头, 不导出 — **我推荐**
- D6.B: 加入 light.h LT 命名空间公共 API (用户也能用)

---

## 五. 时间预算 (基于方案 A)

| 阶段 | 预估 | 累计 |
|------|------|------|
| 1. ALIGNMENT (本文) | 0.5h | 0.5h |
| 2. DESIGN (helpers + magic 表) | 0.5h | 1h |
| 3. TASK 拆 (分批 + 优先级) | 0.5h | 1.5h |
| 4. helpers 抽取 + magic 引入 5-10 文件 | 2h | 3.5h |
| 5. light_graphics.cpp inline cast 修复 | 0.5h | 4h |
| 6. smoke 30 用例 + CI 接入 | 1h | 5h |
| 7. FINAL + ACCEPTANCE + TODO + HANDOFF + commit | 0.5h | 5.5h |

**预估总耗时**: 5.5h, 与 HANDOFF 4-6h 估时吻合 (取上界).

---

## 六. 风险提示

| 风险 | 级别 | 缓解 |
|------|------|------|
| magic header 加字段破坏 ABI | 🟡 中 | 仅给 ctx struct 加, 不动 public API; 加在 struct 末尾或前置都需要重测 |
| smoke 用例触发现存 bug | 🟢 低 | 是预期收益, 修了再 PASS 就好 |
| OOP 框架迁移阻力 | 🔴 高 | 方案 A 不动 OOP 框架, 仅加 magic 是 opt-in |
| CI 平台间 magic 字节序 | 🟡 中 | 用 `'X'<<24 | ...` 编译期常量, 跨平台一致 |

---

## 七. 用户决策结果 (2026-05-19)

**D1**: ✅ **方案 C — 全面重构** (用户选定)
- 79 文件全 audit
- OOP magic header 强制全应用
- nil+err 返回风格统一规范化
- 完全消除 crash 面

**D2-D6 默认值** (与方案 C 兼容):
- D2: magic header 强制全应用 (D2.A 升级版)
- D3: 引入返回风格规范, 但允许不同 API 选择 (返 boolean / nil+err / luaL_error 三套, 由 helpers 决定)
- D4: lua_tostring 全 100 处加 isstring 前置检查 (升级 D4.B)
- D5: 100 用例覆盖全 79 module (升级 D5.B)
- D6: helpers 内置在 light_lua_helpers.h, 私有头不导出 (D6.A)

---

## 八. 方案 C 多会话执行规划

### 8.1 子阶段拆分 (跨多会话)

| 子阶段 | 范围 | 估时 | 主交付物 |
|--------|------|------|---------|
| **G.1.7.0** (本期) | helpers 头 + 第一批高风险 ~10 文件 + smoke 框架 | 6-8h | 基础设施 + 标杆 + smoke 模板 |
| G.1.7.1 | 第二批 ~16 文件 (Graphics 子系统) | 6-8h | Graphics 全 audit |
| G.1.7.2 | 第三批 ~16 文件 (Audio + Network 子系统) | 6-8h | Audio/Net 全 audit |
| G.1.7.3 | 第四批 ~16 文件 (Physics + Animation + ECS) | 6-8h | Physics/Anim 全 audit |
| G.1.7.4 | 第五批 ~21 文件 (剩余系统类) | 6-8h | 全部 binding 完成 |
| G.1.7.5 | 全 fuzz smoke + benchmark + 文档收尾 | 4-5h | 完结 |

**总估时**: 36-46h, 与用户预期 30-40h+ 区间一致.

### 8.2 第一批文件优先级 (G.1.7.0 范围)

按真实 crash 风险 + 高频使用排序:

| 文件 | crash 路径 | 优先级 |
|------|-----------|--------|
| `light_graphics.cpp` | 5 处 inline cast type-confusion | 🔴 P0 |
| `light_graphics_canvas.cpp` | __instance pattern, 高频 SetCanvas/Pop | 🔴 P0 |
| `light_graphics_image.cpp` | __instance + 4 lua_touserdata | 🔴 P0 |
| `light_graphics_font.cpp` | font ctx 不验证 | 🔴 P0 |
| `light_db.cpp` | SQLite ctx, 用户高频 query | 🟠 P1 |
| `light_av.cpp` | AVCtx + VideoWrapper 双 instance | 🟠 P1 |
| `light_data.cpp` | Buffer mutation, 高风险 | 🟠 P1 |
| `light_network.cpp` | HttpContext 高频 | 🟠 P1 |
| `light_physics.cpp` | World/Body/Shape/Fixture 5 helpers | 🟠 P1 |
| `light_particles.cpp` / `light_tilemap.cpp` | 简单 ctx, 验证 magic 模式 | 🟡 P2 |

### 8.3 进度跟踪机制

**文件**: `docs/Phase G.1.7 Lua API Robustness Audit/progress.txt`

每子阶段更新:
- 完成的文件清单
- 引入的 magic constant
- smoke 用例累计数
- 已发现/已修的真实 crash 数

### 8.4 commit 策略

每子阶段一个 commit (与 G.1.0~G.1.6 一致), 每 commit 含:
- 该批文件改造
- 该批配套 smoke 用例
- progress.txt 更新
- 子阶段对应 ALIGNMENT 增量 (如有新决策点)

---

## 九. 进入下一阶段

**当前会话目标**: 完成 G.1.7.0 子阶段
- DESIGN_PhaseG_1_7.md (helpers + magic 表 + 整体架构)
- TASK_PhaseG_1_7.md (G.1.7.0~G.1.7.5 全部子任务清单)
- helpers 头实现 (`light_lua_helpers.h`)
- 第一批 P0 文件改造 (4 文件)
- smoke 框架 + 第一批 ~30 用例
- progress.txt 初始化
- commit + push + CI 验证

**后续会话**: 用户输入 "继续 G.1.7.1" / "继续 G.1.7.2" / etc 推进各批次.
