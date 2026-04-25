# ALIGNMENT — ChocoLight 引擎升级计划

> 创建日期: 2026-04-25 | 基于: ENGINE_EVALUATION.md 改进路线图

---

## 一、项目特性规范

### 1.1 技术栈

| 层级 | 技术 | 版本 |
|------|------|------|
| 虚拟机 | Lumen (Lua 5.1 兼容 C++17) | v1.3.2 |
| 原生模块 | ChocoLight DLL | v0.2.3 |
| 构建系统 | CMake | ≥3.16 |
| 图形 | OpenGL 1.x/2.x + GLFW 3.4 | 固定管线 |
| 音视频 | FFmpeg 59/57/4 动态加载 + WinAPI PlaySound | — |
| 网络 | WinSock2 | 仅 Windows |
| 数据库 | SQLite3 amalgamation | 内嵌 |
| 字体 | stb_truetype (动态字形缓存) | — |
| 图片 | stb_image | — |
| 音频库 | miniaudio.h (已在 third_party，尚未集成) | — |

### 1.2 代码规模

- **Lumen VM**: 67 文件, 26,613 LOC
- **ChocoLight DLL**: 16 源文件 + 3 头文件, ~6,554 LOC
- **总计**: ~35,000 LOC

### 1.3 架构模式

- DLL 导出 `luaopen_Light_*` 入口函数，由 Lumen VM `require` 加载
- 模块内部用全局 `struct` 或 `static` 变量持有上下文
- OOP 通过 Lua metatable 继承实现 (`Light:New` 框架)
- 图形: `glBegin/glEnd` 固定管线 + FBO Canvas
- 音频: FFmpeg 解码 → PCM → Windows PlaySound (阻塞)
- 网络: WinSock2 直接调用, 无跨平台抽象

### 1.4 目标平台

Windows x64 / Linux x64 / macOS Universal / Android / iOS / Web(WASM)

---

## 二、原始需求 (来自 ENGINE_EVALUATION.md §七)

### Phase 1: 基础增强 (短期)
1. 升级 OpenGL 3.3 core profile + shader pipeline
2. 跨平台网络 (POSIX socket / libuv)
3. 集成 miniaudio 替代 PlaySound
4. 自动生成 API 文档 (从代码注释)

### Phase 2: 游戏能力 (中期)
5. 集成 Box2D 物理引擎
6. 场景图 / ECS 实体系统
7. 粒子系统
8. Tilemap 支持
9. 输入管理器 (手柄 / 触摸)

### Phase 3: 高级特性 (长期)
10. Lumen JIT 编译器
11. Vulkan / Metal 渲染后端
12. 可视化编辑器
13. 热重载开发工具
14. AES-256 加密替代 XOR

---

## 三、边界确认

### 范围内
- Phase 1 的 4 项任务为本次升级核心目标
- Phase 2/3 仅做架构预留，不实现

### 范围外
- Lumen VM 内核修改 (GC/JIT)
- 移动端模板更新 (需独立任务)
- 工具链 (lightpack) 修改
- 现有 Lua API 的破坏性变更

---

## 四、需求理解

### 4.1 OpenGL 3.3 升级
- **现状**: `light_graphics.cpp` 使用 `glBegin/glEnd`、`glTranslatef`、`glRotatef` 等 GL 1.x 固定管线调用
- **目标**: 迁移到 OpenGL 3.3 Core Profile, 使用 VAO/VBO + shader pipeline
- **影响面**: `light_graphics.cpp`, `light_graphics_canvas.cpp`, `light_graphics_image.cpp`, `light_graphics_font.cpp`, `light_av.cpp`(Video 纹理)
- **约束**: 必须保持 Lua API 完全兼容 (Draw/Print/Rectangle/... 签名不变)
- **跨平台**: macOS 最高支持 OpenGL 4.1; Web(WASM) 需要 OpenGL ES 3.0 / WebGL 2.0; Android/iOS 需 ES 3.0

### 4.2 跨平台网络
- **现状**: `light_network.cpp` 硬编码 `#include <winsock2.h>`, 使用 `SOCKET`/`WSAStartup` 等 Win32 API
- **目标**: Linux/macOS/Web 也能使用网络功能
- **方案选择**: POSIX socket (轻量) vs libuv (异步, 重依赖)
- **约束**: Lua API 保持不变 (Http:Open/SendRequest/OnHttp)

### 4.3 miniaudio 集成
- **现状**: `miniaudio.h` 已存在于 `third_party/`, 但音频播放使用 Windows `PlaySound` (阻塞, 仅WAV)
- **目标**: 跨平台异步音频播放, 支持音量/混音/格式
- **影响面**: `light_av.cpp` 中的 Audio 播放部分
- **约束**: FFmpeg 解码链保持不变, 仅替换播放后端

### 4.4 API 文档生成
- **现状**: 代码注释详尽 (IDA 还原标注), 但缺少用户可读的 API 手册
- **目标**: 从 C++ 代码注释自动生成 Lua API 文档
- **方案**: 自定义标注格式 + Python/Node 脚本生成 Markdown/HTML

---

## 五、疑问清单 (需决策)

### 🔴 高优先级

| # | 问题 | 影响 | 建议 |
|---|------|------|------|
| Q1 | OpenGL 升级是否保留 GL 1.x 回退路径? | 兼容老旧 GPU / 简化调试 | **建议保留** — 通过编译宏 `LIGHT_USE_LEGACY_GL` 切换 |
| Q2 | 跨平台网络选 POSIX socket 还是 libuv? | libuv 更强大但增加 ~200KB 依赖 | **建议 POSIX socket** — 引擎定位轻量, 且已有同步模型 |
| Q3 | miniaudio 是否替代 FFmpeg 解码? | miniaudio 自带解码器 (MP3/WAV/FLAC) | **建议分层**: FFmpeg 解码 → PCM, miniaudio 仅做播放后端 |

### 🟡 中优先级

| # | 问题 | 影响 | 建议 |
|---|------|------|------|
| Q4 | shader 管理方案: 内嵌字符串 vs 外部文件? | 发行包复杂度 | **建议内嵌** — 作为 C++ 字符串常量, 避免文件依赖 |
| Q5 | 是否向 Lua 暴露 shader 自定义 API? | 扩展能力 vs 复杂度 | **Phase 1 不暴露**, Phase 2 视需求开放 |
| Q6 | API 文档格式选择? | 用户体验 | **建议 Markdown** — 可直接在 GitHub 阅读, 后续可转 HTML |

---

## 六、自主决策记录

基于项目架构分析和同类引擎 (LÖVE 2D, Defold) 实践:

1. **GL 3.3 升级策略**: 采用 glad 加载器 + 内嵌 shader 字符串, 与 LÖVE 2D 方案一致
2. **网络抽象层**: 引入 `platform_socket.h` 抽象, Windows 用 WinSock2, 其他用 POSIX socket
3. **miniaudio 集成**: 使用 `ma_device` 做低级播放, FFmpeg 继续做解码, 解耦明确
4. **文档**: 自定义 `@lua_api` 标注 + Python 脚本提取生成

---

## 七、用户决策确认 ✅

| # | 问题 | 用户决策 | 备注 |
|---|------|----------|------|
| Q1 | OpenGL 回退策略 | **运行时检测** | 启动时检测 GPU 能力, 自动选择 GL 1.x 或 3.3 路径 |
| Q2 | 跨平台网络方案 | **libuv** | 全功能异步I/O库, 支持 DNS/timer/fs, ~200KB 依赖 |
| Q3 | miniaudio 定位 | **双路径共存** | miniaudio 解码常见格式(MP3/WAV/FLAC), FFmpeg 处理稀有格式, 自动回退 |

> 所有关键决策已确认, 进入架构阶段。
