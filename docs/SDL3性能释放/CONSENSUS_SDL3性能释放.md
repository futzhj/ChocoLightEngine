# CONSENSUS — ChocoLight SDL3 性能释放(三阶段闭环)

> 创建日期: 2026-05-08 | 基于: ALIGNMENT_SDL3性能释放.md
> 用户决策: **全部采纳推荐方案**

---

## 一、明确的需求描述

### 1.1 核心目标(一句话)

> 在 ChocoLight v0.3 现状基础上,以三阶段闭环(A→B→C)真正释放 SDL3 升级带来的性能红利,**移动端 GPU 节能 30-50%、桌面 draw call 吞吐 2-5×、典型 2D 场景 draw call 数从 N 降至 ≤5**,同时保证现有 24 个绘图 Lua API 完全向后兼容。

### 1.2 三阶段交付物

| 阶段 | 名称 | 工期 | 主要交付 |
|:----:|------|:----:|---------|
| **A** | Sprite Batcher + EBO + Hint | 3-4 天 | 批渲染、共享 EBO、SDL_HINT、SDL3 升级 |
| **B** | SDL_GPU 后端 | 2-3 周 | `SDLGPUBackend`、shadercross 工具链、4 平台原生 GPU |
| **C** | SDL_AsyncIO + Storage | 1 周 | 异步资源加载、Android/iOS 原生资源容器 |

---

## 二、技术实现方案

### 2.1 整体策略

- **后端兼容并存**: GL33 / Legacy / SDLGPU 三个 RenderBackend 平级,运行时按平台/驱动/环境变量选择
- **API 无破坏**: Lua 24 个绘图函数签名不变,新增 `Light.Graphics.GetBackendName()` 一只读诊断函数
- **批渲染后端无关**: Phase A 设计的 SpriteBatcher 与具体后端解耦,Phase B 直接复用
- **Shader 单源工作流**: HLSL 作主语言,SDL_shadercross 离线交叉编译为 SPIR-V/MSL/DXIL,Phase B 启用
- **资源加载兼容**: Phase C 同步 API 内部走 wait-on-handle,新增 Async API 走 callback

### 2.2 关键技术选型

| 组件 | 选型 | 理由 |
|------|------|------|
| 批渲染索引 | uint16 索引 + 65536 quad/batch 上限 | 平台兼容性最高,移动端 GLES2 也支持 |
| EBO 拓扑 | 静态 [0,1,2,0,2,3, 4,5,6,4,6,7, ...] 共享 | 一次生成永久使用,零分配 |
| Buffer 上传 | `glBufferSubData` (Phase A) → `glMapBufferRange` (Phase B 时机优化) | 兼容 GL 3.3 / GLES 3.0 |
| Shader 工具链 | SDL_shadercross + DXC | SDL3 官方推荐,工业界主流 |
| Async IO | `SDL_AsyncIO` (SDL 3.2+) | 平台原生 IO,SDL 自管线程池 |
| 存储抽象 | `SDL_OpenStorage` | Android APK / iOS Bundle / 桌面 fs 统一 |

### 2.3 后端运行时选择算法

```
CreateRenderBackend():
  1. 读取环境变量 CHOCO_RENDER_BACKEND (gl33|legacy|sdlgpu|auto)
  2. 默认 auto:
     a. 平台/编译开关检查:
        - Web/WebGL2 → GL33
        - Android API < 26 / iOS < 13 → GLES3 (GL33 路径)
     b. 优先尝试 SDL_GPU (D3D12/Metal/Vulkan)
     c. 失败回退 GL33
     d. GL33 也失败回退 Legacy
  3. 日志 INFO 输出: "RenderBackend: SDLGPU/D3D12 selected" 等
```

---

## 三、技术约束

### 3.1 兼容性约束

- ✅ **现有 24 个 Lua 绘图 API 签名/语义完全保持**
- ✅ **现有 .lua 脚本零修改运行**(包括 Light-0.2.3/、samples/、examples/)
- ✅ **GL33Backend 永久保留**(Web 必需、旧设备兜底)
- ✅ **Phase A/B/C 任意阶段独立可发布**

### 3.2 性能目标

| 场景 | 当前 v0.3 | Phase A 后 | Phase B 后 |
|------|----------|-----------|-----------|
| 1024 粒子 draw call/帧 | 1024 | 1 | 1 |
| 10000 sprite @60fps | 不达标 | ✅ 达标 | ✅ 达标 + 50% CPU 余量 |
| 移动端 GPU 功耗(同场景) | 100% | ~95% | ~50-70% |
| 桌面 GPU driver overhead | 100% | ~80% | ~30-50% |

### 3.3 平台约束

| 平台 | Phase A | Phase B | Phase C |
|------|:-------:|:-------:|:-------:|
| Windows x64 | ✅ GL33 | ✅ D3D12 主 / GL33 兜底 | ✅ |
| Linux x64 | ✅ GL33 | ✅ Vulkan 主 / GL33 兜底 | ✅ |
| macOS Universal | ✅ GL33 | ✅ Metal 主 / GL33 兜底 | ✅ |
| Android arm64 | ✅ GLES3 | ✅ Vulkan(API≥26) / GLES3 | ✅ Storage 直读 APK |
| iOS arm64 | ✅ GLES3 | ✅ Metal | ✅ Storage 直读 Bundle |
| Web/WASM | ✅ GL33 | ⚠️ 维持 GL33 (SDL_GPU 不支持 Web) | ⚠️ 同步 fopen(无 AsyncIO) |

### 3.4 工程约束

- 工作流: 6A 标准流程,每阶段独立 6A 子循环
- 隔离: git worktree `feature/sdl3-perf-release`
- CI: GitHub Actions `build-templates.yml` 6 平台全绿才合并
- 推送: 仅 origin (https://github.com/futzhj/ChocoLightEngine.git)
- smoke: 每阶段新增对应 smoke 脚本 + lightc -p 验证
- 文档: 沿用 `@lua_api`/`@brief`/`@param` 注释规范

---

## 四、集成方案

### 4.1 与现有架构的集成点

```
┌──────────────────────────────────────────────────────────┐
│  Lua 脚本层 (24 个绘图 API,完全不变)                     │
│  Light.Graphics.Draw / DrawSprite / Print / Tilemap ...  │
└──────────────────────────────────────────────────────────┘
                           ↓ Phase A 增加批渲染入口
┌──────────────────────────────────────────────────────────┐
│  light_graphics.cpp / light_particles.cpp / 等 模块       │
│   修改: 各 DrawArrays 调用点 → BatchRenderer::Submit*    │
└──────────────────────────────────────────────────────────┘
                           ↓ Phase A 新增
┌──────────────────────────────────────────────────────────┐
│  BatchRenderer (新增,后端无关)                            │
│   - Begin/End/Flush                                       │
│   - SubmitQuad / SubmitTri                                │
│   - 状态变化时自动 Flush(纹理/blend/scissor 切换)        │
└──────────────────────────────────────────────────────────┘
                           ↓ 通过 RenderBackend 接口
┌──────────────────────────────────────────────────────────┐
│  RenderBackend 抽象 (扩展接口)                            │
│   - 新增 BatchSubmit(VBO* + EBO*)                         │
│   - 现有 DrawArrays 保留作非批路径                        │
└──────────────────────────────────────────────────────────┘
                           ↓ 多实现
┌────────────┬────────────┬────────────────────────────────┐
│ GL33 (有)  │ Legacy(有) │ SDLGPU(Phase B 新增)           │
│ + EBO 扩展 │ 不动       │ HLSL→shadercross→native        │
└────────────┴────────────┴────────────────────────────────┘
                           ↓ 通过 PlatformWindow + SDL3
┌──────────────────────────────────────────────────────────┐
│  PlatformWindow (SDL3 实现,Phase A 补 HINT)              │
└──────────────────────────────────────────────────────────┘

资源侧 (Phase C 新增,与上面正交):
┌──────────────────────────────────────────────────────────┐
│  Lua: Light.Asset.LoadAsync(path, cb) / 同步 API 不变     │
└──────────────────────────────────────────────────────────┘
                           ↓
┌──────────────────────────────────────────────────────────┐
│  light_asset.cpp (新增,封装 SDL_AsyncIO + SDL_Storage)   │
└──────────────────────────────────────────────────────────┘
```

### 4.2 文件级影响清单

#### Phase A 影响

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `ChocoLight/include/render_backend.h` | 修改 | 增加 `BeginBatch/SubmitQuad/SubmitTri/EndBatch` 接口 |
| `ChocoLight/include/batch_renderer.h` | **新增** | 后端无关批渲染层 |
| `ChocoLight/src/batch_renderer.cpp` | **新增** | Batcher 实现 |
| `ChocoLight/src/render_gl33.cpp` | 修改 | EBO 静态索引 + Batch 实现 |
| `ChocoLight/src/render_legacy.cpp` | 修改 | Batch 路径退化为单 quad 多次提交 |
| `ChocoLight/src/light_graphics.cpp` | 修改 | 24 个 DrawArrays 调用点 → Batch |
| `ChocoLight/src/light_particles.cpp` | 修改 | EmitterDraw 走 Batch |
| `ChocoLight/src/light_tilemap.cpp` | 修改 | Tilemap 渲染走 Batch |
| `ChocoLight/src/platform_window_sdl3.cpp` | 修改 | 补齐 SDL_HINT |
| `ChocoLight/CMakeLists.txt` | 修改 | SDL3 GIT_TAG 升级 |

#### Phase B 影响

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `ChocoLight/CMakeLists.txt` | 修改 | `SDL_GPU=ON`,链接 shadercross |
| `ChocoLight/include/render_backend.h` | 修改 | `CreateSDLGPUBackend()` 工厂声明 |
| `ChocoLight/src/render_sdlgpu.cpp` | **新增** | SDLGPUBackend 实现 |
| `ChocoLight/shaders/*.hlsl` | **新增** | 单源 HLSL Shader |
| `ChocoLight/shaders/build_shaders.py` | **新增** | 离线 shadercross 脚本 |
| `ChocoLight/shaders/generated/*.h` | **新增** | 编译产物头文件(SPIR-V/MSL/DXIL 字节嵌入) |
| `ChocoLight/src/light_module.cpp` | 修改 | 注册 `Light.Graphics.GetBackendName()` |

#### Phase C 影响

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `ChocoLight/include/light_asset.h` | **新增** | Asset 模块接口 |
| `ChocoLight/src/light_asset.cpp` | **新增** | SDL_AsyncIO + SDL_Storage 封装 + Lua 绑定 |
| `ChocoLight/src/light_graphics_image.cpp` | 修改 | Image:NewAsync 入口 |
| `ChocoLight/src/light_av.cpp` | 修改 | AV:LoadAsync 入口 |
| `ChocoLight/CMakeLists.txt` | 修改 | 新增 light_asset.cpp 源文件 |

---

## 五、任务边界限制

### 5.1 不允许做(明确划界)

- ❌ 修改 Lumen VM 内核(`lumen-master/`)
- ❌ 修改 Box2D / SQLite / FFmpeg / miniaudio 任何第三方库
- ❌ 修改现有 Lua 24 个绘图 API 的签名或语义
- ❌ 删除 GL33Backend 或 LegacyBackend
- ❌ 引入除 SDL_shadercross 外的新第三方依赖
- ❌ 修改反调试 / 加密 / Lumen 入口模块
- ❌ 修改 Android/iOS Java/ObjC 模板(除非 SDL_GPU 必需)
- ❌ 修改任何工作树脏文件(`.agents/skills/`、`examples/`、`scripts/smoke/` 现有 .lua、`tools/` 等)

### 5.2 必须做

- ✅ Phase A/B/C 各自走完整 6A 子流程,不跳步
- ✅ 每阶段独立 PR,独立 CI 全绿才合并
- ✅ 每个新增文件含 `@file/@brief` 注释
- ✅ 每个新增 Lua API 含 `@lua_api/@brief/@param` 注释
- ✅ 每阶段交付独立 ACCEPTANCE 子文档
- ✅ Phase B 后端切换需有日志输出 + Lua 端可查询
- ✅ Phase C 异步加载需有错误回调(失败不能静默)

---

## 六、验收标准

### 6.1 Phase A 验收(立即收益层)

| # | 验收项 | 验证方式 |
|---|--------|---------|
| A-V1 | `samples/perf_benchmark/` 1024 粒子 draw call/帧 ≤ 3 | RenderDoc 抓帧 + perf_smoke.lua 输出 |
| A-V2 | 1000 字符文本 draw call/帧 ≤ 5(按字体纹理切换) | 同上 |
| A-V3 | 现有 24 个 Lua API 签名零变化 | grep diff 验证 |
| A-V4 | 现有 sample/example 全部正常运行 | smoke 脚本 + 本地运行 |
| A-V5 | 6 平台 GitHub Actions 全绿 | Actions 状态 |
| A-V6 | SDL3 升级到 release-3.2.x 最新补丁 | CMakeLists.txt diff |
| A-V7 | SDL_HINT 配置生效(IME/AppID/VSync) | 启动日志 |

### 6.2 Phase B 验收(架构级红利层)

| # | 验收项 | 验证方式 |
|---|--------|---------|
| B-V1 | `Light.Graphics.GetBackendName()` 在 Win/Linux/macOS 默认返回 D3D12/Vulkan/Metal | sdlgpu_smoke.lua |
| B-V2 | iOS 启动默认走 Metal,Android API≥26 走 Vulkan | 真机/模拟器日志 |
| B-V3 | `CHOCO_RENDER_BACKEND=gl33` 强制覆盖生效 | 环境变量测试 |
| B-V4 | SDL_GPU 创建失败自动回退 GL33,日志 WARN | 故障注入测试 |
| B-V5 | 同场景下,SDL_GPU 路径 GPU 占用 ≤ GL33 80% | 移动端 Xcode/Android Profiler |
| B-V6 | Web 平台维持 GL33,无 SDL_GPU 调用 | Web CI |
| B-V7 | 6 平台 GitHub Actions 全绿 | Actions 状态 |
| B-V8 | shadercross 离线编译产物可重现 | build_shaders.py 幂等性测试 |

### 6.3 Phase C 验收(资源加载层)

| # | 验收项 | 验证方式 |
|---|--------|---------|
| C-V1 | `Light.Asset.LoadAsync(path, cb)` 在主线程不阻塞 | asset_async_smoke.lua + 启动时间对比 |
| C-V2 | Android 直接读取 APK assets/(无 cache 抽取步骤) | Android 真机日志 |
| C-V3 | iOS 直接读取 Bundle Resources(无中间复制) | iOS 真机日志 |
| C-V4 | 加载失败走错误回调,不崩溃 | 故障注入(不存在路径) |
| C-V5 | 现有同步 `Image:New(path)` 完全兼容 | 现有 sample 运行 |
| C-V6 | Web 平台维持同步路径(SDL_AsyncIO 不可用),日志 INFO | Web CI |
| C-V7 | 6 平台 GitHub Actions 全绿 | Actions 状态 |

### 6.4 整体验收(三阶段闭环)

| # | 验收项 |
|---|--------|
| ALL-V1 | `docs/SDL3性能释放/` 完整 6 份文档(ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO) |
| ALL-V2 | 三阶段各自独立 PR 已合并 main,提交历史清晰 |
| ALL-V3 | `samples/perf_benchmark/` 性能基准达标 |
| ALL-V4 | 引擎评测分数 v0.3 → v0.4: 图形渲染 8.0→9.0,跨平台 9.5→9.8 |
| ALL-V5 | worktree `feature/sdl3-perf-release` 清理,main 同步 origin |

---

## 七、不确定性已解决确认

✅ 所有 8 个 ALIGNMENT 关键决策点已确认
✅ 三阶段任务边界已划定
✅ 验收标准具体可测试
✅ 文件级影响清单已列出
✅ 跨平台支持矩阵已确认
✅ 与现有架构集成方案已设计

> 进入 Phase 2 Architect — DESIGN 阶段。
