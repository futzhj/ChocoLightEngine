# Phase E.1.7 — 2D Lighting Demo

演示 ChocoLight Phase E.1 的 2D forward 多光照系统端到端集成：
- `Light.Lighting2D` 多光状态管理（C++ 单例）
- `Light.Graphics.DrawLit / DrawLitQuad`（GL33 后端 sprite_lit_2d shader）
- ECS `Light2D` / `LitSprite` component + `_UploadLights2D` 自动 view-space 转换
- 键盘 / 鼠标实时交互（光开关 + spot 朝向）

## 运行

```bash
# 默认 800×600 窗口, ESC 退出
light.exe samples/demo_2d_lighting/main.lua

# 自动退出 (秒)
light.exe samples/demo_2d_lighting/main.lua 10
```

无窗口环境（CI / headless）会自动进入降级路径：调 5 帧 `_UploadLights2D`，校验 `GetLightCount() == 3`，立即退出。

## 场景

| 元素 | 配置 |
|------|------|
| Camera2D | 位置 (0, 0)，zoom = 1，viewport 800×600 |
| Sprite 网格 | 8 列 × 6 行 = 48 个 `LitSprite`，64×48 像素，居中布局 |
| Ambient | `(0.15, 0.15, 0.20)` 偏冷的低光环境 |
| Point #1 | 暖橘色 `(1.0, 0.6, 0.3)`，左上 (200, 150)，range 250，intensity 1.5 |
| Point #2 | 冷蓝色 `(0.3, 0.6, 1.0)`，右下 (600, 450)，range 250，intensity 1.2 |
| Spot | 白色 `(1, 1, 1)`，中央 (400, 300)，range 350，innerAngle 15°/outerAngle 35°，朝向跟随鼠标 |

## 交互

| 输入 | 作用 |
|------|------|
| `ESC` | 退出 |
| `1` | 切换 Point #1 enabled |
| `2` | 切换 Point #2 enabled |
| `3` | 切换 Spot enabled |
| 鼠标移动 | 改变 spot 朝向 (从 spot 位置指向鼠标) |

## 验收要点（E.1.7 §"验收标准"）

| 标准 | 期望视觉 |
|------|----------|
| ambient 可见 | 关闭所有光（按 1 2 3）时 sprite 显示为 ambient × baseColor（很暗） |
| 加 1 point + ambient 后合理 | 仅开 light 1 时，左上区域明显变橘色，远端衰减到 ambient |
| spot 锥形衰减 | 仅开 spot 时，看到锥形光斑，内角全亮 / 外角到暗淡 smoothstep 过渡 |
| 切换实时反馈 | 按 1/2/3 瞬间生效（HUD 显示 ON/OFF 状态） |

## 实现速记

| 文件 | 角色 |
|------|------|
| `main.lua` (~210 行) | demo 入口：模块加载 + 8×6 ECS 网格 + 3 灯 + UI.Window 主循环 |
| `Light.Lighting2D` (E.1.3 + E.1.4) | 16 light slot + ambient + Lua API |
| `Light.Graphics.DrawLit` (E.1.5) | sprite_lit_2d shader + uniform 上传 |
| `ECS Light2D / LitSprite` (E.1.6) | `_UploadLights2D` 每帧 ClearLights + 重新 Add，自动 view-space 转换 |

## 缺失资源 / 已知限制

- **未引入真实 normalMap 资源**：当前 demo 仅用 mock 纯色 sprite（`makeMockImage` 返回 `GetWidth/GetHeight` 假对象），LitSprite 的 `normalMap = nil`，shader 走默认 `N=(0,0,1)` 平面光照。如果用户提供 `normal.png`，可改 `LitSprite` 加 `normalMap = Light(Light.Graphics.Image):New("normal.png")` 触发凹凸效果。
- **1000 lit quad < 16ms 性能验证**：当前 demo 仅 48 个 sprite，性能 benchmark 留到后续 `perf_benchmark/lit2d.lua`。
- **`view space ↔ world space` 用户感知**：用户直接调 `Light.Lighting2D.AddPointLight{x, y}` 时，坐标应与 sprite 渲染时所用 vertex space 一致；ECS 集成会自动 `(world - cam) * zoom` 转换。
