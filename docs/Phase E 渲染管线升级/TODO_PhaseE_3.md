# TODO — Phase E.3 后续事项

> Phase E.3 已完整交付。以下是**留给未来 Phase** 或**需要用户协作**的事项。

---

## 1. 视觉验收（需要用户手动）

| 事项 | 说明 | 建议操作 |
|------|------|----------|
| **demo_hdr 视觉验收** | HDR ON/OFF 对比、Exposure/Gamma 交互、ACES 压缩可见性 | 在 Windows 本地运行 `light.exe samples/demo_hdr/main.lua` 并按 README 操作表验证 |
| **Linux/macOS 本地视觉验收** | CI 仅验证 build，未跑视觉 | 有条件时在两平台各跑一遍 demo_hdr |
| **HDR 与批渲染共存视觉测试** | 同一帧内多 sprite + lit2D 多光源 + HDR 启用 | 扩展 `demo_2d_lighting/main.lua` 加 `HDR.Enable` 按钮 |
| **HDR 与 SetCanvas 混用场景** | 帧内切 HDR → user Canvas → HDR | 构造 Lua 脚本演示多层 RT 嵌套 |

---

## 2. 引擎基础设施缺失

| 事项 | 原因 | 建议解决 |
|------|------|----------|
| **Window resize 回调 → HDR.Resize 自动触发** | 当前 `Light.UI.Window` 没有统一 resize 事件系统 | Phase E.x 或 Phase F 补齐 `Window:OnResize(cb)` 后，HDRRenderer 内部自动接入 |
| **HDR RT readback（截屏用）** | OpenGL `glReadPixels(GL_FLOAT)` 支持 float RT 读回，但 PNG 库不支持 16-bit | 可用 EXR（tinyexr）或 HDR（stb_image） |
| **自动 pixel-diff 视觉回归测试** | 需要黄金图 + readback + diff 工具 | 独立基础设施 phase |
| **Android/iOS GL ES 路径 HDR** | 当前仅 GL33 desktop 实现，GL ES 3.0+ 支持 RGBA16F 但 shader 需 `precision highp float` | 已准备 `tonemapFS_GLES3` shader（见 `render_gl33.cpp`），但 GL ES 后端路径尚未完整接入 |

---

## 3. 功能扩展候选（Phase E.4+）

| 候选 | 价值 | 依赖 |
|------|------|------|
| **Bloom 多级下采样** | HDR 天然受益，视觉提升显著 | Phase E.3（已完成） |
| **Depth of Field** | 3D 场景景深模糊 | 需要正确的 depth buffer（HDR FBO 已有 Depth24） |
| **可选 tonemap operator** | 用户调 `HDR.SetTonemapper("reinhard")` | shader 层多路分支 |
| **sRGB framebuffer 硬件 gamma** | 替代 shader `pow(1/gamma)`，更快 | `glEnable(GL_FRAMEBUFFER_SRGB)` + RGBA16F 兼容性测试 |
| **多 HDR RT（ping-pong）** | 支持多 pass 后处理链（blur 等） | 扩展 `CreateHDRFBO` 返回多 tex |

---

## 4. 文档 / 示例待补

| 事项 | 说明 |
|------|------|
| **API reference 正式更新** | `docs/API_REFERENCE.md` 加 `Light.Graphics.HDR.*` 10 函数条目 |
| **引擎评估文档更新** | `ENGINE_EVALUATION.md` 勾选"HDR + Tonemapping"特性 |
| **性能基准** | HDR ON vs OFF 在 1920×1080 + 不同 Lit 光源数的帧时间对比（视觉验收时顺便记录） |

---

## 5. 需要用户协助的事项

**无**。Phase E.3 所有实现均已自洽，无需用户提供外部资源（区别于 Phase AV 需要 glTF 样本）。

---

## 6. 可直接推进项（若用户希望继续）

按推荐优先级：

1. **Phase E.4 Bloom**（受益最大，复用 HDR RT） ≈ 2-3 天
2. **multiple tonemapper 选项**（低成本功能扩展） ≈ 半天
3. **Android/iOS HDR 完整接入**（跨平台补齐） ≈ 1-2 天
4. **API reference 文档更新**（维护任务） ≈ 半天

---

## 7. 验证脚本（可直接运行）

```powershell
# 本地运行 hdr smoke（headless 验证 API 表面）
cd lumen-master\build\src\light\Release
.\light.exe ..\..\..\..\..\scripts\smoke\hdr.lua

# 本地运行 demo_hdr（需要窗口）
.\light.exe ..\..\..\..\..\samples\demo_hdr\main.lua

# 跟踪 CI
gh run watch  # (选最新 run)

# 查 Phase E.3 所有 commit
git log --oneline 9ce4431^..d6eae5d
```
