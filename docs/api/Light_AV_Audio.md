# Light.AV.Audio

## `Light.AV.Audio.__call`

构造函数, 加载音频文件 (WAV/MP3/FLAC/OGG)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | 音频文件路径 |

### 返回值

`void`

### 示例

```lua
local audio = Light(Light.AV.Audio):New("bgm.mp3")
Light.AV.Play(audio)
```

---
