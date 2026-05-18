# Light Engine 类型安全设计 (Type Safety)

> **范围**: Lua ↔ C++ 互操作中的 userdata 类型安全机制
> **基线**: Phase G.1.7 完成 (25 文件 / 42 ctx struct)
> **目标**: Lua 端任意错参 / 攻击都不应导致 C++ 层 segfault 或未定义行为

---

## 一. 核心问题

### 1.1 Lua userdata 攻击向量

Lua 5.1/LuaJIT 的 userdata 机制天然不安全:

```lua
-- 攻击 1: 构造 fake __instance, metatable 替换
local fakeImage = {__instance = realImage.__instance}
setmetatable(fakeImage, getmetatable(realMesh))  -- 把 Image 当 Mesh
local v = mesh:GetVertexCount(fakeImage)  -- C++ reinterpret_cast → segfault

-- 攻击 2: nil __instance
local fakeObj = setmetatable({}, getmetatable(realImage))
realImage.GetWidth(fakeObj)  -- nullptr deref

-- 攻击 3: __gc 后再用
img:Delete()
img:GetWidth()  -- use-after-free

-- 攻击 4: 错类型 userdata (raw lightuserdata 套到 OOP API)
local raw = some_raw_pointer()
realImage.GetWidth(raw)  -- 把 raw ptr 当 ImageContext
```

### 1.2 历史风险

Phase G.1.7 之前, ChocoLight 的 79 个 Lua binding 文件中:
- 60% 用 `lua_isuserdata` + `lua_touserdata` 但无类型校验
- 30% 用 `luaL_checkudata` (metatable 名校验) — 已基本安全
- 10% 用 `__instance` field + 手动 cast — 完全不安全 (无任何校验)

任意 Lua 脚本错参/恶意脚本都可能导致 C++ 层崩溃.

---

## 二. 解决方案: 双保险防御

### 2.1 设计原则

对每个 Lua-facing userdata, 提供 **两层独立** 的类型校验:

1. **Layer 1: Metatable 名校验** (Lua VM 层)
   - 用 `luaL_checkudata(L, idx, "Light.Module.Type")`
   - Lua VM 验证 userdata 关联的 metatable name 与期望匹配
   - 防止 user 把 type A 的 userdata 直接传给 type B 的 API
   - 但 **攻击者可以构造 fake `__instance`** 绕过这层

2. **Layer 2: Magic 首字段校验** (C++ 层)
   - 每个 ctx struct 第一字段是 `uint32_t magic`
   - Check 函数读取 `*((uint32_t*)ud_ptr)` 与预期 magic 比对
   - 攻击者无法伪造 magic, 除非 corruption 内存

### 2.2 防御矩阵

| 攻击向量 | Layer 1 (Metatable) | Layer 2 (Magic) |
|---------|---------------------|------------------|
| `__instance` 替换 (跨类型) | ❌ 绕过 | ✅ 拒绝 |
| Metatable 替换 (raw userdata) | ✅ 拒绝 | ✅ 拒绝 |
| nil __instance | ✅ 拒绝 | ✅ 拒绝 |
| 错类型 userdata (raw lightuserdata) | ✅ 拒绝 | ✅ 拒绝 |
| Use-after-free (`__gc` 后再用) | ❌ 绕过 | ✅ 拒绝 (DEAD magic) |
| 内存 corruption | ❌ 不可能拒绝 | ❌ 不可能拒绝 |

**双保险**: 至少一层就足以拒绝多数攻击, 两层叠加 = 任何单层绕过都被另一层兜底.

---

## 三. 实现规范

### 3.1 步骤 1: 在 ctx struct 第一字段加 magic

```cpp
// light_lua_helpers.h 已声明 magic 常量
#include "light_lua_helpers.h"

struct ImageContext {
    uint32_t     magic;     // ← 必须为首字段
    unsigned int texId;
    int          width;
    int          height;
    // ...
};
```

### 3.2 步骤 2: 创建路径设置 magic

```cpp
static int l_Image_Call(lua_State* L) {
    auto* ctx = (ImageContext*)lua_newuserdata(L, sizeof(ImageContext));
    memset(ctx, 0, sizeof(ImageContext));
    ctx->magic = LT::LT_MAGIC_IMAGE;  // ← 立即设置 magic
    ctx->texId = ...;
    // ...
}
```

### 3.3 步骤 3: Check 函数走 helpers

**对带 metatable 的 userdata** (推荐):

```cpp
static ImageContext* CheckImage(lua_State* L, int idx) {
    auto* ctx = (ImageContext*)luaL_checkudata(L, idx, IMAGE_MT);
    if (ctx && ctx->magic != LT::LT_MAGIC_IMAGE) {
        luaL_error(L, "Light.Image: type confusion at arg #%d", idx);
    }
    return ctx;
}
```

**对 `__instance`-style userdata** (`Light.Image:GetWidth(...)`):

```cpp
static ImageContext* GetImageCtx(lua_State* L, int idx) {
    return LT::TryCheckInstance<ImageContext>(L, idx, LT::LT_MAGIC_IMAGE);
}
```

`TryCheckInstance` 自动:
1. 读 `lua_getfield(L, idx, "__instance")`
2. 校验 lua_isuserdata
3. 取 userdata 指针, 读首字段 magic
4. 比对 magic, 不匹配返回 `nullptr`

### 3.4 步骤 4: __gc 路径标 DEAD

```cpp
static int l_Image_GC(lua_State* L) {
    auto* ctx = (ImageContext*)luaL_checkudata(L, 1, IMAGE_MT);
    if (ctx->magic == LT::LT_MAGIC_DEAD) return 0;  // 重复 __gc 防腔
    ctx->magic = LT::LT_MAGIC_DEAD;  // ← 标记为已销毁
    if (ctx->texId) {
        g_render->DeleteTexture(ctx->texId);
        ctx->texId = 0;
    }
    return 0;
}
```

---

## 四. 添加新 ctx 类型的步骤

### 4.1 申请新 magic

在 `@e:\jinyiNew\Light\ChocoLight\src\light_lua_helpers.h` 加常量:

```cpp
// 在合适分组下
constexpr uint32_t LT_MAGIC_YOURTYPE = Magic4('Y','O','U','R');  // your_module.cpp
```

`Magic4('A','B','C','D')` 编译期计算 little-endian uint32 = 'A' | 'B'<<8 | 'C'<<16 | 'D'<<24.

### 4.2 改造 ctx struct

```cpp
struct YourContext {
    uint32_t magic;   // ← 第一字段必须是这个
    // ... 其他字段
};
```

### 4.3 改造创建路径

```cpp
auto* ctx = (YourContext*)lua_newuserdata(L, sizeof(YourContext));
memset(ctx, 0, sizeof(YourContext));  // 或 placement-new
ctx->magic = LT::LT_MAGIC_YOURTYPE;
```

### 4.4 改造 Check / Get 函数

```cpp
static YourContext* GetYourCtx(lua_State* L, int idx) {
    return LT::TryCheckInstance<YourContext>(L, idx, LT::LT_MAGIC_YOURTYPE);
}
```

### 4.5 改造 __gc 路径

```cpp
static int l_YourType_GC(lua_State* L) {
    auto* ctx = (YourContext*)lua_touserdata(L, 1);
    if (!ctx || ctx->magic == LT::LT_MAGIC_DEAD) return 0;
    ctx->magic = LT::LT_MAGIC_DEAD;
    // ... 释放资源
    return 0;
}
```

### 4.6 加 smoke 测试

在 `@e:\jinyiNew\Light\scripts\smoke\lua_api_robustness.lua` 加测试用例:

```lua
-- 测试 nil 参数
local good = pcall(YourType.SomeMethod, nil)
if not good then ok("nil 参数被拒绝") end

-- 测试 type confusion
local fake = setmetatable({__instance = otherCtx.__instance}, getmetatable(yourObj))
local good = pcall(fake.SomeMethod, fake)
ok("type confusion 不崩 (magic 拒绝)")
```

---

## 五. helpers API 参考

### 5.1 `LT::Magic4`

```cpp
constexpr uint32_t magic = LT::Magic4('I','M','G','E');
// → 'I' | 'M'<<8 | 'G'<<16 | 'E'<<24
```

### 5.2 `LT::CheckInstance<T>` (严格模式)

```cpp
template <typename T>
T* LT::CheckInstance(lua_State* L, int idx, uint32_t expectedMagic, const char* typeName);
```

- 失败时 **直接 `luaL_error` 抛出** (longjmp)
- 返回非空指针
- 适用于必须存在的强制参数

### 5.3 `LT::TryCheckInstance<T>` (容错模式)

```cpp
template <typename T>
T* LT::TryCheckInstance(lua_State* L, int idx, uint32_t expectedMagic);
```

- 失败时返回 `nullptr` (不抛错)
- 适用于可选参数 / 优雅降级路径

### 5.4 `LT::CheckStringStrict` / `LT::OptStringStrict`

```cpp
const char* LT::CheckStringStrict(lua_State* L, int idx);     // 强制 string
const char* LT::OptStringStrict(lua_State* L, int idx, const char* dflt);  // 可选 string
```

比 `luaL_checkstring` 严格: 不接受 number 自动转换.

### 5.5 错误返回辅助

```cpp
int LT::PushNilError(lua_State* L, const char* msg);    // push nil, push msg, return 2
int LT::PushBooleanError(lua_State* L, const char* msg); // push false, push msg, return 2
```

---

## 六. Magic 常量分配 (39 个, 至 2026-05-19)

### G.1.7.0 第一批
- `LT_MAGIC_IMAGE`     = `'IMGE'`
- `LT_MAGIC_CANVAS`    = `'CNVS'`
- `LT_MAGIC_FONT`      = `'FONT'`
- `LT_MAGIC_DB`        = `'SQLI'`
- `LT_MAGIC_AV`        = `'AVCT'`
- `LT_MAGIC_VIDEO`     = `'VIDO'`
- `LT_MAGIC_DATABUF`   = `'DBUF'`
- `LT_MAGIC_HTTPCTX`   = `'HTTP'`
- `LT_MAGIC_EMITTER`   = `'EMIT'`
- `LT_MAGIC_TILEMAP`   = `'TLMP'`

### G.1.7.1 Graphics
- `LT_MAGIC_MESH`      = `'MESH'`
- `LT_MAGIC_SHADER`    = `'SHDR'`
- `LT_MAGIC_SURFACE`   = `'SRFC'`

### G.1.7.2 Audio + Network
- `LT_MAGIC_AUDIO_SRC` = `'ASRC'`
- `LT_MAGIC_AUDIO_GRP` = `'AGRP'`
- `LT_MAGIC_AUDIO_FX`  = `'AFX0'`
- `LT_MAGIC_NET_UDP`   = `'NUDP'`
- `LT_MAGIC_NET_RPC`   = `'NRPC'`
- `LT_MAGIC_NET_ROOM`  = `'NROM'`
- `LT_MAGIC_NET_WEB`   = `'NWEB'`
- `LT_MAGIC_IOSTREAM`  = `'IOST'`

### G.1.7.3 Physics
- `LT_MAGIC_WORLD`     = `'WRLD'`  (Box2D)
- `LT_MAGIC_BODY`      = `'BODY'`  (Box2D)
- `LT_MAGIC_SHAPE`     = `'SHPE'`  (Box2D & Bullet 共享, MT 区分)
- `LT_MAGIC_FIXTURE`   = `'FXTR'`  (Box2D)
- `LT_MAGIC_JOINT`     = `'JNTI'`  (Box2D & Bullet 共享)
- `LT_MAGIC_PHY3D_W`   = `'P3WD'`  (Bullet)
- `LT_MAGIC_PHY3D_B`   = `'P3BD'`  (Bullet)

### G.1.7 P1 (Physics3D 闭合 + Animation)
- `LT_MAGIC_CHARACTER` = `'CH3D'`
- `LT_MAGIC_VEHICLE`   = `'VH3D'`
- `LT_MAGIC_SOFTBODY`  = `'SB3D'`
- `LT_MAGIC_SKELETON`  = `'SKEL'`
- `LT_MAGIC_ANIMCLIP`  = `'ACLP'`
- `LT_MAGIC_ANIMATOR`  = `'ANMR'`
- `LT_MAGIC_SKINMESH`  = `'SKMH'`

### G.1.7.4 系统
- `LT_MAGIC_NEM`       = `'NEMC'`
- `LT_MAGIC_WDF`       = `'WDFC'`

### 特殊
- `LT_MAGIC_DEAD`      = `'DEAD'`  (use-after-free 标记)

---

## 七. 已知例外 / 设计决策

### 7.1 Light.Graphics.Material (已闭合 — P2.1)

✅ **双层防护** (Layer 1 metatable + Layer 2 magic).

**实现策略**: 因 `MaterialDesc` 定义在 `render_backend.h` 是 RenderBackend 核心 POD struct, 改 ABI 风险高, 改用 **wrapper struct** 模式:

```cpp
struct MaterialUserdata {
    uint32_t     magic;   // = LT_MAGIC_MATERIAL
    MaterialDesc desc;    // 嵌入原始 POD, 调用 RenderBackend 时传 &ud->desc
};
```

**透明性**: 调用点 (e.g. `g_render->DrawMeshMaterial`) 不需要修改, `CheckMaterialUserdata` 仍返回 `const MaterialDesc*`.

**跨模块创建**: 添加公开 helper `PushNewMaterialUserdata(L) -> MaterialDesc*` 供 `light_graphics_mesh.cpp` (glTF mesh load 路径) 调用.

### 7.2 Animation pointer-holder 模式

`Light.Animation.Skeleton` 等 userdata 是 `Skeleton**` (8 字节单指针), 不是直接持 struct. 加 magic 不在 userdata, 而在 **pointed-to** `Skeleton::magic`:

```cpp
struct Skeleton {
    uint32_t magic = LT::LT_MAGIC_SKELETON;  // 首字段
    // ... 真实字段
};

static Skeleton* CheckSkeleton(lua_State* L, int idx) {
    Skeleton** pp = (Skeleton**)luaL_checkudata(L, idx, SKELETON_MT);
    if (!pp || !*pp) luaL_error(...);
    if ((*pp)->magic != LT_MAGIC_SKELETON) luaL_error(...);  // 解引用后验 magic
    return *pp;
}
```

**优势**: 零 ABI break (userdata 大小不变).

### 7.3 ECS 不需要 magic

`light_ecs.cpp` 用 `uint64_t entityId` (handle id) 而非 userdata ctx. 无指针 = 无 type confusion 风险.

### 7.4 Cross-module 共享 layout

`light_audio_sound.cpp` 需要访问 `GroupUserdata` / `EffectUserdata` 的首字段 (handle pointer). 改 layout 时必须同步 sound 文件中的 `GroupUserdata_Shared` / `EffectUserdata_Shared` 镜像 struct:

```cpp
// In light_audio_sound.cpp:
struct GroupUserdata_Shared {
    uint32_t magic;      // ← 与真 GroupUserdata 同步
    GroupHandle* h;
    // ...
};
```

---

## 八. 性能

### 8.1 单次 Check 开销

- `luaL_checkudata`: ~10 ns (Lua VM 层 metatable name 比较)
- Magic 校验: ~2 ns (1 个 uint32 load + 1 个 compare)
- **总开销**: ~12 ns / 调用

### 8.2 实际性能影响

典型 Lua 调用约 200 ns, magic 校验占比 < 1%, 可忽略.

详见 `TODO P3.1` benchmark 待补.

---

## 九. 调试技巧

### 9.1 检测 magic mismatch

C++ 抛出的 `luaL_error` 会包含 magic 信息:

```
Light.Image: type confusion at arg #1 (magic mismatch)
```

或:

```
Light.Image: use-after-free detected at arg #1 (object already destroyed)
```

### 9.2 在 debugger 中验证 magic

```
(gdb) p ctx->magic
$1 = 0x45474D49      ← LT_MAGIC_IMAGE little-endian
(gdb) p (char*)&ctx->magic
$2 = "IMGE\0..."
```

肉眼可读, 极便于调试.

### 9.3 静态分析 (未来工具)

可写 clang-tidy custom check, 检测:
- 所有 `lua_newuserdata(L, sizeof(X))` 后 N 行内必须出现 `ctx->magic = LT_MAGIC_X`
- 所有 `__instance` 类型 cast 必须走 `LT::TryCheckInstance`

TODO P3.4.

---

## 十. 参考链接

- 完整交付报告: `@e:\jinyiNew\Light\docs\Phase G.1.7 Lua API Robustness Audit\FINAL_PhaseG_1_7.md`
- 任务清单: `@e:\jinyiNew\Light\docs\Phase G.1.7 Lua API Robustness Audit\TASK_PhaseG_1_7.md`
- 后续 TODO: `@e:\jinyiNew\Light\docs\Phase G.1.7 Lua API Robustness Audit\TODO_PhaseG_1_7.md`
- helpers 头: `@e:\jinyiNew\Light\ChocoLight\src\light_lua_helpers.h`
- 安全 smoke: `@e:\jinyiNew\Light\scripts\smoke\lua_api_robustness.lua`
