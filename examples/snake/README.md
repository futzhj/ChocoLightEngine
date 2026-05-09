# 贪吃蛇 Demo

ChocoLight Phase 3 综合演示游戏。

## 涉及模块

| 模块 | 用途 |
|------|------|
| `Light.Scene` | Menu / Play / GameOver 三场景切换 |
| `Light.UI.Widget` | Button / Label / Panel / Container 控件 |
| `Light.Input` | 键盘按下检测 + 方向控制 |
| `Light.HotReload` | `main.lua` 修改后自动重载 |
| `Light.Graphics` | 矩形/网格/文本绘制 |

## 运行

```bash
# 桌面 (假设已构建 ChocoLight.exe)
ChocoLight.exe examples/snake/main.lua
```

## 操作

| 按键 | 动作 |
|------|------|
| 方向键 / WASD | 移动 |
| R | 重新开始当前对局 |
| ESC | 返回主菜单 |
| 鼠标 | 点击菜单按钮 |

## 代码亮点

### 场景栈用法

```lua
Scene.Push(MenuScene.New())     -- 入栈
Scene.Replace(PlayScene.New())  -- 替换栈顶
Scene.Pop()                     -- 弹栈
```

`GameOverScene` 用 `Push` 而非 `Replace`，让死亡画面"覆盖"在游戏画面上方。

### UI 控件树

```lua
local root = Widget.Container.New(0, 0, W, H)
local btn  = Widget.Button.New(x, y, w, h, "START", {
  OnClick = function() Scene.Replace(PlayScene.New()) end
})
root:AddChild(btn)
```

### 热重载

```lua
Hot.Watch("examples/snake/main.lua", function(path)
  Scene.Clear()
  dofile(path)
end)
```

修改并保存 `main.lua`，0.5 秒内场景自动重置 + 重新加载脚本。
