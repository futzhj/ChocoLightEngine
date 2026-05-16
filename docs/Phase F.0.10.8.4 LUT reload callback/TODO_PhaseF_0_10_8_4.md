# Phase F.0.10.8.4 — LUT Reload Callback TODO

---

## 1. 强制

### 1.1 ⏳ CI 6/6 验证

**等**: GitHub Actions 完成所有平台.

**风险**:
- `luaL_ref` / `lua_pcall` 跨平台 — Lumen 已用 100+ 次 (light_hotreload.cpp 同模式)
- 无新 GPU / GL 改动
- 无新 size_t / cstddef 类问题
- 风险**极低**

**用户操作**: 检查 [GitHub Actions](https://github.com/futzhj/ChocoLightEngine/actions)

---

## 2. 可选

### 2.1 demo live 演示 (推荐 ~30 min)

**目标**: demo_taa_split2 主循环加 PollLUTReloads + cb, 用户改 `warm_red.cube` 看到:
1. 画面立即更新调色
2. console 打印 reload 通知

```lua
HDR.SetLUTReloadCallback(function(path, oldId, newId)
    print('[LUT] reloaded:', path, oldId, '→', newId)
end)

-- 主循环每秒调
local last = 0
function on_frame(now)
    if now - last > 1.0 then
        HDR.PollLUTReloads()
        last = now
    end
end
```

### 2.2 Per-watch callback (~1h)

**目标**: 每个 watched LUT 可有独立 callback.

**API**:
```lua
HDR.WatchLUTWithCallback(path, function(oldId, newId) ... end) → id
```

**实施**: WatchEntry 加 cbRef 字段, PollLUTReloads 内 trigger 时按 entry 取 cb.

**评估**: 当前单全局 cb + Lua user 自己 dispatch by path 已足. 仅大型项目 (10+ watched LUT) 需要.

### 2.3 Reload 失败回调 (~0.5h)

**目标**: 文件正被写时 reload 失败, 通知用户.

**API**:
```lua
HDR.SetLUTReloadFailCallback(function(path, err) ... end)
```

**实施**: PollLUTReloads 内 newId == 0 路径 trigger 第二个 cb.

**评估**: 目前打 log warn 已够; 仅 debug 用. 评估优先级低.

### 2.4 多 callback 列表 (~1h)

**目标**: SetLUTReloadCallback 接受多个 fn (列表).

**API**:
```lua
HDR.AddLUTReloadCallback(fn) → handle
HDR.RemoveLUTReloadCallback(handle)
```

**实施**: s_lutReloadCbRef 改为 vector<int>; trampoline 遍历 invoke.

**评估**: Lua 端可自己 multiplex (listeners 表), 不必重复实现.

---

## 3. 用户支持

### 3.1 callback 内禁忌

回调期间**禁止**:
- 调 `HDR.PollLUTReloads()` (递归无穷)
- 调 `HDR.UnwatchLUT(id)` (迭代器失效)
- 调 `HDR.SetLUTReloadCallback(nil)` (运行中清自己, ref 不一致)

回调期间**可以**:
- 读 `HDR.GetWatchedLUTId(path)` (查最新)
- 调 `HDR.SetGradingLUT(newId, strength)` (切换 active LUT)
- 任何**只读**或**非 LUT 状态修改**操作

### 3.2 错误处理

callback 抛 error → CC::Log warn 记录, 不阻塞后续 reload. Lua 用户可自己加 pcall:

```lua
HDR.SetLUTReloadCallback(function(path, oldId, newId)
    local ok, err = pcall(function()
        my_ui_toast('LUT updated: ' .. path)
    end)
    if not ok then
        print('[LUT cb error]', err)
    end
end)
```

### 3.3 性能

- callback 调用频率 = reload 频率 = 文件变化频率, 通常 < 1 Hz
- 单次 lua_pcall 开销 ~1-5 μs (3 arg + 简单 fn)
- **零开销当无回调**: HasLUTReloadCallback() = false → 不进 trampoline

---

## 4. 文档导航

| 文档 | 用途 |
|------|------|
| `PLAN_PhaseF_0_10_8_4.md` | ALIGN+DESIGN+TASK 合并简化 (8 决策 + 数据流图 + 6 任务) |
| `FINAL_PhaseF_0_10_8_4.md` | 项目总结 + 8 决策落地 + 端到端示例 + 子生态闭环 |
| **`TODO_PhaseF_0_10_8_4.md`** | 本文件 |
