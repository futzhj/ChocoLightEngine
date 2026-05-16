# Phase F.0.10.8.3 — LUT 热重载 TODO

> 6A · 阶段 6 (Assess) · 待办

---

## 1. 强制

### 1.1 ⏳ CI 6/6 验证

**等**: GitHub Actions 完成所有平台 build.

**风险点**:
- `SDL_GetPathInfo` 在 Linux/macOS/iOS/Windows/Android/Web 都已 ready (Light.Filesystem 已用过)
- `<string>` / `<vector>` 跨平台
- 无新 GPU 改动 — 风险**极低**

**特别注意**: F.0.10.8 系列前 4 commit 因 `<cstddef>` 缺失 4 平台挂, 已 `e2937ba` 修复. 本 phase 与之相同 size_t 用法**已通过验证**.

**用户操作**: 检查 [GitHub Actions](https://github.com/futzhj/ChocoLightEngine/actions)

---

## 2. 可选 (建议)

### 2.1 demo 真热重载演示 (推荐 ~30min)

**目标**: demo_taa_split2 加 mainloop 调 PollLUTReloads, 用户修改 `luts/warm_red.cube` → 引擎自动 reload + split-screen P1 视觉变化.

**实施**:
```lua
-- 启动时 watch (不用 Load*)
local lut1 = HDR.WatchLUT('samples/demo_taa_split2/luts/warm_red.cube')

-- 主循环每秒 poll 1 次 (节流)
local last_poll = 0
function on_frame(now)
    if now - last_poll > 1.0 then
        local n = HDR.PollLUTReloads()
        if n > 0 then
            print('[hotreload]', n, 'LUT(s) reloaded')
        end
        last_poll = now
    end

    -- 渲染时引用最新 id (PollLUTReloads 已自动更新 g.lutTexId, 但也可显式查):
    local cur_id = HDR.GetWatchedLUTId('samples/demo_taa_split2/luts/warm_red.cube')
    HDR.Tonemap(0, 0, W/2, H, {lut=cur_id, lutStrength=0.8})
end

-- 退出
HDR.UnwatchLUT(lut1)
```

**测试方法**: demo 运行中, 用编辑器改 `warm_red.cube` 的 RGB 数值, 保存. demo 应实时显示新调色 (无需重启).

**工作量**: ~30 min

### 2.2 Callback on reload (~1h)

**目标**: 用户能注册 callback 接收 reload 通知 (UI toast / 美术 sound feedback).

**API**:
```lua
HDR.SetLUTReloadCallback(function(path, old_id, new_id) ... end)
```

**实施**: 加 `reloadCallback` Lua ref to State, PollLUTReloads 内 reload 后 lua_call.

### 2.3 HDR LUT 完整 (DOMAIN > 1.0 + 16-bit PNG) (~3h)

**目标**: 支持 HDR LUT (Resolve HDR project + ACES workflow).

**实施**:
- backend `CreateLUT3D` 加 `format` 参数 (RGB8 / RGB16F)
- `.cube` parser: 检测 DOMAIN_MAX > 1.0 → 选 RGB16F
- HALD parser: stbi_load_16 + 16-bit PNG path
- shader 加 HDR 路径 (无 clamp)

### 2.4 路径规范化 (~1h)

**目标**: `WatchLUT('a.cube')` 和 `WatchLUT('./a.cube')` 应视为同 entry.

**实施**: WatchLUT 内调 SDL_GetPrefPath / realpath 规范化后存储. 现版本接受相对路径不规范化, 用户须传一致路径.

---

## 3. 用户支持

### 3.1 使用模板

```lua
-- 启动时一次性 watch (不用 LoadCubeLUT/LoadHaldLUT)
local lut_id = HDR.WatchLUT('assets/luts/cinematic.cube')
if lut_id then
    HDR.SetGradingLUT(lut_id, 1.0)
end

-- 主循环 (按需 poll, 推荐 1 Hz)
local accumulator = 0
function update(dt)
    accumulator = accumulator + dt
    if accumulator > 1.0 then
        accumulator = 0
        local n = HDR.PollLUTReloads()
        if n > 0 then
            print('LUT reloaded, count =', n)
            -- g.lutTexId 已自动更新, 无需做任何事
        end
    end
end

-- 退出时清理
HDR.UnwatchLUT(lut_id)
```

### 3.2 调试 / 排查

**情况 1**: WatchLUT 返 nil
```
"file read failed"      → path 错或文件不存在
"stbi_load failed"      → 不是合法图像
"out of range"          → size 越界
"image not square"      → HALD 必须方阵
```

**情况 2**: PollLUTReloads 总返 0
- 检查 `HDR.GetLUTHotReload()` 是否 true
- 检查 path 是否真有变化 (文件系统的 mtime 精度 ~1s, 太快保存可能 mtime 不变)

**情况 3**: reload 后画面无变化
- `PollLUTReloads` 已自动同步 `g.lutTexId` (如果之前 SetGradingLUT)
- 但 `HDR.Tonemap(rgn, {lut=cached_id, ...})` 中用户缓存的 lutId 旧, 需用 `GetWatchedLUTId(path)` 取最新

### 3.3 性能建议

- `PollLUTReloads` 调用频率: 1 Hz (1秒1次) 够用. 每帧调也行 (10 个 watched < 1ms).
- 关掉热重载: `HDR.SetLUTHotReload(false)` (发布版本)
- watched LUT 数量: 建议 < 20 (实际用 1-5 个就够)

---

## 4. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_8_3.md` | scope + 12 决策矩阵 |
| `DESIGN_PhaseF_0_10_8_3.md` | parser 算法 + 接口契约 |
| `TASK_PhaseF_0_10_8_3.md` | 6 原子任务 + 依赖图 |
| `FINAL_PhaseF_0_10_8_3.md` | 项目总结 + 12 决策落地 + LUT 闭环生态 |
| **`TODO_PhaseF_0_10_8_3.md`** | 本文件 |
