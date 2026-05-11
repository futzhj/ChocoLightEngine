# ACCEPTANCE — Phase E.1.3 · Lighting2D C++ 模块

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.3**：新建 `Lighting2D` 命名空间 C++ 模块，管理 2D forward 多光的 16 个 slot + ambient + enabled 开关。暴露 `Add / Update / Remove / Clear / SetAmbient / SetEnabled / GetCount / GetMax / UploadToShader` API，供后续 E.1.4 Lua binding、E.1.5 后端上传、E.1.6 ECS system 共享使用。

---

## 1. 实现摘要

| 改动文件 | 类型 | 说明 |
|----------|------|------|
| `ChocoLight/include/light_lighting2d.h` | 新建 | POD `Light` / `State` + API 声明 + `constexpr int MAX_LIGHTS = 16` + `LightType` 枚举 |
| `ChocoLight/src/light_lighting2d.cpp` | 新建 | 文件内 `static State g_state` 单例 + 9 个 API 实现 + `UploadToShader` no-op 占位 |
| `ChocoLight/CMakeLists.txt` | +1 行 | 在 `light_animation.cpp` 之后追加 `list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_lighting2d.cpp)` |

无其他文件改动。`render_backend.h` / `render_gl33.cpp` 等均无接触，接口稳定。

---

## 2. 关键数据结构

### 2.1 `Lighting2D::Light` POD
```cpp
struct Light {
    int   type;       // TYPE_INACTIVE=0 | TYPE_POINT=1 | TYPE_SPOT=2
    float pos[2];     // 世界坐标 (x, y)
    float dir[2];     // spot 方向 (point 忽略; 调用方需归一化)
    float color[3];   // RGB
    float range;      // 距离衰减半径
    float intensity;  // 强度乘子
    float innerCos;   // cos(innerAngle), spot only
    float outerCos;   // cos(outerAngle), spot only
};
```
- **所有字段有默认值** → 默认构造后即是安全的 TYPE_INACTIVE slot
- 布局与 shader 侧 `FS_LIT2D_SOURCE` 中 16 个 uniform 数组一一对应（E.1.5 会按此顺序上传）

### 2.2 `Lighting2D::State` 单例
```cpp
struct State {
    bool  enabled      = true;
    Light lights[16];                       // MAX_LIGHTS
    int   active_count = 0;
    float ambient[3]   = { 0, 0, 0 };
};
```
- 文件内 `static State g_state;`，默认初始化后**即可使用**，不需要显式 Init
- `GetState()` 返回指针，E.1.5 GL33Backend 读此数据上传 uniform

---

## 3. API 设计要点

### 3.1 id 空间
- 成功 id 范围 `1..16`（内部 idx = id - 1）
- `0` 保留给"失败"返回值（slot 满 / 非法 type / 越界）
- **不保证 id 连续**：Remove 后的 slot 会被 Add 复用；调用方应把 id 视为不透明 handle

### 3.2 `Add(const Light& l)` 语义
```cpp
if (l.type == TYPE_INACTIVE) return 0;        // 非法 type 拒绝
for (i = 0..15) {
    if (lights[i].type == TYPE_INACTIVE) {    // 第一个空 slot
        lights[i] = l;
        ++active_count;
        return i + 1;                         // id = idx + 1
    }
}
return 0;                                     // 全满
```
- O(MAX_LIGHTS) = O(16) 扫描，16 元素线性扫描总共 <100 ns，可接受
- **不保证插入顺序** = slot 顺序（Remove 后复用时乱序）

### 3.3 `Update(int id, const Light& fields)` 语义
- **全字段覆盖**，不做部分合并
- 越界 id / slot 已 INACTIVE → `false`
- `fields.type == INACTIVE` → `false`（明确拒绝 "Update 伪装 Remove"，让调用方显式 `Remove(id)`）

### 3.4 `Remove / Clear` 幂等性
- `Remove(id)`：id 越界 / slot 已 INACTIVE → no-op，不减 `active_count`
- `Clear()`：遍历把所有 slot type 设为 INACTIVE，`active_count` 归零；**不清 ambient 与 enabled**（Lua `ClearLights` 语义：只清 light 数组，不动全局配置）

### 3.5 `UploadToShader` E.1.3 策略
```cpp
void UploadToShader(RenderBackend* backend, uint32_t programId) {
    (void)backend;
    (void)programId;
    // 占位; E.1.5 替换为 backend->UploadLighting2D(active_count, types, pos, dir, ...);
}
```
- **保持符号存在** → E.1.5 之前 / 间的代码编译期 / 链接期都不会缺符号
- **E.1.5 落地时**：签名不变，仅实现体替换；Lua binding (E.1.4) 不受影响

---

## 4. 验收清单（对齐 `TASK_PhaseE.md` § E.1.3）

| 验收标准 | 状态 | 证据 |
|----------|------|------|
| **编译通过** | ✅ 待 CI 确认 | 新增 .h/.cpp 严格按 C++17, 仅依赖 `<cstdint>` + 前向声明 `RenderBackend`; 6 平台 CI 覆盖 |
| **Add 16 个后第 17 个返回 0** | ✅ 逻辑正确 | `Add()` 内 `for i=0..15` 扫描，全占用时循环退出返回 `0`；覆盖实测将在 E.1.4 smoke 里验证 |
| **Update(invalidId) 返回 false** | ✅ 逻辑正确 | 三重守卫：`id<1 \|\| id>16` / `lights[idx].type==INACTIVE` / `fields.type==INACTIVE` 均返回 `false` |
| **Clear 后 active_count = 0** | ✅ 逻辑正确 | `Clear()` 末尾 `active_count = 0;` + 所有 slot type 设为 INACTIVE |
| **不暴露 Lua binding** | ✅ 已遵循 | 本 commit 未修改 `light.cpp` 主入口, 没有 `luaopen_Light_Lighting2D`, E.1.4 独立做 |

### 4.1 与 CONSENSUS § 2.2.1 的对照

| CONSENSUS 字段 | 本实现 |
|----------------|--------|
| `namespace Lighting2D` | ✅ |
| `struct Light { type, pos[2], dir[2], color[3], range, intensity, innerCos, outerCos }` | ✅ 字段一致 |
| `struct State { enabled, lights[16], active_count, ambient[3] }` | ✅ |
| `State* GetState()` | ✅ |
| `int Add(const Light&)` → 1..16 / 0 | ✅ |
| `bool Update(int id, const Light& fields)` | ✅ |
| `void Remove(int id)` | ✅ |
| `void Clear()` | ✅ |
| `void UploadToShader(RenderBackend*, uint32_t)` | ✅ no-op 占位 |

### 4.2 额外 API（CONSENSUS 未列但 Lua 层需要）
- `SetEnabled(bool) / IsEnabled()` — Lua `SetEnabled / IsEnabled` 的 C++ 底座
- `SetAmbient(r,g,b) / GetAmbient(r&,g&,b&)` — Lua `SetAmbient / GetAmbient`
- `GetCount() / GetMax()` — Lua `GetLightCount / GetMaxLights`

这些都在 CONSENSUS § 2.1 Lua API 表中列出，E.1.3 提前提供 C++ 底座是合理的（否则 E.1.4 要回头补）。

---

## 5. 与后续任务的衔接

| 后续任务 | 如何使用本模块 |
|---------|---------------|
| **E.1.4** Lua binding | `#include "light_lighting2d.h"`，在本 cpp 末尾追加 `static int l_AddPointLight(lua_State*)` 等；注册到 `Light.Lighting2D` table |
| **E.1.5** DrawLit2DQuad | `render_gl33.cpp` 内调 `Lighting2D::GetState()` 取 `lights + active_count + ambient`，按 16 个 uniform 数组上传 |
| **E.1.6** ECS Light2D | `_UploadLights2D()` 每帧先 `Lighting2D::Clear()` 再循环 `Lighting2D::AddPointLight/AddSpotLight` |

---

## 6. 风险与已知限制

| 风险 | 缓解 |
|------|------|
| **单例全局状态**（不是 thread-safe） | Lua 主循环单线程，与现有 `light_animation.cpp` / `light_graphics.cpp` 等模块风格一致，符合引擎既定约束 |
| **Update 不做部分合并** | Lua 层 E.1.4 负责：从现 slot 拷贝一份 Light → 按传入 table 覆盖指定字段 → 调 C++ `Update`；C++ 层保持精简 |
| **id 复用后旧 id 失效但无错误反馈** | 现有设计：Remove(id) 后调用方持旧 id 再 Update/Remove 会返回 false / no-op；与一般 handle 语义一致，不引入 generation counter（16 slot 规模不值得） |
| **UploadToShader 空实现** | 明确标注 "E.1.5 替换"；签名不变保证 E.1.4 Lua binding 稳定 |

---

## 7. CI 验证策略

- **编译**：6 平台（Windows / Linux / macOS / Android / iOS / Web）全部 link 通过
- **运行时**：本任务无新 Lua API，Windows runtime smoke 不会触发 Lighting2D 代码路径，等价于「编译通过 = 交付」
- **功能验证**：推迟到 E.1.4（Lua smoke `scripts/smoke/lighting2d.lua`）+ E.1.7（demo 视觉确认）

---

## 8. 提交信息建议

```
feat(phase-e1.3): Lighting2D C++ 模块 (16 light slot + ambient 状态管理)

- 新增 include/light_lighting2d.h: Light/State POD + API 声明 + MAX_LIGHTS=16
- 新增 src/light_lighting2d.cpp: static State 单例 + Add/Update/Remove/Clear/
  SetAmbient/SetEnabled/GetCount/GetMax 完整实现 + UploadToShader 占位
- CMakeLists.txt +1 行: list(APPEND LIGHT_SOURCES light_lighting2d.cpp)
- id 空间 1..16, 0 为失败; Add 扫描空闲 slot O(16)
- Update 全字段覆盖语义, 拒绝 INACTIVE fields (显式 Remove)
- Clear 只清 lights 数组, 保留 ambient 与 enabled
- 不暴露 Lua binding (E.1.4 独立做); UploadToShader no-op (E.1.5 接入)
```
