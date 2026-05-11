# ACCEPTANCE — Phase E.3.1 · RenderBackend HDR 能力 + ACES Shader

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.3.1**：`RenderBackend` 4 个 HDR 虚接口 + GL33 实现（RGBA16F FBO + 全屏 ACES tonemap）。

---

## 1. 改动摘要

| 文件 | 改动量 | 类型 | 关键点 |
|------|--------|------|--------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +70 行 | 修改 | 追加 4 个虚接口（`SupportsHDR` / `CreateHDRFBO` / `DeleteHDRFBO` / `DrawTonemapFullscreen`），默认实现为 false / 0 / no-op |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | ~300 行 | 修改 | (1) 追加 `VS_TONEMAP_SOURCE` / `FS_TONEMAP_SOURCE`（GLES 3.0 + GL 3.3 双版本）；(2) `GL33Backend` 加 HDR 成员（`vaoTonemap` / `vboTonemap` / `programTonemap` / 3 个 uniform location / `hdrFboDepthRB` map）；(3) `InitTonemap()` 方法：创建全屏 quad + 编译 shader + 缓存 uniform；(4) `Init()` 末尾 hook `InitTonemap()`；(5) `Shutdown` 末尾清理 HDR 资源；(6) 4 个虚接口完整实现 |

---

## 2. 关键设计

### 2.1 HDR FBO 结构

| 附件 | 格式 | 选择理由 |
|------|------|----------|
| COLOR_ATTACHMENT0 | `GL_RGBA16F` (half-float) | 可存 HDR 值 > 1.0，比 RGBA32F 省 50% 显存，GL 3.3 core 标配 |
| DEPTH_ATTACHMENT | `GL_DEPTH_COMPONENT24` RBO | 给 3D 留精度（现有 `CreateFBO` 用 16 够 2D，HDR 路径升级） |
| filter | `GL_LINEAR` min/mag | tonemap pass 需可线性采样 |
| wrap | `GL_CLAMP_TO_EDGE` | 全屏 quad 只采 `[0,1]` 范围内，边界外无关 |

### 2.2 ACES fitted (Narkowicz 2016)

```glsl
vec3 ACESFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a*x + b)) / (x * (c*x + d) + e), 0.0, 1.0);
}
```

- 4 个乘法 + 2 个除法 + 1 个 clamp；6 个 const 系数
- 比完整 ACES (RRT + ODT) 便宜一个数量级，视觉上 90% 还原
- 工业标杆（UE4 / Unity URP 默认）

### 2.3 HDR FBO → depth RBO 关系管理

**背景**：接口只返回 `fbo` + `tex`，但实际需要 3 个 GL 对象（fbo + tex + depthRB）。选择用 `unordered_map<fbo, depthRB>` 内部追踪，保证 `DeleteHDRFBO(fbo, tex)` 签名与 `DeleteFBO` 一致（后者是外部传入 depthRB）。

**陷阱**：调用方没配对 Delete 时内存泄漏。防护：`Shutdown()` 末尾遍历 `hdrFboDepthRB` map 兜底释放所有 depthRB。

### 2.4 双平台 shader 源码

| 平台 | VS / FS 版本 |
|------|-------------|
| Emscripten / Android / iOS | `#version 300 es` + `precision highp float` |
| 桌面（Windows / Linux / macOS） | `#version 330 core` |

shader 逻辑完全相同，仅头部 pragma 不同。

### 2.5 DrawTonemapFullscreen 的 GL state 管理

```
调用者责任:           本函数处理:                  函数出口:
UnbindFBO() 切 dflt → glDisable DEPTH/BLEND/SCISSOR → 不恢复 state
                    → glUseProgram(programTonemap)
                    → glUniform1f exposure/gamma
                    → glBindTexture(hdrTex) on TU 0
                    → glBindVertexArray(vaoTonemap)
                    → glDrawArrays(6 verts)
                    → 解绑 VAO/tex/program
```

**选择不恢复 state 的理由**：下次 `BeginFrame` 会重置 viewport / depth / blend；中间再画别的 2D/3D 内容前也会 `glEnable` 回来。Phase E.3.2 主循环会在 `EndScene` 后立刻 `SwapBuffers`，下一帧 `BeginFrame` 重置。

---

## 3. 验收清单

| 标准 | 状态 | 证据 |
|------|------|------|
| `Light.dll` 编译通过（GL33） | ✅ | `Light.vcxproj → Light.dll` |
| 4 个 HDR 虚接口接口签名正确 | ✅ | `include/render_backend.h:491-549` |
| GL33 `SupportsHDR` = tonemapSupported | ✅ | `render_gl33.cpp:1582` |
| GL33 `CreateHDRFBO` RGBA16F + Depth24 + FBO completeness check | ✅ | `render_gl33.cpp:1585-1635` |
| GL33 `DeleteHDRFBO` 查 map 释放 depthRB | ✅ | `render_gl33.cpp:1638-1653` |
| GL33 `DrawTonemapFullscreen` 6 顶点 ACES | ✅ | `render_gl33.cpp:1657-1682` |
| `InitTonemap` 在 `Init` 末尾调 | ✅ | `render_gl33.cpp:1187-1188` |
| `Shutdown` 清理 program/VAO/VBO + map 兜底 | ✅ | `render_gl33.cpp:1565-1575` |
| 既有 smoke 零回归 | ✅ | `lighting2d.lua` 40 PASS + DONE / `ecs_render.lua` ALL PASS / `graphics.lua` 11/11 |

---

## 4. 单元层验证

headless smoke 不能测真正的 GL 能力（无 context），但验证了：

1. **Light.dll 加载** 不崩（meaning 新增符号链接 OK，`unordered_map` 头文件已 include，4 个虚接口所有默认实现编译通过）
2. **既有 lighting2d.lua / ecs_render.lua / graphics.lua 行为不变**（无意外副作用）

真正 HDR 管线视觉验收留 E.3.3 demo + 手动运行。

---

## 5. 已知限制（留给 E.3.2/E.3.3）

| 限制 | 留给 |
|------|------|
| 无 `HDRRenderer` 模块（状态 / Begin/EndScene / Enable/Disable） | E.3.2 |
| 无 Lua API `Light.Graphics.HDR.*` | E.3.3 |
| 无主循环 hook（`light_ui.cpp:Window_Call`） | E.3.2 |
| 无 smoke `hdr.lua` | E.3.3 |
| 无 demo `demo_hdr` | E.3.3 |
| Legacy GL / iOS Metal / 低端 Android 是否真能创 RGBA16F RT | 与 E.3.2 / E.3.3 联合验证（IsSupported 负责决策） |

---

## 6. 下一步

**E.3.2**：`HDRRenderer` 命名空间模块 + `light_ui.cpp` 主循环 hook + `l_SetCanvas` HDR 兼容。
