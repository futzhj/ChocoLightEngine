# Light.Graphics

## `Light.Graphics.Push`

保存当前变换矩阵到栈

### 返回值

`void`

---

## `Light.Graphics.Pop`

从栈恢复上一个变换矩阵

### 返回值

`void`

---

## `Light.Graphics.Translate`

平移变换

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number` | 水平偏移 |
| `y` | `number` | 垂直偏移 |
| `z` | `number?` | 深度偏移 (默认 0) |

### 返回值

`void`

---

## `Light.Graphics.Rotate`

旋转变换

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `angle` | `number` | 旋转角度 (度) |
| `x` | `number?` | 旋转轴 X (默认 0) |
| `y` | `number?` | 旋转轴 Y (默认 0) |
| `z` | `number?` | 旋转轴 Z (默认 1) |

### 返回值

`void`

---

## `Light.Graphics.Scale`

缩放变换

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `sx` | `number` | X 缩放比 |
| `sy` | `number?` | Y 缩放比 (默认同 sx) |
| `sz` | `number?` | Z 缩放比 (默认 1) |

### 返回值

`void`

---

## `Light.Graphics.SetColor`

设置当前绘制颜色

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `r` | `number` | 红色 (0~1) |
| `g` | `number` | 绿色 (0~1) |
| `b` | `number` | 蓝色 (0~1) |
| `a` | `number?` | 透明度 (默认 1) |

### 返回值

`void`

---

## `Light.Graphics.GetColor`

获取当前绘制颜色

### 返回值

`number,number,number,number r,g,b,a`

---

## `Light.Graphics.SetCanvas`

设置当前渲染目标

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `canvas` | `Canvas|nil` | Canvas 离屏画布, nil 恢复默认 |

### 返回值

`void`

---

## `Light.Graphics.GetCanvas`

获取当前渲染目标

### 返回值

`Canvas|nil`

---

## `Light.Graphics.SetScissor`

设置裁剪区域 (无参数时禁用裁剪)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x` | `number?` | 裁剪区域左上 X |
| `y` | `number?` | 裁剪区域左上 Y |
| `w` | `number?` | 裁剪区域宽 |
| `h` | `number?` | 裁剪区域高 |

### 返回值

`void`

---

## `Light.Graphics.GetScissor`

获取当前裁剪区域

### 返回值

`number,number,number,number x,y,w,h`

---

## `Light.Graphics.Draw`

绘制纹理/图像到屏幕

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `drawable` | `Image|Canvas` | 可绘制对象 |
| `x` | `number?` | 水平位置 (默认 0) |
| `y` | `number?` | 垂直位置 (默认 0) |
| `z` | `number?` | 深度 (默认 0) |
| `rx` | `number?` | X 旋转 (后续 9 个变换参数可选) |

### 返回值

`void`

### 示例

```lua
local img = Light(Light.Graphics.Image):New("hero.png")
Light.Graphics.Draw(img, 100, 200)
```

---

## `Light.Graphics.DrawQuad`

绘制纹理子区域 (sprite sheet 裁切)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `drawable` | `Image|Canvas` | 可绘制对象 |
| `x` | `number?` | 屏幕位置 X |
| `y` | `number?` | 屏幕位置 Y |
| `z` | `number?` | 深度 |
| `qx` | `number?` | 子区域左上 X |
| `qy` | `number?` | 子区域左上 Y |
| `qw` | `number?` | 子区域宽 |
| `qh` | `number?` | 子区域高 |

### 返回值

`void`

---

## `Light.Graphics.DrawLit`

Phase E.1.5 — 绘制受 2D forward 多光照影响的 sprite（走 `sprite_lit_2d` shader + `Light.Lighting2D` state）

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `image` | `Image|Canvas\|nil` | baseColor 纹理；`nil` 时仅顶点色 |
| `normalMap` | `Image\|nil` | 法线贴图；`nil` 时 shader 用默认 `N=(0,0,1)` 平面光照 |
| `x` | `number?` | 水平位置 (默认 0) |
| `y` | `number?` | 垂直位置 (默认 0) |
| `z` | `number?` | 深度 (默认 0) |
| `rx, ry, rz, sx, sy, sz, ox, oy, oz` | `number?` | 与 `Draw` 一致的 9 个 transform 参数 |

### 返回值

`void`

### 行为

1. `g_render->SupportsLit2D() == false` 时直接 return（Legacy / 无 Lit2D 后端）
2. 主动 `BatchRenderer::Flush()`，保证累积的普通 sprite 先出再画 lit sprite
3. 构造 4 个 `RenderVertex2DLit`（默认 `normal=(0,0,1)`, `tangent=(1,0,0,1)`）
4. 内部调 `RenderBackend::DrawLit2DQuad`：切 program → 绑 texture → 上传 MVP/Model/HasNormalMap → `Lighting2D::UploadToShader` → glDrawElements → 切回默认 2D shader

### 示例

```lua
local hero    = Light(Light.Graphics.Image):New("hero.png")
local hero_n  = Light(Light.Graphics.Image):New("hero_normal.png")
Light.Lighting2D.SetAmbient(0.2, 0.2, 0.2)
Light.Lighting2D.AddPointLight{x=200, y=100, color={r=1,g=0.8,b=0.5}, range=400}
Light.Graphics.DrawLit(hero, hero_n, 150, 200)
```

---

## `Light.Graphics.DrawLitQuad`

Phase E.1.5 — 绘制受光照的 sprite 子区域（sprite sheet 裁切，对应 `DrawQuad` 的 Lit 版本）

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `image` | `Image|Canvas\|nil` | baseColor 纹理 |
| `normalMap` | `Image\|nil` | 法线贴图（可选） |
| `x, y, z` | `number?` | 屏幕位置 + 深度 |
| `qx, qy, qw, qh` | `number?` | 子区域 (默认 0/0/64/64) |

### 返回值

`void`

---

## `Light.Graphics.Print`

文字渲染 (支持 Unicode/CJK)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `text` | `string` | 要渲染的文本 |
| `font` | `Font` | 字体对象 |
| `x` | `number?` | 水平位置 |
| `y` | `number?` | 垂直位置 |
| `z` | `number?` | 深度 |

### 返回值

`void`

### 示例

```lua
local font = Light(Light.Graphics.Font):New("Arial.ttf", 24)
Light.Graphics.Print("Hello World", font, 10, 10)

还原自 sub_1800AA170
支持 Unicode/CJK: UTF-8 解码 + FontGetGlyph 动态字形查询
两遍策略: 第一遍预烘焙所有字形 (glTexImage2D 必须在 glBegin 外),
          第二遍一次性渲染所有四边形
```

---

## `Light.Graphics.Line`

绘制直线

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `x1` | `number` | 起点 X |
| `y1` | `number` | 起点 Y |
| `z1` | `number` | 起点 Z |
| `x2` | `number` | 终点 X |
| `y2` | `number` | 终点 Y |
| `z2` | `number` | 终点 Z |

### 返回值

`void`

---

## `Light.Graphics.Triangle`

绘制三角形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框(LineMode) 2=填充(FillMode) |
| `x1` | `number` | 顶点 1 X |
| `y1` | `number` | 顶点 1 Y |
| `z1` | `number` | 顶点 1 Z |

### 返回值

`void`

---

## `Light.Graphics.Rectangle`

绘制矩形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 左上 X |
| `y` | `number` | 左上 Y |
| `z` | `number` | 深度 |
| `w` | `number` | 宽度 |
| `h` | `number` | 高度 |
| `d` | `number?` | 深度尺寸 (默认 0) |

### 返回值

`void`

---

## `Light.Graphics.RoundedRectangle`

绘制圆角矩形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 左上 X |
| `y` | `number` | 左上 Y |
| `z` | `number` | 深度 |
| `w` | `number` | 宽度 |
| `h` | `number` | 高度 |
| `r` | `number?` | 圆角半径 (默认 5) |
| `segments` | `number?` | 圆弧段数 (默认 8) |

### 返回值

`void`

---

## `Light.Graphics.Quad`

绘制任意四边形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |

### 返回值

`void`

---

## `Light.Graphics.Polygon`

绘制多边形 (可变顶点数)

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |

### 返回值

`void`

---

## `Light.Graphics.Arc`

绘制圆弧

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 中心 X |
| `y` | `number` | 中心 Y |
| `z` | `number` | 深度 |
| `radius` | `number` | 半径 |
| `startAngle` | `number` | 起始角度 (度) |
| `endAngle` | `number` | 结束角度 (度) |
| `segments` | `number` | 圆弧段数 |

### 返回值

`void`

---

## `Light.Graphics.Circle`

绘制圆形

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `number` | 1=线框 2=填充 |
| `x` | `number` | 中心 X |
| `y` | `number` | 中心 Y |
| `z` | `number` | 深度 |
| `radius` | `number` | 半径 |
| `segments` | `number` | 分段数 |

### 返回值

`void`

---

## `Light.Graphics.DrawSprite`

绘制 WAS 精灵帧

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `spriteData` | `table` | GetSpriteImagesData 返回的精灵数据 |
| `frameIdx` | `number` | 帧索引 (1-based) |
| `x` | `number?` | 屏幕位置 X |
| `y` | `number?` | 屏幕位置 Y |

### 返回值

`void`

### 备注

- 懒加载: 首次绘制时将 pixels 上传为纹理, 缓存在 frame.__texId spriteData.frames[frameIdx] = { x, y, w, h, pixels(userdata) } 懒加载: 首次绘制时将 pixels 上传为 GL 纹理, 缓存在 frame.__texId

---

## `Light.Graphics.Mesh.New`

创建静态 3D mesh。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `vertices` | `table` | 顶点数组（位置 + UV + normal + 可选 tangent，依后端布局） |
| `indices`  | `table` | 三角索引数组（0-based） |

### 返回值

| 状态 | 返回 |
|------|------|
| 成功 | `Mesh` userdata |
| 失败 | `nil, err_string` |

---

## `mesh:Draw([textureOrMaterial], [prevModelMat4])`

绘制 Mesh 到当前 3D 场景。所有参数都是可选的，自动按参数类型分发：

| 调用形式 | 分发到 |
|----------|--------|
| `mesh:Draw()` / `mesh:Draw(nil)` / `mesh:Draw(0)` | 默认贴图路径（`DrawMesh(meshId, 0)`） |
| `mesh:Draw(textureId:int)` | 指定贴图 id（`DrawMesh(meshId, texId)`） |
| `mesh:Draw(material:Material)` | 走 Phase AS 材质路径（`DrawMeshMaterial`） |
| `mesh:Draw(prevModelMat4:table[16])` | 默认贴图路径 + Phase E.13 prev model |
| `mesh:Draw(textureId:int, prevModelMat4:table[16])` | 贴图路径 + Phase E.13 prev model |
| `mesh:Draw(material:Material, prevModelMat4:table[16])` | 材质路径 + Phase E.13 prev model |

### Phase E.13 — `prevModelMat4` 参数

> 启用后参与 motion vector velocity buffer 计算，配合 Temporal SSR 提供更准确的反射时序累积。详见 `docs/Phase E.13 Motion Vector Velocity/FINAL_PhaseE_13.md`。

| 项 | 说明 |
|----|------|
| 类型 | `table` 长度 16，列主序 mat4 |
| 语义 | 该 mesh 在**上一帧**的世界模型矩阵（含 parent chain） |
| 作用 | shader 中计算 `prevClip = uPrevViewProj * prevModel * vec4(localPos, 1)`，velocity = `(curUV - prevUV)` 写入 MRT slot 2 (RG16F) |
| 一次性 | 仅对**紧接着的一次** `mesh:Draw` 生效；下次 draw 不会复用 |
| 缺省行为 | 不传则 `prevModel = curModel`，相当于物体认为自己上一帧没动；shader 仍能输出 velocity（值为 0，纯相机运动） |
| 错误参数 | 非 16 元素 table 抛 Lua 错误 |

### 与 ECS 集成

`Light.ECS` 的 `MeshRenderer` 系统已内置 `_mesh_prev_model_cache`：

- 每个 entity 第一帧无 prev → 走「不传 prevModel」分支
- 第二帧起把上一帧 world matrix 作为 `prevModel` 自动传入 `mesh:Draw`
- `MeshRenderer.visible == false` 或 `mr.mesh == nil` 时清除该 entity 的缓存，避免重新可见时产生伪 velocity

使用 ECS 的脚本无需主动管理 prev model；只需在自定义渲染路径（手写 `mesh:Draw` 调用）需要 velocity buffer 支持时显式传入。

### 示例

```lua
-- 旧调用，保持兼容
mesh:Draw()
mesh:Draw(0)
mesh:Draw(textureId)
mesh:Draw(material)

-- Phase E.13：传入上一帧世界矩阵以参与 velocity buffer
local prevModel = computePrevWorldMatrix(entity)  -- 列主序 mat4
mesh:Draw(prevModel)
mesh:Draw(0,        prevModel)
mesh:Draw(material, prevModel)
```

### 返回值

`void`

---

# `Light.Graphics.HDR` 子表

> Phase E.3 ~ E.14 — HDR 离屏渲染管线 + Tonemap + Velocity buffer 控制

`Light.Graphics.HDR` 是一组 **16 个函数** 的 Lua 子表，控制 HDR RGBA16F 离屏渲染、tonemap 曲线、曝光/伽马、以及 velocity buffer 的 dilation 与存储格式。

## 管线总览

```
BeginFrame
  ↓ HDR.BeginScene() → 绑定 RGBA16F FBO + Clear
  ↓ Lua Draw / DrawLit / Draw3D（所有像素可 > 1.0）
  ↓ Bloom / AutoExposure / LensDirt / LensFlare 后处理（Phase E.4 ~ E.7）
  ↓ HDR.EndScene() → tonemap blit 到 default fb（用 GetTonemapper 选曲线）
  ↓ SwapBuffers
EndFrame
```

- **未 Enable 时**：所有 API 静默 no-op，主循环走 LDR 路径
- **Legacy 后端**：`HDR.IsSupported() == false`，`Enable` 永远返 false
- **GL3.3 后端（默认）**：完整支持

## 子表函数索引

| 分类 | 函数 | Phase |
|------|------|-------|
| 生命周期 | `Enable / Disable / IsEnabled / IsSupported / Resize` | E.3 |
| 后端纹理 | `GetSceneTexture` | E.3 |
| 曝光 / 伽马 | `SetExposure / GetExposure / SetGamma / GetGamma` | E.3 |
| Tonemap operator | `SetTonemapper / GetTonemapper` | E.3.4 |
| Velocity dilation | `SetVelocityDilation / GetVelocityDilation` | E.14 |
| Velocity dilation 半分辨率 | `SetVelocityDilationHalfRes / GetVelocityDilationHalfRes` | E.18.1 |
| Velocity dilation 自动跳过 | `SetVelocityDilationAutoSkip / GetVelocityDilationAutoSkip` | E.18.2 |
| Velocity 格式 | `SetVelocityFormat / GetVelocityFormat` | E.14 |

---

## `HDR.Enable`

启用 HDR：创建 RGBA16F 离屏 RT 与配套 velocity（RG16F/RG8）/depth/normal MRT。`Init` 必须由引擎在主循环启动时已调（`light_ui.cpp` 自动），用户脚本只调 `Enable`。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `w` | `number` | RT 宽度（像素，> 0） |
| `h` | `number` | RT 高度（像素，> 0） |

### 返回值

| 状态 | 返回 |
|------|------|
| 成功 | `boolean true` |
| 失败 | `boolean false`（后端不支持 / 参数非法 / 资源创建失败） |

### 行为

- 允许同进程多次 `Enable`，等价于 `Resize(w, h)`
- 成功后下一帧主循环走 HDR 路径（所有像素可 > 1.0）
- 自动联动：成功后会通知 `Bloom` / `AutoExposure` / `LensDirt` / `LensFlare` 模块（autoEnable=true 时各自拉起）
- `IsSupported() == false` 时直接返 false 并 warn log

### 示例

```lua
if Light.Graphics.HDR.IsSupported() then
    Light.Graphics.HDR.Enable(1280, 720)
end
```

---

## `HDR.Disable`

释放 HDR RT，主循环回退到 LDR 路径。

### 参数

无

### 返回值

`void`

### 行为

- 联动通知 `Bloom::OnHDRDisabled`（其他后处理模块按需联动）
- 已 disabled 时 idempotent

---

## `HDR.IsEnabled`

查询 HDR 是否当前启用（`Enable` 成功 + 未 `Disable`）。

### 返回值

`boolean`

---

## `HDR.IsSupported`

查询当前后端是否支持 HDR。`Init` 未调时返 false。

### 返回值

`boolean`

---

## `HDR.Resize`

调整 HDR RT 尺寸（窗口 resize 时调用方主动调）。内部实现 = `Disable + Enable(w, h)`。未 `Enable` 时等价于 `Enable(w, h)`。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `w` | `number` | 新宽度（> 0） |
| `h` | `number` | 新高度（> 0） |

### 返回值

`boolean`：true = 成功；false = 资源创建失败 / 非法尺寸

---

## `HDR.GetSceneTexture`

获取当前 HDR RT 的颜色纹理 GL id（用于自定义 shader 采样、调试 IMGui blit 等高级用法）。

### 返回值

`number`：GL texture id；未 `Enable` / 后端不支持时返 0

---

## `HDR.SetExposure`

设置线性曝光预乘（默认 1.0）。LDR 模式下写入值但不影响渲染。**注意**：当 `Light.Graphics.AutoExposure` 启用时，渲染会用 AE 计算的 exposure 覆盖此 manual 值；AE 关闭时回归 manual。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `v` | `number` | 曝光预乘（推荐 0.1 ~ 10.0） |

### 返回值

`void`

---

## `HDR.GetExposure`

### 返回值

`number`：当前 manual exposure（不反映 AE 覆盖）

---

## `HDR.SetGamma`

设置 sRGB encode gamma（默认 2.2）。内部 clamp 到 `> 0.0001` 防止除零。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `v` | `number` | gamma（推荐 1.8 ~ 2.4） |

### 返回值

`void`

---

## `HDR.GetGamma`

### 返回值

`number`：当前 gamma

---

## `HDR.SetTonemapper`

切换 tonemap 曲线（Phase E.3.4）。**入参大小写无关**；未知名静默回退 `"aces"`（不报错）。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `name` | `string` | 曲线名：`"aces"` / `"reinhard"` / `"uncharted2"` / `"linear"` |

### 4 种曲线

| 名 | 算法 | 风格 |
|----|------|------|
| `aces` | Narkowicz 2016 fitted | 默认，电影感，对比度高 |
| `reinhard` | `x / (1 + x)` | 简单基线，柔和 |
| `uncharted2` | Hable filmic（含 white scale） | 电影感另一种 |
| `linear` | `clamp(x, 0, 1)` | 调试用，等同 LDR clip |

### 返回值

`void`

### 示例

```lua
Light.Graphics.HDR.SetTonemapper("uncharted2")
print(Light.Graphics.HDR.GetTonemapper())  --> "uncharted2"

Light.Graphics.HDR.SetTonemapper("UnKnOwN")
print(Light.Graphics.HDR.GetTonemapper())  --> "aces"（回退）
```

---

## `HDR.GetTonemapper`

获取当前 tonemap operator 的规范小写名。

### 返回值

`string`：`"aces"` / `"reinhard"` / `"uncharted2"` / `"linear"` 之一

---

## `HDR.SetVelocityDilation`

控制 velocity buffer **3x3 max-length 邻域采样**（dilation）开关。开启可抑制几何边缘的 1 像素错配伪影；关闭可省 8 次 texture fetch / pixel。**默认 ON**。

> **Phase E.18 行为升级（自动透明）**
>
> dilation 算法已从 SSRTemporal / Motion Blur shader 内 inline 9-tap 抽出为 **独立的 HDR EndScene pass**（FBO=`dilatedVelocityFbo`, RG16F）。当后端支持且 dilation=ON 时，HDR EndScene 内会：
>
> 1. 在 SSR / Motion Blur 之前对 raw velocityTex 做一次 9-tap max-length；
> 2. SSR Temporal + Motion Blur consumer 共享 `dilatedTex` 走 **单点采样路径**；
> 3. 多消费者场景下节省约 50% velocity fetch（同一像素由 9 次 → 1 次共享）。
>
> 后端不支持或 dilation pass RT 创建失败时，consumer **自动 fallback 到 raw velocityTex + inline 9-tap**（零回归）。本 API 语义/默认值/Lua 行为完全保持向后兼容。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `on` | `boolean` | true = 开 dilation；false = 单点采样 |

### 返回值

| 状态 | 返回 |
|------|------|
| 成功 | `boolean true` |
| 入参非 boolean | `nil, string err`（多返回值，遵循 Lua 错误约定） |

### 行为

- 不重建 RT，仅修改后端状态
- 下一帧 HDR EndScene 立即生效：
  - dilation=ON + backend 支持 dilation pass → 共享 dilated pass 路径
  - dilation=ON + backend 不支持 → 沿用 inline 9-tap
  - dilation=OFF → 单点采样（最便宜）

### 性能 / VRAM 收益（Phase E.18）

| 场景 | E.17 inline 9-tap | E.18 共享 dilation pass | 节省 |
|------|-------------------|------------------------|------|
| 仅 SSR Temporal | 9 fetch / pixel | 9 (dilate) + 1 (sample) = 10 | ❌ 略负（建议关 dilation pass） |
| 仅 Motion Blur | 9 × N samples (1..32) | 9 + N | 大幅 ✓ |
| SSR + Motion Blur 同开 | 9 × 2 + 9 × N | 9 + 1 + N | **~50%** ✓ |

VRAM 开销：1× `dilatedVelocityTex` (RG16F, full-res) + 可选 1× `dilatedCameraVelocityTex`（与 `cameraVelocityTex` 同条件）。1080p 下约 8MB / RT。

### 示例

```lua
if Light.Graphics.HDR.SetVelocityDilation(false) then
    print("dilation disabled, perf++ (但几何边缘 1px 错配可能可见)")
end

local ok, err = Light.Graphics.HDR.SetVelocityDilation("yes")  -- 类型错
if not ok then print("err: " .. err) end  -- "SetVelocityDilation: expect boolean"
```

---

## `HDR.GetVelocityDilation`

### 返回值

`boolean`：当前 dilation 开关状态（默认 true）

---

## `HDR.SetVelocityDilationHalfRes`

**Phase E.18.1** —— 控制独立 dilation pass 的输出 RT 是否为**半分辨率**。开启后：

- `dilatedVelocityTex` / `dilatedCameraVelocityTex` 尺寸 = `((W+1)/2, (H+1)/2)` 向上取整
- **VRAM 节省 75%**（1080p：每张 RT 由 8MB → 2MB；双 RT 同步 12MB 总省）
- **dilation pass 自身性能 +4×**（fragment count -75%）
- **uTexel = `1/(sw, sh)`**：邻域物理覆盖在 raw velocity space 由 3 raw 像素扩展为 6 raw 像素，max-filter 自动更鲁棒（不会引入伪信号）
- Consumer (SSR Temporal / Motion Blur) 仍单点采样 `dilatedTex`，硬件 `GL_LINEAR + GL_CLAMP_TO_EDGE` 自动 bilinear 上采至 full-res 屏幕坐标
- **默认 OFF**（与 Phase E.18 完全等价行为）

> **生效前提**
>
> 此开关**仅在 dilation pass 启用时有意义**：需要满足 `SetVelocityDilation(true)`（默认 ON）+ backend 支持 dilation pass（`SupportsVelocityDilation()=true`）。若不满足，字段被保存但 dilated RT 未创建，状态对渲染无影响。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `on` | `boolean` | true = 半分辨率；false = 全分辨率（默认）|

### 返回值

| 状态 | 返回 |
|------|------|
| 成功 | `boolean true` |
| 入参非 boolean | `nil, string err`（多返回值）|

### 行为

- 切换时若 HDR 已 Enable → **立即释放并重建** dilated RT（combined + camera-only 双 RT 同步切换），无需调用 Resize
- 切换时若未 Enable → 仅更新 state，下次 Enable 时按新尺寸创建
- 同值切换 = no-op（不重建 RT）

### 性能 / VRAM 收益（Phase E.18.1，1080p）

| 配置 | VRAM (combined+camera) | dilation pass 像素数 | dilation pass 估算 ms |
|------|----------------------|---------------------|----------------------|
| `halfRes=false`（默认）| 16 MB | 2.07 M | 0.10 ms |
| `halfRes=true` | **4 MB** | **0.52 M** | **0.025 ms** |
| **节省** | **-75%** | **-75%** | **-75%（dilation 自身）** |

> Consumer fetch 数不变（仍单点采 dilatedTex），故总管线收益主要在 dilation pass 自身 + VRAM。

### 适用场景

| 场景 | halfRes 推荐 |
|------|------------|
| Mobile / VR | ✅ 始终 ON |
| 4K (2160p) | ✅ ON |
| 1440p QHD | ✅ ON |
| 1080p + 单 consumer（仅 SSR Temporal 或仅 Motion Blur N≤4）| ⚖️ 看视觉容忍度 |
| 1080p + 多 consumer（SSR + MB N=8） | ✅ ON |
| 720p HD | ❌ OFF（VRAM 已不大）|
| 极宽窄物体高速运动场景 | ❌ OFF（half-res 可能漏 1px 物体）|

### 示例

```lua
-- 移动端推荐配置：dilation pass + half-res 双开
Light.Graphics.HDR.Enable(1280, 720)
Light.Graphics.HDR.SetVelocityDilation(true)         -- 默认 ON
Light.Graphics.HDR.SetVelocityDilationHalfRes(true)  -- VRAM -75%

-- 桌面 4K 配置 + 多 consumer
Light.Graphics.HDR.Enable(3840, 2160)
Light.Graphics.HDR.SetVelocityDilationHalfRes(true)  -- 4K dilation pass 受益最大

-- 类型错
local ok, err = Light.Graphics.HDR.SetVelocityDilationHalfRes("yes")
if not ok then print("err: " .. err) end  -- "SetVelocityDilationHalfRes: expect boolean"
```

---

## `HDR.GetVelocityDilationHalfRes`

### 返回值

`boolean`：当前 dilation pass 半分辨率状态（默认 false）

### 示例

```lua
if Light.Graphics.HDR.GetVelocityDilationHalfRes() then
    print("dilation pass running at half-res")
end
```

---

## `HDR.SetVelocityDilationAutoSkip`

**Phase E.18.2** —— 控制 HDR EndScene 是否在**单消费者**场景下自动跳过 dilation pass，让 consumer 走 inline 9-tap 旧路径。**默认 OFF**（与 Phase E.18.1 行为完全等价）。

> **跳过判定规则**
>
> 仅在以下场景跳过：
>
> ```
> autoSkip=true && SSR.IsEnabled() && SSR.GetTemporalEnabled() && !MotionBlur.IsEnabled()
> ```
>
> 即 **"仅 SSR Temporal 启用 + Motion Blur 未启用"** 的单消费者场景。

> **跳过收益分析（fetch / pixel）**
>
> | 场景 | dilation pass | inline 9-tap | autoSkip 决定 |
> |------|---------------|--------------|---------------|
> | 仅 SSR Temporal | 9 dilate + 1 sample = 10 | 9 | **跳过省 1 fetch** ✅ |
> | 仅 Motion Blur(N=8) | 9 + 8 = 17 | 9 × 8 = 72 | 不跳过（dilation 节省 55 fetch）❌ |
> | SSR + Motion Blur(N=8) | 9 + 1 + 8 = 18 | 9 + 9 × 8 = 81 | 不跳过（dilation 节省 63 fetch）❌ |
>
> 故 autoSkip 真正受益场景**仅在单 SSR Temporal**；其他场景无视 autoSkip 状态始终运行 dilation pass。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `on` | `boolean` | true = 自动跳过单 SSR 场景；false = 始终运行 dilation pass（默认）|

### 返回值

| 状态 | 返回 |
|------|------|
| 成功 | `boolean true` |
| 入参非 boolean | `nil, string err`（多返回值）|

### 行为

- **不重建 RT**（与 `SetVelocityDilation`/`SetVelocityDilationHalfRes` 不同）：决策每帧 EndScene 时重新评估
- once-log：active ↔ skip 状态转变时打印一次日志（避免每帧 spam）
- 与 `SetVelocityDilation` 正交：dilation OFF 时本字段无效（dilation pass 整体不运行）
- 与 `SetVelocityDilationHalfRes` 正交：autoSkip 决定本帧是否跳过；halfRes 决定不跳过时 RT 用什么尺寸

### 适用场景

| 场景 | autoSkip 推荐 |
|------|--------------|
| 仅 SSR Temporal（无 Motion Blur）的反射场景（如水面、光滑地板）| ✅ ON（每帧省 1 fetch/px）|
| Motion Blur 必开的赛车 / 动作游戏 | ❌ OFF（autoSkip 不跳过；显式关也行）|
| 多消费者通用场景 | ❌ OFF（autoSkip 不跳过；保持简单状态）|
| 不想引入条件分支调试不便的开发期 | ❌ OFF（默认值，行为可预测）|

### 示例

```lua
-- 仅启用 SSR Temporal 的纯反射 demo / 室内静态场景
Light.Graphics.HDR.Enable(1280, 720)
Light.Graphics.HDR.SetVelocityDilation(true)        -- 默认 ON
Light.Graphics.HDR.SetVelocityDilationAutoSkip(true) -- 单消费者自动跳过省 1 fetch/px

-- 后续如启用 Motion Blur，autoSkip 自动失效（不跳过 dilation pass）
Light.Graphics.MotionBlur.Enable(1280, 720)
-- 此时不论 autoSkip 状态，dilation pass 始终运行（多消费者收益最大）

-- 类型错
local ok, err = Light.Graphics.HDR.SetVelocityDilationAutoSkip(1)
if not ok then print("err: " .. err) end  -- "SetVelocityDilationAutoSkip: expect boolean"
```

---

## `HDR.GetVelocityDilationAutoSkip`

### 返回值

`boolean`：当前 dilation pass 自动跳过状态（默认 false）

### 示例

```lua
if Light.Graphics.HDR.GetVelocityDilationAutoSkip() then
    print("dilation auto-skip enabled (SSR-only single consumer scenarios)")
end
```

---

## `HDR.SetVelocityFormat`

切换 velocity buffer 存储格式（Phase E.14）。RG16F = 默认全精度；RG8 = 节省 4× VRAM（@ 1080p：8MB → 2MB），通过 shader 内 bias/scale 解码（精度 ≈ 2 像素 / 1080p，scale = 0.25 即 ±540px / frame）。**入参大小写敏感**。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `fmt` | `string` | `"rg16f"`（默认）或 `"rg8"`（小写） |

### 返回值

| 状态 | 返回 |
|------|------|
| 切换成功（含 RT 重建） | `boolean true` |
| 重建失败 | `boolean false` |
| 入参非法 / 大小写错 | `nil, string err` |

### 行为

- HDR 已 `Enable` 时：走 `ReleaseRT + CreateRT` 重建路径，**会隐含重置 velocity history**（首帧 velocity 失效一次）
- HDR 未 `Enable` 时：仅更新 state，下次 `Enable` 时按新格式创建
- 无效名（如 `"rg32f"` / `"RG8"`）返 `nil, err`，**不修改 state**

### 适用建议

| 场景 | 推荐 |
|------|------|
| 桌面 / 主机 / 高端移动 | `"rg16f"`（默认） |
| 中低端移动 / VRAM 紧张 / 1080p+ 多 RT 管线 | `"rg8"`（视觉差异 ≤ 2 像素，多数场景不可见） |
| 极端快速运动（> ±540px / frame） | 必须 `"rg16f"`，RG8 会 clamp 饱和 |

### 示例

```lua
-- 移动端切到 RG8 节省 VRAM
local ok = Light.Graphics.HDR.SetVelocityFormat("rg8")
if ok then
    print("velocity format = " .. Light.Graphics.HDR.GetVelocityFormat())  --> "rg8"
end

-- 大小写敏感
local ok2, err = Light.Graphics.HDR.SetVelocityFormat("RG8")
print(ok2, err)
--> nil    SetVelocityFormat: expect 'rg16f' or 'rg8', got 'RG8'
```

---

## `HDR.GetVelocityFormat`

### 返回值

`string`：`"rg16f"`（默认）或 `"rg8"`（规范小写）

---

# `Light.Graphics.MotionBlur` 子表

> Phase E.15 — Velocity-driven Motion Blur（基于 velocity buffer 的相机/物体运动模糊）

`Light.Graphics.MotionBlur` 是 **11 个函数** 的 Lua 子表，控制 per-pixel 速度模糊后处理。复用 Phase E.13 / E.14 velocity buffer（RG16F 或 RG8）；插在 HDR 后处理链 LensFlare 之后、Tonemap 之前。

## 管线总览

```
HDRRenderer::EndScene
  ↓ Bloom / LensDirt / Streak / SSAO / SSR / LensFlare 累积到 sceneTex
  ↓ ★ MotionBlur.Process
    ├ Pass1 (shader): 沿 velocity × strength 多采样 sceneTex → 写 motionBlurTex
    │   - 复用 SSRTemporal 的 DecodeVelocity + SampleVelocityDilated（3x3 max-length）
    │   - 软限 max blur = 屏幕对角线 × 30%（防极端运动糊死画面）
    └ Pass2 (blit): glBlitFramebuffer motionBlurTex → 覆盖 sceneTex
  ↓ DrawTonemapFullscreen → default fb
```

- **未 Enable 时**：所有 API 静默 no-op，HDR 管线仍正常工作
- **依赖**：HDR 必须先 `Enable`（velocity buffer 来自 HDR FBO 的 MRT slot 2）
- **后端**：GL3.3 支持；Legacy / GLES3 后端 `IsSupported() = false` 时所有调用 no-op
- **默认行为**：`autoEnable = false`，HDR.Enable 不会自动拉起 MotionBlur（与 LensDirt/SSAO/SSR 一致）

## 子表函数索引

| 分类 | 函数 |
|------|------|
| 生命周期 | `Enable / Disable / IsEnabled / IsSupported / Resize` |
| HDR 联动 | `SetAutoEnable / GetAutoEnable`（默认 `false`） |
| 强度 | `SetStrength / GetStrength`（默认 `1.0`，clamp `[0, 4]`） |
| 采样数 | `SetSampleCount / GetSampleCount`（默认 `8`，clamp `[1, 32]`） |

---

## `MotionBlur.Enable`

启用 Motion Blur：创建 RGBA16F ping-pong RT（与 HDR sceneTex 同尺寸）。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `w` | `number` | RT 宽度（> 0，建议与 HDR RT 同） |
| `h` | `number` | RT 高度（> 0） |

### 返回值

`boolean`：成功 / 失败（后端不支持 / 参数非法 / FBO 创建失败）

### 行为

- 允许同进程多次 `Enable`，等价于 `Resize`
- 必须先 `Light.Graphics.HDR.Enable(...)`；如 HDR 未启用，本调用仍可成功但 `Process` 因 hdrTex=0 silent skip

### 示例

```lua
Light.Graphics.HDR.Enable(1280, 720)
if Light.Graphics.MotionBlur.IsSupported() then
    Light.Graphics.MotionBlur.Enable(1280, 720)
end
```

---

## `MotionBlur.Disable`

释放 ping-pong RT；下一帧管线跳过 motion blur。

### 返回值

`void`

### 行为

- idempotent：未启用时为 no-op
- HDR.Disable 会**自动**触发本函数（OnHDRDisabled 强制 Disable）

---

## `MotionBlur.IsEnabled`

### 返回值

`boolean`：当前是否启用

---

## `MotionBlur.IsSupported`

### 返回值

`boolean`：后端是否支持 motion blur（shader 编译成功才 true；Legacy/Init 未调时返 false）

---

## `MotionBlur.Resize`

调整 ping-pong RT 尺寸。内部 = `Disable + Enable`。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `w` | `number` | 新宽度（> 0） |
| `h` | `number` | 新高度（> 0） |

### 返回值

`boolean`

---

## `MotionBlur.SetAutoEnable / GetAutoEnable`

控制 HDR.Enable 是否自动拉起 MotionBlur。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `flag` | `boolean` | true = HDR.Enable 时自动 Enable；false = 用户必须显式 Enable |

### 默认值

`false`（与 LensDirt/SSAO/SSR 一致；仅 Bloom 默认 true）

---

## `MotionBlur.SetStrength`

强度（默认 1.0）。1.0 = velocity 位移直接做 blur；> 1.0 加强；0 = 关闭 blur 效果（但仍跑两个 pass）。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `v` | `number` | 强度（clamp 到 `[0, 4]`） |

### 软限

shader 内置 **max blur distance = 屏幕对角线 × 30%**（UV 空间 0.4243）。即便 strength=4 + RG8 饱和也不会糊死画面。

### 示例

```lua
Light.Graphics.MotionBlur.SetStrength(1.5)   -- 加强 50%
print(Light.Graphics.MotionBlur.GetStrength())  --> 1.5

Light.Graphics.MotionBlur.SetStrength(99)    -- clamp
print(Light.Graphics.MotionBlur.GetStrength())  --> 4.0
```

---

## `MotionBlur.GetStrength`

### 返回值

`number`：当前强度

---

## `MotionBlur.SetSampleCount`

沿 velocity 方向的采样数（默认 8）。高质量 16~32，性能优先 4~8。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `n` | `integer` | 采样数（clamp 到 `[1, 32]`） |

### 性能预算（1080p）

| 采样数 | Pass1 估算 | 适用场景 |
|--------|-----------|----------|
| 4 | ~0.25 ms | 移动端 / 性能优先 |
| 8 | ~0.5 ms | 桌面默认 |
| 16 | ~1.0 ms | 高质量 |
| 32 | ~2.0 ms | 极致质量 / 慢速运动 |

### 示例

```lua
Light.Graphics.MotionBlur.SetSampleCount(16)
print(Light.Graphics.MotionBlur.GetSampleCount())  --> 16

Light.Graphics.MotionBlur.SetSampleCount(0)        -- clamp
print(Light.Graphics.MotionBlur.GetSampleCount())  --> 1
```

---

## `MotionBlur.GetSampleCount`

### 返回值

`number`（整数）：当前采样数

---

## `MotionBlur.SetMode`

**Phase E.16** — Motion blur 模式选择。三档供取舍：拖尾跟随相机/物体/合一。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `m` | `integer` | 模式编号（clamp 到 `[0, 2]`） |

### 模式映射

| 值 | 名称 | 数学公式 | 适用场景 |
|----|------|---------|----------|
| `0` | `combined`（默认） | `v = curUV − prevUV`（camera + object 合一） | 通用场景，与 Phase E.15 完全一致 |
| `1` | `camera_only` | `v = curUV − (prevVP × curModel × pos).xy/w` | 第一人称 / 赛车：相机晃动时拖尾，物体在屏幕静止时不拖 |
| `2` | `object_only` | `v ≈ v_combined − v_camera` | 物体快速运动（爆炸碎片、武器挥舞）拖尾，相机平稳时使用 |

### 设计

- 3D shader VS 同时输出 `vPrevClip` (combined) 和 `vPrevClipCameraOnly` (camera-only)，FS 写入 HDR FBO MRT slot 2/3
- VRAM 增量：1080p 下 +1 MB（RG8）/ +4 MB（RG16F），跟随 `HDR.SetVelocityFormat`
- mode=1/2 但 `cameraVelocityTex` 缺失（旧 backend）→ silent fallback 到 mode=0
- 不影响 SSR Temporal（reproject 数学要求 combined velocity）

### 示例

```lua
Light.Graphics.MotionBlur.SetMode(1)  -- camera_only：仅相机运动拖尾
print(Light.Graphics.MotionBlur.GetMode())  --> 1

Light.Graphics.MotionBlur.SetMode(99)  -- clamp
print(Light.Graphics.MotionBlur.GetMode())  --> 2
```

---

## `MotionBlur.GetMode`

### 返回值

`number`（整数）：当前模式编号（0/1/2）

---

## `MotionBlur.SetHalfRes`

**Phase E.17** — Half-res motion blur 开关。`motionBlurTex` 改为 `((w+1)/2, (h+1)/2)`，Pass2 用 `GL_LINEAR` 硬件 bilinear 上采样回原分辨率。

### 参数

| 名称 | 类型 | 说明 |
|------|------|------|
| `flag` | `boolean` | true = 启用 half-res；false = full-res（默认） |

### 性能与 VRAM 收益（@ 1080p RGBA16F）

| 项 | full-res（默认） | half-res（Phase E.17） | 收益 |
|----|----------------|------------------------|------|
| Pass1 fragment 数 | 2,073,600 | 518,400 | **−75%** |
| Pass1 时间（sampleCount=8） | ~0.50 ms | ~0.13 ms | **−74%** |
| Pass2 blit | ~0.05 ms (NEAREST) | ~0.07 ms (LINEAR) | +0.02 ms |
| **合计** | ~0.55 ms | **~0.20 ms** | **−64%** |
| `motionBlurTex` VRAM | 8 MB | **2 MB** | **−75%** |

### 设计

- **velocity buffer 保持全分辨率**：shader 9-tap dilation 物理覆盖范围正确，精度无损
- **shader 零改动**：uTexel 仍按全分辨率 `1/vec2(w, h)` 上传，dilation 邻域物理覆盖一致
- **bilinear 上采样自动低通**：行业惯例不补偿 strength/sampleCount，依赖低分辨率本身的低通效应自然平滑视觉
- **切换时机**：已 Enable 时立即 Resize 重建 RT；未 Enable 时下次 Enable 生效
- **零回归**：默认 false → 与 Phase E.15/E.16 完全一致；Pass2 自动 fallback `GL_NEAREST`

### 使用建议

| 场景 | 推荐 |
|------|------|
| 移动端 / VR | ✅ 始终 ON |
| 高分屏（≥ 1440p） | ✅ ON（视觉损失肉眼几乎不可见） |
| 桌面 1080p | ⚖️ 性能优先 ON / 极致质量 OFF |
| 720p 以下 | ❌ OFF（half-res 360p 视觉劣化明显） |

### 示例

```lua
Light.Graphics.MotionBlur.SetHalfRes(true)         -- 启用
print(Light.Graphics.MotionBlur.GetHalfRes())      --> true
```

---

## `MotionBlur.GetHalfRes`

### 返回值

`boolean`：当前 half-res 开关状态

---

# Phase F.0 — TAA 主管线 (`Light.Graphics.TAA.*`)

> **TAA (Temporal Anti-Aliasing) 主管线**：sub-pixel jittered projection + history 累积 + neighborhood AABB clip + alpha blend，对**整个 HDR scene** 做时序超采样与抗锯齿。
>
> **管线位置**：`SSAO → dilation → SSR → LensFlare → MotionBlur → TAA → Tonemap`。TAA 是 HDR linear 空间最后一个 pass，输出后直接 Tonemap。
>
> **Backend 双 projection 架构**：
>   - `GetProjection()` 始终返 unjittered（SSR/SSAO/velocity 零改动）
>   - `LoadJitteredProjection()` 由 `ApplyJitter()` 每帧 BeginScene 后注入
>   - vertex shader 内 `vCurClip = uCurViewProj * (uModel * pos)`（用 unjittered 算 velocity，避免 jitter 污染）
>
> **复用 Phase E 资产**：Halton-2,3 8-sample（与 SSR Temporal 共表）+ HDR FBO velocity buffer (E.13/E.14) + dilated velocity (E.18)
>
> **与 SSR Temporal 关系**：完全共存（用户自负责）。同开时反射会被 temporal 两次（略过 blur），推荐启用 TAA 时手动 `Light.Graphics.SSR.SetTemporalEnabled(false)`。
>
> **默认 OFF**：用户主动 `Enable` 才生效（与 Phase E 所有模块一致），零回归保障。

## TAA API 速查表（共 17 函数 = F.0 13 + F.0.1 2 + F.0.4 2）

| 类别 | 函数 | 默认值 / 说明 |
|------|------|--------------|
| lifecycle | `TAA.Enable(w, h)` / `Disable()` / `IsEnabled()` / `IsSupported()` / `Resize(w, h)` | 创建 RGBA16F × 2 history ping-pong RT |
| 参数 | `SetBlendAlpha(a)` / `GetBlendAlpha()` | history 权重，clamp `[0, 1]`，**默认 0.92** |
| 参数 | `SetNeighborhoodClip(on)` / `GetNeighborhoodClip()` | 9-tap AABB clip，**默认 true** |
| 参数 | `SetJitterEnabled(on)` / `GetJitterEnabled()` | sub-pixel projection jitter，**默认 true** |
| 参数 (F.0.1) | `SetSharpness(s)` / `GetSharpness()` | 4-tap unsharp mask，clamp `[0, 2]`，**默认 0.5**（0=blit fallback） |
| 参数 (F.0.4) | `SetAntiFlicker(on)` / `GetAntiFlicker()` | Karis luma weighting blend，**默认 true**（false=F.0 纯 alpha blend） |
| 状态 | `GetFrameCounter()` | `int` Halton 索引 `[0, 7]`（debug HUD） |
| 状态 | `GetCurrentJitter()` | `(jx, jy)` 本帧 sub-pixel 偏移（±0.5 px） |

---

## `TAA.Enable`

启用 TAA：创建 RGBA16F × 2 history ping-pong RT（与 sceneTex 同尺寸）。

### 参数

- `w` (`integer`)：宽度，需 > 0
- `h` (`integer`)：高度，需 > 0

### 返回值

`boolean`：`true` = 成功；`false` = backend 不支持 / 参数非法 / RT 创建失败

### 示例

```lua
local TAA = Light.Graphics.TAA
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    TAA.SetBlendAlpha(0.92)
    TAA.SetNeighborhoodClip(true)
    TAA.SetJitterEnabled(true)
end
```

---

## `TAA.Disable`

释放 history RT；下一帧管线跳过 TAA。Disable 内部自动 `ClearJitteredProjection()` 复位 backend jitter 状态。

---

## `TAA.IsEnabled` / `TAA.IsSupported`

### 返回值

- `IsEnabled()` → `boolean`：当前是否已 `Enable`
- `IsSupported()` → `boolean`：backend 是否支持（GL33 = TAA shader 编译成功 + RGBA16F 可用）

---

## `TAA.Resize`

调整 history RT 尺寸；内部 = `Disable + Enable`。等价于 `Enable(w, h)`（重建 RT）。

### 参数

- `w` (`integer`) / `h` (`integer`)：与 `Enable` 同

### 返回值

`boolean`：`true` 成功

---

## `TAA.SetBlendAlpha` / `TAA.GetBlendAlpha`

history 权重 `α`：`result = mix(cur, history, α)`。

### 参数

- `a` (`number`)：clamp `[0, 1]`

### 默认值

`0.92`（略高于 SSR Temporal 的 0.9，主管线累积更稳）

### 取值建议

| α 值 | 视觉效果 | 适用场景 |
|------|----------|---------|
| 0.85 | 响应快、抖动可见 | 高速运动镜头 |
| 0.92 | 平衡（推荐） | 通用 |
| 0.95 | 累积稳、响应慢 | 静态/慢动作场景 |
| 0.99 | 几乎不更新 | debug 用 |

### 示例

```lua
Light.Graphics.TAA.SetBlendAlpha(0.95)        -- 慢动作高质量
print(Light.Graphics.TAA.GetBlendAlpha())     --> 0.95
Light.Graphics.TAA.SetBlendAlpha(2.0)         -- clamp
print(Light.Graphics.TAA.GetBlendAlpha())     --> 1.0
```

---

## `TAA.SetNeighborhoodClip` / `TAA.GetNeighborhoodClip`

是否启用 9-tap AABB clip：用本帧 3×3 邻域颜色 min/max 包围盒裁剪 history，防止 ghosting / disocclusion。

### 参数

- `on` (`boolean`)：必须为 boolean，**严格类型检查**

### 默认值

`true`（业界共识：必开 clip）

### 关闭后果

纯 reproject + blend → 高速运动会出现拖尾/重影；仅 debug 用。

### 错误处理

非 boolean 返 `nil + err`：

```lua
local ok, err = Light.Graphics.TAA.SetNeighborhoodClip("yes")
print(ok, err)  --> nil  TAA.SetNeighborhoodClip: 期望 boolean 参数
```

---

## `TAA.SetJitterEnabled` / `TAA.GetJitterEnabled`

sub-pixel projection jitter 开关。关闭后 TAA 退化为**纯时序 stability filter**（无 super-sampling 效果）。

### 参数

- `on` (`boolean`)：必须为 boolean

### 默认值

`true`（含 super-sampling）

### 工作原理

启用时每帧 BeginScene 后注入 Halton-2,3 sub-pixel 偏移到 backend `LoadJitteredProjection()`：
- raster 路径（`gl_Position`）用 jittered projection（NDC 偏 ±0.5 px）
- velocity 计算（`vCurClip`）用 unjittered `uCurViewProj`（保持 reproject 准确）
- `GetProjection()` 仍返 unjittered（SSR/SSAO 零改动）

### 应用场景

| jitter | 效果 | 推荐场景 |
|--------|------|---------|
| ON | 抗锯齿 + super-sampling 效果 | 通用静态/中速场景 |
| OFF | 仅 history 累积平滑 | 极慢运动或 disocclusion 严重场景 |

---

## `TAA.SetSharpness` / `TAA.GetSharpness`

**Phase F.0.1** — TAA 后 4-tap unsharp mask 锐化补偿，弥补 sub-pixel 累积带来的高频损失。

### 参数

- `s` (`number`)：clamp `[0, 2]`

### 默认值

`0.5`（中等强度，视觉差异可感知；UE5 保守值约 0.3）

### 算法

`sharpened = max(0, c + (c - avg4) × s)`，其中 `avg4` 为上下左右 4 邻域均值。

### 取值建议

| sharpness 值 | 视觉效果 | 适用场景 |
|--------------|----------|---------|
| 0.0 | 关闭锐化 (走纯 blit, 零 ALU) | 性能敏感 / 完全软场景 |
| 0.3 | 轻微补偿 (UE5 保守值) | 自然画面 / 慢动作 |
| 0.5 | 中等锐化（推荐） | 通用 |
| 0.8 | 明显锐化 | 高细节场景 / cyber-punk 风格 |
| > 1.5 | 易产生 ringing 伪影 / firefly 加剧 | 仅 debug |

### 性能优化

`sharpness = 0` 时引擎走 `BlitTAAToHDR` 原路径（与 Phase F.0 一致），零 ALU 开销；`> 0` 时启用 `DrawTAASharpenPass` (~0.03 ms @ 1080p)。

### 示例

```lua
local TAA = Light.Graphics.TAA
TAA.SetSharpness(0.5)                     -- 默认推荐
print(TAA.GetSharpness())                 --> 0.5

TAA.SetSharpness(3.0)                     -- clamp
print(TAA.GetSharpness())                 --> 2.0

TAA.SetSharpness(0)                       -- 关闭锐化 (走纯 blit)
```

---

## `TAA.SetAntiFlicker` / `TAA.GetAntiFlicker`

**Phase F.0.4** — Anti-flicker filter (Karis luma-weighted blend)，压制 firefly 在时序累积中的闪烁伪影。

### 参数

- `on` (`boolean`)：`true` 启用 Karis weighting / `false` 走 F.0 纯 alpha blend

### 默认值

`true`（与 F.0.1 sharpening 配合使用收益更佳）

### 错误处理

非 boolean 返 `nil + err`（与 `SetNeighborhoodClip` / `SetJitterEnabled` 同模式）：

```lua
local ok, err = Light.Graphics.TAA.SetAntiFlicker("yes")
print(ok, err)  --> nil  TAA.SetAntiFlicker: 期望 boolean 参数
```

### 算法

参考 Brian Karis (UE4 2014 SIGGRAPH "High Quality Temporal Supersampling")：

```
lumaCur  = dot(cur,  Rec709)
lumaHist = dot(hist, Rec709)
wCur  = 1 / (1 + lumaCur)
wHist = 1 / (1 + lumaHist)
result = (cur * wCur * (1-α) + hist * wHist * α) / (wCur*(1-α) + wHist*α)
```

高 luma 像素被赋予较低权重，使得 firefly 在时序累积中被"稀释"而非"放大"。

### 作用范围

| luma_history | Karis 影响 | 说明 |
|--------------|------------|------|
| `0` (黑色) | 零 | wCur=wHist=1，退化为 `mix(cur, hist, α)`、与 F.0 同结果 |
| `0.5` (中等亮) | 微弱 | < 1/256 LDR 灯零级的影响 |
| `10` (HDR 高光) | 中等 | wHist≈0.091，稍压低 history |
| `100` (极端 firefly) | 强烈 | wHist≈0.01， history 被有效压制 |

**零视觉副作用**：对低/中等 luma 几乎不改变行为，仅对极端高 luma firefly 起抑制作用。

### 性能

+0.01 ms @ 1080p（2 dot + 4 div + 1 div / px）。`uAntiFlicker=0` 走 shader 内 if 分支跳过 luma 计算，零 ALU 回退。

### 推荐配合

与 Phase F.0.1 sharpening 配合使用收益最佳，特别是 `sharpness > 1.0` 时 sharpening 加剧的 firefly 会被 Karis weighting 抑制。

### 示例

```lua
local TAA = Light.Graphics.TAA

-- 默认：anti-flicker on + sharpness 0.5。
TAA.SetAntiFlicker(true)
TAA.SetSharpness(0.5)

-- 调高锐化，Karis weighting 负责压住可能被放大的 firefly
TAA.SetSharpness(1.2)
TAA.SetAntiFlicker(true)

-- 需要严格复现 F.0 原始行为 (走纯 alpha blend)
TAA.SetAntiFlicker(false)
```

---

## `TAA.GetFrameCounter`

### 返回值

`integer`：当前帧 Halton 索引 `[0, 7]`（用于 debug HUD）

---

## `TAA.GetCurrentJitter`

### 返回值

`(number, number)`：本帧 sub-pixel jitter offset（±0.5 像素范围；jitter 关时返 `(0, 0)`）

### 示例

```lua
local TAA = Light.Graphics.TAA
local jx, jy = TAA.GetCurrentJitter()
print(string.format("frame=%d jx=%.4f jy=%.4f", TAA.GetFrameCounter(), jx, jy))
-- frame=3 jx=-0.2500 jy=0.1111  (Halton index 3)
```

---

## TAA 完整用法示例

```lua
local Gfx = require 'Light.Graphics'
local HDR = Gfx.HDR
local SSR = Gfx.SSR
local TAA = Gfx.TAA

-- 1. HDR 必须先启用 (TAA 依赖 HDR sceneTex + velocity buffer)
HDR.Enable(1280, 720)

-- 2. 推荐：启用 TAA 时关 SSR Temporal (避免双 temporal 模糊反射)
SSR.Enable(1280, 720)
SSR.SetTemporalEnabled(false)

-- 3. 启用 TAA 主管线
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    TAA.SetBlendAlpha(0.92)         -- 通用平衡值
    TAA.SetNeighborhoodClip(true)   -- 防 ghosting (默认即开)
    TAA.SetJitterEnabled(true)      -- super-sampling 必开
end

-- 4. 主循环 (Window:__call 内自动 BeginScene + ApplyJitter)
function App:Draw()
    -- 用户 3D 渲染代码; jitter 由引擎透明注入
    Gfx.SetPerspective(60, 16/9, 0.1, 100)
    Gfx.LoadView(viewMat)
    -- ... DrawMesh / DrawLit ... (raster 自动用 jittered projection)
end

-- 5. 反向清理
TAA.Disable()
SSR.Disable()
HDR.Disable()
```

---

## TAA 性能预算

| 项 | 值 (1080p) |
|----|------------|
| TAA pass GPU 时长 | ~0.10 ms (12 fetch + 1 write/px) |
| history RT VRAM | 2 × 1920 × 1080 × 8 byte = **16 MB** |
| 4K 场景 history VRAM | 64 MB（移动端注意） |
| jitter 开销 | ~0.001 ms (CPU side, NDC 矩阵改 2 元素) |

---

## 完整用法示例

```lua
local Gfx = require 'Light.Graphics'
local HDR = Gfx.HDR
local MB  = Gfx.MotionBlur

-- 1. HDR 必须先启用
HDR.Enable(1280, 720)

-- 2. 启用 motion blur
if MB.IsSupported() then
    MB.Enable(1280, 720)
    MB.SetStrength(1.5)
    MB.SetSampleCount(16)
    MB.SetMode(1)               -- Phase E.16: camera-only motion blur
    MB.SetHalfRes(true)         -- Phase E.17: half-res 优化（移动端 / 高分屏推荐）
end

-- 3. 主循环：Draw 3D 场景（mesh:Draw 传 prevModel 才会写 velocity buffer）
-- ...

-- 4. 关闭（顺序反向）
MB.Disable()
HDR.Disable()
```

---
