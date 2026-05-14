# Phase E.12 Temporal SSR — FINAL 项目总结报告

> **任务**：Phase E.12 — Temporal SSR（跨帧累积降噪）
> **基线**：Phase E.11 Bilateral SSR Blur
> **状态**：🟡 实现完成，待 CI / runtime smoke 最终确认
> **范围**：Backend + GL33 shader + SSRRenderer + Lua API + smoke + demo + docs

---

## 1. 一句话总结

Phase E.12 在 Phase E.9/E.10/E.11 SSR 管线之上加入 **TAA-style Temporal SSR**：

- ✅ **Halton-2,3 8-sample jitter**：SSR ray march 起点跨帧抖动
- ✅ **Reverse reprojection from depth**：不引入 velocity buffer，使用 `prevViewProj * invCurViewProj`
- ✅ **Full-res history ping-pong**：RGBA16F × 2，Temporal 输出可继续进入 blur
- ✅ **Neighborhood AABB clip rejection**：降低 ghost / history leak
- ✅ **Lua 可控**：新增 6 个 API，SSR API 28 → 34
- ✅ **安全降级**：Temporal shader/history 不可用时回退 Phase E.11 行为

---

## 2. 实施过程回顾（6A 工作流）

| 阶段 | 输入 | 输出 | 关键产出 |
|------|------|------|---------|
| Align | 用户继续 Phase E.12 | 需求与边界对齐 | `ALIGNMENT_PhaseE_12.md` |
| Align (续) | 用户选择「全 A」 | 方案冻结 | `CONSENSUS_PhaseE_12.md` |
| Architect | 共识文档 | 架构图、数据流、接口契约 | `DESIGN_PhaseE_12.md` |
| Atomize | 设计文档 | 原子任务拆分 | `TASK_PhaseE_12.md` |
| Automate | T1/T2/T3/T4 | C++ / GLSL / Lua / docs 实现 | 源码与脚本改动 |
| Assess | 实现结果 | 验收与总结 | `ACCEPTANCE_*` / `FINAL_*` / `TODO_*` |

---

## 3. 核心改动清单

### 3.1 Backend / GL33

| 文件 | 改动 |
|------|------|
| `ChocoLight/include/render_backend.h` | `DrawSSR` 增加 `jitterX/jitterY`；新增 `CreateSSRHistoryRT/DeleteSSRHistoryRT/DrawSSRTemporal` |
| `ChocoLight/src/render_gl33.cpp` | `FS_SSR` 增加 jitter；新增 `FS_SSR_TEMPORAL`；新增 program/uniform 缓存；实现 history RT 与 temporal pass |

### 3.2 SSRRenderer

| 文件 | 改动 |
|------|------|
| `ChocoLight/include/ssr_renderer.h` | 新增 `Set/GetTemporalEnabled`、`Set/GetTemporalAlpha`、`Set/GetRejectionMode` |
| `ChocoLight/src/ssr_renderer.cpp` | 新增 temporal state、Halton jitter、列主序 `Mat4Mul`、history lifecycle、Process temporal 插入 |

### 3.3 Lua / smoke / demo / docs

| 文件 | 改动 |
|------|------|
| `ChocoLight/src/light_graphics.cpp` | 新增 6 个 SSR Temporal Lua bridge 并注册 |
| `scripts/smoke/ssr.lua` | SSR API 覆盖 34 个，新增 E.12 默认值 / round-trip / clamp / 联动测试 |
| `samples/demo_ssr/main.lua` | 增加 T/U/I/N 控制与 HUD 展示 |
| `samples/demo_ssr/README.md` | 同步 Temporal SSR 管线、按键、默认值、限制 |
| `docs/API_REFERENCE.md` | SSR API 28 → 34，Phase E.12 文档入口 |

---

## 4. 技术设计重点

### 4.1 数据流

```text
HDR FBO depth/color/normal
    ↓ Blit depth
SSR raw pass (+ jitter)
    ↓
Temporal pass (reproject + reject + blend)
    ↓
可选 SSR Blur (Gaussian / Bilateral)
    ↓
SSR Composite 写回 HDR
```

### 4.2 Temporal 参数

| 参数 | 默认 | 范围 | 用途 |
|------|------|------|------|
| `TemporalEnabled` | `true` | bool | 开关 Temporal SSR |
| `TemporalAlpha` | `0.9` | `[0.5, 0.99]` | history 权重 |
| `RejectionMode` | `1` | `{0,1}` | 0=current-depth threshold，1=neighborhood clip |

### 4.3 关键不变量

- `historyFbos[0/1]` 与 `historyTexs[0/1]` 必须同时有效才允许 temporalActive
- 首帧或 resize 后 `hasPrevViewProj=false`，shader 输出当前帧，避免黑帧
- `Mat4Mul` 必须与项目 `Mat4::operator*` 列主序一致
- `DrawSSR` 的 jitter 参数为像素单位，GL33 backend 转换为 UV 空间
- Temporal shader 不可用时不分配 history，避免读取未写入纹理

---

## 5. 质量评估

### 5.1 代码质量

| 维度 | 评价 | 备注 |
|------|------|------|
| 架构一致性 | ✅ | 沿用 RenderBackend 虚接口 + SSRRenderer 模块模式 |
| 向后兼容 | ✅ | `TemporalEnabled=false` 或 history 不可用时回退 Phase E.11 |
| 命名清晰 | ✅ | temporal/history/rejection/jitter 语义明确 |
| 资源管理 | ✅ | history RT 与 blur RT 同生命周期模式 |
| 风险控制 | ✅ | shader 不可用、RT 分配失败均 graceful fallback |

### 5.2 测试质量

| 测试维度 | 覆盖 |
|----------|------|
| API surface | 34/34 函数存在性 |
| 默认值 | TemporalEnabled / Alpha / RejectionMode |
| Round-trip | bool / float / int |
| Clamp | Alpha 下界/上界，RejectionMode clamp |
| 联动 | Temporal 与 Blur / Bilateral 参数独立 |
| 视觉 | 需真实窗口 demo 手测 |

---

## 6. 验证状态

| 项 | 状态 |
|---|---|
| 源码静态一致性 | ✅ 已完成 |
| 文档一致性 | ✅ 已同步 |
| 本地 CMake build | 🚫 按用户偏好不执行 |
| 本地 `light.exe` smoke | 🚫 按用户偏好不执行 |
| `lightc -p` Lua 语法检查 | ✅ `scripts/smoke/ssr.lua` + `samples/demo_ssr/main.lua` 通过 |
| `git diff --check` | ✅ 通过 |
| CI 6 平台 build | ⏳ push 后确认 |
| Windows runtime smoke | ⏳ CI / runtime 环境确认 |

---

## 7. 已知限制

| 限制 | 影响 | 后续方向 |
|------|------|----------|
| 无 velocity buffer | 动态物体 / 相机快速运动下可能 ghost | Phase E.x 可引入 motion vectors |
| mode=0 depth-only 仍为预留 | 当前主要有效模式为 neighborhood clip | 后续实现 depth threshold rejection |
| 视觉无 CI 自动回归 | 只能靠 smoke + 手动观察 | 后续可引入截图 diff |
| full-res history VRAM 增加 | 1080p 约 +16MB | 移动端可考虑 half-res 或 dynamic disable |

---

## 8. 结论

Phase E.12 Temporal SSR 已完成源码与文档层交付，形成完整可配置的时序 SSR 管线。当前只剩项目既定流程中的 CI / runtime smoke / 真实窗口视觉确认。
