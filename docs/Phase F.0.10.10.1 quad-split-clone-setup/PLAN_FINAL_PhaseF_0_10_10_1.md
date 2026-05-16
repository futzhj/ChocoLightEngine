# Phase F.0.10.10.1 — Quad-Split Demo with CloneInstance Setup

> 6A 简化版 (单 doc, demo refactor + 小修)
> 状态: ✅ 完成
> 基线: F.0.10.9.x.3 commit `3220ade`

---

## 1. 目标 (Align + Architect)

利用 F.0.10.9.x.3 新加的 `CloneInstance(srcId)` + `GetState()` API
重构 `samples/demo_quad_split/main.lua`, 演示新 API 的实际收益:
- `CloneInstance(0) + override` 替代 `CreateInstance + 全量 Set*`
- `GetState()` 在 setup 完成后打印 4 instance 关键字段, 直观确认 per-quad profile 隔离

附带顺手修两个小问题:
1. TAA `GetState()` 字段缺 `clip_mode` / `anti_flicker` / `motion_*`, demo 打不出来
2. `Bloom.SetRadius(>1.0)` silent clamp 到 1.0 (Bloom radius 实际 clamp [0, 1])

## 2. 实施 (Atomize + Automate)

### 2.1 demo HDR 4 instance setup (Clone 模式)

```lua
-- 旧 (F.0.10.10): 4 × {CreateInstance + SetActiveInstance + Enable + SetAutoTonemap(false)} ≈ 16 行
-- 新 (本 phase): 1 × full setup default + 3 × {CloneInstance(0) + Enable} ≈ 6 行

if not HDR.Enable(HALF_W, HALF_H) then ... end
HDR.SetAutoTonemap(false)   -- ← 这一行同时定义后续 clone 的默认 auto_tonemap=false

for i = 1, 3 do
    local id = HDR.CloneInstance(0)        -- ✨ 复制 default 全部字段 (含 autoTonemap=false)
    g_hdr_ids[i] = id
    HDR.SetActiveInstance(id)
    HDR.Enable(HALF_W, HALF_H)             -- Clone 重置 RT 句柄, 必须 Enable 重建
end
```

### 2.2 demo TAA 4 instance setup (Clone + override 差异字段)

TL base 设完整 profile, TR/BL/BR 用 Clone 后只 override 差异字段:

```lua
setup_taa_base_profile()                   -- TL: ycocg + rcas + sharp=1.2 + antiFlicker

for i = 1, 3 do
    local id = TAA.CloneInstance(0)         -- ✨ 复制 TL 全部字段
    TAA.SetActiveInstance(id)
    TAA.Enable(HALF_W, HALF_H)              -- 重建 RT
    override_taa_diff(i)                    -- 仅 3~6 字段差异
end
```

差异统计 (vs TL base):
| Quad | override 字段数 | 备注 |
|------|----------------|------|
| TR (1) | 6 | clipMode/varianceGamma/halfRes/upscaleMode/sharpness/sharpenMode |
| BL (2) | 4 | clipMode/sharpness/sharpenMode/antiFlicker |
| BR (3) | 3 | clipMode/varianceGamma/sharpness (sharpenMode='rcas' 继承 base) |

### 2.3 GetState() snapshot 打印

OnOpen 末尾遍历 4 instance, 打印关键字段:

```lua
for i = 0, 3 do
    HDR.SetActiveInstance(g_hdr_ids[i])
    TAA.SetActiveInstance(g_taa_ids[i])
    local hs, ts = HDR.GetState(), TAA.GetState()
    print("Q" .. i, hs.enabled, hs.auto_tonemap,
          ts.clip_mode, ts.sharpen_mode, ts.sharpness,
          ts.anti_flicker, ts.half_res_history, ts.upscale_mode)
end
```

实测输出:
```
Q0 enabled=true auto_tonemap=false | clip=ycocg    sharpen=rcas    sharp=1.20 antiflicker=true  halfRes=false upscale=bilinear
Q1 enabled=true auto_tonemap=false | clip=variance sharpen=unsharp sharp=0.00 antiflicker=true  halfRes=true  upscale=lanczos
Q2 enabled=true auto_tonemap=false | clip=rgb      sharpen=unsharp sharp=0.50 antiflicker=false halfRes=false upscale=bilinear
Q3 enabled=true auto_tonemap=false | clip=variance sharpen=rcas    sharp=1.50 antiflicker=true  halfRes=false upscale=bilinear
```

每个 quad 配置确实独立 ✓

### 2.4 TAA GetState 字段补全

`l_TAA_GetState` 在 F.0.10.9.x.3 漏掉 6 字段, 本 phase 补全:

| 字段 | 类型 | 来源 |
|------|------|------|
| `clip_mode` | string | F.0.2/F.0.3 (rgb/ycocg/variance) |
| `anti_flicker` | bool | F.0.4 |
| `motion_gamma` | number | F.0.8 |
| `motion_adaptive_gamma` | bool | F.0.8 |
| `motion_sharpness` | number | F.0.13 |
| `motion_adaptive_sharpness` | bool | F.0.13 |

TAA `GetState()` 字段数 10 → **16**.

### 2.5 Bloom radius silent clamp 修

```lua
-- 旧 (silent clamp 到 1.0)
Bloom.SetRadius(1.5)
-- 新
Bloom.SetRadius(1.0)   -- max
Bloom.SetRadius(0.9)   -- 显式 < 1.0
```

## 3. 验证 (Assess)

### 3.1 8 smoke 全 PASS (零回归)

```
PASS hdr (含 27.* GetState/Clone)
PASS bloom
PASS ssr
PASS auto_exposure
PASS lens_fx
PASS motion_blur
PASS taa (含 9.17.1 GetState 16 字段)
PASS lighting2d
```

### 3.2 真 GL 启动验证

`light.exe samples/demo_quad_split/main.lua` 启动 OK:
- 4 HDR instance via Clone: ids=[0, 1, 2, 3]
- 4 TAA instance via Clone: ids=[0, 1, 2, 3]
- GetState snapshot 4 行配置全部独立 ✓

## 4. 文件变更

| 文件 | 行数变更 |
|------|---------|
| `samples/demo_quad_split/main.lua` | +60/-30 (HDR/TAA setup 重构 + GetState 打印 + banner 升级) |
| `ChocoLight/src/light_graphics.cpp` | +6/-0 (TAA GetState 补 6 字段) |
| `scripts/smoke/taa.lua` | +5/-2 (9.17.1 字段列表更新) |
| `docs/Phase F.0.10.10.1 quad-split-clone-setup/PLAN_FINAL_*.md` | 新建 |
| **总计** | **4 files, +71/-32** |

## 5. 关键收益

1. **demo 代码可读性** ↑: HDR/TAA setup 模式从"全量 Set*"变为"clone + override 差异", 更接近用户实际生产代码
2. **新 API 正向反馈**: GetState print 4 行配置直接证明 Clone(0) + override 工作正确, 后续用户参考 demo 时不会怀疑
3. **TAA GetState 完整性补全**: 16 字段覆盖 TAA 全部 user-settable 状态 (vs 旧 10 字段)
4. **零回归**: 8 smoke + demo headless API probe + 真 GL 启动全 PASS

## 6. F.0.10.x 累计

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0.10.0 ~ F.0.10.9.x.3 | 多 instance + LUT 子生态 + Clone/GetState | 105 |
| **F.0.10.10.1** | **demo refactor + TAA GetState 字段补全** | **+0 (105)** |

## 7. 后续候选

- F.0.11 demo 截图 / 录屏 (~3h, **高优**: 配合本 demo 视觉验收)
- F.0.10.9.x.4 SetState (反向 API, ~1h, 中优, save/load profile 闭环)
- F.0.10.10.2 Bloom/SSR/MB multi-instance setup in demo (~2h, 低优, 替换 `apply_postfx_profile` 全局切换)
- F.1 TAAU DLSS-like (~10-15h, 大版本)
