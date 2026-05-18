# demo_hot_reload — Phase G.0 Lua 脚本热重载演示

演示 `Light.Reload` 三大能力：模块自动重载、状态保留、错误恢复。

## 启动

```powershell
e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe `
  e:\jinyiNew\Light\samples\demo_hot_reload\main.lua
```

启动后看到一个青色旋转方块。

## 试一试

### 1. 改颜色 (实时生效)
打开 `game_logic.lua`, 找到:
```lua
M.COLOR = {0.2, 0.8, 1.0}    -- 青色
```
改成 `{1.0, 0.3, 0.3}` (红色) → 保存 → **方块立即变红**。

### 2. 改速度 (state 保留)
改:
```lua
M.SPIN = 1.0
```
为 `3.0` → 保存 → 方块旋转加速到 3 倍。
注意 HUD 上 `frame=` 和 `angle=` 数值**没有归零**，证明 state 保留生效。

### 3. 故意写错 (错误恢复)
改 `M.COLOR = {1.0, 0.3` (缺右括号 + 缺第三分量) → 保存：
- 控制台打印 `[RELOAD ERR] ... -- ...`
- 方块仍按**旧逻辑** (青色) 继续转, demo 不挂

修正错误 → 保存 → 自动恢复到新值。

### 4. 手动操作
- 按 `R` — 手动调 `Reload.Module('game_logic')`
- 按 `S` — 打印 `Reload.Stats()` (modules_reloaded / files_reloaded / errors / preserved_count)
- 按 `ESC` — 退出 demo

## 代码结构

```
samples/demo_hot_reload/
├── main.lua         主入口 (固定结构, 一般不改)
└── game_logic.lua   用户改这个文件演示热重载
```

`main.lua` 关键点:
1. **`Reload.WatchModule('game_logic')`** — 注册自动 reload
2. **`Reload.Preserve('demo_state', factory)`** — state 跨 reload 保留
3. **`pcall(require, 'game_logic')`** — 间接调用, 每帧拿最新版
4. **`Reload.SetErrorHandler(fn)`** — 全局错误 hook, 不挂 demo
5. **`Light.HotReload.Check(dt)`** — 主循环每帧轮询 mtime

## 学习要点

要让自己的 demo 支持热重载, 把游戏逻辑拆出来:
- **不可热重载**: `Game:Open(...)`, 资源创建, 主循环 `while UI.Loop()`
- **可热重载**: 渲染函数, 配置常量, 业务逻辑模块

用 `require('your_module').fn(...)` 的间接调用模式, reload 后自动取最新版。

## 已知限制

- **不重载主入口**: main.lua 本身 (含 `while UI.Loop()` 行) 改了不生效, 要重启或调 `Reload.RestartScript()`
- **状态保留是表共享**: 修改 game_logic.lua 不能改 `state` 的字段语义 (会兼容性问题), 推荐 state 只存简单字段
- **闭包不更新**: 你在主 demo 里 `local fn = require('m').f` 一次取走 fn, reload 后 fn 仍是旧版. 用 `require('m').f(...)` 每次取
