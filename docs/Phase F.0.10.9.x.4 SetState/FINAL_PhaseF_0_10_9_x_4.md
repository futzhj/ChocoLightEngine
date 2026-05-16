# Phase F.0.10.9.x.4 — SetState 反向 API FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (5 renderer × 1 fn = +5 Lua API, **105 → 110**)

---

## 1. 完成工作

为 5 个 multi-instance renderer (HDR / TAA / Bloom / SSR / MotionBlur)
各加 `SetState(table) → bool, int`, 与 F.0.10.9.x.3 `GetState()` 反向对称.

| Renderer | 可写字段数 | 注意点 |
|----------|----------|--------|
| HDR | 13 | `tonemap_mode` 接 int (与 GetState 对称); `lut_id` + `lut_strength` 合并为 `SetGradingLUT(id, strength)` 单调用 |
| TAA | 14 | `clip_mode` / `sharpen_mode` / `upscale_mode` 接 string |
| Bloom | 5 | `radius` clamp [0,1] (Set 内部) |
| SSR | 14 | 全部数值字段, 无 string |
| MotionBlur | 5 | `mode` 接 int (0=combined / 1=camera / 2=object) |
| **总计** | **51** | |

## 2. API 设计

### 2.1 接口签名

```lua
local ok, applied = renderer.SetState(table)
-- ok=bool      (true 当 applied >= 1)
-- applied=int  (实际应用字段数)

-- 入参非 table → nil + err string
local r, err = renderer.SetState("not table")  -- r=nil, err="SetState: expect table arg"
```

### 2.2 5 行为约定

1. **partial 接受**: 缺失字段不改, 只改提供字段
2. **未知字段 silent skip**: `enabled`/`supported` 等 read-only 字段 silent ignore
3. **类型错误 silent skip**: 字段类型不符 silent skip, 不计入 applied
4. **空 table**: applied=0 → ok=false (无字段应用)
5. **clamp**: 由各 Setter 内部处理, SetState 不二次校验

## 3. 实现

### 3.1 公共 APPLY_* 宏

```cpp
#define APPLY_NUM(K, SETTER)  /* 类型 number */
#define APPLY_INT(K, SETTER)  /* 类型 number → int cast */
#define APPLY_BOOL(K, SETTER) /* 类型 boolean */
#define APPLY_STR(K, SETTER)  /* 类型 string */
```

每宏: `lua_getfield + 类型检查 + 转换 + Set 调用 + ++applied + lua_pop`. 5 行展开.

### 3.2 单 SetState 模板

```cpp
static int l_HDR_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_NUM("exposure",     HDRRenderer::SetExposure);
    APPLY_INT("tonemap_mode", HDRRenderer::SetTonemapper);
    // ...
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}
```

### 3.3 HDR lut 字段特殊处理

GetState 拆 `lut_id` + `lut_strength` 2 字段, 但 C++ 端是合并 `SetGradingLUT(id, strength)` 单函数.
SetState 在解析完 11 个普通字段后, 单独读 lut 双字段并合并调用:

```cpp
// 任一字段提供即触发, 缺省 fallback 当前值
bool hasId = false, hasStrength = false;
uint32_t newId = HDRRenderer::GetGradingLUTId();
float newStrength = HDRRenderer::GetGradingLUTStrength();
// ... lua_getfield + check ...
if (hasId || hasStrength) {
    HDRRenderer::SetGradingLUT(newId, newStrength);
    applied += (hasId ? 1 : 0) + (hasStrength ? 1 : 0);
}
```

## 4. 用户用例

### 4.1 save/load profile (主用途)

```lua
-- save
local snap = HDR.GetState()
File.WriteJson("profile.json", snap)

-- load (一行)
local snap = File.ReadJson("profile.json")
HDR.SetState(snap)             -- ✨ 自动还原 13 字段, vs 旧 13 行 SetExposure/SetGamma/...
```

### 4.2 partial update

```lua
-- 仅改 2 字段, 其他保持
HDR.SetState({ exposure = 2.0, tonemap_mode = 2 })
```

### 4.3 clone-then-modify (与 CloneInstance 配合)

```lua
local cid = HDR.CloneInstance(0)
HDR.SetActiveInstance(cid)
local s = HDR.GetState()
s.exposure = 2.0; s.lut_id = warm_lut
HDR.SetState(s)               -- 只 1 行修改 instance state
```

### 4.4 GetState → SetState round-trip 一致性

smoke 28.6 已验证: `SetState(GetState())` 是幂等的 (含 read-only 字段 silent skip 不影响).

## 5. 验证

### 5.1 smoke 测试覆盖

每 renderer 加 3~6 段 SetState 测试:

| 测试段 | 覆盖内容 |
|-------|---------|
| 28.1 / 9.18.1 / SS.1 | round-trip: 改 N 字段 → SetState → GetState 验证 |
| 28.2 / SS.2 | partial: 仅 1 字段, 其他保持 |
| 28.3 / 9.18.2 / SS.2 | 类型错误 silent skip, 仅匹配类型字段 applied |
| 28.4 / 9.18.3 / SS.3 | 入参非 table → nil + err string |
| 28.5 (HDR only) | enabled/supported read-only 字段 silent skip |
| 28.6 (HDR only) | 完整 round-trip 一致性 (GetState 输出可被 SetState 完全消费) |

### 5.2 8 smoke 全 PASS (零回归)

```
PASS hdr  (含 28.1~6 SetState)
PASS bloom (含 SS.1~3)
PASS ssr  (含 SS.1~3)
PASS auto_exposure
PASS lens_fx
PASS motion_blur (含 SS.1~3)
PASS taa  (含 9.18.1~3)
PASS lighting2d
```

### 5.3 真 GL 启动验证

`light.exe samples/demo_quad_split/main.lua` 启动 OK, 零回归.

## 6. 文件变更

| 文件 | 行数 |
|------|------|
| `ChocoLight/src/light_graphics.cpp` | +110 (5 SetState fn + APPLY_* 宏 + 5 funcs 表 entry) |
| `scripts/smoke/hdr.lua` | +75 (28.1~6 + fn_names) |
| `scripts/smoke/taa.lua` | +40 (9.18.1~3 + fn_names) |
| `scripts/smoke/bloom.lua` | +35 (SS.1~3 + fn_names) |
| `scripts/smoke/ssr.lua` | +40 (SS.1~3 + fn_names) |
| `scripts/smoke/motion_blur.lua` | +35 (SS.1~3 + fn_names) |
| `docs/Phase F.0.10.9.x.4 SetState/{PLAN,FINAL,TODO}.md` | 新建 |
| **总计** | **9 files, +400/-0** |

## 7. 关键决策回顾

### 7.1 tonemap_mode 接 int (vs string)

GetState 输出 `tonemap_mode` 是 int (0..3, 与 `GetTonemapper()` int 一致), 因此 SetState 也接 int.
用户想用 string 仍可走 `HDR.SetTonemapper("aces")`. 不对称设计因为已有 SetTonemapper(string), 但 SetState 内部走 int 路径更简洁.

### 7.2 lut_id + lut_strength 合并 vs 分别调用

C++ 端 `SetGradingLUT(id, strength)` 是合并函数, 没有单独的 SetGradingLUTId / SetGradingLUTStrength.
SetState 内部读双字段合并 1 次调用, 而非加新 wrapper. 用户视角无感 (table 字段仍分开).

### 7.3 silent skip vs 抛 error

类型错误 silent skip 是为了 partial table 友好性 + GetState→SetState round-trip 兼容性
(`enabled`/`supported` 等 read-only 字段不会让 SetState 抛错). 代价: 用户 typo 字段名时
不会得到提示. 折中可加 `applied` 计数比对.

### 7.4 mode 接 int (MotionBlur)

GetState 返 `mode` 是 int (0=combined / 1=camera / 2=object), SetState 接 int 与之对称.
未提供 string 别名 (没有 `MB.SetMode("camera")` 这种 API), 保持简洁.

## 8. F.0.10.x 累计

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0.10.0 ~ F.0.10.9.x.3 | multi-instance + LUT + Clone + GetState | 105 |
| F.0.10.10.1 | demo refactor + TAA GetState 字段补全 | +0 (105) |
| **F.0.10.9.x.4 (本)** | **5 renderer × SetState** | **+5 (110)** |
| **总计** | **完整 multi-instance + Clone + Get/SetState 闭环** | **110 fn** |

至此, 5 个 multi-instance renderer 完整闭环:
- 创建/销毁: `CreateInstance` / `DestroyInstance`
- 切换: `SetActiveInstance` / `GetActiveInstance` / `GetInstanceCount`
- 复制: `CloneInstance(srcId)`
- 读取: `GetState()`
- 写入: `SetState(table)`  ← **本 phase**

## 9. 下一步

- **F.0.11 demo 截图/录屏** (~3h, 高优, 可视化验收 multi-instance + state 闭环)
- F.0.10.10.2 demo Bloom/SSR/MB multi-instance setup (~2h, 替换 `apply_postfx_profile` 切换)
- F.1 TAAU DLSS-like (~10-15h)
