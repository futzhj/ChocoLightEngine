# Phase F.0.10.6 — HDR multi-instance ACCEPTANCE 验收

> 6A 工作流 · 阶段 6 (Assess) · 验收记录
> 关联: `ALIGNMENT_PhaseF_0_10_6.md` / `DESIGN_PhaseF_0_10_6.md` / `TASK_PhaseF_0_10_6.md`

---

## 1. Sub-Phase 完成状态

| Sub-Phase | 范围 | Commit | 验收 | 工作量 |
|-----------|------|--------|------|-------|
| **SP1+SP2 合并** | 文档 + backend + HDRRenderer + autoTonemap + Lua API + smoke | `b9afe74` | ✅ 31 fn smoke PASS + 8 smoke 零回归 + demo 8 PASS | ~3h |
| **SP3** 6A Assess | ACCEPTANCE/FINAL/TODO | 本 commit | ✅ 文档完整 | 0.5h |
| **合计** | 1 backend + 4 HDR API + 3 Lua API + 7 smoke case | 2 commits | 全过 | **~3.5h** |

---

## 2. Backend 改造 (T1.1 + T1.2)

### 2.1 render_backend.h

```cpp
// 新增虚接口 (默认 no-op)
virtual void DrawTonemapRegion(uint32_t /*hdrTex*/, float /*exposure*/,
                                float /*gamma*/, int /*tonemapMode*/,
                                int /*rgnX*/, int /*rgnY*/,
                                int /*rgnW*/, int /*rgnH*/) {}
```

### 2.2 render_gl33.cpp

```cpp
void DrawTonemapRegion(uint32_t hdrTex, float exposure, float gamma,
                        int tonemapMode, int rgnX, int rgnY,
                        int rgnW, int rgnH) override {
    if (!tonemapSupported || !hdrTex) return;
    // 退化路径: rgn 无效时走老 fullscreen
    if (rgnW <= 0 || rgnH <= 0) {
        DrawTonemapFullscreen(hdrTex, exposure, gamma, tonemapMode);
        return;
    }
    // scissor + glUniform + 全屏 quad
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST); glScissor(rgnX, rgnY, rgnW, rgnH);
    glUseProgram(programTonemap);
    if (locTonemap_Exposure >= 0) glUniform1f(locTonemap_Exposure, exposure);
    if (locTonemap_Gamma    >= 0) glUniform1f(locTonemap_Gamma,    gamma);
    if (locTonemap_Mode     >= 0) glUniform1i(locTonemap_Mode,     tonemapMode);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);
    glBindVertexArray(vaoTonemap); glDrawArrays(GL_TRIANGLES, 0, 6);
    // 解绑 + 复位 scissor
    glBindVertexArray(0); glBindTexture(GL_TEXTURE_2D, 0); glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}
```

**关键决策**:
- 复用 fullscreen 的 `programTonemap` / `vaoTonemap` / `locTonemap_*`
- 退化路径 `rgnW=0` 直接 fwd 给 `DrawTonemapFullscreen` (零回归 fallback)
- 不复位 depth/blend (与 fullscreen 一致, 下次 BeginFrame 重置)
- 复位 scissor (重要: 防影响后续 pass)

---

## 3. HDRRenderer 改造 (T1.3 + T1.4)

### 3.1 hdr_renderer.h

新增 4 个 API:

```cpp
bool SetAutoTonemap(bool on);
bool GetAutoTonemap();
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH);                  // 全局 path (含 AE 叠加)
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH,
              float exposure, float gamma, int tonemapMode);           // 显式 params (不叠加 AE)
```

### 3.2 hdr_renderer.cpp

```cpp
struct State {
    // ...现有字段...
    bool autoTonemap = true;   // 新增, 默认 true 零回归
};

// EndScene 末尾
if (g.autoTonemap) {
    g.backend->DrawTonemapFullscreen(g.sceneTex, exposure, g.gamma, g.tonemap);
}

// 4 个 API 实现 (略, 详见 hdr_renderer.cpp)
```

---

## 4. Lua API 改造 (T2.1-T2.3)

### 4.1 light_graphics.cpp

新增 3 个 Lua fn:

```cpp
{"SetAutoTonemap",              l_HDR_SetAutoTonemap},
{"GetAutoTonemap",              l_HDR_GetAutoTonemap},
{"Tonemap",                     l_HDR_Tonemap},
```

### 4.2 l_HDR_Tonemap 入口

| 参数 | 类型 | 说明 |
|-----|-----|------|
| 1-4 | int | rgnX, rgnY, rgnW, rgnH (像素, 左下原点) |
| 5 (可选) | table | `{exposure=number, gamma=number, tonemap=string|int}` |

**params 解析**:
- 不传 / 非 table → 走 `Tonemap(rgn)` 全局 path (含 AE 叠加)
- 传 table → 各字段独立可选, 缺省回填全局; `tonemap` 接受 `string` (`"aces"/"reinhard"/"uncharted2"/"linear"`) 或 `int` (`0..3`)
- 复用 `hdr_tonemap_name_to_mode` (与 `SetTonemapper` 共享)

**返回值**:
- HDR 未启用 / sceneTex=0 → `nil, "HDR.Tonemap: HDR not enabled (sceneTex = 0)"`
- 成功 → `true`

---

## 5. Smoke 验证

### 5.1 hdr.lua 加 7 case

```
PASS: HDR.GetAutoTonemap() default = true (零回归)
PASS: HDR.SetAutoTonemap true/false round-trip ok
PASS: HDR.SetAutoTonemap bad-arg returns nil + err string
PASS: HDR.SetAutoTonemap idempotent (no-op same value)
PASS: HDR.Tonemap(rgn) headless returns nil + err
PASS: HDR.Tonemap(rgn, params={...}) headless returns nil + err
PASS: HDR.Tonemap(rgn, {tonemap=0..3 int}) all 4 modes accepted (headless)
```

### 5.2 8 smoke 零回归

| Smoke | 结果 |
|-------|------|
| `hdr.lua` | ✅ 31 fn (28→31) PASS |
| `motion_blur.lua` | ✅ |
| `bloom.lua` | ✅ |
| `ssr.lua` | ✅ |
| `ssao.lua` | ✅ |
| `taa.lua` | ✅ |
| `lens_flare.lua` | ✅ |
| `lens_fx.lua` | ✅ |

### 5.3 demo_taa_split2 headless

8 PASS 维持 (零回归).

---

## 6. 验收清单 (TASK §6 全局)

- [x] 1 个 backend 接口 (`DrawTonemapRegion`)
- [x] 4 个 HDRRenderer API (`Tonemap` x2 + `SetAutoTonemap` + `GetAutoTonemap`)
- [x] 3 个 Lua API (`HDR.Tonemap` + `HDR.SetAutoTonemap` + `HDR.GetAutoTonemap`)
- [x] 8 smoke 全过 (零回归)
- [x] demo_taa_split2 headless 8 PASS (零回归)
- [ ] CI 6/6 success (待 push 后 webhook 验证)
- [x] Lua API 总数: 54 → **57** (+3)

---

## 7. 风险矩阵 (实际 vs ALIGN 预测)

| 风险 | 预测 | 实际 | 缓解 |
|-----|------|------|------|
| AE 与 region exposure 互动 | 中 | ⚠️ 通过 2 个重载解决: `Tonemap(rgn)` 含 AE / `Tonemap(rgn,exp,gamma,mode)` 不含 AE | 文档明确 |
| auto=false 用户忘调 region tonemap → 黑屏 | 低 | 实际未影响 (smoke + demo 不退化) | TODO 文档 |
| Backend 代码重复 | 低 | ✅ 通过退化路径解决 (`rgnW<=0` 直接 fwd) | OK |
| CI 平台差异 | 极低 | ⏳ 待 push 后验证 | 不改 shader |

---

## 8. 工作量统计

| 阶段 | 工作量 |
|------|-------|
| ALIGN + DESIGN + TASK 文档 | 0.5h |
| Sub-Phase 1 (backend + renderer) | 1h (低于估 1.5h, 因为复用 fullscreen 模式) |
| Sub-Phase 2 (Lua + smoke) | 1.5h (高于估 1h, 因为加了 7 case 而非 3) |
| Sub-Phase 3 (Assess) | 0.5h |
| **合计** | **~3.5h** (vs DESIGN 估 6-8h, vs TASK 修订估 3.5h) |

**结论**: 远低于初步估的 6-8h, 因为:
1. 不动 shader (复用 4 operator)
2. 不复制 HDR 内容 (sceneTex 仍单实例)
3. 复用 F.0.10.3 region scissor 模式
4. 不改 demo (留后续)

---

## 9. Lua API 演化

| API | 状态 | 说明 |
|-----|------|------|
| `Light.Graphics.HDR.SetAutoTonemap(bool)` | **新增** | 关闭后 EndScene 不自动 tonemap, 必须用户手动 `HDR.Tonemap(rgn)` |
| `Light.Graphics.HDR.GetAutoTonemap()` | **新增** | 默认 true |
| `Light.Graphics.HDR.Tonemap(rgnX, rgnY, rgnW, rgnH [, params])` | **新增** | per-region tonemap, params 可选 |

**当前总数**: 54 → **57** (+3)

---

## 10. 下一步候选

详见 `FINAL_PhaseF_0_10_6.md` §7 + `TODO_PhaseF_0_10_6.md`.
