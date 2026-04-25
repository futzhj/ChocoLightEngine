<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 18:27:05
 * @LastEditTime: 2026-04-25 18:37:55
-->
# Light.Graphics.Font

## `Light.Graphics.Font.__call`

构造函数, 加载 TTF 字体

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | 字体文件路径 (.ttf) |
| `size` | `number?` | 字号 (默认 16) |

### 返回值

`void`

### 示例

```lua
local font = Light(Light.Graphics.Font):New("font.ttf", 24)
```

---
