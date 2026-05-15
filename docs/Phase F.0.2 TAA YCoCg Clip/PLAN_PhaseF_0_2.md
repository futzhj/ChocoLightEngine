# Phase F.0.2 — TAA YCoCg Color-Space Clip · 实施计划

> 6A 工作流（精简化）· 阶段 1-3 合并：Align + Architect + Atomize
> 上游基线：Phase F.0 (`bc82376`) + F.0.1 (`011a549`) + F.0.4 (`361a56f` + docs `1465132`)
> 当前 commit：`1465132`
> 关联：`ACCEPTANCE_PhaseF_0_2.md` / `FINAL_PhaseF_0_2.md` / `TODO_PhaseF_0_2.md` (T4 生成)

---

## 1. Align — 任务对齐

### 1.1 用户原始需求

> 在 Phase F.0.4 完成后从 TODO §2 候选列表选择 Phase F.0.2 — YCoCg color-space clip 推进。

### 1.2 任务边界

**做**：

1. FS_TAA shader 在已有的 `uNeighborhoodClip == 1` 9-tap AABB clip 路径内加 YCoCg 色彩空间 clip 模式
2. shader 加 `uniform int uClipMode` (0=RGB, 1=YCoCg)，if 分支保护，零 ALU 回退到 F.0
3. backend 接口 `DrawTAAPass` 增加 `int clipMode = 1` 默认参数（向后兼容 legacy 后端）
4. TAARenderer state + Process 透传 + `Set/GetClipMode` C++ API
5. Lua API +2：`Light.Graphics.TAA.SetClipMode("rgb"|"ycocg") / GetClipMode() → string`
6. smoke `taa.lua` 17 → 19 fn surface + 默认 `"ycocg"` + round-trip + type-error
7. demo HUD 加 `clip=ycocg` / `clip=rgb` 字段（不加新键，复用 N=reject 思路也可保留 mode 静态展示）
8. `Light_Graphics.md` 速查表 + Set/GetClipMode 完整文档段
9. 6A 文档四件套（PLAN/ACCEPTANCE/FINAL/TODO）

**不做（明确排除）**：

- 不改 9-tap 邻域采样数量（仍是 8 邻 + cur 共 9 tap）
- 不引入 variance clipping（留给 Phase F.0.3）
- 不改 sharpening / anti-flicker 任何逻辑（F.0.1 + F.0.4 完全保留）
- 不增加新 RT / 不修改 history pingpong / 不改 jitter 行为
- 不在 demo 加新键（mode 通过 HUD 静态展示，避免 demo keymap 进一步膨胀）

### 1.3 验收标准

| ID | 标准 | 验证方式 |
|----|------|---------|
| AC-1 | shader 编译通过（GLES3 + GL33 双源） | CI windows/linux/macos/android/ios/web 6/6 success |
| AC-2 | clipMode="rgb" 严格复现 F.0 行为 | shader 内 if 分支保护，零 ALU 回退 |
| AC-3 | clipMode="ycocg" 默认启用 | TAARenderer state `clipMode = 1` |
| AC-4 | Lua surface 17 → 19 fn | smoke taa.lua surface check |
| AC-5 | type-error 走 `nil + err` | 与 F.0 / F.0.4 同模块同模式 |
| AC-6 | 字符串值 "rgb"/"ycocg" 大小写不敏感 + 非法值返 nil+err | smoke round-trip 4 个测试 |
| AC-7 | 与 antiFlicker / sharpness / clip 三者共存 | smoke 三启共存测试 |
| AC-8 | runtime smoke "Phase F.0 + F.0.1 + F.0.2 + F.0.4" 全 PASS | CI Windows runtime smoke 输出 |
| AC-9 | API 文档速查表 17 → 19 fn 行 | Light_Graphics.md |

---

## 2. Architect — 架构设计

### 2.1 数据流（FS_TAA shader 内）

```
cur (RGB) ──┐                       ┌──> alpha blend / Karis blend
            │                       │
            ▼                       │
     [if uClipMode == 0]            │
       9-tap RGB AABB ──────────────┤
       hist = clamp(hist, mn, mx)   │
                                    │
     [if uClipMode == 1]            │
       9-tap each → RGBToYCoCg      │
       AABB in YCoCg space          │
       histY = clamp(histY, mnY, mxY)
       hist = YCoCgToRGB(histY) ────┘
```

### 2.2 YCoCg 转换公式（lift 形式，与 FXAA / Inside 标准一致）

```glsl
// RGB → YCoCg (info-preserving, integer-reversible 系数 1/4 1/2 1/4)
vec3 RGBToYCoCg(vec3 c) {
    return vec3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,    // Y  (luma)
         0.5  * c.r              - 0.5  * c.b,   // Co (orange-blue chroma)
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);   // Cg (green-magenta chroma)
}

// YCoCg → RGB (lift 反变换)
vec3 YCoCgToRGB(vec3 c) {
    return vec3(
        c.x + c.y - c.z,    // R
        c.x       + c.z,    // G
        c.x - c.y - c.z);   // B
}
```

### 2.3 Shader if 分支结构

GLES3 + GL33 双源对称改造，clip 段从 `if (uNeighborhoodClip == 1) {...}` 改为：

```glsl
if (uNeighborhoodClip == 1) {
    if (uClipMode == 1) {
        // Phase F.0.2 YCoCg 路径
        vec3 curY = RGBToYCoCg(cur.rgb);
        vec3 mn = curY, mx = curY;
        vec3 s;
        s = RGBToYCoCg(texture(uCurHdrTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb); mn = min(mn, s); mx = max(mx, s);
        // ... 8 邻域同
        vec3 histY = RGBToYCoCg(hist.rgb);
        histY = clamp(histY, mn, mx);
        hist.rgb = YCoCgToRGB(histY);
    } else {
        // F.0 RGB AABB clip 路径
        vec3 mn = cur.rgb, mx = cur.rgb;
        vec3 s;
        s = texture(uCurHdrTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        // ... 8 邻域同
        hist.rgb = clamp(hist.rgb, mn, mx);
    }
}
```

### 2.4 接口签名变更

```cpp
// render_backend.h (基类)
virtual void DrawTAAPass(...,
                         int /*antiFlicker*/  = 1,
                         int /*clipMode*/     = 1) {}   // Phase F.0.2: 默认 YCoCg

// render_gl33.cpp (子类)
void DrawTAAPass(..., int antiFlicker, int clipMode) override;

// taa_renderer.h
void   SetClipMode(const char* mode);   // "rgb" / "ycocg" (case-insensitive)
const char* GetClipMode();               // 返回当前 mode 字符串

// light_graphics.cpp
static int l_TAA_SetClipMode(lua_State* L);   // string arg, nil+err on invalid
static int l_TAA_GetClipMode(lua_State* L);   // returns string
```

---

## 3. 决策矩阵（自动决策 7/7）

| # | 决策点 | 选项 | 选择 | 依据 |
|---|--------|------|------|------|
| D1 | clip 算法 | RGB AABB / YCoCg AABB / Variance | **YCoCg AABB** | 任务定义；UE5 / Inside 主流 |
| D2 | 集成方式 | 修改 FS_TAA / 新 shader / 多 pass | **修改 FS_TAA clip 段** | 与 F.0.4 同思路，零新 pass / 零新 RT |
| D3 | 默认 mode | "rgb" (F.0 行为) / "ycocg" (F.0.2 默认) | **"ycocg"** | F.0.2 主题即新算法默认；与 F.0.4 默认 anti-flicker=true 同思路 |
| D4 | API 类型 | bool toggle / int enum (0/1) / string enum ("rgb"/"ycocg") | **string enum** | 与 `HDR.SetVelocityFormat("rg16f"/"rg8")` 同风格；未来扩展 ("variance") 自然 |
| D5 | 命名 | SetClipMode / SetClipColorSpace / SetClipAlgorithm | **SetClipMode** | 简洁，与既有 SetVelocityFormat / SetRejectionMode 名词一致 |
| D6 | shader 分支 | if-else 双路径 / 仅 YCoCg | **if-else 双路径** | 保留 F.0 严格复现路径；shader uniform 分支零 warp divergence |
| D7 | 大小写敏感 | 严格 / 不敏感 | **不敏感** | "RGB"/"YCoCg" 用户拼写习惯多样；与 SetVelocityFormat 行为对齐 |

---

## 4. Atomize — 任务拆分（5 个原子任务）

### T1 — Shader 改造（GLES3 + GL33 双源对称）

**输入**：`render_gl33.cpp` `FS_TAA_SOURCE` (GLES3 line 2095-2179) + `FS_TAA_GL33_SOURCE` (line 2734-2812)

**操作**：

1. 两份 shader 各加 `uniform int uClipMode;` 紧跟 `uAntiFlicker` 后
2. 两份 shader 各加 `RGBToYCoCg` + `YCoCgToRGB` 函数（在 `void main()` 前）
3. clip 段 `if (uNeighborhoodClip == 1) {...}` 内加 `if (uClipMode == 1) {YCoCg 路径} else {RGB 路径}`

**输出契约**：两份 shader 编译通过；`uClipMode == 0` 时 ALU 与 F.0 完全等价

**验收**：CI 6/6 平台 success

---

### T2 — Backend 接口 + impl

**输入**：`render_backend.h::DrawTAAPass` 虚函数 + `render_gl33.cpp::DrawTAAPass` impl + state 字段

**操作**：

1. `render_backend.h::DrawTAAPass` 加 `int /*clipMode*/ = 1` 默认参数 + 注释段
2. `render_gl33.cpp` 加 `GLint locTAA_ClipMode = -1;` state 字段
3. Init 内 `glGetUniformLocation(programTAA, "uClipMode")`
4. DrawTAAPass impl 加 `int clipMode` 参数 + `glUniform1i(locTAA_ClipMode, clipMode)`
5. Shutdown 内 `locTAA_ClipMode = -1;` 重置

**输出契约**：legacy 后端不需修改（默认参数 = 1 表示新行为）；gl33 backend uniform 上传正确

---

### T3 — TAARenderer state + Set/GetClipMode

**输入**：`taa_renderer.h` + `taa_renderer.cpp`

**操作**：

1. `taa_renderer.h` 在 `Set/GetAntiFlicker` 之后加 `void SetClipMode(const char*); const char* GetClipMode();` 声明 + 注释
2. `taa_renderer.cpp` state 加 `int clipMode = 1;` 字段（1=YCoCg 默认）
3. Process 内 DrawTAAPass 调用末尾追加 `g.clipMode`
4. Set/GetClipMode 实现：
   - `SetClipMode(const char*)`: case-insensitive 解析 "rgb" → 0 / "ycocg" → 1；其他字符串静默忽略（C++ 层不返错误，错误处理在 Lua 层）
   - `GetClipMode()`: 返回 `"rgb"` 或 `"ycocg"` 字面量字符串

---

### T4 — Lua API + smoke + demo HUD + docs

**输入**：`light_graphics.cpp` + `scripts/smoke/taa.lua` + `samples/demo_ssr/main.lua` + `docs/api/Light_Graphics.md`

**操作**：

1. `light_graphics.cpp` 加 `l_TAA_SetClipMode` (luaL_checkstring + 大小写不敏感比对 + 非法值 nil+err) + `l_TAA_GetClipMode` (push string)
2. `taa_funcs[]` 17 → 19 fn 加 `SetClipMode` / `GetClipMode`
3. `taa.lua` 增加：
   - surface check 19 fn
   - 默认 `"ycocg"` 检查
   - round-trip "rgb" / "ycocg" / "RGB" / "YCoCg" 大小写不敏感
   - 非法值 "abc" / number / nil 返 nil+err
   - 与 antiFlicker / sharpness 三启共存测试
4. `demo_ssr/main.lua` HUD 加 `clip=rgb/ycocg` 字段（不加新键，避免 keymap 膨胀）
5. `Light_Graphics.md` 速查表 17 → 19 fn 行 + Set/GetClipMode 完整文档段（参数 / 默认值 / 错误处理 / 算法 / YCoCg 转换公式 / 性能 / 推荐配合 / 示例）

---

### T5 — 6A 文档 + commit + push + CI 验证

**输入**：`docs/Phase F.0.2 TAA YCoCg Clip/{ACCEPTANCE, FINAL, TODO}.md`

**操作**：

1. 写 ACCEPTANCE_PhaseF_0_2.md（任务完整性表 + 决策矩阵对齐 + 验收清单 + 关键技术洞察 + CI 待回填）
2. 写 FINAL_PhaseF_0_2.md（交付物总览 + 算法 + API surface + 协同 + 性能 + 反思）
3. 写 TODO_PhaseF_0_2.md（必做完成状态 + Phase F.0.x 候选剩余 + CI 回填）
4. `git add` + commit + push
5. `gh run list --limit 1` + 等 ~8min + `gh run view <id> --json` 验证 6/6
6. 抓 Windows runtime smoke log 抽 ClipMode 相关 PASS
7. 回填 ACCEPTANCE §CI + FINAL §CI + TODO §CI
8. docs-only commit + push

---

## 5. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| YCoCg lift 公式 typo（系数错） | 低 | 高（颜色全错） | shader 内做 `RGBToYCoCg(YCoCgToRGB(c)) ≈ c` 自检 (在 PLAN 阶段，但实际不实施防御代码，靠 CI smoke + 视觉对比) |
| `clipMode="rgb"` 路径与 F.0 行为不完全等价 | 极低 | 中 | shader 内 RGB 分支保留原代码完全字面照搬 |
| 大小写不敏感解析 bug | 低 | 中 | 用 `tolower` 简单 lower-case 比对，smoke 覆盖 4 大小写组合 |
| 非法字符串泄露到 backend | 低 | 中 | C++ 层默认值保护：未识别字符串保持当前 mode；Lua 层做错误返回 |
| YCoCg 路径与 F.0.4 anti-flicker 同时启用产生意外 | 极低 | 中 | 两者完全独立（clip 在 blend 之前），逻辑无干扰；smoke 三启共存测试 |
| API 速查表行数同步（17→19）漏改 | 低 | 低 | 文档段最后审核 |

---

## 6. 工作量预估

| 任务 | 预估 | 备注 |
|------|------|------|
| T0 PLAN | 0.5h | 本文件 |
| T1 Shader | 0.5h | 两份对称 + YCoCg 函数 |
| T2 Backend | 0.3h | 4 处对称改造（与 F.0.4 同模式） |
| T3 TAARenderer | 0.3h | state + Process + Set/Get |
| T4 Lua/smoke/demo/docs | 1.0h | smoke 4 大小写组合最复杂 |
| T5 6A docs + CI | 0.5h | 三件套 + commit + 监控 |
| **合计** | **3.1h** | TODO §2 预估 4h，符合 |

---

## 7. 验证策略

### Lua 语法验证

```sh
.\Light\lightc.exe -p scripts/smoke/taa.lua
.\Light\lightc.exe -p samples/demo_ssr/main.lua
```

### 本地 CMake 不构建（用户偏好）

直接 git push → CI 6/6 平台同步验证。

### Windows runtime smoke 期望日志

```
PASS: Default ClipMode = "ycocg" (Phase F.0.2)
PASS: ClipMode round-trip ok
PASS: ClipMode case-insensitive ok ("RGB"/"rgb"/"YCoCg"/"ycocg")
PASS: SetClipMode invalid value rejected (nil+err)
PASS: SetClipMode type-error rejected (number / nil)
PASS: ClipMode + AntiFlicker + Sharpness 三启共存
=== Phase F.0 + F.0.1 + F.0.2 + F.0.4 TAA smoke: ALL TESTS PASSED ===
Functions covered: 19 / 19
```
