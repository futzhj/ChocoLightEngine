# Phase F.0.10.10.2 — demo_quad_split Bloom/SSR/MB multi-instance refactor FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (demo 重构, 5 renderer 全部 multi-instance, 零回归)
> 基线: F.0.11 commit `5926b7d`

---

## 1. 目标

完成 `demo_quad_split` 5 renderer 全部 multi-instance 化:
- **F.0.10.10.1**: HDR + TAA 多 instance via Clone (已完成)
- **F.0.10.10.2 (本)**: Bloom + SSR + MotionBlur 多 instance via Clone + SetState (新)

最终: 5 个 multi-instance renderer 全部走统一 Clone + Get/SetState 范式.

## 2. 变化

### 2.1 Before (F.0.10.10.1)

```lua
local function apply_postfx_profile(i)
    if Bloom.IsEnabled() then
        if     i == 0 then Bloom.SetIntensity(1.5); Bloom.SetThreshold(0.8); Bloom.SetRadius(1.0)
        elseif i == 1 then Bloom.SetIntensity(0.4); Bloom.SetThreshold(1.5); Bloom.SetRadius(0.8)
        ...   -- 4 quad × 3 字段 = 12 行
    end
    if SSR.IsEnabled() then
        if     i == 0 then SSR.SetIntensity(0.4); SSR.SetTemporalEnabled(false)
        ...   -- 4 quad × 2 字段 = 8 行
    end
    if MB.IsEnabled() then ... end  -- 4 quad × 2 字段 = 8 行
end
-- 每帧 4 次调用 × 总 ~16 次 SetXxx + IsEnabled check = 大量重复 GL state 切换
```

问题:
1. **状态污染风险**: Bloom/SSR/MB 是全局单例, 每帧切换状态; 任何遗漏 SetXxx 会让上一帧 quad 的 state 泄露
2. **每帧切换开销**: 16+ 次 SetXxx 调用
3. **与 HDR/TAA 不统一**: 同 demo 内 HDR/TAA 已用 multi-instance + SetActiveInstance, Bloom/SSR/MB 却走老式手动切换, 学习曲线断裂
4. **缺 demo 验证**: F.0.10.9.x.2 加的 multi-instance + F.0.10.9.x.4 加的 SetState 在 demo 里没充分使用, 视觉验收弱

### 2.2 After (F.0.10.10.2)

#### Profile 表 (一次性定义)

```lua
local BLOOM_PROFILES = {
    [0] = { intensity = 1.5, threshold = 0.8, radius = 1.0 },
    [1] = { intensity = 0.4, threshold = 1.5, radius = 0.8 },
    [2] = { intensity = 0.8, threshold = 1.0, radius = 0.9 },
    [3] = { intensity = 1.2, threshold = 0.9, radius = 1.0 },
}
local SSR_PROFILES = { ... }
local MB_PROFILES  = { ... }
```

#### OnOpen 阶段 setup (一次性)

```lua
-- helper: 给单 renderer 创建 4 instance, 写入 profiles 表 (per-i field map)
local function setup_4_instance(RND, name, w, h, profiles, id_table)
    RND.SetActiveInstance(0); RND.Enable(w, h); RND.SetState(profiles[0])
    id_table[0] = 0
    for i = 1, 3 do
        local id = RND.CloneInstance(0)
        id_table[i] = id
        RND.SetActiveInstance(id); RND.Enable(w, h); RND.SetState(profiles[i])
    end
    RND.SetActiveInstance(0)
end

setup_4_instance(Bloom, 'Bloom', HALF_W, HALF_H, BLOOM_PROFILES, g_bloom_ids)
setup_4_instance(SSR,   'SSR',   HALF_W, HALF_H, SSR_PROFILES,   g_ssr_ids)
setup_4_instance(MB,    'MB',    HALF_W, HALF_H, MB_PROFILES,    g_mb_ids)
```

#### Per-frame 切换 (apply_postfx_profile)

```lua
-- 简化为 3 次 SetActiveInstance 调用
local function apply_postfx_profile(i)
    if Bloom.IsEnabled() then Bloom.SetActiveInstance(g_bloom_ids[i]) end
    if SSR.IsEnabled()   then SSR.SetActiveInstance(g_ssr_ids[i])     end
    if MB.IsEnabled()    then MB.SetActiveInstance(g_mb_ids[i])       end
end
```

#### Cleanup (对称 teardown)

```lua
local function teardown_4_instance(RND, name, id_table)
    for i = 3, 1, -1 do
        if id_table[i] > 0 then
            RND.SetActiveInstance(id_table[i])
            RND.Disable()
            RND.DestroyInstance(id_table[i])
        end
    end
    RND.SetActiveInstance(0); RND.Disable()
end
teardown_4_instance(MB,    'MB',    g_mb_ids)
teardown_4_instance(SSR,   'SSR',   g_ssr_ids)
teardown_4_instance(Bloom, 'Bloom', g_bloom_ids)
```

## 3. 收益

### 3.1 行数对比

| 阶段 | apply_postfx_profile | 总相关行数 |
|------|---------------------|-----------|
| Before | ~25 行 (4 quad × 多 setter) | ~28 (函数本身) |
| After  | 9 行 (3 个 SetActiveInstance) | ~30 (含 helper + profile 表) + 一次性 setup_4_instance helper |

净增 setup helper, 但 setup 是一次性的, render loop 部分变薄.

### 3.2 性能

| 指标 | Before | After |
|------|--------|-------|
| 每帧 SetXxx 调用 | ~12 次 (Bloom 3 + SSR 2 + MB 2) × 4 quad = 28+ | 3 次 SetActiveInstance × 4 quad = 12 |
| 状态污染风险 | 高 (任一字段忘 Set 即泄露) | 0 (instance 完全隔离) |
| 学习曲线 | 5 renderer 中 3 个走单例切换, 2 个走 multi-instance | 5 renderer 全统一 |

### 3.3 demo 范式价值

**这是 F.0.10.x 系列的最终形态展示**:
- F.0.10.9.x.2 multi-instance (Create/Destroy/SetActive)
- F.0.10.9.x.3 CloneInstance + GetState
- F.0.10.9.x.4 SetState

3 个 phase 累积的 multi-instance 工具链, 在本 demo 里被一次性串联演示, 5 renderer 全部用一致代码路径:

```
default Enable → SetState(base) → Clone × 3 → SetActive + Enable + SetState(diff) × 3
                                ↓ 渲染时 ↓
                  apply_postfx_profile(i) → SetActiveInstance(id) × 3 renderer
                                ↓ 退出时 ↓
                  teardown_4_instance × 3 renderer (Disable + Destroy)
```

## 4. 验证

### 4.1 真 GL 启动 (CHOCO_AUTO_EXIT=1 模式)

```
[demo_quad_split] 4 HDR instance via Clone: ids=[0, 1, 2, 3], each 640x360
[demo_quad_split] 4 Bloom instance via Clone: ids=[0,1,2,3]    ← 新
[demo_quad_split] 4 SSR instance via Clone: ids=[0,1,2,3]      ← 新
[demo_quad_split] 4 MB instance via Clone: ids=[0,1,2,3]       ← 新
[demo_quad_split] 4 TAA instance via Clone: ids=[0, 1, 2, 3]
[demo_quad_split] OnOpen: setup ok, entering render loop
[demo_quad_split] auto screenshot → docs/screenshots/frame_0000.png
[demo_quad_split] cleanup: releasing 4× (HDR/TAA/Bloom/SSR/MB) instances, LUTs, meshes
demo_quad_split ok
```

### 4.2 PNG 视觉零回归

| Phase | PNG | 字节 |
|-------|-----|------|
| F.0.11 baseline | `quad_split.png` | 36980 |
| F.0.10.10.2 refactor | `quad_split_F0_10_10_2.png` | 36980 |

**字节级一致** — 视觉效果完全相同 (符合预期, 因为 profile 表的字段值和写入时机都是等价的; 区别仅在状态管理方式).

### 4.3 10 smoke 零回归

```
PASS graphics    PASS screenshot   PASS hdr         PASS bloom
PASS ssr         PASS auto_exposure PASS lens_fx    PASS motion_blur
PASS taa         PASS lighting2d
```

## 5. 文件变更

| 文件 | 操作 | 行数 |
|------|------|------|
| `samples/demo_quad_split/main.lua` | refactor | +85 / -25 = 净 +60 |
| `docs/Phase F.0.10.10.2 demo refactor/FINAL_*.md` | 新建 | — |
| `docs/screenshots/quad_split_F0_10_10_2.png` | 视觉验证 (字节同 F.0.11 baseline) | 36980 |
| **总计** | | **~60 LOC** |

## 6. 关键决策回顾

### 6.1 为何不彻底删 apply_postfx_profile 函数

保留函数名是为了:
- diff 友好 (函数边界不变, 旧 readers 易定位)
- render_quad 调用点不动 (减少修改半径)
- 抽象一层 IsEnabled 检查 (将来加新 renderer 可以扩展)

### 6.2 为何 setup_4_instance 是 OnOpen 局部 helper 而非全局

- 仅 OnOpen 1 次调用, 不需要全局可见
- closure 捕获 `self:Close()` 路径方便 (虽然没用到, 保留扩展性)

### 6.3 setup_4_instance 不返回 id_table 而是 in-place 修改

避免局部 table 与全局 g_xxx_ids 之间的赋值开销 (5 行变 1 行), 简化调用点.

## 7. F.0.10.x 系列完整闭环

至此 F.0.10 系列全部 sub-phase 完成:

| Sub-Phase | 功能 |
|-----------|------|
| F.0.10.0~7 | 基础: split-screen + region + LUT 路径 |
| F.0.10.8.* | LUT 系列: cube/HALD/hot reload/能力探测 |
| F.0.10.9 | HDR multi-instance |
| F.0.10.9.x.1 | LUT id remap (跨 instance 同步) |
| F.0.10.9.x.2 | Bloom/SSR/MB multi-instance |
| F.0.10.9.x.3 | Clone + GetState (1 行 setup 模板) |
| F.0.10.9.x.4 | SetState (反向 API, 51 字段) |
| **F.0.10.10.1** | **demo: HDR+TAA 用 Clone 重构** |
| **F.0.10.10.2 (本)** | **demo: Bloom+SSR+MB 用 Clone+SetState 重构 → 5 renderer 全统一** |

API 总数: **115** (F.0.10.0 起累加).

## 8. 下一步

| 任务 | 工作量 | 建议 |
|-----|-------|------|
| **F.1 TAAU DLSS-like upscaling** | ~10-15h | **高优, 重头戏** |
| F.0.11.1 frame_skip 录屏优化 | ~1-2h | 低优 |
| F.0.11.2 HDR .hdr / .exr 截图 | ~3h | 低优 |
| F.1.1 后 TAAU motion vector 改进 | F.1 后续 | 取决 F.1 |

推荐: **F.1 TAAU**, 性能 + 画质双收益, 上下文最完整.
