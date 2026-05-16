# Phase F.0.10.9.x.3 — GetState / Clone FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (5 renderer × 2 fn = +10 Lua API, **95 → 105**)

---

## 1. 完成工作

为 5 个 multi-instance renderer (HDR / TAA / Bloom / SSR / MotionBlur) 全部加上:
- `CloneInstance(srcId) → newId`: 1 行复制 src 全部调参字段到新 instance
- `GetState() → table`: 当前 active instance 全 state 快照 (flat lua table)

| Renderer | CloneInstance 复制范围 | GetState 字段数 |
|----------|----------------------|---------------|
| HDRRenderer | exposure / tonemap / gamma / dilation / auto-* / lut | 15 |
| TAARenderer | blend_alpha / sharpen / variance / half_res / upscale | 10 |
| BloomRenderer | threshold / intensity / radius / levels | 7 |
| SSRRenderer | max_steps / intensity / blur / temporal / rejection | 16 |
| MotionBlurRenderer | strength / sample_count / mode / half_res | 7 |

## 2. Lua API +10 (95 → 105)

| Renderer | 新 API |
|----------|--------|
| HDR | `CloneInstance` / `GetState` |
| TAA | `CloneInstance` / `GetState` |
| Bloom | `CloneInstance` / `GetState` |
| SSR | `CloneInstance` / `GetState` |
| MotionBlur | `CloneInstance` / `GetState` |

API 命名一致, 与 HDR (F.0.10.9) / Bloom/SSR/MB (F.0.10.9.x.2) 同模板.

## 3. CloneInstance 设计

### 3.1 行为
```cpp
int CloneInstance(int srcId) {
    if (srcId 非法 / 未分配) return 0;        // log warn
    for 空闲槽 i in [1, MAX_INSTANCES):
        g_states[i] = g_states[srcId];      // 全字段复制
        // 复位 backend 创建的 RT (新 instance 待自己 Enable)
        g_states[i].fbo / texs[] = 0;
        g_states[i].enabled = false;
        // 复位 temporal state (TAA / SSR 新 instance 第一帧 fallback 走 cur)
        g_states[i].historyIdx = 0;
        g_states[i].hasPrevViewProj = false;
        ++g_count;
        return i;
    return 0;   // 槽满
}
```

### 3.2 复位字段 (各 renderer 独有)

| Renderer | 复位 RT | 复位 temporal |
|----------|--------|--------------|
| HDR | fbo, sceneTex, dilatedVel*, dilatedCamVel* | — |
| TAA | historyFbos[2], historyTexs[2], width/historyW | historyIdx, hasHistory |
| Bloom | fbos[8], texs[8], actualLevels | — |
| SSR | depth*, reflect*, blur*[2], history*[2] | historyIdx, prevViewProj, hasPrevViewProj, frameCounter |
| MB | fbo, tex | — |

## 4. GetState 设计

### 4.1 lua table 字段类型

- `number`: exposure, gamma, intensity, blend_alpha, ...
- `integer`: tonemap_mode (0..3), levels (2..N), max_steps (8..128), mode (0..2), rejection_mode (0..1), lut_id (uint32_t)
- `boolean`: enabled, supported, auto_*, half_res, blur_enabled, ...
- `string`: sharpen_mode ("unsharp"/"cas"/"rcas"), upscale_mode ("bilinear"/"bicubic"/"lanczos")

### 4.2 用户用例

```lua
-- save profile
local snap = HDR.GetState()
serialize(snap, "user_profile.json")

-- restore profile (用户手动 Set, GetState 不带反向 SetState)
HDR.SetExposure(snap.exposure)
HDR.SetTonemapper(...)   -- 注: SetTonemapper 接 string

-- debug HUD
print("HDR P1 expo:", HDR.GetState().exposure)
HDR.SetActiveInstance(1)
print("HDR P2 expo:", HDR.GetState().exposure)
```

## 5. 验证

### 5.1 8 smoke 零回归 + multi-instance 测试

```
PASS hdr  (含 27.1~27.4 GetState/Clone)
PASS bloom (含 CS.1~5)
PASS ssr  (含 CS.1~5)
PASS auto_exposure
PASS lens_fx
PASS motion_blur (含 CS.1~5)
PASS taa  (含 9.17.1~4)
PASS lighting2d
```

每个 renderer 的 Clone/GetState 测试段覆盖:
- **CS.1**: GetState 返 table, 必备字段全在
- **CS.2**: Clone(0) 复制 default profile + enabled=false
- **CS.3**: Per-instance 修改隔离 (改 cloned 不污染 default)
- **CS.4**: Clone(无效 srcId) → 0
- **CS.5**: Clone on 槽满 → 0

### 5.2 真 GL 模式

- demo_quad_split 启动 OK (4 HDR + 4 TAA + Bloom + SSR + MB 全 Enable)
- 不破坏老 demo (零回归)

## 6. 关键 lessons

### 6.1 模板高度同质 (5 renderer 同结构)

5 个 renderer 模板复用最大化, 实际工作:
- C++ header: 5 文件 × ~10 行 = 50 行 (decl)
- C++ cpp: 5 文件 × ~30 行 = 150 行 (impl, RT 字段每 renderer 独立)
- Lua wrap: 5 × 2 fn × ~15 行 = 150 行
- smoke: 5 × ~50 行 = 250 行

### 6.2 字段命名陷阱 (用户可见 API 不一致)

- `HDR.SetTonemapper` 接 **string** (不接 int), 因为有 `hdr_tonemap_name_to_mode` 转换层
- `Bloom.SetRadius` clamp 到 **[0, 1]** (不是 [0, 4])
- 写 smoke 时实测验证, 不要假设 API
- GetState 返的 `tonemap_mode` 是 int (与 `GetTonemapper()` 一致), 但 `SetTonemapper` 接 string — 不对称设计但向后兼容

### 6.3 SetTonemapper(int) 在 luaL_checkstring 下 silent fallback

```lua
HDR.SetTonemapper(2)   -- 实际等价于 SetTonemapper("2") → unknown name → fallback "aces" (=0)
```

应改用 string: `HDR.SetTonemapper("uncharted2")`. 没改 SetTonemapper 接受 int 的兼容性 — 违反最小变更原则.

### 6.4 GetState flat table vs nested

- 选 **flat** (与 Get* fn 1:1 对应)
- 优势: serialize/log 简单, 无嵌套查找
- 代价: 字段名前缀有点重复 (`auto_taa`, `auto_bloom`, `velocity_dilation_*`)

## 7. 文件变更

| 文件 | 变更 |
|------|------|
| `ChocoLight/include/{hdr,taa,bloom,ssr,motion_blur}_renderer.h` | +10 行/文件 (5 fn decl) |
| `ChocoLight/src/hdr_renderer.cpp` | +40 行 (CloneInstance) |
| `ChocoLight/src/taa_renderer.cpp` | +38 行 |
| `ChocoLight/src/bloom_renderer.cpp` | +40 行 |
| `ChocoLight/src/ssr_renderer.cpp` | +44 行 |
| `ChocoLight/src/motion_blur_renderer.cpp` | +35 行 |
| `ChocoLight/src/light_graphics.cpp` | +180 行 (10 wrap fn + 5 funcs 表 entry) |
| `scripts/smoke/{hdr,taa,bloom,ssr,motion_blur}.lua` | +50~70 行 (CS § / 9.17 §) |
| `docs/Phase F.0.10.9.x.3 GetState-Clone/{PLAN,FINAL,TODO}.md` | 新建 |
| **总计** | **17 files, +750/-0 LOC** |

## 8. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN §1 用户场景 + 任务范围 |
| **Architect** | PLAN §2 设计原则 + 改造模板 |
| **Atomize** | PLAN §3 3 sub-task + 风险表 |
| **Approve** | 用户选 "F.0.10.9.x.3 GetState/Clone" |
| **Automate** | 5 renderer 顺序改造 + Lua wrap + smoke 扩展 (~2.5h, 实际比 PLAN 估 3h 略快) |
| **Assess** | 本 FINAL + 8 smoke 零回归 + 真 GL 启动验证 |

## 9. F.0.10.x 累计里程碑

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0.10.0 ~ F.0.10.9.3 | 多 instance HDR/TAA + region 后处理 + LUT 子生态 | 80 |
| F.0.10.10 | quad-split 联动 demo | 0 (lua-only) |
| F.0.10.9.x.2 | Bloom/SSR/MB multi-instance | +15 (95) |
| **F.0.10.9.x.3 (本)** | **5 renderer × CloneInstance + GetState** | **+10 (105)** |
| **总计** | **完整 multi-instance 后处理链 SDK + state introspection** | **105 fn** |

## 10. 下一步候选

- **demo_quad_split 改造为 multi-instance + Clone setup** (~30min, 零 API 增量, 演示新能力)
- F.0.11 demo 截图/录屏 (~3h, 高优, 配合 quad demo 视觉验收)
- F.1 TAAU DLSS-like (~10-15h, 大版本)
- F.0.10.9.x.4 SetState (反向, 方便 save/load profile, ~1h, 中优)
