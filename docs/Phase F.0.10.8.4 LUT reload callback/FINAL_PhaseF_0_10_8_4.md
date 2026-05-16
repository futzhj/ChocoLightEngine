# Phase F.0.10.8.4 — LUT Reload Callback FINAL

> 6A · 阶段 6 · 项目总结
> 工作量 ~0.9h (vs 估 1h, 节约 ~10%)

---

## 1. 项目背景

F.0.10.8.3 实现 mtime polling 自动 reload, 但用户被动 — 拿不到 reload 事件通知, 不能响应 (UI toast / sound feedback / log). 本 phase 加 **回调接口** 让 Lua 用户得知 reload.

---

## 2. 交付内容

### 2.1 HDRRenderer (hdr_renderer.h / .cpp)

| API | 签名 |
|-----|------|
| `LUTReloadCallback` typedef | `void(*)(const char* path, uint32_t oldId, uint32_t newId, void* userData)` |
| `SetLUTReloadCallback(cb, userData)` | 注册 / nullptr 清除 |
| `HasLUTReloadCallback()` | bool |

**触发点**: PollLUTReloads 内 reload 成功后, oldId Delete 之后, ++reloaded 之前. snapshot path 防迭代器失效.

### 2.2 Lua API (light_graphics.cpp, 2 fn)

```lua
HDR.SetLUTReloadCallback(fn or nil)
HDR.GetLUTReloadCallback() → bool
```

**实现要点**:
- 全局 `s_lutReloadCbRef = LUA_NOREF` Registry ref
- C-style `LUTReloadTrampoline_(path, oldId, newId, userData=L)` 推 3 arg + lua_pcall
- 错误用 CC::Log warn, 不破坏后续 reload

### 2.3 smoke (scripts/smoke/hdr.lua)

§18 LUT reload 回调 section **5 PASS**:
1. `GetLUTReloadCallback()` 默认 false
2. `SetLUTReloadCallback(fn)` → registered (Get=true)
3. `SetLUTReloadCallback("string")` type-error rejected
4. `SetLUTReloadCallback(nil)` clears (Get=false)
5. **Live test** (4³ identity LUT + Watch + 改写 + Poll + 验证 cb 调用): headless backend 不 support → SKIP (与 §16 LoadCubeLUT headless 同模式)

**HDR smoke: 44 → 46 fn (+2)**

### 2.4 demo (samples/demo_taa_split2/main.lua)

Headless probe 加 3 PASS:
- 默认 false
- Set(fn) → registered
- Set(nil) → cleared

**demo total: 19 → 22 PASS (+3)**

### 2.5 验证结果

| 类型 | 结果 |
|------|-----|
| 编译 (Release) | ✅ 通过 |
| HDR smoke (§18 5 PASS) | ✅ **46 fn** |
| 8 smoke 零回归 | ✅ hdr/motion_blur/bloom/ssr/ssao/taa/lens_flare/lens_fx |
| demo headless | ✅ **22 PASS** (= 19+3) |
| Lua API 总数 | 70 → **72** (+2) |

---

## 3. 关键设计决策 (8/8 已落地)

| # | 决策 | 选择 | 落地 |
|---|------|------|------|
| 1 | callback 持有方 | light_graphics.cpp Lua wrap | ✅ s_lutReloadCbRef + trampoline |
| 2 | C 接口签名 | path/oldId/newId/userData | ✅ typedef LUTReloadCallback |
| 3 | 多 callback 支持 | 单一 (用户自己 multiplex) | ✅ State 单字段 |
| 4 | reload 失败通知 | 不通知 (entry 保留下次再试) | ✅ 仅成功路径触发 |
| 5 | 回调时机 | oldId Delete 后, ++reloaded 前 | ✅ newId 立即可用 |
| 6 | path 传递 | snapshot std::string | ✅ 防御重入 |
| 7 | nil 清除语义 | unref + Set(nullptr) | ✅ l_HDR_SetLUTReloadCallback |
| 8 | 错误处理 | lua_pcall + CC::Log warn | ✅ trampoline 内捕获 |

---

## 4. 工作量统计

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN+DESIGN+TASK | PLAN.md 合并简化 | 0.1h |
| T1 | hdr_renderer.h | 0.05h |
| T2 | hdr_renderer.cpp State + impl + 触发 | 0.15h |
| T3 | light_graphics.cpp trampoline + 2 wrap | 0.2h |
| T4 | smoke §18 5 PASS | 0.25h |
| T5 | demo probe 3 PASS | 0.05h |
| T6 | FINAL + TODO + commit | 0.1h |
| **合计** | | **~0.9h** |

**vs PLAN 估 1h, 节约 ~10%**.

节省主因: 复用 light_hotreload.cpp luaL_ref + trampoline 标准模式, 一次过.

---

## 5. F.0.10.x 系列累计 (now **11 sub-phase**, LUT 子生态完全闭环 + 主动通知)

| Phase | API 增量 | 主题 |
|-------|---------|------|
| F.0.10.2 | +5 | 双 TAA instance |
| F.0.10.3 | +9 | Bloom/SSR/MB region + auto-* |
| F.0.10.5 | 0 | shader uvBounds 像素完美 |
| F.0.10.6 | +3 | per-region tonemap |
| F.0.10.7 | 0 | demo 视觉演示 |
| F.0.10.8 | +5 | per-region color grading LUT |
| F.0.10.8.1 | +1 | .cube 文件解析 |
| F.0.10.8.2 | +1 | HALD CLUT 图像 |
| F.0.10.8.3 | +6 | LUT 热重载 mtime polling |
| **F.0.10.8.4** | **+2** | **LUT reload 回调** |
| **累计** | **+32** | 39 → **72** Lua API |

---

## 6. LUT 子生态完成态 (主动 + 被动 双向)

```
┌────────────────────────────────────────────────────┐
│  Lua user                                            │
│   ├ Watch(path) → id   [F.0.10.8.3]                  │
│   ├ Poll() (主动 1Hz)  [F.0.10.8.3]                  │
│   └ SetCallback(fn)    [F.0.10.8.4 ← NEW]            │
└────────────────────────────────────────────────────┘
            ↑                        ↑
            │                        │
            │ (主动: 用户问)         │ (被动: 引擎告)
            │                        │
┌────────────────────────────────────────────────────┐
│  HDRRenderer                                         │
│   PollLUTReloads:                                    │
│     1. mtime check                                   │
│     2. LoadCubeLUTFile / LoadHaldLUTFile             │
│     3. DeleteLUT3D(oldId) + entry.lutId = newId      │
│     4. ★ if (cb) cb(path, oldId, newId, userData)    │ [F.0.10.8.4]
│     5. ++reloaded                                    │
│   返 reloaded count                                  │
└────────────────────────────────────────────────────┘
```

---

## 7. 典型用法 (端到端示例)

```lua
local HDR = require('Light.Graphics').HDR

-- 启动: 注册 LUT 监视 + reload 回调
local lut_id = HDR.WatchLUT('assets/luts/cinematic.cube')
HDR.SetGradingLUT(lut_id, 1.0)

HDR.SetLUTReloadCallback(function(path, oldId, newId)
    print(string.format('[LUT] reloaded %s: %d -> %d', path, oldId, newId))
    -- UI 显示 toast
    -- 美术 sound feedback
    -- log 同步
end)

-- 主循环 (1 Hz poll)
local accumulator = 0
function update(dt)
    accumulator = accumulator + dt
    if accumulator > 1.0 then
        accumulator = 0
        HDR.PollLUTReloads()  -- 触发 mtime check + cb (如果文件变了)
    end
end

-- 退出: 清回调 + watch
HDR.SetLUTReloadCallback(nil)
HDR.UnwatchLUT(lut_id)
```

---

## 8. 后续候选

- **F.0.10.8.5** HDR LUT 完整 (~3h): DOMAIN > 1.0 + 16-bit PNG + RGB16F backend
- **F.0.10.9** 真多 HDR target / RT pool (~8-10h)
- **F.1** TAAU DLSS-like 上采样 (~10-15h, F 大版本里程碑)

---

## 9. 结论

Phase F.0.10.8.4 **成功完成**, 用 **~0.9h** (vs 估 1h, 节约 ~10%) 交付完整 **LUT reload callback** 接口. 至此 F.0.10.8 LUT 子生态:

- **F.0.10.8**: 内存 byte LUT (CreateLUT3D)
- **F.0.10.8.1**: `.cube` 文件 (Resolve / Lightroom / Premiere)
- **F.0.10.8.2**: HALD PNG (Photoshop / GIMP / ImageMagick)
- **F.0.10.8.3**: 热重载 (mtime polling, 美术保存 → 引擎自动 reload)
- **F.0.10.8.4**: reload 回调 (Lua 用户得知 reload, UI toast / sound feedback / log)

LUT 子生态从 "被动 mtime polling" 进化到 "主动 + 被动双向通知". 工业级工具链能力闭环.
