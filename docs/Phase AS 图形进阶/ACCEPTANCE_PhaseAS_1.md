# Phase AS.1 — Canvas 增强 + Shader uniform 扩展 — 验收文档

> **状态**: 已完成本地实施 + lightc 语法检查通过, 待 GitHub Actions CI 全平台验证

---

## 一、实施概览

| 维度 | 数量 / 内容 |
|---|---|
| 修改 Lua 模块 | 4 (Light.Graphics, Canvas, Image, Shader) |
| 新增 Lua 函数 (Canvas) | 4 (GetTextureId, GetWidth, GetHeight, Clear) |
| 新增 Lua 函数 (Image) | 1 (GetTextureId, 与 Canvas 对称) |
| 新增 Lua 函数 (Graphics 模块) | 2 (PushCanvas, PopCanvas) |
| 新增 Lua 方法 (Shader 元表) | 7 (SetMat3, SetIVec2/3/4, SetFloatArray, SetVec2Array, SetTexture) |
| 新增 RenderBackend 虚函数 | 9 (SetUniformMat3, SetUniform2i/3i/4i, SetUniform1fv, SetUniform2fv, SetUniformSampler, GenerateMipmap, ClearCurrent) |
| 修改文件 | 6 (render_backend.h, render_gl33.cpp, light_graphics.cpp, light_graphics_canvas.cpp, light_graphics_image.cpp, light_graphics_shader.cpp) |
| 新增文件 | 4 (canvas_shader_ext.lua + 3 docs) |
| **C++ 总代码** | **~770 行** |

---

## 二、API 详细列表

### 2.1 `Light.Graphics.Canvas` 增强 (4 个新方法)

```lua
canvas:GetTextureId() -> int   -- 原生 GL 纹理 ID (用于 shader:SetTexture); 0=未初始化
canvas:GetWidth()    -> int    -- 画布像素宽
canvas:GetHeight()   -> int    -- 画布像素高
canvas:Clear([r], [g], [b], [a])  -- 清空 canvas (默认 0,0,0,0)
```

### 2.2 `Light.Graphics.Image` 增强 (1 个新方法, 对称)

```lua
image:GetTextureId() -> int    -- 原生 GL 纹理 ID; 0=未初始化
```

### 2.3 `Light.Graphics` 模块 (2 个新栈式控制)

```lua
Light.Graphics.PushCanvas(canvas)  -- 入栈, 切到 canvas (栈深 8 软限制)
Light.Graphics.PopCanvas()         -- 出栈, 恢复
```

### 2.4 `Light.Graphics.Shader` 元表 (7 个新 setter)

```lua
shader:SetMat3(name, {m00..m22})              -- 9 floats
shader:SetIVec2(name, x, y)
shader:SetIVec3(name, x, y, z)
shader:SetIVec4(name, x, y, z, w)
shader:SetFloatArray(name, {v1..vN})          -- 软限制 256
shader:SetVec2Array(name, {x1,y1, x2,y2..})    -- 平铺存储, 软限制 256 vec2
shader:SetTexture(name, tex_id, [slot])       -- slot 默认 1, 0 留给引擎默认纹理
```

### 2.5 RenderBackend C++ 接口扩展 (9 个新虚函数)

```cpp
virtual void SetUniformMat3(int loc, const float* m);
virtual void SetUniform2i/3i/4i(int loc, int x, int y, [int z, [int w]]);
virtual void SetUniform1fv(int loc, int count, const float* v);
virtual void SetUniform2fv(int loc, int count, const float* v);
virtual void SetUniformSampler(int loc, int slot, uint32_t texId);
virtual void GenerateMipmap(uint32_t texId);
virtual void ClearCurrent(float r, float g, float b, float a);
```

GL33 后端真实现, Legacy 后端通过基类默认 no-op (与 `IsSupported() == false` 一致)。

---

## 三、关键设计决策 (与 CONSENSUS_PhaseAS_1 对齐)

| Q | 决策 | 实施 |
|---|---|---|
| Q1 Canvas 取纹理 API | A 调整 — `canvas:GetTextureId() -> int`, Image 也加 GetTextureId 对称 | 简洁清晰, shader:SetTexture 直接收数字 |
| Q2 Push/PopCanvas 栈深 | A — 软限制 8 | `kCanvasStackMax = 8`, 溢出仅警告 |
| Q3 SetTexture slot | A — 可选, 默认 1 | luaL_optinteger(L, 4, 1) |
| Q4 Mat3 数据格式 | A — 9 float table (与 Mat4 一致) | lua_rawgeti loop 提取 9 浮点 |
| Q5 Legacy GL 后端行为 | A — silent no-op | 基类默认实现已为空, Legacy 不重载 |

---

## 四、Smoke 测试覆盖 (`scripts/smoke/canvas_shader_ext.lua`)

5 个 stage:

1. **Light.Graphics 模块**: PushCanvas/PopCanvas 是 function; SetCanvas/GetCanvas 仍存在; 空栈 PopCanvas 不崩
2. **Light.Graphics.Canvas**: 4 新方法都是 function; nil ctx 调用安全 (返回 0/不崩)
3. **Light.Graphics.Image**: GetTextureId 注册; 现有 GetWidth/Height/Depth/Dimensions 不破坏
4. **Light.Graphics.Shader**: New/UseDefault/IsSupported 仍 OK; 在 GL context 可用时验证 7 新 setter 在元表; 边界路径 (空表/奇数长度/超 256) 安全
5. **回归兼容**: Light.UI Phase AQ 常量 + Light.Event Phase AR 模块仍可用

---

## 五、跨编译单元设计

CanvasContext 内存布局在 `light_graphics.cpp:CanvasCtxMirror` 镜像声明 (与 `light_graphics_canvas.cpp:CanvasContext` 二进制兼容: `{ uint fbo, uint texture, uint depthRB, int width, int height }`)。

避免引入额外头文件: 跨模块仅通过 `void*` 指针传递, 强制类型同步靠注释/CR 维护。后续如有第 3 处用到, 应抽出 `include/canvas_context.h`。

---

## 六、回归影响评估

| 受影响模块 | 影响 |
|---|---|
| `Light.Graphics.Canvas` 元表 | 新增 4 方法, 不动 __call/__tostring |
| `Light.Graphics.Image` 元表 | 新增 1 方法, 不动现有 |
| `Light.Graphics` 模块函数表 | 新增 2 fn, 顺序在 SetScissor 之后 |
| `Light.Graphics.Shader` userdata 元表 | 新增 7 method, 不动现有 SetFloat/Vec2/3/4/Int/Mat4/Use/Delete |
| `RenderBackend` 接口 | 新增 9 虚函数, 默认 no-op 体不破坏二进制兼容 |
| `RenderVertex` layout | **零修改** (本 Phase 不涉及 3D mesh) |
| `Event` POD layout | **零修改** |
| 其他 Light.* 模块 (Audio/Physics/Sensor 等) | **零修改** |

---

## 七、未实施项 (按计划放在后续子 Phase)

- **AS.2** 3D Mesh 基础: `Light.Graphics.Mesh` userdata + vertex layout 抽象 + 深度测试启用
- **AS.3** cgltf 集成 + .gltf/.glb 解析
- **AS.4** 材质系统 (PBR/Unlit 双 shader, 多 texture slot, 基础 lighting)

---

## 八、CI 验收标准

- [x] `lightc -p scripts/smoke/canvas_shader_ext.lua` Exit=0 (本地)
- [ ] GitHub Actions `Build Templates (All Platforms)` 全绿:
  - [ ] Windows x64: 编译 + Windows runtime smoke (含 canvas_shader_ext.lua)
  - [ ] Linux x64: 编译 + 语法检查
  - [ ] macOS Universal: 编译 + 语法检查
  - [ ] Android arm64+x86_64: 编译
  - [ ] iOS arm64: 编译
  - [ ] Web WASM: 编译

CI 全绿后此子 Phase 才算最终交付完成。
