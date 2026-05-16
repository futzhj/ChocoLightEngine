# Phase F.0.10.8.3 — LUT 热重载 ALIGNMENT

> 6A · 阶段 1 (Align)

---

## 1. 项目上下文

F.0.10.8 + F.0.10.8.1 + F.0.10.8.2 完成 LUT 3 入口 (内存 byte / `.cube` / HALD PNG), 但**每次修改都要重启**程序. 本 phase 加**热重载** (file mtime polling + 自动 reload), 让美术调色实时反馈 — AAA 级工作流关键能力 (DaVinci / Photoshop 同款体验).

---

## 2. HALD CLUT 标准 / SDL3 mtime API

- **SDL_GetPathInfo(path, &info)** → `info.modify_time` (SDL_Time = int64 nanoseconds since epoch)
- 已在 `Light.Filesystem.GetPathInfo` 暴露 (light_filesystem.cpp:170-189)
- 无需新依赖, 跨平台 ready

---

## 3. 边界确认 (Scope)

### 3.1 In Scope

- 内部 watch list 跟踪 `.cube` / HALD 文件 mtime
- 用户调 `HDR.WatchLUT(path)` 取代 LoadCubeLUT/LoadHaldLUT (内部自动判格式)
- `HDR.PollLUTReloads()` 周期检 mtime + 自动 reload
- reload 时**自动更新** `g.gradingLutId` (如果当前 grading 用的就是这个 watched id)
- 全局开关 `HDR.SetLUTHotReload(true/false)` (默认 true)
- 共 6 fn

### 3.2 Out of Scope

- ❌ 异步 file watcher (用 inotify / ReadDirectoryChangesW) — 仅同步 mtime polling
- ❌ in-place GL texture 更新 (保 lutId 不变) — reload 后 id 变, 用户用 `GetWatchedLUTId(path)` 查
- ❌ 回调注册 (callback on reload) — 留后续 phase
- ❌ Tonemap(rgn, {lut=cached_id}) 自动更新 — 用户须查询新 id (文档说明)

---

## 4. 用户故事

```lua
-- 启动时: watch LUT
local lut_id = HDR.WatchLUT("assets/luts/warm.cube")
HDR.SetGradingLUT(lut_id, 1.0)

-- 主循环每帧 / 每 N 帧 (按需):
function on_frame()
    local reloaded = HDR.PollLUTReloads()
    if reloaded > 0 then
        print("[hotreload]", reloaded, "LUT(s) reloaded")
        -- g.gradingLutId 已自动更新, 美术看到新调色
    end
    -- ... render ...
end

-- 退出
HDR.UnwatchLUT(lut_id)
```

---

## 5. 接口契约

| API | 签名 | 说明 |
|-----|------|-----|
| `HDR.WatchLUT(path)` | `→ tex_id, err` | 内部判 .cube / image, 加入 watch list |
| `HDR.UnwatchLUT(tex_id)` | `→ bool` | 移除 watch + DeleteLUT3D |
| `HDR.PollLUTReloads()` | `→ reloaded_count` | 检 mtime + 自动 reload 所有 watched |
| `HDR.GetWatchedLUTId(path)` | `→ tex_id or nil` | 查 path 当前 id (reload 后用) |
| `HDR.SetLUTHotReload(bool)` | `→ (none)` | 全局开关, 默认 true |
| `HDR.GetLUTHotReload()` | `→ bool` | 查全局开关 |

---

## 6. 错误处理

| 错误 | 返回 |
|------|-----|
| WatchLUT 文件不存在 / decode 失败 | `nil, "<inner err>"` |
| UnwatchLUT 找不到 id | `false` (silent, 与 DeleteLUT3D 同模式) |
| PollLUTReloads reload 失败 (文件被删) | 跳过该 entry, log warn, 不 throw |
| SetLUTHotReload(非 bool) | luaL_typeerror |

---

## 7. 技术决策矩阵

| # | 决策 | 选项 | 选择 | 理由 |
|---|------|------|------|------|
| 1 | 实现位置 | (a) hdr_renderer.cpp / (b) 新模块 | **(a)** | 与 LoadCubeLUT/LoadHaldLUT 同模块 |
| 2 | 检测机制 | (a) mtime polling / (b) inotify/ReadDir | **(a)** | 跨平台简单, SDL3 API ready, 性能足够 |
| 3 | poll 频率 | (a) 每帧 / (b) 每 N 帧 / (c) 用户调 | **(c) 用户调** | 用户控制开销 (debounce 在 Lua 端容易) |
| 4 | 格式判定 | (a) 扩展名 / (b) magic byte | **(a) 扩展名** | 简单, 与用户期望一致 (.cube / .png/.jpg/.bmp) |
| 5 | reload 后 id 处理 | (a) 复用 id (in-place) / (b) 新 id + 自动更新 grading | **(b)** | 简单, 不改 backend; 用户用 GetWatchedLUTId 取最新 |
| 6 | 默认开关 | (a) 开 / (b) 关 | **(a) 开** | 与"热重载"语义一致, 不调 Poll 也无开销 |
| 7 | mtime 类型 | (a) SDL_Time int64 / (b) double | **(a) int64** | 精确 (纳秒), 内部用 |
| 8 | watch list 数据结构 | (a) std::vector / (b) std::unordered_map | **(a) vector** | 数量少 (典型 <10), 遍历廉价 |
| 9 | path 规范化 | (a) 原 path / (b) absolute | **(a) 原 path** | 简洁; 用户责任传一致路径 |
| 10 | reload 失败行为 | (a) entry 保留 (下次再试) / (b) 自动 unwatch | **(a) 保留** | 美术可能临时删文件, 下次保存再恢复 |
| 11 | API 命名 | Watch* (Lua 风格) / Reload* / HotReload* | **Watch + PollReloads** | Watch = 注册, Poll = 触发 — 语义清晰 |
| 12 | Lua return | reloaded_count vs change list | **reloaded_count** | 简单, 用户用 GetWatchedLUTId 拿新 id |

---

## 8. 验收标准

- ✅ Watch/Unwatch round-trip ok
- ✅ Poll 在 mtime 不变时 reloaded_count = 0
- ✅ Poll 在 mtime 变化后 reloaded_count > 0 + GetWatchedLUTId 返新 id
- ✅ reload 时 g.gradingLutId 自动同步 (如果匹配)
- ✅ SetLUTHotReload(false) → PollLUTReloads 总返 0
- ✅ smoke 加 5+ PASS (fn 存在 + Watch missing file rejected + Watch/Unwatch round-trip + Poll basic + SetHotReload round-trip)
- ✅ demo headless probe +1 PASS (16 → 17)
- ✅ 8 smoke 零回归
- ✅ Lua API 64 → 70 (+6)

---

## 9. 工作量预算

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN/DESIGN/TASK | 0.3h |
| SP1 (parser + state) | hdr_renderer 加 watchList + 6 fn | 1.0h |
| SP2 (Lua + smoke + demo + Assess) | wrap + 5 PASS + 1 PASS + docs | 1.2h |
| **合计** | | **~2.5h** |

vs 估 2h, 略超 (主因 Poll fn 实现含 mtime 调用 + 自动 reload + grading state 同步, 比单纯 file IO 复杂).

---

## 10. 共识

实现 **mtime polling 的 LUT 热重载**, 加 6 个 fn, 默认开启, 用户控制 poll 频率. 不上 inotify / callback / in-place GL update — 留扩展空间, 本 phase 聚焦"可用 + 简洁".
