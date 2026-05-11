# ALIGNMENT — Phase E 渲染管线升级

> 6A 工作流 · 阶段 1 · 对齐（Align）
> 目标：把"渲染管线升级（灯光/阴影/后处理）"模糊主题，对齐为可执行规范。

---

## 1. 原始需求

用户：在已交付的 ECS 渲染 D 系列（D.x.6 SpriteBatch 性能优化收尾）之上，进一步升级渲染管线，方向包括「灯光 / 阴影 / 后处理」。

经主题细化讨论后，本轮 Phase E 确认范围：

- **E.1 — 2D 灯光系统**
- **E.2 — 后处理栈（Post-Process Pipeline）**
- E.3 阴影 mapping 推迟到下一轮 Phase E.x（3D 专属，独立交付）

---

## 2. 项目特性规范（现有渲染能力盘点）

### 2.1 渲染后端

| 组件 | 位置 | 状态 |
|------|------|------|
| 抽象接口 | `ChocoLight/include/render_backend.h` | ✅ 完整 |
| GL 3.3 Core 后端 | `ChocoLight/src/render_gl33.cpp` (2127 行) | ✅ 完整 |
| GL Legacy 回退 | `ChocoLight/src/render_legacy.cpp` | ✅ 兼容 |
| 平台抽象 | `ChocoLight/include/platform_window.h` | ✅ SDL3 桥接 |
| 后端选择 | `CreateRenderBackend()` 工厂 | ✅ 自动 GLES3/GL33/Legacy |

### 2.2 2D 渲染

| 能力 | API | 状态 |
|------|-----|------|
| BatchRenderer (Phase A4-A7) | `BatchRenderer::Submit*` | ✅ |
| Sprite/Quad/Triangle/Line | `Light.Graphics.{Draw,DrawQuad,Print,Rectangle,Line}` | ✅ |
| 字体渲染 + UTF-8 | `Light.Graphics.Font / Print` | ✅ |
| FBO Canvas | `Light.Graphics.Canvas` + Push/Pop | ✅ |
| 用户 Shader | `Light.Graphics.Shader` (10+ uniform setter) | ✅ |
| 顶点格式 | `RenderVertex{ pos, uv, color }` 9 floats | ⚠ **无 normal/tangent** |

### 2.3 3D 渲染（Phase AS.2 - AX）

| 能力 | 状态 |
|------|------|
| Mesh 创建/绘制 + 透视投影 + 深度测试 | ✅ |
| **Unlit + PBR 双 shader** | ✅ |
| **方向光 1 个** (`SetDirectionalLight`) | ✅ |
| **点光 ≤ 4 个**  (`AddPointLight`，硬上限) | ✅ 但太少 |
| **环境光** (`SetAmbientLight`) | ✅ |
| PBR 材质：BaseColor / MetallicRoughness / Normal / Emissive / Occlusion | ✅ |
| GPU Skinning (64 joints UBO) | ✅ Phase AW |
| GPU Morph Target (8 targets) | ✅ Phase AX |
| SpotLight | ❌ 缺失 |
| Shadow Map | ❌ 缺失 |

### 2.4 ECS 渲染层（Phase D 系列）

入口：`ECSWorld:Render()` — 自动跑「2D camera → sprite → SpriteBatch → TextRenderer → 3D camera → mesh」六阶段。

| Component | 字段 | 状态 |
|-----------|------|------|
| `Transform2D / 3D` (+ parent chain) | x/y/z/rot/scale/parent | ✅ |
| `Sprite` | image/color/visible/anchor/flipX/Y/quad | ✅ |
| `SpriteBatch` (D.x.6) | image/quads[]/color/visible | ✅ |
| `TextRenderer` | text/font/color/visible | ✅ |
| `Camera2D` | active/zoom/viewportW/H | ✅ |
| `MeshRenderer` / `SkinnedMeshRenderer` / `AnimationState` | mesh/material/state | ✅ |
| `Camera3D` | active/fovY/aspect/near/far/target/up | ✅ |
| **`Light2D`** | 颜色/范围/类型 | ❌ Phase E.1 待加 |
| **`PostProcess`**（绑 Camera） | passes[] | ❌ Phase E.2 待加 |

性能优化：camera cache, sprite list cache, frustum cull, parent-aware AABB（D.x.5.3）。

### 2.5 后处理基础设施

| 基础块 | 状态 | 缺口 |
|--------|------|------|
| FBO 创建/绑定/解绑 | ✅ `CreateFBO/BindFBO/UnbindFBO` | — |
| Canvas + Push/Pop 栈 | ✅ | — |
| 用户 Shader 编译/激活 | ✅ | — |
| `GenerateMipmap` | ✅ | — |
| sampler2D uniform | ✅ `SetUniformSampler` | — |
| **HDR (RGBA16F) FBO** | ❌ | 仅支持 8-bit color 附着 |
| **MRT (Multiple Render Targets)** | ❌ | 单一颜色附着 |
| **PostProcess Pass 管理器** | ❌ | 待 Phase E.2 引入 |
| **内置 PP effect**（Bloom/Blur/Tonemap/Vignette/ColorGrade） | ❌ | 待 Phase E.2 引入 |

---

## 3. 边界确认

### 3.1 本轮包含（IN-SCOPE）

**E.1 — 2D 灯光系统**

- 新增 C++ 层：`Light.Lighting2D` 模块（管理 2D 灯光、ambient、上传 uniforms）
- 新增 2D Lit Shader：扩展 `BatchRenderer` 的 fragment shader 支持 forward lighting
- 新增 ECS Component：`Light2D`（type=point/spot/ambient，绑定 Transform2D）
- 新增 ECS System：`Light2DSystem`（每帧收集 active Light2D，上传 uniforms）
- 内置示例：`samples/demo_2d_lighting/`

**E.2 — 后处理栈**

- 新增 C++ 层：`Light.PostProcess` 模块
  - `Pass` 资源管理（shader + uniform 参数 + 输入/输出 Canvas）
  - `Pipeline` 链式 Apply（自动 ping-pong RT）
  - 内置 5 个 effect：`Bloom / Blur / Tonemap / Vignette / ColorGrade`
  - 用户自定义 effect API：`PostProcess.AddCustomPass(name, shader, paramsTable)`
- 主 RT 概念引入：可选启用 "MainRenderTarget"（HDR or LDR FBO，自动 blit 到 default framebuffer）
- ECS 集成：Camera2D / Camera3D 加 `postProcess` 字段（pipeline 引用）
- 内置示例：`samples/demo_post_process/`

### 3.2 本轮不包含（OUT-SCOPE）

- ❌ **3D Shadow Mapping**：单独留给 Phase E.3
- ❌ **Deferred Rendering**：当前 forward 路径足够
- ❌ **Cascaded Shadow Map (CSM)** / **Volumetric Lighting** / **Screen-Space Reflections (SSR)**
- ❌ **更多 PBR 光**（>4 PointLight）：3D 部分本轮不动
- ❌ **Indirect Lighting / GI**
- ❌ **MSAA / FXAA / TAA**（可作为后续 PP pass 扩展）

### 3.3 兼容性约束

- 不破坏现有 2D / 3D / Canvas / Shader API
- BatchRenderer 现有合批行为保留；2D 灯光走**新增** Lit shader 路径，沿用 fallback
- 不强制启用 HDR；HDR FBO 是 opt-in（默认 LDR 兼容现有 demo）
- GLES3 / iOS / Android / Web 全平台兼容（必须的 shader feature 都在 GLES 3.0+ 范围内）

---

## 4. 需求理解（技术方案概要）

### 4.1 E.1 — 2D 灯光系统

#### 4.1.1 Shader 扩展

现有 2D shader（`render_gl33.cpp` 的 `VS_SOURCE/FS_SOURCE`）输出：

```
FragColor = uUseTexture ? vColor * texture(uTexture, vTexCoord) : vColor;
```

新增一个 **Lit 2D shader 路径**（`FS_LIT_2D_SOURCE`），支持：

```glsl
// 多光叠加 (forward)
vec3 lightSum = uAmbient2D;
for (i = 0; i < uLightCount; i++) {
  float att = compute_attenuation(uLightPos[i], vWorldPos, uLightRange[i], uLightType[i], uLightDir[i], uLightInner[i], uLightOuter[i]);
  lightSum += uLightColor[i] * uLightIntensity[i] * att;
}
FragColor = baseColor * vec4(lightSum, 1.0);
```

**关键设计**：

- 2D Light 仅作用于**启用 Lit shader 的 sprite**（默认 sprite 仍走原 batch 路径，向后兼容）
- 触发条件：`Light.Graphics.SetLightingEnabled(true)` 或 ECS `LitSprite` component
- 灯光上限：**16 个 PointLight / SpotLight 同时启用**（uniform array, 业界常见）
- 顶点 worldPos：BatchRenderer 提交时通过 modelview 矩阵推算（VS 中 `vWorldPos = (uModel * aPos).xy`）

#### 4.1.2 Lua API

```lua
-- 全局开关
Light.Lighting2D.SetEnabled(true)
Light.Lighting2D.SetAmbient(0.2, 0.2, 0.3)           -- 环境光颜色

-- 灯光管理
local lid = Light.Lighting2D.AddPointLight({
    x=100, y=200, color={1, 0.9, 0.7}, range=300, intensity=1.5
})
local lid = Light.Lighting2D.AddSpotLight({
    x=100, y=200, dirX=1, dirY=0, color={1,1,1},
    range=400, innerAngle=20, outerAngle=35, intensity=2
})
Light.Lighting2D.UpdateLight(lid, {x=150, y=250})    -- 部分字段更新
Light.Lighting2D.RemoveLight(lid)
Light.Lighting2D.ClearLights()

-- 诊断
Light.Lighting2D.GetLightCount()
Light.Lighting2D.GetMaxLights()       -- 返回 16
```

#### 4.1.3 ECS 集成

新增 component：

```lua
{name='Light2D', defaults={
    type='point',                    -- 'point' | 'spot' | 'ambient'
    color={r=1, g=1, b=1},
    range=200, intensity=1,
    -- spot only:
    dirX=1, dirY=0, innerAngle=20, outerAngle=35,
    enabled=true,
}}
```

ECS Render 入口在 2D 阶段开始前调用 `_UploadLights()`：扫描所有 active Light2D entity，把 Transform2D 转 worldPos，上传到 shader uniforms。

### 4.2 E.2 — 后处理栈

#### 4.2.1 架构

```
┌──────────────────────┐                            ┌─────────────────┐
│ Window.Draw() Scene  │ ──► MainRT (FBO RGBA16F) ──►   PostProcess    │ ──► Default FB
└──────────────────────┘                            │   Pipeline       │
                                                    │  ┌──────────┐    │
                                                    │  │ Pass 1   │    │
                                                    │  │ ping ──► │ pong
                                                    │  └──────────┘    │
                                                    │  ┌──────────┐    │
                                                    │  │ Pass 2   │    │
                                                    │  │ pong ──► │ ping
                                                    │  └──────────┘    │
                                                    │   ...            │
                                                    │  ┌──────────┐    │
                                                    │  │ Pass N   │    │
                                                    │  │ ping ──► │ default FB
                                                    │  └──────────┘    │
                                                    └─────────────────┘
```

#### 4.2.2 内置 effect

| Effect | 用途 | 参数 |
|--------|------|------|
| **Tonemap** | HDR → LDR | mode=Reinhard/ACES/Filmic, exposure |
| **Bloom** | HDR 高光辉光 | threshold, intensity, blurPasses |
| **Blur** | 高斯模糊 | radius, direction |
| **Vignette** | 暗角 | radius, softness, color |
| **ColorGrade** | 色彩分级 | contrast, saturation, hue, lift/gamma/gain |

#### 4.2.3 Lua API

```lua
-- 创建 Pipeline
local pp = Light(Light.Graphics.PostProcess):New()
pp:AddBloom({threshold=1.0, intensity=0.5})
pp:AddTonemap({mode='ACES', exposure=1.0})
pp:AddVignette({radius=0.7, softness=0.4})
pp:AddCustom('myEffect', shaderInstance, {uTime=0, uIntensity=0.5})  -- 用户自定义

-- 启用主 RT
Light.Graphics.SetMainRT({hdr=true, width=1024, height=768})

-- 帧渲染
function Game:Draw()
    Light.Graphics.BeginScene()    -- 隐式绑定 MainRT
    -- ... 正常绘制
    Light.Graphics.EndScene()      -- 解绑 + 跑 pp Pipeline
    pp:Apply()                     -- 把 MainRT 经 pipeline 处理后输出到 default FB
end

-- ECS 集成（PostProcess 绑 Camera2D/3D）
camera2D.postProcess = pp
```

---

## 5. 疑问澄清（待用户决策）

以下决策点会影响 Phase E 的范围和工作量。我已对每项写出**默认推荐**和**取舍说明**。请在下一步用户决策环节回答。

### Q1 — SpotLight 是否本轮包含？

- **推荐**：包含（视觉冲击大，工程成本约 +20%）
- **取舍**：不包含则 Light2D type 只有 point/ambient，shader 简化

### Q2 — Normal Map 是否本轮包含？

- **推荐**：**不包含**（需要 sprite 提供 normal texture，要扩展 RenderVertex 加 tangent，BatchRenderer 改造较大）
- **取舍**：可作为 Phase E.1.b 子阶段后续追加。本轮专注 forward multi-light 即可显著提升 2D 质感

### Q3 — 后处理 effect 范围？

- **推荐 A（核心 3 个）**：Tonemap + Bloom + Vignette（覆盖 80% 用途，先快速上线）
- **推荐 B（全 5 个）**：再加 Blur + ColorGrade（完整可商用）
- 用户自定义 PP shader API 两个方案都包含

### Q4 — HDR Framebuffer 默认启用？

- **推荐**：**Opt-in**（`SetMainRT({hdr=true})` 才启用 RGBA16F；默认仍是 RGBA8 LDR）
- **取舍**：默认启用会破坏现有 demo 颜色精度预期，opt-in 更安全
- **影响**：决定 Tonemap 是否必装（HDR 必须配 Tonemap，否则颜色溢出）

---

## 6. 验收标准（高层）

待 CONSENSUS 阶段细化为可测试的具体标准。当前框架：

- ✅ E.1：`samples/demo_2d_lighting/` 跑通，sprite 在多光照明下颜色叠加正确
- ✅ E.1：`scripts/smoke/lighting2d.lua` 全平台通过（Win/Linux/Mac/Android/iOS/Web）
- ✅ E.1：ECS Light2D + Transform2D parent chain 联动正确
- ✅ E.2：`samples/demo_post_process/` 跑通 Bloom + Tonemap + Vignette 链
- ✅ E.2：`scripts/smoke/postprocess.lua` 全平台通过
- ✅ E.2：用户自定义 PP shader API 跑通（demo 中含自定义 effect）
- ✅ 现有 D.x 系列 demo（`demo_ecs_render` / `demo_ecs_skinned` / `perf_benchmark`）**无回归**
- ✅ API 文档同步：`docs/api/Light_Graphics.md` / `MODULE_INDEX.md` 更新

---

## 7. 文档导航

- **本文档**：`docs/Phase E 渲染管线升级/ALIGNMENT_PhaseE.md` — 顶层对齐
- 后续产出（按 6A 工作流）：
  - `CONSENSUS_PhaseE.md` — 决策后共识 + 验收标准细化
  - `DESIGN_PhaseE_1.md` — E.1 2D 灯光架构设计
  - `DESIGN_PhaseE_2.md` — E.2 后处理栈架构设计
  - `TASK_PhaseE.md` — 原子任务拆分 + 依赖图
  - `ACCEPTANCE_PhaseE_*.md` — 各 step 验收记录
  - `FINAL_PhaseE.md` — 总结报告
  - `TODO_PhaseE.md` — 待办与缺失配置
