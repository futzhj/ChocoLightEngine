# Phase F.0.10.9.x.3 — GetState / CloneInstance PLAN

> 6A · ALIGN + ARCHITECT + ATOMIZE
> 目标: 为 5 个 multi-instance renderer (HDR/TAA/Bloom/SSR/MB) 加 GetState + CloneInstance,
>      让 multi-instance setup 由 ~30 行 → 1 行 (clone + 微调).

## 1. 对齐 (ALIGN)

### 1.1 用户场景 (痛点)

当前 multi-instance setup 模板 (split-screen 4 player demo):
```lua
-- 笨拙: 每个 instance 手动 Set 所有字段
for i = 1, 3 do
    local id = Bloom.CreateInstance()
    Bloom.SetActiveInstance(id)
    Bloom.SetThreshold(profiles[i].threshold)   -- 手动 1
    Bloom.SetIntensity(profiles[i].intensity)   -- 手动 2
    Bloom.SetRadius(profiles[i].radius)         -- 手动 3
    Bloom.SetLevels(profiles[i].levels)         -- 手动 4
    -- ... HDR/TAA/SSR/MB 同样要重复 manual set
end
```

理想:
```lua
-- 优雅: clone default + 微调
for i = 1, 3 do
    local id = Bloom.CloneInstance(0)         -- ✨ 一键复制 default
    Bloom.SetActiveInstance(id)
    Bloom.SetIntensity(profiles[i].intensity) -- 仅微调差异字段
end

-- save/load profile (debug / 编辑器):
local snap = Bloom.GetState()                 -- ✨ 全 state 快照
print("当前 intensity:", snap.intensity)
```

### 1.2 任务范围

| 工作 | 数量 |
|------|------|
| C++ namespace fn `CloneInstance(srcId)` | 5 (HDR/TAA/Bloom/SSR/MB) |
| Lua wrap `CloneInstance` + `GetState` | 5 × 2 = 10 |
| smoke 5 个加 Clone+GetState 测试段 | 5 |
| **新增 Lua API** | **+10 (95 → 105)** |

### 1.3 不在范围
- ❌ `SetState(table)` 反向 (用户可 `GetState` + 手动 `Set*` 还原, 不增加 API)
- ❌ HDR LUT id 持有 (CloneInstance 复制 lutTex id, 但不增引用计数 — 与现有 SetGradingLUT 一致)
- ❌ CloneInstance(已销毁 srcId) 自动恢复 (返 0 失败, 非业务路径)

## 2. 架构 (ARCHITECT)

### 2.1 设计原则

#### CloneInstance(srcId) → newId
- **复制范围**: srcId 全部调参字段 (intensity / threshold / mode / lutTex / lutStrength / ...)
- **复位字段**: backend 自创建的 RT (fbo / texs[]) → 0
  - 因为新 instance 还没 `Enable()`, 没有 RT 资源
  - 用户传入资源 (lutTex id) **保留** (是 state 一部分)
- **temporal state 复位**: hasPrevViewProj=false, frameCounter=0, historyIdx=0
  - 新 instance 第一帧应当从 cur 路径走, 不继承 src 的 history 时序
- **slot 状态**: 与 CreateInstance 等价 (g_count++, slot_in_use[newId]=true)
- **失败条件**:
  - srcId 非法 (< 0 / >= MAX) → 返 0
  - srcId 未分配 → 返 0
  - 槽满 → 返 0

#### GetState() → table
- **flat table**: `{exposure=1.0, gamma=2.2, ...}` (不嵌套)
- **覆盖范围**: 与每个 renderer 现有 `Get*` fn 1:1 对应
- **不含**: enabled / inited / supported / 内部 RT id / temporal 临时变量
- **数据类型**: 数字字段 → number, bool → boolean, int → integer

### 2.2 各 renderer state 字段表

| Renderer | state 字段 (CloneInstance 复制) |
|----------|-------------------------------|
| HDRRenderer | exposure, tonemap_mode, gamma, lut_tex, lut_strength, lut_hot_reload, vignette*, ... |
| TAARenderer | alpha, clip_scale, jitter_strength, half_res_history, color_clamp, ... |
| BloomRenderer | threshold, intensity, radius, requested_levels |
| SSRRenderer | max_steps, step_size, thickness, max_distance, intensity, edge_fade, blur_enabled, blur_radius, bilateral_enabled, blur_depth_sigma, temporal_enabled, temporal_alpha, rejection_mode |
| MotionBlurRenderer | strength, sample_count, mode, half_res |

实际字段以代码现有 Get* fn 列表为准.

### 2.3 改造模板 (5 renderer 共用)

```cpp
// C++ namespace (例: BloomRenderer)
int CloneInstance(int srcId) {
    // 边界检查
    if (srcId < 0 || srcId >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN, "CloneInstance: 非法 srcId=%d", srcId);
        return 0;
    }
    if (!g_slot_in_use[srcId]) {
        CC::Log(CC::LOG_WARN, "CloneInstance: srcId=%d 未分配", srcId);
        return 0;
    }

    // 找空闲槽
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 全字段复制
            g_states[i] = g_states[srcId];
            // 复位 backend 创建的 RT (新 instance 待 Enable 重建)
            for (int k = 0; k < MAX_LEVELS; ++k) {
                g_states[i].fbos[k] = 0;
                g_states[i].texs[k] = 0;
            }
            g_states[i].actualLevels = 0;
            g_states[i].enabled = false;        // 新 instance 默认未 Enable
            g_states[i].width = 0;
            g_states[i].height = 0;
            g_slot_in_use[i] = true;
            ++g_count;
            return i;
        }
    }
    return 0;   // 槽满
}
```

```c
// Lua wrap (例: Bloom)
static int l_Bloom_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, BloomRenderer::CloneInstance(srcId));
    return 1;
}

static int l_Bloom_GetState(lua_State* L) {
    lua_newtable(L);
    #define SET_NUM(key, val) do { lua_pushnumber(L, (lua_Number)(val)); lua_setfield(L, -2, key); } while(0)
    #define SET_INT(key, val) do { lua_pushinteger(L, (lua_Integer)(val)); lua_setfield(L, -2, key); } while(0)
    #define SET_BOOL(key, val) do { lua_pushboolean(L, (val) ? 1 : 0); lua_setfield(L, -2, key); } while(0)
    SET_NUM("threshold", BloomRenderer::GetThreshold());
    SET_NUM("intensity", BloomRenderer::GetIntensity());
    SET_NUM("radius",    BloomRenderer::GetRadius());
    SET_INT("levels",    BloomRenderer::GetLevels());
    SET_BOOL("enabled",  BloomRenderer::IsEnabled());
    #undef SET_NUM
    #undef SET_INT
    #undef SET_BOOL
    return 1;
}
```

## 3. 原子化 (ATOMIZE)

### 3.1 Sub-task 拆分 (3 sub-phase)

#### Sub-task A: HDR + Bloom + MB CloneInstance + GetState (~1.2h)
- 简单 renderer 先做, 验证模板
- HDR state 字段最多, 但模板成熟可批量 push field

#### Sub-task B: TAA + SSR CloneInstance + GetState (~1.0h)
- TAA / SSR 状态稍多, 但 SSR 最复杂 (13 字段)
- temporal state 复位特殊处理

#### Sub-task C: smoke 5 个 + FINAL/TODO (~0.8h)
- 5 smoke 各加 Clone (基础 + 槽满 + 非法 srcId) + GetState (字段一致性) 测试段
- 跑 8 smoke 零回归 + 真 GL 启动验证
- FINAL + TODO + 1 commit + CI

总计 ~3h, 1 commit.

### 3.2 风险

| 风险 | 缓解 |
|------|------|
| 字段命名不一致 (snake_case vs camelCase) | 统一用 snake_case (Lua 习惯) |
| GetState 字段遗漏 | 与每个 renderer 的 Get* fn 1:1 检查 |
| CloneInstance 漏复位 RT | 模板列出每 renderer RT 字段; smoke 测 enabled=false |
| temporal state 不复位导致首帧异常 | 复位 hasPrevViewProj/frameCounter/historyIdx |

## 4. 提交策略

- **1 commit** (减少 CI 次数), 内部 3 sub-task 顺序完成
- 文件变更预计:
  - 5 header (各 +10 行) = +50
  - 5 cpp (各 +30 行 CloneInstance 实现) = +150
  - light_graphics.cpp 10 wrap fn = +200
  - 5 smoke 加段 = +250
  - docs PLAN/FINAL/TODO
- 总: ~14 files, +700/-0 LOC
- CI 风险: 低 (10 fn 全是新增, 无现有逻辑改动)
