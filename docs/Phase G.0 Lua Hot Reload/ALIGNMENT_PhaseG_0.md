# Phase G.0 — Lua 脚本热重载 ALIGNMENT

> **日期**: 2026-05-18
> **基线**: Phase F.0.11.6.5.A13 (WASAPI audio + mp4 全功能完成)
> **范围**: 用户选定 "深入 + main 热重载 (~6h)"
> **方向决策**: 重新评估范围后调整 — 见 §3

---

## 1. 原始需求

让用户能在 demo 运行时改 `.lua` 文件，无需重启 light.exe，改动立即生效。配合现有 Light.HotReload (mtime watch + callback) 做语义提升。

## 2. 现有基础设施盘点

| 组件 | 文件 | 功能 |
|------|------|------|
| `Light.HotReload.Watch(path, cb)` | `@e:\jinyiNew\Light\ChocoLight\src\light_hotreload.cpp:115-148` | 文件 mtime 轮询, 256 watch 上限 |
| `Light.HotReload.Check(dt)` | `@e:\jinyiNew\Light\ChocoLight\src\light_hotreload.cpp:176-187` | 主循环每帧扫描, 触发 cb |
| `dofile`/`loadfile` | `@e:\jinyiNew\Light\lumen-master\lib\lua\base.cpp:344-351` | Lua 内置, 运行时加载 |
| `require()` + `package.loaded` | Lua 标准 | 模块缓存机制 |
| `g_callbackL` 全局 state | `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp:85` | 跨原生模块共享 lua_State* |
| HDR LUT 热重载 (参考) | `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp:1547-1597` | 业务资源热重载范例 |

## 3. 关键发现 (重新评估范围)

### 3.1 主循环现状
所有 demo 都用 `while Light.UI.Loop() do Light.UI.Resume() end` 模式 (见 `@e:\jinyiNew\Light\samples\demo_taa_split\main.lua:264`)。
- `Light.UI.Loop()` C++ 内驱动事件 + Lua callback
- `Light.UI.Resume()` 调 Lua 注册的 method (`Game:OnRender` / `Game:OnUpdate` 等)

### 3.2 不需要动 lumen-master 主循环的原因
"main 热重载" 的本质需求 = "改了 game logic 后, 主循环执行新版逻辑"。这能通过**间接调用**模式实现:

```lua
-- 用户范式 (推荐):
function Game:OnRender(dt)
    require('game').on_render(self, dt)   -- 间接调用, 每帧从 package.loaded 取最新
end

-- reload 时:
package.loaded['game'] = nil
local ok, err = pcall(require, 'game')   -- 重新 require 自动重建 package.loaded['game']
```

这样**完全不需要改 light.exe 主循环或 lumen-master**, 改 game.lua 后 Reload.Module('game') 即可让下一帧 OnRender 走新代码。

### 3.3 范围调整决策

| 原计划 (6h) | 调整后 (4.5h) | 节省理由 |
|-------------|---------------|---------|
| Light.Reload 模块 (~3h) | Light.Reload 模块 (~2.5h) | 不变 |
| 状态保留 helper (~2h) | 状态保留 helper (~1h) | 实现更简洁 |
| 改 lumen-master 主循环 (~1h) | ❌ 取消 | 间接调用范式无需改 C++ |
| 文档 + 示例 (~0h) | demo_hot_reload 示例 (~1h) | **新增**, 教用户写可热重载代码 |

**最终范围**:
1. `Light.Reload` 原生模块 (~2.5h)
2. `Reload.Preserve` 状态保留 helper (~1h)
3. `samples/demo_hot_reload/` 演示 (~1h)
4. smoke + FINAL doc (~0.5h)

---

## 4. 任务边界

### 4.1 IN-SCOPE

- **`Light.Reload` 原生模块** (~150-200 行 C++):
  - `Module(name)` — 清 `package.loaded[name]` 并 `require(name)`, 返新模块/error
  - `File(path)` — `dofile(path)`, 返结果 / 失败 + 错误
  - `WatchModule(name)` — 集成 HotReload, 文件改动自动 reload module
  - `WatchFile(path, cb)` — 与 HotReload.Watch 相同, 但 dofile 失败时不挂掉
  - `Preserve(key, factory)` — 状态保留: 第一次 factory() 创建, 之后复用同 state
  - `SetErrorHandler(fn)` — 全局 reload 失败 hook (取代 default log)
  - `GetLastError()` — 查最近一次 reload 失败信息
  - `Stats()` — 返 {modules_reloaded, files_reloaded, errors}
- **`samples/demo_hot_reload/main.lua`** (~150 行):
  - 演示模块化 + Reload.Preserve + WatchModule
  - 屏幕显示一个会"在线改色"的旋转方块, 演示 reload 后效果
- **smoke 用例 ~10 个** (`scripts/smoke/reload.lua` 新文件)

### 4.2 OUT-OF-SCOPE

- ❌ 改 `lumen-master/src/light/light.cpp` 主入口 (无必要)
- ❌ Lua function 序列化/反序列化 (太复杂, 无标准方案)
- ❌ Native object (FFI / GL handle) 自动迁移 (用户自己管)
- ❌ 跨平台 file watcher (用现有 stat polling)
- ❌ Webview / Live debugger 集成 (超范围)

---

## 5. 关键决策点

### 5.1 错误时的恢复策略
**决策**: reload 失败时保留**老版本** package.loaded, log + 调 SetErrorHandler
- 老版本继续运行, 不挂 demo
- 用户改对后下次 reload 自动恢复

### 5.2 状态保留语义
**决策**: 用 Lua registry 持 state, 按 key 索引
- 模块写: `local M = {}; local state = Light.Reload.Preserve("game/player", function() return {hp=100, x=0} end); M.state = state; return M`
- 第一次 require: factory() 创建初始 state, registry 记 key→state
- reload 后 require: 找到 registry 中 key 的旧 state, 直接返
- factory 仅在 key 不存在时调用, 之后总是返旧 state (用户可手动 Reload.ResetState(key))

### 5.3 与 HotReload 模块的关系
**决策**: Light.Reload **依赖** Light.HotReload (调 Watch/Check), 不重复实现
- HotReload 负责底层 mtime polling
- Reload 负责语义层 (module/file/state)
- 用户既可单独用 HotReload (灵活), 也可用 Reload (开箱即用)

### 5.4 module name 到 file path 解析
**决策**: 用 Lua `package.path` 查找 (与 require 一致)
- `Reload.WatchModule('game')` 内部走 `package.searchpath('game', package.path)` 解析到 `./game.lua`
- 失败 → 返 false + 错误 (找不到模块文件)

---

## 6. 验收标准

### 6.1 功能验收

- [ ] `Light.Reload.Module('foo')` 能清缓存并重新加载, 返新版 module table
- [ ] `Light.Reload.File('bar.lua')` 能 dofile, 失败时返 nil + err string
- [ ] `Light.Reload.WatchModule('game')` 注册 watcher, 改文件后 1s 内自动 reload (poll interval=0.5s 默认)
- [ ] `Light.Reload.Preserve("k", factory)` 第一次调 factory, 之后返同一 state table
- [ ] `Light.Reload.SetErrorHandler(fn)` 注册后, reload 失败调 fn(path, err)
- [ ] `Light.Reload.GetLastError()` 返最近失败信息 (path, err, timestamp)
- [ ] `Light.Reload.Stats()` 返计数器
- [ ] reload 失败时, **老 package.loaded 不变**, demo 继续跑

### 6.2 demo_hot_reload 验收

- [ ] 启动 demo 显示旋转彩色方块
- [ ] 用户改 `game_logic.lua` 中的颜色常量, 保存后 1s 内方块颜色变化
- [ ] 用户改旋转速度, 保存后立即生效
- [ ] 用户写 syntax error → 方块继续按旧逻辑转 + 日志报错
- [ ] 修正 error → 自动恢复
- [ ] 旋转角度 (state) 在 reload 后**继续从原角度**累加, 不归零

### 6.3 smoke 验收

- [ ] `reload.lua` ~10 用例全 PASS (Module / File / WatchModule / Preserve / 错误处理 / Stats)
- [ ] 全 7 套 smoke 回归不退化 (screenshot 仍 81 PASS, 其他不变)
- [ ] Release 编译零 warning

---

## 7. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| `package.searchpath` 在 lumen-master 中行为不同 | 低 | 中 | 提前测试, 找不到则 fallback "name.lua" |
| Lua state 跨原生模块访问 (g_callbackL) | 低 | 高 | 已验证 (LUT reload 用同机制) |
| Preserve 的 registry ref 内存泄漏 | 中 | 低 | Clear() API + smoke 验证 |
| WatchModule 自动 reload 时 OnRender 正在执行 | 低 | 中 | HotReload.Check 在 OnRender 之外被用户主动调 (用户责任) |

---

## 8. 待用户确认 (Align 阶段问题)

无 — 用户已明确选择"深入 + main 热重载", 但分析后我建议**取消改 lumen-master**, 仅做 Lua 层方案 (节省 1h, 风险降低)。

若用户坚持要改 lumen-master 主入口 (例如支持 `Reload.RestartScript()` 重新 doFile main.lua), 可后续追加, 不影响当前 Light.Reload 模块设计。

**默认按 §3.3 调整后的 4.5h 范围执行**, 进入 Architect 阶段。
