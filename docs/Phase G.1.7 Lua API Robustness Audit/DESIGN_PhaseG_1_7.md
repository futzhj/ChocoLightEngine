# Phase G.1.7 — Lua API 容错 audit (DESIGN)

> **创建日期**: 2026-05-19
> **基于**: ALIGNMENT_PhaseG_1_7.md (方案 C — 全面重构)
> **总工时**: 30-40h+, 拆 G.1.7.0~G.1.7.5 六个子阶段

---

## 一. 整体架构

### 1.1 核心组件

```
┌────────────────────────────────────────────────────────────────┐
│  light_lua_helpers.h (新增 - 私有头, 不导出)                     │
│  ├── LT::Magic4(a,b,c,d) → uint32_t (constexpr, 跨平台一致)     │
│  ├── LT::CheckInstance<T>(L, idx, magic, typeName) → T*         │
│  ├── LT::TryGetInstance<T>(L, idx) → T*  (兼容老代码)           │
│  ├── LT::CheckStringStrict / OptStringStrict (拒绝 number)      │
│  ├── LT::PushNilError(L, fmt, ...) → int 2                       │
│  └── LT::PushBooleanError(L, fmt, ...) → int 2                   │
└────────────────────────────────────────────────────────────────┘
                            ↓
┌────────────────────────────────────────────────────────────────┐
│  各 light_*.cpp ctx struct 改造                                  │
│  ┌──────────────────────────────┐                                │
│  │ struct ImageContext {         │                                │
│  │   uint32_t magic;       ← 新  │  // LT_MAGIC_IMAGE             │
│  │   unsigned int texId;         │                                │
│  │   int width, height;          │                                │
│  │ };                            │                                │
│  └──────────────────────────────┘                                │
│  分配时: ctx->magic = LT_MAGIC_IMAGE;                            │
│  读取时: LT::CheckInstance<ImageContext>(L, 1, ..., "Image")     │
└────────────────────────────────────────────────────────────────┘
                            ↓
┌────────────────────────────────────────────────────────────────┐
│  scripts/smoke/lua_api_robustness.lua (新增)                     │
│  100+ fuzz 用例, 类型混淆 / nil / 越界 / 错 OOP table            │
└────────────────────────────────────────────────────────────────┘
```

### 1.2 数据流 (改造后)

```
Lua 用户:   db:Query("SELECT ...")
              │
              ↓
C 入口:     l_DB_Query(L)
              │
              ↓
helper:    SQLiteContext* ctx = LT::CheckInstance<SQLiteContext>(
                L, 1, LT_MAGIC_DB, "Light.DB");
              │
              ↓ (失败时 → luaL_error 抛出, 不 crash)
              ↓ (成功时)
业务逻辑:   sqlite3_exec(ctx->db, ...);
```

---

## 二. helpers 设计 (light_lua_helpers.h)

### 2.1 Magic 编码

```cpp
namespace LT {

/// 4 字符 magic 编码 (LE/BE 都一致, 因为读写都用同一 macro)
constexpr uint32_t Magic4(char a, char b, char c, char d) noexcept {
    return  uint32_t(uint8_t(a))
         | (uint32_t(uint8_t(b)) << 8)
         | (uint32_t(uint8_t(c)) << 16)
         | (uint32_t(uint8_t(d)) << 24);
}

}  // namespace LT
```

### 2.2 通用 instance check

```cpp
namespace LT {

/// 验证 OOP `__instance` userdata + magic; 失败抛 Lua 错误
/// T 必须以 `uint32_t magic` 作为首字段
template <typename T>
inline T* CheckInstance(lua_State* L, int idx, uint32_t expectedMagic, const char* typeName) {
    if (!lua_istable(L, idx)) {
        luaL_typerror(L, idx, typeName);
        return nullptr;  // unreachable
    }
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "%s: expected instance table with __instance userdata", typeName);
        return nullptr;
    }
    void* raw = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!raw) {
        luaL_error(L, "%s: __instance is null", typeName);
        return nullptr;
    }
    // magic 在 ctx struct 首字段
    uint32_t actualMagic = *static_cast<uint32_t*>(raw);
    if (actualMagic != expectedMagic) {
        luaL_error(L,
            "%s: type confusion detected (magic mismatch: got 0x%08X, expected 0x%08X)",
            typeName, actualMagic, expectedMagic);
        return nullptr;
    }
    return static_cast<T*>(raw);
}

/// 不抛错版本 (返 nullptr); 用于可选参数 / 老代码迁移过渡期
template <typename T>
inline T* TryCheckInstance(lua_State* L, int idx, uint32_t expectedMagic) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    void* raw = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!raw) return nullptr;
    if (*static_cast<uint32_t*>(raw) != expectedMagic) return nullptr;
    return static_cast<T*>(raw);
}

}  // namespace LT
```

### 2.3 字符串严格校验

```cpp
namespace LT {

/// 严格 string 校验 (拒绝 number 隐式转换); luaL_check 风格
inline const char* CheckStringStrict(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TSTRING) {
        luaL_typerror(L, idx, "string");
        return nullptr;  // unreachable
    }
    return lua_tostring(L, idx);
}

/// 可选 string (nil → def, number 拒绝)
inline const char* OptStringStrict(lua_State* L, int idx, const char* def) {
    int t = lua_type(L, idx);
    if (t == LUA_TNONE || t == LUA_TNIL) return def;
    if (t != LUA_TSTRING) {
        luaL_typerror(L, idx, "string or nil");
        return nullptr;
    }
    return lua_tostring(L, idx);
}

}  // namespace LT
```

### 2.4 错误返回辅助

```cpp
namespace LT {

/// 推 nil + err 字符串 (纯返回风格 API 用); 总返 2
inline int PushNilError(lua_State* L, const char* fmt, ...) {
    lua_pushnil(L);
    va_list ap; va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    return 2;
}

/// 推 false + err (boolean 风格 API 用); 总返 2
inline int PushBooleanError(lua_State* L, const char* fmt, ...) {
    lua_pushboolean(L, 0);
    va_list ap; va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    return 2;
}

}  // namespace LT
```

---

## 三. Magic 常量表

### 3.1 全 ctx 类 (跨子阶段汇总)

| Magic constant | 值 | 文件 | ctx struct | 子阶段 |
|----------------|----|----|----|--------|
| `LT_MAGIC_IMAGE` | `'IMGE'` | light_graphics_image.cpp | ImageContext | G.1.7.0 |
| `LT_MAGIC_CANVAS` | `'CNVS'` | light_graphics_canvas.cpp | CanvasContext | G.1.7.0 |
| `LT_MAGIC_FONT` | `'FONT'` | light_graphics.cpp (inline) | FontCtxHeader | G.1.7.0 |
| `LT_MAGIC_DB` | `'SQLI'` | light_db.cpp | SQLiteContext | G.1.7.0 |
| `LT_MAGIC_AV` | `'AVCT'` | light_av.cpp | AVContext | G.1.7.0 |
| `LT_MAGIC_VIDEO` | `'VIDO'` | light_av.cpp | VideoWrapper | G.1.7.0 |
| `LT_MAGIC_DATABUF` | `'DBUF'` | light_data.cpp | DataBuffer | G.1.7.0 |
| `LT_MAGIC_HTTPCTX` | `'HTTP'` | light_network.cpp | HttpContext | G.1.7.0 |
| `LT_MAGIC_EMITTER` | `'EMIT'` | light_particles.cpp | ParticleEmitter | G.1.7.0 |
| `LT_MAGIC_TILEMAP` | `'TLMP'` | light_tilemap.cpp | TilemapData | G.1.7.0 |
| `LT_MAGIC_WORLD` | `'WRLD'` | light_physics.cpp | PhysicsWorld | G.1.7.3 |
| `LT_MAGIC_BODY` | `'BODY'` | light_physics.cpp | PhysicsBody | G.1.7.3 |
| `LT_MAGIC_SHAPE` | `'SHPE'` | light_physics.cpp | PhysicsShape | G.1.7.3 |
| `LT_MAGIC_FIXTURE` | `'FXTR'` | light_physics.cpp | PhysicsFixture | G.1.7.3 |
| `LT_MAGIC_JOINT` | `'JNTI'` | light_physics.cpp | PhysicsJoint | G.1.7.3 |
| `LT_MAGIC_AUDIO_SRC` | `'ASRC'` | light_audio_sound.cpp | (TBD) | G.1.7.2 |
| `LT_MAGIC_AUDIO_GRP` | `'AGRP'` | light_audio_group.cpp | (TBD) | G.1.7.2 |
| `LT_MAGIC_AUDIO_FX` | `'AFX0'` | light_audio_effect.cpp | (TBD) | G.1.7.2 |
| `LT_MAGIC_NEM` | `'NEMC'` | light_plugins.cpp | NEMContext | G.1.7.4 |
| `LT_MAGIC_WDF` | `'WDFC'` | light_plugins.cpp | WDFContext | G.1.7.4 |
| `LT_MAGIC_NET_UDP` | `'NUDP'` | light_network_udp.cpp | (TBD) | G.1.7.2 |
| `LT_MAGIC_NET_RPC` | `'NRPC'` | light_network_rpc.cpp | (TBD) | G.1.7.2 |
| `LT_MAGIC_NET_ROOM` | `'NROM'` | light_network_room.cpp | (TBD) | G.1.7.2 |
| `LT_MAGIC_NET_WEB` | `'NWEB'` | light_network_web.cpp | WebHttpContext | G.1.7.2 |
| `LT_MAGIC_ANIMATOR` | `'ANIM'` | light_animation.cpp | (已有 luaL_checkudata, 评估是否需要) | G.1.7.3 |
| `LT_MAGIC_SKELETON` | `'SKEL'` | light_animation.cpp | (同上) | G.1.7.3 |
| `LT_MAGIC_CLIP` | `'CLIP'` | light_animation.cpp | (同上) | G.1.7.3 |
| `LT_MAGIC_SKIN` | `'SKIN'` | light_animation.cpp | (同上) | G.1.7.3 |
| `LT_MAGIC_BATCH` | `'BTCH'` | light_asset.cpp | BatchHandleUd | (G.1.6 已用 luaL_checkudata, 跳过) |
| (其余 30+) | ... | ... | ... | G.1.7.4~5 |

**总计**: ~40 个 magic 常量, 全部声明在 `light_lua_helpers.h` 顶部, constexpr 编译期求值.

### 3.2 Magic 选取规则

1. 4 字符 ASCII printable
2. 与 ctx 类型语义关联 (e.g. 'IMGE' for ImageContext)
3. 同子系统前缀 (e.g. 'A' for Audio, 'N' for Network)
4. 避免常见词以减少意外冲突 (e.g. 'NULL' 不用)
5. 全表唯一 — DESIGN 一次性敲定全部值

---

## 四. ctx struct 改造范式

### 4.1 改造前 (light_graphics_image.cpp 现状)

```cpp
struct ImageContext {
    unsigned int texId;
    int          width;
    int          height;
    // ... 其他字段
};
```

### 4.2 改造后 (G.1.7.0 范式)

```cpp
#include "light_lua_helpers.h"

constexpr uint32_t LT_MAGIC_IMAGE = LT::Magic4('I','M','G','E');

struct ImageContext {
    uint32_t     magic;          // ← 新增首字段, 必须为 LT_MAGIC_IMAGE
    unsigned int texId;
    int          width;
    int          height;
    // ... 其他字段
};

// 创建时 (e.g. l_NewImage)
ImageContext* ctx = (ImageContext*)lua_newuserdata(L, sizeof(ImageContext));
ctx->magic  = LT_MAGIC_IMAGE;
ctx->texId  = ...;
ctx->width  = ...;
ctx->height = ...;
// 关联 OOP table 等...
```

### 4.3 GetXxxCtx 函数改造

```cpp
// 改造前:
static ImageContext* GetImageCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    ImageContext* ctx = (ImageContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

// 改造后:
static ImageContext* CheckImage(lua_State* L, int idx) {
    return LT::CheckInstance<ImageContext>(L, idx, LT_MAGIC_IMAGE, "Light.Image");
}
// 调用方原 nullptr 返回路径自然消失 (CheckInstance 失败抛错)
```

### 4.4 兼容迁移过渡

- 老代码 `if (!ctx) return 0;` 路径**保留** — `CheckInstance` 在错参时已抛错, ctx 不可能为 null
- 但 `ctx->magic == 0` 的 stale userdata (e.g. 已 free) 仍会被 magic 检查捕获 → 抛 type confusion
- `__gc` 析构后建议 `ctx->magic = 0xDEADBEEF` (use-after-free 检测)

---

## 五. 子阶段执行计划

### 5.1 G.1.7.0 (本期会话目标)

**范围**:
- ✅ helpers 头 (light_lua_helpers.h)
- ✅ Magic 常量表 (全 40 个声明完, 仅 P0+P1 文件应用)
- ✅ 9 个 P0+P1 文件改造:
  - `light_graphics_image.cpp` (ImageContext)
  - `light_graphics_canvas.cpp` (CanvasContext)
  - `light_graphics.cpp` (FontCtxHeader inline + 两处 anon CanvasCtx)
  - `light_db.cpp` (SQLiteContext)
  - `light_av.cpp` (AVContext + VideoWrapper)
  - `light_data.cpp` (DataBuffer)
  - `light_network.cpp` (HttpContext)
  - `light_particles.cpp` (ParticleEmitter)
  - `light_tilemap.cpp` (TilemapData)
- ✅ smoke 框架 + 30+ 用例
- ✅ progress.txt
- ✅ commit + CI

**估时**: 6-8h

### 5.2 G.1.7.1 — Graphics 子系统其余

**范围** (~15 文件):
- `light_graphics_mesh.cpp`
- `light_graphics_shader.cpp`
- `light_graphics_material.cpp`
- `light_graphics_spriteanimation.cpp`
- `light_lighting2d.cpp`
- `light_camera.cpp`
- `light_blendmode.cpp`
- `light_pixels.cpp`
- `light_surface.cpp`
- `light_cursor.cpp`
- `light_display.cpp`
- (其他 graphics 相关)

**估时**: 6-8h

### 5.3 G.1.7.2 — Audio + Network 子系统

**范围** (~15 文件):
- `light_audio.cpp` / `light_audio_backend.cpp` / `light_audio_effect.cpp` / `light_audio_group.cpp` / `light_audio_sound.cpp`
- `light_network_udp.cpp` / `light_network_rpc.cpp` / `light_network_room.cpp` / `light_network_web.cpp`
- (其他 io / messaging)

**估时**: 6-8h

### 5.4 G.1.7.3 — Physics + Animation + ECS

**范围** (~15 文件):
- `light_physics.cpp` (5 helpers 全改 magic)
- `light_physics3d.cpp`
- `light_animation.cpp` (评估是否需要 magic, 已有 luaL_checkudata)
- `light_ecs.cpp`
- (其他 game logic 相关)

**估时**: 6-8h

### 5.5 G.1.7.4 — 系统类剩余

**范围** (~20 文件):
- `light_filesystem.cpp` (已是标杆, 可能仅做 smoke)
- `light_input.cpp` / `light_keyboard.cpp` / `light_mouse.cpp` / `light_gamepad.cpp` / `light_joystick.cpp` / `light_touch.cpp`
- `light_clipboard.cpp` / `light_dialog.cpp` / `light_messagebox.cpp` / `light_tray.cpp`
- `light_time.cpp` / `light_log.cpp` / `light_locale.cpp` / `light_url.cpp`
- (剩余系统类)

**估时**: 6-8h

### 5.6 G.1.7.5 — 完结 + 全 fuzz smoke

**范围**:
- 最终 fuzz smoke 100+ 用例
- benchmark (magic 检查开销 ~3-5 ns / 调用, 实测验证)
- 文档收尾 (FINAL + ACCEPTANCE + TODO)
- HANDOFF 同步

**估时**: 4-5h

---

## 六. smoke 测试设计

### 6.1 测试维度 (每子阶段补充)

**A. nil 参数 → 不 crash**
```lua
local ok, err = pcall(Light.Graphics.NewImage)
assert(not ok and err:match("string"))
```

**B. 错类型 → 不 crash**
```lua
local ok = pcall(function() canvas:Clear({}) end)  -- table 当 number
assert(not ok)
```

**C. 错 OOP table (fake __instance) → magic 检查抛错**
```lua
local img = Light.Graphics.NewImage("a.png")
local fake = setmetatable({__instance = img.__instance}, getmetatable(realCanvas))
local ok, err = pcall(function() fake:GetTextureId() end)
assert(err:match("type confusion"))
```

**D. 错 userdata 外壳 (raw lightuserdata 当 OOP table)**
```lua
local ok = pcall(function()
    Light.Graphics.SetCanvas(123)  -- number 当 canvas
end)
assert(not ok)
```

**E. nil __instance**
```lua
local fake = setmetatable({__instance = nil}, getmetatable(realDb))
local ok, err = pcall(function() fake:Query("...") end)
assert(err:match("__instance"))
```

**F. 错 ctx 类型 (Image userdata 当 Canvas)**
```lua
local img = Light.Graphics.NewImage("a.png")
local fake = setmetatable({__instance = (...img的userdata)}, getmetatable(realCanvas))
local ok, err = pcall(function() fake:Clear() end)
assert(err:match("magic mismatch"))
```

**G. 越界 index** (现有的 luaL_optnumber 已覆盖, 仅 sanity)

**H. 长字符串 / 二进制安全 / utf-8** (字符串 API)

### 6.2 smoke 输出

```
=== Phase G.1.7 Lua API Robustness smoke ===
[G.1.7.0] PASS: A1-A5 nil parameters
[G.1.7.0] PASS: B1-B5 wrong type primitives
[G.1.7.0] PASS: C1-C9 type confusion via fake __instance
[G.1.7.0] PASS: D1-D3 raw lightuserdata rejection
[G.1.7.0] PASS: E1-E5 nil __instance
[G.1.7.0] PASS: F1-F8 magic mismatch detection
=== Phase G.1.7.0 (35 cases) ALL PASS ===
```

---

## 七. CI 接入

### 7.1 Windows runtime smoke

`@e:/jinyiNew/Light/.github/workflows/build-templates.yml` Windows job 增加:

```yaml
- name: Run Phase G.1.7 robustness smoke
  run: |
    & "$runtimeDir\light.exe" "$env:GITHUB_WORKSPACE\scripts\smoke\lua_api_robustness.lua"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

### 7.2 全套既有 smoke 零回归

每子阶段末确认现有 30+ smoke 全 PASS (与 G.1.6 同模式).

---

## 八. 性能影响评估

### 8.1 magic 检查开销

```
每次 CheckInstance 调用:
  - lua_istable: ~3 ns
  - lua_getfield(__instance): ~10 ns (string lookup)
  - lua_isuserdata + lua_touserdata + lua_pop: ~5 ns
  - magic 比较: ~1 ns
  - 总计: ~20 ns / 调用
```

对比改造前:
```
  - lua_getfield + lua_isuserdata + lua_touserdata + lua_pop: ~18 ns
  - 增量: ~2 ns
```

**结论**: 性能影响 < 1% (即使每帧 1000 次 API 调用也仅 20μs / 16.7ms 帧). 可接受.

### 8.2 ctx struct 加 4 字节 magic

绝大多数 ctx ≥ 32B, 增加 4B 后对齐保持. 内存影响 < 1%.

---

## 九. 风险与缓解

| 风险 | 级别 | 缓解 |
|------|------|------|
| 字段顺序破坏 ABI (如插件预编译) | 🔴 高 | ChocoLight 没有外部 C ABI 用户; ctx 全是 anonymous namespace 私有 |
| Lua 5.1 lua_pushvfstring 兼容性 | 🟢 低 | lumen-master/lib/lua/auxiliary.cpp 已 shim |
| 跨平台 magic 字节序 | 🟢 低 | LT::Magic4 编译期值, 读写一致 |
| smoke 触发现存其他 bug | 🟡 中 | 修了再 PASS, 是预期收益 |
| 子阶段间 commit 不一致状态 | 🟡 中 | helpers + 全 magic 表一次声明完, 各文件改造 idempotent |

---

## 十. 验收标准

### 10.1 子阶段级

每子阶段:
- 所有目标 ctx struct 加 magic 字段
- 所有 GetXxxCtx 函数走 LT::CheckInstance
- 所有创建路径设置 magic
- 该批 smoke 用例 PASS
- 全套既有 smoke 零回归

### 10.2 整体 (G.1.7.5 完结)

- 79 个 binding 文件全部 audit
- 30+ ctx struct 全部 magic 化
- fuzz smoke 100+ 用例全 PASS
- 6 平台 build green
- benchmark 显示性能影响 < 1%
- 完整文档 (ALIGNMENT/DESIGN/TASK/FINAL/ACCEPTANCE/TODO) 6 件套

---

## 十一. 进入 Atomize 阶段

下一步: TASK_PhaseG_1_7.md 拆 G.1.7.0~G.1.7.5 全部子任务清单.
