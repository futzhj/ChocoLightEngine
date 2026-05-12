# Phase E.9 SSR — TODO 与遗留事项

> 完成日期：2026-05-12  
> 状态：核心实现 + 验证完成，待 CI 6 平台 build 验证 + 后续可选优化

---

## P0 立即待办（本次提交剩余）

### 1. Git Commit + Push

```bash
git add ChocoLight/include/ssr_renderer.h \
        ChocoLight/src/ssr_renderer.cpp \
        ChocoLight/include/render_backend.h \
        ChocoLight/src/render_gl33.cpp \
        ChocoLight/src/hdr_renderer.cpp \
        ChocoLight/src/light_ui.cpp \
        ChocoLight/src/light_graphics.cpp \
        ChocoLight/CMakeLists.txt \
        scripts/smoke/ssr.lua \
        samples/demo_ssr/ \
        "docs/Phase E.9 SSR/"

git commit -m "feat(ssr): Phase E.9 Screen-Space Reflection — high-quality scheme

- Backend: 5 SSR virtual interfaces + GL33 shader (linear ray march 64 step)
- SSRRenderer module: 7 params + autoEnable HDR integration
- HDR pipeline: SSR inserted between SSAO and LensFlare
- Lua API: Light.Graphics.SSR (22 functions)
- smoke: scripts/smoke/ssr.lua (38 checks, all pass)
- demo: samples/demo_ssr/ with metal reflection scene
- docs: full 6A workflow (ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO)

Resolves: Phase E.9 SSR"

git push origin main
```

### 2. CI 验证（6 平台）

```bash
gh run list -L 5
gh run watch <id>
```

预期状态：所有 6 平台 build 成功（与 Phase E.8.x baseline 一致）。

**如失败**：按 `debug.md` 规则 — 先看日志，确定具体错误，最小修复，再推。

---

## P1 可选优化（高价值，下个迭代候选）

### 1. 反射 blur（粗糙度模糊）

**问题**：当前 SSR 反射为单点采样，金属表面看起来过于锐利"硬"，缺少粗糙度模拟。

**方案**：
- 新增 `programSSRBlur` GLSL 程序（separable Gaussian / Kawase）
- `SetBlurEnabled(true)` 时实际生效（当前 API 已保留但 no-op）
- 增加 `SetRoughness(float)` 参数（[0, 1]）控制模糊半径

**预估工作量**：~6 小时（shader + 集成 + smoke）

### 2. iOS / Android GPU profiling

**目标**：验证移动端实际帧时间，确认 32 步 / 16 步配置是否真能达到 60 FPS。

**步骤**：
- 用 Xcode Metal Frame Debugger / Android Studio GPU Inspector
- 1080p 场景 + SSR Enable，记录 SSR pass 耗时
- 标定 `SetMaxSteps` 推荐值（高/中/低三档预设）

### 3. ECS 渲染层 SSR 集成

**目标**：把 SSR 作为可序列化的渲染参数，与 ECS 实体绑定。

**方案**：
- `material.reflective: bool`（决定该实体写不写 G-buffer normal）
- ECS scene config 暴露 `ssr.enabled / ssr.intensity / ssr.maxSteps`
- ECS load 时自动调 `SSR.Enable + SetIntensity` 等

---

## P2 可选增强（低优先，未来研究）

### 1. SSR jitter（反走样）

- 在 ray 起点 + step 加 hash noise 抖动
- 减少 ray miss 边缘的硬锯齿
- 实际效果可能需要 temporal accumulation 才明显

### 2. Fresnel 调制

- 当前反射 = `reflect.rgb * fade * intensity` 直接加性
- 加入 `fresnel = pow(1 - dot(viewN, viewV), 5)` 让擦边角反射更强、正面反射更弱
- 更符合物理 BRDF 直觉

### 3. SSR + Bloom 组合预设

- 添加 `Light.Graphics.PostEffectPreset.cinematic()` / `realistic()` / `arcade()` 等便利函数
- 一次性调好多个后处理模块的参数组合

### 4. 半透明物体反射

- 当前半透明（z=0 / depth=1）不参与 SSR
- 可考虑加 alpha-clipped depth-write 选项

---

## 已知限制（设计选择，非缺陷）

| 限制 | 原因 | 影响 |
|------|------|------|
| 屏外物体不反射 | SSR 本身限制 | 边缘镜子反射缺失（用 cubemap fallback 即可缓解；下个 phase 候选） |
| 半透明物体不被反射 | 不写 depth/normal | 玻璃/水面 SSR 不完整 |
| 法线背向相机（`dot < 0.05`）跳过 | 自反射防御 | 极端切角反射被剔除 |
| 反射 RT 与 HDR 同尺寸 | 高画质方案 | 移动端可能成显存压力（~16 MB @ 1080p RGBA16F） |
| Legacy backend 不支持 | `SupportsSSR()` 默认 false | 旧后端自动 no-op，无 fallback |

---

## 维护性改进建议

### 1. shader 源码外置（可选）

当前 shader 双 profile (GLES3 + GL33) 内嵌为 C++ raw string。可考虑：
- 移到 `assets/shaders/ssr.frag` + `ssr_composite.frag`
- 运行时根据 backend profile 选取 / 预编译
- 代价：增加文件 IO 与打包复杂度；收益：方便 hot reload + 跨平台 spirv-cross

**建议**：暂不动，与 SSAO/Bloom 模式保持一致；如 Phase F 引入 shader hot reload 一起重构。

### 2. SSRRenderer 状态结构 thread-safety

当前 `static State g` 假设单线程渲染。若未来引入多窗口 / 双缓冲渲染线程，需：
- 把 `g` 改为 `unique_ptr<State>` 实例化
- 或加 mutex 保护

**建议**：当前 ChocoLight 单渲染线程，暂保持单例模式。

### 3. shader uniform 数量优化

`FS_SSR` 9 个 uniform（proj/invProj + 5 标量 + maxSteps + 3 texture）。
可合并为 UBO：
- 优点：减少 driver 状态切换
- 缺点：GLES3 UBO 对齐要求严格，移植代价高

**建议**：暂不优化（uniform 数量在合理范围）。

---

## 需要用户支持的事项

### 1. CI build 失败时的人工分析

如果 `gh run watch` 显示某平台失败：
- 用 `gh run view <id> --log-failed` 看具体错误
- 报给我，我会按 `debug.md` 5 阶段流程定位修复

### 2. 移动端实测验证

如果你有 iOS / Android 真机环境：
- 跑 `samples/demo_ssr/main.lua`
- 截图 / 录屏 SSR ON vs OFF 效果差异
- 记录 1080p 下 SSR Enable/Disable 时的 FPS 数字
- 反馈给我用于性能调优文档更新

### 3. 高质量反射场景 demo（可选）

当前 demo 是程序生成 cube，效果一般。如果你想更直观看到反射：
- 提供一个 glTF 模型（金属车 / 抛光地板的房间等）
- 我可改 demo 加载该模型展示反射

### 4. 是否进入 Phase E.10？

Phase E.x 系列建议下一个候选：
- **Phase E.10 SSR Blur**（粗糙度模糊，P1 优化最直接的延续）
- **Phase F 后期效果链**（color grading / vignette / film grain 等）
- **Phase G ECS 渲染整合**（材质系统 + 场景图）

请告诉我你的优先级。

---

## 文档清单（本 phase 完整 6A 留痕）

```
docs/Phase E.9 SSR/
├── ALIGNMENT_PhaseE_9.md       # 阶段 1：需求对齐 + 默认决策
├── CONSENSUS_PhaseE_9.md       # 阶段 1：用户拍板高质量方案
├── DESIGN_PhaseE_9.md          # 阶段 2：架构 + 接口 + shader + Lua API 详细设计
├── TASK_PhaseE_9.md            # 阶段 3：17 原子任务 + 依赖图
├── ACCEPTANCE_PhaseE_9.md      # 阶段 6：验收报告（任务完成度 + 质量评估）
├── FINAL_PhaseE_9.md           # 阶段 6：项目总结（亮点 + 用户操作指引）
└── TODO_PhaseE_9.md            # 本文（剩余事项 + 后续路线图）
```
