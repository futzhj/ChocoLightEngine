# Phase AN — Light.Audio 回调补完 — Alignment

## 0. 目标

补完 Phase AM 的 5 个 out-of-scope 回调 API 中**技术可行**的 4 个,不绑定 1 个高风险的跨线程回调(postmix)。

## 1. 现状

Phase AM 已绑 51 fns,未绑 5 fns:

| fn | 线程模型 | 技术风险 |
|---|---|---|
| `SDL_LoadWAV_IO` | 主线程同步 | 低 — 需要跨模块 IOStream handle |
| `SDL_GetAudioStreamProperties` | 主线程同步 | 零 — SDL_PropertiesID 是 Uint32,直接返回 |
| `SDL_SetAudioStreamGetCallback` | 主线程同步(`PutAudioStreamData` 触发) | 中 — Lua ref 生命周期管理 |
| `SDL_SetAudioStreamPutCallback` | 主线程同步(`GetAudioStreamData` 触发) | 中 — 同上 |
| `SDL_SetAudioPostmixCallback` | **真音频线程** | 高 — 跨线程 lua_State 竞态 |

**关键发现**(读 SDL3 源码 `SDL_audiocvt.c`):
- `put_callback` / `get_callback` 在 **调用者线程** (即 Lua 主线程) 触发 ← 可安全直接绑定
- `postmix_callback` 在 **音频硬件线程** 触发 ← 必须 deferred 机制,不能在该线程直接跑 Lua

## 2. 绑定决策

**绑定 4 fns**:

1. **`LoadWAV_IO(iostream_ud, closeio)` → (spec, bytes, err)**
   - 接收 `Light.IOStream` userdata (metatable `"Light.IOStream.Stream"`)
   - 跨模块 handle 共享: 用 `luaL_checkudata` 取 `LIOStream* → io`
   - closeio=true 时 SDL 接管 IO 生命周期,需 `LIOStream.io = NULL` 避免 GC double-close

2. **`GetAudioStreamProperties(stream_ud)` → (props_id, err)**
   - 直接 `SDL_GetAudioStreamProperties` → push Uint32 作为 integer
   - 返回的 id 可直接传给 `Light.Properties.*` fns

3. **`SetAudioStreamGetCallback(stream_ud, lua_fn|nil, user_arg?)` → (ok, err)**
   - fn 在 `SDL_GetAudioStreamData` 内被同步调用 (主线程)
   - 实现: Lua fn 存到 registry (luaL_ref),C wrapper 被调用时 rawgeti 取出并调
   - nil fn = 清除 callback + luaL_unref

4. **`SetAudioStreamPutCallback(stream_ud, lua_fn|nil, user_arg?)` → (ok, err)**
   - 同上,在 `SDL_PutAudioStreamData` 触发

**不绑定 1 fn**(`SDL_SetAudioPostmixCallback`):

- 理由 1: 音频线程调用 Lua callback 需要跨线程 lua_State 锁,主线程会被音频回调频率(48kHz / buffer_size = ~100Hz)阻塞
- 理由 2: deferred queue 方案(音频线程 copy buffer → Lua poll)会丢失 postmix 的核心价值(修改 mix),退化为"只读 monitor"
- 理由 3: 对游戏引擎用例(音乐 + SFX)非必需;若用户需要 DSP 可用 `ConvertAudioSamples` + `MixAudio` 在 Lua 端处理
- **延后**: 若未来需要,开独立 Phase (设计 postmix-specific ring buffer + lockfree SPSC 通道)

## 3. 技术设计

### 3.1 跨模块 handle 约定

- **IOStream**: metatable `"Light.IOStream.Stream"` 全局共享名,任何模块 `luaL_checkudata(L, idx, "Light.IOStream.Stream")` 即可
- **Properties**: SDL_PropertiesID 是 Uint32,直接 integer,无需 handle 转换

### 3.2 Callback ref 管理

在 light_audio.cpp 新增:

```cpp
struct StreamCallbackRefs {
    int get_fn_ref = LUA_NOREF;   // Lua fn registry ref
    int get_arg_ref = LUA_NOREF;  // 可选 user_arg registry ref
    int put_fn_ref = LUA_NOREF;
    int put_arg_ref = LUA_NOREF;
    lua_State* L = nullptr;       // 挂 callback 的 lua_State
};
std::map<SDL_AudioStream*, StreamCallbackRefs> g_stream_callbacks;
```

- `DestroyAudioStream` 时清理所有 ref (luaL_unref) + erase map entry
- `SetXxxCallback(stream, nil)` 时清理对应 ref

### 3.3 递归保护

- 若 Lua callback 在 fn 内又调 Put/GetAudioStreamData → 无限递归崩栈
- 简单方案: per-stream `bool in_callback` flag,递归时 SDL 调用在 C wrapper 直接返回不再触发 Lua

### 3.4 LoadWAV_IO closeio 语义

```cpp
LIOStream* h = (LIOStream*)luaL_checkudata(L, 1, "Light.IOStream.Stream");
bool closeio = lua_toboolean(L, 2);
if (!h->io) luaL_error(L, "iostream is closed");
SDL_IOStream* io = h->io;
SDL_AudioSpec spec; Uint8* buf = nullptr; Uint32 len = 0;
bool ok = SDL_LoadWAV_IO(io, closeio, &spec, &buf, &len);
if (closeio) h->io = nullptr;  // SDL took ownership, clear 以避 GC double-free
```

## 4. 验收

- 4 fns 全注册,`Audio` table keys 从 69 → 73
- LoadWAV_IO: 创建 IOStream from mem (WAV 字节) → LoadWAV_IO → 对比 LoadWAV(path) 结果一致
- GetAudioStreamProperties: 返回的 id > 0,`Light.Properties.HasProperty` 能查询
- Set/Clear callback: Lua counter 记数准确,nil 清除不崩
- 递归保护: callback 内部调 Get/PutAudioStreamData 不崩
- GC + DestroyAudioStream 清理 callback ref 不泄漏

## 5. 非目标

- 不绑 postmix (理由见 §2)
- 不实现 audio thread deferred queue
- 不改 Phase AM 已绑的 51 fns

## 6. 工作量

- light_audio.cpp: +~250 行
- scripts/smoke/audio.lua: +~130 行
- 2-3 小时

无需中断询问,直接实施。
