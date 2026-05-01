# ChocoLight Examples

ChocoLight 引擎示例集合，演示各模块的实际用法。

## 示例列表

| 示例 | 涉及模块 | 用途 |
|------|---------|------|
| [`snake/`](./snake) | Scene · UI.Widget · Input · HotReload · Graphics | 完整的贪吃蛇小游戏，三场景切换 |
| [`editor/`](./editor) | UI.Widget · Graphics | 简易可视化场景编辑器，支持拖拽/选中/序列化 |

## 运行方式

桌面平台 (Windows / Linux / macOS):

```bash
# 假设已构建好引擎可执行文件
ChocoLight examples/snake/main.lua
ChocoLight examples/editor/main.lua
```

如需在 Android 模拟器/真机运行，请将 `main.lua` 放入 Android 模板的 `assets/` 目录后重新打包 APK。

## 目录约定

```
examples/
├── README.md               # 本文件
├── <demo-name>/
│   ├── main.lua            # 入口脚本
│   ├── README.md           # 示例说明
│   └── assets/             # 可选: 图片/字体/音频
```

## 编写自己的示例

最小骨架：

```lua
---@diagnostic disable: undefined-global
require("Light.UI.Window")
require("Light.Graphics")

local App = Light(Light.UI.Window):New()

function App:OnOpen()      end
function App:Update(dt)    end
function App:Draw()        end
function App:OnKey(...)    end
function App:OnMouseButton(...) end

App:Open(800, 600, "My Demo")
while Light.UI.Loop() do Light.UI.Resume() end
```

更高级的功能（场景栈、UI 控件、热重载）参见 `snake/main.lua` 完整示例。
