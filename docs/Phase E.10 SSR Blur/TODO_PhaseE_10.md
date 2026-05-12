# Phase E.10 SSR Blur — TODO 待办清单

> **交付后剩余事项**，用户可按优先级直接查阅。
> commit `ac166f5`，CI run [`25719344367`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25719344367)。

---

## ✅ P0 — 已关闭

### T-1. CI run 25719344367 — 已 6/6 green ✅

| Job | 状态 |
|-----|------|
| build-linux | ✅ |
| build-macos | ✅ |
| build-ios | ✅ |
| build-web | ✅ |
| build-android | ✅ |
| build-windows（含 runtime smoke chain，ssr.lua 49 检查点全过）| ✅ |

run conclusion=success — **Phase E.10 闭环**。

---

## 🟡 P1 — 建议尽快（用户决策）

### T-2. 真机 GLES3 验证（移动端）

**为什么需要**：
- 目前 CI 只做 Android cross-compile（build 绿 ≠ runtime 可用）
- half-res RT 在 Mali G71 / Adreno 3xx 等老 GPU 上 `glBlitFramebuffer` 可能慢
- ping-pong FBO 切换成本在 tile-based GPU 上需实测

**建议设备清单**（任选一类即可）：
- iOS：iPhone 11 / iPad 6 以上（A13+ GPU）
- Android：Pixel 4+ 或 Samsung S10+（Adreno 640 / Mali G76）

**操作指引**：
1. 用 `templates/android-sdl3/` 打包 demo_ssr
2. 真机运行，打开 B 键，观察：
   - Blur 是否可见（不崩溃）
   - FPS 掉幅是否 < 5%（60 → 57 fps 可接受）
   - GPU 温度是否在 15min 内稳定

**需要**：你的测试机型

---

### T-3. HDR 在实机启动后 SSR 联动验证

**为什么需要**：
- 当前 smoke 全 headless（`IsSupported=false`）
- autoEnable 联动路径未走真实 GL context

**操作指引**（有显示器的 Windows/macOS/Linux）：
```bash
light samples/demo_ssr/main.lua
```
观察：
1. HDR on 时，`SSR.GetAutoEnable()` 是否生效
2. `SSR.IsEnabled()` 变 true 后按 B 切换 blur 可见
3. 按 0 多次 → 反射越来越糊
4. 按 R → 所有参数复位（含 BlurEnabled=false, BlurRadius=1.5）

**需要**：你有显示器 + GL 3.3+ 驱动的机器

---

## 🟢 P2 — 优化建议（非阻塞）

### T-4. blur quality preset API（低/中/高 3/5/7 tap）

**价值**：
- 低端设备可用 3-tap 更省 GPU（~0.15 ms saving）
- 高端 desktop 可用 7-tap 更柔和

**工作量**：~半日
- shader 3 个变种
- `SetBlurQuality(enum {LOW, MED, HIGH})` API
- smoke + demo 键位

**Phase**：E.10.x 子任务

---

### T-5. Blur Bilateral（depth-aware）

**价值**：
- half-res 上采样 + blur 可能造成前景 / 背景 leak
- bilateral gate 基于 G-buffer depth 门控

**工作量**：~1 日
- shader FS_SSR_BLUR 加 depth tex sampler + threshold uniform
- 接入 SSAO 的 depth RT（复用）

**Phase**：E.11 候选

---

### T-6. Roughness-aware blur radius

**价值**：
- 真正 PBR：金属镜面几乎不模糊，磨砂表面强模糊
- 每像素 radius（不是全屏统一）

**前提**：
- G-buffer 需要增加 `roughnessTex` RG 或 R channel
- 这是一个较大的 pipeline 改造，非本 phase 范围

**Phase**：E.12+ 候选

---

### T-7. Temporal SSR（TAA-like 累积）

**价值**：
- 大幅减少 noise，可以降低 `MaxSteps` 到 32 而不损质量

**前提**：
- 需要 previous-frame history buffer
- 摄像机 jitter + re-projection 逻辑

**Phase**：E.13 候选（大改造）

---

## ⚪ P3 — 文档 / 运维（可选）

### T-8. API_REFERENCE SSR 速查卡完整示例

**为什么**：
- 当前只有 API 列表，缺完整 lua 代码示例
- 开发者 copy-paste 友好度有提升空间

**建议增补**：
```lua
-- 完整示例：金属反射地面
local SSR = Light.Graphics.SSR
local HDR = Light.Graphics.HDR

HDR.Enable(1920, 1080)
SSR.Enable(1920, 1080)
SSR.SetBlurEnabled(true)
SSR.SetBlurRadius(2.0)   -- 半粗糙金属
SSR.SetIntensity(0.7)

-- 运行循环...

SSR.Disable()
HDR.Disable()
```

**工作量**：~1 小时

---

### T-9. 性能 profiling log（GPU time 自动统计）

**为什么**：
- 目前 `docs/Phase E.10 SSR Blur/CONSENSUS_*.md` 里 0.3 ms 是纸面估算
- 缺自动化 profile 证据

**建议**：
- 用 RenderDoc 或 Nsight Graphics 录屏一段
- 截图 GPU timeline 各 pass 耗时
- 更新 ACCEPTANCE / FINAL 文档实测段

**工作量**：~半日

---

## 📋 当前文件清单（6A 文档完整）

```
docs/Phase E.10 SSR Blur/
  ├── ALIGNMENT_PhaseE_10.md      ✅ 已交付
  ├── CONSENSUS_PhaseE_10.md      ✅ 已交付
  ├── DESIGN_PhaseE_10.md         ✅ 已交付
  ├── TASK_PhaseE_10.md           ✅ 已交付
  ├── ACCEPTANCE_PhaseE_10.md     ✅ 已交付
  ├── FINAL_PhaseE_10.md          ✅ 已交付
  └── TODO_PhaseE_10.md           ✅ 本文件
```

---

## 🔧 已知缺失配置 / 依赖

**无** — Phase E.10 不引入任何新依赖 / 外部配置。

- ❌ 无新增 `.env` / API key
- ❌ 无第三方库（复用 Phase E.9 SSR 和 Phase E.4 Bloom 基础）
- ❌ 无 CMake 变更
- ❌ 无字体 / texture asset 依赖

---

## ⚡ 用户快速响应区

### 如果你想立即继续：

**推荐 A**：启动 **Phase E.11 Bilateral Blur**（1 天）
- 依赖：本 Phase 已完成（`ac166f5`）
- 价值：消除 half-res 边缘 leak
- 风险：低

**推荐 B**：启动 **Phase F（新主题）**
- 依赖：Phase E.x 整个 PostFX 链成熟
- 候选：Light.Path（A* / NavMesh）、Light.UI 2.0（IMGUI）、Light.Asset（热重载）

**推荐 C**：做 **性能调优 / 文档完善**（T-9 + T-8）
- 依赖：无
- 价值：工程化成熟度提升
- 工作量：~1 日

### 如果你想停在此处：

Phase E.10 **已完整交付**，可作为稳定里程碑：
- 6A 文档齐全
- CI 等 green（等监控结果）
- 不影响现有 Lua 代码

### 如果你想回退：

```powershell
git revert ac166f5
git push origin main
```

将回退 Phase E.10 SSR Blur（保留 Phase E.9 SSR 基线）。

---

> **询问待决**：你希望我接下来走哪条路径（A / B / C / 等 CI / 停）？
