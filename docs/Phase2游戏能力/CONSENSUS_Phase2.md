<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 23:31:29
 * @LastEditTime: 2026-04-26 01:48:41
-->
# CONSENSUS — Phase 2 游戏能力

## 确认的需求与方案

### 执行顺序
1. **输入管理器** (Light.Input) — 手柄/摇杆/多点触摸/虚拟按键
2. **粒子系统** (Light.Graphics.Particles) — 2D 粒子发射器
3. **Tilemap** (Light.Graphics.Tilemap) — 正交/等距地图 + Tiled TMX
4. **Box2D 物理** (Light.Physics) — 刚体/碰撞/关节
5. **ECS** (Light.ECS) — 纯 Lua 实现

### 技术决策
| 决策点 | 结论 |
|--------|------|
| ECS 实现层 | 纯 Lua (作为内嵌脚本) |
| Box2D 版本 | v3.x (C API, FetchContent) |
| Tilemap 格式 | Tiled TMX (XML 解析) + 自定义二进制 |
| Tilemap 渲染 | 批量绘制 (DrawArrays 一次提交一个 layer) |
| 粒子池大小 | 默认 1024, Lua 可配置 |
| 手柄 API | SDL3 Gamepad API |

### 验收标准
1. **输入管理器**: Lua 脚本可查询手柄按钮/摇杆, 多点触摸有独立 ID
2. **粒子系统**: Lua 创建发射器, 配置参数, Update/Draw 集成到主循环
3. **Tilemap**: 加载 .tmx 文件, 正交渲染正确, 支持多图层
4. **Box2D**: Lua 创建世界/刚体/形状, Step 模拟, 碰撞回调
5. **ECS**: Lua 创建实体/组件/系统, 查询/遍历正常
6. **全平台**: 6 平台 CI 编译通过
