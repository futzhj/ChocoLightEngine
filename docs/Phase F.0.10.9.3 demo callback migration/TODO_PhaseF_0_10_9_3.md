# Phase F.0.10.9.3 — Demo Migration TODO

> 6A · 后续接力. 本 phase 已 ✅ 完成.

---

## ✅ 已完成

- 11 个 immediate-mode demo 全部迁到 OOP callback model:
  hdr, bloom, auto_exposure, ssao, lens_flare, lens_fx, morph_target,
  ssr, taa_compare, taa_split, taa_split2
- 总 LOC 4417 → 2351 (-47%)
- 11 demo 本地真 GL 启动 RUN/Exit ok
- 8 相关 smoke 零回归
- PLAN + FINAL + TODO

## 🟡 待办 — CI 验证

- [ ] commit + push → CI 6 平台
- [ ] 6/6 全绿确认

## 🔵 后续接力

### 1. demo_multi_hdr_pip + demo_taa_split2 真 GL 端到端联跑 (低优, ~30min)

两个最复杂的 demo, 用户可手动按键测试:
- demo_multi_hdr_pip: ESC, L (toggle LUT), E (toggle exp), R (toggle PIP rotate)
- demo_taa_split2: R (reset history), ESC

### 2. samples/_shared/demo_loop.lua helper (中优, ~2h)

11 个 demo 高度重复模板 (require 检测, OnOpen/Update/Draw/OnKey 框架). 提取通用 helper 让未来 demo 写起来轻:

```lua
-- 用法
local Demo = require('samples._shared.demo_loop'){
    title  = 'My Demo',
    width  = 960, height = 540,
    onOpen = function(self) ... end,
    update = function(self, dt) ... end,
    draw   = function(self) ... end,
    onKey  = { [string.byte('R')] = function(self) reset() end, ... },
    cleanup = function() ... end,
}
Demo:Run()
```

不在本 phase 做, 因为有些 demo (taa_split2) 业务太复杂, 用 helper 反而限制.

### 3. demo headless 探针标准化 (低优, ~1h)

11 个 demo 的 headless probe 写法不统一 (有的有 fallback API check, 有的只 print). 可以提取一个 `samples/_shared/headless_probe.lua` 标准化输出格式, 统一 CI smoke 验证模式.

### 4. demo 视觉差异手测脚本 (低优)

写一个工具捕获 11 个 demo 的开屏截图 (前 30 帧), diff 与之前版本的差异. 可作为 CI 视觉回归测试.

## 📚 文档导航

- `PLAN_PhaseF_0_10_9_3.md` — 6A 设计 (11 demo 清单 + 模板 + 映射表)
- `FINAL_PhaseF_0_10_9_3.md` — 实现总结 + lessons
- `TODO_PhaseF_0_10_9_3.md` — 本文
- `../Phase F.0.10.9.2 multi-HDR demo PIP/` — F.0.10.9.2 (PIP demo + HDR.BeginScene/EndScene API)
