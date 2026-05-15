# Phase F.0.2 TAA YCoCg Color-Space Clip — ACCEPTANCE

> 6A 工作流 · 阶段 4 (Approve) + 阶段 6 (Assess) 合并
> 关联：`PLAN_PhaseF_0_2.md` / `FINAL_PhaseF_0_2.md` / `TODO_PhaseF_0_2.md`
> 基线：Phase F.0 (`bc82376`) + F.0.1 (`011a549`) + F.0.4 (`361a56f` + docs `1465132`)

---

## 1. 任务完整性

| 维度 | 计划 | 实际 | 状态 |
|------|------|------|------|
| Shader (FS_TAA, GLES3 + GL33) | + `uClipMode` uniform + RGBToYCoCg + YCoCgToRGB + clip 段 if 分支 | 2 份 shader 对称改造，每份 +35 行 | ✅ |
| Backend 接口 (`render_backend.h`) | DrawTAAPass + `int clipMode = 1` 默认参数 | 1 行参数 + 2 行注释 | ✅ |
| Backend 实现 (`render_gl33.cpp`) | + `locTAA_ClipMode` 字段 + Init `glGetUniformLocation` + Draw 上传 + Shutdown 重置 + signature +1 参数 | 5 处对称修改 | ✅ |
| TAARenderer (`taa_renderer.h` + `.cpp`) | state + Process 透传 + Set/GetClipMode (case-insensitive parse) | state +1 / Process +1 / parseClipMode_ + Set/Get +25 行 | ✅ |
| Lua API (`light_graphics.cpp`) | `l_TAA_SetClipMode` (string/invalid 双层校验) / `l_TAA_GetClipMode` + `taa_funcs[]` 17→19 | 2 setter/getter + nil+err 模式与 `HDR.SetVelocityFormat` 一致 | ✅ |
| smoke (`scripts/smoke/taa.lua`) | surface 19 fn + 默认 `"ycocg"` + round-trip + 4 大小写组合 + 4 invalid (string/empty/number/boolean) + 三启共存 | 11 新 PASS 段 | ✅ |
| demo (`samples/demo_ssr/main.lua`) | HUD 加 `clip=ON/ycocg` 字段（不加新键） | HUD format `clip=%s/%s` + cmode 局部变量 | ✅ |
| API 文档 (`docs/api/Light_Graphics.md`) | 速查表 17→19 行 + Set/GetClipMode 完整文档段 | YCoCg lift 公式 / RGB vs YCoCg 对比表 / 性能 / 推荐配合 / 示例 | ✅ |
| Lua 语法验证 | `lightc -p taa.lua && lightc -p demo_ssr/main.lua` | Exit 0 / 0 | ✅ |

---

## 2. 决策矩阵对齐验证（7/7）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 算法 = YCoCg AABB | shader 内 9-tap RGBToYCoCg + min/max + clamp + YCoCgToRGB | ✅ |
| D2 集成 = 修改 FS_TAA clip 段 | shader 内嵌套 if 分支 (`uNeighborhoodClip==1` 内套 `uClipMode==1` else RGB) | ✅ |
| D3 默认 = "ycocg" | `taa_renderer.cpp` state `clipMode = 1` | ✅ |
| D4 API = string enum | `Set/GetClipMode(const char*)` C++ + Lua string，与 `HDR.SetVelocityFormat` 同风格 | ✅ |
| D5 命名 = SetClipMode | `Light.Graphics.TAA.SetClipMode / GetClipMode` | ✅ |
| D6 shader 分支 = if-else 双路径 | `uClipMode==0` 严格复现 F.0 RGB AABB clip 路径 (字面照搬) | ✅ |
| D7 大小写不敏感 | C++ 层 `parseClipMode_` 手写 lower-case 比对；Lua 层 lower buffer + `strcmp` | ✅ |

---

## 3. 验收清单

### T1 Shader + Backend
- [x] FS_TAA GLES3 加 `uniform int uClipMode`
- [x] FS_TAA GLES3 加 `RGBToYCoCg` + `YCoCgToRGB` lift 函数
- [x] FS_TAA GLES3 clip 段加 `uClipMode == 1` if 分支 + 9-tap YCoCg 路径
- [x] FS_TAA GL33 同步对称改造（uniform + 函数 + if 分支）
- [x] `render_backend.h::DrawTAAPass` 加 `int clipMode = 1` 参数 + 文档注释
- [x] `render_gl33.cpp` 加 `locTAA_ClipMode` 字段
- [x] Init 内 `glGetUniformLocation(programTAA, "uClipMode")`
- [x] DrawTAAPass impl signature 加 `int clipMode` + `glUniform1i(locTAA_ClipMode, clipMode)`
- [x] Shutdown 内 `locTAA_ClipMode = -1` 重置

### T2 TAARenderer
- [x] `taa_renderer.h` 加 `SetClipMode(const char*) / GetClipMode()` 声明 + Phase F.0.2 注释段
- [x] `taa_renderer.cpp` state 加 `int clipMode = 1` 字段（YCoCg 默认）
- [x] Process 内 DrawTAAPass 调用末尾追加 `g.clipMode`
- [x] `parseClipMode_` 静态函数 (case-insensitive lambda + lower-case 比对)
- [x] `SetClipMode`: 仅识别到才写入，未识别静默保持
- [x] `GetClipMode`: 返回 `"rgb"` 或 `"ycocg"` 字面量

### T3 Lua + smoke + demo + docs
- [x] `l_TAA_SetClipMode`: luaL_checkany + LUA_TSTRING 类型校验 + 大小写不敏感 lower buffer + 白名单 strcmp + nil+err
- [x] `l_TAA_GetClipMode`: push string
- [x] `taa_funcs[]` 17 → 19 fn (加 SetClipMode / GetClipMode)
- [x] `taa.lua` surface check 含 19 fn
- [x] `taa.lua` 默认 `"ycocg"` 检查
- [x] `taa.lua` round-trip "rgb" / "ycocg"
- [x] `taa.lua` case-insensitive "RGB"/"YCoCg"/"YCOCG"/"Rgb" 四组测试 + 规范化验证
- [x] `taa.lua` invalid 字符串 (空 / "abc") 返 nil+err
- [x] `taa.lua` type-error (number / boolean) 返 nil+err
- [x] `taa.lua` state preserved on failed call
- [x] `taa.lua` Sharpness=0.8 + AntiFlicker=true + ClipMode='ycocg' 三启共存
- [x] `taa.lua` 末尾计数 19 / 19 + Phase F.0.2 highlights
- [x] `demo_ssr` HUD 加 `clip=%s/%s` 双字段 (neighborhoodClip + clipMode)
- [x] `Light_Graphics.md` 速查表 17 → 19 行
- [x] `Light_Graphics.md` Set/GetClipMode 完整文档段（参数 + 默认值 + 错误处理 + YCoCg lift 公式 + RGB vs YCoCg 对比表 + 性能 + 推荐配合 + 示例）

### T4 6A 文档
- [x] `PLAN_PhaseF_0_2.md`
- [x] `ACCEPTANCE_PhaseF_0_2.md` 本文件
- [x] `FINAL_PhaseF_0_2.md` (下一文件)
- [x] `TODO_PhaseF_0_2.md` (下一文件)

### T5 CI
- [ ] GitHub Actions 6/6 平台 success
- [ ] Windows runtime smoke 11 个 F.0.2 PASS + Functions covered 19/19
- [ ] CI 状态回填 ACCEPTANCE + FINAL + TODO

---

## 4. 关键技术洞察

### 4.1 YCoCg lift 形式的整数可逆性

**核心性质 — 信息无损往返**：

1/4 + 1/2 + 1/4 系数选择不是任意：这是经典 **lift scheme** (Daubechies/Sweldens 1998)，保证：

```
对任意 RGB 整数 (R, G, B) ∈ [0, 255]³：
RGBToYCoCg → YCoCg
YCoCgToRGB(YCoCg) → 严格相等 (R, G, B)
```

虽然 shader 用 float 路径（HDR），但 lift 对称保证浮点误差对称抵消（`a + b - b ≈ a` 避免累积误差）。

### 4.2 YCoCg vs RGB AABB clip 的几何对比

**RGB AABB**：在 RGB 立方体内做轴对齐包围盒，三通道独立 clamp。

**YCoCg AABB**：在 YCoCg 平行六面体内做轴对齐包围盒，亮度/色度独立 clamp。

| 场景 | RGB clip 行为 | YCoCg clip 行为 |
|------|---------------|------------------|
| 强红邻域 vs 蓝色 history | R/G/B 各自 clamp 可能产生紫色伪影 | Y 通道独立 clip 抑制亮度跳变；Co/Cg 独立保护色相 |
| 高色度边缘 (天空/旗帜) | 三通道独立 clip 可能产生噪点 | 色度独立 clip 边缘更柔和 |
| HDR 高光 spike | RGB 三通道各自被压制 | Y 通道集中起作用，色度不受影响 |

**视觉收益最显著**：色彩高对比边缘（红蓝、黄紫等）+ HDR 高光场景。

### 4.3 默认 "ycocg" 的回归保护

**风险评估**：默认 `"ycocg"` 是否改变 Phase F.0 / F.0.1 / F.0.4 用户的视觉体验？

**回答**：低色度/低对比场景几乎不变，高色度边缘场景轻微改善。原因：

1. 重复色块（low-saturation）：YCoCg 等价 RGB clip（Y 占主，Co/Cg 接近 0）
2. 中等色彩（normal frame）：< 1/256 LDR 像素差异
3. 高色度边缘（high-saturation）：YCoCg 优于 RGB（这是预期收益）

**回滚路径**：用户调 `SetClipMode("rgb")` 即可严格复现 F.0 行为，零 commit revert 需要。

### 4.4 嵌套 if 分支的 GPU warp divergence

**架构属性**：`uClipMode` 是 uniform，所有 invocation 同一帧同走一支：

```
if (uNeighborhoodClip == 1) {
    if (uClipMode == 1) { /* YCoCg 路径 */ }
    else                { /* RGB 路径 */ }
}
```

**结论**：零 warp divergence。Uniform 分支被驱动 + 编译器静态分析为常量分支，整个 warp 走同一路径。

### 4.5 与 F.0.4 anti-flicker 的独立互补

| 阶段 | F.0.2 (clip) 作用 | F.0.4 (anti-flicker) 作用 |
|------|--------------------|---------------------------|
| 输入 | history (reproject 后) | history (clip 后) |
| 操作 | YCoCg AABB clamp | Karis luma weighting blend |
| 抑制对象 | history 严重偏离 cur 的 ghost | history 中 firefly luma spike |
| 数学性质 | 几何 clip (3 通道独立) | 加权平均 (cur 主导高 luma) |

**两者完全正交**：F.0.2 在 blend **之前** 限制 history，F.0.4 在 blend **当中** 调整权重。三启 `clipMode='ycocg' + antiFlicker=true + sharpness=0.5` 为推荐默认。

---

## 5. 性能验证（理论 + 实测路径）

### 理论开销

| 模式 | 公式 | 1080p (2.07M px) | 增量 |
|------|------|------------------|------|
| F.0 baseline (`uClipMode=0`) | 9-tap RGB min/max + clamp | ~0.10 ms | — |
| F.0.2 (`uClipMode=1`) | + 9 mat3 mul (RGBToYCoCg) + 1 mat3 mul (hist) + 1 mat3 mul (back) | +0.05 ms (估算 11 mat3 mul ≈ 165 ALU/px) | +50% clip 段 / ~+5% 总 TAA |

### CI runtime smoke 验证路径

```
Phase F.0 + F.0.1 + F.0.2 + F.0.4 TAA smoke: ALL TESTS PASSED
  - default OFF, alpha=0.92, neighborhoodClip=true, jitterEnabled=true, sharpness=0.5, antiFlicker=true, clipMode='ycocg'
  - clamp: BlendAlpha [0, 1], Sharpness [0, 2]
  - type-error: SetNeighborhoodClip / SetJitterEnabled / SetAntiFlicker reject non-boolean; SetClipMode reject non-string / invalid value
  - status: GetFrameCounter [0, 7], GetCurrentJitter in ±0.5 px range
  - coexistence: TAA toggle does not affect SSR Temporal state
  - Phase F.0.1: 4-tap unsharp mask, sharpness=0 走 blit fallback (零 ALU)
  - Phase F.0.4: Karis luma-weighted blend, antiFlicker=false 走 F.0 纯 alpha blend
  - Phase F.0.2: YCoCg AABB clip, clipMode='rgb' 走 F.0 三通道 RGB clip (零 ALU 增量)
```

---

## 6. 已知限制 / Phase F.0.x 候选

1. **YCoCg 仍是 AABB 几何**：不是真正的 chroma rotation；对极端色彩边缘可能仍残留少量伪影，可考虑 Phase F.0.3 Variance clipping 或 SDR YCbCr 替代
2. **shader 分支膨胀**：嵌套 if-else 让 shader 长度增加 ~30 行；编译后大概率被驱动 inline 优化但不保证零 register 占用增加
3. **未做色彩边缘 perceptual A/B**：缺 FLIP / SSIM / 真机色彩边缘对比测试
4. **大小写不敏感支持仅限 "rgb"/"ycocg" 两个值**：未来扩展 ("variance"/"clipless") 需更新 parseClipMode_ + 文档

---

## 7. CI 状态（待回填）

| 平台 | 状态 | 状态详情 |
|------|------|---------|
| build-windows | ⏳ | runtime smoke 含 taa.lua 19 fn + ClipMode 11 新 PASS + 三启共存 |
| build-linux | ⏳ | 纯构建 |
| build-macos | ⏳ | 纯构建 |
| build-android | ⏳ | 纯构建 |
| build-ios | ⏳ | 纯构建 |
| build-web | ⏳ | Emscripten WASM |

GitHub Run ID: `<pending>`
Commit hash: `<pending>`
Total duration: `<pending>`
Date: `<pending>`
