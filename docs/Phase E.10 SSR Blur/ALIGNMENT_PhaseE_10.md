# Phase E.10 SSR Blur (反射模糊) — 需求对齐文档

> 启动日期：2026-05-12  
> 6A 阶段：**阶段 1 Align**  
> 范围：在 Phase E.9 SSR 基础上加反射模糊（粗糙度模拟）

---

## 1. 原始需求

> "Phase E.10 SSR Blur（直接延续）"

—— 用户在 Phase E.9 SSR 完成后选定的下一 phase；TODO_PhaseE_9.md 中已列为 **P1 高价值候选**，预估工作量 ~6 小时。

---

## 2. 现状分析

### 2.1 Phase E.9 现有 API（22 函数）

`SetBlurEnabled(bool) / GetBlurEnabled()` API **已存在但 no-op**：

```lua
-- @e:/jinyiNew/Light/ChocoLight/include/ssr_renderer.h:89-91
/// 是否启用反射 blur, 默认 false (Phase E.9 暂不实现; 保留 API 兼容 Phase E.10+)
void SetBlurEnabled(bool flag);
bool GetBlurEnabled();
```

本 phase 的核心任务是 **让该 API 真正生效** + 增加配套参数。

### 2.2 现有 SSR 管线（Process 函数）

```
SSRRenderer::Process(hdrFbo, hdrTex):
  0. BlitHDRDepthToSSAO(hdrFbo, ssrDepthFbo, w, h)
  1. GetHDRNormalTex(hdrFbo) -> normalTex
  2. DrawSSR(... -> reflectFbo)                   // raw reflection RGBA16F full-res
  3. DrawSSRComposite(reflectTex, hdrFbo, ...)    // HDR += reflect * intensity
```

**Phase E.10 集成点**：步骤 2 与 3 之间插入 blur pass（仅当 `blurEnabled` 时）。

### 2.3 现有 SSAO Blur 模板（可镜像）

SSAO 已实现完整的 separable bilateral blur：

```cpp
// @e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:1508 - FS_SSAO_BLUR_SOURCE
// 双边分离滤波: srcAOTex + depthTex -> dstFbo (axis 0/1)
DrawSSAOBlur(srcAOTex, depthTex, dstFbo, w, h, axis);
```

SSAO 使用 ping-pong FBO（半分辨率 R16F × 2），axis=0 水平 + axis=1 垂直。

---

## 3. 边界确认

### 3.1 范围内（IN SCOPE）

- ✅ **反射 RT blur**：在 `reflectTex` 上做 separable Gaussian 模糊
- ✅ **2 个新参数 API**：`SetBlurRadius / GetBlurRadius`
- ✅ **激活 `SetBlurEnabled` API**：从 no-op 变为真正控制 blur pass
- ✅ **Backend 扩展**：新增 1-2 个虚接口（`CreateSSRBlurRT` + `DrawSSRBlur`）
- ✅ **新增 1 个 GLSL shader**：`FS_SSR_BLUR`（双 profile GLES3+GL33）
- ✅ **smoke 扩展**：`scripts/smoke/ssr.lua` 加 BlurRadius 参数检查
- ✅ **demo 扩展**：`samples/demo_ssr/main.lua` 加 BlurRadius 键位
- ✅ **文档**：完整 6A 流程

### 3.2 范围外（OUT OF SCOPE）

- ❌ **per-pixel 粗糙度**：需要 G-buffer 加 R8 roughness channel，工作量翻倍
- ❌ **物理 BRDF 反射（GGX importance sampling）**：超 Phase E.10 复杂度
- ❌ **反射时间累积（temporal accumulation）**：需要 motion vector，超 scope
- ❌ **粗糙度 fresnel 调制**：可选优化，留 TODO
- ❌ **修改现有 Phase E.9 API 签名**：完全 backward compatible

### 3.3 兼容性约束

| 约束 | 处理 |
|------|------|
| 现有 22 函数 API 不能 break | 仅 **新增** 2 函数（24 总数）；`SetBlurEnabled` 行为升级（no-op→生效） |
| Legacy backend 自动 no-op | `SupportsSSR()=false` 时 blur 自然无效，无需额外判断 |
| 内存预算 | 启用 blur 时 +1 个 full-res RGBA16F 临时 RT（~8MB @ 1080p） |
| 现有 smoke 通过 | `ssr.lua` 已有 38 检查点全保留，新增检查仅追加 |
| 现有 demo 兼容 | demo 键位仅追加，不动现有按键 |

---

## 4. 需求理解

### 4.1 用户视角期望

Phase E.10 完成后，Lua 端体验：

```lua
local SSR = Graphics.SSR
SSR.Enable(960, 540)
SSR.SetBlurEnabled(true)       -- ★ 新生效
SSR.SetBlurRadius(2.0)         -- ★ 新增

-- 视觉效果：反射边缘变柔，模拟金属粗糙度
-- 性能影响：blur=1 时 +2 个 pass (~0.5 ms 1080p)
```

### 4.2 算法层期望

- **separable Gaussian**：横扫一遍 + 竖扫一遍，O(2N) 代替 O(N²)
- **5-tap 单核**：与 SSAO blur 同采样数，性能/质量平衡
- **radius 控制**：texel 单位的扩散范围（[0.5, 4.0] 像素）
- **不做 depth-aware**：反射本身要模糊化"金属感"，bilateral 会保边反而破坏效果

### 4.3 集成期望

- **零侵入 reflect RT**：保留 `reflectTex` 单 RT，复用 `DrawSSRComposite(reflectTex, ...)` 不变
- **ping-pong 方式**：reflect → blur temp → reflect（写回）→ composite
- **silent fallback**：blur 资源分配失败 → blur 跳过 → reflect 不模糊（但 composite 仍工作）

---

## 5. 关键技术决策（需用户拍板）

### 决策点 1：blur 算法

| 方案 | 优势 | 劣势 |
|------|------|------|
| **A. 纯 separable Gaussian（推荐）** | 实现简单；反射模糊连续自然 | 跨深度物体反射可能 bleeding |
| B. depth-aware bilateral | 保边，不同物体反射隔离 | 反射边缘锐利，与"模糊金属"初衷违背 |
| C. Kawase blur（多 pass 小核） | 性能略优于 Gaussian | 需要多 pass，资源管理复杂 |

**我的推荐：A**（反射模糊就是要"糊掉"，bilateral 反而破坏粗糙感）

### 决策点 2：blur RT 分辨率

| 方案 | 内存（1080p RGBA16F） | 质量 |
|------|----------------------|------|
| **A. full-res ping-pong（推荐）** | ~8MB 额外 | 与 reflect RT 同精度，最优 |
| B. half-res ping-pong | ~2MB 额外 | 略锐化，需 upscale，shader 略复杂 |
| C. quarter-res ping-pong | ~0.5MB 额外 | 移动端友好，但反射质量下降 |

**我的推荐：A**（Phase E.9 高质量方案延续；blur 用 half-res 会反差太大）

### 决策点 3：blur 半径控制

| 方案 | API | 灵活度 |
|------|-----|------|
| **A. 全局 BlurRadius 参数（推荐）** | `SetBlurRadius(float)` | 单一参数，简单 |
| B. per-pixel roughness（material channel） | 需 G-buffer R8 roughness | 物理正确，但超 scope |
| C. 距离衰减 blur（远处更模糊） | 自动按 view-z 调制 | 自然但可能不可控 |

**我的推荐：A**（per-pixel roughness 需要 Phase E.x 重新设计材质系统）

### 决策点 4：BlurRadius 数值范围

我的推荐：`[0.5, 4.0]` 默认 `1.5`

- `0.5`：极轻微模糊（接近镜面）
- `1.5`：中等模糊（默认，类似 brushed metal）
- `4.0`：强烈模糊（类似 frosted glass / 漫反射）

### 决策点 5：是否同时改 BlurEnabled 默认值

- Phase E.9 默认 `false`
- Phase E.10 默认值：**保持 false**（不破坏现有用户的 Phase E.9 行为）；用户主动 `SetBlurEnabled(true)` 启用

**我的推荐：保持 false 默认**

---

## 6. 疑问澄清

### 用户决策表

| # | 决策点 | 我的推荐 | 用户选择 |
|---|--------|---------|---------|
| 1 | blur 算法 | 纯 separable Gaussian | _待确认_ |
| 2 | blur RT 分辨率 | full-res ping-pong | _待确认_ |
| 3 | blur 半径控制 | 全局 BlurRadius 参数 | _待确认_ |
| 4 | BlurRadius 范围 / 默认值 | [0.5, 4.0] / 1.5 | _待确认_ |
| 5 | BlurEnabled 默认值 | false（与 Phase E.9 一致） | _待确认_ |
| 6 | 是否需要 5-tap → 9-tap 升级？ | 5-tap（与 SSAO 一致） | _待确认_ |

### 行业参考

| 引擎 | SSR Blur 方案 |
|------|--------------|
| Unreal Engine 4 | hierarchical roughness mip chain（4 级 down-sampled cone trace） |
| Unity HDRP | denoise pass + temporal stabilize |
| Godot 4 | per-pixel roughness + Lambertian filter |
| Bevy | 暂未实现专门 SSR blur |
| **ChocoLight Phase E.10（拟定）** | **separable Gaussian, 全局 radius**（轻量、与 SSAO 平行设计） |

ChocoLight 选最轻量方案首发，未来 Phase F+ 可升级 hierarchical mip chain。

---

## 7. 默认决策声明

若用户在 24h 内未明确反馈，采用以下**默认方案**进入下一阶段：

```
算法:        纯 separable Gaussian (5-tap)
RT 分辨率:    full-res ping-pong (RGBA16F)
radius API:  全局 SetBlurRadius / GetBlurRadius
radius 范围: [0.5, 4.0], 默认 1.5
BlurEnabled 默认值: false (与 Phase E.9 保持一致)
新增 API 数: 2 (24 总数)
新增 backend 接口: 2 (CreateSSRBlurRT/DeleteSSRBlurRT + DrawSSRBlur)
新增 shader:  1 (FS_SSR_BLUR, 双 profile)
```

---

## 8. 任务粗估

| 任务 | 工作量 |
|------|-------|
| ALIGNMENT + CONSENSUS + DESIGN + TASK | ~30 min |
| T1 Backend（shader + 2 接口） | ~90 min |
| T2 SSRRenderer 集成（ping-pong + 参数） | ~45 min |
| T3 Lua 绑定（+2 函数） | ~15 min |
| T4 smoke + demo + 全量回归 | ~45 min |
| ACCEPTANCE + FINAL + TODO + CI 验证 | ~15 min |
| **合计** | **~4 小时**（低于 TODO_PhaseE_9 预估的 6 小时） |

---

**待用户确认上述决策点（或采用我的默认方案）即可进入 CONSENSUS。**
