# Phase E.18 Independent Velocity Dilation Pass — TASK 拆分

> 6A 工作流 · 阶段 3 · Atomize
> 基线：Phase E.17 commit `f8d7e41`

---

## 1. 任务依赖图

```
T1 (backend.h 4 fn) ──┐
                      ├─→ T4 (HDRRenderer 集成)
T2 (GL33 shader+impl) ┤
                      └─→ T3 (uniform 控制)
                              │
T4 ──┐                        │
     ├─→ T5 (consumer 切换) ───┘
T3 ──┘
     ↓
T6 (smoke+demo+docs)
     ↓
T7 (ACCEPTANCE+FINAL+TODO)
     ↓
T8 (commit+push+CI)
```

---

## 2. 原子任务清单

### T1: RenderBackend 接口扩展（5 分钟）

**文件**: `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h`

**改动**:
- 新增 Phase E.18 段（4 个虚接口）：`SupportsVelocityDilation` / `CreateVelocityDilateRT` / `DeleteVelocityDilateRT` / `DrawVelocityDilate`
- 新增 backend state 控制接口（2 个）：`SetDilationPassActive(bool)` / `GetDilationPassActive()`

**输入契约**: Phase E.15 接口段存在
**输出契约**: 6 个虚接口（默认空 impl），其他 backend 零改动通过编译
**验收**: 编译通过

---

### T2: GL33Backend dilation pass 实现（35 分钟）

**文件**: `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp`

**改动**:
1. 新增 shader 常量 `FS_VELOCITY_DILATE_SOURCE`（与 FS_BLOOM 同位置附近）
2. 新增 GL state 字段：`programVelocityDilate` + 3 个 uniform location + `velocityDilateSupported`
3. InitShaders 内编译 + 缓存 location
4. 实现 4 个虚接口（CreateVelocityDilateRT / DeleteVelocityDilateRT / DrawVelocityDilate / SupportsVelocityDilation）
5. 实现 dilationPassActive_ 字段 + Set/Get
6. destructor 内清理

**输入契约**: T1 完成
**输出契约**: GL33 完整支持 dilation pass
**验收**: 运行时无 GL error；CC::Log 输出 shader 编译成功

---

### T3: SSR Temporal / Motion Blur shader uniform 控制（10 分钟）

**文件**: `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp`

**改动**:
- DrawSSRTemporal 内 `uVelocityDilation` 上传逻辑：`dilationPassActive_ ? 0 : (velocityDilation ? 1 : 0)`
- DrawMotionBlur 内同上

**输入契约**: T2 完成
**输出契约**: shader 在 dilation pass 启用时走单点采样路径
**验收**: dilationPassActive=true 时 shader 单点采 dilatedTex；=false 时走 inline 9-tap 路径

---

### T4: HDRRenderer 集成（25 分钟）

**文件**:
- `@e:/jinyiNew/Light/ChocoLight/include/hdr_renderer.h`
- `@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp`

**改动**:
1. State 增字段：`dilatedVelocityFbo/Tex` + `dilatedCameraVelocityFbo/Tex`
2. CreateRT 内并创建 dilated RT（combined 始终；camera 与 cameraVelocityTex 同条件）
3. ReleaseRT 内统一释放
4. EndScene 内执行 dilation pass（`velocityDilation==true && backend 支持`）
5. Process 内通知 backend dilationPassActive 状态
6. 新增公开 API：`GetDilatedVelocityTexture()` / `GetDilatedCameraVelocityTexture()`

**输入契约**: T1+T2+T3 完成
**输出契约**: EndScene 自动执行 dilation pass，consumer 可拿 dilatedTex
**验收**: HDR Enable 时 dilatedFbo/Tex 创建成功；Disable 时正确释放

---

### T5: Consumer 切换 dilatedTex 优先（15 分钟）

**文件**:
- `@e:/jinyiNew/Light/ChocoLight/src/motion_blur_renderer.cpp`
- `@e:/jinyiNew/Light/ChocoLight/src/ssr_renderer.cpp`

**改动**:
- MotionBlurRenderer::Process — 取 `GetDilatedVelocityTexture` 优先，失败 fallback `GetVelocityTexture`
- 同步处理 `GetDilatedCameraVelocityTexture` / `GetCameraVelocityTexture`
- SSRRenderer::Process — 取 `GetDilatedVelocityTexture` 优先，失败 fallback `GetVelocityTexture`

**输入契约**: T4 完成
**输出契约**: 消费者透明切换 dilated/raw
**验收**: dilation==true 时绑 dilatedTex；==false 时绑 raw

---

### T6: smoke + demo + Light_Graphics.md 更新（15 分钟）

**文件**:
- `@e:/jinyiNew/Light/scripts/smoke/motion_blur.lua`（仅头部注释更新；fn_names 不变）
- `@e:/jinyiNew/Light/samples/demo_ssr/main.lua`（HUD 显示 dilationPass 状态）
- `@e:/jinyiNew/Light/docs/api/Light_Graphics.md`（HDR.SetVelocityDilation 段补 Phase E.18 行为说明）

**改动**:
- smoke 注释加 Phase E.18 行为升级（fn_names 不变 — 沿用 `HDR.SetVelocityDilation`）
- demo HUD 显示 dilation pass 状态 + Velocity HUD 增 dilation passes 数
- docs 补 dilation pass 数据流图 + 性能/VRAM 收益表

**输入契约**: T5 完成
**输出契约**: 文档/demo/smoke 状态一致
**验收**: `lightc -p` 双 pass

---

### T7: 6A 完结 3 件套（15 分钟）

**文件**:
- `@e:/jinyiNew/Light/docs/Phase E.18 Independent Velocity Dilation Pass/ACCEPTANCE_PhaseE_18.md`
- `@e:/jinyiNew/Light/docs/Phase E.18 Independent Velocity Dilation Pass/FINAL_PhaseE_18.md`
- `@e:/jinyiNew/Light/docs/Phase E.18 Independent Velocity Dilation Pass/TODO_PhaseE_18.md`

**输入契约**: T6 完成
**输出契约**: 决策矩阵 10/10 + 任务核对 + CI 占位
**验收**: 6A 文档完整

---

### T8: commit + push + CI 监控（5 分钟 + ~9 分钟 CI）

**改动**:
- 单 commit：feat: add Phase E.18 independent velocity dilation pass
- push origin main
- `gh run watch` 监控 CI 6/6 green
- 回填 CI 状态到 ACCEPTANCE + FINAL + TODO

**输入契约**: T7 完成
**输出契约**: CI 6/6 success，文档 CI 状态回填
**验收**: GitHub Actions 6/6 平台全 success

---

## 3. 风险矩阵

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| dilation pass shader 与 inline 不一致 | 低 | 视觉回归 | 算法完全复刻；shader 代码 copy-paste |
| dilation pass 创建失败 silent skip 路径 bug | 中 | 视觉异常 | T2/T4 双层兜底；consumer fallback raw |
| dilation==false 但 consumer 仍绑 dilatedTex | 低 | 黑屏 / crash | T5 严格优先级判断 |
| 现有 SSR/MB smoke 回归 | 低 | CI fail | 沿用现有 smoke；新增独立测试 |
| Legacy backend 编译失败 | 低 | CI fail | 默认 impl 兜底（虚函数默认 return 0） |

---

## 4. 总工作量

| 任务 | 估算 |
|------|------|
| T1 | 5 min |
| T2 | 35 min |
| T3 | 10 min |
| T4 | 25 min |
| T5 | 15 min |
| T6 | 15 min |
| T7 | 15 min |
| T8 | 5 min + 9 min CI |
| **总计** | **~120 min（不含 CI）** |

---

## 5. 推进确认

TASK 阶段已完成。

直接进入 **阶段 4: Approve**（10 决策全自动定，无需用户拍板）→ **阶段 5: Automate**（T1~T6 实施）。
