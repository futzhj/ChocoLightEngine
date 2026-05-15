# Phase F.0.10.5 — Shader uvBounds 完美边界 TODO

> 6A 工作流 · 阶段 6 (Assess) · 待办事项
> 关联: `ACCEPTANCE_PhaseF_0_10_5.md` / `FINAL_PhaseF_0_10_5.md`

---

## 1. 强制 (必须解决)

### 1.1 ⏳ CI 6/6 验证

**状态**: 已 push (commit `47a8161`), 待 GitHub Actions webhook 完成所有平台 build

**期望平台**:
- ubuntu-latest (Linux GL 3.3)
- macos-latest (macOS GL 3.3 / Metal)
- windows-latest (Windows GL 3.3)
- emscripten (Web GLES 3.0)
- iOS (mobile GLES 3.0)
- Android (mobile GLES 3.0)

**用户操作**: 等待 5-10 分钟后, 检查 https://github.com/futzhj/ChocoLightEngine/actions

**异常处理**:
- 若 GLES 平台 shader 编译失败 → 检查 `vec4` / `clamp` GLSL ES 3.00 兼容性
- 若 macOS Metal 转 GLSL 失败 → 检查 GLSL → MSL 转译是否丢 uniform
- 若 Linux 失败 → 检查 GL 3.3 core profile 严格模式

---

## 2. 可选 (建议但非阻塞)

### 2.1 视觉边界验证

**目标**: 在 demo_taa_split2 中实际跑双 player split-screen, 截图对比 F.0.10.4 (无 uvBounds) 与 F.0.10.5 (本 phase) 边界处.

**步骤** (用户本地):
```powershell
cd e:\jinyiNew\Light\lumen-master\build\src\light\Release
.\light.exe ..\..\..\..\..\samples\demo_taa_split2\main.lua
```

**预期**:
- F.0.10.4: split-screen 边界处可能可见 ~1px 色彩串扰 (尤其 Bloom glow / TAA history reproject)
- F.0.10.5: 边界完美隔离, 零泄漏 (理论上)

**建议**: 截图 boundary 区放大对比, 加到 demo README 中

### 2.2 性能 benchmark

**目标**: 验证 ClampUV 的 4 ALU/sample 不会带来 > 1% 帧率回归

**工具**: RenderDoc + frame timer (60 fps target)

**测试场景**:
- demo_taa_split2 (split-screen, 2 region)
- 单 viewport baseline (全屏, 无 region)

**预期**: 性能差 < 1% (clamp 是 native shader 指令, ~0.5 ALU)

### 2.3 SSR / Motion Blur shader 扩展

**目标**: 对 SSR (downsample / blur / temporal) 和 Motion Blur (8-tap radial) shader 也加 uvBounds, 实现 **全后处理 region 完美边界**

**工作量**: ~3-4h (SSR 比较复杂, MB 简单)

**优先级**: 中 (本 phase 已解决 Bloom + TAA 主要泄漏源, SSR/MB 影响较小)

### 2.4 提取 "region-aware shader sampling" 模板

**目标**: 把本 phase 创造的 (uvBounds uniform + ClampUV helper + 0.5 inset) 标准化为可复用模板

**输出**: `docs/templates/shader_uvbounds_template.md`

**用途**: 后续任何 multi-tap 后处理 (DOF / chromatic aberration / glitch) 都可直接套

---

## 3. 用户支持需求

### 3.1 配置 / 环境

- **无新配置**: F.0.10.5 是 shader 内部优化, 不暴露新 API / 不引入新依赖
- **现有 demo 兼容**: demo_taa_split2 自动受益 (region 调用透传 uvBounds)

### 3.2 文档使用指引

| 角色 | 推荐阅读顺序 |
|------|-------------|
| 业务开发者 (用 Bloom/TAA region API) | `FINAL_PhaseF_0_10_5.md` §6 Lua API 演化 |
| 引擎开发者 (扩展 SSR/MB) | `DESIGN_PhaseF_0_10_5.md` + `FINAL_PhaseF_0_10_5.md` §3 关键设计决策 |
| Phase 维护者 (复用模板) | `FINAL_PhaseF_0_10_5.md` §5 模板可复用度 |

### 3.3 调试 / 排查

如发现视觉问题 (例: split-screen 仍有边界 artifact):

```glsl
// 在 shader 内 debug 输出 uvBounds (临时改 FragColor)
FragColor = vec4(uUvBounds.xy, uUvBounds.zw - uUvBounds.xy);
// uMin 在 RG, region 大小在 BA
```

或 CPU 端 log uvBounds:
```cpp
CC::Log(CC::LOG_INFO, "Phase F.0.10.5 uvBounds: %.4f,%.4f - %.4f,%.4f",
        uvBoundsBuf[0], uvBoundsBuf[1], uvBoundsBuf[2], uvBoundsBuf[3]);
```

---

## 4. 后续 Phase 候选

详见 `FINAL_PhaseF_0_10_5.md` §7. 主要候选:

| Phase | 主题 | 预期工作量 | 优先级 |
|-------|------|-----------|--------|
| **F.0.10.6** | HDR multi-instance (每 region 独立 tonemap) | ~6-8h | 中 |
| **F.1** | DLSS-like TAAU (output > input upscale) | ~10-15h | 高 (是 TAA 终极形态) |
| **F.0.11** | Volumetric Fog region 化 | ~4-5h | 低 |
| **F.0.10.7** (新建议) | SSR/MB shader uvBounds (完美完整) | ~3-4h | 中 |

---

## 5. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_5.md` | 需求 / 假设 / 约束 (Phase 启动) |
| `DESIGN_PhaseF_0_10_5.md` | shader 范本 + backend / renderer 算法 + 风险 |
| `TASK_PhaseF_0_10_5.md` | 16 个原子任务 + 依赖 + 工作量 |
| `ACCEPTANCE_PhaseF_0_10_5.md` | 验收记录 + 风险矩阵 + 工作量统计 |
| `FINAL_PhaseF_0_10_5.md` | 项目总结 + 模板可复用度 + 后续候选 |
| **`TODO_PhaseF_0_10_5.md`** | 本文件 (强制 + 可选 + 用户支持) |
