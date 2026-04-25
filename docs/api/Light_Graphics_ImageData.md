# Light.Graphics.ImageData

## `Light.Graphics.ImageData.GetWidth`

获取数据宽度

### 返回值

`number`

---

## `Light.Graphics.ImageData.GetHeight`

获取数据高度

### 返回值

`number`

---

## `Light.Graphics.ImageData.GetDepth`

获取像素位深/通道数

### 返回值

`number`

---

## `Light.Graphics.ImageData.GetPointer`

获取原始像素数据指针

### 返回值

`lightuserdata,number 指针,字节数`

---

## `Light.Graphics.ImageData.Count`

获取像素总数

### 返回值

`number`

---

## `Light.Graphics.ImageData.__call`

构造函数 (支持 3 种创建方式)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `filename_or_w` | `string|number` | 文件名或宽度 |

### 返回值

`void`

### 备注

- __call(self, filename) — 从文件加载
- __call(self, pointer, size) — 从缓冲区创建
- __call(self, w, h, depth, format) — 指定尺寸创建

---
