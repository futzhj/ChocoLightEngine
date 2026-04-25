# Light.Graphics.Image

## `Light.Graphics.Image.GetWidth`

获取图像宽度

### 返回值

`number 宽度(像素)`

---

## `Light.Graphics.Image.GetHeight`

获取图像高度

### 返回值

`number 高度(像素)`

---

## `Light.Graphics.Image.GetDepth`

获取图像通道数

### 返回值

`number 通道数(3=RGB,4=RGBA)`

---

## `Light.Graphics.Image.GetDimensions`

获取图像尺寸和通道数

### 返回值

`number,number,number w,h,depth`

---

## `Light.Graphics.Image.__call`

构造函数, 从文件加载图像

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | 图像文件路径 (PNG/JPG/BMP/TGA) |

### 返回值

`void`

### 示例

```lua
local img = Light(Light.Graphics.Image):New("hero.png")
```

---
