# Phase F.0.10.8.4 — LUT Reload Callback PLAN

> 6A 简化 (ALIGN + DESIGN + TASK 合并, 工作量 < 1.5h)

---

## 1. ALIGN — 任务对齐

### 目标
让 Lua 用户在 LUT 文件被 hot-reload 时收到通知 (path/oldId/newId), 用于:
- UI toast 提示美术 ("LUT 已更新")
- 美术 sound feedback (reload 确认音效)
- 日志同步 (debug 用)
- 其他被动响应

### 边界
- **In**: 全局单一回调; 函数 / nil 注册; 在 PollLUTReloads 内 reload 成功后触发
- **Out**: 多回调列表 (用户自己用 multiplex); reload 失败时通知 (留后续); per-watch callback (留后续)

### 决策矩阵 (8 项, 全部自动决策)

| # | 决策点 | 选择 | 理由 |
|---|--------|------|------|
| 1 | callback 持有方 | light_graphics.cpp Lua wrap | HDRRenderer 不持 lua_State |
| 2 | C 接口签名 | `void(*)(path, oldId, newId, userData)` | 标准 C trampoline 模式 |
| 3 | 多 callback 支持 | 单一全局 | 简化, 用户自己 multiplex |
| 4 | reload 失败通知 | 不通知 (entry 保留, 下次再试) | 与现有 PollLUTReloads 行为一致 |
| 5 | 回调时机 | 旧 GL tex Delete 后, ++reloaded 前 | newId 立即可用, oldId 仅供 log |
| 6 | path 传递 | snapshot std::string 防迭代器失效 | 防御性 (用户约定不重入但代码鲁棒) |
| 7 | nil 清除语义 | luaL_unref ref + Set(nullptr, nullptr) | 完全清状态 |
| 8 | 错误处理 | lua_pcall + CC::Log warn | 错误不破坏后续 reload |

---

## 2. DESIGN — 接口设计

### 2.1 hdr_renderer.h

```cpp
typedef void (*LUTReloadCallback)(const char* path, uint32_t oldId, uint32_t newId, void* userData);
void SetLUTReloadCallback(LUTReloadCallback cb, void* userData);
bool HasLUTReloadCallback();
```

### 2.2 hdr_renderer.cpp

State 加 2 字段:
```cpp
LUTReloadCallback lutReloadCb     = nullptr;
void*             lutReloadCbUser = nullptr;
```

PollLUTReloads 内 reload 成功段加触发:
```cpp
if (g.lutReloadCb) {
    const std::string pathSnap = e.path;
    g.lutReloadCb(pathSnap.c_str(), oldId, newId, g.lutReloadCbUser);
}
```

### 2.3 light_graphics.cpp Lua trampoline

全局 static `s_lutReloadCbRef = LUA_NOREF` 持 Lua function ref.

```cpp
static void LUTReloadTrampoline_(const char* path, uint32_t oldId, uint32_t newId, void* userData) {
    lua_State* L = (lua_State*)userData;
    if (!L || s_lutReloadCbRef == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_lutReloadCbRef);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushstring(L, path ? path : "");
    lua_pushinteger(L, oldId);
    lua_pushinteger(L, newId);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        CC::Log(CC::LOG_WARN, "HDR.LUTReloadCallback error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
```

`l_HDR_SetLUTReloadCallback`: 接受 fn / nil, 管理 ref + 注册 trampoline.

### 2.4 数据流图

```
Lua: HDR.SetLUTReloadCallback(my_cb)
  └→ s_lutReloadCbRef = luaL_ref(my_cb)
     HDRRenderer::SetLUTReloadCallback(LUTReloadTrampoline_, L)

(美术保存 .cube 文件, mtime 变化)

Lua: HDR.PollLUTReloads()
  └→ HDRRenderer 内 mtime check + LoadCubeLUTFile 重 load
     └→ DeleteLUT3D(oldId) + entry.lutId = newId
        └→ g.lutReloadCb(path, oldId, newId, L)
           └→ LUTReloadTrampoline_(path, oldId, newId, L)
              └→ lua_rawgeti(s_lutReloadCbRef) + push 3 args + lua_pcall
                 └→ my_cb(path, oldId, newId)  -- Lua 用户代码
```

---

## 3. TASK — 原子任务

| T# | 内容 | 工作量 | 依赖 |
|----|------|------|------|
| T1 | hdr_renderer.h 加 typedef + 2 fn 声明 | 0.05h | - |
| T2 | hdr_renderer.cpp State 字段 + 2 fn impl + PollLUTReloads 触发 | 0.15h | T1 |
| T3 | light_graphics.cpp trampoline + 2 Lua wrap + 注册 | 0.2h | T2 |
| T4 | smoke §18 5 PASS (默认 / 注册 / 类型错 / nil 清 / live SKIP) | 0.25h | T3 |
| T5 | demo headless probe 3 PASS | 0.05h | T3 |
| T6 | FINAL + TODO + commit + push + CI 验证 | 0.2h | T4+T5 |
| **合计** | | **~0.9h** | |

---

## 4. 验收标准

| 类型 | 标准 |
|------|-----|
| 编译 | Release 通过 (Win 本地 + 6 平台 CI) |
| HDR smoke | 44 → **46 fn**, §18 **5 PASS** |
| 8 smoke | 零回归 (motion_blur/bloom/ssr/ssao/taa/lens_flare/lens_fx 全 PASS) |
| demo headless | 19 → **22 PASS** |
| Lua API | 70 → **72** (+2) |

---

## 5. 风险

- **极低**: 复用 light_hotreload.cpp 同模式, ChocoLight 已验证 luaL_ref + lua_pcall trampoline 模式 100+ 次
- **headless 测试**: §18.5 live cb test 走 SKIP 路径 (backend 不 support), 模式与 §16 LoadCubeLUT 一致
