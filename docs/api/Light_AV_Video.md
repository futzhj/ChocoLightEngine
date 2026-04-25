# Light.AV.Video

## `Light.AV.Video.__call`

构造函数, 打开视频文件

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | 视频文件路径 |

### 返回值

`void`

### 示例

```lua
local video = Light(Light.AV.Video):New("intro.mp4")
```

---

## `Light.AV.Video.Update`

解码并更新视频帧 (每帧调用)

### 返回值

`void`

---

## `Light.AV.Video.Draw`

绘制当前视频帧

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number?` | 屏幕位置 X (默认 0) |
| `y` | `number?` | 屏幕位置 Y (默认 0) |
| `w` | `number?` | 绘制宽度 (默认视频宽) |
| `h` | `number?` | 绘制高度 (默认视频高) |

### 返回值

`void`

---

## `Light.AV.Video.IsPlaying`

返回视频是否正在播放

### 返回值

`boolean`

---

## `Light.AV.Video.GetWidth`

获取视频宽度

### 返回值

`number`

---

## `Light.AV.Video.GetHeight`

获取视频高度

### 返回值

`number`

---

## `Light.AV.Video.Stop`

停止视频播放

### 返回值

`void`

---
