# Phase F.0.7 TAA Compare Demo — ACCEPTANCE

> 6A 工作流 · 阶段 4 (Approve) + 阶段 6 (Assess) 合并
> 关联：`PLAN_PhaseF_0_7.md` / `FINAL_PhaseF_0_7.md` / `TODO_PhaseF_0_7.md`
> 基线：F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 master TAA pipeline (commit `103bc07`)

---

## 1. 任务完整性

| 维度 | 计划 | 实际 | 状态 |
|------|------|------|------|
| demo 实现 (`samples/demo_taa_compare/main.lua`) | 8 preset 切换 + HUD + 高对比场景 | ~480 行 demo, 1-8 preset 切换 + HUD 含 stabilization 进度 + 旋转 cube + 8 薄棒 + 黑底 plane | ✅ |
| README (`samples/demo_taa_compare/README.md`) | 8 preset 表 + 控制键 + 推荐观察顺序 | 含 8 preset 表 + 推荐观察 8 步 + HUD 字段说明 + 故障排查 | ✅ |
| 6A 文档 (`docs/Phase F.0.7 TAA Compare Demo/`) | PLAN/ACCEPTANCE/FINAL/TODO 4 文件 | 4 文件齐 | ✅ |
| Lua 语法验证 | `lightc -p main.lua` Exit 0 | Exit 0 | ✅ |
| 代码改动 | 零 (纯 demo Phase) | 仅新增 `samples/demo_taa_compare/` 目录 + 4 docs | ✅ |
| smoke 改动 | 零 | demo 不入 CI smoke 链 | ✅ |

---

## 2. 决策矩阵对齐验证（5/5）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 split-screen 实现方式 = 单 viewport 预设切换 | apply_preset() 切换 TAA 全局状态; 不动 viewport 渲染 | ✅ |
| D2 切换时 history 处理 = 切配置 + frame counter 显示稳定进度 | stabilizeCounter 0→30 + HUD 进度条 + 自然 alpha=0.92 收敛 | ✅ |
| D3 场景内容 = 旋转 specular 球 + 细薄物 | 中央 1.2m 金色旋转 cube + 8 根 4cm 厚薄棒围绕 | ✅ |
| D4 preset 数量与映射 = 8 个数字键 (1-8) | for i=1,8 do keyTap(tostring(i)) → apply_preset(i) | ✅ |
| D5 demo 是否进 CI = 仅 lightc -p 语法检查 | 不动 build-templates.yml; CI 通过 lightc -p 自动覆盖（Phase F.0.5 commit 已添加 demo lua 检查路径） | ✅ |

---

## 3. 验收清单

### T1 demo 实现
- [x] 启动后默认 preset = 1 (OFF, baseline 让用户先看 raw aliasing)
- [x] 数字键 1-8 立即切换 preset (apply_preset 内 print 日志 + reset stabilizeCounter)
- [x] preset 表 8 项渐进式叠加 (OFF → F.0 → F.0.1 → F.0.2 → F.0.3 → F.0.4 → F.0.5 → ALL)
- [x] HUD 显示 preset 索引 + 名 + algorithm 说明
- [x] HUD 显示 TAA 实时参数 (alpha / clipMode / sharpness / antiFlicker / halfRes)
- [x] HUD 显示 history stabilization 进度条 (30 帧)
- [x] HUD 显示 frame counter
- [x] HUD 显示 Keys 帮助行
- [x] R 键重置 history (Disable + apply_preset(currentPreset))
- [x] ESC 退出
- [x] 反向清理 (TAA.Disable / HDR.Disable / mesh:Delete)

### T1 场景设计
- [x] 中央旋转金色 cube (1.2m, 自转 30°/s, HDR 高光 → firefly 触发)
- [x] 8 根围绕薄棒 (4cm 厚 × 1.2m 高, 公转 60°/s, 彩虹色 → aliasing/trail 触发)
- [x] 黑色 plane (12×12, RGB=0.04 → 反衬 HDR 高光)
- [x] HDR 强光 (intensity=5, 暖白)
- [x] 静态相机 (eye=(0, 2.2, 5), at=(0, 0.6, 0), 60° FOV)

### T1 防御性编程
- [x] safe_require Light.Graphics / Light.UI / Light.Time
- [x] HDR + TAA subtable 缺失 → headless 退出
- [x] Window.Open 失败 → headless 退出 (pcall guard)
- [x] Gfx.Mesh.New 不存在 → 优雅退出
- [x] TAA.Enable 失败 → preset abort + log
- [x] HUD 字段防御 (TAA.GetXXX and TAA.GetXXX() or default)

### T2 文档
- [x] PLAN_PhaseF_0_7.md (决策矩阵 + 8 preset 表 + 场景设计 + 实施顺序)
- [x] ACCEPTANCE_PhaseF_0_7.md (本文件)
- [x] FINAL_PhaseF_0_7.md (下一文件)
- [x] TODO_PhaseF_0_7.md (下一文件)
- [x] README.md (启动 + 8 preset 表 + 控制键 + 推荐观察顺序 + HUD 字段说明)

### T3 CI
- [x] `lightc -p main.lua` Exit 0
- [ ] GitHub Actions 6/6 平台 success (待回填)
- [ ] CI 状态回填 ACCEPTANCE + FINAL + TODO (待回填)

---

## 4. 关键技术决策

### 4.1 为什么不做真正 split-screen 双 viewport？

**TAA 单实例约束**：
- TAARenderer 是 namespace 模式（单一全局 state）
- history RT 是单一 ping-pong 双 buffer
- 双 viewport 双 TAA 实例需要 TAARenderer 重构为 instance class（~6h，超工作量）
- 任意 split-screen 都会让 TAA history pixel 边界不连续，引入 seam artifact

**单 viewport 预设切换的等效价值**：
- 切 preset 后 ~30 帧 history 自然替换（alpha=0.92 收敛）
- HUD 进度条提示用户"等稳定后再对比"
- 渐进式 8 preset 让用户看到每个 Phase 边际贡献（比 split-screen 双对比更全面）

### 4.2 为什么是 8 个 preset？

```
1 = OFF (baseline, 看 raw aliasing)
2 = F.0 base (jitter + reproject + RGB clip)
3 = F.0.1 sharpening (+ unsharp mask)
4 = F.0.2 YCoCg (+ 色彩鲁棒)
5 = F.0.3 variance (+ Salvi clip)
6 = F.0.4 anti-flicker (+ Karis)
7 = F.0.5 half-res (+ VRAM 优化)
8 = ALL (推荐配置, sharp=0.8 弥补 halfRes 模糊)
```

数字键 1-8 直接对应，键盘布局直观；与 demo_ssr 数字键习惯一致。

### 4.3 stabilization 进度条的必要性

**没有进度条会怎样？**：用户切 preset 后立即看画面，但 TAA history 还残留旧配置（如 RGB clip → YCoCg clip 切换瞬间），画面会出现"过渡 ghosting"，让用户误以为新 preset 画质差。

**有进度条**：用户清楚等 30 帧再观察，避免误判。

### 4.4 ASCII-only HUD 限制

ChocoLight 的 `win:DrawText` 字体可能不包含 γ / σ / × 等 Unicode 符号，HUD 内 fallback：
- γ → `g` (variance gamma)
- σ → `sigma`
- × → `x`

这是 ChocoLight bitmap font 的硬约束（与 demo_ssr / demo_hdr 一致）。

---

## 5. 已知限制 / 未来候选

1. **不是真 split-screen**：TAA 单实例约束导致；如需真左右对比，需 Phase F.0.10 双 TAA 实例化（~6h）
2. **桌面 1080p 看不出 VRAM 节省**：preset 7 / 8 的 VRAM -75% 是 4K 才显著；HUD 仅文字描述
3. **bar 厚度 4cm 在 1080p 是 ~1.5px**：实际 1px 测试需 0.025m 厚度，但 1px 在桌面屏看不清；当前 4cm 是折衷值
4. **未做 perceptual A/B 对比工具**：FLIP / SSIM 自动量化；留 Phase F.0.10
5. **没有 frame-by-frame 截图功能**：用户需自行录屏对比；留 Phase F.0.11

---

## 6. CI 状态（待回填）

| 平台 | 状态 | 状态详情 |
|------|------|---------|
| build-windows | ⏳ | demo lua 通过 lightc -p 语法检查（CI 已包含 samples/*/main.lua 路径自动检查） |
| build-linux | ⏳ | 纯构建 |
| build-macos | ⏳ | 纯构建 |
| build-android | ⏳ | 纯构建 |
| build-ios | ⏳ | 纯构建 |
| build-web | ⏳ | Emscripten WASM |

GitHub Run ID: `<pending>`
Commit hash: `<pending>`
Total duration: `<pending>`
Date: `<pending>`
