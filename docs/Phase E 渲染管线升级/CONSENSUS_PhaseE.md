# CONSENSUS — Phase E 渲染管线升级

> 6A 工作流 · 阶段 1 · 共识（Align 收尾）
> 在 `ALIGNMENT_PhaseE.md` 基础上锁定决策、形成可执行共识。

---

## 1. 决策结果

| 决策点 | 选项 | 影响 |
|--------|------|------|
| **Q1 SpotLight** | ✅ **包含** | Light2D type 含 point/spot/ambient |
| **Q2 Normal Map** | ✅ **包含** | RenderVertex 加 tangent；BatchRenderer 改造 |
| **Q3 后处理 effect** | ✅ **核心 3 个**（Tonemap / Bloom / Vignette） | + 用户自定义 PP shader API |
| **Q4 HDR Framebuffer** | ✅ **默认 HDR（RGBA16F）** | 启用 MainRT 后默认 16-bit；Tonemap 必装 |

---

## 2. 最终范围

### 2.1 E.1 — 2D 灯光系统

#### 必交付

- **C++ 渲染层**
  - `RenderVertex` 扩展（保留向后兼容）：新增 `RenderVertex2DLit { x,y,z, u,v, r,g,b,a, nx,ny,nz, tx,ty,tz,tw }` 16 floats，专用于 Lit sprite；原 `RenderVertex` 不变
  - GL33 后端新增 Lit 2D shader：`VS_LIT2D_SOURCE` + `FS_LIT2D_SOURCE`（GLES 3.0 兼容）
  - GL33 后端新增 `programLit2D` 槽位 + `DrawLitQuad` / `DrawLitTriangles` 接口
  - 多光 uniform arrays（**最大 16 个 light**）：`uLight[i] { type, pos, dir, color, range, intensity, innerCos, outerCos }`
  - 环境光 `uAmbient2D` (vec3)
- **C++ 模块**：`light_lighting2d.cpp`（`Light.Lighting2D` 全局单例模块）
- **Lua API**：
  ```lua
  Light.Lighting2D.SetEnabled(bool)
  Light.Lighting2D.SetAmbient(r, g, b)
  Light.Lighting2D.AddPointLight({x, y, color={r,g,b}, range, intensity}) -> id
  Light.Lighting2D.AddSpotLight({x, y, dirX, dirY, color, range, innerAngle, outerAngle, intensity}) -> id
  Light.Lighting2D.UpdateLight(id, fields)
  Light.Lighting2D.RemoveLight(id)
  Light.Lighting2D.ClearLights()
  Light.Lighting2D.GetLightCount() -> int
  Light.Lighting2D.GetMaxLights() -> 16
  ```
- **Sprite 渲染路径扩展**：`Light.Graphics.DrawLit(image, normalMap?, x, y, ...)` — 新增 API（与 `Draw/DrawQuad` 平行）。若 `normalMap=nil` 用 default flat normal (0,0,1)
- **ECS Component 与 System**：
  - `Light2D` component（type/color/range/intensity/dirX/dirY/innerAngle/outerAngle/enabled）
  - `LitSprite` component（image, normalMap, color, visible, anchor, quad）— 与 `Sprite` 平行
  - `Light2DSystem`：每帧扫描 active Light2D + Transform2D → 上传 uniforms
  - `LitSpriteSystem`：每帧渲染 LitSprite component（在 Sprite/SpriteBatch 之间）
- **Demo + Smoke**：`samples/demo_2d_lighting/main.lua` + `scripts/smoke/lighting2d.lua`

#### 不交付（推迟）

- ❌ 2D Shadow（alpha mask projection）→ Phase E.3
- ❌ 顶点光（每顶点而非每片段计算）→ 性能优化 Phase E.x
- ❌ Light culling / clustered light → Phase E.x
- ❌ Light 的 falloff curve 自定义 → 用固定平方反比衰减

### 2.2 E.2 — 后处理栈

#### 必交付

- **HDR FBO**
  - `CreateHDRFBO(w, h)` — RGBA16F 颜色附着 + 24-bit depth
  - 兼容 GLES 3.0+（`GL_RGBA16F` 内部格式 + `GL_HALF_FLOAT` 数据类型必须支持）
  - Web/Android 检测 fallback 到 RGBA8（用户层提示降级）
- **MainRT 概念**
  - `Light.Graphics.SetMainRT({width, height, hdr=true})` — 启用全局主 RT
  - `Light.Graphics.BeginScene(clearR, g, b, a)` — 隐式绑 MainRT FBO
  - `Light.Graphics.EndScene()` — 解绑，后续 `Pipeline:Apply()` 用 MainRT 作输入
- **PostProcess Pipeline**
  - `Light.Graphics.PostProcess` 类
  - 内部维护 2 个 ping-pong FBO（自动按 MainRT 尺寸创建）
  - 链式 `:AddXxx()` + `:Apply(srcCanvas?, dstCanvas?)`（默认 src=MainRT, dst=default FB）
- **内置 effect（3 个）**
  - **Tonemap**：mode = 'reinhard' | 'aces' | 'filmic'；exposure；gamma
  - **Bloom**：threshold；intensity；blurPasses (default 4)；spread
  - **Vignette**：radius；softness；color (rgb)；intensity
- **用户自定义 PP shader**
  - `pipeline:AddCustom(name, shaderInstance, paramsTable)`
  - shader 约定：`uniform sampler2D uInput; uniform vec2 uResolution; ...用户其他 uniform`
  - 引擎自动绑 input texture + uResolution
- **ECS 集成**
  - `Camera2D` / `Camera3D` 加 `postProcess` 字段（pipeline 引用）
  - `world:Render()` 在 camera 阶段开始/结束自动 BeginScene/EndScene + Apply
- **Demo + Smoke**：`samples/demo_post_process/main.lua` + `scripts/smoke/postprocess.lua`

#### 不交付（推迟）

- ❌ Blur / ColorGrade → Phase E.2.b
- ❌ MRT (Multi Render Target) → Phase E.x（暂时单 color 附着够用）
- ❌ TAA / SMAA / FXAA → Phase E.x
- ❌ Screen-Space Ambient Occlusion (SSAO) → 3D 专属，Phase E.3

### 2.3 跨阶段共用基础

- **2D Lit shader** 也会被未来的 3D normal-mapped sprite 等场景复用
- **HDR FBO** 同时服务 E.1（高动态范围光照输出）和 E.2（Tonemap pipeline 输入）
- **Lighting2D** uniform 上传机制与 3D PBR lighting 共用 helper（已有 `UploadPBRLightingUniforms`）

---

## 3. 技术约束

### 3.1 平台兼容性

| 平台 | GLES/GL 版本 | HDR 支持 | Lit Shader |
|------|--------------|---------|------------|
| Windows / macOS / Linux 桌面 | GL 3.3 Core | ✅ RGBA16F | ✅ |
| Android | GLES 3.0+ | ✅ RGBA16F (Android 5.0+) | ✅ |
| iOS | GLES 3.0+ | ✅ RGBA16F | ✅ |
| Web (Emscripten WebGL2) | GLES 3.0 | ✅ EXT_color_buffer_half_float | ✅ |
| GL 1.x Legacy（极老硬件） | — | ❌ fallback LDR | ❌ Lit shader 不可用 |

**HDR 检测降级**：`CreateHDRFBO` 失败时自动尝试 RGBA8，并通过 `Light.Graphics.IsHDRSupported()` 返回 false。

### 3.2 性能预期

- BatchRenderer 现有 quad 路径**不退化**（默认 sprite 走老路径）
- Lit sprite 因 uniform 上传 + 多光 fragment 计算，吞吐降至 60-70%（可接受）
- Bloom 默认 4 passes blur（downsample → blur → upsample），1080p < 2ms
- Tonemap < 0.5ms
- 16 个 active light 时 fragment shader 内 loop（GLES 3.0 支持 uniform-bound loop count）

### 3.3 内存预算

- 主 RT (HDR 1024×768 RGBA16F + depth 24)：~7.5 MB
- Bloom ping-pong 4 级 downsample：~5 MB
- PP pipeline ping-pong 2 个：~7.5 MB × 2
- **总增加内存**：~25 MB（HDR 启用时）/ ~12 MB（LDR 启用时）

### 3.4 顶点格式演进

| 顶点类型 | 大小 | 用途 |
|----------|------|------|
| `RenderVertex` (现有) | 36 字节（9 floats） | 默认 sprite/quad/text — 不变 |
| `RenderVertex3D` (现有) | 48 字节 | 3D mesh — 不变 |
| `RenderVertex3DSkin` (现有) | 68 字节 | GPU skinning — 不变 |
| **`RenderVertex2DLit`** (新增) | **64 字节**（16 floats） | 2D Lit sprite/quad — 含 normal+tangent |

`RenderVertex2DLit`：pos(12) + uv(8) + color(16) + normal(12) + tangent(16) = 64 字节

### 3.5 Shader 资产

- 现有 shader 全部内嵌于 `render_gl33.cpp`（无外部 shader 文件）
- 本轮新增也保持内嵌；后续考虑独立 `.glsl` 文件管理（Phase E.x）

---

## 4. 验收标准

### 4.1 功能性验收

#### E.1 — 2D 灯光

- [ ] `Light.Lighting2D.SetEnabled(true)` 启用 lit 路径
- [ ] PointLight：单灯距离衰减正确（平方反比）
- [ ] SpotLight：innerAngle/outerAngle 渐变正确（cone falloff）
- [ ] AmbientLight：环境光叠加在所有 lit sprite
- [ ] 16 个 light 同时启用 + 渲染正确
- [ ] LitSprite 含 normalMap 时凹凸感正确（侧光下高光偏向光源）
- [ ] LitSprite 无 normalMap 时退化为 diffuse-only（平面光照）
- [ ] ECS `Light2D` component CRUD 正常
- [ ] ECS `Light2D` 跟随 Transform2D parent chain 移动
- [ ] 不影响现有 Sprite/SpriteBatch/TextRenderer 渲染（默认 sprite 走原路径）

#### E.2 — 后处理

- [ ] `SetMainRT({hdr=true})` 启用 RGBA16F MainRT
- [ ] `IsHDRSupported()` 在不支持平台返回 false 并降级 RGBA8
- [ ] Tonemap (ACES/Reinhard) 在 HDR 输入下输出合理颜色
- [ ] Bloom (threshold + multi-pass blur) 高光辉光效果
- [ ] Vignette 暗角渐变到边缘
- [ ] 用户自定义 PP shader 跑通（demo 含一个 grayscale custom effect）
- [ ] Pipeline 多 pass 链式跑通（Bloom → Tonemap → Vignette）
- [ ] ECS Camera2D `postProcess` 字段自动应用

#### 跨阶段

- [ ] 现有 demo 全部无回归：`demo_ecs_render`, `demo_ecs_skinned`, `demo_ecs_network`, `demo_animation`, `perf_benchmark`
- [ ] 现有 smoke 全部通过
- [ ] API 文档同步：`docs/api/Light_Graphics.md`, `MODULE_INDEX.md`

### 4.2 性能验收

- [ ] 默认 Sprite 路径 1000 quad < 16ms（保持 Phase A 性能）
- [ ] Lit Sprite 路径 200 quad + 8 light < 16ms
- [ ] Bloom 4-pass 1080p < 2ms（桌面）
- [ ] Tonemap 1080p < 0.5ms
- [ ] Vignette 1080p < 0.5ms

### 4.3 全平台 CI

- [ ] Windows / Linux / macOS 桌面 CI 通过
- [ ] Android (NDK + GLES3) 编译通过
- [ ] iOS (Xcode + GLES3) 编译通过
- [ ] Web (Emscripten WebGL2) 编译通过

### 4.4 代码质量

- [ ] 所有新增 cpp/lua 含中文注释（关键节点 + 难懂代码）
- [ ] API 中文 `@lua_api` 注释完整（用于文档自动生成）
- [ ] 无新增 lint warning（除现有 LSP 历史误报外）
- [ ] 单元测试 / smoke 覆盖率：>= 现有水平
- [ ] 无内存泄漏（FBO/texture/shader/VAO 析构正确）

---

## 5. 任务拆分预览

> 详细原子任务定义见 `TASK_PhaseE.md`，本节仅给出路线图。

### Phase E.1 — 2D 灯光（按依赖顺序）

1. **E.1.1** 新增 `RenderVertex2DLit` 数据结构 + GL33 VAO 描述
2. **E.1.2** 实现 `VS_LIT2D` + `FS_LIT2D` shader（含 multi-light + normal map）
3. **E.1.3** `Light.Lighting2D` C++ 模块（管理 16 个 light + ambient + 上传 helper）
4. **E.1.4** Lua API 绑定（AddPoint/AddSpot/SetAmbient/Update/Remove/Clear）
5. **E.1.5** `Light.Graphics.DrawLit` API + GL33 后端 `DrawLitQuad/Triangles`
6. **E.1.6** ECS `Light2D` + `LitSprite` component + 2 个 System + Render() 入口集成
7. **E.1.7** `samples/demo_2d_lighting/` + `scripts/smoke/lighting2d.lua`
8. **E.1.8** API 文档同步

### Phase E.2 — 后处理（按依赖顺序）

1. **E.2.1** `CreateHDRFBO` (RGBA16F) + 平台检测降级 + `IsHDRSupported`
2. **E.2.2** `SetMainRT / BeginScene / EndScene` API
3. **E.2.3** `Light.Graphics.PostProcess` 类 + Pipeline + ping-pong RT 管理
4. **E.2.4** 内置 Tonemap effect (Reinhard / ACES / Filmic) + Lua API
5. **E.2.5** 内置 Bloom effect (downsample + threshold + blur + composite)
6. **E.2.6** 内置 Vignette effect + 用户自定义 PP shader API
7. **E.2.7** ECS `Camera2D/3D.postProcess` 集成 + Render() 入口
8. **E.2.8** `samples/demo_post_process/` + `scripts/smoke/postprocess.lua`
9. **E.2.9** API 文档同步

### Phase E.3 — 收尾

1. **E.3.1** 全平台 CI 验证 + 现有 demo 回归测试
2. **E.3.2** `FINAL_PhaseE.md` 总结 + `TODO_PhaseE.md` 待办

**预估总工作量**：18 个原子任务，预计需要多个 commit 分批交付。

---

## 6. 关键风险与缓解

| 风险 | 严重度 | 缓解 |
|------|--------|------|
| Lit shader 在低端 GPU 性能不达标 | 中 | 默认 sprite 不走 Lit 路径；Lit 是 opt-in |
| Web 平台 RGBA16F 不支持 | 中 | HDR 失败时降级 RGBA8 + warning |
| Normal Map 顶点格式改造影响 BatchRenderer | 高 | Lit 用独立 VAO；不动现有 BatchRenderer |
| Bloom 在 HDR 关闭时颜色不正确 | 低 | Tonemap 与 Bloom 解耦；Bloom 可单独跑 |
| 工作量估计不准（18 个任务） | 中 | 分阶段交付；先做 E.1 一组 commit，再 E.2 |
| 现有 demo 回归 | 高 | 任务结尾必跑全套 smoke + 至少 3 个 demo 截图对比 |

---

## 7. 共识签字

- ✅ 范围明确：E.1 + E.2 全功能 + ECS 集成 + Demo + Smoke + 文档
- ✅ 决策固化：SpotLight 包含、Normal Map 包含、3 个 PP effect、默认 HDR
- ✅ 兼容性约束：向后兼容现有 Sprite 路径
- ✅ 验收标准：功能性 22 项 + 性能 5 项 + 全平台 CI + 代码质量
- ✅ 风险已识别且有缓解方案

**下一步**：编写 `DESIGN_PhaseE_1.md` + `DESIGN_PhaseE_2.md`（架构详细设计），然后 `TASK_PhaseE.md`（原子任务拆分），最后人工审查（Approve）→ 开始 Automate 实现。
