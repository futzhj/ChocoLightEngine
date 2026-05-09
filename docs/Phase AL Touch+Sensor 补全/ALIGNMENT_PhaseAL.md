# Phase AL — Touch + Sensor 补全 — Alignment

## 0. 项目上下文

- **引擎**: ChocoLight, Lua 5.1 + C++17, SDL3 v3.2.30 后端
- **Lua 模块协议**: `luaopen_Light_<Module>` 返回模块表;handle 通过 `lightuserdata` 暴露;subsystem lazy init
- **Phase 基线**: AH/AI/AJ/AK 已完成 (Mouse/Joystick/Gamepad/Haptic 全覆盖),CI 全 6 平台绿
- **本 Phase 目标**: 收尾 Input thin 模块,使 Touch / Sensor 与 SDL3 头文件 1:1 对齐

## 1. 现状盘点

### 1.1 `@e:\jinyiNew\Light\ChocoLight\src\light_touch.cpp` (225 行)

**当前 4 fns**: GetDevices / GetDeviceName / GetDeviceType / GetFingers
**当前 2 const**: MOUSE_ID / TOUCH_ID

`@e:\jinyiNew\Light\ChocoLight\build\_deps\sdl3-src\include\SDL3\SDL_touch.h` 全集:

- 4 fns: SDL_GetTouchDevices / SDL_GetTouchDeviceName / SDL_GetTouchDeviceType / SDL_GetTouchFingers
- 2 macro: SDL_TOUCH_MOUSEID / SDL_MOUSE_TOUCHID
- 1 enum: SDL_TouchDeviceType (4 values)
- 1 struct: SDL_Finger {id,x,y,pressure}

**结论**: Touch fns **已 100% 覆盖**。

### 1.2 `@e:\jinyiNew\Light\ChocoLight\src\light_sensor.cpp` (158 行)

**当前 5 fns** (高级简化):
- OpenAccel(): 单例 — 自动找第一个 ACCEL 类型并打开
- OpenGyro(): 单例 — 自动找第一个 GYRO 类型并打开
- GetAccel() / GetGyro(): 读已打开的单例
- Close(): 关 accel + gyro

**当前 0 const**

`@e:\jinyiNew\Light\ChocoLight\build\_deps\sdl3-src\include\SDL3\SDL_sensor.h` 全集:

- 14 fns:
  - 枚举 (4): GetSensors / GetSensorNameForID / GetSensorTypeForID / GetSensorNonPortableTypeForID
  - 句柄管理 (3): OpenSensor / GetSensorFromID / CloseSensor
  - 句柄查询 (6): GetSensorProperties / GetSensorName / GetSensorType / GetSensorNonPortableType / GetSensorID / GetSensorData
  - 全局 (1): UpdateSensors
- 8 SDL_SensorType: INVALID/UNKNOWN/ACCEL/GYRO/ACCEL_L/GYRO_L/ACCEL_R/GYRO_R
- 1 macro: SDL_STANDARD_GRAVITY = 9.80665

## 2. 任务范围 (边界确认)

### 2.1 In-scope

- **Touch**: 修复常量精度 + 加 SDL_TouchDeviceType string ↔ enum 一致性 smoke
- **Sensor**:
  - 新增底层 fns: GetSensors / GetSensorNameForID / GetSensorTypeForID / GetSensorNonPortableTypeForID / OpenSensor / GetSensorFromID / CloseSensorByHandle / GetSensorName / GetSensorType / GetSensorID / GetSensorData / UpdateSensors
  - 新增常量: SENSOR_INVALID / UNKNOWN / ACCEL / GYRO / ACCEL_L / GYRO_L / ACCEL_R / GYRO_R + STANDARD_GRAVITY
  - 保留 Phase G 旧 5 fns 不破坏向后兼容
- **smoke**: 扩充 sensor.lua / touch.lua,覆盖新 fns + const + 边界

### 2.2 Out-of-scope

- 事件回调 (SDL_EVENT_FINGER_DOWN/MOTION/UP, SDL_EVENT_SENSOR_UPDATE) — 由 lightw event loop 统一处理,不在本 Phase
- SDL_GetSensorProperties — 返回 SDL_PropertiesID,Light 无 properties 统一封装,先不绑
- 多 Sensor 并行 (能开多个 Accel) — 旧 API 单例,新底层 API 用 lightuserdata 句柄,自然支持

## 3. 关键设计决策

### 3.1 Sensor 句柄策略

新底层 API 句柄用 `lightuserdata` (与 Joystick/Gamepad/Haptic 一致),由 Lua 显式管理生命周期。旧 5 fns (OpenAccel/OpenGyro/...) 内部 C++ 单例不变,**两套互不干扰**。

### 3.2 SensorID 数值类型

`SDL_SensorID` 是 Uint32,Lua double 53 bit 完全装得下,直接 `lua_pushnumber`。

### 3.3 SensorData 返回

`SDL_GetSensorData(sensor, float* data, int num_values)` 是变长。设计:Lua 调用 `GetSensorData(handle, n)` 返回 n 个 float (1 ≤ n ≤ 16,clamp 防越界)。

### 3.4 Touch TOUCH_ID 常量精度

当前 `lua_pushnumber(L, -1.0)` 推 -1.0,但 SDL_MOUSE_TOUCHID 实际是 `(Uint64)-1` = 18446744073709551615。

**事件层一致性确认**: 需查 lightw event 层把 `event.tfinger.touchID` (Uint64) 推到 Lua 时是怎么 cast 的:
- 若用 `lua_pushnumber((double)id)` → 推出 `1.8446744073709552e+19`
- 若用 `lua_pushinteger((lua_Integer)id)` → Lua 5.1 lua_Integer 是 ptrdiff_t (Win64 下 int64),推出 -1

**决策**: 选与事件层一致的 cast。若事件层用 push number → 修 TOUCH_ID 为 `(lua_Number)((double)(Uint64)-1)`;若用 push integer → 保持 `-1.0`。**待 Phase AL 实施时查证一次**。

### 3.5 SensorType ↔ string 双向映射

参考 light_touch.cpp 的 `TouchTypeName` 模式,新增 `SensorTypeName` 辅助函数,enum→string,以保证 GetSensorType*ForID 返回 string 一致。

## 4. 验收标准

- [x] **Coverage**: Sensor 14 fns × 100% 覆盖 (除明确 Out-of-scope 的 GetSensorProperties)
- [x] **Constants**: 8 SensorType enum + STANDARD_GRAVITY 全部暴露
- [x] **Backward compat**: 旧 5 fns (OpenAccel/OpenGyro/GetAccel/GetGyro/Close) 行为不变
- [x] **smoke**: scripts/smoke/sensor.lua + scripts/smoke/touch.lua 覆盖新 API + 边界 + nil safety
- [x] **CI**: GitHub Actions 全 6 平台绿,Windows runtime smoke pass
- [x] **No regression**: 8 个相邻 Phase smoke (gamepad/joystick/mouse/keyboard/atomic/mutex/surface/clipboard/haptic) 全绿

## 5. 工作量预估

| 文件 | 当前行 | 预计行 | 增量 |
|---|---|---|---|
| `light_sensor.cpp` | 158 | ~520 | +362 |
| `light_touch.cpp` | 225 | ~245 | +20 |
| `scripts/smoke/sensor.lua` | (待查) | ~140 | (待查) |
| `scripts/smoke/touch.lua` | (待查) | ~110 | (待查) |

**总评**: 单 Phase 中规模最小的之一,1 轮编译 + 1 轮 smoke 即可收尾。

## 6. 共识 (无歧义)

- ✅ 任务范围: Sensor 补底层 + Touch 修常量精度 + smoke 加固
- ✅ 技术方案: lightuserdata 句柄 + lazy init + 单例兼容
- ✅ 验收标准: API/const/smoke/CI 全绿
- ✅ 边界: 不动事件层;不绑 GetSensorProperties

无需中断询问,直接进入 Phase AL Architect → Atomize → Automate。
