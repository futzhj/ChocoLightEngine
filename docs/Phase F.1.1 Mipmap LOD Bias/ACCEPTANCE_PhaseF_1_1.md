# Phase F.1.1 Mipmap LOD Bias — ACCEPTANCE 文档

> **阶段**：6A Workflow — 阶段 5 Acceptance（验收）
> **基线**：PLAN_PhaseF_1_1.md
> **实施记录**：FINAL_PhaseF_1_1.md
> **验收日期**：2026-05-17

---

## 1. 功能验收

### 1.1 Backend 接口
- [x] `RenderBackend::SetMipBias(float)` / `GetMipBias() const` 虚函数 ([render_backend.h:346-355](../../ChocoLight/include/render_backend.h#L346))
- [x] GL3.3 backend 实现含 clamp [-4, +4] ([render_gl33.cpp:8118-8125](../../ChocoLight/src/render_gl33.cpp#L8118))
- [x] state cache `mipBias_` 默认 0.0 (零回归)
- [x] Legacy backend default no-op (与 F.0 行为一致)

### 1.2 Shader 改造 (4 shader 源)
- [x] `FS_UNLIT_SOURCE` GLES3 (line 304+) 增 `uniform float uMipBias` + 2 处 `texture(.., uMipBias)`
- [x] `FS_UNLIT_SOURCE` GL3.3 (line 684+) 同上
- [x] `FS_PBR_SOURCE` GLES3 (line 387+) 增 uniform + 5 处 `texture(.., uMipBias)` (BaseColor / MR / Normal / Occlusion / Emissive)
- [x] `FS_PBR_SOURCE` GL3.3 (line 766+) 同上
- [x] `uMipBias=0.0` 时 GLSL `texture(s, uv, 0.0)` ≡ `texture(s, uv)` (零回归证明)

### 1.3 Uniform 上传
- [x] `UploadVelocityUniforms` 末尾自动上传 `uMipBias` ([render_gl33.cpp:4017-4019](../../ChocoLight/src/render_gl33.cpp#L4017))
- [x] 共享路径 (Unlit/PBR static + skinned + skinned+morph 3 路径) 全部覆盖
- [x] 旧 program 缺 uniform 时 `glGetUniformLocation` 返 -1, 静默跳过 (兼容性保障)

### 1.4 TAARenderer 自动 hook
- [x] `autoMipBias_` 全局标志 (默认 true)
- [x] `updateMipBias_()` 内部 helper 计算公式 `bias = log2(renderScale) - 0.7` (TAAU 启用时), 否则 = 0
- [x] Hook 点: `applyTAAUChange_()` (覆盖 SetTAAUEnabled 与 SetRenderScale 的所有路径) + `SetActiveInstance` (切 instance 后重算)
- [x] 4 新公开 API: `SetAutoMipBias / GetAutoMipBias / SetMipBias / GetMipBias`

### 1.5 Lua bridge
- [x] 4 个 l_TAA_* 函数 + taa_funcs[] 注册 ([light_graphics.cpp](../../ChocoLight/src/light_graphics.cpp))
- [x] taa.lua surface 列表增 4 函数 (总数 49→53)

### 1.6 Smoke 增量 (5 检查点)
- [x] 13.1: 默认值 (autoMipBias=true, mipBias=0.0)
- [x] 13.2: SetAutoMipBias round-trip
- [x] 13.3: 手动 SetMipBias 持久 (autoMipBias=false 时)
- [x] 13.4: backend clamp [-4, +4]
- [x] 13.5: TAAU 关闭时 auto-bias 保持 0 (与 RenderScale 无关)

**Smoke 结果**: **171 PASS / 0 FAIL**

### 1.7 Demo 增强
- [x] `samples/demo_taau/main.lua` HUD 增 MipBias 显示 + B 键切 autoMipBias
- [x] `samples/demo_taau/README.md` 更新 F.1.1 进展说明

---

## 2. 兼容性验收 (零回归)

| Demo | 启动 | warn/error/fail/undef |
|---|---|---|
| `demo_ssr` | ✅ | 0 |
| `demo_taa_split2` | ✅ | 0 |
| `demo_taau` | ✅ | 0 |
| `demo_multi_hdr_pip` | ✅ | 0 |

**Smoke**: 171 PASS / 0 FAIL (F.0 → F.1.0 → F.1.0.1 → F.1.1)

---

## 3. 设计决策回顾

### 3.1 Bias 公式选择: `log2(renderScale) - 0.7`
| 业界标准 | bias 偏移 | 锐度 | alias 风险 |
|---|---|---|---|
| UE4 TAAU | log2(scale) | 中 | 低 |
| **ChocoLight F.1.1** | **log2(scale) - 0.7** | **较高** | **中-低** |
| FSR2 / DLSS | log2(scale) - 1.0 | 高 | 中 |

折衷选 -0.7: 比 UE4 锐度高, 比 FSR2 略保守, 与 ChocoLight 现有 TAA clip 强度配合不易引入闪烁.

### 3.2 Shader 改造范围: 仅 3D mesh shader
**纳入**: FS_UNLIT_SOURCE / FS_PBR_SOURCE (含 skinned + morph 变体, 共享 fragment)

**排除**:
- `FS_SOURCE` (2D batch): UI/HUD 纹理多无 mipmap, bias 反引入 alias
- `FS_LIT2D_SOURCE`: 同上 2D 路径不参与 jitter
- 后处理 shader: 全屏 quad, 不涉及 LOD 决策

### 3.3 单一全局 autoMipBias_ vs per-instance
backend `mipBias_` 是单一全局 (硬件 sampler bias 是 GPU 状态), 必须与 active instance 同步. 多 HDR-instance × TAAU 场景下:
- 切 active TAA instance → updateMipBias_ → backend mipBias_ 跟着切
- 用户调用约定: HDR active 与 TAA active 同步 (F.1.0.1 已建立)

不实现 per-instance autoMipBias 因为:
- 真实场景下用户对 4 instance 不会要求各自不同 bias 策略
- 多一层 state 反而引入混乱

### 3.4 GLSL `texture(s, uv, bias)` 兼容
- GLES 3.0+ / GL 3.3+ 全支持 (重载形式, 自动识别第 3 个参数为 bias)
- bias 添加到自动计算的 LOD level
- ChocoLight legacy backend (GL 2.x) 没有 PBR shader, 不受影响

---

## 4. 文档验收

- [x] `docs/Phase F.1.1 Mipmap LOD Bias/PLAN_PhaseF_1_1.md`
- [x] `docs/Phase F.1.1 Mipmap LOD Bias/ACCEPTANCE_PhaseF_1_1.md` (本文)
- [x] `docs/Phase F.1.1 Mipmap LOD Bias/FINAL_PhaseF_1_1.md`
- [x] `docs/HANDOFF_REMAINING_TASKS.md` 更新
- [x] `samples/demo_taau/README.md` 更新

---

## 5. 验收结论

**核心交付**: TAAU 启用时纹理细节自动锐化 (3D mesh shader 4 个), 配合 4 新 Lua API 让用户精细控制. 5 smoke 检查点 + demo HUD + B 键调试.

**验收级别**:
- ✅ **代码层**: PASS (Release build clean, smoke 171 PASS / 0 FAIL)
- ✅ **兼容性**: PASS (4 demo 全零回归; uMipBias=0 时 shader 行为与 F.0 完全等价)
- ⏳ **真机视觉**: 待用户在 demo_taau 切 0.667 scale 对比 F.1.0 (无 bias) vs F.1.1 (auto bias) 的远处纹理锐度

**结论**: F.1.1 **代码层通过验收**, 进入用户真机评估阶段。

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 验收提交 — 代码层 PASS, 真机评估 PENDING |
