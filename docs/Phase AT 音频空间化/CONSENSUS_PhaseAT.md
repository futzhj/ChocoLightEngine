# Phase AT — Audio 全套 (3D + Mixer + Effects) — 共识文档

> **6A Stage 2**: 用户决策 — **全套含 Audio Effects** (3D + Mixer + 滤波器 + 延迟)
>
> **关键技术决策**: reverb 用 `ma_delay_node` + 反馈实现 echo (简化版); 完整 reverb 需自定义 ma_node 节点, 留更后续。

---

## 1. 范围 (3 大模块 + Light.Audio 扩展)

### 1.1 `Light.Audio.Sound` (高层 sound + 3D + group + effect)

```lua
-- 加载
local s = Light.Audio.Sound.Load(path) -> Sound|nil [, err]
local s = Light.Audio.Sound.LoadPCM(data, fmt, ch, rate) -> Sound|nil

-- 播放控制
s:Play() / Pause() / Stop() / IsPlaying() -> bool
s:SetLooping(b) / GetLooping() -> bool

-- 音量 / 音调 / pan
s:SetVolume(v) / GetVolume() -> v          -- [0, 1+]
s:SetPitch(p) / GetPitch() -> p             -- 1.0 = 原速
s:SetPan(pan) / GetPan() -> pan             -- [-1, 1]

-- 3D 空间化
s:SetPosition(x,y,z) / GetPosition()
s:Set3DEnabled(bool) / Get3DEnabled() -> bool
s:SetAttenuationModel(s) / Get  -- "none"|"inverse"|"linear"|"exp"
s:SetMinDistance(d) / GetMinDistance()
s:SetMaxDistance(d) / GetMaxDistance()
s:SetRolloff(r) / GetRolloff()
s:SetVelocity(vx,vy,vz)                    -- 多普勒

-- Mixer / Effect 集成
s:SetGroup(group | nil)
s:SetEffect(effect | nil)                  -- 单 effect (不 chain)

-- 生命周期
s:Delete() / __gc / __tostring
```

### 1.2 `Light.Audio.SoundGroup` (Mixer)

```lua
local g = Light.Audio.SoundGroup.New() -> SoundGroup
g:SetVolume(v) / GetVolume() -> v
g:SetPitch(p) / GetPitch() -> p
g:Pause() / Resume() / Stop()
g:SetParent(group | nil)                 -- 嵌套 (Q3=B)
g:SetEffect(effect | nil)                -- group 级 effect
g:GetSoundCount() -> int
g:Delete() / __gc / __tostring
```

### 1.3 `Light.Audio.Effect` (滤波器 + 延迟 + echo)

```lua
-- 工厂 (5 种内置 + echo)
local lpf = Light.Audio.Effect.NewLowPass(cutoffHz, [order=4]) -> Effect|nil
local hpf = Light.Audio.Effect.NewHighPass(cutoffHz, [order=4]) -> Effect|nil
local bpf = Light.Audio.Effect.NewBandPass(cutoffHz, [order=4]) -> Effect|nil
local notch = Light.Audio.Effect.NewNotch(cutoffHz, [q=0.7])    -> Effect|nil
local peak = Light.Audio.Effect.NewPeak(cutoffHz, gainDB, [q=0.7]) -> Effect|nil

-- Echo (delay + feedback, 简化 reverb 替代)
local echo = Light.Audio.Effect.NewEcho({
    delay_ms = 250,         -- 延迟时间
    decay    = 0.5,         -- 反馈衰减 [0, 1)
    wet      = 0.5,         -- 湿度 [0, 1]
    dry      = 0.5,         -- 干度 [0, 1]
}) -> Effect|nil

-- 操作
e:SetEnabled(bool) / GetEnabled() -> bool
e:Delete() / __gc / __tostring
```

**注**: reverb 不做 (无 miniaudio 原生节点, 自实现成本高); echo 是最常用的"混响替代"。

### 1.4 `Light.Audio` 模块新增 7 fns

```lua
Light.Audio.SetListenerPosition(x, y, z, [idx=0])
Light.Audio.SetListenerDirection(x, y, z, [idx=0])
Light.Audio.SetListenerWorldUp(x, y, z, [idx=0])
Light.Audio.SetListenerVelocity(x, y, z, [idx=0])
Light.Audio.GetListenerCount() -> int
Light.Audio.SetGlobalVolume(v)
Light.Audio.GetGlobalVolume() -> v
```

---

## 2. AudioBackend C++ 接口扩展 (~40 fns)

```cpp
// 已有 (10 fns): Init/Shutdown/LoadFile/LoadPCM/Play/Pause/Stop/SetVolume/GetVolume/IsPlaying/Free

// AT 新增 (~40 fns)

// Sound 控制
void SetLooping(AudioHandle*, bool); bool GetLooping(AudioHandle*);
void SetPitch(AudioHandle*, float); float GetPitch(AudioHandle*);
void SetPan(AudioHandle*, float); float GetPan(AudioHandle*);

// Sound 3D
void SetSpatializationEnabled(AudioHandle*, bool); bool GetSpatializationEnabled(AudioHandle*);
void SetPosition(AudioHandle*, float x, float y, float z);
void GetPosition(AudioHandle*, float* x, float* y, float* z);
void SetAttenuationModel(AudioHandle*, int model);  // 0..3
void SetMinDistance(AudioHandle*, float); float GetMinDistance(AudioHandle*);
void SetMaxDistance(AudioHandle*, float); float GetMaxDistance(AudioHandle*);
void SetRolloff(AudioHandle*, float); float GetRolloff(AudioHandle*);
void SetVelocity(AudioHandle*, float x, float y, float z);

// SoundGroup
struct GroupHandle;
GroupHandle* CreateGroup(GroupHandle* parent);  // parent=nullptr -> master
void FreeGroup(GroupHandle*);
void SetGroupVolume(GroupHandle*, float); float GetGroupVolume(GroupHandle*);
void SetGroupPitch(GroupHandle*, float);  float GetGroupPitch(GroupHandle*);
void GroupPause(GroupHandle*); void GroupResume(GroupHandle*); void GroupStop(GroupHandle*);
void SetSoundGroup(AudioHandle*, GroupHandle*);  // nullptr -> master

// Effect (滤波器/echo)
struct EffectHandle;
EffectHandle* CreateLowPass(float cutoffHz, int order);
EffectHandle* CreateHighPass(float cutoffHz, int order);
EffectHandle* CreateBandPass(float cutoffHz, int order);
EffectHandle* CreateNotch(float cutoffHz, float q);
EffectHandle* CreatePeak(float cutoffHz, float gainDB, float q);
EffectHandle* CreateEcho(int delayMs, float decay, float wet, float dry);
void FreeEffect(EffectHandle*);
void SetEffectEnabled(EffectHandle*, bool); bool GetEffectEnabled(EffectHandle*);
void SetSoundEffect(AudioHandle*, EffectHandle*);   // nullptr -> 移除
void SetGroupEffect(GroupHandle*, EffectHandle*);

// Listener
void SetListenerPosition(int idx, float x, float y, float z);
void SetListenerDirection(int idx, float x, float y, float z);
void SetListenerWorldUp(int idx, float x, float y, float z);
void SetListenerVelocity(int idx, float x, float y, float z);
int  GetListenerCount();

// Master
void SetGlobalVolume(float);
float GetGlobalVolume();
```

---

## 3. miniaudio Node Graph 集成

### 3.1 Effect 节点实现

miniaudio 内置 (拿来即用):
- `ma_lpf_node` (低通滤波)
- `ma_hpf_node` (高通滤波)
- `ma_bpf_node` (带通滤波)
- `ma_notch_node` (陷波)
- `ma_peak_node` (峰值/钟形)
- `ma_delay_node` (延迟, 用作 echo)

### 3.2 Effect 应用方式

```cpp
// 默认: sound 输出直接 → engine endpoint
// 加 effect: sound 输出 → effect node 输入 → effect 输出 → engine endpoint
ma_node_attach_output_bus(&sound, 0, &effect_node, 0);
ma_node_attach_output_bus(&effect_node, 0, ma_engine_get_endpoint(engine), 0);
```

`SetSoundEffect(sound, NULL)` 时复原 (sound 直接接 endpoint)。

### 3.3 嵌套 SoundGroup

miniaudio `ma_sound_group` 原生支持 parent:
```cpp
ma_sound_group_set_parent(child, parent);  // parent=NULL -> master
```

---

## 4. 实施顺序 (单 commit)

1. `light_audio_backend.h/.cpp` 扩展 AudioBackend 接口 (~600 行)
2. `light_audio_sound.cpp` 新模块 (~500 行) — Light.Audio.Sound userdata
3. `light_audio_group.cpp` 新模块 (~200 行) — Light.Audio.SoundGroup userdata
4. `light_audio_effect.cpp` 新模块 (~280 行) — Light.Audio.Effect userdata
5. `light_audio.cpp` 加 Listener + GlobalVolume 7 fns (~80 行)
6. `light.h` 加 3 个 luaopen 声明
7. `lumen-master/src/light/light.cpp` 注册 3 个新模块
8. `CMakeLists.txt` 加 3 个新源文件
9. `scripts/smoke/audio_3d_mixer_effect.lua` smoke (~150 行)
10. `.github/workflows/build-templates.yml` 加 smoke 到 Windows runtime chain

---

## 5. 工作量

| 子模块 | C++ 行 |
|---|---|
| AudioBackend 接口扩展 (~40 fns + 2 structs) | ~600 |
| Light.Audio.Sound 模块 | ~500 |
| Light.Audio.SoundGroup 模块 | ~200 |
| Light.Audio.Effect 模块 | ~280 |
| Light.Audio Listener + Global API | ~80 |
| smoke audio_3d_mixer_effect.lua | ~150 |
| 文档 (ALIGN/CONSENSUS/ACCEPTANCE) | - |
| **合计** | **~1810 行** |

预期 ~10h, 1 commit, 6 平台 CI 验证。**风险高于一般 Phase**, 因为涉及 miniaudio node graph runtime 行为, headless 测试受限。

---

## 6. 风险与缓解

| 风险 | 缓解 |
|---|---|
| miniaudio engine 在某平台 (CI headless) 无法 init | LoadFile/LoadPCM 已 guard, 失败返回 nil + err, smoke 不依赖实际播放 |
| Effect node graph 嵌入失败 (sound 已 attach 后再 attach effect 顺序) | 仔细管理 attach/detach 顺序, `ma_node_detach_output_bus` 先解绑再重绑 |
| iOS/Android miniaudio 行为差异 | Init 已经 in CI 通过 (Phase AM 已用), 不应有新问题 |
| Reverb 不做导致用户体验缺 | 用 echo 替代; 真正需要 reverb 留 AT.x (要求自定义 ma_node) |
| Effect 生命周期 (Sound 引用 Effect, Effect 先 GC) | Sound 的 userdata 持有 Effect 的 Lua ref, 防止 GC |
| 嵌套 group 循环引用 (g1 parent = g2, g2 parent = g1) | `SetParent` 内部循环检测, 拒绝并返回 false |

---

## 7. 决策锁定 (Q1~Q5)

| Q | 锁定 |
|---|---|
| Q1 Sound 与 SDL3 关系 | A — 完全独立 |
| Q2 衰减模型枚举 | A — 字符串 ("none"/"inverse"/"linear"/"exp") |
| Q3 SoundGroup 嵌套 | B — 任意嵌套, 含循环检测 |
| Q4 默认 listener 数 | A — 1 个 (idx=0) |
| Q5 资源释放 | A — __gc 自动 + 显式 Delete 都支持 |

---

## 8. 验收标准

- [ ] Light.Audio.Sound 模块加载, 25+ 方法可用
- [ ] Light.Audio.SoundGroup 模块加载, 嵌套 group 工作
- [ ] Light.Audio.Effect 模块加载, 6 种 effect 可创建
- [ ] Light.Audio 加 7 fns Listener + Global
- [ ] 现有 51 fns Light.Audio 零回归
- [ ] `lightc -p audio_3d_mixer_effect.lua` Exit=0
- [ ] 6 平台 CI 全绿
