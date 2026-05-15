# Phase E.15 Velocity-driven Motion Blur — TODO

> Phase E.15 已实施完毕。本文档记录待办与未来候选。

---

## 1. 必须项（合入前完成）

### 1.1 CI 验证

- ✅ GitHub Actions build run **6/6 success** ([`25894807417`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25894807417), 524s)
- ✅ Windows runtime smoke `motion_blur.lua` **0 fail**
- ✅ 全 lua 语法 syntax check 通过（Linux/macOS for-loop）
- ✅ run ID 已填入 `FINAL_PhaseE_15.md` § 5

### 1.2 真机视觉验收（用户侧）

- ⏳ 桌面 GL3.3 跑 `samples/demo_ssr/main.lua`
- ⏳ 按 **M** 切换 MotionBlur ON/OFF：静止时无 blur 伪影
- ⏳ 移动相机（旋转视角）时可见方向性拖尾
- ⏳ 物体（cube）相对相机平移时其方向性 blur
- ⏳ 切换 RG16F ↔ RG8（按 L）：MotionBlur 长度近似一致
- ⏳ 关闭 dilation（按 K）：物体边缘 blur 撕裂略增
- ⏳ 调高 SampleCount 至 32 + Strength 至 4：观察对角线 30% 软限是否生效（不糊死）

---

## 2. 建议项（不阻塞合入）

### 2.1 文档增量

- ✅ `docs/api/Light_Graphics.md`：MotionBlur 子表段（11 API + 用法表）
- ⏳ `docs/Phase E.14 Velocity Dilation RG8/TODO_PhaseE_14.md` §3 候选清单标 motion blur → Phase E.15 已完成
- 💡 可选：在 `docs/PROJECT_SUMMARY.md` Phase 表格追加 Phase E.15 一行

### 2.2 用户可调进一步收紧（可选）

- 💡 `MotionBlur.SetStrength` 当前 clamp [0, 4]；如真机评估觉得 4.0 不够剧烈或太剧烈，可调整
- 💡 `MotionBlur.SetSampleCount` clamp [1, 32]；如真机性能允许，可放宽到 64
- 💡 `HDR.SetVelocityScale(float)` Lua API：Phase E.14 TODO §2.2 建议项；同时受益 SSR Temporal 和 Phase E.15 MotionBlur

### 2.3 性能仪表（可选）

- 💡 加 `MotionBlur.GetLastDrawTime() -> number ms`：让用户监控本帧 motion blur 开销
- 💡 加 `MotionBlur.GetVRAMBytes() -> integer`：让用户监控显存占用

---

## 3. 后续阶段候选（不在本期范围）

### 3.1 Phase E.15.1（短期，半天）

- **HDR.SetVelocityScale Lua API**：Phase E.14 TODO §2.2 同款建议项，受益 SSR Temporal + Phase E.15
- 增加 `kVelocityScaleMin / kVelocityScaleMax` 常量；clamp [0.05, 1.0]

### 3.2 Phase E.16 候选（中期，1-2 天）— ✅ 已完成

- **Camera-only motion blur 模式** — ✅ Phase E.16 已完成（CI run [25896826324](https://github.com/futzhj/ChocoLightEngine/actions/runs/25896826324) 6/6 green，commit `46cd329`）
- 实施回顾：双 RT 方案（A1），HDR FBO MRT slot 3 = cameraVelocityTex，3D shader VS 同时写两路；MotionBlur shader 内 mode 切换，object_only = combined − camera
- API：`Light.Graphics.MotionBlur.SetMode(0/1/2)` / `GetMode()`，默认 0 = combined 完全兼容
- 详见 `@e:/jinyiNew/Light/docs/Phase E.16 Camera-only Motion Blur/FINAL_PhaseE_16.md`

### 3.3 Phase E.17 候选（中期，1 天）

- **1/2 分辨率 motion blur**：移动端性能优化
- motionBlurTex 改 (w/2, h/2)；Pass2 用 bilinear filter 上采样
- 视觉质量损失 ≤ 5%，性能 ≈ 4×

### 3.4 Phase E.18 候选（短期，半天）

- **Independent velocity dilation pass**
- 当 motion blur + SSR Temporal 都开启时，dilation 计算重复（两个 module 各算一次）
- 提前 dilation pass 输出 dilatedVelocityTex 共享
- 节省 ~0.2 ms @ 1080p（多消费者场景才有意义）

### 3.5 Phase F.x 远期

- **Velocity-driven TAA**：anti-aliasing 利用 velocity 做 temporal 累积
- **MDFG 2012 motion blur**：tile-max + reconstruction filter（AAA 级别质量）
- **Stochastic motion blur**：dither + 蒙特卡洛采样减少 banding

---

## 4. 已知遗留问题

| 项 | 描述 | 影响 | 处置 |
|---|------|------|------|
| 2D sprite 在 HDR 模式下也被 motion blur | 与设计一致（HDR scene 包含 2D） | 用户期望 | 不修，文档说明 |
| SetStrength 类型错 → Lua 抛错（不返 nil+err） | 与 Bloom 一致 | 一致性 | 不修 |
| Lumen `lua_pushinteger` 在某些版本可能返 number | smoke 已用 `math.floor + 0.5` 兼容 | 不影响 | 不修 |
| shader 内 const-bound for loop max 32 | 兼容 GL3.3 | 性能极小 | 不修，符合 spec |

---

## 5. 状态总览

| 类别 | 数量 | 进度 |
|------|------|------|
| **必须项** | 8 | ⏳ 待 CI + 真机 |
| **建议项** | 6 | 2/6 完成（文档），4 待评估 |
| **后续候选** | 5 phase | 0/5 启动 |
| **已知遗留** | 4 | 4/4 不修 |

Phase E.15 工程主线已闭环。真机验收 + CI 双 ✅ 后正式关闭 phase。
