# TODO — Phase E.8.x · G-buffer normal RT 升级 SSAO

> 6A 工作流 · 阶段 6 · Assess · 遗留事项与建议
>
> Phase E.8.x 主体已交付（功能 + smoke + 文档 6 份）；以下是 P1/P2 可选优化或后续维护需求。

---

## 1. 已完成总结

✅ **22 / 24 原子任务完成**（T2.3 / T2.8 / T2.9 经设计优化变为 N/A，等价目标已用 FS uViewMat3 达成）。

| 已交付 | 数量 |
|--------|------|
| RenderBackend MRT 接口 | 3 (CreateHDRFBO / DeleteHDRFBO / GetHDRNormalTex / DrawSSAO 升级) |
| GL33 实现升级 | 6 处（FBO MRT / map / SSAO bind / 6 FS shader 双 profile） |
| Module / Lua API | 4 处（SSAORenderer fallback / HDRRenderer::GetFBO / SSAO.GetNormalTexId / smoke +3） |
| 文档 | 6 份（ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL） |

---

## 2. P1 — 建议尽快补充（不影响功能但提升体验）

> **更新（2026-05-12）**：以下 P1 三项中 2 项完成 + 1 项确认无需改动：
> - ✅ **API_REFERENCE.md** — 已加 "Phase E 高级特性入口" 段 + SSAO 速查（含 `GetNormalTexId` 完整描述）
> - ✅ **CMakeLists POST_BUILD** — Light.dll 自动同步到 Lumen runtime（`ChocoLight/CMakeLists.txt:505-522`），build 时 `Sync Light.dll to Lumen runtime` step 自动执行
> - ⚪ **ENGINE_EVALUATION.md** — 该文档为 v0.3 月度评测快照，无 Phase E.8 细分章节可升级，留待评测人下次重写时统一更新
>
> 剩余 P1 项目仅 demo_ssao 可视化 toggle（见 2.1）。

### 2.1 demo_ssao 可视化 normal RT toggle

**当前**：demo 跑起来后看不到 normal MRT 是否正确，需要靠 smoke 间接验证。

**建议**：在 `samples/demo_ssao/main.lua` 里加 `N` 键 toggle，按下后切换全屏显示 normal tex（用一个简单的 quad + 自定义 shader 直接采 `SSAO.GetNormalTexId()` 返回的 GL id）。

**所需变更**：
```lua
-- samples/demo_ssao/main.lua
if pressed("N") then
    showNormalRT = not showNormalRT
end

if showNormalRT then
    local nid = Light.Graphics.SSAO.GetNormalTexId()
    if nid > 0 then
        Light.Graphics.UseShader(debugNormalShader)
        Light.Graphics.BindTextureRaw(nid)
        Light.Graphics.DrawFullscreenQuad()
        Light.Graphics.UseDefaultShader()
    end
end
```

**预计**：~30 行 Lua + 1 个 debug shader（GLSL 直接 `texture(uTex, uv).xy` 输出 RG）

---

### 2.2 normal MRT 视觉单元测试

**当前**：smoke 只验证 `GetNormalTexId` 接口，未验证编码正确性（即 view-space 法线 z 重建是否准确）。

**建议**：在 `samples/demo_ssao` 加一个已知朝向的 quad（朝 +Z），渲染后用 `glReadPixels` 读 normal tex 像素，断言 `r ≈ 0.5, g ≈ 0.5`（对应 view-space (0,0,1)）。

**所需变更**：在 demo 里加一个 N 键截图模式，调 `Light.Graphics.ReadPixels(x, y, 1, 1, "rgba")` 读出 RG 通道。可能需要 Lua 端 `ReadPixels` API（如不存在则待补）。

**预计**：~50 行 Lua + 可能 +1 个 Light.Graphics binding

---

### 2.3 ENGINE_EVALUATION.md 更新

**当前**：Phase E.8 评估描述 SSAO 用 ddx/dFdy。

**建议**：把这段升级为 "G-buffer normal MRT" 并标注 Phase E.8.x。

**位置**：`e:\jinyiNew\Light\ENGINE_EVALUATION.md` (grep "ddx" / "dFdy" / "SSAO")

**预计**：~5 行改动

---

## 3. P2 — 长期优化（性能或扩展性）

### 3.1 UBO 共享 view/projection 矩阵

**痛点**：当前 `uViewMat3` 每 draw 单独上传 9 floats，2000 drawcall/frame 浪费 ~70 KB/frame。

**方案**：扩展现有 `uboJointMatrices`（用于 Skinning）为 `uboFrameMatrices`，一帧绑一次。

**收益**：CPU-GPU 同步降低 ~5%（仅在高 drawcall 场景显著）

**代价**：需要改 6 个 program 的 uniform block 声明 + binding point 管理

**建议**：等 drawcall 突破 5000 / frame 再做

---

### 3.2 normal RT half-resolution

**痛点**：SSAO 在 half-res 工作，但读 full-res normal tex，浪费一半采样带宽。

**方案**：normal RT 改为 half-res；其他需要 full-res normal 的功能（如未来 SSR）单独配 full-res。

**收益**：~3 MB/frame 带宽节省

**代价**：mip mapping + filter 模式需要重新调；可能需要 normal blur pass 修复 alias

**建议**：等加 SSR 时统一规划，现在不动

---

### 3.3 Specular convolution（IBL）的 normal RT 复用

**痛点**：未来如果加 IBL（PBR 环境光），需要 normal 做 cubemap 查询。

**方案**：本 phase 已经搭好 view-space normal RT 链路，IBL 复用即可（注意 IBL 是 world-space 法线，需要 inverse 转换）。

**建议**：作为 Phase E.10 PBR-IBL 的前置已经就位。

---

## 4. P3 — 维护性 / 可选改进

### 4.1 lumen-master/build/.../Light.dll 自动同步

**痛点**：每次 ChocoLight 重新构建后必须手动 `Copy-Item` 到 lumen 目录，否则 smoke 跑到的是旧 dll。

**方案 A**：CMakeLists.txt 加 POST_BUILD step 拷贝 Light.dll 到 lumen runtime 目录。

**方案 B**：lumen-master 加 `--engine-dll` 命令行参数指定 dll 路径。

**建议**：方案 A 风险小，1 行 CMake 代码可解决：
```cmake
add_custom_command(TARGET Light POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:Light>
        ${CMAKE_CURRENT_SOURCE_DIR}/../lumen-master/build/src/light/$<CONFIG>/Light.dll
)
```

---

### 4.2 API_REFERENCE.md 更新

**位置**：`docs/API_REFERENCE.md` SSAO section

**建议**：加 `Light.Graphics.SSAO.GetNormalTexId()` 的描述（参考 ACCEPTANCE 文档 4.2 节）。

---

### 4.3 SKILLS.md 加 G-buffer 维护备忘

**位置**：`docs/SKILLS.md`

**建议**：加一个 "G-buffer 维护" section，记录"加新 fragment shader 时必须输出 FragNormal 到 location=1"。

---

## 5. 已知限制 / 不修复

| 限制 | 影响 | 是否修复 |
|------|------|----------|
| Legacy OpenGL 后端不支持 MRT | SSAO 跳过（已 silent fallback）| ❌ Legacy 后端非主要目标 |
| 透明物体（alphaMode=blend）不写 FragNormal | 透明区域 normal RT 是背景值，SSAO 估计偏离 | ❌ 标准做法，透明物体不写 G-buffer |
| 2D batch 默认朝相机（0,0,1） | 2D 旋转 sprite 法线不对 | ❌ 2D 用 Lit2D shader + normal map 而非 batch 的法线 |

---

## 6. 配置 / 部署相关

### 6.1 不需要配置变更

✅ 无新增 .env / 配置项
✅ 无新增 third_party 依赖
✅ 无新增 cmake option

### 6.2 运行环境要求

- **桌面**：OpenGL 3.3 Core+（要求 `glDrawBuffers`，2009+ 显卡均支持）
- **移动**：OpenGL ES 3.0+（要求 `layout(location=...) out` 与 RG16F，2014+ 设备均支持）
- **Web**：WebGL 2.0（= GLES 3.0 子集，原生支持）

---

## 7. 用户操作指引

### 7.1 用户侧无感升级

Phase E.8.x 是**全透明升级**：
- 不需要改 Lua 代码
- 不需要重新调 `SSAO.Enable(w, h)`
- 不需要改任何 sample / demo

只要使用 GL33Core 后端，启用 HDR 后调 SSAO，背后已自动用 G-buffer normal MRT。

### 7.2 验证升级是否生效

```lua
local HDR = require("Light.Graphics").HDR
local SSAO = require("Light.Graphics").SSAO

HDR.Enable(1920, 1080)
SSAO.Enable(1920, 1080)
local nid = SSAO.GetNormalTexId()
if nid > 0 then
    print("G-buffer normal MRT 已启用，纹理 ID =", nid)
else
    print("后端不支持 MRT，SSAO 静默跳过")
end
```

---

## 8. 总结

Phase E.8.x **核心目标已完成**。剩余事项均为 P1（demo 可视化）或 P2（性能优化）级别，不阻塞主分支合入或后续 phase 推进。

建议优先级：
1. P1.2.3 ENGINE_EVALUATION.md 更新（~5 min）
2. P3.4.2 API_REFERENCE.md 更新（~10 min）
3. P3.4.1 CMakeLists Light.dll 自动同步（~5 min）
4. P1.2.1 demo_ssao N 键 toggle（~30 min）
5. 其余视后续 phase 规划

---

**文档维护**：发现遗漏或新需求时直接 append 至本文件 "新增" 节。
