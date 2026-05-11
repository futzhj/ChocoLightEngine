# TODO — Phase E.5 · Auto Exposure（交付后未尽事项）

> 6A 工作流 · 阶段 6 · Assess 衍生物
> 列出 Phase E.5 已交付但**仍需用户/后续阶段补完**的事项。

---

## 1. 必要补完（建议下个阶段着手）

### 1.1 真机视觉验收 [可选 - 用户参与]

**问题**：Phase E.5 全部 CI smoke 跑通（API + 参数 clamp + lifecycle + AutoEnable 联动），但没有人眼视觉验收 demo_auto_exposure 在真实 GL ctx 下的画面效果。

**做法**：
1. 本地编译 `Light.dll` 或拿 CI 产物：
   ```powershell
   gh run download <id> --name windows-template
   ```
2. 跑 demo：
   ```powershell
   ./light.exe samples/demo_auto_exposure/main.lua
   ```
3. **关键验收路径**：
   - 启动后 split 模式 → 整体 EV 平衡（左暗 + 右亮平均）
   - 按 `D` 切到 darkOnly：观察 1-3s 内 `CurrentEV` 上升 (~+3 ~ +5)，画面整体变亮
   - 再按 `D` 切到 brightOnly：观察 0.5-1s 内 `CurrentEV` 下降 (~-2)，过曝消退
   - 按 `A` 关 AE：exposure 立即回归 manual (1.0)，亮场景立即过曝
   - 按 `1/2` 调 SpeedUp：观察暗→亮过渡更快/更慢
4. 截图归档（可选）到 `docs/Phase E 渲染管线升级/assets/`

**操作指引**：参见 `samples/demo_auto_exposure/README.md` 完整按键表。

---

### 1.2 Bloom + AE 组合验证 [可选]

**问题**：DESIGN 决定 AE 在 Bloom 之后测量（含 bloom 输出），未测试两者同时启用时的实际表现。

**做法**：
1. demo_auto_exposure 中手动 `Gfx.Bloom.Enable(960, 540)`
2. 切到 darkOnly 模式，观察 bloom 辉光后 `CurrentEV` 上升量是否合理（应略高于纯 dark，因为 bloom 把亮点扩散后整体平均亮度上升）
3. 切到 brightOnly 模式，观察 bloom + AE 联动是否仍稳定（不应出现震荡）

**预期**：Bloom 提高场景平均亮度 → AE 略下调 exposure，达到新平衡点。两者无负反馈震荡。

---

### 1.3 性能基线 [可选]

**问题**：未测 AE 在常见分辨率下的实际开销（含 readback stall）。

**做法**：
1. 启用 demo_auto_exposure + 注入 Time 测帧时间
2. 分别测 AE OFF / AE ON 的平均帧时间和 99% 帧时间
3. 推荐基线（Mid-range GPU, 1920×1080）：
   - OFF: baseline
   - ON: +~100us（含 luma extract + mipmap + readback）

记录到 `docs/Phase E 渲染管线升级/assets/ae_perf.md`（如需追踪）。

---

## 2. 引擎基础设施缺口（与 E.5 强相关）

### 2.1 PBO 异步 readback [中优先]

**问题**：v1 用同步 `glReadPixels` 读 1×1 R16F（实际用 `GL_FLOAT` 4 字节兼容路径），约 10us stall。在高帧率（120+ fps）场景下累计可见。

**建议**：
- 双 PBO ping-pong：第 N 帧发 `glReadPixels` 到 PBO[N%2]，CPU 读 PBO[(N+1)%2]（上一帧结果）
- AE 算法吃 1 帧延迟可接受（视觉上无感）
- 接口签名不变：`ReadbackLuminance1x1` 内部状态机化

**收益**：完全 0 stall。

---

### 2.2 Center-weighted / Spot metering [低优先]

**问题**：v1 全画面 average，HUD/UI 像素污染测量。游戏中常见做法是 center-weighted（中心 80% 权重，边缘 20%）或 spot（屏幕中点 ±5% 范围）。

**建议**：
- 加 `AE.SetMeteringMode("average" | "center" | "spot")` Lua API
- 后端加 1-2 个 luma extract shader 变体（`uMeteringMask` uniform 加权采样）

---

### 2.3 Histogram-based AE [低优先]

**问题**：average 法对极端值（少数极亮像素）敏感；行业最佳实践是 histogram percentile（取 50%-95% 区间均值）。

**前置**：需要 GLES 3.1+ compute shader 或 SSBO atomic counter，目标平台不全支持。

**建议**：留待 Phase F.x（compute pipeline）落地后实现。

---

## 3. 未来扩展候选

### 3.1 Phase E.6 — Lens Dirt + Streak [候选]

- **Lens dirt**：bloom 输出与 dirt 噪点图相乘叠加，模拟镜头脏污
- **Streak**：anamorphic 横向条纹光晕（电影感）

参考 ALIGNMENT_PhaseE_4.md 已有此候选。

---

### 3.2 Phase E.7 — SSAO / SSR [候选]

- **SSAO**：屏幕空间环境光遮蔽（depth-only 重建路径）
- **SSR**：屏幕空间反射

需先决定是否引入轻量 G-buffer。

---

### 3.3 AE 与 ECS 集成 [低优先]

- ECS 相机组件可挂 AE 配置（exposure curve / lock target / metering region）
- 镜头切换自动 reset AE history（避免长 fade 跨场景）

---

## 4. 验证脚本（速查）

```powershell
# CI 状态
gh run list --limit 5 --branch main

# 看某次 run 失败日志
gh run view <id> --log-failed

# 本地 sanity（不编译，仅 Lua 语法 check）
lua -e "loadfile('scripts/smoke/auto_exposure.lua')"
lua -e "loadfile('samples/demo_auto_exposure/main.lua')"

# 提取 AutoExposure 子表函数清单
grep -E '"(Enable|Disable|Is|Resize|Set|Get)" *=' ChocoLight/src/light_graphics.cpp | grep -i ae_
```

---

## 5. 完结清单

- [x] E.5.1 backend（虚接口 + luma shader + R16F mipmap RT + readback）✅ CI 通过
- [x] E.5.2 module（namespace + HDR 联动 + chrono dt）✅ CI 通过
- [x] E.5.3 Lua + smoke + demo（18 函数 + ~51 断言 + 3 场景 demo）✅ 等 CI
- [x] ACCEPTANCE_PhaseE_5.md ✅
- [x] FINAL_PhaseE_5.md ✅
- [x] TODO_PhaseE_5.md ✅（本文件）
- [ ] 真机视觉验收（用户参与）
- [ ] Bloom + AE 组合 visual check（可选）
- [ ] 性能基线（可选）

---

**Phase E.5 主交付完结，无阻塞剩余。** 上面 [ ] 项为锦上添花，不影响后续 phase 启动。

HDR 链路三剑客（HDR + Bloom + AE）全部上线 ✨
