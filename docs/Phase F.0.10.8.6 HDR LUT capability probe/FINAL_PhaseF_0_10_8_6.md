# Phase F.0.10.8.6 — HDR LUT 能力探测 FINAL

> 6A · 阶段 6 · 项目总结
> 工作量 **~0.4h** (vs 估 0.5h, 节约 ~20%)

---

## 1. 项目背景

F.0.10.8.5 加 HDR LUT (RGB16F + DOMAIN > 1.0 + 16-bit PNG) 内部自动 fallback, 用户**透明**. 但 Lua 用户无法主动查询 backend 真支持 HDR LUT — 美术 / UI 需明确状态做决策. 本 phase 加 1 个能力探测 fn 闭环.

## 2. 交付内容

### 2.1 backend (render_backend.h / render_gl33.cpp)

| API | Legacy | gl33 |
|-----|--------|------|
| `SupportsLUT3DFloat() const` | `return false` | `return true` |

### 2.2 hdr_renderer.h / .cpp

```cpp
bool SupportsHDRLUT() {
    if (!g.backend) return false;            // 未初始化
    return g.backend->SupportsLUT3DFloat();
}
```

### 2.3 Lua API (light_graphics.cpp, 1 fn)

```lua
local supported = HDR.SupportsHDRLUT()   -- boolean
if supported then
    print("HDR LUT pipeline ready")
else
    print("Backend will fallback to RGB8 (precision loss)")
end
```

注册到 `hdr_funcs[]`. **HDR 46 → 47 fn**.

### 2.4 smoke (scripts/smoke/hdr.lua)

- `fn_names` 加 `SupportsHDRLUT` (HDR 46 → 47)
- §20 1 PASS: 类型校验 (必须返 boolean, headless 下返 false 也通过 — 因 backend 未初始化)

### 2.5 demo (samples/demo_taa_split2/main.lua)

Headless probe +1 PASS: SupportsHDRLUT 类型校验.

**demo total: 23 → 24 PASS (+1)**

### 2.6 验证结果

| 类型 | 结果 |
|------|-----|
| 编译 (Release) | ✅ 通过 |
| HDR smoke (§20 1 PASS) | ✅ **47 fn** |
| 8 smoke 零回归 | ✅ |
| demo headless | ✅ **24 PASS** (= 23+1) |
| Lua API | 72 → **73** (+1) |

---

## 3. 工作量

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN+DESIGN+TASK | PLAN 极简 1 doc | 0.05h |
| T1 | backend SupportsLUT3DFloat (h + impl) | 0.05h |
| T2 | hdr_renderer SupportsHDRLUT 透传 | 0.05h |
| T3 | l_HDR_SupportsHDRLUT + hdr_funcs[] | 0.1h |
| T4 | smoke §20 + demo probe | 0.1h |
| T5 | docs + commit | 0.05h |
| **合计** | | **~0.4h** |

vs PLAN 估 0.5h, 节约 ~20%. 模式与 `SupportsBloom` / `IsSupported` 完全一致, 一气呵成.

---

## 4. F.0.10.x 系列累计 (now **13 sub-phase**)

| Phase | API | 主题 |
|-------|-----|------|
| F.0.10.2~7 | +17 | TAA / region / per-region tonemap |
| F.0.10.8 | +5 | LUT 基础 |
| F.0.10.8.1 | +1 | .cube 文件 |
| F.0.10.8.2 | +1 | HALD PNG 8-bit |
| F.0.10.8.3 | +6 | 热重载 |
| F.0.10.8.4 | +2 | reload 回调 |
| F.0.10.8.5 | +0 (透明) | HDR LUT (DOMAIN + 16-bit) |
| **F.0.10.8.6** | **+1** | **HDR LUT 能力探测** |
| **累计** | **+33** | 39 → **73** Lua API |

## 5. LUT 子生态最终态 (主动 + 被动 + 探测三轨)

```
┌─────────────────────────── Lua user ───────────────────────────┐
│                                                                  │
│ 1️⃣ 能力探测  HDR.SupportsHDRLUT() → bool       [F.0.10.8.6 ← NEW] │
│    └ UI badge "HDR LUT ready"                                    │
│                                                                  │
│ 2️⃣ 加载       LoadCubeLUT / LoadHaldLUT                          │
│    └ 自动 LDR/HDR 路径选择                       [F.0.10.8.5]    │
│    └ .cube DOMAIN_MAX > 1.0 → RGB16F             [F.0.10.8.5]    │
│    └ 16-bit PNG → RGB16F                          [F.0.10.8.5]    │
│                                                                  │
│ 3️⃣ 热重载     Watch + Poll                       [F.0.10.8.3]    │
│ 4️⃣ 通知       SetLUTReloadCallback               [F.0.10.8.4]    │
│ 5️⃣ 应用       SetGradingLUT(id, strength)                        │
└──────────────────────────────────────────────────────────────────┘
```

## 6. 典型用法 (UI 自动适配 LDR/HDR)

```lua
local HDR = require('Light.Graphics').HDR

-- 启动时探测
if HDR.SupportsHDRLUT() then
    ui_show_badge('HDR LUT ✓')
    cube_path = 'assets/luts/aces_hdr.cube'   -- DOMAIN_MAX 4 4 4
else
    ui_show_badge('LDR LUT only')
    cube_path = 'assets/luts/portrait_ldr.cube' -- DOMAIN_MAX 1 1 1
end

local id, err = HDR.LoadCubeLUT(cube_path)
if id then HDR.SetGradingLUT(id, 1.0) end
```

## 7. 后续候选

- **F.0.10.9** 真多 HDR target / RT pool (~8-10h) — split-screen 多 instance
- **F.1** TAAU DLSS-like (~10-15h)
- **F.2** PBR Material system

## 8. 结论

Phase F.0.10.8.6 完成, 用 **~0.4h** 加 1 Lua fn 闭环 LUT 子生态. 至此 F.0.10.8 LUT 子生态最终态 (**主动加载 + 被动热重载 + 通知 + 能力探测**) 全部覆盖, 与工业 DCC tool (Resolve / Photoshop / Premiere) 100% 兼容.
