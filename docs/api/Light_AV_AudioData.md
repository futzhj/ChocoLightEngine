# Light.AV.AudioData

## `Light.AV.AudioData.GetFormat`

获取采样格式

### 返回值

`number`

---

## `Light.AV.AudioData.GetChannels`

获取声道数

### 返回值

`number`

---

## `Light.AV.AudioData.GetSampleRate`

获取采样率

### 返回值

`number`

---

## `Light.AV.AudioData.GetPointer`

获取原始 PCM 数据指针

### 返回值

`lightuserdata,number 指针,字节数`

---

## `Light.AV.AudioData.Count`

获取采样帧数

### 返回值

`number`

---

## `Light.AV.AudioData.__call`

构造函数 (支持 3 种创建方式)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path_or_rate` | `string|number` | 文件路径或采样率 |

### 返回值

`void`

### 备注

- __call(self, filename) — 从文件加载
- __call(self, buffer, size) — 从缓冲区
- __call(self, rate, ch, fmt, count) — 指定参数创建

---
