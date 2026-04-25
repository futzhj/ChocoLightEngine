# ALIGNMENT — Phase 2 游戏能力

## 原始需求

从 ENGINE_EVALUATION.md Phase 2 路线图：
- [ ] 集成 Box2D 物理引擎
- [ ] 场景图 / ECS 实体系统
- [ ] 粒子系统
- [ ] Tilemap 支持
- [ ] 输入管理器 (手柄 / 触摸)

## 项目上下文分析

### 现有架构模式
| 特征 | 说明 |
|------|------|
| 模块注册 | `LT::RegisterModule(L, name, funcs)` → 创建 `Light.XXX` 子表 |
| Lua API 风格 | LNI handle-based OOP (`Light(Light.XXX):New()`) |
| 渲染 | `RenderBackend` 抽象 (GL3.3 / GLES3 / Legacy), 2D 基元绑定已完整 |
| 事件 | `PlatformWindow::PollEvent` 拉模式 → 分发 OnKey/OnMouse/OnTouch |
| 平台 | 6 平台 (Win/Linux/macOS/Android/iOS/Web), SDL3 统一后端 |
| 构建 | CMakeLists.txt 条件编译, FetchContent 拉 SDL3 |
| 第三方库 | stb_image, miniaudio, sqlite3, libuv — 全嵌入源码编译 |

### 缺失能力
| 能力 | 当前状态 |
|------|---------|
| 物理 | 无, Graphics 仅绘制无碰撞 |
| ECS | 无, Lua 端手动管理对象 |
| 粒子 | 无 |
| Tilemap | 无, Image 可手动拼但无专用 API |
| 手柄输入 | SDL3 有 Gamepad API 但引擎未接入, PlatformWindow::Event 无 Gamepad 类型 |

### 技术约束
1. **Lua 5.1 (Lumen)** — 无 `__gc` userdata 以外的自动回收, 模块需遵循现有 OOP 模式
2. **RenderBackend 抽象** — 新绘制代码必须通过 `g_render->DrawArrays()`, 不能直接调 GL
3. **6 平台兼容** — 新增第三方库必须支持全平台编译或提供条件编译
4. **逆向还原代码风格** — 保持 `light_xxx.cpp` 命名和 `luaopen_Light_XXX` 导出模式
5. **CMake FetchContent** — 大型依赖 (Box2D) 用 FetchContent, 小型库 (粒子/Tilemap) 嵌入 third_party

## 需求理解

### 1. Box2D 物理引擎
- **范围**: 2D 刚体 + 碰撞检测 + 关节约束
- **接口**: `Light.Physics.World`, `Light.Physics.Body`, `Light.Physics.Shape`
- **版本**: Box2D v3.x (C API, 单头文件, MIT 协议)
- **集成方式**: FetchContent + 静态链接

### 2. 场景图 / ECS
- **范围**: 轻量 ECS (Entity-Component-System), 不需要完整框架
- **实现**: 纯 Lua 层实现 (性能足够, 避免 C++ 复杂度)
- **接口**: `Light.ECS.World`, `Light.ECS.Entity`, `Light.ECS.System`

### 3. 粒子系统
- **范围**: 2D 粒子发射器 (位置/速度/加速度/颜色/大小/生命周期)
- **接口**: `Light.Graphics.Particles`
- **实现**: C++ 粒子池 + RenderBackend 批量绘制

### 4. Tilemap
- **范围**: 正交/等距 Tile 地图, 支持 Tiled (.tmx) 格式加载
- **接口**: `Light.Graphics.Tilemap`
- **实现**: C++ 解析 + 批量绘制优化

### 5. 输入管理器
- **范围**: 手柄/摇杆 (SDL3 Gamepad API) + 多点触摸 + 虚拟按键
- **接口**: `Light.Input`
- **实现**: 扩展 PlatformWindow::Event + 新 Lua 模块

## 疑问

### 需要确认的关键决策

1. **ECS 实现层**: 纯 Lua 还是 C++? 纯 Lua 开发快但性能有上限
2. **Box2D 版本**: v3.x (新 C API, 更轻量) 还是 v2.4.x (经典 C++ API)?
3. **Tilemap 格式**: 仅 Tiled TMX? 还是也支持自定义二进制格式?
4. **优先级排序**: 5 个功能哪个先做? 建议:
   - **P0**: 输入管理器 (手柄) — 最基础, 其他功能也需要
   - **P1**: 粒子系统 — 视觉反馈, 复杂度低
   - **P2**: Tilemap — 2D 游戏核心
   - **P3**: Box2D 物理 — 依赖最重
   - **P4**: ECS — 可后期纯 Lua 补充
5. **Tilemap 渲染**: 逐 tile 绘制还是 batch 为大纹理? batch 性能好但内存占用高
