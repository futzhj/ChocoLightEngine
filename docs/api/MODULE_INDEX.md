# ChocoLight Lua API 模块索引

ChocoLight Lua 引擎已演进至 **80+ 个 `Light.*` 模块**（核心 + Phase C-K + Phase AM~AV），按用途分组列出。所有模块通过 `require 'Light.XXX'` 加载。

---

## 索引快速跳转

- [核心 OOP](#核心-oop)
- [图形 / 渲染](#图形--渲染)
- [视频 / 音频](#视频--音频)
- [窗口 / UI](#窗口--ui)
- [输入设备](#输入设备-sdl3)
- [系统服务](#系统服务-sdl3)
- [桌面集成](#桌面集成-sdl3)
- [文件 / 数据](#文件--数据)
- [数据库](#数据库)
- [网络](#网络)
- [物理 / ECS / 场景](#物理--ecs--场景)
- [安全 / 工具](#安全--工具)
- [骨骼动画 (Phase AV)](#骨骼动画-phase-av)
- [SDL3 直接绑定 (Phase AR)](#sdl3-直接绑定-phase-ar)
- [插件](#插件)

---

## 核心 OOP

| 模块 | 用途 |
|------|------|
| `Light` | OOP 框架: `Light.Class`, `Light.Object`, `Light.Module`, `Light.Mixin` |
| `Light.Math` | 数学辅助: `lerp`, `clamp`, `smoothstep`, `vec2/vec3` |

---

## 图形 / 渲染

| 模块 | 用途 |
|------|------|
| `Light.Graphics` | 2D/3D 绘制顶层: 颜色 / 变换 / 视口 |
| `Light.Graphics.Image` | 纹理加载 (PNG/JPG/BMP via stb_image) |
| `Light.Graphics.ImageData` | CPU 像素缓冲 + RGBA 操作 |
| `Light.Graphics.PixelFormat` | 像素格式枚举 |
| `Light.Graphics.Canvas` | 离屏 RT (FBO) |
| `Light.Graphics.Font` | TTF 字体加载 + 渲染 |
| `Light.Graphics.Particles` | 粒子系统 |
| `Light.Graphics.Shader` | GLSL Shader 程序 |
| `Light.Graphics.SpriteAnimation` | 精灵动画 (帧序列 + 时间轴) |
| `Light.Graphics.Tilemap` | 瓦片地图 |
| `Light.Graphics.Mesh` | 3D 静态 mesh + glTF 加载 (Phase AS) |
| `Light.Graphics.Material` | PBR 材质 (baseColor / metallic / roughness / emissive / textures, Phase AS) |
| `Light.Lighting2D` | 2D forward 多光照状态管理 (16 lights + ambient + Point/Spot, Phase E.1) |

详见 `docs/api/Light_Graphics*.md`、`docs/api/Light_Lighting2D.md`。

---

## 视频 / 音频

| 模块 | 用途 |
|------|------|
| `Light.AV` | 多媒体顶层 |
| `Light.AV.Audio` | 音频播放 (miniaudio) |
| `Light.AV.AudioData` | PCM 数据缓冲 |
| `Light.AV.Video` | 视频解码 (FFmpeg 动态加载,可选) |
| `Light.Audio` | SDL3 audio device + stream + 回调 (Phase AM/AN 全新绑定 + 回调补完) |
| `Light.Audio.Sound` | 3D 空间化音源 (位置/速度/距离衰减, Phase AT) |
| `Light.Audio.SoundGroup` | 音源分组混音 + 总线 (Phase AT) |
| `Light.Audio.Effect` | 音效 (Reverb / Echo / Filter, Phase AT) |

详见 `docs/api/Light_AV*.md`。

---

## 窗口 / UI

| 模块 | 用途 |
|------|------|
| `Light.UI` | UI 顶层 |
| `Light.UI.Window` | 窗口创建 + 事件循环 (SDL3) |
| `Light.UI.Widget` | 简易 widget 基类 |

---

## 输入设备 (SDL3)

| 模块 | 用途 | Phase |
|------|------|-------|
| `Light.Input` | 键盘 / 鼠标 输入查询 | Phase 2 |
| `Light.Gamepad` | 游戏手柄 (按键 / 摇杆 / 振动 / 电量) | **Phase G** |
| `Light.Hidapi` | 原始 HID 设备访问 (17 fn) | **Phase J** |
| `Light.Haptic` | 触觉反馈 / 力反馈 (22 fn) | **Phase K** |

---

## 系统服务 (SDL3)

| 模块 | 用途 | Phase |
|------|------|-------|
| `Light.IO` | 标准 I/O 流 + 临时文件 | **Phase D** |
| `Light.Storage` | 持久化存储路径 (config / cache / data) | **Phase D** |
| `Light.Locale` | 系统语言偏好 | **Phase D** |
| `Light.Power` | 电源状态 / 电量 | **Phase D** |
| `Light.Sensor` | 加速度计 / 陀螺仪 | **Phase D** |
| `Light.Process` | 子进程派生 + 管道 | **Phase E** |
| `Light.Camera` | 摄像头帧抓取 | **Phase E** |
| `Light.System` | OS / CPU / 内存 / 平台名 | **Phase H** |
| `Light.Guid` | 跨平台 GUID 生成 | **Phase H** |

---

## 桌面集成 (SDL3)

| 模块 | 用途 | Phase |
|------|------|-------|
| `Light.MessageBox` | 系统消息框 (info / warn / error / 选择) | **Phase E** |
| `Light.Clipboard` | 系统剪贴板读写 (text / image) | **Phase F** |
| `Light.URL` | URL 启动 (浏览器 / 邮件 / 文件) | **Phase F** |
| `Light.Display` | 显示器枚举 + 分辨率 / DPI | **Phase F** |
| `Light.Cursor` | 鼠标光标自定义 | **Phase F** |
| `Light.Tray` | 系统托盘图标 + 菜单 + 回调 (20 fn) | **Phase I / I.2 / I.3** |

---

## 文件 / 数据

| 模块 | 用途 |
|------|------|
| `Light.Data` | 二进制缓冲区 (encode / decode / pack) |
| `Light.HotReload` | 热重载 (Lua 文件监听) |

---

## 数据库

| 模块 | 用途 |
|------|------|
| `Light.DB` | 数据库顶层 |
| `Light.DB.SQLite` | SQLite WAL + ORM |
| `Light.Record` | 纯 Lua ORM (无依赖) |

详见 `docs/api/Light_DB_SQLite.md`。

---

## 网络

| 模块 | 用途 |
|------|------|
| `Light.Network` | 网络顶层 (TCP/UDP) |
| `Light.Network.Http` | HTTP 客户端 (libuv) |
| `Light.Network.HttpServer` | HTTP 服务器 |
| `Light.Network.Web` | WebSocket |

详见 `docs/api/Light_Network*.md`。

---

## 物理 / ECS / 场景

| 模块 | 用途 |
|------|------|
| `Light.Physics` | 物理顶层 (Box2D v2.4.1) |
| `Light.Physics.World` | 物理世界 (重力 / 步进) |
| `Light.Physics3D` | 3D 物理 (Bullet 3): RigidBody / Vehicle / SoftBody / Constraints (Phase AU) |
| `Light.ECS` | Entity-Component-System |
| `Light.Scene` | 场景树 + 节点 |

详见 `docs/api/Light_Physics*.md`。

---

## 安全 / 工具

| 模块 | 用途 |
|------|------|
| `Light.Crypto` | AES-128 / SHA / HMAC / Base64 |
| `Light.Debug` | 反调试 (5 层检测 + 静默异常) |

---

## 骨骼动画 (Phase AV)

| 模块 | 用途 |
|------|------|
| `Light.Animation` | 顶层: `LoadSkinnedGLTF` / `NewAnimator` / `DrawSkinnedMesh` |
| `Light.Animation.Skeleton` | 骨骼数据 (≤ 64 关节) + bind pose + inverseBind 矩阵 |
| `Light.Animation.Clip` | AnimationClip (LINEAR/STEP/CUBICSPLINE 采样器) |
| `Light.Animation.Animator` | 状态机 + Transition / Crossfade / Event / Param + 关节矩阵计算 |
| `Light.Animation.SkinnedMesh` | 蒙皮网格 (CPU skinning) |

详见 `docs/api/Light_Animation.md`、`docs/Phase AV 骨骼动画/`。

---

## SDL3 直接绑定 (Phase AR)

Phase AR 把 SDL3 的核心子系统直接以 1:1 形式暴露给 Lua（用于需要更底层控制的场景）。多数业务推荐使用上层封装（`Light.Graphics` / `Light.Audio` 等），SDL3 直接绑定适合编写引擎扩展或迁移 SDL2/3 C 代码的项目。

| 模块 | SDL3 子系统 |
|------|------------|
| `Light.Touch` | 触摸输入（设备 + 手指 + 多点） |
| `Light.Hints` | 引擎/平台 hint |
| `Light.Time` | 时间 / 计时器 |
| `Light.Endian` | 字节序 + 转换 |
| `Light.Log` | SDL3 日志输出 |
| `Light.Pixels` | 像素格式工具 |
| `Light.BlendMode` | 混合模式枚举 |
| `Light.Filesystem` | SDL3 文件系统 |
| `Light.CPUInfo` | CPU 特性查询（与 `Light.System` 部分重叠） |
| `Light.Error` | SDL3 错误字符串 |
| `Light.Loadso` | 动态库加载 |
| `Light.Version` | SDL3 版本信息 |
| `Light.Properties` | KV 属性容器 |
| `Light.Misc` | 杂项工具 |
| `Light.Rect` | 矩形 / 点 数学 |
| `Light.Atomic` | 原子操作 |
| `Light.Mutex` | 互斥锁 / 信号量 / 条件变量 |
| `Light.IOStream` | SDL_IOStream（替代 SDL_RWops） |
| `Light.Surface` | CPU 像素 surface |
| `Light.Keyboard` | 键盘状态查询 |
| `Light.Mouse` | 鼠标状态查询 |
| `Light.Joystick` | 原始 joystick（区别于上层 `Light.Gamepad`） |
| `Light.Audio` | SDL3 audio device + stream（Phase AM 全新绑定 + Phase AN 回调补完） |
| `Light.Event` | SDL3 事件队列 + 注册过滤器 |
| `Light.Dialog` | 系统文件/文件夹选择对话框 |

**注**：`Light.Input` 在 Phase AQ 增强了 TextInput / IME 子集（`StartTextInput` / `StopTextInput` / `SetIMERect` / `IsTextInputActive`），不是新模块。

---

## 插件

| 模块 | 用途 |
|------|------|
| `Light.Plugins` | 插件加载 |
| `Light.Plugins.WDFData` | WDF 文件格式解析 |
| `Light.Plugins.NEMData` | NEM 文件格式解析 |

详见 `docs/api/Light_Plugins*.md`。

---

# Phase C-K 新增模块详细 fn 列表

下面是 Phase C-K 在 SDL3 升级中新增模块的完整 Lua API 签名。每条 fn 后注明返回值约定,失败一律返回 `nil/false + err_string`。

## Light.IO (Phase D)

```lua
local IO = require 'Light.IO'

IO.GetTempPath()                      -- -> path_str, err
IO.CreateTempFile([prefix])           -- -> path_str, err  (auto-deleted on exit)
IO.GetExePath()                       -- -> path_str, err
IO.GetCwd()                           -- -> path_str, err
IO.SetCwd(path)                       -- -> ok, err
IO.PathExists(path)                   -- -> bool
IO.IsFile(path) / IsDirectory(path)   -- -> bool
IO.MakeDir(path)                      -- -> ok, err  (recursive)
IO.RemoveFile(path) / RemoveDir(path) -- -> ok, err
IO.ListDir(path)                      -- -> array<string>, err
```

## Light.Storage (Phase D)

```lua
local Storage = require 'Light.Storage'

Storage.GetBasePath()                  -- exe 所在目录
Storage.GetUserPath(org, app)          -- 平台标准 user data 目录
Storage.GetPrefPath(org, app)          -- alias of GetUserPath (love-style)
```

## Light.Locale (Phase D)

```lua
local Locale = require 'Light.Locale'

Locale.GetPreferred()  -- -> array<{language, country}>, err
                       -- 例如: { {language="en", country="US"}, {language="zh", country="CN"} }
```

## Light.Power (Phase D)

```lua
local Power = require 'Light.Power'

Power.GetState()  -- -> {state, percent, seconds}, err
                  -- state: 'on_battery' / 'no_battery' / 'charging' / 'charged' / 'unknown'
                  -- percent: 0..100 或 -1
                  -- seconds: 剩余秒数 或 -1
```

## Light.Sensor (Phase D)

```lua
local Sensor = require 'Light.Sensor'

Sensor.GetSensors()        -- -> array<{id, name, type, non_portable_type}>
Sensor.Open(id) / Close(d) -- 打开/关闭传感器
Sensor.GetData(d)          -- -> array<float>  (3 个浮点: x, y, z)
```

## Light.Process (Phase E)

```lua
local Process = require 'Light.Process'

Process.Create(args, [opts])  -- args: array<string>, opts: {stdin/stdout/stderr/cwd/env}
                              -- -> proc_handle, err
Process.Wait(proc, [block])   -- -> exit_code, err
Process.Kill(proc, [force])   -- -> ok, err
Process.Read(proc, "stdout"|"stderr", [max_bytes]) -- -> data_str, err
Process.Write(proc, data)     -- -> bytes_written, err
Process.Destroy(proc)         -- -> ok, err
Process.GetPid(proc)          -- -> int_pid
```

## Light.Camera (Phase E)

```lua
local Camera = require 'Light.Camera'

Camera.GetCameras()       -- -> array<{id, name}>
Camera.Open(id, [spec])   -- spec: {format, width, height, framerate}
Camera.AcquireFrame(cam)  -- -> {timestamp_ns, width, height, pixels}, err
Camera.ReleaseFrame(cam, frame)
Camera.Close(cam)
Camera.GetPermissionState(cam)  -- 'approved' / 'denied' / 'waiting'
```

## Light.MessageBox (Phase E)

```lua
local MB = require 'Light.MessageBox'

MB.Show(level, title, msg [, buttons])
  -- level: 'info' / 'warning' / 'error'
  -- buttons: optional array<string>
  -- -> selected_button_index, err  (1-based; -1 if dismissed)

MB.ShowSimple(level, title, msg)  -- 单 OK 按钮
```

## Light.Clipboard (Phase F)

```lua
local Clip = require 'Light.Clipboard'

Clip.HasText() / GetText() / SetText(s)  -- -> bool / str / ok
Clip.HasData(mime) / GetData(mime) / SetData(mime, bytes)  -- 任意 MIME
Clip.Clear()                              -- -> ok, err
```

## Light.URL (Phase F)

```lua
local URL = require 'Light.URL'

URL.Open(url)            -- 启动浏览器/邮件/文件
URL.OpenFile(path)       -- 打开本地文件 (file://)
```

## Light.Display (Phase F)

```lua
local Display = require 'Light.Display'

Display.GetDisplays()      -- -> array<id>
Display.GetPrimary()       -- -> id
Display.GetBounds(id)      -- -> {x, y, w, h}
Display.GetUsableBounds(id)-- 排除任务栏/dock 后的可用区
Display.GetName(id)        -- -> string
Display.GetDPI(id)         -- -> {ddpi, hdpi, vdpi}
Display.GetContentScale(id)-- -> float (UI 缩放系数)
Display.GetOrientation(id) -- 'landscape' / 'portrait' / 'landscape_flipped' / 'portrait_flipped'
Display.GetForPoint(x, y)  -- 哪个显示器包含此屏幕坐标
```

## Light.Cursor (Phase F)

```lua
local Cursor = require 'Light.Cursor'

Cursor.Create(image_data, hot_x, hot_y)  -- 自定义光标
Cursor.CreateSystem(name)  -- 'default' / 'text' / 'crosshair' / 'wait' / 'hand' 等
Cursor.Set(cursor) / Hide() / Show() / IsVisible()
Cursor.Destroy(cursor)
```

## Light.Gamepad (Phase G)

```lua
local Pad = require 'Light.Gamepad'

Pad.GetGamepads()                 -- -> array<instance_id>
Pad.Open(id) / Close(pad)         -- -> handle, err
Pad.GetID(pad) / GetName(pad) / GetType(pad)
Pad.IsConnected(pad) / GetConnectionState(pad)
Pad.GetPowerInfo(pad)             -- -> state_str, percent
Pad.HasButton(pad, name) / GetButton(pad, name)  -- 'a','b','x','y','start','back',...
Pad.HasAxis(pad, name) / GetAxis(pad, name)      -- 'leftx','lefty','rightx','righty','lefttrigger','righttrigger'
Pad.Rumble(pad, low, high, duration_ms)
Pad.RumbleTriggers(pad, left, right, duration_ms)
Pad.SetLED(pad, r, g, b)
```

## Light.Guid (Phase H)

```lua
local Guid = require 'Light.Guid'

Guid.New()                        -- -> 36-char string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
Guid.IsValid(str)                 -- -> bool
Guid.Equal(a, b)                  -- -> bool
Guid.NewBinary()                  -- -> 16-byte binary string
Guid.ToBinary(s) / FromBinary(b)  -- 字符串<->二进制 互转
```

## Light.System (Phase H)

```lua
local Sys = require 'Light.System'

Sys.GetPlatform()        -- 'Windows' / 'macOS' / 'Linux' / 'Android' / 'iOS' / 'Web'
Sys.GetCPUCount()        -- -> int
Sys.GetRAMMB()           -- -> int (总内存 MB)
Sys.GetCacheLineSize()   -- -> int
Sys.HasAVX() / HasSSE2() / HasNEON() / HasAltiVec()  -- -> bool
Sys.GetTickMS()          -- -> int (单调时钟毫秒)
Sys.GetPerformanceCounter() / GetPerformanceFrequency()  -- 高精度计时
```

## Light.Tray (Phase I / I.2 / I.3) — 20 fn

```lua
local Tray = require 'Light.Tray'

-- Tray 本身
Tray.Create(tooltip)            -> tray, err
Tray.Destroy(tray)              -> ok, err
Tray.SetTooltip(tray, str)
Tray.SetIconFromFile(tray, path)  -- PNG/JPG/BMP via stb_image (Phase I.2)

-- Menu / Entry
Tray.GetMenu(tray)              -> menu, err
Tray.AddButton(menu, label)     -> entry
Tray.AddCheckbox(menu, label, [checked]) -> entry
Tray.AddSeparator(menu)         -> entry
Tray.AddSubmenu(menu, label)    -> entry, submenu, err
Tray.SetEntryLabel / GetEntryLabel(entry, [str])
Tray.SetEntryEnabled / GetEntryEnabled  -- 注意 SDL3 v3.2.30 Win 上游 bug
Tray.SetEntryChecked / GetEntryChecked
Tray.RemoveEntry(entry)

-- 主循环
Tray.Update()                   -- 每帧调用 (event loop pump)

-- 轮询模型
Tray.WasClicked(entry)          -> int (累计次数,read-and-reset)

-- 回调模型 (Phase I.3, 与轮询共存)
Tray.SetClickCallback(entry, fn|nil)  -- fn 签名: function(count_int)
Tray.PollCallbacks()            -> dispatched_count
```

## Light.Hidapi (Phase J) — 17 fn

```lua
local HID = require 'Light.Hidapi'

HID.Init() / Exit()
HID.DeviceChangeCount()                 -> int (单调累加,变更通知)
HID.Enumerate([vid], [pid])             -> array of devices
HID.Open(vid, pid, [serial_ascii])      -> dev, err
HID.OpenPath(path)                      -> dev, err
HID.Close(dev)
HID.Write(dev, data) / Read(dev, max) / ReadTimeout(dev, max, ms)
HID.SendFeatureReport(dev, data)
HID.GetFeatureReport(dev, max) / GetInputReport(dev, max)
HID.SetNonblocking(dev, bool)
HID.GetManufacturerString / GetProductString / GetSerialNumberString(dev)
```

## Light.Haptic (Phase K) — 22 fn

```lua
local Haptic = require 'Light.Haptic'

-- 设备
Haptic.Init() / Quit()
Haptic.GetHaptics()                  -> array<{id, name}>
Haptic.IsMouseHaptic()               -> bool
Haptic.Open(id) / OpenFromMouse() / Close(dev)

-- 查询
Haptic.GetID / GetName / GetFeatures(dev)
Haptic.GetMaxEffects / GetMaxEffectsPlaying / GetNumAxes(dev)

-- 简易 rumble
Haptic.RumbleSupported(dev) / InitRumble(dev)
Haptic.PlayRumble(dev, strength_0_1, length_ms)
Haptic.StopRumble(dev)

-- 全局控制
Haptic.Pause(dev) / Resume(dev) / StopAll(dev)
Haptic.SetGain(dev, 0_100) / SetAutocenter(dev, 0_100)
```

---

# 进一步阅读

- 详细 Phase 1/2/3 模块: `docs/api/Light_*.md`
- 项目总览: `docs/PROJECT_SUMMARY.md`
- 技能与最佳实践: `docs/SKILLS.md`
- API 参考: `docs/API_REFERENCE.md`
- 演示样例: `samples/`
