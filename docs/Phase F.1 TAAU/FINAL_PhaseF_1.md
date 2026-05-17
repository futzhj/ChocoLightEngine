# Phase F.1 TAAU — FINAL 文档（实施记录）

> **阶段**：6A Workflow — 阶段 4 Approve / Apply（实施）
> **基线**：CONSENSUS_PhaseF_1.md (v1.0, 全 A)
> **实施日期**：2026-05-17
> **完成度**：F.1.0 单 instance 主路径全量交付

---

## 1. 实施时间线

| 时段 | 任务 | 实际产出 |
|---|---|---|
| T1 | Backend 接口扩展 + FS_TAA shader uniforms | `render_backend.h` 扩展 + `render_gl33.cpp` DrawTAAPass + OutputSceneTex impl. **Shader uniform 简化为 0 个** (uTexel 始终上传 1/render-res) |
| T2 | HDRRenderer 双尺寸路径 | State +5 字段 + 4 新接口 + EndScene/Tonemap/Resize/SetVelocityFormat/CloneInstance 全更新 |
| T3 | TAARenderer F.1 API + Process 双路径 | State +5 字段 + 8 新公开 API + Q5 仲裁 + ApplyJitter render-res NDC + Process finalDstFbo 切换 |
| T4 | Lua bridge + smoke | 8 Lua bridge + taa_funcs[] 注册 + scripts/smoke/taa.lua 章节 11 (8 子检查点) |
| T5 | demo_taau + 文档收尾 | demo_taau/main.lua + README + 7 件套文档 |

---

## 2. 文件改动清单

### 2.1 Backend 层 (T1)
| 文件 | 改动 |
|---|---|
| `ChocoLight/include/render_backend.h` | DrawTAAPass 签名扩展 5 默认参数; CreateOutputSceneTex / DeleteOutputSceneTex 新虚函数 |
| `ChocoLight/src/render_gl33.cpp` | DrawTAAPass 实现支持双尺寸 + glViewport 用 outputW/H + uTexel 用 1/render-res; CreateOutputSceneTex / DeleteOutputSceneTex backend 实现 |

### 2.2 HDR 层 (T2)
| 文件 | 改动 |
|---|---|
| `ChocoLight/include/hdr_renderer.h` | OnTAAURenderScaleChanged / OnTAAUDisabled / GetSceneTexForOutput / GetSceneFboForOutput 4 新声明 |
| `ChocoLight/src/hdr_renderer.cpp` | State +5 字段 (outputW/H, taauActive, outputSceneFbo/Tex); ReleaseRT 同步释放 outputSceneTex; Enable / Resize / SetVelocityFormat / CloneInstance 处理 F.1 字段; 4 新 API 实现; EndScene + 3 Tonemap 重载改用 GetSceneTexForOutput |

### 2.3 TAA 层 (T3)
| 文件 | 改动 |
|---|---|
| `ChocoLight/include/taa_renderer.h` | 8 新公开 API 声明 (SetTAAUEnabled/RenderScale/UpscalePreset + getter + Get*Resolution × 2) |
| `ChocoLight/src/taa_renderer.cpp` | State +5 字段 + ApplyJitter render-res NDC + Process finalDstFbo + 8 新 API 实现含 Q5 仲裁 + CloneInstance 复位 F.1 字段 |

### 2.4 Lua + Smoke (T4)
| 文件 | 改动 |
|---|---|
| `ChocoLight/src/light_graphics.cpp` | 8 个 l_TAA_* 函数 + taa_funcs[] 注册 |
| `scripts/smoke/taa.lua` | fn_names 增 8 + 章节 11 (8 子检查点) |

### 2.5 Demo + 文档 (T5)
| 文件 | 改动 |
|---|---|
| `samples/demo_taau/main.lua` | 新建 (~250 行) |
| `samples/demo_taau/README.md` | 新建 |
| `docs/Phase F.1 TAAU/ALIGNMENT_PhaseF_1.md` | 新建 |
| `docs/Phase F.1 TAAU/CONSENSUS_PhaseF_1.md` | 新建 |
| `docs/Phase F.1 TAAU/DESIGN_PhaseF_1.md` | 新建 |
| `docs/Phase F.1 TAAU/TASK_PhaseF_1.md` | 新建 |
| `docs/Phase F.1 TAAU/ACCEPTANCE_PhaseF_1.md` | 新建 |
| `docs/Phase F.1 TAAU/FINAL_PhaseF_1.md` | 本文 |
| `docs/Phase F.1 TAAU/TODO_PhaseF_1.md` | 新建 |
| `CHANGELOG.md` | Phase F.1 入口 |

**总计**: 9 文件修改 + 9 文件新建 = 18 文件

---

## 3. 关键设计决策修订

### 3.1 FS_TAA shader 0 新 uniform (vs DESIGN 计划的 4 个)

**DESIGN 原计划**:
- 增 `uRenderTexel / uOutputTexel / uRenderToOutputRatio / uTaauEnabled` 4 个 uniform
- shader main 函数引入 `neighborStep = uTaauEnabled ? uRenderTexel : uTexel` 分支

**实际实施**: shader 完全不动, backend 始终上传 `uTexel = 1.0 / (renderW, renderH)`
- F.0 模式: renderW/H == w/h (调用方未启用 TAAU 时传 0, backend 自动退化), uTexel = 1/(w, h), 与 main HEAD 完全等价
- TAAU 模式: uTexel = 1/(renderW, renderH), cur 邻域 + velocity dilation 9-tap 步长按 render 像素步进, 与 cur tex (render-res) 像素对齐

**为什么这是对的**:
1. cur 邻域采样的目的是采集 cur tex 周围像素做 AABB clip — cur tex 的实际像素步距就是 1/render-res
2. velocity dilation 9-tap 采样 velocityTex (也是 render-res), 同理
3. history reproject 用 vUV 直接读 historyTex (output-res) 不依赖 uTexel
4. 业界 UE4 / Unity HDRP / FSR2 均如此实现, shader 一份伺候 F.0 + F.1

**影响**: shader 改动 0, 复杂度大降, 性能等价, 测试范围缩小 (无需测 F.0 vs F.1 shader 分支)

### 3.2 finalDstFbo 通过 HDRRenderer 跨模块查询

TAA Process 内 sharpen / blit 的目标 FBO 在 TAAU 模式下是 outputSceneFbo (HDRRenderer 维护). 解决方案: 新增 `HDRRenderer::GetSceneFboForOutput()` 暴露给 TAARenderer, 与 `GetSceneTexForOutput()` 配对.

**注意**: 此 helper 隐含 g_active 同步假设 — TAARenderer 与 HDRRenderer 共用 active instance index. F.1.0 仅 default (0) 支持 TAAU, 此假设 trivially 成立; F.1.0.1 多 instance 扩展时需明确锁定 g_active 一致 (TODO).

### 3.3 Multi-instance × TAAU 的 F.1.0 限制

`SetTAAUEnabled` 内显式检查 `g_active == 0`, 非 default instance 启用 TAAU 直接 reject + warning. 这避免了:
- HDR g_active=2 与 TAA g_active=2 时, GetSceneFboForOutput 返 g_active=2 的 outputSceneFbo, 但 OnTAAURenderScaleChanged 是改 g_active 槽的 RT, 跨实例耦合复杂
- 4 instance × 各自 renderScale = 16 种组合, F.1.0 范围内难全验证

留 F.1.0.1 增量做完整 multi-instance 支持 (改 OnTAAURenderScaleChanged 接受 instance id 参数 + 严格 active 同步)

---

## 4. 测试覆盖

### 4.1 单元/Smoke (已通过)
- ✅ Lua API surface (8 新函数全部存在, type=function)
- ✅ Default values (TAAUEnabled=false, RenderScale=1.0, UpscalePreset="native")
- ✅ Clamp 边界 ([0.5, 1.0])
- ✅ Preset ↔ Scale 双向同步 (4 档)
- ✅ 大小写不敏感 ("BALANCED" → "balanced")
- ✅ 容错 (未知 preset 不改变状态)
- ✅ 类型错 (非 string preset 返 nil + err)
- ✅ Headless 防御 (HDR 未启用时 SetTAAUEnabled 不 crash)

### 4.2 集成 (已通过)
- ✅ demo_ssr 启动 + GL3.3 backend 完整 shader 编译, 无 warning/error
- ✅ demo_taa_split2 启动 + multi-instance + split-screen 路径无 warning/error
- ✅ demo_taau 启动 + HDR + TAA 自动启用 (1280×720)

### 4.3 真机视觉/性能 (待用户验证)
- ⏳ 4 档预设视觉对比 (Performance / Balanced / Quality / Native)
- ⏳ GPU profiler 帧时间测量
- ⏳ 镜头快速旋转 ghost 程度
- ⏳ 静态画面收敛速度

---

## 5. 性能数据 (理论估算, 待真机校验)

| 配置 | Render Pixels | Post Pixels | TAA Pixels | Sharpen Pixels | Total GPU 时间 |
|---|---|---|---|---|---|
| F.0 baseline (1.0) | 2.07M | 2.07M | 2.07M | 2.07M | 100% |
| F.1 Quality (0.75) | 1.17M | 1.17M | 2.07M | 2.07M | ~75-85% |
| F.1 Balanced (0.667) | 0.92M | 0.92M | 2.07M | 2.07M | ~60-70% |
| F.1 Performance (0.5) | 0.52M | 0.52M | 2.07M | 2.07M | ~40-55% |

**关键洞察**: TAA pass 本身在 output-res 不变, 但 raster + Bloom + SSR + MotionBlur 全在 render-res 受益, 因此中后处理负担重的场景 (高级 lighting / 多 cube map / 厚重 SSR) TAAU 收益最大.

---

## 6. 已知 / 留观察问题

### 6.1 待 F.1.0.1 解决
- Multi-instance × TAAU 协作 (CONSENSUS Q4: F.1.0 仅 default)
- 跨 instance HDR.GetSceneFboForOutput 共用 g_active 假设的明确化

### 6.2 待 F.1.1 解决
- Mipmap LOD bias = log2(scale) - 0.7 (FSR2 标准): 让纹理细节随 render-res 锐化
- 需扩展 mesh shader uniform, 影响范围 ~10 个 3D shader 文件

### 6.3 待 F.1.2 解决 (可选)
- Velocity nearest-filter 选项 (避开 bilinear 1-pixel 误差)
- 仅当真机测试显示 ghost 严重时再启用

### 6.4 监控点 (实施时未测但 risk)
- ⚠️ Mali / Adreno GLES3 移动端 NPOT FBO 支持 (renderW/H 非 2 倍数)
- ⚠️ 极端 renderScale (0.5) 下 dilation pass 半 res 路径冲突 — 需关注 dilation halfRes + TAAU 0.5 同时启用时是否 1/4 res velocity 太低
- ⚠️ HDR multi-instance Resize 与 TAAU OnTAAURenderScaleChanged 的 g_active 假设 (今版用 saved/restored 局部, 非全局锁)

---

## 7. Commit 拆分 (实际)

实施过程在单个工作流内一气呵成, 未拆 5 commit. 建议 PR review 时按文件分组:

1. **Backend** (T1): `render_backend.h` + `render_gl33.cpp`
2. **HDR** (T2): `hdr_renderer.h` + `hdr_renderer.cpp`
3. **TAA** (T3): `taa_renderer.h` + `taa_renderer.cpp`
4. **Lua + Smoke** (T4): `light_graphics.cpp` + `scripts/smoke/taa.lua`
5. **Demo + Docs** (T5): `samples/demo_taau/*` + `docs/Phase F.1 TAAU/*` + `CHANGELOG.md`

---

## 8. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 实施完结 — F.1.0 全量代码层 + 文档交付 |
