# Phase G.0 — Lua 脚本热重载 FINAL

> **交付日期**: 2026-05-18
> **基线**: Phase F.0.11.6.5.A13 (WASAPI audio + mp4 全功能完成)
> **范围**: 用户选定 "深入 + main 热重载 (~6h)"
> **commits**: (待 commit) — Light.Reload 模块 + lumen-master RestartScript + demo_hot_reload + smoke

---

## 一. 目标达成

让用户能在引擎运行时改 `.lua` 文件，无需重启 light.exe，改动立即生效。两层覆盖：

1. **模块级热重载** (Light.Reload.Module / WatchModule): 改 `game_logic.lua` 自动 reload, 主入口 main.lua 不变, demo 不中断
2. **入口级热重启** (Light.Reload.RestartScript): 整个 main.lua 重新走一次 (lumen-master 主循环退出后 dofile)

两种工具用户按场景自选。

---

## 二. 交付内容

### 2.1 新文件

| 文件 | 行数 | 备注 |
|------|------|------|
| `@e:\jinyiNew\Light\ChocoLight\src\light_reload.cpp` | 580 | Light.Reload 12 API + lumen 符号反查 |
| `@e:\jinyiNew\Light\samples\demo_hot_reload\main.lua` | 100 | 旋转方块演示 + 间接调用模式 |
| `@e:\jinyiNew\Light\samples\demo_hot_reload\game_logic.lua` | 45 | 用户改这个文件演示热重载 |
| `@e:\jinyiNew\Light\samples\demo_hot_reload\README.md` | 80 | 操作指南 + 学习要点 |
| `@e:\jinyiNew\Light\scripts\smoke\reload.lua` | 175 | 41 个 smoke 用例 |
| `@e:\jinyiNew\Light\scripts\smoke\reload_restart_e2e.lua` | 55 | RestartScript 端到端验证 |
| `@e:\jinyiNew\Light\docs\Phase G.0 Lua Hot Reload\ALIGNMENT_PhaseG_0.md` | 130 | Align 阶段文档 |
| `@e:\jinyiNew\Light\docs\Phase G.0 Lua Hot Reload\DESIGN_PhaseG_0.md` | 380 | 架构 + API 设计 |
| `@e:\jinyiNew\Light\docs\Phase G.0 Lua Hot Reload\FINAL_PhaseG_0.md` | — | 本文档 |

### 2.2 修改文件

| 文件 | 增量 | 关键改动 |
|------|------|----------|
| `@e:\jinyiNew\Light\ChocoLight\include\light.h` | +4 | `luaopen_Light_Reload` 声明 |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 | 加 light_reload.cpp 到源列表 |
| `@e:\jinyiNew\Light\lumen-master\src\light\light.cpp` | +37 / -2 | `lumen_RequestRestart` + pMain restart 循环 + Light.Reload 注册 |

---

## 三. API 完整清单 (Light.Reload, 12 个)

| API | 签名 | 功能 |
|-----|------|------|
| `Module(name)` | `→ module \| nil, err` | 清 package.loaded[name] + require |
| `File(path)` | `→ any... \| nil, err` | luaL_loadfile + pcall, 返 chunk 返回值 |
| `Preserve(key, factory)` | `→ any` | 状态保留 (registry 持久化) |
| `ResetState(key)` | `→ bool` | 强制下次 Preserve 重新调 factory |
| `WatchModule(name)` | `→ bool` | 集成 HotReload, 自动 reload |
| `UnwatchModule(name)` | `→ bool` | 反向操作, 释放 watcher |
| `SetErrorHandler(fn\|nil)` | `→ —` | 注册 reload 失败 hook |
| `GetLastError()` | `→ {path, msg, time} \| nil` | 查最近失败信息 |
| `Stats()` | `→ {...}` | 计数器: modules/files/errors/preserved/watched |
| `Clear()` | `→ —` | 清所有 preserved + watched + error 信息 |
| `RestartScript(path?)` | `→ bool [, err]` | 请 lumen 重启脚本, 默认 arg[0] |
| `IsRestartPending()` | `→ bool` | 查询 lumen 已收到 restart 请求 |

---

## 四. 关键设计决策

### 4.1 lumen 符号反查机制
**问题**: Light.dll 是被 light.exe 用 LoadLibrary 加载的, **无法在链接期**引用 light.exe 内符号 (LNK2019)。

**解决方案**: 三步法
1. light.exe 端: `__declspec(dllexport)` 修饰 lumen_RequestRestart / IsRestartPending
2. Light.dll 端 (light_reload.cpp): `GetModuleHandleA(nullptr)` 取当前进程主模块, `GetProcAddress` 查导出符号
3. 函数指针懒解析 (s_lumenSymbolsResolved flag): 第一次调用时查找, 之后复用指针

**优点**: 跨平台干净 (mobile/web 函数指针保持 nullptr 即 no-op), 不需要静态链接 light.exe

### 4.2 RestartScript 主循环架构
**原理**: lumen pMain 内的脚本退出 (脚本 return) 不一定意味"程序结束", 可能是"主脚本完成等 restart"。

修改 pMain 加 while 循环 (~10 行):
```cpp
if (script) s->status = handleScript(L, argv, script);   // 执行主脚本
while (s->status == 0 && s_restartPending) {              // 新增: restart 循环
    s_restartPending = false;
    s->status = doFile(L, s_restartTargetPath);
}
```
**注意**: lua_State 跨 restart **复用**, package.loaded / 全局表 / Window 都保留. 用户脚本要自行判断"二次启动"(可用全局 flag).

### 4.3 状态保留 (Preserve) 语义
**决策**: 用 Lua registry 存 state, 按 key 索引, factory **仅在 key 不存在时调用**。

```lua
local state = Reload.Preserve('player', function() return {hp=100, x=0} end)
-- 第 1 次: factory 返 {hp=100, x=0}
-- 之后 reload: state 仍指向同一 table (hp/x 累计变化保留)
-- 用户可 Reload.ResetState('player') 强制下次重新 factory
```

**优点**: state 共享是引用传递, 用户改字段后保留, factory 仅初始化默认值

### 4.4 错误回滚 = "不破坏老 package.loaded"
**决策**: reload 失败时**不清** package.loaded[name] (Lua 内置 require 行为: 失败不缓存)。
- 用户脚本失败 → 老版本继续运行
- 修正后下次 reload 自动恢复
- 失败信息记录到 GetLastError + 调 SetErrorHandler hook

### 4.5 显式空字符串拒绝
**决策**: `RestartScript("")` 报错, **不** fallback 到 arg[0]
- 防止用户误传空串导致 lumen 重启 smoke 进入死循环
- 仅 `RestartScript()` (无参) 或 `RestartScript(nil)` 走 arg[0] 路径

---

## 五. 验证矩阵

### 5.1 Smoke 测试
| 测试 | 数据 | 状态 |
|------|------|------|
| `scripts/smoke/reload.lua` | **41 PASS / 0 FAIL** | ✅ |
| `scripts/smoke/reload_restart_e2e.lua` | run #1 + run #2 → 端到端 PASS | ✅ |
| 全 7 套 smoke 回归 (`screenshot/hdr/mesh_3d/...`) | 全部 0 退化 | ✅ |

### 5.2 编译矩阵
| 平台 | 状态 | 备注 |
|------|------|------|
| Windows Release (Light.dll) | ✅ 零 warning | 完整 GetProcAddress 反查 |
| Windows Release (light.exe) | ✅ 仅 codepage warning | LUMEN_EXPORT dllexport |
| macOS / Linux | ✅ 桩 (visibility default) | lumen_RequestRestart 为本地无效 |
| 移动端 / Web | ✅ 桩 | Light.Reload 仍可用, RestartScript no-op |

### 5.3 demo_hot_reload (待用户交互验收)
**操作流程**:
```powershell
e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe `
  e:\jinyiNew\Light\samples\demo_hot_reload\main.lua
```

**预期**:
1. 启动后看到青色旋转方块
2. 改 `game_logic.lua` 中 `M.COLOR = {1,0,0}` → 保存 → 方块约 0.5s 内变红
3. 改 `M.SPIN = 3.0` → 方块加速 (state.angle 不归零, frame 持续累加)
4. 写 syntax error → 方块仍按旧版转 + 控制台 `[RELOAD ERR]` 日志
5. 修正 error → 自动恢复
6. 按 R 手动 reload, 按 S 看 Stats, ESC 退出

---

## 六. 已知限制

### 6.1 不支持的场景
- **不重载主入口 (main.lua) 渲染逻辑**: main.lua 含 `while UI.Loop()` 主循环, reload 它会破坏当前循环. 用 `RestartScript()` 整体重启 (会重新 `Game:Open(...)` 等)
- **不更新已捕获的闭包**: `local f = require('m').f` 之后 reload m, 局部 `f` 仍指向旧版. 用每帧 `require('m').f(...)` 间接调用模式
- **不迁移 native object**: 用户自己管 GL handle / FFI userdata 生命周期 (Preserve 仅保留 Lua table)
- **不锁文件读写**: 如果文件在写一半时 mtime 变 → loadfile 失败 (语法错误) → 老版本保留 + log; 下次保存完后自动恢复

### 6.2 性能考虑
- **HotReload polling 间隔默认 0.5s**: 用户在 `while UI.Loop()` 内调 `Light.HotReload.Check(dt)`, 内部累加直到 0.5s 才扫描 (避免每帧 stat syscall)
- **Preserve 数量上限**: 256 keys (固定数组). 实际游戏一般 < 10 个 state
- **WatchModule 数量上限**: 128 modules. 一般也够
- **lumen restart 是"硬重启"**: lua_State 复用但脚本从头执行, 比 Module 慢得多

---

## 七. 与之前 Phase 的关系

| Phase | 主题 | 与 G.0 关系 |
|-------|------|------------|
| Phase 3 (HotReload 模块) | 资源 mtime 轮询 + Lua callback | **依赖**: WatchModule 内部调 HotReload.Watch |
| F.0.10.8.4 (LUT 热重载) | 资源粒度热重载范例 | **参考**: 错误恢复策略一致 |
| F.0.11.6.5.A13 (WASAPI audio) | mp4 录音 | 独立, 无关 |
| **G.0 (Lua 脚本热重载)** | **本 phase** | 第一个 Lua 语义级热重载 |

---

## 八. 后续优化候选

### G.0.1 — Module 依赖追踪
当前: `Reload.Module('utils')` 只 reload utils 本身, 已 `require('utils')` 的其他模块仍持旧引用.
增强: 反向依赖图 + 自动级联 reload (~50 行).

### G.0.2 — Preserve 兼容性版本
模块改了 state 结构 (新字段 / 类型变化) → 老 state 不兼容崩溃.
增强: `Preserve(key, factory, version)` — version 变化时强制 factory (~10 行).

### G.0.3 — RestartScript 用户态准备 hook
当前: RestartScript 调用后用户必须自己 Close window 才退主循环.
增强: 加 `Reload.SetBeforeRestart(fn)` hook, 自动 Close + 释放资源 (~30 行).

### G.0.4 — 跨进程脚本调试器
LSP-like 协议接受外部编辑器 reload 触发 (而非 mtime 轮询), 立即响应 (~200 行).

---

## 九. 用户操作指南 (quick start)

### 9.1 让自己的 demo 支持热重载

**Step 1**: 把游戏逻辑拆到模块
```lua
-- before (main.lua):
function Game:Draw()
    Gfx.SetColor(1, 0, 0)
    Gfx.Rectangle(...)
end

-- after (main.lua + game.lua):
-- game.lua
local M = {}
function M.draw(window)
    local Gfx = Light.Graphics
    Gfx.SetColor(1, 0, 0)
    Gfx.Rectangle(...)
end
return M

-- main.lua
function Game:Draw()
    require('game').draw(self)   -- 间接调用
end
```

**Step 2**: 注册热重载
```lua
Light.Reload.SetErrorHandler(function(p, e) print('reload err:', p, e) end)
Light.Reload.WatchModule('game')

local poll = 0
while Light.UI.Loop() do
    Light.UI.Resume()
    poll = poll + 0.016
    if poll >= 0.5 then Light.HotReload.Check(poll); poll = 0 end
end
```

**Step 3**: state 保留 (可选)
```lua
local state = Light.Reload.Preserve('game_state', function()
    return { player = {x=0, y=0, hp=100}, score = 0 }
end)
-- reload 后 state 保留, 改 game.lua 不影响 player 位置
```

### 9.2 用 RestartScript 重启整个 demo

```lua
function Game:OnKey(key, ...)
    if key == string.byte('R') then
        self:Close()                          -- 1) 退主循环
        Light.Reload.RestartScript()          -- 2) 请求 lumen restart
    end
end
```

按 R 后: 主循环退出 → main 脚本 return → lumen 检测 pending → dofile 重启 main.lua

---

## 十. 文件清单

### 新增
- `@e:\jinyiNew\Light\ChocoLight\src\light_reload.cpp` (580 行)
- `@e:\jinyiNew\Light\samples\demo_hot_reload\main.lua` (100 行)
- `@e:\jinyiNew\Light\samples\demo_hot_reload\game_logic.lua` (45 行)
- `@e:\jinyiNew\Light\samples\demo_hot_reload\README.md` (80 行)
- `@e:\jinyiNew\Light\scripts\smoke\reload.lua` (175 行)
- `@e:\jinyiNew\Light\scripts\smoke\reload_restart_e2e.lua` (55 行)
- `@e:\jinyiNew\Light\docs\Phase G.0 Lua Hot Reload\ALIGNMENT_PhaseG_0.md`
- `@e:\jinyiNew\Light\docs\Phase G.0 Lua Hot Reload\DESIGN_PhaseG_0.md`
- `@e:\jinyiNew\Light\docs\Phase G.0 Lua Hot Reload\FINAL_PhaseG_0.md` (本文档)

### 修改
- `@e:\jinyiNew\Light\ChocoLight\include\light.h` (luaopen_Light_Reload 声明)
- `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` (新源文件)
- `@e:\jinyiNew\Light\lumen-master\src\light\light.cpp` (lumen_RequestRestart + pMain restart loop)

**总代码量**: ~1080 行 (C++ 615 + Lua 375 + Markdown 数百)
**模块注册**: 当前 lumen 加载 **97 → 98 modules**
