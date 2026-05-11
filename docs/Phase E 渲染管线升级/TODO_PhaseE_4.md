# TODO — Phase E.4 · Bloom 后处理（交付后未尽事项）

> 6A 工作流 · 阶段 6 · Assess 衍生物
> 列出 Phase E.4 已交付但**仍需用户/后续阶段补完**的事项，分**必要补完**与**未来增强**两组。

---

## 1. 必要补完（建议下个阶段着手）

### 1.1 视觉验收 [可选 - 用户参与]

**问题**：Phase E.4 全部 CI smoke 跑通 (API + 参数 clamp + lifecycle + AutoEnable 联动)，但没有人眼视觉验收 demo_bloom 在真实 GL ctx 下的画面效果。

**做法**：
1. 本地编译 `Light.dll`（或拿 CI 产物：`gh run download <id> --name windows-template`）
2. 跑 demo：
   ```
   ./light.exe samples/demo_bloom/main.lua
   ```
3. 按 B 切换 Bloom，预期：
   - **OFF**：8 个亮色块边缘清晰，背景纯黑
   - **ON**：色块四周浮现彩色辉光，强度随 `3/4` 键 Intensity 改变
4. 同样在 demo_hdr 按 B 验证联动：H+B 联动开关 Bloom，OSD 状态实时
5. 截图存档到 `docs/Phase E 渲染管线升级/assets/`（可选）

**操作指引**：参见 `samples/demo_bloom/README.md` 完整按键表。

---

### 1.2 Bloom + Tonemap 组合验证 [可选]

**问题**：4 个 tonemap operator（ACES/Reinhard/Uncharted2/Linear）切换时 bloom 输出在管线上是独立的（先 bloom additive 进 HDR RT，再 tonemap）。理论无影响，未实测。

**做法**：
1. demo_hdr 同时打开 HDR + Bloom
2. T 键循环 4 op，观察亮点辉光形状
3. 预期：辉光"颜色"会因 tonemap 不同而压缩程度不同（Reinhard 偏灰，ACES 偏暖），但**形状/范围**完全一致

**判定**：颜色变化合理 + 范围不变 = 通过。

---

### 1.3 性能基线 [可选]

**问题**：未测 Bloom 在常见分辨率下的实际开销。

**做法**：
1. 启用 demo_bloom + 注入 Time 测帧时间
2. 分别测 Bloom OFF / Bloom ON (levels=2/4/6/8) 的平均帧时间
3. 推荐基线（Mid-range GPU, 1920×1080）：
   - OFF: baseline (e.g. ~3 ms)
   - levels=4: +~1.2 ms
   - levels=5: +~1.5 ms  ← 默认
   - levels=8: +~2.2 ms

记录到 `docs/Phase E 渲染管线升级/assets/bloom_perf.md`（如需追踪）。

---

## 2. 引擎基础设施缺口（与 E.4 强相关）

### 2.1 缺乏 GPU profiler hook [中优先]

**问题**：当前 BloomRenderer 没暴露任何 GPU timing。RenderDoc 可手动抓，但代码内无 `glBeginQuery(TIME_ELAPSED)` 包装。

**建议**：Phase F.x 加 `RenderBackend::BeginGPUTimer(name)` / `EndGPUTimer()` 通用机制，供所有 namespace 模块（Lit/HDR/Bloom）注入。

---

### 2.2 缺少跨平台 framegraph 调试 [低优先]

**问题**：Bloom pyramid 内部状态（每层 fbo/tex id + 尺寸）只在 `CC::Log` info 中出现一次。无法运行期查询。

**建议**：加 `BloomRenderer::DumpPyramidInfo()` 返回字符串，供 demo OSD 显示或 Lua 取出做高级诊断。

---

## 3. 未来扩展候选

### 3.1 Phase E.5 — Lens dirt + Streak + Eye adaptation [候选]

- **Lens dirt**：bloom 与一张 dirt 噪点图相乘叠加，模拟镜头脏污
- **Streak**：anamorphic 横向条纹光晕（电影感）
- **Eye adaptation**：自动测量场景平均亮度，动态调 `HDR.SetExposure`

API 设计参考：`Light.Graphics.LensDirt`, `Light.Graphics.Streak`, `Light.Graphics.AutoExposure`

---

### 3.2 Phase E.6 — SSAO / SSR [候选]

- **SSAO**：屏幕空间环境光遮蔽（依赖 G-buffer 或 depth-only 重建）
- **SSR**：屏幕空间反射

需要先确认引擎 G-buffer 路径（当前 forward）。

---

### 3.3 Compute shader bloom [候选]

GL 4.3 / GLES 3.1+ 用 `glDispatchCompute` 替代 fixed-function blend，理论吐量翻倍且更灵活（变焦 bloom / non-power-of-2 pyramid）。

需引入 `RenderBackend::SupportsCompute()` + 一套 CS shader。

---

## 4. 验证脚本（速查）

```powershell
# CI 状态
gh run list --limit 5 --branch main

# 看某次 run 日志（失败时）
gh run view <id> --log-failed

# 本地 sanity（不编译，仅 Lua 语法 check）
lua -e "loadfile('scripts/smoke/bloom.lua')"
lua -e "loadfile('samples/demo_bloom/main.lua')"

# 提取 Bloom 子表函数清单
grep -E '"(Enable|Disable|Is|Resize|Set|Get)" *=' ChocoLight/src/light_graphics.cpp | grep -i bloom
```

---

## 5. 完结清单

- [x] E.4.1 backend（虚接口 + 3 shader）✅ CI 通过
- [x] E.4.2 module（命名空间 + HDR 联动）✅ CI 通过（fix 后）
- [x] E.4.3 Lua + smoke + demo ✅ CI 通过
- [x] ACCEPTANCE_PhaseE_4.md ✅
- [x] FINAL_PhaseE_4.md ✅
- [x] TODO_PhaseE_4.md ✅（本文件）
- [ ] 真机视觉验收（用户参与）
- [ ] 性能基线（可选）
- [ ] Bloom + Tonemap 组合 visual check（可选）

---

**Phase E.4 主交付完结，无阻塞剩余。** 上面 [ ] 项为锦上添花，不影响后续 phase 启动。
