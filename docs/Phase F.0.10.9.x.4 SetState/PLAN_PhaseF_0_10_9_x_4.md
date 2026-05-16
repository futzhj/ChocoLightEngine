# Phase F.0.10.9.x.4 — SetState 反向 API (PLAN)

> 6A · Align + Architect + Atomize
> 基线: F.0.10.10.1 commit `4645e20` (95 → 105 → 105 不变)
> 目标 Lua API: 105 → **110** (+5)

---

## 1. 目标

为 5 个 multi-instance renderer 各加 `SetState(table) → bool, int`,
与 F.0.10.9.x.3 `GetState()` 反向对称, 闭合 save/load profile 循环.

### 用户场景

```lua
-- 完整 round-trip
local snap = HDR.GetState()
File.WriteJson("hdr_profile.json", snap)
local snap2 = File.ReadJson("hdr_profile.json")
HDR.SetState(snap2)             -- ✨ 一键还原 (vs 13 行手动 SetExposure/SetGamma/...)

-- partial update (table 仅含部分字段)
HDR.SetState({ exposure = 2.0, gamma = 2.4 })   -- 只改这两个, 其他保持

-- profile clone-then-modify
local cid = HDR.CloneInstance(0)
HDR.SetActiveInstance(cid)
local s = HDR.GetState()
s.exposure = 2.0; s.tonemap_mode = 2
HDR.SetState(s)
```

## 2. 接口设计

### 2.1 函数签名

```lua
-- 入参: table (字段名匹配 GetState() 输出)
-- 返回: bool, int  (true=至少应用了一个字段, applied 字段数)
local ok, applied = HDR.SetState({ exposure = 2.0, gamma = 2.4 })
-- ok=true, applied=2
```

### 2.2 字段映射 (51 总计可写)

#### HDR (13 可写, 跳过 `enabled`/`supported`)

| GetState 字段 | 类型 | C++ 调用 | 备注 |
|--------------|------|---------|------|
| `exposure` | number | `SetExposure(float)` | clamp 内部处理 |
| `tonemap_mode` | int | `SetTonemapper(int)` | 直接接 int, 跳过 string 转换 |
| `gamma` | number | `SetGamma(float)` | |
| `velocity_dilation` | bool | `SetVelocityDilation(bool)` | |
| `velocity_dilation_half_res` | bool | `SetVelocityDilationHalfRes(bool)` | |
| `velocity_dilation_auto_skip` | bool | `SetVelocityDilationAutoSkip(bool)` | |
| `auto_tonemap` | bool | `SetAutoTonemap(bool)` | |
| `auto_taa` | bool | `SetAutoTAA(bool)` | |
| `auto_bloom` | bool | `SetAutoBloom(bool)` | |
| `auto_ssr` | bool | `SetAutoSSR(bool)` | |
| `auto_motion_blur` | bool | `SetAutoMotionBlur(bool)` | |
| `lut_id` | int | `SetGradingLUT(uint32_t)` | 注: 需对应 lut id 已 LoadCubeLUT |
| `lut_strength` | number | `SetGradingLUTStrength(float)` | |

> **关键决策**: `tonemap_mode` 在 SetState 中直接接 int (与 GetState 输出类型一致), 不走 SetTonemapper 的 string API. 直接调 `HDRRenderer::SetTonemapper(int)` overload.

#### TAA (14 可写)
`blend_alpha` / `neighborhood_clip` / `clip_mode` (string) / `jitter_enabled` / `sharpness` / `variance_gamma` / `half_res_history` / `sharpen_mode` (string) / `upscale_mode` (string) / `anti_flicker` / `motion_gamma` / `motion_adaptive_gamma` / `motion_sharpness` / `motion_adaptive_sharpness`

#### Bloom (5 可写)
`threshold` / `intensity` / `radius` / `levels` / `auto_enable`

#### SSR (14 可写)
`max_steps` / `step_size` / `thickness` / `max_distance` / `intensity` / `edge_fade` / `blur_enabled` / `blur_radius` / `bilateral_enabled` / `blur_depth_sigma` / `temporal_enabled` / `temporal_alpha` / `rejection_mode` / `auto_enable`

#### MotionBlur (5 可写)
`strength` / `sample_count` / `mode` / `half_res` / `auto_enable`

### 2.3 行为约定

1. **partial table 接受**: 缺失字段 → 不改; 只改提供的字段
2. **未知字段 silent skip**: `enabled`/`supported` 等 read-only 字段 silent ignore (使 round-trip 友好)
3. **类型错误 silent skip**: 如果字段类型与期望不符 (e.g. `exposure="text"`), silent skip, applied 不计数
4. **null/nil 字段**: 跳过 (与未提供等价)
5. **clamp 由 Set* 内部处理**: 用户传超范围值会被 clamp, SetState 不做额外校验

### 2.4 错误返回

```lua
-- 入参非 table
local ok, err = HDR.SetState("not a table")
-- ok=nil, err="SetState: expect table arg"
```

## 3. Architect (实现策略)

### 3.1 公共宏减少 boilerplate

```cpp
#define APPLY_NUM(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isnumber(L, -1)) { SETTER((float)lua_tonumber(L, -1)); ++applied; } \
         lua_pop(L, 1); } while(0)
#define APPLY_INT(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isnumber(L, -1)) { SETTER((int)lua_tointeger(L, -1)); ++applied; } \
         lua_pop(L, 1); } while(0)
#define APPLY_BOOL(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isboolean(L, -1)) { SETTER(lua_toboolean(L, -1) != 0); ++applied; } \
         lua_pop(L, 1); } while(0)
#define APPLY_STR(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isstring(L, -1)) { SETTER(lua_tostring(L, -1)); ++applied; } \
         lua_pop(L, 1); } while(0)
```

每个 `l_*_SetState` 大约 15~20 行 (含 5~14 个 APPLY_* 调用).

### 3.2 单 renderer 模板

```cpp
static int l_HDR_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_NUM("exposure",                     HDRRenderer::SetExposure);
    APPLY_INT("tonemap_mode",                 HDRRenderer::SetTonemapper);
    APPLY_NUM("gamma",                        HDRRenderer::SetGamma);
    APPLY_BOOL("velocity_dilation",           HDRRenderer::SetVelocityDilation);
    APPLY_BOOL("velocity_dilation_half_res",  HDRRenderer::SetVelocityDilationHalfRes);
    APPLY_BOOL("velocity_dilation_auto_skip", HDRRenderer::SetVelocityDilationAutoSkip);
    APPLY_BOOL("auto_tonemap",                HDRRenderer::SetAutoTonemap);
    APPLY_BOOL("auto_taa",                    HDRRenderer::SetAutoTAA);
    APPLY_BOOL("auto_bloom",                  HDRRenderer::SetAutoBloom);
    APPLY_BOOL("auto_ssr",                    HDRRenderer::SetAutoSSR);
    APPLY_BOOL("auto_motion_blur",            HDRRenderer::SetAutoMotionBlur);
    APPLY_INT("lut_id",                       HDRRenderer::SetGradingLUT);
    APPLY_NUM("lut_strength",                 HDRRenderer::SetGradingLUTStrength);
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}
```

### 3.3 文件位置

宏定义: `light_graphics.cpp` 在 5 个 SetState 之前 (集中位置)
SetState 实现: 紧邻各自 GetState (便于对照)
funcs 表: `{"SetState", l_*_SetState}` 紧邻 `GetState`

## 4. Atomize (子任务)

| 子任务 | 工作量 | 依赖 |
|-------|-------|------|
| **A1** APPLY_* 宏定义 + tonemap_mode int overload 检查 | 10min | 无 |
| **A2** HDR.SetState (13 字段) | 15min | A1 |
| **A3** TAA.SetState (14 字段, 含 string clip_mode/sharpen_mode/upscale_mode) | 20min | A1 |
| **A4** Bloom.SetState (5 字段) | 8min | A1 |
| **A5** SSR.SetState (14 字段) | 18min | A1 |
| **A6** MotionBlur.SetState (5 字段) | 8min | A1 |
| **A7** smoke 5 renderer × round-trip + partial + invalid | 40min | A2~A6 |
| **A8** 编译 + 8 smoke + 真 GL + commit + push + CI | 30min | A7 |
| **总计** | **2.5h** | |

## 5. 风险

1. **HDR.SetTonemapper 接 int 还需检查**: 当前 Lua wrap 是 string-only. C++ 端 `HDRRenderer::SetTonemapper(int)` 必须存在 (验过, 有这个 overload, 在 enum 范围内).
2. **TAA.SetClipMode/SetSharpenMode/SetUpscaleMode 接 string**: 已确认存在, 直接调用.
3. **Bloom.SetLevels 整数语义**: GetState 返 int, SetState 接 int, 完美对称.
4. **lut_id 必须已加载**: SetGradingLUT 内部不会校验 lut_id 是否合法; 若用户传无效 id, 后续 Tonemap 会失败. 这是已知 hazard, 不在本 phase 解决.

## 6. 验收标准

- 5 renderer × SetState 全部实现
- 5 smoke 各加 round-trip 测试 (Get → 改 N 字段 → Set → Get → 验证 N 字段恢复)
- 8 smoke 零回归
- demo_quad_split 真 GL 启动 OK
- CI 6 平台全绿

## 7. 不在本 phase 范围

- HDR LUT 校验 (lut_id 是否已加载) — 留 hazard
- 字段验证错误信息收集 (silent skip, 不报告)
- 嵌套 table 字段 (5 renderer 全 flat, 无嵌套需求)
