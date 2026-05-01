# ChocoLight Lua API 参考文档

> 自动从源代码 `/// @lua_api` 注释生成

**API 总数**: 94
**模块数**: 16

## 模块目录

- [Light.AV](#lightav) (3 个 API)
- [Light.AV.Audio](#lightavaudio) (1 个 API)
- [Light.AV.AudioData](#lightavaudiodata) (6 个 API)
- [Light.AV.Video](#lightavvideo) (7 个 API)
- [Light.Crypto](#lightcrypto) (11 个 API)
- [Light.DB.SQLite](#lightdbsqlite) (5 个 API)
- [Light.Graphics](#lightgraphics) (23 个 API)
- [Light.Graphics.Canvas](#lightgraphicscanvas) (1 个 API)
- [Light.Graphics.Font](#lightgraphicsfont) (1 个 API)
- [Light.Graphics.Image](#lightgraphicsimage) (5 个 API)
- [Light.Graphics.ImageData](#lightgraphicsimagedata) (6 个 API)
- [Light.Network](#lightnetwork) (1 个 API)
- [Light.Network.Http](#lightnetworkhttp) (7 个 API)
- [Light.Network.HttpServer](#lightnetworkhttpserver) (3 个 API)
- [Light.Plugins.NEMData](#lightpluginsnemdata) (8 个 API)
- [Light.Plugins.WDFData](#lightpluginswdfdata) (6 个 API)

---

## Light.AV

> 共 3 个 API

**函数列表:**

- [`Pause`](#lightavpause) — 暂停播放
- [`Play`](#lightavplay) — 播放音频
- [`Stop`](#lightavstop) — 停止播放

---

### `Light.AV.Pause`

**暂停播放**

**返回:**

- `void`

<sub>📄 `light_av.cpp:239`</sub>

---

### `Light.AV.Play`

**播放音频**

**返回:**

- `void`

<sub>📄 `light_av.cpp:225`</sub>

---

### `Light.AV.Stop`

**停止播放**

**返回:**

- `void`

<sub>📄 `light_av.cpp:252`</sub>

---

## Light.AV.Audio

> 共 1 个 API

**函数列表:**

- [`__call`](#lightavaudio__call) — 构造函数, 加载音频文件 (WAV/MP3/FLAC/OGG)

---

### `Light.AV.Audio.__call`

**构造函数, 加载音频文件 (WAV/MP3/FLAC/OGG)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | 音频文件路径 | 是 |

**返回:**

- `void`

**示例:**

```lua
local audio = Light(Light.AV.Audio):New("bgm.mp3")
Light.AV.Play(audio)
```

<sub>📄 `light_av.cpp:282`</sub>

---

## Light.AV.AudioData

> 共 6 个 API

**函数列表:**

- [`Count`](#lightavaudiodatacount) — 获取采样帧数
- [`GetChannels`](#lightavaudiodatagetchannels) — 获取声道数
- [`GetFormat`](#lightavaudiodatagetformat) — 获取采样格式
- [`GetPointer`](#lightavaudiodatagetpointer) — 获取原始 PCM 数据指针
- [`GetSampleRate`](#lightavaudiodatagetsamplerate) — 获取采样率
- [`__call`](#lightavaudiodata__call) — 构造函数 (支持 3 种创建方式)

---

### `Light.AV.AudioData.Count`

**获取采样帧数**

**返回:**

- `number`

<sub>📄 `light_av.cpp:389`</sub>

---

### `Light.AV.AudioData.GetChannels`

**获取声道数**

**返回:**

- `number`

<sub>📄 `light_av.cpp:354`</sub>

---

### `Light.AV.AudioData.GetFormat`

**获取采样格式**

**返回:**

- `number`

<sub>📄 `light_av.cpp:344`</sub>

---

### `Light.AV.AudioData.GetPointer`

**获取原始 PCM 数据指针**

**返回:**

- `lightuserdata,number` — 指针,字节数

<sub>📄 `light_av.cpp:374`</sub>

---

### `Light.AV.AudioData.GetSampleRate`

**获取采样率**

**返回:**

- `number`

<sub>📄 `light_av.cpp:364`</sub>

---

### `Light.AV.AudioData.__call`

**构造函数 (支持 3 种创建方式)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path_or_rate` | `string|number` | 文件路径或采样率 | 是 |

**返回:**

- `void`

**说明:**

- __call(self, filename) — 从文件加载
- __call(self, buffer, size) — 从缓冲区
- __call(self, rate, ch, fmt, count) — 指定参数创建

<sub>📄 `light_av.cpp:418`</sub>

---

## Light.AV.Video

> 共 7 个 API

**函数列表:**

- [`Draw`](#lightavvideodraw) — 绘制当前视频帧
- [`GetHeight`](#lightavvideogetheight) — (无描述)
- [`GetWidth`](#lightavvideogetwidth) — (无描述)
- [`IsPlaying`](#lightavvideoisplaying) — (无描述)
- [`Stop`](#lightavvideostop) — (无描述)
- [`Update`](#lightavvideoupdate) — 解码并更新视频帧 (每帧调用)
- [`__call`](#lightavvideo__call) — 构造函数, 打开视频文件

---

### `Light.AV.Video.Draw`

**绘制当前视频帧**

<sub>📄 `light_av.cpp:561`</sub>

---

### `Light.AV.Video.GetHeight`

<sub>📄 `light_av.cpp:590`</sub>

---

### `Light.AV.Video.GetWidth`

<sub>📄 `light_av.cpp:583`</sub>

---

### `Light.AV.Video.IsPlaying`

<sub>📄 `light_av.cpp:575`</sub>

---

### `Light.AV.Video.Stop`

<sub>📄 `light_av.cpp:597`</sub>

---

### `Light.AV.Video.Update`

**解码并更新视频帧 (每帧调用)**

<sub>📄 `light_av.cpp:551`</sub>

---

### `Light.AV.Video.__call`

**构造函数, 打开视频文件**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | 视频文件路径 | 是 |

**返回:**

- `void`

<sub>📄 `light_av.cpp:522`</sub>

---

## Light.Crypto

> 共 11 个 API

**函数列表:**

- [`AES256_Decrypt`](#lightcryptoaes256_decrypt) — AES-256-CBC 解密
- [`AES256_Encrypt`](#lightcryptoaes256_encrypt) — AES-256-CBC 加密 (PKCS7 填充)
- [`Base64Decode`](#lightcryptobase64decode) — Base64 解码
- [`Base64Encode`](#lightcryptobase64encode) — Base64 编码
- [`KeyFromPassword`](#lightcryptokeyfrompassword) — 从密码派生固定长度密钥 (简化 PBKDF: 多轮 SHA-256 链)
- [`MD5`](#lightcryptomd5) — 计算 MD5 哈希 (hex 字符串)
- [`MD5_Raw`](#lightcryptomd5_raw) — 计算 MD5 哈希 (原始 16 字节)
- [`RandomBytes`](#lightcryptorandombytes) — 生成密码学随机字节
- [`RandomHex`](#lightcryptorandomhex) — 生成随机 hex 字符串
- [`SHA256`](#lightcryptosha256) — 计算 SHA-256 哈希 (hex 字符串)
- [`SHA256_Raw`](#lightcryptosha256_raw) — 计算 SHA-256 哈希 (原始 32 字节)

---

### `Light.Crypto.AES256_Decrypt`

**AES-256-CBC 解密**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `ciphertext` | `string` | 密文 | 是 |
| `key` | `string` | 32 字节密钥 | 是 |
| `iv` | `string` | 16 字节初始向量 | 是 |

**返回:**

- `string` — 明文 (失败返回 nil + 错误信息)

<sub>📄 `light_crypto.cpp:198`</sub>

---

### `Light.Crypto.AES256_Encrypt`

**AES-256-CBC 加密 (PKCS7 填充)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `plaintext` | `string` | 明文 | 是 |
| `key` | `string` | 32 字节密钥 | 是 |
| `iv` | `string` | 16 字节初始向量 | 是 |

**返回:**

- `string` — 密文

<sub>📄 `light_crypto.cpp:161`</sub>

---

### `Light.Crypto.Base64Decode`

**Base64 解码**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `b64` | `string` | Base64 字符串 | 是 |

**返回:**

- `string` — 原始字节 (失败返回 nil)

<sub>📄 `light_crypto.cpp:260`</sub>

---

### `Light.Crypto.Base64Encode`

**Base64 编码**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `data` | `string` | 输入字节 | 是 |

**返回:**

- `string` — Base64 字符串

<sub>📄 `light_crypto.cpp:247`</sub>

---

### `Light.Crypto.KeyFromPassword`

**从密码派生固定长度密钥 (简化 PBKDF: 多轮 SHA-256 链)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `password` | `string` | 密码 | 是 |
| `salt` | `string` | 盐值 (避免彩虹表) | 是 |
| `keyLen` | `number` | 输出密钥长度 (1~64) | 是 |
| `iterations` | `number?` | 迭代次数 (默认 10000) | 是 |

**返回:**

- `string` — 派生密钥

<sub>📄 `light_crypto.cpp:317`</sub>

---

### `Light.Crypto.MD5`

**计算 MD5 哈希 (hex 字符串)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `data` | `string` | 输入数据 | 是 |

**返回:**

- `string` — 32 字符 hex 字符串

<sub>📄 `light_crypto.cpp:132`</sub>

---

### `Light.Crypto.MD5_Raw`

**计算 MD5 哈希 (原始 16 字节)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `data` | `string` | 输入数据 | 是 |

**返回:**

- `string` — 16 字节二进制

<sub>📄 `light_crypto.cpp:146`</sub>

---

### `Light.Crypto.RandomBytes`

**生成密码学随机字节**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `n` | `number` | 字节数 (1~4096) | 是 |

**返回:**

- `string` — 随机字节

<sub>📄 `light_crypto.cpp:280`</sub>

---

### `Light.Crypto.RandomHex`

**生成随机 hex 字符串**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `n` | `number` | 字节数 (输出长度 = n*2) | 是 |

**返回:**

- `string` — hex 字符串

<sub>📄 `light_crypto.cpp:297`</sub>

---

### `Light.Crypto.SHA256`

**计算 SHA-256 哈希 (hex 字符串)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `data` | `string` | 输入数据 | 是 |

**返回:**

- `string` — 64 字符 hex 字符串

<sub>📄 `light_crypto.cpp:105`</sub>

---

### `Light.Crypto.SHA256_Raw`

**计算 SHA-256 哈希 (原始 32 字节)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `data` | `string` | 输入数据 | 是 |

**返回:**

- `string` — 32 字节二进制

<sub>📄 `light_crypto.cpp:119`</sub>

---

## Light.DB.SQLite

> 共 5 个 API

**函数列表:**

- [`Blob`](#lightdbsqliteblob) — 创建 X'AABBCC...' 格式 BLOB 字面量
- [`Escape`](#lightdbsqliteescape) — SQL 字符串转义 (单引号双写)
- [`Execute`](#lightdbsqliteexecute) — 执行 SQL 语句
- [`TypeName`](#lightdbsqlitetypename) — Record ORM 字段类型 ID 转 SQL DDL 类型名
- [`__call`](#lightdbsqlite__call) — 构造函数, 打开 SQLite 数据库

---

### `Light.DB.SQLite.Blob`

**创建 X'AABBCC...' 格式 BLOB 字面量**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `data` | `string` | 二进制数据 | 是 |

**返回:**

- `string` — SQL BLOB 字面量

<sub>📄 `light_db.cpp:138`</sub>

---

### `Light.DB.SQLite.Escape`

**SQL 字符串转义 (单引号双写)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `str` | `string` | 要转义的字符串 | 是 |

**返回:**

- `string`

<sub>📄 `light_db.cpp:119`</sub>

---

### `Light.DB.SQLite.Execute`

**执行 SQL 语句**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `sql` | `string` | SQL 语句 | 是 |

**返回:**

- `number,table` — affected_rows, 结果行数组

<sub>📄 `light_db.cpp:80`</sub>

---

### `Light.DB.SQLite.TypeName`

**Record ORM 字段类型 ID 转 SQL DDL 类型名**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `typeId` | `number` | 类型 ID (0=Serial ~ 13=TimeStamp) | 是 |

**返回:**

- `string` — SQL 类型名

<sub>📄 `light_db.cpp:159`</sub>

---

### `Light.DB.SQLite.__call`

**构造函数, 打开 SQLite 数据库**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | 数据库文件路径 | 是 |

**返回:**

- `void`

**示例:**

```lua
local db = Light(Light.DB.SQLite):New("game.db")
```

<sub>📄 `light_db.cpp:201`</sub>

---

## Light.Graphics

> 共 23 个 API

**函数列表:**

- [`Arc`](#lightgraphicsarc) — 绘制圆弧
- [`Circle`](#lightgraphicscircle) — 绘制圆形
- [`Draw`](#lightgraphicsdraw) — 绘制纹理/图像到屏幕
- [`DrawQuad`](#lightgraphicsdrawquad) — 绘制纹理子区域 (sprite sheet 裁切)
- [`DrawSprite`](#lightgraphicsdrawsprite) — 绘制 WAS 精灵帧
- [`GetCanvas`](#lightgraphicsgetcanvas) — 获取当前渲染目标
- [`GetColor`](#lightgraphicsgetcolor) — 获取当前绘制颜色
- [`GetScissor`](#lightgraphicsgetscissor) — 获取当前裁剪区域
- [`Line`](#lightgraphicsline) — 绘制直线
- [`Polygon`](#lightgraphicspolygon) — 绘制多边形 (可变顶点数)
- [`Pop`](#lightgraphicspop) — 从栈恢复上一个变换矩阵
- [`Print`](#lightgraphicsprint) — 文字渲染 (支持 Unicode/CJK)
- [`Push`](#lightgraphicspush) — 保存当前变换矩阵到栈
- [`Quad`](#lightgraphicsquad) — 绘制任意四边形
- [`Rectangle`](#lightgraphicsrectangle) — 绘制矩形
- [`Rotate`](#lightgraphicsrotate) — 旋转变换
- [`RoundedRectangle`](#lightgraphicsroundedrectangle) — 绘制圆角矩形
- [`Scale`](#lightgraphicsscale) — 缩放变换
- [`SetCanvas`](#lightgraphicssetcanvas) — 设置当前渲染目标
- [`SetColor`](#lightgraphicssetcolor) — 设置当前绘制颜色
- [`SetScissor`](#lightgraphicssetscissor) — 设置裁剪区域 (无参数时禁用裁剪)
- [`Translate`](#lightgraphicstranslate) — 平移变换
- [`Triangle`](#lightgraphicstriangle) — 绘制三角形

---

### `Light.Graphics.Arc`

**绘制圆弧**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框 2=填充 | 是 |
| `x` | `number` | 中心 X | 是 |
| `y` | `number` | 中心 Y | 是 |
| `z` | `number` | 深度 | 是 |
| `radius` | `number` | 半径 | 是 |
| `startAngle` | `number` | 起始角度 (度) | 是 |
| `endAngle` | `number` | 结束角度 (度) | 是 |
| `segments` | `number` | 圆弧段数 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:767`</sub>

---

### `Light.Graphics.Circle`

**绘制圆形**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框 2=填充 | 是 |
| `x` | `number` | 中心 X | 是 |
| `y` | `number` | 中心 Y | 是 |
| `z` | `number` | 深度 | 是 |
| `radius` | `number` | 半径 | 是 |
| `segments` | `number` | 分段数 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:812`</sub>

---

### `Light.Graphics.Draw`

**绘制纹理/图像到屏幕**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `drawable` | `Image|Canvas` | 可绘制对象 | 是 |
| `x` | `number?` | 水平位置 (默认 0) | 是 |
| `y` | `number?` | 垂直位置 (默认 0) | 是 |
| `z` | `number?` | 深度 (默认 0) | 是 |
| `rx` | `number?` | X 旋转 (后续 9 个变换参数可选) | 是 |

**返回:**

- `void`

**示例:**

```lua
local img = Light(Light.Graphics.Image):New("hero.png")
Light.Graphics.Draw(img, 100, 200)
```

<sub>📄 `light_graphics.cpp:307`</sub>

---

### `Light.Graphics.DrawQuad`

**绘制纹理子区域 (sprite sheet 裁切)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `drawable` | `Image|Canvas` | 可绘制对象 | 是 |
| `x` | `number?` | 屏幕位置 X | 是 |
| `y` | `number?` | 屏幕位置 Y | 是 |
| `z` | `number?` | 深度 | 是 |
| `qx` | `number?` | 子区域左上 X | 是 |
| `qy` | `number?` | 子区域左上 Y | 是 |
| `qw` | `number?` | 子区域宽 | 是 |
| `qh` | `number?` | 子区域高 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:362`</sub>

---

### `Light.Graphics.DrawSprite`

**绘制 WAS 精灵帧**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `spriteData` | `table` | GetSpriteImagesData 返回的精灵数据 | 是 |
| `frameIdx` | `number` | 帧索引 (1-based) | 是 |
| `x` | `number?` | 屏幕位置 X | 是 |
| `y` | `number?` | 屏幕位置 Y | 是 |

**返回:**

- `void`

**说明:**

- 懒加载: 首次绘制时将 pixels 上传为纹理, 缓存在 frame.__texId
- spriteData.frames[frameIdx] = { x, y, w, h, pixels(userdata) }
- 懒加载: 首次绘制时将 pixels 上传为 GL 纹理, 缓存在 frame.__texId

<sub>📄 `light_graphics.cpp:868`</sub>

---

### `Light.Graphics.GetCanvas`

**获取当前渲染目标**

**返回:**

- `Canvas|nil`

<sub>📄 `light_graphics.cpp:214`</sub>

---

### `Light.Graphics.GetColor`

**获取当前绘制颜色**

**返回:**

- `number,number,number,number` — r,g,b,a

<sub>📄 `light_graphics.cpp:173`</sub>

---

### `Light.Graphics.GetScissor`

**获取当前裁剪区域**

**返回:**

- `number,number,number,number` — x,y,w,h

<sub>📄 `light_graphics.cpp:249`</sub>

---

### `Light.Graphics.Line`

**绘制直线**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `x1` | `number` | 起点 X | 是 |
| `y1` | `number` | 起点 Y | 是 |
| `z1` | `number` | 起点 Z | 是 |
| `x2` | `number` | 终点 X | 是 |
| `y2` | `number` | 终点 Y | 是 |
| `z2` | `number` | 终点 Z | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:548`</sub>

---

### `Light.Graphics.Polygon`

**绘制多边形 (可变顶点数)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框 2=填充 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:737`</sub>

---

### `Light.Graphics.Pop`

**从栈恢复上一个变换矩阵**

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:102`</sub>

---

### `Light.Graphics.Print`

**文字渲染 (支持 Unicode/CJK)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `text` | `string` | 要渲染的文本 | 是 |
| `font` | `Font` | 字体对象 | 是 |
| `x` | `number?` | 水平位置 | 是 |
| `y` | `number?` | 垂直位置 | 是 |
| `z` | `number?` | 深度 | 是 |

**返回:**

- `void`

**示例:**

```lua
local font = Light(Light.Graphics.Font):New("Arial.ttf", 24)
Light.Graphics.Print("Hello World", font, 10, 10)

还原自 sub_1800AA170
支持 Unicode/CJK: UTF-8 解码 + FontGetGlyph 动态字形查询
两遍策略: 第一遍预烘焙所有字形 (glTexImage2D 必须在 glBegin 外),
          第二遍一次性渲染所有四边形
```

<sub>📄 `light_graphics.cpp:441`</sub>

---

### `Light.Graphics.Push`

**保存当前变换矩阵到栈**

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:94`</sub>

---

### `Light.Graphics.Quad`

**绘制任意四边形**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框 2=填充 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:707`</sub>

---

### `Light.Graphics.Rectangle`

**绘制矩形**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框 2=填充 | 是 |
| `x` | `number` | 左上 X | 是 |
| `y` | `number` | 左上 Y | 是 |
| `z` | `number` | 深度 | 是 |
| `w` | `number` | 宽度 | 是 |
| `h` | `number` | 高度 | 是 |
| `d` | `number?` | 深度尺寸 (默认 0) | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:614`</sub>

---

### `Light.Graphics.Rotate`

**旋转变换**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `angle` | `number` | 旋转角度 (度) | 是 |
| `x` | `number?` | 旋转轴 X (默认 0) | 是 |
| `y` | `number?` | 旋转轴 Y (默认 0) | 是 |
| `z` | `number?` | 旋转轴 Z (默认 1) | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:124`</sub>

---

### `Light.Graphics.RoundedRectangle`

**绘制圆角矩形**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框 2=填充 | 是 |
| `x` | `number` | 左上 X | 是 |
| `y` | `number` | 左上 Y | 是 |
| `z` | `number` | 深度 | 是 |
| `w` | `number` | 宽度 | 是 |
| `h` | `number` | 高度 | 是 |
| `r` | `number?` | 圆角半径 (默认 5) | 是 |
| `segments` | `number?` | 圆弧段数 (默认 8) | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:654`</sub>

---

### `Light.Graphics.Scale`

**缩放变换**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `sx` | `number` | X 缩放比 | 是 |
| `sy` | `number?` | Y 缩放比 (默认同 sx) | 是 |
| `sz` | `number?` | Z 缩放比 (默认 1) | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:140`</sub>

---

### `Light.Graphics.SetCanvas`

**设置当前渲染目标**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `canvas` | `Canvas|nil` | Canvas 离屏画布, nil 恢复默认 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:184`</sub>

---

### `Light.Graphics.SetColor`

**设置当前绘制颜色**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `r` | `number` | 红色 (0~1) | 是 |
| `g` | `number` | 绿色 (0~1) | 是 |
| `b` | `number` | 蓝色 (0~1) | 是 |
| `a` | `number?` | 透明度 (默认 1) | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:156`</sub>

---

### `Light.Graphics.SetScissor`

**设置裁剪区域 (无参数时禁用裁剪)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `x` | `number?` | 裁剪区域左上 X | 是 |
| `y` | `number?` | 裁剪区域左上 Y | 是 |
| `w` | `number?` | 裁剪区域宽 | 是 |
| `h` | `number?` | 裁剪区域高 | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:226`</sub>

---

### `Light.Graphics.Translate`

**平移变换**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `x` | `number` | 水平偏移 | 是 |
| `y` | `number` | 垂直偏移 | 是 |
| `z` | `number?` | 深度偏移 (默认 0) | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:110`</sub>

---

### `Light.Graphics.Triangle`

**绘制三角形**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `mode` | `number` | 1=线框(LineMode) 2=填充(FillMode) | 是 |
| `x1` | `number` | 顶点 1 X | 是 |
| `y1` | `number` | 顶点 1 Y | 是 |
| `z1` | `number` | 顶点 1 Z | 是 |

**返回:**

- `void`

<sub>📄 `light_graphics.cpp:582`</sub>

---

## Light.Graphics.Canvas

> 共 1 个 API

**函数列表:**

- [`__call`](#lightgraphicscanvas__call) — 构造函数, 创建离屏画布 (FBO)

---

### `Light.Graphics.Canvas.__call`

**构造函数, 创建离屏画布 (FBO)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `w` | `number` | 画布宽度 | 是 |
| `h` | `number` | 画布高度 | 是 |

**返回:**

- `void`

**示例:**

```lua
local canvas = Light(Light.Graphics.Canvas):New(800, 600)
```

<sub>📄 `light_graphics_canvas.cpp:41`</sub>

---

## Light.Graphics.Font

> 共 1 个 API

**函数列表:**

- [`__call`](#lightgraphicsfont__call) — 构造函数, 加载 TTF 字体

---

### `Light.Graphics.Font.__call`

**构造函数, 加载 TTF 字体**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | 字体文件路径 (.ttf) | 是 |
| `size` | `number?` | 字号 (默认 16) | 是 |

**返回:**

- `void`

**示例:**

```lua
local font = Light(Light.Graphics.Font):New("font.ttf", 24)
```

<sub>📄 `light_graphics_image.cpp:410`</sub>

---

## Light.Graphics.Image

> 共 5 个 API

**函数列表:**

- [`GetDepth`](#lightgraphicsimagegetdepth) — 获取图像通道数
- [`GetDimensions`](#lightgraphicsimagegetdimensions) — 获取图像尺寸和通道数
- [`GetHeight`](#lightgraphicsimagegetheight) — 获取图像高度
- [`GetWidth`](#lightgraphicsimagegetwidth) — 获取图像宽度
- [`__call`](#lightgraphicsimage__call) — 构造函数, 从文件加载图像

---

### `Light.Graphics.Image.GetDepth`

**获取图像通道数**

**返回:**

- `number` — 通道数(3=RGB,4=RGBA)

<sub>📄 `light_graphics_image.cpp:79`</sub>

---

### `Light.Graphics.Image.GetDimensions`

**获取图像尺寸和通道数**

**返回:**

- `number,number,number` — w,h,depth

<sub>📄 `light_graphics_image.cpp:89`</sub>

---

### `Light.Graphics.Image.GetHeight`

**获取图像高度**

**返回:**

- `number` — 高度(像素)

<sub>📄 `light_graphics_image.cpp:69`</sub>

---

### `Light.Graphics.Image.GetWidth`

**获取图像宽度**

**返回:**

- `number` — 宽度(像素)

<sub>📄 `light_graphics_image.cpp:59`</sub>

---

### `Light.Graphics.Image.__call`

**构造函数, 从文件加载图像**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | 图像文件路径 (PNG/JPG/BMP/TGA) | 是 |

**返回:**

- `void`

**示例:**

```lua
local img = Light(Light.Graphics.Image):New("hero.png")
```

<sub>📄 `light_graphics_image.cpp:101`</sub>

---

## Light.Graphics.ImageData

> 共 6 个 API

**函数列表:**

- [`Count`](#lightgraphicsimagedatacount) — 获取像素总数
- [`GetDepth`](#lightgraphicsimagedatagetdepth) — 获取像素位深/通道数
- [`GetHeight`](#lightgraphicsimagedatagetheight) — 获取数据高度
- [`GetPointer`](#lightgraphicsimagedatagetpointer) — 获取原始像素数据指针
- [`GetWidth`](#lightgraphicsimagedatagetwidth) — 获取数据宽度
- [`__call`](#lightgraphicsimagedata__call) — 构造函数 (支持 3 种创建方式)

---

### `Light.Graphics.ImageData.Count`

**获取像素总数**

**返回:**

- `number`

<sub>📄 `light_graphics_image.cpp:194`</sub>

---

### `Light.Graphics.ImageData.GetDepth`

**获取像素位深/通道数**

**返回:**

- `number`

<sub>📄 `light_graphics_image.cpp:169`</sub>

---

### `Light.Graphics.ImageData.GetHeight`

**获取数据高度**

**返回:**

- `number`

<sub>📄 `light_graphics_image.cpp:159`</sub>

---

### `Light.Graphics.ImageData.GetPointer`

**获取原始像素数据指针**

**返回:**

- `lightuserdata,number` — 指针,字节数

<sub>📄 `light_graphics_image.cpp:179`</sub>

---

### `Light.Graphics.ImageData.GetWidth`

**获取数据宽度**

**返回:**

- `number`

<sub>📄 `light_graphics_image.cpp:149`</sub>

---

### `Light.Graphics.ImageData.__call`

**构造函数 (支持 3 种创建方式)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `filename_or_w` | `string|number` | 文件名或宽度 | 是 |

**返回:**

- `void`

**说明:**

- __call(self, filename) — 从文件加载
- __call(self, pointer, size) — 从缓冲区创建
- __call(self, w, h, depth, format) — 指定尺寸创建

<sub>📄 `light_graphics_image.cpp:215`</sub>

---

## Light.Network

> 共 1 个 API

**函数列表:**

- [`Resume`](#lightnetworkresume) — 驱动网络 IO 事件循环 (基于 libuv, 每帧调用)

---

### `Light.Network.Resume`

**驱动网络 IO 事件循环 (基于 libuv, 每帧调用)**

**返回:**

- `number` — 始终返回 1

<sub>📄 `light_network.cpp:191`</sub>

---

## Light.Network.Http

> 共 7 个 API

**函数列表:**

- [`Close`](#lightnetworkhttpclose) — 关闭 TCP 连接
- [`GetFD`](#lightnetworkhttpgetfd) — 获取底层句柄指针 (兼容旧接口)
- [`Open`](#lightnetworkhttpopen) — 建立 TCP 连接 (异步, libuv 驱动)
- [`SendMessage`](#lightnetworkhttpsendmessage) — 发送 WebSocket 消息
- [`SendRequest`](#lightnetworkhttpsendrequest) — 发送 HTTP 请求
- [`Upgrade`](#lightnetworkhttpupgrade) — HTTP 升级到 WebSocket
- [`__call`](#lightnetworkhttp__call) — 构造函数, 创建 HTTP 客户端实例

---

### `Light.Network.Http.Close`

**关闭 TCP 连接**

**返回:**

- `void`

<sub>📄 `light_network.cpp:250`</sub>

---

### `Light.Network.Http.GetFD`

**获取底层句柄指针 (兼容旧接口)**

**返回:**

- `number`

<sub>📄 `light_network.cpp:388`</sub>

---

### `Light.Network.Http.Open`

**建立 TCP 连接 (异步, libuv 驱动)**

**返回:**

- `void`

<sub>📄 `light_network.cpp:202`</sub>

---

### `Light.Network.Http.SendMessage`

**发送 WebSocket 消息**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `msg` | `string` | 消息内容 | 是 |
| `opcode` | `number?` | 操作码 (1=文本, 2=二进制, 默认 1) | 是 |

**返回:**

- `void`

<sub>📄 `light_network.cpp:304`</sub>

---

### `Light.Network.Http.SendRequest`

**发送 HTTP 请求**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `method` | `string` | HTTP 方法 (GET/POST/PUT/DELETE) | 是 |
| `path` | `string` | 请求路径 | 是 |
| `body` | `string?` | 请求体 | 是 |

**返回:**

- `void`

<sub>📄 `light_network.cpp:271`</sub>

---

### `Light.Network.Http.Upgrade`

**HTTP 升级到 WebSocket**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string?` | 升级路径 (默认 "/") | 是 |

**返回:**

- `void`

<sub>📄 `light_network.cpp:362`</sub>

---

### `Light.Network.Http.__call`

**构造函数, 创建 HTTP 客户端实例**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `ip` | `string` | 服务器 IP/域名 | 是 |
| `port` | `number` | 端口 | 是 |

**返回:**

- `void`

<sub>📄 `light_network.cpp:398`</sub>

---

## Light.Network.HttpServer

> 共 3 个 API

**函数列表:**

- [`Close`](#lightnetworkhttpserverclose) — 关闭服务器
- [`Open`](#lightnetworkhttpserveropen) — 启动服务器监听 (基于 libuv, 新连接通过 OnAccept 回调分发)
- [`__call`](#lightnetworkhttpserver__call) — 构造函数, 创建 HTTP 服务器 (基于 libuv)

---

### `Light.Network.HttpServer.Close`

**关闭服务器**

**返回:**

- `void`

<sub>📄 `light_network.cpp:469`</sub>

---

### `Light.Network.HttpServer.Open`

**启动服务器监听 (基于 libuv, 新连接通过 OnAccept 回调分发)**

**返回:**

- `boolean` — 监听是否成功

<sub>📄 `light_network.cpp:431`</sub>

---

### `Light.Network.HttpServer.__call`

**构造函数, 创建 HTTP 服务器 (基于 libuv)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `ip` | `string` | 监听 IP (如 "0.0.0.0") | 是 |
| `port` | `number` | 监听端口 | 是 |

**返回:**

- `void`

<sub>📄 `light_network.cpp:488`</sub>

---

## Light.Plugins.NEMData

> 共 8 个 API

**函数列表:**

- [`GetDimensions`](#lightpluginsnemdatagetdimensions) — 获取地图尺寸
- [`GetHeight`](#lightpluginsnemdatagetheight) — 获取地图高度
- [`GetImageData`](#lightpluginsnemdatagetimagedata) — 获取指定 tile 坐标的图像数据
- [`GetMaskImageData`](#lightpluginsnemdatagetmaskimagedata) — 获取指定 tile 坐标的遮罩图像
- [`GetPath`](#lightpluginsnemdatagetpath) — A* 寻路
- [`GetWidth`](#lightpluginsnemdatagetwidth) — 获取地图宽度
- [`IsObstacle`](#lightpluginsnemdataisobstacle) — 查询指定位置是否为障碍物
- [`__call`](#lightpluginsnemdata__call) — 构造函数, 加载 NEM 地图文件

---

### `Light.Plugins.NEMData.GetDimensions`

**获取地图尺寸**

**返回:**

- `number,number` — w,h

<sub>📄 `light_plugins.cpp:581`</sub>

---

### `Light.Plugins.NEMData.GetHeight`

**获取地图高度**

**返回:**

- `number`

<sub>📄 `light_plugins.cpp:570`</sub>

---

### `Light.Plugins.NEMData.GetImageData`

**获取指定 tile 坐标的图像数据**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `tx` | `number` | tile X 坐标 | 是 |
| `ty` | `number` | tile Y 坐标 | 是 |

**返回:**

- `ImageData|nil`

<sub>📄 `light_plugins.cpp:593`</sub>

---

### `Light.Plugins.NEMData.GetMaskImageData`

**获取指定 tile 坐标的遮罩图像**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `tx` | `number` | tile X 坐标 | 是 |
| `ty` | `number` | tile Y 坐标 | 是 |

**返回:**

- `ImageData|nil`

<sub>📄 `light_plugins.cpp:627`</sub>

---

### `Light.Plugins.NEMData.GetPath`

**A* 寻路**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `sx` | `number` | 起点 X | 是 |
| `sy` | `number` | 起点 Y | 是 |
| `ex` | `number` | 终点 X | 是 |
| `ey` | `number` | 终点 Y | 是 |

**返回:**

- `table` — 路径点数组 {{x,y}, ...}

<sub>📄 `light_plugins.cpp:658`</sub>

---

### `Light.Plugins.NEMData.GetWidth`

**获取地图宽度**

**返回:**

- `number`

<sub>📄 `light_plugins.cpp:559`</sub>

---

### `Light.Plugins.NEMData.IsObstacle`

**查询指定位置是否为障碍物**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `x` | `number` | 地图 X 坐标 | 是 |
| `y` | `number` | 地图 Y 坐标 | 是 |

**返回:**

- `boolean`

<sub>📄 `light_plugins.cpp:707`</sub>

---

### `Light.Plugins.NEMData.__call`

**构造函数, 加载 NEM 地图文件**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | NEM 文件路径 | 是 |

**返回:**

- `void`

<sub>📄 `light_plugins.cpp:728`</sub>

---

## Light.Plugins.WDFData

> 共 6 个 API

**函数列表:**

- [`GetAudioData`](#lightpluginswdfdatagetaudiodata) — 提取解码后的音频数据
- [`GetData`](#lightpluginswdfdatagetdata) — 按哈希提取原始数据 (不解码)
- [`GetImageData`](#lightpluginswdfdatagetimagedata) — 提取解码后的图像数据 (RGBA)
- [`GetSpriteImagesData`](#lightpluginswdfdatagetspriteimagesdata) — 提取并解析 WAS 精灵帧数据
- [`GetTGAData`](#lightpluginswdfdatagettgadata) — 提取解码后的 TGA 纹理数据
- [`__call`](#lightpluginswdfdata__call) — 构造函数, 打开 WDF 资源包

---

### `Light.Plugins.WDFData.GetAudioData`

**提取解码后的音频数据**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `hash` | `number` | 文件哈希 | 是 |

**返回:**

- `AudioData|nil`

<sub>📄 `light_plugins.cpp:186`</sub>

---

### `Light.Plugins.WDFData.GetData`

**按哈希提取原始数据 (不解码)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `hash` | `number` | 文件哈希 | 是 |

**返回:**

- `userdata,number` — 原始字节,大小 | nil

<sub>📄 `light_plugins.cpp:110`</sub>

---

### `Light.Plugins.WDFData.GetImageData`

**提取解码后的图像数据 (RGBA)**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `hash` | `number` | 文件哈希 | 是 |

**返回:**

- `ImageData|nil`

<sub>📄 `light_plugins.cpp:158`</sub>

---

### `Light.Plugins.WDFData.GetSpriteImagesData`

**提取并解析 WAS 精灵帧数据**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `hash` | `number` | 资源哈希 | 是 |

**返回:**

- `table|nil` — { directions, framesPerDir, width, height, keyX, keyY, frames }

<sub>📄 `light_plugins.cpp:210`</sub>

---

### `Light.Plugins.WDFData.GetTGAData`

**提取解码后的 TGA 纹理数据**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `hash` | `number` | 文件哈希 | 是 |

**返回:**

- `userdata,number` — TGA数据,大小 | nil

<sub>📄 `light_plugins.cpp:134`</sub>

---

### `Light.Plugins.WDFData.__call`

**构造函数, 打开 WDF 资源包**

**参数:**

| 名称 | 类型 | 描述 | 必需 |
|------|------|------|------|
| `path` | `string` | WDF 文件路径 | 是 |

**返回:**

- `void`

<sub>📄 `light_plugins.cpp:393`</sub>

---
