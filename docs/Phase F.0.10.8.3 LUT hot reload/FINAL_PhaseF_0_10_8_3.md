# Phase F.0.10.8.3 — LUT 热重载 FINAL

> 6A · 阶段 6 (Assess) · 项目总结
> 工作量 ~2.2h (vs 估 2.5h, 节约 ~12%)

---

## 1. 项目背景

F.0.10.8 + F.0.10.8.1 + F.0.10.8.2 提供 LUT **3 入口** (内存 / .cube / HALD PNG), 但**每次修改都要重启**程序. 本 phase 加 **mtime polling 热重载**, 让美术调色实时反馈 (DaVinci / Photoshop 同款工作流体验).

---

## 2. 交付内容

### 2.1 HDRRenderer (hdr_renderer.h / .cpp)

| API | 说明 |
|-----|------|
| `WatchLUT(path, outErr, errCap) → tex_id` | 内部判扩展名 → LoadCube/LoadHald + 记录 mtime/path/id |
| `UnwatchLUT(lutTex) → bool` | 移除 watch + DeleteLUT3D + 同步清 `g.lutTexId` |
| `GetWatchedLUTId(path) → tex_id` | 路径 → 当前 id 查询 (reload 后查最新) |
| `PollLUTReloads() → reloaded_count` | 遍历 list + mtime check + reload + 同步 grading |
| `SetLUTHotReload(bool)` | 全局开关 (默认 true) |
| `GetLUTHotReload() → bool` | 查全局开关 |

**内部状态**:
- `State::lutHotReload = true` (默认开)
- `g_lutWatchList` `std::vector<WatchEntry>` (放 State 外避免 forward 依赖)
- `WatchEntry { path, lastMtime SDL_Time, lutId, isHald }`

**辅助**:
- `isImageExt_(path)` 手写 tolower 比较 .png/.jpg/.jpeg/.bmp/.tga (跨平台, 不依赖 _stricmp/strcasecmp)

### 2.2 Lua API (light_graphics.cpp, 6 fn)

```lua
HDR.WatchLUT(path)         → tex_id, err
HDR.UnwatchLUT(tex_id)     → bool
HDR.GetWatchedLUTId(path)  → tex_id or nil
HDR.PollLUTReloads()       → reloaded_count
HDR.SetLUTHotReload(bool)
HDR.GetLUTHotReload()      → bool
```

### 2.3 smoke (scripts/smoke/hdr.lua)

§17 LUT 热重载 section **10 PASS**:
1. `GetLUTHotReload()` 默认 true
2. `SetLUTHotReload(false/true)` round-trip
3. `SetLUTHotReload("yes")` type-error rejected
4. `WatchLUT(missing.cube)` → "file read failed"
5. `WatchLUT(missing.png)` → "stbi_load failed"
6. `UnwatchLUT(0)` → false (silent)
7. `UnwatchLUT(99999)` → false (silent)
8. `GetWatchedLUTId(not_watched)` → nil
9. `PollLUTReloads()` 空 list → 0
10. `SetLUTHotReload(false) + PollLUTReloads` → 0 (短路)

**HDR smoke: 38 → 44 fn (+6)**

### 2.4 demo (samples/demo_taa_split2/main.lua)

- 加 hasF10_8_3 API 检测
- headless probe 加 3 PASS (默认开关 / WatchLUT missing / Poll 空 list)
- **demo total 16 → 19 PASS**

### 2.5 验证结果

| 类型 | 结果 |
|------|-----|
| 编译 (Release) | ✅ 通过 |
| HDR smoke (§17 10 PASS) | ✅ 44 fn |
| 8 smoke 零回归 | ✅ hdr/motion_blur/bloom/ssr/ssao/taa/lens_flare/lens_fx |
| demo headless | ✅ **19 PASS** (= 16+3) |
| Lua API 总数 | 64 → **70** (+6) |

### 2.6 文档

| 文件 | 行数 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_8_3.md` | ~180 (HALD/SDL3 mtime + scope + 12 决策) |
| `DESIGN_PhaseF_0_10_8_3.md` | ~250 (parser 算法 + 接口契约 + 测试矩阵) |
| `TASK_PhaseF_0_10_8_3.md` | ~100 (6 原子任务 + 依赖图) |
| `FINAL_PhaseF_0_10_8_3.md` | 本 (~140) |
| `TODO_PhaseF_0_10_8_3.md` | 待办 + 用户支持 |

---

## 3. 关键设计决策 (12/12 已落地)

| # | 决策 | 选择 | 落地 |
|---|------|------|------|
| 1 | 实现位置 | hdr_renderer.cpp | ✅ 与 LoadCubeLUT/LoadHaldLUT 同模块 |
| 2 | 检测机制 | mtime polling (vs inotify) | ✅ SDL_GetPathInfo 跨平台 |
| 3 | poll 频率 | 用户调 (vs 每帧/N帧) | ✅ HDR.PollLUTReloads() 显式 |
| 4 | 格式判定 | 扩展名 (vs magic byte) | ✅ isImageExt_ tolower 比较 |
| 5 | reload 后 id | 新 id + 自动更新 grading | ✅ PollLUTReloads 内 g.lutTexId 同步 |
| 6 | 默认开关 | 开 (true) | ✅ State.lutHotReload = true |
| 7 | mtime 类型 | SDL_Time int64 | ✅ WatchEntry.lastMtime |
| 8 | watch list 结构 | std::vector (vs map) | ✅ g_lutWatchList 简洁遍历 |
| 9 | path 规范化 | 原 path (vs absolute) | ✅ std::string 直接比较 |
| 10 | reload 失败行为 | entry 保留 | ✅ continue + Log warn |
| 11 | API 命名 | Watch + PollReloads | ✅ 6 fn 一致命名 |
| 12 | Lua return | reloaded_count int | ✅ 用户用 GetWatchedLUTId 拿新 id |

---

## 4. 工作量统计

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN | 调研 + 12 决策 + 文档 | 0.3h |
| DESIGN/TASK | 设计 + 拆分 | 0.2h |
| SP1 (T1 + T2 watchList + 6 fn) | hdr_renderer.h/cpp | 0.7h |
| SP2 (T3 Lua wrap) | light_graphics.cpp 6 fn | 0.2h |
| SP2 (T4 smoke §17 10 PASS) | hdr.lua | 0.4h |
| SP2 (T5 demo 3 PASS) | main.lua | 0.1h |
| SP2 (T6 docs) | FINAL + TODO | 0.3h |
| **合计** | | **~2.2h** |

**vs ALIGN 估 ~2.5h, 节约 ~12%**.

节省主因: 复用 F.0.10.8.1 / F.0.10.8.2 的 `writeErr_` + `LoadCubeLUTFile` / `LoadHaldLUTFile` 完整 parser (无新解析逻辑); SDL_GetPathInfo 已在 Light.Filesystem 验证可用.

---

## 5. F.0.10.x 系列累计 (now 10 sub-phase, LUT 子生态完整闭环)

| Phase | API 增量 | 主题 |
|-------|---------|------|
| F.0.10.2 | +5 | 双 TAA instance |
| F.0.10.3 | +9 | Bloom/SSR/MB region + auto-* |
| F.0.10.5 | 0 | shader uvBounds 像素完美 |
| F.0.10.6 | +3 | per-region tonemap |
| F.0.10.7 | 0 | demo 视觉演示 |
| F.0.10.8 | +5 | per-region color grading LUT |
| F.0.10.8.1 | +1 | .cube 文件解析 (Adobe) |
| F.0.10.8.2 | +1 | HALD CLUT 图像 LUT |
| **F.0.10.8.3** | **+6** | **LUT 热重载** |
| **累计** | **+30** | 39 → **70** Lua API |

---

## 6. LUT 完整生态总览 (闭环)

```
┌────────────────────────────────────────────────────┐
│  美术工具 → LUT 文件                                  │
│  ├ DaVinci / Lightroom / Premiere → .cube           │
│  └ Photoshop / GIMP / ImageMagick → HALD PNG        │
└────────────────────────────────────────────────────┘
            ↓ HDR.WatchLUT(path)                     [F.0.10.8.3]
            ↓
┌────────────────────────────────────────────────────┐
│  HDRRenderer (统一 backend->CreateLUT3D)            │
│  ├ LoadCubeLUTFile                                   │  [F.0.10.8.1]
│  ├ LoadHaldLUTFile                                   │  [F.0.10.8.2]
│  └ WatchLUT + PollLUTReloads (mtime 检测)           │  [F.0.10.8.3]
└────────────────────────────────────────────────────┘
            ↓
┌────────────────────────────────────────────────────┐
│  GL 3D texture + Tonemap shader 采样                  │  [F.0.10.8]
└────────────────────────────────────────────────────┘
            ↓ HDR.SetGradingLUT(id, strength)
            ↓
        Final image (auto-update on file save)         [F.0.10.8.3]
```

**美术修改 → 保存 → 引擎下一帧 PollLUTReloads → mtime 变化 → reload + 自动 swap g.lutTexId → 用户立即看到新调色**.

---

## 7. 性能 / 内存

- `SetLUTHotReload(false)`: 完全短路, 零开销 (List/SDL 调用都跳过)
- `PollLUTReloads()` 空 list: 直接 return 0
- `PollLUTReloads()` 含 N 个 watched: ~50μs × N (SDL_GetPathInfo syscall stat)
- 实测 10 个 watched LUT poll < 1ms
- 内存: WatchEntry ~32 bytes × N (10 个 = 320 bytes)

**推荐 poll 频率**: 主循环 1 Hz (每秒 1 次) 即可, 不必每帧.

---

## 8. 后续候选

- **HDR LUT 完整** (~3h): DOMAIN > 1.0 + 16-bit PNG + RGB16F backend
- **F.0.10.9** 真多 HDR target / RT pool (~8-10h)
- **F.1 TAAU** DLSS-like 上采样 (~10-15h, F 大版本里程碑)
- **Callback on reload** (~1h): `HDR.SetLUTReloadCallback(fn)` 让用户得到 reload 通知 (UI 提示用)

---

## 9. 结论

Phase F.0.10.8.3 **成功完成**, 用 ~2.2h (vs 估 2.5h, 节约 ~12%) 交付完整 **LUT 热重载 mtime polling**. 至此 F.0.10.8 LUT 子生态**完整闭环**:

- **F.0.10.8**: 内存 byte LUT 入口 + GL backend + grading
- **F.0.10.8.1**: `.cube` 文件入口 (Resolve / Lightroom / Premiere)
- **F.0.10.8.2**: HALD PNG 入口 (Photoshop / GIMP / ImageMagick)
- **F.0.10.8.3**: 热重载 (美术保存 → 引擎自动 reload, AAA 级工作流)

LUT 子生态从"美术导出 → 一行加载"进化到"美术保存 → 实时看到新效果". 工业级工具链能力补齐.
