# Phase G.1.7 — Lua API 容错 audit (TASK)

> **创建日期**: 2026-05-19
> **基于**: DESIGN_PhaseG_1_7.md (方案 C 全面重构)
> **总工时**: 36-46h, 拆 6 个子阶段

---

## 一. G.1.7.0 — 基础设施 + 第一批高风险文件 (本期)

### T0.1 — 创建 light_lua_helpers.h
- **输入**: DESIGN §2 helpers 设计
- **输出**: `@e:/jinyiNew/Light/ChocoLight/src/light_lua_helpers.h`
- **验收**: 头文件编译无错; 包含 `LT::Magic4` / `LT::CheckInstance<T>` / `LT::TryCheckInstance<T>` / `LT::CheckStringStrict` / `LT::OptStringStrict` / `LT::PushNilError` / `LT::PushBooleanError`
- **依赖**: 无
- **估时**: 0.5h

### T0.2 — 在 helpers 头部声明全 Magic 常量表 (~30 个)
- **输入**: DESIGN §3 magic 表
- **输出**: 在 light_lua_helpers.h 内声明 30+ `LT_MAGIC_*` constexpr
- **验收**: 全部唯一; 编译期常量
- **依赖**: T0.1
- **估时**: 0.3h

### T0.3 — 改造 light_graphics_image.cpp (P0)
- **输入**: 现有 ImageContext 结构体 + GetImageCtx
- **输出**:
  - ImageContext 加 `uint32_t magic` 首字段
  - 创建路径设置 `magic = LT_MAGIC_IMAGE`
  - GetImageCtx → CheckImage 走 LT::CheckInstance
  - __gc 路径设 `magic = 0xDEADBEEF`
- **验收**: smoke 4-5 用例 PASS; 老用法零回归
- **估时**: 0.5h

### T0.4 — 改造 light_graphics_canvas.cpp (P0)
- **输入**: CanvasContext + GetCanvasCtx
- **输出**: 加 magic 字段 + CheckInstance 改造
- **验收**: smoke 3-4 用例 PASS
- **估时**: 0.5h

### T0.5 — 改造 light_graphics.cpp 中的 FontCtxHeader 与 anon CanvasCtx (P0)
- **输入**: 两处 anon struct (line 287 / 572 / 1056)
- **输出**:
  - line 287 / 572 的 `CanvasCtx` 改用 light_graphics_canvas.cpp 的 CanvasContext (磁场化)
  - line 1056 的 `FontCtxHeader` 加 magic 字段 (在 ctx struct 共享头加)
  - 改用 LT::TryCheckInstance (兼容老 nullptr 返回 fallback)
- **验收**: 不破坏现有 SetCanvas / Push/PopCanvas 流程; smoke 3 用例
- **估时**: 0.7h

### T0.6 — 改造 light_db.cpp (P1)
- **输入**: SQLiteContext + GetSQLiteCtx
- **输出**: 加 magic + CheckSQLite 改造
- **验收**: smoke 3 用例 (pcall query 错误参数 / type confusion / nil __instance)
- **估时**: 0.4h

### T0.7 — 改造 light_av.cpp (P1)
- **输入**: AVContext + VideoWrapper + GetAVCtx + GetVideoWrapper
- **输出**: 两个 ctx 都加 magic, 区分 AVCT / VIDO
- **验收**: smoke 4 用例
- **估时**: 0.5h

### T0.8 — 改造 light_data.cpp (P1)
- **输入**: DataBuffer + 内部辅助
- **输出**: 加 magic + CheckBuffer 改造; 注意 line 137 / 257 处 `lua_isuserdata + lua_touserdata` (raw byte data 不是 Buffer instance, 无需 magic, 但需 cast 成 const uint8_t* 时校验来源)
- **验收**: smoke 3 用例
- **估时**: 0.5h

### T0.9 — 改造 light_network.cpp (P1)
- **输入**: HttpContext + GetHttpCtx
- **输出**: 加 magic + CheckHttpCtx 改造
- **验收**: smoke 3 用例
- **估时**: 0.4h

### T0.10 — 改造 light_particles.cpp (P2) + light_tilemap.cpp (P2)
- **输入**: ParticleEmitter + TilemapData
- **输出**: 加 magic + Check 改造
- **验收**: smoke 各 2 用例
- **估时**: 0.5h

### T0.11 — smoke 框架 + 35 用例
- **输入**: DESIGN §6 测试维度
- **输出**: `@e:/jinyiNew/Light/scripts/smoke/lua_api_robustness.lua`
  - A: nil 参数 (5 用例)
  - B: 错类型基本类型 (5 用例)
  - C: 错 OOP table fake __instance (9 用例, 9 类型互冲)
  - D: 错 userdata 外壳 (3 用例)
  - E: nil __instance (5 用例)
  - F: magic mismatch (8 用例, 9 类型 cross-product 抽样)
- **验收**: 全 35 用例 PASS, 全 headless 兼容
- **估时**: 1.5h

### T0.12 — CMake + CI 接入
- **输入**: 现有 CMakeLists.txt + build-templates.yml
- **输出**:
  - light_lua_helpers.h 加入 ChocoLight target (header-only, 无 .cpp)
  - Windows job 加 `lua_api_robustness.lua` smoke
- **验收**: 6 平台 build 不破坏; Windows runtime smoke 35/35 PASS
- **估时**: 0.3h

### T0.13 — progress.txt + 文档更新
- **输入**: 完成的子任务清单
- **输出**:
  - `@e:/jinyiNew/Light/docs/Phase G.1.7 Lua API Robustness Audit/progress.txt`
  - 更新 ALIGNMENT 状态 = G.1.7.0 完成
- **验收**: 进度可追溯
- **估时**: 0.2h

### T0.14 — commit + push + CI 验证
- **输入**: 全部 G.1.7.0 改动
- **输出**: git commit + push + CI run id
- **验收**: 6/6 平台绿; smoke 35/35 PASS; 全套既有 smoke 零回归
- **估时**: 0.4h

**G.1.7.0 累计估时**: 6.7h, 与计划 6-8h 一致

---

## 二. G.1.7.1 — Graphics 子系统 (后续会话)

### 文件清单 (15 文件)
- `light_graphics_mesh.cpp` (Mesh ctx + GLTF mesh asset)
- `light_graphics_shader.cpp` (Shader ctx + uniforms / SSBO)
- `light_graphics_material.cpp` (PBR Material ctx)
- `light_graphics_spriteanimation.cpp` (sprite animation)
- `light_lighting2d.cpp` (Light2D ctx)
- `light_camera.cpp` (Camera ctx + view/proj matrix wrapping)
- `light_blendmode.cpp` (无 ctx, 仅枚举)
- `light_pixels.cpp` (像素格式)
- `light_surface.cpp` (Surface ctx)
- `light_cursor.cpp` (Cursor ctx)
- `light_display.cpp` (display info, 无 ctx)
- `light_gpumem.cpp` (engine internal, 无 user-facing ctx)
- `light_graphics.cpp` (剩余 4 处 inline cast 改造完成)
- `light_animation.cpp` (评估是否磁场化, 已用 luaL_checkudata)
- `light_asset.cpp` (G.1.6 已优秀, 仅做 sanity smoke)

### 估时分布
- 高风险 (Mesh/Shader/Material/SpriteAnimation): 0.5h × 4 = 2h
- 中风险 (Lighting2D/Camera/Surface/Cursor/Animation): 0.4h × 5 = 2h
- 低风险 / 验证 (其他): 0.3h × 6 = 1.8h
- smoke 增量 (~25 用例): 1.5h
- progress + commit: 0.5h
- **累计**: 7.8h

---

## 三. G.1.7.2 — Audio + Network 子系统

### 文件清单 (15 文件)
- Audio (5): `light_audio.cpp` / `light_audio_backend.cpp` / `light_audio_effect.cpp` / `light_audio_group.cpp` / `light_audio_sound.cpp`
- Network (4): `light_network_udp.cpp` / `light_network_rpc.cpp` / `light_network_room.cpp` / `light_network_web.cpp`
- IO (3): `light_io.cpp` / `light_iostream.cpp` / `light_filesystem.cpp` (已是标杆)
- Misc (3): `light_clipboard.cpp` / `light_dialog.cpp` / `light_messagebox.cpp`

### 估时分布
- Audio (4 ctx 类型, 高风险): 2.5h
- Network (4 ctx 类型, 高风险): 2.5h
- IO/Misc (大多无 ctx 或简单): 1h
- smoke (~30 用例): 1.5h
- progress + commit: 0.5h
- **累计**: 8h

---

## 四. G.1.7.3 — Physics + Animation + ECS

### 文件清单 (15 文件)
- `light_physics.cpp` (5 helpers - World/Body/Shape/Fixture/Joint)
- `light_physics3d.cpp` (Bullet 3D world + body)
- `light_animation.cpp` (Skeleton/Clip/Animator/SkinnedMesh - 已用 luaL_checkudata, 加 magic)
- `light_ecs.cpp` (Entity + Component)
- `light_particles.cpp` (G.1.7.0 已做, sanity)
- `light_tilemap.cpp` (G.1.7.0 已做, sanity)
- `light_lighting2d.cpp` (G.1.7.1 已做, sanity)
- 其他 game logic (e.g. `light_camera.cpp` 已做)

### 估时分布
- Physics (5 helpers, 复杂): 3h
- Physics3D (4-5 ctx 类型): 2h
- Animation (4 ctx 类型, 已部分实现): 1h
- ECS (Entity + Component): 1h
- smoke (~30 用例): 1.5h
- progress + commit: 0.5h
- **累计**: 9h

---

## 五. G.1.7.4 — 系统类剩余

### 文件清单 (~20 文件)
- Input (5): `light_input.cpp` / `light_keyboard.cpp` / `light_mouse.cpp` / `light_gamepad.cpp` / `light_joystick.cpp`
- 设备 (3): `light_haptic.cpp` / `light_sensor.cpp` / `light_touch.cpp`
- 系统 (4): `light_time.cpp` / `light_log.cpp` / `light_locale.cpp` / `light_url.cpp`
- 工具 (4): `light_cpuinfo.cpp` / `light_endian.cpp` / `light_guid.cpp` / `light_atomic.cpp`
- 进程 / 库 (4): `light_process.cpp` / `light_loadso.cpp` / `light_plugins.cpp` / `light_hidapi.cpp`

### 估时分布
- 大多数无 ctx (常量+函数): 0.2h × 12 = 2.4h
- 少数有 ctx (input devices / process / plugins): 0.5h × 8 = 4h
- smoke (~25 用例): 1.5h
- progress + commit: 0.5h
- **累计**: 8.4h

---

## 六. G.1.7.5 — 完结 + 全 fuzz smoke + benchmark

### T5.1 — 全 fuzz smoke 100+ 用例汇总
- **输入**: G.1.7.0~G.1.7.4 各批次 smoke
- **输出**: `lua_api_robustness.lua` 累计 100+ 用例
- **估时**: 1h

### T5.2 — magic 检查性能 benchmark
- **输入**: 改造完成的 ctx Check helpers
- **输出**:
  - benchmark Lua 脚本: 调用 1000 次 Image:GetWidth() / SQLite:Query() 等, 测量 wall-clock
  - C++ 内部 benchmark: 直接调 LT::CheckInstance 100 万次
- **验收**: magic 检查开销 < 1% 总时间
- **估时**: 1h

### T5.3 — FINAL + ACCEPTANCE + TODO 文档
- **输入**: 6 个子阶段结果
- **输出**:
  - `FINAL_PhaseG_1_7.md` (整体交付概览, ~400 行)
  - `ACCEPTANCE_PhaseG_1_7.md` (验收清单)
  - `TODO_PhaseG_1_7.md` (后续可选项: 静态分析工具集成 / fuzz smoke 持续扩充 / API 文档化)
- **估时**: 1h

### T5.4 — HANDOFF 同步 + commit + push
- **输入**: 全部完成
- **输出**: HANDOFF_REMAINING_TASKS.md 标 G.1.7 完成
- **估时**: 0.5h

### T5.5 — CI run 验证 + 文档回填
- **输入**: 最终 commit
- **输出**: docs-only commit 回填 CI run id + 实测数据
- **估时**: 0.5h

**G.1.7.5 累计估时**: 4h, 与计划 4-5h 一致

---

## 七. 总体时间预算

| 子阶段 | 估时 | 累计 |
|--------|------|------|
| G.1.7.0 (本期) | 6.7h | 6.7h |
| G.1.7.1 | 7.8h | 14.5h |
| G.1.7.2 | 8h | 22.5h |
| G.1.7.3 | 9h | 31.5h |
| G.1.7.4 | 8.4h | 39.9h |
| G.1.7.5 | 4h | 43.9h |

**总计**: ~44h, 与用户预期 30-40h+ 区间一致 (取上界).

---

## 八. 验收门控 (每子阶段)

每子阶段 commit 前必须满足:

1. ✅ 该批所有目标 ctx struct 加 magic 字段
2. ✅ 所有 GetXxxCtx 函数走 LT::CheckInstance / TryCheckInstance
3. ✅ 所有创建路径正确设置 magic
4. ✅ 该批 smoke 用例 100% PASS
5. ✅ 全套既有 smoke (TAAU/VRAM/GLTF Async/Audio/SSR/Bloom/Tonemap/G.1.6 Preload) 零回归
6. ✅ 6 平台 build 全绿
7. ✅ progress.txt 更新

---

## 九. 异常处理

如遇:
- ctx struct 体积过小 (e.g. <16B), magic 占 25%+ 内存 → 评估是否豁免 (一般不豁免, 收益 < 1KB)
- ctx struct 已用 luaL_checkudata + metatable (e.g. light_animation.cpp / light_asset.cpp) → 加 magic 双保险, 不去除 metatable
- 用户传入合法 OOP table 但 __instance 是另一类型 (合法 type confusion 攻击) → magic 检查必须 PASS, 抛错
- 多线程 ctx (e.g. AssetLoader future) → magic 字段不变, 但要保证创建/销毁的内存 visibility (atomic 不必, magic 是 const 后初始化)

---

## 十. 进入 Implement 阶段

下一步: 创建 `light_lua_helpers.h` + 改造第一批 9 文件 + smoke + commit.
