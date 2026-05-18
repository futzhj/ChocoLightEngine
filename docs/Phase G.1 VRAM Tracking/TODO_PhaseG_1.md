# Phase G.1 — VRAM Tracking TODO (v2+ 候选)

> Phase G.1 已交付 (commit `ef91120`, CI 全平台绿, 13/13 smoke PASS).
> 本文档记录用户/未来扩展可能需要的事项. **无任何阻塞 TODO**.

## 一. 用户配置 TODO

**无**. Phase G.1 是纯引擎自计 tracker, 零配置, 零密钥, 零依赖. 直接使用 `Light.Graphics.GetMemoryStats()` 即可.

## 二. v2 扩展候选 (按优先级)

### P0 (高价值, 实现简单)

#### 1. Bloom mipmap chain 跟踪
- **价值**: Bloom 是大块占用 (1080p ≈ 30 MB, 6-7 levels mipmap)
- **改动**: 1 处 hook 在 `bloom_renderer.cpp::CreateRT`
- **公式**: `base × (1 + 1/4 + 1/16 + ... ) ≈ base × 4/3`
- **估时**: 0.5h

#### 2. SSAO RT 跟踪 (occlusion + blur)
- **价值**: 1 项 R8/R16F 占 ~2-4 MB
- **改动**: 1 处 hook 在 `ssao_renderer.cpp`
- **估时**: 0.5h

#### 3. Auto Exposure luminance RT 跟踪
- **价值**: R16F 1x1 (1 KB), 体积小但完整性高
- **改动**: 1 处 hook 在 `auto_exposure_renderer.cpp`
- **估时**: 0.5h

#### 4. TAAU outputSceneTex 主动 Track
- **现状**: 仅 ReleaseRT 路径已 Untrack, CreateRT 路径未 Track (TAAU 创建 outputSceneTex 在另一函数)
- **改动**: 找到 TAAU outputSceneTex 创建点, 加 1 行 Track
- **估时**: 0.3h

### P1 (中价值, 改动适中)

#### 5. 用户 Image / ImageData 跟踪
- **价值**: 用户大量 Sprite 时可见占用
- **难点**: 50+ 处 `glGenTextures + glTexImage2D`, 需要在 `Image::Create*` / `ImageData::Create*` 高层 wrapper 加 hook
- **改动**: ~10 处 (覆盖 PNG/JPG/HDR/EXR/ImageData/RenderToTexture)
- **估时**: 2h

#### 6. Font glyph atlas 跟踪
- **价值**: 大字号 + CJK + emoji 时可见
- **改动**: 1 处 hook 在 `light_graphics_font.cpp::EnsureAtlas`
- **估时**: 1h

#### 7. Mesh VBO/EBO 跟踪
- **价值**: 大型 GLB 模型加载后可见占用
- **难点**: `asset_loader.cpp` worker thread 上传, 需要 **mutex 化 tracker**
- **改动**: tracker 加 `std::mutex` + 2-3 处 hook 在 mesh upload 路径
- **估时**: 2h (含 mutex 化)

### P2 (低价值或工程繁琐)

#### 8. Per-instance ID 字段
- **价值**: split-screen 调试时区分 instance
- **改动**: `GpuMemItem` 加 `int instance_id` 字段; `Track/Untrack` 多一参数; Lua items[] 多 `instance_id`
- **估时**: 1.5h

#### 9. VRAM 预算 + 超额抛 Lua error
- **价值**: mobile 平台主动限制
- **设计**: `Light.Graphics.SetMemoryBudget(bytes)` + Track 时若超额 -> `lua_error`
- **风险**: 触发位置在 RT 创建点, error 抛出会中断渲染初始化, 需仔细评估
- **估时**: 1.5h

#### 10. OSD 实时显存表 (HUD overlay)
- **价值**: 调试时方便, 但用户也可自行 Lua 调 GetMemoryStats 用 BatchRenderer 画
- **改动**: 引擎层加 `Light.Graphics.ShowMemoryHUD(true)`
- **估时**: 2h (重复 RecordOSD 模式)

#### 11. NVAPI / DXGI 实测显存 (Windows only)
- **价值**: 对比 tracker 估算 vs 真实 OS 占用 (差值 = 驱动开销 + 不跟踪部分)
- **难点**: 仅 NVIDIA / Windows; cross-platform 难一致
- **估时**: 3h (含 driver detect)

#### 12. RenderDoc / Tracy 集成
- **价值**: 专业 GPU profiler 接管
- **估时**: 5h+

## 三. 已知"接受"限制 (不计入 TODO)

以下为 v1 明确接受的简化, 非 bug:

| 限制 | 原因 |
|------|------|
| silent fallback over-report ~1.5 MB | legacy GL2 normalTex 创建失败时 tracker 仍计入; 差异 < 1MB, 注释明示 |
| 不跟踪用户 Image | 改动面广 (50+ 点), v1 焦点在 RT (占 90%+) |
| 不算 mipmap | 跟踪 RT 都不开 mipmap; Bloom mipmap 算 v2 (P0 #1) |
| 64 项上限 | 现 ~30 项, 2× headroom; 满则静默丢弃 (理论不会触发) |
| 不区分 instance | 多 instance 累加 count 已能体现 |
| Reset 不动 GPU | 诊断工具与 GPU 操作分离 (Reset 后重 Enable 会 count 漂移, 这是预期) |
| 无 mutex | 现所有 hook 在主线程; worker upload 时 (v2 P1 #7) 再加 |

## 四. 操作指引 (用户如何使用)

### 4.1 基础查询
```lua
local m = Light.Graphics.GetMemoryStats()
print(string.format("VRAM: %.1f MB", m.total_bytes / 1048576))
```

### 4.2 详细诊断 (per-item)
```lua
local m = Light.Graphics.GetMemoryStats()
table.sort(m.items, function(a, b) return a.bytes > b.bytes end)
for _, it in ipairs(m.items) do
    print(string.format("%6.1f MB  %-28s %s %dx%d x%d",
        it.bytes / 1048576, it.name, it.format, it.w, it.h, it.count))
end
```

### 4.3 多 instance 显存监控
```lua
-- 创建 4 个 HDR instance (split-screen)
for i = 1, 4 do
    Light.Graphics.HDR.CreateInstance("player_" .. i)
    Light.Graphics.HDR.SetActiveInstance("player_" .. i)
    Light.Graphics.HDR.Enable(960, 540)
end

local m = Light.Graphics.GetMemoryStats()
-- 应看到 HDR sceneTex / depthRBO 等 count=4
for _, it in ipairs(m.items) do
    if it.count > 1 then
        print(string.format("[multi] %s ×%d (%.1f MB total)",
            it.name, it.count, it.bytes / 1048576))
    end
end
```

### 4.4 切换 velocity format 测内存差异
```lua
Light.Graphics.HDR.Enable(1920, 1080)
local before = Light.Graphics.GetMemoryStats().total_bytes
Light.Graphics.HDR.SetVelocityFormat("RG8")  -- 切到紧凑格式
Light.Graphics.HDR.Resize(1920, 1080)         -- 触发 Untrack 旧 + Track 新
local after = Light.Graphics.GetMemoryStats().total_bytes
print(string.format("RG16F->RG8 saved: %.1f MB", (before - after) / 1048576))
```

### 4.5 mobile 上手动检查预算
```lua
local function check_budget(max_mb)
    local m = Light.Graphics.GetMemoryStats()
    local mb = m.total_bytes / 1048576
    if mb > max_mb then
        error(string.format("VRAM over budget: %.1f MB > %.1f MB", mb, max_mb))
    end
end

-- 启用所有特效后检查
Light.Graphics.HDR.Enable(1920, 1080)
Light.Graphics.TAA.Enable(1920, 1080)
Light.Graphics.SSR.Enable(1920, 1080)
check_budget(150)  -- 150 MB 预算
```

## 五. 后续 Phase 候选 (与 G.1 关系)

| Phase 候选 | 与 G.1 关系 |
|------------|-------------|
| G.2 Bloom mipmap tracking | 直接扩展 (P0 #1) |
| G.3 Asset memory tracking | mutex 化 (P1 #7) |
| G.4 VRAM budget enforcement | 上层 (P2 #9) |
| H.x GPU profiler 整合 | 完全独立模块 |
