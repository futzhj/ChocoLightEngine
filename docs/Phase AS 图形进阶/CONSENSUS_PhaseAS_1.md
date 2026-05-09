# Phase AS.1 — Canvas 增强 + Shader uniform 扩展 — 共识文档

> **状态**: ✅ 决策已锁定 (Q1~Q5 全 A), 进入实施

---

## 1. 锁定范围 (AS.1, 13 fns)

### 1.1 Canvas 增强 (6 个方法)

```lua
-- 在 Canvas 元表上加 4 个查询/操作方法
canvas:GetTextureId() -> int                   -- 返回原生 GL texture id (用于 Shader:SetTexture)
canvas:GetWidth()    -> int
canvas:GetHeight()   -> int
canvas:Clear([r], [g], [b], [a])  -> ()        -- 清空当前 canvas 内容 (默认 0,0,0,0)

-- 在 Light.Graphics 模块加 2 个栈式渲染目标控制
Light.Graphics.PushCanvas(canvas) -> ()         -- 入栈, 切到 canvas
Light.Graphics.PopCanvas()         -> ()         -- 出栈, 恢复
```

### 1.2 Shader uniform 扩展 (7 个 setter)

```lua
shader:SetTexture(name, image, [slot])    -- slot 默认 1 (slot 0 留给引擎)
shader:SetMat3(name, {m00..m22})          -- 9 floats
shader:SetIVec2(name, x, y)
shader:SetIVec3(name, x, y, z)
shader:SetIVec4(name, x, y, z, w)
shader:SetFloatArray(name, {v1, v2, ...})  -- 1D float 数组
shader:SetVec2Array(name, {x1,y1, x2,y2, ...})  -- 平铺存储, count = #table / 2
```

实际计入 Shader 6 个新方法 (SetVec2Array 算扩展但工程相同), Canvas 6 个方法, 合计 **12-13 fns**。

### 1.3 RenderBackend 内部接口扩展

```cpp
// 新增 virtual (GL33 真实现, Legacy 默认 no-op 或回退)
virtual uint32_t GetFBOTexture(uint32_t fbo) const;       // 已知, 但需暴露 query
virtual void ClearCurrent(float r, float g, float b, float a);  // 清空当前 FBO/默认目标
virtual void GenerateMipmap(uint32_t texId);              // canvas:GetTexture 可选 mipmap
virtual void SetUniform1iv(int loc, int count, const int* v);
virtual void SetUniformMat3(int loc, const float* m);
virtual void SetUniform1fv(int loc, int count, const float* v);
virtual void SetUniform2fv(int loc, int count, const float* v);
virtual void SetUniformSampler(int loc, int slot, uint32_t texId);  // 关键: 绑纹理到 slot, 设 sampler uniform
```

---

## 2. 关键决策 (Q1~Q5 全 A)

| Q | 决策 |
|---|---|
| Q1 Canvas 取纹理 API | **A 调整** — `canvas:GetTextureId() -> int` 返回原生 GL id; Shader:SetTexture 智能识别 number/lightuserdata/table(__instance) |
| Q2 Push/Pop 栈深度 | **A** — 软限制 8 (超出报警告但不中断) |
| Q3 SetTexture 的 slot 参数 | **A** — 可选, 默认 slot=1 (slot=0 引擎专用) |
| Q4 Mat3 数据格式 | **A** — 9 float 的 table (与 SetMat4 一致) |
| Q5 Legacy GL 后端行为 | **A** — silent no-op (与 IsSupported() 一致) |

---

## 3. 技术方案

### 3.1 Canvas:GetTexture 实现

`Image` userdata 已有结构 (光检查 `light_graphics_image.cpp`). 我们在 Canvas 元表加方法:

```cpp
static int l_Canvas_GetTexture(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__instance");
    CanvasContext* cc = (CanvasContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!cc || !cc->texture) { lua_pushnil(L); return 1; }

    // 调 light_graphics_image.cpp 提供的 helper, 用 GL texture id 包成 Image
    // 该 Image 不拥有 GL texture 所有权 (Canvas 仍持有)
    extern int Image_FromTexture(lua_State*, uint32_t texId, int w, int h, bool owns);
    return Image_FromTexture(L, cc->texture, cc->width, cc->height, false);
}
```

需在 `light_graphics_image.cpp` 增加导出函数 `Image_FromTexture`。

### 3.2 PushCanvas / PopCanvas 栈

```cpp
struct CanvasStackEntry {
    CanvasContext* canvas;  // nullptr = default render target
    int viewport[4];
};
static CanvasStackEntry g_canvasStack[8];
static int g_canvasStackTop = 0;

static int l_PushCanvas(lua_State* L) {
    if (g_canvasStackTop >= 8) {
        CC::Log(CC::LOG_WARN, "PushCanvas: stack overflow (max 8)");
        return 0;
    }
    // 保存当前状态, 切到新 canvas
    ...
}

static int l_PopCanvas(lua_State* L) {
    if (g_canvasStackTop == 0) return 0;
    // 恢复之前状态
    ...
}
```

### 3.3 Shader:SetTexture 实现

```cpp
static int l_Shader_SetTexture(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    // arg 3 = Image userdata 或 lightuserdata 或 number(GL tex id)
    uint32_t texId = ExtractTextureId(L, 3);  // helper, 支持多种输入
    int slot = (int)luaL_optinteger(L, 4, 1);

    if (!g_render || !ud->programId || !texId) return 0;
    int loc = g_render->GetUniformLocation(ud->programId, name);
    if (loc < 0) return 0;

    g_render->SetUniformSampler(loc, slot, texId);
    return 0;
}
```

GL33 实现: `glActiveTexture(GL_TEXTURE0 + slot)` + `glBindTexture(GL_TEXTURE_2D, texId)` + `glUniform1i(loc, slot)`.

### 3.4 数组 setter

数组传入用 Lua table, 长度通过 `lua_objlen` (Lua 5.1) 取:

```cpp
static int l_Shader_SetFloatArray(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    int count = (int)lua_objlen(L, 3);
    if (count <= 0 || count > 256) {  // 软限制防泄漏
        return 0;
    }
    std::vector<float> values(count);
    for (int i = 0; i < count; i++) {
        lua_rawgeti(L, 3, i + 1);
        values[i] = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);
    }
    int loc = g_render->GetUniformLocation(ud->programId, name);
    if (loc >= 0) g_render->SetUniform1fv(loc, count, values.data());
    return 0;
}
```

---

## 4. 验收标准

- [ ] `Canvas` 元表 4 新方法都是 function (GetTexture/GetWidth/GetHeight/Clear)
- [ ] `Light.Graphics.PushCanvas / PopCanvas` 是 function
- [ ] `Shader` 元表 6 新 setter 都是 function (SetTexture/SetMat3/SetIVec2/3/4/SetFloatArray + 可选 SetVec2Array)
- [ ] `Canvas:GetTexture()` 返回 Image userdata, 可调用 Image API (如 GetWidth/GetHeight)
- [ ] PushCanvas 栈溢出 (>8 层) 仅警告不崩, PopCanvas 在空栈时也不崩
- [ ] Shader:SetTexture 支持 slot=1 (默认) 和 slot>=2 (多纹理)
- [ ] `scripts/smoke/canvas_shader_ext.lua` 通过 lightc -p
- [ ] **Windows runtime smoke 通过** — 关键, 因 Canvas/Shader 测试需要 GL 上下文
- [ ] 6 平台 CI 全绿
- [ ] 不引入 cgltf / 任何 3D mesh 依赖

---

## 5. 实施步骤

1. `light_graphics_image.cpp` 加 `Image_FromTexture(L, texId, w, h, owns)` helper
2. `light_graphics_canvas.cpp` 加 GetTexture / GetWidth / GetHeight / Clear 4 个方法
3. `light_graphics.cpp` 加 PushCanvas / PopCanvas + canvas stack
4. `render_backend.h` 加 6 个新 virtual (sampler, mat3, 1iv, 1fv, 2fv, mipmap)
5. `render_gl33.cpp` 实现 6 个新接口
6. `render_legacy.cpp` 默认 no-op (基类 default body 已有, 无需重载)
7. `light_graphics_shader.cpp` 加 SetTexture / SetMat3 / SetIVec2/3/4 / SetFloatArray (+ 可选 SetVec2Array)
8. `scripts/smoke/canvas_shader_ext.lua` smoke
9. `.github/workflows/build-templates.yml` 加 smoke
10. ACCEPTANCE_PhaseAS_1.md
11. commit + push + 6 平台 CI 全绿
