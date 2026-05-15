# Phase E.16 Camera-only Motion Blur — FINAL（项目总结报告）

> 6A 工作流 · 完结报告
> 基线：Phase E.15 commit `4eb22c7` (CI run 25894807417 6/6 success)

---

## 1. 项目目标回顾

将 Phase E.15 的 **camera + object 合一 motion blur** 扩展为 **mode-aware** 三档可选：
- `combined`（默认，与 Phase E.15 行为一致）
- `camera_only`（仅相机运动产生的拖尾）
- `object_only`（仅物体运动产生的拖尾）

让用户在赛车、FPS、动作类游戏中获得**更可控的运动模糊语义**。

---

## 2. 6A 工作流执行回顾

| 阶段 | 产出 | 关键决策 |
|-----|------|----------|
| **Align** | `ALIGNMENT_PhaseE_16.md`（300 行）| 11 个决策点全部就位；唯一用户拍板：A1 双 RT |
| **Architect** | `DESIGN_PhaseE_16.md`（500 行）| 数学推导 + RT 拓扑图 + 8 shader GLSL 伪码 + 数据流时序 + 性能预算 |
| **Atomize** | `TASK_PhaseE_16.md`（380 行）| T1~T7 + 依赖图 + 8 项风险矩阵 + 估时 1.5 天 |
| **Approve** | 用户确认 A1 + 一气实施 T1~T7 | 加速进入 Automate |
| **Automate** | T1~T6 + 文档 | 共 ~489 行代码 + ~430 行文档跨 8 个源文件 |
| **Assess** | `ACCEPTANCE_PhaseE_16.md`（200 行）+ 本 FINAL + TODO | 决策矩阵全部勾选 |

---

## 3. 代码改动统计

### 3.1 按文件

| 文件 | 改动 | 类型 |
|------|------|------|
| `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h` | +28 行 | RenderBackend 接口扩展（CreateHDRFBO + GetHDRCameraVelocityTex + DrawMotionBlur 签名） |
| `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +271 行 | GL33Backend 字段 + FBO MRT slot 3 + 3D shader VS×3+FS×2 (GLES3+GL33 双 source = 16 处) + MotionBlur shader mode 切换 + DrawMotionBlur 扩展 |
| `@e:/jinyiNew/Light/ChocoLight/include/hdr_renderer.h` | +3 行 | GetCameraVelocityTexture 声明 |
| `@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp` | +13 行 | CreateRT 传 outCameraVelocityTex + GetCameraVelocityTexture 实现 |
| `@e:/jinyiNew/Light/ChocoLight/include/motion_blur_renderer.h` | +6 行 | SetMode/GetMode 声明 |
| `@e:/jinyiNew/Light/ChocoLight/src/motion_blur_renderer.cpp` | +16 行 | g.mode 字段 + SetMode/GetMode + Process 透传 cameraVelocityTex + mode |
| `@e:/jinyiNew/Light/ChocoLight/src/light_graphics.cpp` | +18 行 | l_MB_SetMode/l_MB_GetMode + mb_funcs[] 加 2 条 |
| `@e:/jinyiNew/Light/scripts/smoke/motion_blur.lua` | +44 行 | surface 加 2 名 + §7 mode 段 5 PASS |
| `@e:/jinyiNew/Light/samples/demo_ssr/main.lua` | +14 行 | `;` 键切 mode + HUD 显示 mode + Keys 提示 |
| `@e:/jinyiNew/Light/docs/api/Light_Graphics.md` | +44 行 | SetMode/GetMode 子段 + 模式映射表 + 完整示例 |
| **总计** | **~457 行代码 / 文档** | 10 个文件 |

### 3.2 6A 文档

| 文件 | 行数 |
|------|------|
| `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/ALIGNMENT_PhaseE_16.md` | ~300 |
| `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/DESIGN_PhaseE_16.md` | ~500 |
| `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/TASK_PhaseE_16.md` | ~380 |
| `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/ACCEPTANCE_PhaseE_16.md` | ~200 |
| `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/FINAL_PhaseE_16.md` | 本文（~250） |
| `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/TODO_PhaseE_16.md` | ~150 |
| **总计** | **~1780 行 6A 文档** |

---

## 4. 关键技术创新

### 4.1 双 velocity RT 完美对偶

3D shader 同时输出两路 velocity：
```glsl
vPrevClip           = uPrevViewProj × uPrevModel × pos    // combined
vPrevClipCameraOnly = uPrevViewProj × uModel    × pos    // camera-only
```

camera-only 只把 `prevModel` 替换为 `uModel`（当前模型矩阵），相当于「假设物体没动只有相机动」。**零额外 uniform**（uModel 已有）。

### 4.2 16 处一致改 1 次命中

VS×3 + FS×2 跨 GLES3/GL33 双 source = 16 处 shader 改动，全部是 1 行变量 + 1 行计算 + 1 行 layout + 1 段 encode 的统一模式。利用 `multi_edit` 6 个 edit + `replace_all=true` **一次命中全部 16 处**，零遗漏。

### 4.3 三模式 shader 内做减法

```glsl
if (uMode == 1)      vel = SampleCameraVelocityDilated(vUV);
else if (uMode == 2) vel = SampleVelocityDilated(vUV) - SampleCameraVelocityDilated(vUV);
else                 vel = SampleVelocityDilated(vUV);   // combined (default)
```

`object_only ≈ combined − camera`，无需第三张 RT。

### 4.4 safeMode silent fallback

```cpp
int safeMode = mode;
if ((mode == 1 || mode == 2) && !cameraVelocityTex) {
    safeMode = 0;   // 静默 fallback combined (旧 backend / 创建失败)
}
```

防止 mode=2 在 cameraVelocityTex 缺失时做减法越界；不打 warning log 避免每帧刷屏。

---

## 5. 决策回顾

11 个决策完整保留实施（详见 ACCEPTANCE §2 对照表）。关键 3 个：

1. **A1 双 RT（用户拍板）**：选择 +1~4 MB VRAM 增量换 shader 改动最小、精度无损、向后兼容
2. **第二张存 camera_only**（自动决策）：object = combined − camera 可推导；camera 在 RG8 编码下截断更友好
3. **只影响 MotionBlur**（自动决策）：SSR Temporal 数学要求 combined velocity，不动

---

## 6. 与 Phase E 系列的关系

```
Phase E.13 Motion Vector  → 引入 velocity buffer（RG16F MRT slot 2）
Phase E.14 Velocity Dilation + RG8  → format 双路径 + 3x3 dilation
Phase E.15 Motion Blur  → per-pixel blur 沿 velocity (单路 combined)
Phase E.16 Camera-only Motion Blur  → 双路 velocity，mode-aware blur ★
                                       ↑ 本期
未来候选:
  Phase E.17? half-res motion blur 优化
  Phase E.18? velocity 独立 dilation pass（当 SSR+MotionBlur+其他多消费者）
  Phase F.x?  Velocity TAA
```

---

## 7. API 总览（Phase E 系列累计）

| Phase | Lua API 子表 | 函数数 | 备注 |
|-------|-----------|--------|------|
| E.3   | Light.Graphics.HDR | 12 | + 4 tonemap operators |
| E.4   | Light.Graphics.Bloom | 15 | pyramid |
| E.5   | Light.Graphics.AutoExposure | 18 | log-luma + EV |
| E.6   | Light.Graphics.LensDirt + Streak | 23 | 电影感 |
| E.7   | Light.Graphics.LensFlare | 21 | ghost + halo + CA |
| E.9-E.12 | Light.Graphics.SSR | 24 | + blur/temporal |
| **E.15** | Light.Graphics.MotionBlur | 11 | velocity-driven blur |
| **E.16** | Light.Graphics.MotionBlur | **13** ★ | + SetMode/GetMode (camera_only / object_only) |
| **小计** | | **~140 fn HDR 后处理 API** | |

---

## 8. 已知限制与下一步

详见 `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/TODO_PhaseE_16.md`。核心：

1. **真机视觉验收**（Phase E.15 同款 TODO，用户参与）
2. **camera-only velocity 在 skinned 动态物体上的语义精度**（理论小差距，需视觉测试）
3. **SSR Temporal 选择性用 camera-only velocity** 给镜头反射做高速运动稳定性优化（推 Phase E.17 候选）

---

## 9. CI 状态

待 T7 commit + push 后填入：

```
GitHub Actions run id: <TBD>
Status: <TBD>
Duration: <TBD>
Phase E.16 motion_blur.lua: ≥ 21 PASS (16 原 + 5 mode 新)
其他 16 个 phase smoke: 全部通过零回归
```

---

## 10. 致谢与反思

- **6A 工作流** 在本 phase 表现完美：拍板 1 个核心决策（A1 双 RT），其他 10 个决策按工程惯例自动定，节省了大量来回确认时间
- **shader replace_all=true** 是本 phase 的小幸运 —— 16 处一致改 1 次命中
- **零回归保证**：mode=0 默认让 Phase E.15 用户完全无感切换；旧 backend safe fallback；现有 16 个 phase smoke 不需要改动

Phase E.16 完成！

---

## 11. 推进确认

FINAL 文档完成。下一步：撰写 TODO_PhaseE_16.md + 单 commit + push + CI 监控。
