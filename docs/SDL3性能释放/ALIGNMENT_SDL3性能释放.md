# ALIGNMENT — ChocoLight SDL3 性能释放(三阶段闭环)

> 创建日期: 2026-05-08 | 基于: ENGINE_EVALUATION.md / SDL3迁移 FINAL / 当前 v0.3 引擎评测
> 任务定位: 在 SDL3 迁移工程基础上,**真正释放** SDL3 升级的性能红利,同时修复现存 GL 后端的 draw call 浪费

---

## 一、项目特性规范(当前 v0.3 现状)

### 1.1 技术栈

| 层级 | 技术 | 版本/状态 |
|------|------|----------|
| 虚拟机 | Lumen (Lua 5.1 兼容 C++17) | v1.3.x,无 JIT |
| 原生模块 | ChocoLight DLL | v0.3 |
| 窗口/事件 | SDL3 | release-3.2.0,**仅作 GLFW 替身** |
| 渲染 | OpenGL 3.3 Core / GLES 3.0 | VAO/VBO + shader,**无 EBO/批渲染** |
| 音频 | miniaudio + FFmpeg(回退) | 已脱离 SDL_AUDIO |
| 物理 | Box2D v2.4.1 | 桌面/移动可用 |
| 资源加载 | 同步 fopen / `ifstream` | 主线程阻塞 |
| 平台数 | 6 (Windows/Linux/macOS/Web/Android/iOS) | 全部 CI 绿 |

### 1.2 SDL3 在引擎中的当前应用面

经全量源码扫描(`@e:\jinyiNew\Light\ChocoLight\src`)证实:

| SDL3 子系统 | 编译开关 | 引擎调用次数 |
|-------------|---------|------------|
| SDL_INIT_VIDEO | ON | 1(`platform_window_sdl3.cpp:159`) |
| SDL_INIT_EVENTS | ON | 1 |
| SDL_INIT_GAMEPAD | ON | 1 |
| SDL_GL_* (上下文) | 默认 | 5(创建/销毁/Make/Swap/ProcAddr) |
| SDL_PollEvent | 默认 | 1 |
| **SDL_GPU** | **OFF**(显式禁用 `CMakeLists.txt:34`) | **0** |
| SDL_AUDIO | OFF | 0 |
| SDL_RENDER | OFF | 0 |
| SDL_HINT_* | — | **0** |
| SDL_AddTimer / Thread / Atomic | — | **0** |
| SDL_AsyncIO / Storage | — | **0** |
| SDL_StartTextInput / IME | — | **0** |
| SDL_Camera / Sensor / Pen | — | **0** |

**结论**: SDL3 在引擎中等同于 "GLFW + 移动端窗口入口",升级红利留在桌上。

### 1.3 关键性能瓶颈定位(实测代码证据)

| 瓶颈 | 位置 | 严重度 |
|------|------|--------|
| 每粒子 1 次 DrawArrays | `light_particles.cpp:138-157` | 🔴 1024 粒子 = 1024 draw call |
| 每 sprite 1 次 DrawArrays | `light_graphics.cpp:347/412/532/956` | 🔴 字符/精灵不批渲染 |
| 每 tile 1 次 DrawArrays | `light_tilemap.cpp:94` | 🔴 一屏几百 draw call |
| Quad→Tri 用 `std::vector` 每帧分配 | `render_gl33.cpp:301-313` | 🟠 内存压力 |
| 无 EBO,顶点冗余 6/4 = +50% | `render_gl33.cpp` | 🟠 VBO 带宽浪费 |
| `glBufferSubData` 替代 `glMapBufferRange` | `render_gl33.cpp:291` | 🟡 失去零拷贝 |
| 资源加载主线程阻塞 | 散落各模块 fopen | 🟡 启动卡顿 |

---

## 二、原始需求

来自用户问题与本次决策:

> **"分析项目,总结项目优缺点以及优化建议和优化方向;SDL3 引擎升级后的性能释放是否应用到引擎中"**
> 用户选择: **全面优化(三阶段闭环) — Batcher + SDL_GPU + AsyncIO**

---

## 三、边界确认

### 3.1 范围内 ✅

#### Phase A — 立即收益层(GL 后端优化)
- A1 实现 **Sprite Batcher** + 共享 EBO,所有 2D 绘图收敛为批处理
- A2 改造 `RenderBackend::DrawArrays` 路径,Quad→Tri 走静态 EBO 而非每帧 vector
- A3 在 `platform_window_sdl3.cpp::Init` 补齐 SDL_HINT 性能/兼容调优
- A4 升级 SDL3 到 release-3.2.x 最新补丁

#### Phase B — 架构级红利层(SDL_GPU 后端)
- B1 在 `RenderBackend` 抽象之上,**新增 `SDLGPUBackend`** 实现,与 `GL33Backend`/`LegacyBackend` 平级
- B2 CMake 打开 `SDL_GPU=ON`,运行时按平台/驱动选择: D3D12(Win) / Metal(macOS,iOS) / Vulkan(Linux,Android),旧设备/Web 回退 GL33
- B3 Shader 处理:Phase B1 阶段提供 GLSL → SPIR-V 离线编译 + 运行时反射,移动端 backbone 用 SDL_shadercross 统一
- B4 验证桌面/移动 4 平台原生 GPU API 可用

#### Phase C — 资源加载现代化层
- C1 引入 `SDL_AsyncIO`(SDL 3.2+) 替代同步 fopen,后台线程加载图片/音频/字体
- C2 引入 `SDL_OpenStorage` 抽象,Android 直读 APK 资源、iOS 直读 Bundle、桌面映射 fs
- C3 资源加载完成回调走 Lua 异步 callback,保留同步 API 兼容

### 3.2 范围外 ❌

- Lumen JIT(独立大任务,推迟到 v0.5)
- WebGPU 后端(SDL_GPU 不支持 Web,Web 平台 Phase B 维持 GL33Backend)
- ECS-Physics 自动同步桥接(独立任务)
- AES-256 加密替换 XOR(独立安全任务)
- TextInput / IME 完整支持(独立任务,但 Phase A 中顺手补 SDL_HINT_IME)
- Audio Mixer / 3D Sound(独立任务)
- 现有 24 个 Lua 绘图 API 的破坏性变更
- Box2D / SQLite / FFmpeg 升级
- Windows 反调试体系修改

### 3.3 跨平台支持矩阵

| 平台 | Phase A | Phase B(SDL_GPU 主路径) | Phase B(GL33 兜底) | Phase C |
|------|:-------:|:----------------------:|:-----------------:|:-------:|
| Windows | ✅ | D3D12 | ✅ GL33 | ✅ |
| Linux | ✅ | Vulkan | ✅ GL33 | ✅ |
| macOS | ✅ | Metal | ✅ GL33 | ✅ |
| Android | ✅ | Vulkan(API 26+)/GLES3 | ✅ GLES3 | ✅ |
| iOS | ✅ | Metal | ✅ GLES3 | ✅ |
| Web/WASM | ✅ | ❌(SDL_GPU 不支持) | ✅ WebGL2(GL33) | ⚠️ 仅 IndexedDB 同步 |

---

## 四、需求理解

### 4.1 Phase A: Sprite Batcher + EBO + Hint(3-4 天)

- **现状**: 1024 粒子触发 1024 次 draw call
- **目标**: 单帧 typical 2D 场景 draw call 降至 ≤ 5(纹理切换/scissor 切换/blend 切换才 flush)
- **方案**: `RenderBackend` 增加 `BeginBatch / SubmitQuad / SubmitTriangles / EndBatch`,引擎所有现有 `DrawArrays` 调用点改走批处理路径,语义零变化
- **EBO**: 预生成 65536 quad 的索引缓冲 `[0,1,2,0,2,3, 4,5,6,4,6,7, ...]`,VBO 仅存 4 顶点/quad,`glDrawElements` 渲染
- **SDL_HINT**:补齐 IME / Wayland AppID / 鼠标焦点 / VSync hint
- **不破坏 Lua API**:所有 24 个绘图函数签名/语义完全保持

### 4.2 Phase B: SDL_GPU 后端(2-3 周)

- **现状**: SDL_GPU 显式禁用,渲染走 OpenGL
- **目标**: 在 5 个原生平台上提供 SDL_GPU 主路径,GL33 仅作兜底
- **方案**: 新增 `SDLGPUBackend : public RenderBackend`,实现同接口;Shader 用 SDL_shadercross 离线编译 HLSL → MSL/SPIR-V/DXIL
- **运行时选择**: `CreateRenderBackend()` 增加平台/驱动探测优先级,环境变量 `CHOCO_RENDER_BACKEND=gl33|sdlgpu` 强制覆盖
- **批渲染逻辑复用**: Phase A 的 Batcher 设计为后端无关,Phase B 直接复用

### 4.3 Phase C: SDL_AsyncIO + SDL_Storage(1 周)

- **现状**: 资源加载主线程阻塞,Android/iOS 需手动 JNI 抽资源
- **目标**: 后台线程异步加载,Android/iOS 直读原生资源容器
- **方案**:`light_asset.cpp` 新模块,封装 `SDL_OpenStorage` + `SDL_AsyncIO`,`Light.Asset.Load(path, callback)` 暴露给 Lua
- **现有 API 兼容**:`Light.Graphics.Image:New("path")` 等同步 API 内部走 wait-on-handle 模式,新增 `:NewAsync(path, cb)`

### 4.4 验证策略

| 阶段 | 验证手段 |
|------|---------|
| 单元 | 每子任务 lightc -p 语法检查 + 模块注册检查 |
| 运行 | scripts/smoke/ 新增 perf_smoke.lua + sdlgpu_smoke.lua + asset_async_smoke.lua |
| 性能 | 新建 samples/perf_benchmark/ 压力场景(粒子 / Tilemap / 文本) |
| CI | GitHub Actions origin/main,6 平台 build-templates.yml 全绿 |
| 推送 | 仅推 origin (https://github.com/futzhj/ChocoLightEngine.git),不推 iosndesign |

---

## 五、疑问清单(需用户决策)

### 🔴 高优先级 — 阻塞架构设计

| # | 问题 | 影响 | 我的建议 |
|---|------|------|--------|
| Q1 | **是否使用 git worktree 隔离三阶段开发?** | 主分支保持干净 / 长任务流期间不阻塞日常 | **建议使用**: 创建 `feature/sdl3-perf-release` 分支于 worktree `C:\Users\Administrator\.config\superpowers\worktrees\Light\sdl3-perf-release`,与你既有的 `phase3-smoke-validation`、`love-physics-p0-p1` 实践一致 |
| Q2 | **Phase B 的 Shader 工作流?** | 决定工程复杂度 | a. **SDL_shadercross**(单源 HLSL → SPIR-V/MSL/DXIL,需引入 shadercross 工具链,推荐) <br> b. 手写多份 GLSL/MSL/HLSL 源(无新工具,但维护翻倍) <br> c. 仅 SPIR-V + 运行时翻译(SDL_GPU 内部跨编译,但缺反射元信息) <br> **建议 a**:工业界主流,SDL3 官方 example 也用此方案 |
| Q3 | **Phase B 完成后 GL33 后端去留?** | 决定是替换还是并存 | a. **永久并存**(SDL_GPU 主,GL33 兜底,旧设备/Web 必需) <br> b. 移动端逐步替换 GL33(Android API < 26 / iOS < 13 失去支持) <br> **建议 a**:与 SDL3 官方策略一致,Web 平台必需 GL33,且 GL33 已通过 CI 验证 |
| Q4 | **Phase A 与 Phase B 谁更优先?** | 决定子任务调度 | a. **严格 A → B → C**(顺序闭环,每阶段独立交付,推荐) <br> b. A 与 B 并行(资源足够,但 Batcher 设计需后端中立,有返工风险) <br> **建议 a** |

### 🟡 中优先级 — 影响交付范围

| # | 问题 | 影响 | 我的建议 |
|---|------|------|--------|
| Q5 | **Web 平台是否同步引入 WebGPU 后端?** | 扩大跨平台一致性 | **建议否**:WebGPU 在 Emscripten 支持仍不稳定,SDL_GPU 不支持 Web,Web 维持 WebGL2(GL33Backend),作为 v0.5 独立任务 |
| Q6 | **是否新建 perf benchmark 场景?** | 性能验证可复现 | a. **新建 `samples/perf_benchmark/`**(独立目录,不污染现有 sample,推荐) <br> b. 复用 `Light-0.2.3/` 添加压力开关 <br> **建议 a**:目标对齐 6A 工作流的"边界限制" |
| Q7 | **Phase A 的 SDL3 版本升级到哪个 tag?** | 决定 FetchContent GIT_TAG | a. release-3.2.20(当前最新稳定) <br> b. 维持 3.2.0 不动(变化最小) <br> **建议 a**:获取 3.2.x 累积 bug 修复,API 二进制兼容 |
| Q8 | **Phase B 是否需要 Lua 端用户 API 改动?** | 影响向后兼容 | a. **零改动**(Lua 完全不感知 backend 切换,推荐) <br> b. 暴露 `Light.Graphics.GetBackendName()`(诊断用,可选) <br> **建议 b**:一行只读 API,不破坏兼容 |

### 🟢 低优先级 — 实现细节

| # | 问题 | 建议 |
|---|------|------|
| Q9 | Phase A 的 Batcher 最大 quad 数 | **65536**(uint16 索引上限) |
| Q10 | 性能基准目标 | **10000 sprites @ 60fps,移动端 5000 sprites @ 60fps** |
| Q11 | SDL_HINT 配置时机 | **PlatformWindow::Init 之前(SDL_Init 前生效)** |

---

## 六、自主决策记录

基于项目惯例和行业实践,以下事项已自主决策,无需用户回答:

1. **任务名 / 文档目录**: `SDL3性能释放` / `docs/SDL3性能释放/`(对齐 `SDL3迁移`、`引擎升级` 中文目录习惯)
2. **推送目标**: 仅 `origin`(GitHub 仓库 https://github.com/futzhj/ChocoLightEngine.git),不推 iosndesign
3. **编译验证**: 全部走 GitHub Actions `build-templates.yml`,本机仅做开发态验证,不作最终验证依据
4. **smoke 脚本目录**: 新增 `scripts/smoke/perf_smoke.lua` / `sdlgpu_smoke.lua` / `asset_async_smoke.lua`
5. **Lua API 向后兼容**: 现有 24 个绘图 API 签名/语义完全保持,Phase A/B 内部全透明
6. **CI 触发**: 每阶段独立 PR + Actions 全平台绿后合并 main
7. **API 注释规范**: 沿用现有 `@lua_api` / `@brief` / `@param` 体系,Phase B/C 新模块同等注释密度
8. **错误处理策略**: SDL_GPU 创建失败 → 自动回退 GL33,日志 WARN 级输出,不向用户抛错
9. **Shader 缓存**: Phase B 编译产物存 `<exe_dir>/cache/shaders/` 仅 release 模式启用
10. **AsyncIO 线程数**: 默认 SDL 调度(SDL_AsyncIO 自管),不开放手动配置

---

## 七、用户决策确认 ✅(2026-05-08)

用户选择:**全部采纳 ALIGNMENT 推荐方案**

| # | 问题 | 用户决策 | 备注 |
|---|------|---------|------|
| Q1 | 是否使用 git worktree 隔离 | ✅ 采纳 | 创建 `feature/sdl3-perf-release` 分支于 `C:\Users\Administrator\.config\superpowers\worktrees\Light\sdl3-perf-release` |
| Q2 | Phase B Shader 工作流 | ✅ SDL_shadercross | 单源 HLSL → SPIR-V/MSL/DXIL,工业界主流方案 |
| Q3 | Phase B 完成后 GL33 去留 | ✅ 永久并存 | GL33Backend 作 Web/旧设备兜底,SDL_GPU 失败自动回退 |
| Q4 | A/B/C 调度 | ✅ 严格串行 | A → B → C 闭环,每阶段独立 PR,Actions 全绿后合并 main |
| Q5 | Web 是否引入 WebGPU | ❌ 不引入 | 推迟到 v0.5 独立任务,Web 维持 WebGL2 (GL33Backend) |
| Q6 | perf benchmark 位置 | ✅ 新建独立目录 | `samples/perf_benchmark/` |
| Q7 | SDL3 升级 tag | ✅ release-3.2.x 最新 | Phase A 任务 A4 中升级到当前最新 3.2.x 稳定补丁 |
| Q8 | 暴露 `GetBackendName` Lua API | ✅ 是 | 一行只读诊断 API,不破坏兼容 |

> 所有关键决策已确认,进入 CONSENSUS → DESIGN → TASK 阶段。
