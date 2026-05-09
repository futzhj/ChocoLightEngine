# Phase AT — Audio 3D 空间化 + Mixer + Effects — 验收文档

> **状态**: ✅ **已完成** — 6 平台 CI 全绿 (一次成功, 无修复)
>
> GitHub Actions run: [25602842850](https://github.com/futzhj/ChocoLightEngine/actions/runs/25602842850) (commit `7e8a4e3`)
>
> **范围**: 3 个新 Lua 模块 + Light.Audio 7 fns 扩展 + miniaudio engine 全套深度集成

---

## 一、交付内容

| 维度 | 数量 / 内容 |
|---|---|
| 新 Lua 模块 | 3 (`Light.Audio.Sound` / `Light.Audio.SoundGroup` / `Light.Audio.Effect`) |
| 新 Lua 方法总数 | **44** (Sound 28 + SoundGroup 10 + Effect 5 + Light.Audio 7 fns 扩展 - 但有些重叠) |
| AudioBackend C++ 接口 | 10 (原) + ~40 (新) = 50 个 fns |
| 新 C++ 源文件 | 3 (`light_audio_sound/group/effect.cpp`) |
| 新 C++ 总行数 | **~2300 行** (含 backend 扩展 + 3 模块 + smoke + 文档) |
| **CI 修复次数** | **0** (一次成功) |

---

## 二、API 总览

### 2.1 `Light.Audio.Sound` (28 方法)

```lua
local Sound = require("Light.Audio.Sound")

-- 加载
local s = Sound.Load(path)                                    -- WAV/MP3/FLAC/OGG (miniaudio)
local s = Sound.LoadPCM(data, "f32"|"s16"|"s32"|"u8", ch, sr) -- 自定义 PCM

-- 播放控制 (4)
s:Play() / Pause() / Stop() / IsPlaying() -> bool

-- 音量 / 循环 / 音调 / pan (8)
s:SetVolume(v) / GetVolume()
s:SetLooping(b) / GetLooping()
s:SetPitch(p) / GetPitch()                                    -- 1.0 = 原速
s:SetPan(p) / GetPan()                                        -- [-1, 1]

-- 3D 空间化 (12)
s:Set3DEnabled(b) / Get3DEnabled()
s:SetPosition(x, y, z) / GetPosition() -> x,y,z
s:SetVelocity(x, y, z)                                        -- 多普勒
s:SetAttenuationModel("none"|"inverse"|"linear"|"exp") / GetAttenuationModel()
s:SetMinDistance(d) / GetMinDistance()
s:SetMaxDistance(d) / GetMaxDistance()
s:SetRolloff(r) / GetRolloff()

-- Routing (2)
s:SetGroup(group | nil)                                       -- 加入 mixer
s:SetEffect(effect | nil)                                     -- DSP 链

-- 生命周期 (2)
s:Delete() / __gc / __tostring
```

### 2.2 `Light.Audio.SoundGroup` (10 方法)

```lua
local SoundGroup = require("Light.Audio.SoundGroup")

local g = SoundGroup.New([parent_group | nil])

g:SetVolume(v) / GetVolume()                                  -- 与子 sound 相乘
g:SetPitch(p) / GetPitch()
g:Pause() / Resume() / Stop()
g:SetParent(group | nil) -> ok, [err]                         -- 含循环检测
g:SetEffect(effect | nil)                                     -- (group 级 routing TODO)

g:Delete() / __gc / __tostring
```

### 2.3 `Light.Audio.Effect` (6 工厂 + 3 方法)

```lua
local Effect = require("Light.Audio.Effect")

-- 6 种工厂
Effect.NewLowPass(cutoffHz, [order=4])
Effect.NewHighPass(cutoffHz, [order=4])
Effect.NewBandPass(cutoffHz, [order=4])
Effect.NewNotch(cutoffHz, [q=0.7])
Effect.NewPeak(cutoffHz, gainDB, [q=0.7])
Effect.NewEcho({delay_ms=250, decay=0.5, wet=0.5, dry=0.5})  -- reverb 替代

-- 控制
e:SetEnabled(b) / GetEnabled()
e:Delete() / __gc / __tostring
```

### 2.4 `Light.Audio` 扩展 (7 fns)

```lua
local Audio = require("Light.Audio")

-- Listener (3D 摄像机)
Audio.SetListenerPosition(x, y, z, [idx=0])
Audio.SetListenerDirection(dx, dy, dz, [idx=0])
Audio.SetListenerWorldUp(ux, uy, uz, [idx=0])
Audio.SetListenerVelocity(vx, vy, vz, [idx=0])               -- 多普勒
Audio.GetListenerCount() -> int

-- Master engine
Audio.SetGlobalVolume(v)
Audio.GetGlobalVolume() -> v
```

---

## 三、节点图路由 (miniaudio Node Graph)

每个 Sound 的输出经过动态可重写的链:

```
默认:                  ma_sound -> ma_engine_get_endpoint
仅 Group:              ma_sound -> ma_sound_group -> [parent...] -> endpoint
仅 Effect:             ma_sound -> effect_node -> endpoint
Group + Effect:        ma_sound -> effect_node -> ma_sound_group -> ... -> endpoint
```

`SetGroup/SetEffect` 调用 `ma_node_attach_output_bus` 动态重路由, 无需重建 sound。

---

## 四、6 种 Effect 实现

| Effect | miniaudio 节点 | 用途 |
|---|---|---|
| LowPass | `ma_lpf_node` (Butterworth 级联) | 低通滤波 (柔化) |
| HighPass | `ma_hpf_node` | 高通滤波 (亮化) |
| BandPass | `ma_bpf_node` | 带通 (中频聚焦) |
| Notch | `ma_notch_node` | 陷波 (去除特定频率) |
| Peak | `ma_peak_node` | 峰值/钟形 (EQ 提升) |
| Echo | `ma_delay_node` + wet/dry/decay | **替代 reverb** (miniaudio 无原生 reverb_node) |

---

## 五、循环检测 (Q3 嵌套保护)

```cpp
bool SetGroupParent(GroupHandle* g, GroupHandle* parent) {
    GroupHandle* p = parent;
    while (p) {
        if (p == g) return false;  // 环! 拒绝
        p = p->parent;
    }
    g->parent = parent;
    ma_node_attach_output_bus(GroupInputNode(g), 0, GroupOutputTarget(g), 0);
    return true;
}
```

smoke 测试:
- `g1:SetParent(g2)` 后 `g2:SetParent(g1)` → false + err (拒绝)
- `g1:SetParent(g1)` → false + err (自循环拒绝)
- `g1:SetParent(nil)` → true (合法)

---

## 六、内存管理 (Lua registry refs)

```cpp
struct SoundUserdata {
    AudioHandle* h;
    int groupRef;    // luaL_ref 防 group GC
    int effectRef;   // luaL_ref 防 effect GC
};
struct GroupUserdata {
    GroupHandle* h;
    int parentRef;   // 防 parent group GC
    int effectRef;   // 占位 (Group-level effect TODO)
};
```

`SetGroup/SetEffect/SetParent` 时:
1. `luaL_unref` 旧 ref
2. `lua_pushvalue + luaL_ref` 新 ref (防 GC)
3. 调 AudioBackend 重路由

`__gc` 时全部 unref + 释放 backend handle。

---

## 七、Q1~Q5 锁定决策回顾

| Q | 决策 | 实施 |
|---|---|---|
| Q1 Sound 与 SDL3 关系 | A — 独立 | ✅ 基于 miniaudio engine, 与 light_audio.cpp 平行 |
| Q2 衰减模型 | A — 字符串 | ✅ "none"/"inverse"/"linear"/"exp" |
| Q3 SoundGroup 嵌套 | B — 任意 + 循环检测 | ✅ SetGroupParent 含 while 链遍历 |
| Q4 Listener 数 | A — 1 (idx=0) | ✅ 默认 listener 0, 接口支持 idx 参数 |
| Q5 释放方式 | A — __gc + 显式 | ✅ 双重保险 |

---

## 八、未实施项 (Phase AT.x 留更后续)

- ❌ **Group 级 Effect routing**: 占位接口, 实际未连 backend (group output 经 effect)
- ❌ **真正 Reverb**: 用 Echo 替代; miniaudio 无原生 reverb_node, 需自定义 ma_node_vtable
- ❌ **多 Listener (split-screen)**: 当前默认 1
- ❌ **HRTF / binaural 空间化**: 需第三方库 (steam_audio / Resonance Audio)
- ❌ **Streaming (大文件流式)**: miniaudio 默认全量加载

---

## 九、Smoke 6 段验证

`scripts/smoke/audio_3d_mixer_effect.lua` 50+ 断言全部 PASS:

1. **模块加载**: Sound/SoundGroup/Effect 都加载 ✅
2. **Sound 工厂**: nil/bogus 格式/空数据全部正确返回 nil + err ✅
3. **SoundGroup 嵌套**: 循环检测拒绝, 合法 SetParent 通过 ✅
4. **Effect 6 种工厂**: LowPass/HighPass/BandPass/Notch/Peak/Echo 全部工作 ✅
5. **Light.Audio 7 fns**: Listener + GlobalVolume 全部存在 + 边界 ✅
6. **Sound 运行时**: 28 方法全部 callable, Set/Get round-trip 一致 ✅

---

## 十、CI 验收

- [x] `lightc -p audio_3d_mixer_effect.lua` Exit=0 (本地)
- [x] **GitHub Actions 6 平台全绿** (run [25602842850](https://github.com/futzhj/ChocoLightEngine/actions/runs/25602842850)):
  - [x] Windows x64 ✅
  - [x] Linux x64 ✅
  - [x] macOS Universal ✅
  - [x] Android arm64 + x86_64 ✅
  - [x] iOS arm64 ✅
  - [x] Web WASM ✅

---

**Phase AT 一次提交即全平台通过, 含 Q1-Q5 全推荐方案 + 含 Effects 全套**。

---

## 十一、累计进度 (本会话)

| Phase | 主题 | C++ 行 | CI 修复 |
|---|---|---|---|
| AQ | TextInput + IME | ~600 | 0 |
| AR | SDL3 扫尾 | ~1340 | 1 |
| AS.1 | Canvas + Shader | ~1100 | 0 |
| AS.2 | 3D Mesh 基础 | ~1385 | 0 |
| AS.3 | cgltf + glTF/glb | ~7733 (含 cgltf.h) | 0 |
| AS.4 | 完整材质系统 | ~1100 | 1 |
| AS.4.x | glTF 材质提取 | ~200 | 0 |
| **AT** | **Audio 3D + Mixer + Effects** | **~2300** | **0** |
| **总计** | | **~15760 (~7900 我方代码)** | **2/8** |
