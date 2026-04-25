# CONSENSUS — ChocoLight 引擎 Phase 1 升级共识

> 创建日期: 2026-04-25 | 所有不确定性已解决

---

## 一、明确的需求描述

### 升级目标
将 ChocoLight 引擎从当前状态升级至 Phase 1 基础增强水平，包含 4 项核心任务：

| # | 任务 | 现状 | 目标 |
|---|------|------|------|
| T1 | 图形管线升级 | GL 1.x 固定管线 (glBegin/glEnd) | GL 3.3 Core + 运行时自动回退 GL 1.x |
| T2 | 跨平台网络 | WinSock2 仅 Windows | libuv 全平台异步网络 |
| T3 | 音频系统 | Windows PlaySound (阻塞) | miniaudio 跨平台 + FFmpeg 稀有格式回退 |
| T4 | API 文档 | 仅代码注释 | 自动生成 Lua API 文档 (Markdown) |

### 核心约束
- **Lua API 零破坏**: 所有 Lua 层接口签名保持不变
- **跨平台兼容**: Windows/Linux/macOS/Android/iOS/Web
- **渲染后端抽象**: 设计接口层为 Phase 3 (Vulkan/Metal) 做架构预留

---

## 二、技术实现方案

### T1: OpenGL 3.3 升级 + 运行时回退

**方案**: 引入渲染后端抽象层 `RenderBackend`

- 启动时通过 GLFW 请求 GL 3.3 Core Profile
- 成功 → 使用 GL 3.3 后端 (glad 加载器 + VAO/VBO + shader)
- 失败 → 回退创建 GL 2.1 兼容上下文, 使用 Legacy 后端
- shader 以 C++ 字符串常量内嵌 (不暴露给 Lua)
- 新增文件: `render_backend.h`, `render_gl33.cpp`, `render_legacy.cpp`
- 修改文件: `light_graphics.cpp`, `light_graphics_canvas.cpp`, `light_graphics_image.cpp`, `light_ui.cpp`

**GL 调用盘点 (需替换)**:

| GL 1.x 调用 | 出现文件 | GL 3.3 替代 |
|-------------|---------|-------------|
| `glBegin/glEnd/glVertex3f` | graphics, av | VAO/VBO + `glDrawArrays` |
| `glPushMatrix/glPopMatrix` | graphics | 自管理矩阵栈 (glm) |
| `glTranslatef/glRotatef/glScalef` | graphics | uniform mat4 MVP |
| `glColor4f` | graphics | uniform vec4 color |
| `glTexCoord2f` | graphics | VBO 顶点属性 |
| `glEnable(GL_TEXTURE_2D)` | graphics | shader 纹理采样 |
| `wglGetProcAddress` | canvas | glad 统一加载 |

### T2: libuv 跨平台网络

**方案**: 引入 libuv, 封装为 `platform_net.h` 抽象层

- libuv 作为 `third_party/libuv` 子模块
- 封装 TCP connect/listen/read/write 异步接口
- HTTP 解析逻辑保持不变, 仅替换底层 socket 操作
- 修改文件: `light_network.cpp`
- 新增文件: `platform_net.h`, `platform_net.cpp`
- CMake: 新增 libuv 依赖

### T3: miniaudio 双路径音频

**方案**: miniaudio 做主解码+播放, FFmpeg 做稀有格式回退

- 常见格式 (MP3/WAV/FLAC/OGG): miniaudio `ma_decoder` 直接解码+播放
- 稀有格式: FFmpeg 解码 → PCM → miniaudio `ma_device` 播放
- 自动检测: 先尝试 miniaudio, 失败后回退 FFmpeg
- 修改文件: `light_av.cpp`
- 新增文件: `light_audio_backend.h`, `light_audio_backend.cpp`
- CMake: 新增 miniaudio 编译 (已有头文件, 需 `#define MINIAUDIO_IMPLEMENTATION`)

### T4: API 文档自动生成

**方案**: 自定义 `@lua_api` 标注 + Python 提取脚本

- 在 C++ 代码中添加结构化注释标注
- Python 脚本扫描 `src/*.cpp`, 提取生成 `docs/api/` 下 Markdown 文件
- 新增文件: `tools/gen_api_doc.py`, `docs/api/*.md`

---

## 三、技术约束

| 约束 | 说明 |
|------|------|
| C++17 | 保持与 Lumen VM 一致 |
| CMake ≥ 3.16 | 现有构建系统 |
| GLFW 3.4 | 窗口管理不变 |
| 无 Lua API 破坏 | 所有 24 个 Graphics 函数签名不变 |
| DLL 导出兼容 | `luaopen_Light_*` 入口不变 |
| 第三方库最小化 | glad (生成文件), libuv (~200KB), miniaudio (已有) |

---

## 四、验收标准

### T1: 图形管线
- [ ] 在支持 GL 3.3 的机器上, 使用 shader 管线渲染
- [ ] 在仅支持 GL 2.1 的机器上, 自动回退到固定管线
- [ ] 所有 24 个 Lua Graphics API 行为不变
- [ ] Canvas FBO 正常工作
- [ ] 字体渲染 (Print + CJK) 正常工作
- [ ] DrawSprite 精灵绘制正常工作

### T2: 跨平台网络
- [ ] Windows 上 HTTP GET/POST 正常
- [ ] Linux/macOS 上 HTTP GET/POST 正常
- [ ] WebSocket 连接和消息收发正常
- [ ] HttpServer 绑定和路由正常

### T3: 音频系统
- [ ] MP3/WAV/FLAC 通过 miniaudio 播放 (跨平台)
- [ ] 稀有格式通过 FFmpeg → miniaudio 播放
- [ ] Audio Play/Pause/Stop API 正常
- [ ] 非阻塞播放

### T4: API 文档
- [ ] 生成覆盖所有公开模块的 Markdown 文档
- [ ] 每个函数有签名、参数说明、示例
- [ ] 可在 GitHub 直接阅读

---

## 五、任务边界

### 范围内
- Phase 1 的 4 项任务
- 渲染后端抽象层设计 (为 Phase 3 预留)
- 必要的 CMake 构建系统更新
- 单元测试/验证脚本

### 范围外
- Lumen VM 修改
- Phase 2/3 功能实现
- 移动端模板更新
- lightpack 工具修改
- 现有 Lua API 签名变更
