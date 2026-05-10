# ChocoLight Samples

每个目录是一个独立可运行的演示。结构与 `perf_benchmark/` 一致(`main.lua`)。

## 运行方式

ChocoLight 的 Lua 入口默认查找 `main.lua`。在 sample 目录运行 `light` 即可:

```sh
# Windows
cd samples\demo_io_storage
..\..\dist\windows-x64\light.exe main.lua

# Linux / macOS
cd samples/demo_io_storage
../../dist/linux-x64/light main.lua
```

或在仓库根目录直接指定脚本路径:

```sh
light samples/demo_io_storage/main.lua
```

## 样例索引

| 目录 | 模块覆盖 | Phase |
|------|----------|-------|
| `demo_io_storage/` | `Light.IO`, `Light.Storage` | D |
| `demo_locale_power_sensor/` | `Light.Locale`, `Light.Power`, `Light.Sensor` | D |
| `demo_process_messagebox/` | `Light.Process`, `Light.MessageBox`, `Light.System` | E |
| `demo_clipboard_url_display_cursor/` | `Light.Clipboard`, `Light.URL`, `Light.Display`, `Light.Cursor` | F |
| `demo_gamepad/` | `Light.Gamepad`, `Light.System` | G |
| `demo_guid_system/` | `Light.Guid`, `Light.System` | H |
| `demo_tray/` | `Light.Tray` (Phase I/I.2/I.3 全功能, 含 callback) | I |
| `demo_hidapi/` | `Light.Hidapi` | J |
| `demo_haptic/` | `Light.Haptic`, `Light.System` | K |
| `demo_physics3d/` | `Light.Physics3D` (RigidBody / Vehicle / SoftBody / Wheel info / Wind) | AU |
| `demo_animation/` | `Light.Animation` (glTF Skeleton / Clip / Animator / Transition / Crossfade / Event / SkinnedMesh) | AV |
| `demo_skinning_perf/` | Phase AW GPU Skinning 真机性能验证 (frame timing + OSD + G/C/A 模式切换 + 自动 baseline) | AW.x |
| `perf_benchmark/` | 性能基准测试 | - |

## 设计约定

每个 demo 遵循相同模式:

- **优雅降级**: 无设备 / 无平台支持时 `print` 跳过原因并以 `ok` 状态退出,不崩溃
- **无 GUI 阻塞**: 不做长时间阻塞;弹窗等需要交互的 API 用 `DEMO_SHOW = false` 开关
- **简短可读**: 每个文件 < 100 行,演示重点 API 而非完整应用

完整 API 索引见 `docs/api/MODULE_INDEX.md`。

## 跨平台说明

| 模块 | Windows | macOS | Linux | Android | iOS | Web |
|------|---------|-------|-------|---------|-----|-----|
| IO / Storage / System / Guid | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ 沙箱 |
| Locale / Power | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Sensor | ⚠️ 部分 | ❌ | ⚠️ 部分 | ✅ | ✅ | ❌ |
| Process | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ |
| Camera | ✅ | ✅ | ✅ (v4l) | ✅ | ✅ | ⚠️ getUserMedia |
| MessageBox / URL / Display / Cursor | ✅ | ✅ | ✅ | 部分 | 部分 | 部分 |
| Clipboard | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Gamepad | ✅ XInput/DInput | ✅ | ✅ evdev | ✅ | ⚠️ | ✅ Gamepad API |
| Tray | ✅ | ✅ | ✅ GNOME/KDE | ❌ | ❌ | ❌ |
| Hidapi | ✅ | ✅ | ✅ libusb | ✅ | ⚠️ BLE | ❌ |
| Haptic | ✅ DInput/XInput | ✅ IOKit | ✅ evdev | ❌ | ❌ | ❌ |

✅ 完整支持 / ⚠️ 部分支持或受限 / ❌ 不支持
