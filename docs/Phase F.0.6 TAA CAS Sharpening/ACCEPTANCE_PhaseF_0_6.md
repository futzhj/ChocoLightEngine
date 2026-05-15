# Phase F.0.6 TAA CAS Sharpening — ACCEPTANCE

> 6A 工作流 · 阶段 4 (Approve) + 阶段 6 (Assess) 合并
> 关联：`PLAN_PhaseF_0_6.md` / `FINAL_PhaseF_0_6.md` / `TODO_PhaseF_0_6.md`
> 基线：F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.7 (commit `a858a29`)

---

## 1. 任务完整性

| 维度 | 计划 | 实际 | 状态 |
|------|------|------|------|
| GLES3 shader (`render_gl33.cpp` ~2270) | FS_CAS_SOURCE 5-tap CAS (~30 行) | 31 行 GLSL + 算法注释 | ✅ |
| GL33 shader (`render_gl33.cpp` ~3001) | FS_CAS_SOURCE 等价仅切 #version | 28 行 GLSL | ✅ |
| Backend struct | `programCAS` + 3 locs (CAS_InputTex / TexelSize / Sharpness) | 4 个字段 + 注释 | ✅ |
| Backend Init | 编译 programCAS + 绑 sampler slot 0 + log | 12 行新增 | ✅ |
| Backend Shutdown | glDeleteProgram + locs reset | 2 行新增 | ✅ |
| Backend DrawTAACASPass | override impl 复用 DrawTAASharpenPass 模式 | 30 行新增 | ✅ |
| `render_backend.h` virtual | DrawTAACASPass 默认 no-op + doxygen | 10 行新增 | ✅ |
| TAARenderer state | sharpenMode (int, 0=unsharp 默认 / 1=cas) | 1 字段 | ✅ |
| TAARenderer Process | sharpness>0 时按 sharpenMode 切分支 (CAS clamp [0,1]) | 16 行替换 | ✅ |
| TAARenderer Set/Get | parseSharpenMode_ 手写 case-insensitive + 仅识别才写入 | 27 行新增 | ✅ |
| `taa_renderer.h` 声明 | SetSharpenMode/GetSharpenMode + 详细 doxygen | 12 行新增 | ✅ |
| Lua API (`light_graphics.cpp`) | l_TAA_SetSharpenMode (lower + nil+err) / l_TAA_GetSharpenMode + taa_funcs +2 | 40 行新增 | ✅ |
| smoke (`taa.lua`) | surface 25 fn + 默认 + round-trip + 大小写 3 路 + invalid raise + type-error 2 路 + 状态独立 + 六启共存 | 10 新 PASS | ✅ |
| demo (`demo_ssr/main.lua`) | Z 键切 sharpenMode + HUD sharp 字段加 mode 后缀 + Keys help | Z 键块 + sharpStr 重组 + Keys 加 Z=TAASharpenMode | ✅ |
| API docs (`Light_Graphics.md`) | 速查表 23→25 行 + Set/GetSharpenMode 完整文档段（算法对比表 / sharpness 语义 / 推荐场景 / 示例） | +130 行新文档段 | ✅ |
| Lua 语法验证 | `lightc -p taa.lua && lightc -p demo_ssr/main.lua` | Exit 0 / 0 | ✅ |

---

## 2. 决策矩阵对齐验证（6/6）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 CAS 共存模式 | programCAS 与 programSharpen 并存; sharpenMode 切分支 | ✅ |
| D2 sharpness 字段统一 | 共享 `g.sharpness` 字段; Process 内按 mode clamp (CAS [0,1] / unsharp [0,2]) | ✅ |
| D3 CAS sharpness [0, 1] | `casS = (sharpness > 1.0f) ? 1.0f : sharpness` 内部 clamp | ✅ |
| D4 per-mode clamp | TAARenderer Process 内部 clamp，调用方/Lua 不感知 | ✅ |
| D5 HDR clamp(0, ∞) | shader `max(sharpened, vec3(0.0))` 仅截下界，HDR 高光保留 | ✅ |
| D6 默认 mode = "unsharp" | State `sharpenMode = 0` (unsharp 0 / cas 1) | ✅ |

---

## 3. 验收清单

### T1 Backend
- [x] FS_CAS_SOURCE GLES3 编译可通过 (5-tap CAS, AMD FSR1 简化)
- [x] FS_CAS_SOURCE GL33 与 GLES3 完全等价仅切 #version
- [x] backend struct programCAS + 3 locs (复用 sampler slot 0)
- [x] Init: 编译 + 绑 sampler + log INFO/WARN
- [x] Shutdown: glDeleteProgram + locs reset
- [x] DrawTAACASPass override (与 DrawTAASharpenPass 同模式, vaoTonemap 复用)
- [x] render_backend.h: virtual DrawTAACASPass 默认 no-op + doxygen

### T2 TAARenderer
- [x] State 加 `sharpenMode = 0` (默认 unsharp)
- [x] Process 内 sharpness>0 切分支：mode==1 走 DrawTAACASPass (clamp 1.0); 否则 DrawTAASharpenPass
- [x] parseSharpenMode_ 手写 case-insensitive (避免 strcasecmp 跨平台)
- [x] SetSharpenMode 仅识别字符串才写入 (与 SetClipMode 同模式)
- [x] GetSharpenMode 返 const char* "unsharp" / "cas"
- [x] taa_renderer.h 声明 + doxygen 含算法对比 / sharpness 范围 / 性能数据

### T3 Lua + smoke + demo + docs
- [x] l_TAA_SetSharpenMode: lower 转换 + invalid 返 nil+err (与 SetClipMode 同模式)
- [x] l_TAA_GetSharpenMode: lua_pushstring
- [x] taa_funcs[] 23 → 25 fn
- [x] taa.lua surface check 含 25 fn
- [x] taa.lua SharpenMode 默认 "unsharp"
- [x] taa.lua round-trip "cas" / "unsharp"
- [x] taa.lua 大小写不敏感 ("CAS" / "Cas" / "UNSHARP") 3 路
- [x] taa.lua invalid "foo" 返 nil+err 含 "foo" 提示，state 不变
- [x] taa.lua type-error (number / boolean) 返 nil+err
- [x] taa.lua 切换 sharpenMode 不影响 alpha / clipMode / sharpness / antiFlicker / halfRes (状态独立)
- [x] taa.lua F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 六启共存
- [x] taa.lua 末尾计数 25 / 25 + Phase F.0.6 highlights
- [x] demo_ssr Z 键 toggle sharpenMode + 打印切换日志
- [x] demo_ssr HUD sharp 字段加 mode 后缀 ("0.50/unsharp" / "0.50/cas" / "0.00/blit")
- [x] demo_ssr Keys help 加 Z=TAASharpenMode
- [x] Light_Graphics.md 速查表 23 → 25 行 (加 F.0.6 行)
- [x] Light_Graphics.md Set/GetSharpenMode 完整文档段（算法对比表 / sharpness 语义差异 / 推荐场景 / 示例）

### T4 6A 文档
- [x] PLAN_PhaseF_0_6.md
- [x] ACCEPTANCE_PhaseF_0_6.md 本文件
- [x] FINAL_PhaseF_0_6.md (下一文件)
- [x] TODO_PhaseF_0_6.md (下一文件)

### T5 CI
- [x] GitHub Actions 6/6 平台 success (Run 25931268319, 11m42s)
- [x] Windows runtime smoke 25/25 fn + 10 个 F.0.6 PASS + 六启共存
- [x] CI 状态回填 ACCEPTANCE + FINAL + TODO

---

## 4. 关键技术洞察

### 4.1 共存模式 vs 替换 — 哪个更好？

替换 F.0.1 4-tap 为 CAS：
- ✅ shader 只一个，简化代码
- ❌ ~12 ALU vs ~6 ALU 在低端硬件 +0.02ms
- ❌ sharpness 语义变化 (用户 [0, 2] 之前的设置 → CAS 内部 clamp 到 1)

共存模式（采用）：
- ✅ 用户 opt-in CAS，默认零回归
- ✅ 低端硬件可保留 unsharp 节省 ALU
- ✅ Phase F.0.1 文档 / Lua API 不变
- ❌ 多一个 shader / program (但 ~28 行 GLSL，可忽略)

### 4.2 sharpness 字段共享的设计权衡

**方案 A**（采用）：单一 `g.sharpness` 字段，按 mode 解释
- 优势：API 简单，mode 切换不需重设 sharpness
- 劣势：用户切到 CAS 后 sharpness=2.0 实际只用到 1.0（内部 clamp）

**方案 B**（未采）：`g.sharpnessUnsharp` + `g.sharpnessCAS` 双字段
- 优势：mode 切换保留各自值
- 劣势：API 复杂，Set/Get 接口翻倍

D2 选 A 是因为 mode 切换是低频操作，sharpness 调整是高频操作；用户期望"换 mode 后画面差异立现"，而非"还要重设 sharpness"。

### 4.3 HDR clamp(0, ∞) 是 ChocoLight 必须

FSR1 spec 用 `clamp(0, 1)` 因为针对 LDR pipeline。ChocoLight 是 HDR pipeline（sceneTex RGBA16F），CAS 输出后还要走 Tonemap，**绝不能截上界**：

```glsl
// FSR1 标准 (LDR)
return clamp(sharpened, vec3(0.0), vec3(1.0));   // ❌ ChocoLight 不能用

// ChocoLight HDR safe
return max(sharpened, vec3(0.0));                // ✅ 仅截下界防黑斑
```

CAS 算法本身不会让输出超亮（rcpW 归一化），所以丢失上界 clamp 不会引入 firefly；反而保留 HDR 高光让 Tonemap 表现更好。

### 4.4 parseSharpenMode_ 与 parseClipMode_ 完全镜像

复用 Phase F.0.2 的手写 case-insensitive 模式：

```cpp
auto eq = [](const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
};
```

**为什么不用 `strcasecmp`**？
- POSIX 系统 (`<strings.h>`) 有 `strcasecmp`
- Windows MSVC 没有 `strcasecmp`，需 `<string.h>` `_stricmp`
- 跨平台需要 `#ifdef _WIN32` 区分；不如直接手写 8 行 lambda 简洁

### 4.5 CAS peak 公式直观解读

```
peak = -1 / mix(8, 5, sharpness)

sharpness = 0.0  →  peak = -1/8  = -0.125  (最弱锐化)
sharpness = 0.5  →  peak = -1/6.5 ≈ -0.154 (中等)
sharpness = 1.0  →  peak = -1/5  = -0.200  (最强锐化)
```

`peak * ampRGB * (b+c+d+e)` 决定四邻域贡献的负权重；`peak` 越小（绝对值越大），中心像素相对邻域越被强化（锐化更强）。

---

## 5. 性能预期（理论 + 待 CI 实测）

| 阶段 | F.0.1 unsharp | F.0.6 CAS | 增量 |
|------|--------------|----------|------|
| sample 数 | 5 | 5 | 0 |
| ALU 数 | ~6 | ~12 | +6 |
| 时间 (1080p) | ~0.03 ms | ~0.05 ms | **+0.02 ms** |
| 时间 (4K) | ~0.12 ms | ~0.20 ms | +0.08 ms |
| VRAM | 0 (in-place) | 0 (in-place) | 0 |

### CI runtime smoke 验证（pending T5）

```
=== Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 TAA smoke: ALL TESTS PASSED ===
Functions covered: 25 / 25
```

---

## 6. 已知限制 / Phase F.0.x 候选

1. **未做真机 perceptual 对比**：FLIP / SSIM 量化 unsharp vs CAS 画质差异；留 Phase F.0.10
2. **CAS sharpness 语义与 unsharp 不同**：用户切 mode 后期望视觉强度类似但实际有差，文档已明示
3. **5-tap 固定无 4-tap 退化**：CAS 算法本质 5-tap，不能用 4-tap "省"
4. **未 expose CAS internal peak 系数**：用户只能调 sharpness 0~1；FSR1 spec 还允许 peak [-1/8, -1/5] 自定义，但增加 API 复杂度
5. **未实现 RCAS (Robust CAS)**：FSR2 引入；本实现仅 FSR1 简化版

---

## 7. CI 状态 (✅ 全部通过)

| 平台 | 状态 | 状态详情 |
|------|------|---------|
| build-windows | ✅ success | runtime smoke 25 fn + sharpenMode 10 PASS + 六启共存 |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: [25931268319](https://github.com/futzhj/ChocoLightEngine/actions/runs/25931268319)
Commit hash: `7b14f46`
Total duration: `11m42s` (17:16:34 → 17:28:16 UTC)
Date: `2026-05-15`
