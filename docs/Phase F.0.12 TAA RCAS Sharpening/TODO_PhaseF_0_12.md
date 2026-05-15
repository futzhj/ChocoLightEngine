# Phase F.0.12 TAA RCAS Sharpening — TODO

> 6A 工作流 · 阶段 6 · TODO 收尾

---

## 1. 必做

| 任务 | 状态 |
|------|------|
| commit + push 代码到 main | 🔴 待办 |
| 监控 GitHub Actions CI 6/6 success | 🔴 待办 |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 🔴 待办 |

---

## 2. 用户指引

### 2.1 启用 RCAS

```lua
local TAA = Light.Graphics.TAA
TAA.Enable(1280, 720)
TAA.SetSharpness(0.8)               -- RCAS [0, 2], 中高强度
TAA.SetSharpenMode("rcas")          -- FSR2 Robust CAS
```

### 2.2 不同场景建议

| 场景 | sharpness | mode | 说明 |
|------|-----------|------|------|
| LDR 游戏纯画质 | 0.5-1.5 | `unsharp` | 全局均匀锐化，最低开销 |
| HDR 中强锐化 | 0.5-0.8 | `cas` | contrast-adaptive, smooth 区不锁牐 |
| **HDR + TAA noise 明显** | **0.5-1.2** | **`rcas`** | **noise detection 自动跳过 smooth** |
| **HDR 高对比 edges (UI/文字)** | **0.5-1.0** | **`rcas`** | **edge protection 防 ringing** |
| 调试/对比 | 0.5 | demo_ssr Z 键 | 3-cycle 实时切换 |

### 2.3 故障排查

| 现象 | 处理 |
|------|------|
| 切 rcas 后画面无变化 | 检查 sharpness 是否 > 0 (RCAS 仅 sharpness>0 时激活 pass) |
| smooth 区域仍有 noise | 检查 noise threshold 是否过松 (FSR2 标准 1/64, 一般不需调) |
| sharpness=1.5 时过锐 | RCAS [0, 2] 范围比 CAS [0, 1] 宽, 推荐 0.5-1.2 |
| RCAS 比 CAS 慢 ~0.03 ms | 正常 (+10 ALU/px); 若性能敏感可切回 cas |

---

## 3. CI 回填（待 T5 完成后填）

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 4. 候选 Phase

- F.0.10 — TAARenderer 多实例 split-screen demo
- F.0.11 — Demo 截图/录屏
- F.0.13 — Motion-adaptive sharpness (与 F.0.8 motion-adaptive γ 成对)
- F.0.14 — Lanczos-2 25-tap (画质再 ↑5%, +0.07 ms)

---

## 5. 总结

Phase F.0.12 实施完整，**无阻塞性遗留**。主要交付：

- shader: FS_RCAS FSR2 5-tap (noise detection + edge protection)
- backend: programRCAS + DrawTAARCASPass override
- TAARenderer: sharpenMode 三选一 (unsharp/cas/rcas) + Process 3 分支
- Lua API: 0 增量 (sharpenMode 字符串主体扩展, 仍 31 fn)
- smoke: RCAS round-trip + 3 大小写测试 + 三轮循环状态独立 + highlights F.0.12
- demo: Z 键 3-cycle 切换 + 描述 RCAS
- 默认零回归 (sharpenMode='unsharp', F.0.1 行为)
- HDR safe: noise detection 跳过 smooth + edge protection 防 ringing + max(result, 0)

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
