# Light.Plugins.NEMData

## `Light.Plugins.NEMData.GetWidth`

获取地图宽度

### 返回值

`number`

---

## `Light.Plugins.NEMData.GetHeight`

获取地图高度

### 返回值

`number`

---

## `Light.Plugins.NEMData.GetDimensions`

获取地图尺寸

### 返回值

`number,number w,h`

---

## `Light.Plugins.NEMData.GetImageData`

获取指定 tile 坐标的图像数据

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `tx` | `number` | tile X 坐标 |
| `ty` | `number` | tile Y 坐标 |

### 返回值

`ImageData|nil`

---

## `Light.Plugins.NEMData.GetMaskImageData`

获取指定 tile 坐标的遮罩图像

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `tx` | `number` | tile X 坐标 |
| `ty` | `number` | tile Y 坐标 |

### 返回值

`ImageData|nil`

---

## `Light.Plugins.NEMData.GetPath`

A* 寻路

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `sx` | `number` | 起点 X |
| `sy` | `number` | 起点 Y |
| `ex` | `number` | 终点 X |
| `ey` | `number` | 终点 Y |

### 返回值

`table 路径点数组 {{x,y}, ...}`

---

## `Light.Plugins.NEMData.IsObstacle`

查询指定位置是否为障碍物

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number` | 地图 X 坐标 |
| `y` | `number` | 地图 Y 坐标 |

### 返回值

`boolean`

---

## `Light.Plugins.NEMData.__call`

构造函数, 加载 NEM 地图文件

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | NEM 文件路径 |

### 返回值

`void`

---
