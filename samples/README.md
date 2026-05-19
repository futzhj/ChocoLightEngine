# ChocoLight Samples

本目录聚焦 **资源解码（Resource Decode）** 端到端示例：演示 ChocoLight
引擎如何用原生插件直接解 JX3-style 自定义二进制资源（TCP / MAP / WPK / WDF / FLS），
而无需先用第三方 Python 脚本预处理。

## 运行方式

每个示例目录是独立可运行的 sample，结构相同：

```
sample_dir/
  main.lua            # Lua 入口
  assets/             # 测试资源 (按需引入, 部分较大)
  runtime/            # 同步过来的 light.exe + Light.dll (开发期方便)
  run.cmd             # 一键启动脚本 (调 runtime\light.exe main.lua)
```

```sh
# Windows
samples\resource_decode_map_01m\run.cmd

# 或显式调用
.\dist\windows-x64\light.exe samples\resource_decode\main.lua
```

## 样例索引

| 目录 | 用途 | 模块覆盖 |
|------|------|----------|
| `resource_decode/` | TCP + MAP 综合可视化（多 entry 切换浏览） | `Light.Plugins.TCP`, `Light.Plugins.Map` |
| `resource_decode_tcp_sp/` | TCP 单帧 sprite 拆帧浏览（`鸿鸣.tcp`） | `Light.Plugins.TCP` |
| `resource_decode_map_01m/` | MAP 经典 0.1M 分块格式预览（`1001.map` / `1002.map`） | `Light.Plugins.Map` |
| `resource_decode_map_m10/` | MAP 鸿鸣 mx_map 分块 + 多 huffman JPEG 解码（jpgd fallback） | `Light.Plugins.Map` |

## 资源插件 API 速览

```lua
-- 通用 Package (自动 sniff WPK / WDF / FLS magic)
local Pkg = require("Light.Plugins.Package")
local h = Pkg.Open("assets/wzife.wdf")
for _, e in ipairs(h:List()) do
    local data = h:GetData(e.key)   -- 默认自动解压/解密
end
h:Close()

-- TCP / MAP 走专用插件 (含图集 / preview 拼接逻辑)
local TCP = require("Light.Plugins.TCP")
local atlas = TCP.Open(path):DecodeAtlas()  -- {width, height, rgba}

local Map = require("Light.Plugins.Map")
local preview = Map.Open(path):DecodePreview(2048)  -- 等比缩放到最大宽 2048
```

## 设计约定

- **非阻塞退出**：缺资源 / 缺平台支持时 `print` 提示并以 `ok` 状态退出。
- **可视化优先**：每个 sample 渲染解码结果到 `Light.UI.Window`，按数字键切换 entry，`ESC` 退出。
- **简短**：`main.lua` < 150 行，仅演示插件 API，不嵌入完整应用。
