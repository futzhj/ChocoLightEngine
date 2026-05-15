# Phase E.17 Half-res Motion Blur — TASK（原子任务拆分）

> 6A 工作流 · 阶段 3
> 基线：DESIGN_PhaseE_17.md 的接口契约 + 数据流时序

---

## 1. 任务拆分原则

- 每个 T 任务可独立编译（不破现有 16 phase + Phase E.15/E.16/E.17 smoke）
- 严格按依赖顺序执行：T1 → T2 → T3 → T4 → T5 → T6 → T7
- 每个任务有明确验收点（编译通过 / smoke PASS / lightc -p 通过）

---

## 2. 任务清单

### T1 — RenderBackend 接口扩展

**输入**：DESIGN §2.1 的两个签名扩展

**输出**：
- `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h`
  - `CreateMotionBlurRT` 增 `storageW=0, storageH=0`
  - `DrawMotionBlur` 增 `rtW=0, rtH=0`
  - 默认实现保持兼容（fallback 到 w/h）

**验收**：
- 头文件改动 < 10 行
- 现有 GL33 backend 编译通过（默认参数兼容）

**估时**：5 分钟

---

### T2 — GL33Backend 实施

**输入**：T1 接口 + DESIGN §3.1/§3.2 实现伪码

**输出**：
- `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp::CreateMotionBlurRT`
  - 加 `int sw = storageW > 0 ? storageW : w` / `sh = ...`
  - `glTexImage2D` 用 `sw, sh`
- `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp::DrawMotionBlur`
  - 加 `int passW = rtW > 0 ? rtW : w` / `passH = ...`
  - Pass1 `glViewport(0, 0, passW, passH)`
  - Pass2 `glBlitFramebuffer(0, 0, passW, passH, 0, 0, w, h, ..., filter)`
  - `filter = (passW == w && passH == h) ? GL_NEAREST : GL_LINEAR`

**验收**：
- ~15 行实施
- 默认 storageW=0/rtW=0 → 行为与 Phase E.16 完全等价
- halfRes ON 时 viewport + blit 尺寸正确

**估时**：15 分钟

---

### T3 — MotionBlurRenderer 状态机扩展

**输入**：T2 backend 已就位 + DESIGN §3.3

**输出**：
- `@e:/jinyiNew/Light/ChocoLight/include/motion_blur_renderer.h`
  - 加 `void SetHalfRes(bool)` / `bool GetHalfRes()` 声明
- `@e:/jinyiNew/Light/ChocoLight/src/motion_blur_renderer.cpp`
  - State 加 `bool halfRes = false`
  - 加 `ComputeStorageSize(w, h, &sw, &sh)` 内部辅助
  - `CreateRT` 调用 backend 时传 sw/sh
  - `Process` 透传 rtW/rtH
  - `SetHalfRes` 立即 Resize（若 enabled）
  - `Disable` / `Shutdown` 不动 halfRes（参数保留）

**验收**：
- ~25 行实施
- SetHalfRes(true) 后 IsEnabled 仍 true
- SetHalfRes 在 Init 前调用不崩溃

**估时**：15 分钟

---

### T4 — Lua API + smoke

**输入**：T3 native API 已就位

**输出**：
- `@e:/jinyiNew/Light/ChocoLight/src/light_graphics.cpp`
  - 加 `l_MB_SetHalfRes` / `l_MB_GetHalfRes`
  - `mb_funcs[]` 末尾加 2 条（13 → 15 fn）
- `@e:/jinyiNew/Light/scripts/smoke/motion_blur.lua`
  - `fn_names` 加 `"SetHalfRes", "GetHalfRes"`
  - 末尾加 §8 Phase E.17 段：3 PASS（默认 false / round-trip / SetHalfRes 不破坏 IsEnabled）
  - 头部注释更新 API count = 15

**验收**：
- `lightc -p scripts/smoke/motion_blur.lua` exit 0
- smoke 总 PASS = 24（21 原 + 3 新）

**估时**：15 分钟

---

### T5 — demo + API docs

**输入**：T4 Lua API 已就位

**输出**：
- `@e:/jinyiNew/Light/samples/demo_ssr/main.lua`
  - 加 `[` 键切 halfRes（cycle false ↔ true）
  - HUD 显示 `halfRes=ON/OFF`
  - Keys 提示加 `[=HalfRes`
- `@e:/jinyiNew/Light/docs/api/Light_Graphics.md`
  - MotionBlur 段加 `SetHalfRes` / `GetHalfRes` 子段
  - 加 性能 / VRAM 节省 表
  - 完整示例加 `MB.SetHalfRes(true)`

**验收**：
- `lightc -p samples/demo_ssr/main.lua` exit 0
- 文档段落与现有 SetMode 风格一致

**估时**：15 分钟

---

### T6 — 6A 收尾文档

**输入**：T1~T5 实施完成

**输出**：
- `ACCEPTANCE_PhaseE_17.md`（决策矩阵 + 任务核对 + CI 占位）
- `FINAL_PhaseE_17.md`（项目总结 + 文件改动统计 + Phase E 累计）
- `TODO_PhaseE_17.md`（必须 / 建议 / 候选 三档）

**验收**：
- 文档结构与 Phase E.16 同款
- 决策矩阵 10/10 全部勾选

**估时**：15 分钟

---

### T7 — Commit + Push + CI 监控

**输入**：T6 文档 + lightc -p 全 pass

**输出**：
- 单 commit（消息含决策回顾 + 影响 + 验证）
- push origin/main
- `gh run watch` CI 6/6 监控
- CI green 后回填 4 个文档

**验收**：
- CI run 6/6 success
- 24 PASS smoke 确认

**估时**：10 分钟（CI 等 ~9 分钟）

---

## 3. 依赖图

```
T1 (Backend 接口) ──▶ T2 (GL33 实施)
                         │
                         ▼
                      T3 (Renderer 状态机)
                         │
                         ▼
                      T4 (Lua API + smoke)
                         │
                         ▼
                      T5 (demo + API docs)
                         │
                         ▼
                      T6 (6A 文档) ──▶ T7 (Commit + CI)
```

---

## 4. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| 默认参数 ABI 改动破坏现有 backend 实现 | T1 编译失败 | C++ 默认参数仅声明端有效，不影响其他 backend 派生类（Legacy 永远不重写） |
| Pass2 GL_LINEAR 在 RGBA16F 上精度问题 | 视觉差 | OpenGL 3.3 / GLES3 spec 都要求 RGBA16F 支持 LINEAR filter；零问题 |
| halfRes ON × mode=2（object_only）二次精度损失 | mode=2 视觉劣化 | 半分辨率本身低通效应足以平滑；用户可手动调 sampleCount |
| Resize 期间 GL 状态污染 | Disable+Enable 切换异常 | 沿用 Phase E.15 现有 `ReleaseRT + CreateRT` 模式，已被 21 PASS 验证 |
| 现有 Phase E.13~E.16 smoke 回归 | CI 失败 | 默认 halfRes=false → 完全等价 Phase E.16 |

---

## 5. 总估时

T1+T2+T3+T4+T5+T6+T7 = 5+15+15+15+15+15+10 = **90 分钟**（不含 CI 等待 9 分钟）

合计 ~100 分钟，与 ALIGNMENT 估计「中期 1 天」对齐（设计 + 实施 + 测试 + 文档）。

---

## 6. 推进确认

TASK 文档完成。Approve 阶段：用户已拍板（"下一步" → 选 Phase E.17）。直接进入 Automate。
