# Phase E.18 Independent Velocity Dilation Pass — ALIGNMENT

> 6A 工作流 · 阶段 1 · Align
> 基线：Phase E.17 commit `f8d7e41`（CI run 25897849619 6/6 success）

---

## 1. 原始需求

抽出 `velocity dilation`（3×3 max-length 邻域采样）为**独立前置 pass**，输出 `dilatedVelocityTex` 供 SSR Temporal + MotionBlur 共享，消除多消费者场景下的重复 9-tap 计算。

---

## 2. 现状（Phase E.17 完成后）

### 2.1 当前 dilation 部署

| Shader | 函数 | 调用次数 / pixel |
|--------|------|----------------|
| `FS_SSR_TEMPORAL` (`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:1997-2010`) | `SampleVelocityDilated` | 1（reproject 时） |
| `FS_MOTION_BLUR` (`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:2591-2603`) | `SampleVelocityDilated` | 1（main） |
| `FS_MOTION_BLUR` (`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:2605-2620`) | `SampleCameraVelocityDilated` | mode=1 时 1 / mode=2 时 1（与 combined 并行） |

### 2.2 重复成本（dilation ON）

| 场景 | 9-tap 次数 / pixel | 全屏 texture fetches @ 1080p |
|------|-------------------|-----------------------------|
| 仅 SSR Temporal 启用 | 1 | 18M |
| 仅 MotionBlur mode=0 启用 | 1 | 18M |
| 仅 MotionBlur mode=2 启用 | 2 | 36M |
| **SSR Temporal + MB mode=2 同启**（最坏情况） | **3** | **54M** |

### 2.3 Phase E.18 收益预估

| 场景 | 当前 cost | E.18 cost（pass + consumer single tap） | 节省 |
|------|-----------|---------------------------------------|------|
| 单消费者（SSR Temporal 或 MB mode=0/1） | 9 fetch / px | 9 + 1 = 10 fetch / px | **−11%（略负）** |
| 双消费者（SSR Temporal + MB mode=0/1） | 18 fetch / px | 9 + 2 = 11 fetch / px | **+39%** |
| 三消费者（SSR Temporal + MB mode=2） | 27 fetch / px | 9 + 9 + 3 = 21 fetch / px（combined + camera 双 dilation） | **+22%** |
| 加 cache locality 优势 | — | dilation pass 单独 + consumer 顺序读 dilated → 更友好 cache | 实测可能更好 |

**结论**：单消费者场景略亏，多消费者场景显著获益。dilation pass 本身只 ~0.05 ms @ 1080p（独立 pass cache 友好），可接受单消费者 11% 多余开销。

---

## 3. 边界与不做项

### 3.1 边界

- ✅ 仅 GL33 backend（Legacy / Vulkan 未来 phase 单独适配）
- ✅ 复用现有 `SetVelocityDilation(bool)` 状态字段（不新增 Lua API）
- ✅ shader 改动方案 **B**（删除 inline 9-tap，sampler 切换 raw↔dilated tex）
- ✅ dilation 状态字段 = false 时跳过 pass（零开销，consumer 直接绑 raw tex）
- ✅ camera-only velocity 同时支持 dilation（与 Phase E.16 双 velocity 一致）

### 3.2 不做项

- ❌ dilation pass 半分辨率优化（Phase E.18.1 候选）
- ❌ 多 mip 级 hierarchical velocity dilation（远期 Phase）
- ❌ separable 2-pass dilation（水平+垂直）— 不能用 max-length 算子（非可分离）
- ❌ shader 内 dilation 完全删除（保留兼容路径）— 改用 B 方案：保留代码但 uVelocityDilation 永远传 0

---

## 4. 项目特性规范对齐

### 4.1 与 Phase E.14 SetVelocityDilation 关系

- **保留** `HDR.SetVelocityDilation(bool)` Lua API（**零 API 变化**）
- 内部行为升级：
  - false → 跳过 dilation pass，consumer 绑 raw velocityTex（同 Phase E.17 单点采样）
  - true → 执行 dilation pass，consumer 绑 dilatedVelocityTex（shader 仍 uVelocityDilation=0，等价单点采样）

### 4.2 与 Phase E.14 RG16F/RG8 双格式关系

- `dilatedVelocityTex` format = **永远 RG16F**（不跟随 raw format）
- 理由：
  1. dilation 是中间数据，不参与最终输出，VRAM 影响可控
  2. shader 无需处理 RG8 encode/decode（仅 input 解码即可）
  3. 简化错误路径（RG8 + dilation 组合）
- VRAM 成本（1080p）：
  - 原 RG8 path: 2 MB × 2 (combined + camera) = 4 MB
  - +Phase E.18 dilated: +4 MB × 2 = +8 MB
  - **总计 +8 MB**（与原 RG16F path 9 MB 同量级，可接受）

### 4.3 与 Phase E.16 cameraVelocityTex 关系

- 同时为 `cameraVelocityTex` 做 dilation → `dilatedCameraVelocityTex`
- 启用条件：与 raw `cameraVelocityTex` 启用条件一致（用户没禁 Phase E.16 双 velocity）
- 工作量：同一份 shader 跑两次（combined + camera），代码复用

### 4.4 与 Phase E.17 half-res 关系

- 完全正交。half-res motion blur 影响的是 `motionBlurTex` 尺寸，**velocityTex 永远 full-res**
- dilation pass 在 velocityTex 之上（full-res），与 half-res 无关
- shader uTexel 仍按全分辨率（Phase E.17 关键洞察）

---

## 5. 决策矩阵

### 5.1 关键决策（10 个，全部自动定）

| # | 决策 | 选项 | 推荐 | 理由 |
|---|------|------|------|------|
| **D1** | dilation pass 触发条件 | A: dilation==true 时始终执行 / B: 仅 ≥2 个消费者启用时 | **A** | 简单可靠，单消费者 +11% 成本可接受；B 引入消费者依赖图复杂度过高 |
| **D2** | 是否做 combined dilation | 是 / 否 | **是** | 主线，SSR Temporal + MB mode=0/2 都需要 |
| **D3** | 是否做 camera dilation | A: 与 cameraVelocityTex 启用条件一致 / B: 永远做 / C: 不做（mode=1/2 仍 inline） | **A** | 与 Phase E.16 双 velocity 启用条件一致；mode=1/2 unused 时 backend 不创建该 RT |
| **D4** | dilatedTex format | A: 永远 RG16F / B: 跟随 raw format | **A** | shader 简化，RG8+dilation 组合错误路径风险高；VRAM +4 MB / RT 可接受 |
| **D5** | pass 触发时机 | HDRRenderer::EndScene 在 SSR/MB 之前自动调 | **A** | 自动化、零用户负担；与 velocity buffer 生成时机相邻 |
| **D6** | shader 改动方案 | A: 删除 inline 9-tap / B: 保留代码但 uVelocityDilation 永远传 0 | **B** | 保留 fallback 路径（dilation pass 失败时可降级 inline）；零行为退化风险 |
| **D7** | 消费者 sampler 切换 | dilation==true 时绑 dilatedTex；==false 时绑 raw tex | **A** | 与 D6 配合：shader 单点采样 + 上游 backend 控制源 |
| **D8** | Lua API 暴露 | A: 不新增（沿用 `HDR.SetVelocityDilation`）/ B: 新增 `EnableVelocityDilationPass` | **A** | 透明升级，零 API breaking change |
| **D9** | dilation pass 失败处理 | A: silent fallback inline 9-tap / B: 关闭 dilation 整体 | **A** | shader B 方案保留 inline 路径，failover 平滑（CC::Log warn 一次） |
| **D10** | half-res dilatedTex 候选 | 不做（Phase E.18.1 候选） | **不做** | 增加 16x 8-bit ALU 不显著，复杂度高 |

### 5.2 自动决策依据

- D1/D2/D3：工程惯例 + 复杂度考虑
- D4：format 简化 vs VRAM trade-off 已平衡
- D5：与 Phase E.13~E.16 EndScene 流程一致
- D6/D7：与 Phase E.17 shader 零改动哲学呼应
- D8：API 稳定性优先
- D9：故障平滑降级
- D10：复杂度边界控制

**无需用户拍板**，10 个决策全部由工程惯例自动确定。

---

## 6. 验收标准（草案）

### 6.1 编译与 ABI

- ✅ `render_backend.h` 新增 2 个虚接口（默认空 impl 兼容现有 backend）
- ✅ GL33 backend 实现完整 dilation pass
- ✅ Legacy / Vulkan backend silent fallback（不实施 dilation pass）

### 6.2 功能

- ✅ dilation==true & SSR Temporal/MB 启用 → 自动执行 dilation pass，结果与 inline 9-tap 像素一致
- ✅ dilation==false → 跳过 pass，consumer 绑 raw velocity，行为同 Phase E.17
- ✅ dilation 运行时切换：立即生效（下一帧）
- ✅ format 切 RG8 与 RG16F 时：dilatedTex 永远 RG16F，shader 内只解码 raw

### 6.3 测试

- ✅ `lightc -p` 双 pass（motion_blur.lua + demo_ssr/main.lua）
- ✅ smoke 现有 24 PASS 零回归
- ✅ velocityDilation 默认行为不变（true）
- ✅ CI 6/6 平台全 success

### 6.4 性能（@ 1080p RGBA16F + RG16F velocity）

- ✅ 双消费者场景（SSR Temporal + MB mode=0/1）：**节省 ≥ 30%**
- ✅ 三消费者场景（SSR Temporal + MB mode=2）：**节省 ≥ 20%**
- ✅ 单消费者场景：性能损失 ≤ 15%（dilation pass 单独 cost）
- ✅ VRAM 增加 ≤ 12 MB（dilated combined + camera RG16F × 2 = 8 MB）

---

## 7. 关键技术风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| dilation pass shader 与 inline 9-tap 像素不一致 | 视觉回归 | 单元测试 + 与 Phase E.16 输出对比；shader 算法完全复刻 |
| dilation==false 路径未走 pass，但 consumer 绑了 dilatedTex（脏读） | crash / 黑屏 | sampler 切换逻辑严格：bool 直接控制，无中间态 |
| Phase E.18 引入额外 EndScene 调用顺序错误 | velocity reproject 失败 | EndScene 内 dilation 严格在 SSR/MB 之前 |
| dilatedTex 创建失败（VRAM 不足）→ consumer 拿 0 | silent skip motion blur | CreateRT 失败 → 关闭 dilation pass，consumer fallback raw velocity |
| RG8 format 下 dilation 输出 RG16F 引入 0.5 偏移误差 | sub-pixel velocity 偏差 | shader 内 raw→DecodeVelocity→max-length→直接写 raw float（无 encode） |

---

## 8. 推进确认

ALIGNMENT 阶段决策 10/10 全部自动定，无需用户拍板。

下一步：进入 **阶段 2: Architect** — 完整 DESIGN 文档。
