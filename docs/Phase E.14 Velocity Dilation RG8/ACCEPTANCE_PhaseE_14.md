# Phase E.14 Velocity Dilation + RG8 Format — ACCEPTANCE 验收文档

> **任务名**：Phase E.14 Velocity Dilation + RG8 Format（速度缓冲底层优化）
> **状态**：✅ **实现完成，CI 6/6 + Windows runtime smoke 通过**
> **基线**：Phase E.13 Motion Vector Velocity（commit `9f32401`）
> **方案**：3x3 max-length dilation（uniform gating）+ RG8/RG16F 双格式可选（uniform 切 decode 路径）

---

## 1. 任务完成度总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Align | `ALIGNMENT_PhaseE_14.md`（5 项决策已拍板） | ✅ |
| Architect | `DESIGN_PhaseE_14.md`（13 节 + mermaid 流程图） | ✅ |
| Atomize | `TASK_PhaseE_14.md`（T1~T7 + 依赖图） | ✅ |
| T1 Backend interface + GL33 internalFormat 分支 | `VelocityFormat` enum、`CreateHDRFBO` 加 format 默认参数、`SetVelocityDilation/GetVelocityDilation/GetVelocityScale/GetActiveVelocityFormat` 接口、GL33 `RG16F`/`RG8` `glTexImage2D` 二选一、`hdrFboVelocityFormat` 跟踪、`activeVelocityFormat_` 缓存、Shutdown/DeleteHDRFBO 清理 | ✅ |
| T2 SSRTemporal shader dilation + decode 双格式 | GLES3 + GL33 双 profile 加 `uVelocityDilation` / `uVelocityFormat` / `uVelocityScale`，新 helper `DecodeVelocity` + `SampleVelocityDilated`（3x3 max-length），主流程改用 dilated 采样；uniform location 缓存 ×3，`DrawSSRTemporal` 签名扩 3 trailing 默认参数并上传 | ✅ |
| T3 3D shader velocity encode 双格式 | 4 处 fragment（FS_UNLIT/FS_PBR × GLES3/GL33）加 `uVelocityFormat` + `uVelocityScale` + 双格式编码分支（RG16F=raw / RG8=clamp(raw/(2*s)+0.5)），Skin/Skin+Morph 复用 FS_UNLIT/FS_PBR 自动覆盖；`UploadVelocityUniforms` 上传 2 个新 uniform | ✅ |
| T4 HDRRenderer state + SetVelocityFormat 切换路径 | `hdr_renderer.h` 前向声明 `enum class VelocityFormat`，加 `SetVelocityDilation/GetVelocityDilation/SetVelocityFormat/GetVelocityFormat`；`hdr_renderer.cpp` State 加 2 字段，`CreateRT` 透传 format，`Init` 同步 backend dilation，`SetVelocityFormat` 走 `ReleaseRT + CreateRT` 重建路径 | ✅ |
| T5 SSRRenderer 透传 + Lua HDR 子表 4 binding | SSRRenderer `DrawSSRTemporal` 调用扩 3 trailing 参数（`backend->GetVelocityDilation/GetVelocityScale/GetActiveVelocityFormat`）；`light_graphics.cpp` 加 4 个 `l_HDR_*` Lua 函数 + 注册到 `hdr_funcs[]`，错误路径用 `nil + err`（避开 MSVC longjmp） | ✅ |
| T6 Smoke + demo HUD | `scripts/smoke/hdr.lua` 加 §8（默认值 / round-trip / bad-arg），10 → 16 functions；`samples/demo_ssr/main.lua` 加 K=Dilation L=Format 按键，HUD 行显示 format/dilation 状态 | ✅ |
| T7 Static verification + CI | `git diff --check` 已通过；`lightc -p` `hdr.lua` + `demo_ssr/main.lua` exit 0；CI run `25892207578` 6/6 success | ✅ |

> 真机视觉验收（dilation 边缘抗锯齿 + RG8 format VRAM 减少）仍待用户在桌面 GL3.3 环境确认。

---

## 2. 关键实现验收

### 2.1 RenderBackend 接口扩展

| 项 | 验收点 | 状态 |
|---|---|---|
| `enum class VelocityFormat : uint8_t { RG16F = 0, RG8 = 1 }` | 定义在 `Phase E.14 — Velocity Buffer Format` 段，C++11 兼容 forward declare | ✅ |
| `CreateHDRFBO` 加 `VelocityFormat velocityFormat = VelocityFormat::RG16F` 默认参数 | 旧调用 100% 兼容 | ✅ |
| `SetVelocityDilation` / `GetVelocityDilation` / `GetVelocityScale` / `GetActiveVelocityFormat` | 默认实现（Legacy 后端 no-op，返合理默认值） | ✅ |
| `DrawSSRTemporal` 加 3 trailing 默认参数 (dilation, scale, format) | 旧调用兼容 | ✅ |

### 2.2 GL33Backend 实现

| 项 | 验收点 | 状态 |
|---|---|---|
| `hdrFboVelocityFormat` map | `CreateHDRFBO` 写入，`DeleteHDRFBO` + `Shutdown` 清理；同步顺手补了 Phase E.13 漏掉的 `hdrFboVelocityTex` 兜底清理 | ✅ |
| `velocityDilation_ = true` 字段 | `SetVelocityDilation` / `GetVelocityDilation` 实现 | ✅ |
| `kVelocityScaleDefault = 0.25f` 静态常量 | `GetVelocityScale` 返常量 | ✅ |
| `activeVelocityFormat_` 字段 | `CreateHDRFBO` 末尾更新；`GetActiveVelocityFormat` 返当前值 | ✅ |
| `CreateHDRFBO` velocity tex 创建分支 | `RG16F` → `(GL_RG16F, GL_RG, GL_FLOAT)`；`RG8` → `(GL_RG8, GL_RG, GL_UNSIGNED_BYTE)` | ✅ |

### 2.3 SSRTemporal shader

| 项 | 验收点 | 状态 |
|---|---|---|
| GLES3 + GL33 两份 source 同步改造 | 都加 3 个新 uniform + 2 个 helper + 主流程改 `prevUV = vUV - SampleVelocityDilated(vUV)` | ✅ |
| `DecodeVelocity` | RG16F = raw；RG8 = `(raw - 0.5) * (2 * scale)` | ✅ |
| `SampleVelocityDilated` | dilation=0 走单点；dilation=1 走 3x3 max-length（dot 长度比较） | ✅ |
| uniform location 缓存 | 3 字段 + Shutdown 重置 | ✅ |
| `DrawSSRTemporal` 上传新 uniform | 3 个 if-loc-valid 上传 | ✅ |

### 2.4 3D shader velocity encode

| Shader 路径 | velocity 写入分支 | 状态 |
|---|---|---|
| FS_UNLIT GLES3 (`render_gl33.cpp:286-319`) | 加 uniform；`if (uVelocityFormat == 1) clamp(raw / (2*s) + 0.5, 0, 1) else raw` | ✅ |
| FS_PBR GLES3 (`render_gl33.cpp:359-466`) | 同上 | ✅ |
| FS_UNLIT GL33 (`render_gl33.cpp:634-666`) | 同上 | ✅ |
| FS_PBR GL33 (`render_gl33.cpp:706-812`) | 同上 | ✅ |
| GPU Skin（VS3D_SKIN）+ FS_UNLIT/FS_PBR | 共用 fragment，自动覆盖 | ✅ |
| GPU Skin Morph（VS3D_SKIN_MORPH）+ FS_UNLIT/FS_PBR | 同上 | ✅ |
| `UploadVelocityUniforms` 上传 `uVelocityFormat` + `uVelocityScale` | `glGetUniformLocation` + `glUniform1i/1f` | ✅ |

### 2.5 HDRRenderer 接入

| 项 | 验收点 | 状态 |
|---|---|---|
| `hdr_renderer.h` 前向声明 `enum class VelocityFormat : uint8_t` | C++11 兼容，避免拖 render_backend.h 进 header | ✅ |
| 4 个 API（`SetVelocityDilation/Get`，`SetVelocityFormat/Get`） | 按 DESIGN §5 落地 | ✅ |
| State `velocityDilation = true` + `velocityFormat = RG16F` | 默认值与 backend 默认对齐 | ✅ |
| `CreateRT` 透传 `g.velocityFormat` 给 backend | 同步 backend dilation 到当前 g.velocityDilation | ✅ |
| `Init` 末尾同步 backend dilation 状态 | 保证 init 后 backend 与 HDR 一致 | ✅ |
| `SetVelocityFormat` 重建路径 | `ReleaseRT + CreateRT`；`g.enabled == false` 时仅更新 state | ✅ |

### 2.6 Lua API（`Light.Graphics.HDR`）

| 函数 | 行为 | 状态 |
|---|---|---|
| `HDR.SetVelocityDilation(bool)` | 非 boolean 入参 → `nil + err`；正常 → 转发 + 返 `true` | ✅ |
| `HDR.GetVelocityDilation()` | 返 boolean | ✅ |
| `HDR.SetVelocityFormat(string)` | `"rg16f"`/`"rg8"` → 转发；其他 → `nil + err`；大小写敏感（`"RG8"` 拒绝） | ✅ |
| `HDR.GetVelocityFormat()` | 返 `"rg16f"` 或 `"rg8"` 规范小写 | ✅ |
| 注册到 `hdr_funcs[]` 末尾（`{NULL, NULL}` 之前） | 12 → 16 functions | ✅ |

### 2.7 Smoke + demo

| 项 | 验收点 | 状态 |
|---|---|---|
| `scripts/smoke/hdr.lua` §8 新段 | 默认值 / round-trip / bad-arg / case-sensitive 共 6 小节，覆盖 4 个新 API | ✅ |
| `fn_names` 列表 12 → 16 | 模块表面检查 | ✅ |
| `samples/demo_ssr/main.lua` 按键 K | toggle dilation；console log 状态 | ✅ |
| `samples/demo_ssr/main.lua` 按键 L | toggle rg16f/rg8（会重建 HDR RT）；console log 状态 | ✅ |
| HUD 新行 | `Velocity: <fmt> | dilation=<ON/OFF> | reproj=<...>` | ✅ |
| HUD keys 行更新 | 加 `K=Dilation L=Format` | ✅ |

---

## 3. 验证清单

| 验证项 | 当前状态 | 说明 |
|---|---|---|
| 源码静态一致性 | ✅ | grep 全仓库确认接口/uniform/format 命名一致；新 uniform 在 program build 后均加入 location 缓存路径 |
| `git diff --check` | ✅ | 仅 LF/CRLF warning，无 whitespace 错误 |
| Lua 语法检查 `lightc -p` | ✅ | `scripts/smoke/hdr.lua` + `samples/demo_ssr/main.lua` exit 0 |
| 本地 CMake build | 🚫 | 按用户偏好不在本地执行 |
| 本地 `light.exe` runtime smoke | 🚫 | 按用户偏好不在本地执行 |
| GitHub Actions 6 平台 build | ✅ | run `25892207578`，6/6 success，耗时 9m17s |
| Windows runtime smoke `hdr.lua` | ✅ | run `25892207578` build-windows success，含 16 functions + §8 段 6 检查 |
| 真实窗口视觉验收 | ⏳ | 用户在桌面 GL3.3 环境对比 demo_ssr K/L 切换前后 |

---

## 4. 与 Phase E.13 兼容性

| 路径 | Phase E.14 行为 |
|---|---|
| `HDR.Enable(w, h)` 旧调用 | 默认 `RG16F + dilation ON`，与 Phase E.13 视觉一致或略好（dilation 减弱边缘 halo） |
| Phase E.12 demo 无 velocity tex 路径 | 不受影响，fallback 到 matrix reproject |
| `mesh:Draw([texOrMat], [prevModel])` | 不受影响；velocity 写入新增 format 分支但不影响接口 |
| Phase AW GPU skin / Phase AX morph | 不受影响（共用 FS，自动获得双格式编码） |
| 旧 `DrawSSRTemporal` 调用方（理论上仅 GL33Backend） | 默认参数兜底，编译/运行兼容 |
| 旧 `CreateHDRFBO` 4-arg 调用 | 默认 `velocityFormat = RG16F` 兜底 |

---

## 5. 关键代码节点引用

| 改动 | 文件 / 行 |
|---|---|
| `VelocityFormat` enum | `ChocoLight/include/render_backend.h:139-146` |
| 4 个新虚接口默认实现 | `ChocoLight/include/render_backend.h:260-271` |
| `CreateHDRFBO` 签名扩展 | `ChocoLight/include/render_backend.h:546-550` |
| `DrawSSRTemporal` 签名扩展 | `ChocoLight/include/render_backend.h:1124-1137` |
| GL33 `hdrFboVelocityFormat` map + 状态字段 | `ChocoLight/src/render_gl33.cpp:2510-2522` |
| GL33 `CreateHDRFBO` RG16F/RG8 分支 | `ChocoLight/src/render_gl33.cpp:3666-3680` |
| GL33 SSRTemporal shader (GLES3) | `ChocoLight/src/render_gl33.cpp:1893-1933` |
| GL33 SSRTemporal shader (GL33) | `ChocoLight/src/render_gl33.cpp:2274-2310` |
| GL33 3D shader Unlit GLES3 velocity 写入 | `ChocoLight/src/render_gl33.cpp:287-319` |
| GL33 3D shader PBR GLES3 velocity 写入 | `ChocoLight/src/render_gl33.cpp:360-466` |
| GL33 3D shader Unlit GL33 velocity 写入 | `ChocoLight/src/render_gl33.cpp:635-666` |
| GL33 3D shader PBR GL33 velocity 写入 | `ChocoLight/src/render_gl33.cpp:707-812` |
| GL33 `UploadVelocityUniforms` uVelocityFormat/Scale | `ChocoLight/src/render_gl33.cpp:2772-2778` |
| GL33 `DrawSSRTemporal` 上传 dilation/format/scale | `ChocoLight/src/render_gl33.cpp:5311-5314` |
| GL33 4 个 override | `ChocoLight/src/render_gl33.cpp:6129-6133` |
| HDRRenderer 4 个 API 实现 | `ChocoLight/src/hdr_renderer.cpp:344-372` |
| HDRRenderer State 字段 | `ChocoLight/src/hdr_renderer.cpp:46-49` |
| SSRRenderer 透传 | `ChocoLight/src/ssr_renderer.cpp:418-431` |
| Lua 4 个 binding | `ChocoLight/src/light_graphics.cpp:1644-1697` |
| Lua HDR 子表注册扩 4 entry | `ChocoLight/src/light_graphics.cpp:1713-1717` |
| smoke §8 | `scripts/smoke/hdr.lua:253-339` |
| demo K/L 按键 | `samples/demo_ssr/main.lua:322-336` |
| demo HUD 新行 | `samples/demo_ssr/main.lua:399-404` |

---

## 6. 验收结论

Phase E.14 的生产代码（C++ + GLSL + Lua）、smoke、demo HUD、文档与静态验证已全部完成。

**最终完成判据**（与 Phase E.12/E.13 对齐）：

- GitHub Actions 6 平台 build success ✅ run `25892207578`
- Windows runtime smoke `hdr.lua` 16 个表面 + 6 段 §8 0 fail ✅ run `25892207578` build-windows success
- 真实窗口环境下，开 dilation 后 Temporal SSR 几何边缘 halo 减弱；切到 RG8 后 VRAM 减少且视觉差异可接受 ⏳ 仍需桌面 GL3.3 手测
