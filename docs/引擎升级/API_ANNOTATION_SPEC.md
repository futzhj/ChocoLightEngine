# API 标注规范 — ChocoLight Lua API 文档生成

> T4.1 交付物 | 创建日期: 2026-04-25

---

## 一、标注格式

在 C++ 源文件中，每个暴露给 Lua 的 API 函数上方添加结构化注释块：

```cpp
/// @lua_api Light.Graphics.Draw
/// @brief 绘制纹理/图像到屏幕指定位置
/// @param drawable Image|Canvas 可绘制对象
/// @param x number 水平位置
/// @param y number 垂直位置
/// @param z number? 深度位置 (默认 0)
/// @param rx number? X轴旋转角度 (默认 0)
/// @param ry number? Y轴旋转角度 (默认 0)
/// @param rz number? Z轴旋转角度 (默认 0)
/// @param sx number? X缩放 (默认 1)
/// @param sy number? Y缩放 (默认 1)
/// @param sz number? Z缩放 (默认 1)
/// @param ox number? X原点偏移 (默认 0)
/// @param oy number? Y原点偏移 (默认 0)
/// @param oz number? Z原点偏移 (默认 0)
/// @return void
/// @since v0.2.3
/// @example
/// local img = Light(Light.Graphics.Image):New("hero.png")
/// Light.Graphics.Draw(img, 100, 200)
/// Light.Graphics.Draw(img, 100, 200, 0, 0, 0, 45, 2, 2, 1)
```

---

## 二、标注字段说明

| 字段 | 必需 | 格式 | 说明 |
|------|:----:|------|------|
| `@lua_api` | ✅ | `Module.SubModule.FuncName` | 完整 Lua 调用路径 |
| `@brief` | ✅ | 单行文本 | 一句话功能描述 |
| `@param` | ✅ | `name type[?] description` | 参数说明, `?` 表示可选 |
| `@return` | ✅ | `type [description]` | 返回值, 多返回值用多行 |
| `@since` | ❌ | `vX.Y.Z` | 引入版本 |
| `@example` | ✅ | Lua 代码块 (多行) | 使用示例 |
| `@note` | ❌ | 文本 | 补充说明 |
| `@see` | ❌ | `Module.Func` | 相关 API 引用 |
| `@deprecated` | ❌ | `reason` | 废弃标记 |

---

## 三、类型约定

| 类型 | 含义 | 示例 |
|------|------|------|
| `number` | Lua 数值 | `x number 水平位置` |
| `string` | Lua 字符串 | `path string 文件路径` |
| `boolean` | Lua 布尔 | `vsync boolean 垂直同步开关` |
| `table` | Lua 表 | `options table 配置选项` |
| `function` | Lua 函数 | `callback function 回调函数` |
| `userdata` | Lua userdata | `pointer userdata 原始指针` |
| `nil` | 空值 | `nil 无返回` |
| `void` | 无返回值 | `@return void` |
| `Image` | Light.Graphics.Image 实例 | 引擎自定义类型 |
| `Canvas` | Light.Graphics.Canvas 实例 | 引擎自定义类型 |
| `Font` | Light.Graphics.Font 实例 | 引擎自定义类型 |
| `Image\|Canvas` | 联合类型 | 多种可接受类型 |
| `number?` | 可选参数 | 后缀 `?` 表示可省略 |

---

## 四、构造函数标注

使用 `__call` 元方法实现的构造函数:

```cpp
/// @lua_api Light.Graphics.Image
/// @brief 从文件或 ImageData 创建纹理对象
/// @constructor
/// @param source string|ImageData 图片文件路径或 ImageData 对象
/// @return Image 纹理对象
/// @example
/// local img = Light(Light.Graphics.Image):New("assets/hero.png")
/// local w = img:GetWidth()
```

---

## 五、生成输出格式

每个模块生成一个 Markdown 文件: `docs/api/Light.Graphics.md`

```markdown
# Light.Graphics

## Draw

绘制纹理/图像到屏幕指定位置

**签名:**
`Light.Graphics.Draw(drawable, x, y, [z], [rx,ry,rz], [sx,sy,sz], [ox,oy,oz])`

**参数:**

| 参数 | 类型 | 必需 | 说明 |
|------|------|:----:|------|
| drawable | Image\|Canvas | ✅ | 可绘制对象 |
| x | number | ✅ | 水平位置 |
| y | number | ✅ | 垂直位置 |
| z | number | ❌ | 深度位置 (默认 0) |

**返回值:** 无

**示例:**
\```lua
local img = Light(Light.Graphics.Image):New("hero.png")
Light.Graphics.Draw(img, 100, 200)
\```
```

---

## 六、解析规则

1. `@lua_api` 行标记一个 API 块的开始
2. 连续的 `///` 行属于同一个块
3. 遇到非 `///` 行或下一个 `@lua_api` 时块结束
4. `@example` 后的所有 `///` 行（直到块结束）视为代码示例
5. `@param name type? desc` 中, `?` 后缀表示参数可选
6. 多个 `@return` 行表示多返回值
