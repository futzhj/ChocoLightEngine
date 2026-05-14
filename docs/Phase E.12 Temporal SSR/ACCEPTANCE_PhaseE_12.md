# Phase E.12 Temporal SSR — ACCEPTANCE 验收文档

> **任务名**：Phase E.12 Temporal SSR（时序 SSR 累积降噪）
> **状态**：✅ **实现完成，CI 6/6 + Windows runtime smoke 通过**
> **基线**：Phase E.11 Bilateral SSR Blur（SSR API 28，Windows runtime smoke 60/60 PASS）
> **方案**：TAA-style reverse reprojection from depth + Halton jitter + full-res history ping-pong + neighborhood clip

---

## 1. 任务完成度总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Align | ALIGNMENT 文档（项目上下文 + 需求对齐） | ✅ |
| Align (续) | CONSENSUS（用户拍板全 A） | ✅ |
| Architect | DESIGN（架构图 + 数据流 + 接口契约） | ✅ |
| Atomize | TASK（T1/T2/T3/T4 原子任务） | ✅ |
| T1 Backend | `DrawSSR` +2 jitter 参数，新增 history RT / temporal pass 虚接口，GL33 shader + implementation | ✅ |
| T2 SSRRenderer | State / helper / setter-getter / Process temporal 插入 | ✅ |
| T3 Lua API | 6 个 Temporal API bridge + `ssr_funcs[]` 注册 | ✅ |
| T4.1 smoke | `scripts/smoke/ssr.lua` 扩展至 34 API 覆盖和 E.12 参数测试 | ✅ |
| T4.2 demo | `samples/demo_ssr` 增加 T/U/I/N 控制与 HUD | ✅ |
| T4.3 docs | `API_REFERENCE.md` + demo README + 本验收文档 | ✅ |
| CI / runtime | GitHub Actions 6 平台 + Windows runtime smoke | ✅ |

---

## 2. 关键实现验收

### 2.1 Backend 接口与 GL33 实现

| 项 | 验收点 | 状态 |
|---|---|---|
| `RenderBackend::DrawSSR` | 签名新增 `jitterX/jitterY`，默认实现 no-op 保持旧 backend 兼容 | ✅ |
| `CreateSSRHistoryRT` | full-res RGBA16F × 2 ping-pong RT，失败返回 false | ✅ |
| `DeleteSSRHistoryRT` | 成对释放 FBO / texture 并清零 | ✅ |
| `DrawSSRTemporal` | 绑定 cur/history/depth 三纹理，上传 reprojection / alpha / rejection / hasHistory | ✅ |
| `FS_SSR` | 新增 `uJitterOffset`，ray march 起点使用 jittered UV | ✅ |
| `FS_SSR_TEMPORAL` | GLES3 + GL33 双 profile，reproject + out-of-bounds reject + neighborhood clip + blend | ✅ |
| shader fallback | `CreateSSRHistoryRT` 依赖 `ssrTemporalSupported`，Temporal shader 不可用时不分配 history | ✅ |

### 2.2 SSRRenderer 管线

| 项 | 验收点 | 状态 |
|---|---|---|
| State | 新增 `temporalEnabled/temporalAlpha/rejectionMode/history*/prevViewProj/frameCounter` | ✅ |
| 参数默认值 | `temporalEnabled=true`，`temporalAlpha=0.9`，`rejectionMode=1` | ✅ |
| clamp | alpha `[0.5, 0.99]`，rejection `{0,1}` | ✅ |
| jitter | Halton-2,3 8-sample，Temporal active 时传像素偏移，backend 转 UV | ✅ |
| 矩阵 | `Mat4Mul` 与项目 `Mat4::operator*` 一致使用列主序 | ✅ |
| temporal pass | `DrawSSR -> DrawSSRTemporal -> DrawSSRBlur -> DrawSSRComposite` | ✅ |
| fallback | history 不可用 / temporal disabled 时回退 Phase E.11 raw/blur/composite | ✅ |
| lifecycle | Disable / Resize / Shutdown 释放 history，并 reset 首帧状态 | ✅ |

### 2.3 Lua API 与 smoke

| 项 | 验收点 | 状态 |
|---|---|---|
| API surface | SSR API 28 → 34（新增 3 对） | ✅ |
| `Set/GetTemporalEnabled` | bool round-trip | ✅ |
| `Set/GetTemporalAlpha` | 默认值、round-trip、上下界 clamp | ✅ |
| `Set/GetRejectionMode` | 默认值、round-trip、clamp `{0,1}` | ✅ |
| 联动测试 | Temporal × Blur × Bilateral 状态独立 | ✅ |
| smoke 脚本 | headless tolerant，不强依赖 GL runtime 成功启用 SSR | ✅ |

---

## 3. 验证清单

| 验证项 | 当前状态 | 说明 |
|---|---|---|
| 源码一致性检查 | ✅ | 已检查接口、调用点、shader uniform、Lua 注册、demo key/HUD |
| Lua 语法检查 `lightc -p` | ✅ | `scripts/smoke/ssr.lua` 与 `samples/demo_ssr/main.lua` 语法检查通过；未跑 runtime smoke |
| 本地 CMake build | 🚫 | 按用户偏好不在本地执行 |
| 本地 `light.exe` smoke | 🚫 | 按用户偏好不在本地执行 |
| GitHub Actions 6 平台 | ✅ | run `25871544298`，6/6 success |
| Windows runtime smoke | ✅ | run `25871544298`，`build-windows` success，包含 `scripts/smoke/ssr.lua` |
| 真实窗口视觉验收 | ⏳ | 需桌面 GL3.3 环境手测 demo_ssr |

---

## 4. 已发现并修复的问题

| 问题 | 影响 | 修复 |
|---|---|---|
| `Mat4Mul` 早期草案与项目列主序不一致 | Temporal reprojection 可能漂移或错误取 history | 已改为与 `Mat4::operator*` 一致的列主序乘法 |
| Temporal shader 不可用时仍可能分配 history | 可能进入 temporalActive 但 `DrawSSRTemporal` no-op，后续 blur/composite 读取未写入 history | `CreateSSRHistoryRT` 改为依赖 `ssrTemporalSupported` |
| API_REFERENCE 停留在 E.11 28 API | 文档与实际 API surface 不一致 | 已同步为 E.12 34 API |
| 设计文档 jitter / 矩阵描述不精确 | 后续维护易误解 | 已同步为 UV jitter 与列主序矩阵 |

---

## 5. 验收结论

Phase E.12 的生产代码、Lua API、smoke 覆盖、demo、文档和 CI/runtime 验证已经完成。

**最终完成判据**：
- GitHub Actions 6 平台 build success ✅ run `25871544298`
- Windows runtime smoke 中 SSR 脚本 0 fail ✅ run `25871544298`
- 真实窗口 demo 中 T/U/I/N 控制可用，Temporal 开启后反射噪声下降且无明显黑帧/闪烁
