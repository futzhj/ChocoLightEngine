# Phase E.12 Temporal SSR — TODO（待办与外部依赖）

> **目的**：精简列出 Phase E.12 交付后仍需用户 / CI 环境确认的事项。
> **状态约定**：🔴 阻塞 ｜ 🟡 推荐 ｜ 🟢 可选

---

## 1. 立即待办（收官前）

### 1.1 ✅ 执行轻量 Lua 语法检查

- **范围**：`scripts/smoke/ssr.lua`、`samples/demo_ssr/main.lua`
- **说明**：按项目偏好，本地只做 Lua 语法检查与补丁静态检查，不执行 CMake build / runtime smoke
- **结果**：`lightc -p` 两个 Lua 脚本通过，`git diff --check` 通过

### 1.2 ✅ CI 结果

- **结果**：GitHub Actions run `25871544298`，6/6 平台 success
- **平台**：Windows / Linux / macOS / Android / iOS / Web
- **关注点**：
  - `RenderBackend::DrawSSR` 签名变更是否全调用点同步 ✅
  - GLES3 / GL33 两份 `FS_SSR_TEMPORAL` shader 是否编译通过 ✅
  - Windows runtime smoke 是否识别 SSR API 34/34 ✅

### 1.3 ✅ Windows runtime smoke

- **脚本**：`scripts/smoke/ssr.lua`
- **期望**：SSR surface 34 functions；Temporal 默认值、round-trip、clamp、联动测试全部 PASS
- **结果**：run `25871544298` 的 `build-windows` success

---

## 2. 真实窗口视觉验证

### 2.1 🟢 demo_ssr 手测

- **脚本**：`samples/demo_ssr/main.lua`
- **按键**：
  - `T`：Temporal on/off
  - `U/I`：降低 / 提高 TemporalAlpha
  - `N`：切换 RejectionMode
  - `B`：Blur on/off
  - `V`：Bilateral on/off
  - `R`：恢复默认
- **观察点**：
  - Temporal on 后反射噪点应随帧数下降
  - 快速移动相机时不应出现明显黑帧
  - RejectionMode=1 应比 mode=0 更少 ghost / smear
  - Blur + Temporal 同时开启时不应出现 RT 尺寸错乱

### 2.2 🟢 性能基线

- **建议场景**：1920×1080，默认 SSR 64 steps，Temporal on，Blur off / on 分别测
- **关注指标**：
  - full-res history × 2 VRAM 增量
  - Temporal pass GPU cost
  - 移动端 GLES3 shader 编译和运行成本

---

## 3. 后续增强候选

### 3.1 🟢 Motion vector / velocity buffer

- **现状**：Phase E.12 使用 depth-only reverse reprojection
- **问题**：动态物体、快速相机运动可能 ghost
- **方向**：引入 per-pixel motion vector G-buffer，Temporal pass 用 velocity reproject
- **成本**：需要改 HDR MRT / 材质 shader / backend texture layout

### 3.2 🟢 History-depth rejection mode

- **现状**：`RejectionMode=0` 已提供 current-depth threshold rejection，`RejectionMode=1` 为默认 neighborhood clip
- **方向**：如需更严格的 depth-only，可额外保存上一帧 depth history，并在 prevUV 处比较历史 depth
- **依赖**：需要 history depth 或更完整的 depth reprojection 策略

### 3.3 🟢 Roughness-aware Temporal / Blur

- **现状**：TemporalAlpha 与 BlurRadius 为全局参数
- **方向**：按 PBR roughness 调整 history 权重和 blur 半径
- **依赖**：材质 roughness G-buffer 或可采样材质属性

### 3.4 🟢 视觉自动回归

- **现状**：CI 只验证 API / smoke，不验证画面
- **方向**：headless GL 截图 + perceptual diff
- **依赖**：稳定渲染环境、基准图、容差策略

---

## 4. 已知技术债务（不阻塞）

| 债务 | 影响 | 优先级 |
|------|------|--------|
| 无 velocity buffer | 动态物体 ghost 风险 | 中 |
| full-res history 固定开启 | 移动端 VRAM 压力 | 中 |
| `RejectionMode=0` 仍偏预留 | A/B 模式语义不够完整 | 低 |
| TemporalAlpha 全局统一 | 不同材质响应无法差异化 | 低 |
| 无视觉自动化测试 | 画质回归依赖人工 | 低 |

---

## 5. 外部依赖与配置

**无新增外部依赖**：

- ✅ 不需要第三方库
- ✅ 不需要 API key / `.env`
- ✅ 不需要修改 CMake 配置
- ✅ 不需要新增 CI workflow

**继承依赖**：

- 桌面：OpenGL 3.3 Core
- 移动 / Web：OpenGL ES 3.0 / WebGL2 等价能力
- HDR MRT normal：继承 Phase E.8.x G-buffer normal

---

## 6. 用户下一步建议

1. **真实窗口打开 demo_ssr**：重点比较 `T` 开关前后噪声与 ghost。
2. **若视觉可接受**：Phase E.12 可标记完成。
3. **若视觉 ghost 明显**：优先考虑 Phase E.13 motion vector / velocity buffer。
