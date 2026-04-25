# Light.Plugins.WDFData

## `Light.Plugins.WDFData.GetData`

按哈希提取原始数据 (不解码)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `hash` | `number` | 文件哈希 |

### 返回值

`userdata,number 原始字节,大小 | nil`

---

## `Light.Plugins.WDFData.GetTGAData`

提取解码后的 TGA 纹理数据

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `hash` | `number` | 文件哈希 |

### 返回值

`userdata,number TGA数据,大小 | nil`

---

## `Light.Plugins.WDFData.GetImageData`

提取解码后的图像数据 (RGBA)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `hash` | `number` | 文件哈希 |

### 返回值

`ImageData|nil`

---

## `Light.Plugins.WDFData.GetAudioData`

提取解码后的音频数据

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `hash` | `number` | 文件哈希 |

### 返回值

`AudioData|nil`

---

## `Light.Plugins.WDFData.GetSpriteImagesData`

提取并解析 WAS 精灵帧数据

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `hash` | `number` | 资源哈希 |

### 返回值

`table|nil { directions, framesPerDir, width, height, keyX, keyY, frames }`

---

## `Light.Plugins.WDFData.__call`

构造函数, 打开 WDF 资源包

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | WDF 文件路径 |

### 返回值

`void`

---
