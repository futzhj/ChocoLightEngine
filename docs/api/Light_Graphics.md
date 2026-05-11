# Light.Graphics

## `Light.Graphics.Push`

保存当前变换矩阵到栈

### 返回值

`void`

---

## `Light.Graphics.Pop`

从栈恢复上一个变换矩阵

### 返回值

`void`

---

## `Light.Graphics.Translate`

平移变换

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number` | 水平偏移 |
| `y` | `number` | 垂直偏移 |
| `z` | `number?` | 深度偏移 (默认 0) |

### 返回值

`void`

---

## `Light.Graphics.Rotate`

旋转变换

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `angle` | `number` | 旋转角度 (度) |
| `x` | `number?` | 旋转轴 X (默认 0) |
| `y` | `number?` | 旋转轴 Y (默认 0) |
| `z` | `number?` | 旋转轴 Z (默认 1) |

### 返回值

`void`

---

## `Light.Graphics.Scale`

缩放变换

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `sx` | `number` | X 缩放比 |
| `sy` | `number?` | Y 缩放比 (默认同 sx) |
| `sz` | `number?` | Z 缩放比 (默认 1) |

### 返回值

`void`

---

## `Light.Graphics.SetColor`

设置当前绘制颜色

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `r` | `number` | 红色 (0~1) |
| `g` | `number` | 绿色 (0~1) |
| `b` | `number` | 蓝色 (0~1) |
| `a` | `number?` | 透明度 (默认 1) |

### 返回值

`void`

---

## `Light.Graphics.GetColor`

获取当前绘制颜色

### 返回值

`number,number,number,number r,g,b,a`

---

## `Light.Graphics.SetCanvas`

设置当前渲染目标

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `canvas` | `Canvas|nil` | Canvas 离屏画布, nil 恢复默认 |

### 返回值

`void`

---

## `Light.Graphics.GetCanvas`

获取当前渲染目标

### 返回值

`Canvas|nil`

---

## `Light.Graphics.SetScissor`

设置裁剪区域 (无参数时禁用裁剪)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number?` | 裁剪区域左上 X |
| `y` | `number?` | 裁剪区域左上 Y |
| `w` | `number?` | 裁剪区域宽 |
| `h` | `number?` | 裁剪区域高 |

### 返回值

`void`

---

## `Light.Graphics.GetScissor`

获取当前裁剪区域

### 返回值

`number,number,number,number x,y,w,h`

---

## `Light.Graphics.Draw`

绘制纹理/图像到屏幕

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `drawable` | `Image|Canvas` | 可绘制对象 |
| `x` | `number?` | 水平位置 (默认 0) |
| `y` | `number?` | 垂直位置 (默认 0) |
| `z` | `number?` | 深度 (默认 0) |
| `rx` | `number?` | X 旋转 (后续 9 个变换参数可选) |

### 返回值

`void`

### 示例

```lua
local img = Light(Light.Graphics.Image):New("hero.png")
Light.Graphics.Draw(img, 100, 200)
```

---

## `Light.Graphics.DrawQuad`

绘制纹理子区域 (sprite sheet 裁切)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `drawable` | `Image|Canvas` | 可绘制对象 |
| `x` | `number?` | 屏幕位置 X |
| `y` | `number?` | 屏幕位置 Y |
| `z` | `number?` | 深度 |
| `qx` | `number?` | 子区域左上 X |
| `qy` | `number?` | 子区域左上 Y |
| `qw` | `number?` | 子区域宽 |
| `qh` | `number?` | 子区域高 |

### 返回值

`void`

---

## `Light.Graphics.DrawLit`

Phase E.1.5 — 绘制受 2D forward 多光照影响的 sprite（走 `sprite_lit_2d` shader + `Light.Lighting2D` state）

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `image` | `Image|Canvas\|nil` | baseColor 纹理；`nil` 时仅顶点色 |
| `normalMap` | `Image\|nil` | 法线贴图；`nil` 时 shader 用默认 `N=(0,0,1)` 平面光照 |
| `x` | `number?` | 水平位置 (默认 0) |
| `y` | `number?` | 垂直位置 (默认 0) |
| `z` | `number?` | 深度 (默认 0) |
| `rx, ry, rz, sx, sy, sz, ox, oy, oz` | `number?` | 与 `Draw` 一致的 9 个 transform 参数 |

### 返回值

`void`

### 行为

1. `g_render->SupportsLit2D() == false` 时直接 return（Legacy / 无 Lit2D 后端）
2. 主动 `BatchRenderer::Flush()`，保证累积的普通 sprite 先出再画 lit sprite
3. 构造 4 个 `RenderVertex2DLit`（默认 `normal=(0,0,1)`, `tangent=(1,0,0,1)`）
4. 内部调 `RenderBackend::DrawLit2DQuad`：切 program → 绑 texture → 上传 MVP/Model/HasNormalMap → `Lighting2D::UploadToShader` → glDrawElements → 切回默认 2D shader

### 示例

```lua
local hero    = Light(Light.Graphics.Image):New("hero.png")
local hero_n  = Light(Light.Graphics.Image):New("hero_normal.png")
Light.Lighting2D.SetAmbient(0.2, 0.2, 0.2)
Light.Lighting2D.AddPointLight{x=200, y=100, color={r=1,g=0.8,b=0.5}, range=400}
Light.Graphics.DrawLit(hero, hero_n, 150, 200)
```

---

## `Light.Graphics.DrawLitQuad`

Phase E.1.5 — 绘制受光照的 sprite 子区域（sprite sheet 裁切，对应 `DrawQuad` 的 Lit 版本）

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `image` | `Image|Canvas\|nil` | baseColor 纹理 |
| `normalMap` | `Image\|nil` | 法线贴图（可选） |
| `x, y, z` | `number?` | 屏幕位置 + 深度 |
| `qx, qy, qw, qh` | `number?` | 子区域 (默认 0/0/64/64) |

### 返回值

`void`

---

## `Light.Graphics.Print`

文字渲染 (支持 Unicode/CJK)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `text` | `string` | 要渲染的文本 |
| `font` | `Font` | 字体对象 |
| `x` | `number?` | 水平位置 |
| `y` | `number?` | 垂直位置 |
| `z` | `number?` | 深度 |

### 返回值

`void`

### 示例

```lua
local font = Light(Light.Graphics.Font):New("Arial.ttf", 24)
Light.Graphics.Print("Hello World", font, 10, 10)

还原自 sub_1800AA170
支持 Unicode/CJK: UTF-8 解码 + FontGetGlyph 动态字形查询
两遍策略: 第一遍预烘焙所有字形 (glTexImage2D 必须在 glBegin 外),
          第二遍一次性渲染所有四边形
```

---

## `Light.Graphics.Line`

绘制直线

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x1` | `number` | 起点 X |
| `y1` | `number` | 起点 Y |
| `z1` | `number` | 起点 Z |
| `x2` | `number` | 终点 X |
| `y2` | `number` | 终点 Y |
| `z2` | `number` | 终点 Z |

### 返回值

`void`

---

## `Light.Graphics.Triangle`

绘制三角形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框(LineMode) 2=填充(FillMode) |
| `x1` | `number` | 顶点 1 X |
| `y1` | `number` | 顶点 1 Y |
| `z1` | `number` | 顶点 1 Z |

### 返回值

`void`

---

## `Light.Graphics.Rectangle`

绘制矩形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 左上 X |
| `y` | `number` | 左上 Y |
| `z` | `number` | 深度 |
| `w` | `number` | 宽度 |
| `h` | `number` | 高度 |
| `d` | `number?` | 深度尺寸 (默认 0) |

### 返回值

`void`

---

## `Light.Graphics.RoundedRectangle`

绘制圆角矩形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 左上 X |
| `y` | `number` | 左上 Y |
| `z` | `number` | 深度 |
| `w` | `number` | 宽度 |
| `h` | `number` | 高度 |
| `r` | `number?` | 圆角半径 (默认 5) |
| `segments` | `number?` | 圆弧段数 (默认 8) |

### 返回值

`void`

---

## `Light.Graphics.Quad`

绘制任意四边形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |

### 返回值

`void`

---

## `Light.Graphics.Polygon`

绘制多边形 (可变顶点数)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |

### 返回值

`void`

---

## `Light.Graphics.Arc`

绘制圆弧

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 中心 X |
| `y` | `number` | 中心 Y |
| `z` | `number` | 深度 |
| `radius` | `number` | 半径 |
| `startAngle` | `number` | 起始角度 (度) |
| `endAngle` | `number` | 结束角度 (度) |
| `segments` | `number` | 圆弧段数 |

### 返回值

`void`

---

## `Light.Graphics.Circle`

绘制圆形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 中心 X |
| `y` | `number` | 中心 Y |
| `z` | `number` | 深度 |
| `radius` | `number` | 半径 |
| `segments` | `number` | 分段数 |

### 返回值

`void`

---

## `Light.Graphics.DrawSprite`

绘制 WAS 精灵帧

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `spriteData` | `table` | GetSpriteImagesData 返回的精灵数据 |
| `frameIdx` | `number` | 帧索引 (1-based) |
| `x` | `number?` | 屏幕位置 X |
| `y` | `number?` | 屏幕位置 Y |

### 返回值

`void`

### 备注

- 懒加载: 首次绘制时将 pixels 上传为纹理, 缓存在 frame.__texId spriteData.frames[frameIdx] = { x, y, w, h, pixels(userdata) } 懒加载: 首次绘制时将 pixels 上传为 GL 纹理, 缓存在 frame.__texId

---
