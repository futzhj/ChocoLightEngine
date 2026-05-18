# Phase F.1.4 — TODO / 后续工作清单

> **当前状态**: Phase F.1.4 已交付; 本清单记录 **未完成** 或 **延后** 的事项, 供 Phase F.1.5+ 接续.

---

## 1. 留观察 (不阻塞, 视实际使用决定)

### 1.1 调整事件 history 1 帧 jitter

- **现象**: DRS 触发跳档时, SetRenderScale → applyTAAUChange_ 会重建 HDR FBO, history 当帧失效 (hasHistory=false), 走 cur 路径出现 1 帧 sharp 切换
- **业界对比**: UE5 / FSR2 / DLSS 同此行为 (重投影成本不值得换 1 帧平滑)
- **可选**: 若用户反馈明显, 可加 history 重投影 (从旧 scale UV → 新 scale UV linear sample), 但代价是 1 个额外 pass

### 1.2 avgFrameTimeMs 含整体主循环

- **现象**: 帧时间统计包含 Lua tick / draw / TAA / 后处理 / SwapBuffer, 无法精确归因 GPU 时间
- **影响**: 用户场景下 Lua 逻辑很重时, DRS 可能误判为 GPU 瓶颈而无效降画质
- **解决**: 需配 GPU profiler (NSight / RenderDoc API hook), 留 F.1.5+

### 1.3 windowSize 120 上限

- **现象**: 144Hz/240Hz 高刷下, 1 秒帧数超过 120, 滑动窗口会丢弃尾部
- **影响**: 微小; 用户可调 cooldownFrames 拉长间接补偿
- **可选**: F.1.5+ 把上限改 240 或换 dt-based windowed average

---

## 2. 已知限制 (设计层, 非 bug)

| # | 限制 | 原因 |
|---|------|------|
| 1 | 仅在 taauEnabled=true 时实际调 SetRenderScale | F.1 TAAU 关闭时 renderScale 仅是状态, 不影响实际渲染 |
| 2 | 4 档预设而非连续 scale | 与 F.1 一致; 连续 scale 抖动会引起明显 jitter |
| 3 | 不支持 cross-instance 联动降级 | multi-pip 每个 instance 独立累积; 用户场景如需联动可在 Lua 层组合 |
| 4 | 决策不参考 GPU profiler / pixel quality metric | 等 F.1.5+ 整合 NSight / SMAA 边缘度量 |

---

## 3. Phase F.1.5+ 候选方向

按价值优先级 (高 → 低):

### 3.1 真实 GPU profiler 整合 (高价值, 高工作量)

- **集成 NSight Aftermath / RenderDoc API**: 拆分 CPU / GPU 帧时间, DRS 决策仅用 GPU 时间
- **预估工作量**: 6h+ (含 cross-platform fallback)
- **依赖**: 需 NSight SDK / Vulkan-Tools 集成

### 3.2 ML-based DRS 预测 (中价值, 中工作量)

- **思路**: 不是 reactive (帧时间已发生), 而是 predictive (基于场景复杂度预测)
- **参考**: UE5 Insights 风格 (CPU + GPU + draw call 数 → 神经网络预测帧时间)
- **预估**: 8h+ (含小型 NN 训练 + 数据采集)

### 3.3 Pixel quality metric 反馈闭环 (中价值, 高工作量)

- **思路**: 不仅看 FPS, 还看 SMAA 边缘锐度 / TAA ghost 程度, 综合决策
- **难点**: 边缘度量本身有性能开销 (相当于 1 个 edge pass)
- **预估**: 10h+

### 3.4 Cross-instance 联动 (低价值, 低工作量)

- **场景**: 4 人 split-screen 时, 1 个 instance 降画质会让全屏不一致, 可联动同步
- **预估**: 2h
- **建议**: Lua 层组合即可, C++ 层无需特殊支持

### 3.5 长效统计 (低价值, 低工作量)

- **思路**: 加 percentile (P50/P95/P99) 统计, 让 DRS 决策对 spike 更鲁棒
- **预估**: 1.5h

---

## 4. 配置与环境

### 4.1 用户配置文件

无需额外配置文件; 所有参数通过 Lua API 调整.

### 4.2 编译标志

无新增 #define / CMake 标志; F.1.4 默认编入 (drsEnabled=false 零回归).

### 4.3 环境依赖

无外部 SDK / 第三方库依赖; 完全基于 F.1 已有路径.

---

## 5. 推荐操作指引 (用户向)

### 5.1 最简启用

```lua
-- 必须先启 TAA + TAAU (DRS 仅在 TAAU 启用时实际调整 scale)
Light.Graphics.TAA.SetTAAUEnabled(true)
Light.Graphics.TAA.SetUpscalePreset('quality')  -- 起始档位

-- 启 DRS
Light.Graphics.TAA.SetDynamicEnabled(true)
Light.Graphics.TAA.SetDynamicTarget(60)         -- 60 FPS

-- 每帧调用
function love.update(dt)
    Light.Graphics.TAA.UpdateDRS(dt)
end
```

### 5.2 移动端推荐配置

```lua
Light.Graphics.TAA.SetDynamicTarget(30)        -- 移动端目标 30
Light.Graphics.TAA.SetDynamicConfig({
    windowSize     = 60,    -- 移动端帧时间噪声大, 拉长窗口
    cooldownFrames = 120,   -- 移动端电池敏感, 减少调整频率
    downThreshold  = 1.15,  -- 更宽容 (避免频繁降画质)
    upThreshold    = 0.80,  -- 更激进升画质
})
```

### 5.3 桌面端高刷推荐

```lua
Light.Graphics.TAA.SetDynamicTarget(144)
Light.Graphics.TAA.SetDynamicConfig({
    windowSize     = 30,    -- 144Hz 噪声小, 短窗口快响应
    cooldownFrames = 30,    -- 30 帧 ≈ 0.2s 间隔, 接受频繁调整
    downThreshold  = 1.05,  -- 144Hz 容差更紧
    upThreshold    = 0.90,
})
```

### 5.4 HUD 监控

```lua
function love.draw()
    local s = Light.Graphics.TAA.GetDynamicStats()
    if s.enabled then
        love.graphics.print(string.format(
            "DRS: %.0f fps target / %.0f fps actual / %s",
            s.targetFps, s.avgFps, s.currentPreset))
    end
end
```

---

## 6. 缺失支持 / 当前不实现的项目

无重要缺失项. 所有 CONSENSUS §3 验收标准已满足.

---

## 版本历史

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初版 — Phase F.1.4 交付后整理 |
