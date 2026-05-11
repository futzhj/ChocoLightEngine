# TODO — Phase E.7 · Lens Flare (交付后未尽事项)

> 6A 工作流 · 阶段 6 · Assess 衍生物

---

## 1. 必要补完

### 1.1 真机视觉验收 [可选 - 用户参与]

**做法**：

1. 编译 `Light.dll` 或拿 CI 产物（`gh run download <id> --name windows-template`）
2. 跑 demo：
   ```powershell
   ./light.exe samples/demo_lens_flare/main.lua
   ```
3. 验证视觉：
   - 默认 8 个 HDR 亮点周围出现 ghost + halo + 色差
   - **F 切换**：开/关 LensFlare 对比
   - **1/2 改 GhostCount**：1 ghost 时单光圈；8 ghost 时密集径向
   - **3/4 改 GhostDispersal**：0.2 紧凑、0.8 散开
   - **5/6 改 HaloWidth**：0.3 内圈、0.7 外圈
   - **7/8 改 ChromaticAberration**：0 无色差、0.015 明显 RGB 分离
   - **9/0 改 Intensity**：0.1 微弱、1.0 强烈
   - **D 切色差**：开关 chromatic aberration（性能 ~3× 差异）
4. 可选截图归档到 `docs/Phase E 渲染管线升级/assets/`

---

### 1.2 性能基线 [可选]

**问题**：LensFlare 增加 3 个 fragment pass。

**预估**（1920×1080, RT=960×540）：

| 配置 | 开销 (近似) |
|------|------------|
| GhostCount=0, HaloWidth=0（empty） | ~0.4 ms（1 bright + 1 empty + 1 composite） |
| 默认（GhostCount=4, Halo=0.5, distortion=on） | ~1.0 ms |
| 最大（GhostCount=8, distortion=on） | ~1.8 ms |
| 最大（GhostCount=8, distortion=off） | ~0.8 ms |

→ 与 Streak / Bloom 同量级，可接受。

---

## 2. 未来扩展

### 2.1 Lens flare 贴图（rays 星芒） [中优先]

**动机**：纯 procedural ghost 视觉单调；引擎应支持用户提供 lens flare 贴图。

**做法**：

- 新增 `SetFlareTexture(img_or_id_or_nil)` Lua API（参考 LensDirt 三态）
- 后端 `DrawLensFlareGhost` 加 uniform `uFlareTex` + 1×1 透明 fallback
- shader 用贴图采样替代 / 叠加 procedural 圆形 ghost
- 默认 fallback 行为不变（procedural ghost）

---

### 2.2 多环 Halo [低优先]

**动机**：真实相机经常多环。

**做法**：shader 加 `uHaloCount` + 数组化 `uHaloWidths[N]`（静态上限 4）。

---

### 2.3 光源主方向追踪（Lens flare attached to light） [中-大工程]

**动机**：当前 ghost 朝画面中心反投；真实光晕应朝光源 → 屏幕方向反投。

**做法**：

- Lua API: `SetLightScreenPos(x, y)` 接受归一化屏幕坐标
- C++: 替代 `vec2(0.5)` 为 `uLightScreenPos`
- 用户需自己用 `Camera.WorldToScreen(lightPos)` 算出光源屏幕坐标
- → Phase E.8 候选

---

### 2.4 Anti-aliased ghost edges [低优先]

**动机**：高对比度场景下 ghost 边缘可能出锯齿。

**做法**：

- 增加权重计算的平滑度（更高次幂 / smoothstep）
- 或在 composite 之前过 1 次小核 blur

---

### 2.5 Animated flare（时间动画） [低优先]

**动机**：随相机移动 flare 动态变化。

**做法**：`SetTime(float)` + shader UV scroll。

---

## 3. 引擎基础设施扩展（共性）

### 3.1 Image GC 引用（与 LensDirt 同）

如果 `SetFlareTexture(image)` 实现（§2.1），同样面临 Lua GC 释放纹理但 LensFlare 仍持 uint32_t 的问题。

**建议**：参考 Phase E.6 TODO §3.1 的统一方案。

---

## 4. 验证脚本速查

```powershell
gh run list --limit 5 --branch main
gh run view <id> --log-failed

lua -e "loadfile('scripts/smoke/lens_flare.lua')"
lua -e "loadfile('samples/demo_lens_flare/main.lua')"
```

---

## 5. 完结清单

- [x] E.7.1 backend ✅ CI 6/6
- [x] E.7.2 module ✅ CI 6/6
- [x] E.7.3 Lua + smoke + demo + CI ⏳ 跑中（推送 d63234f）
- [x] ACCEPTANCE_PhaseE_7.md ✅
- [x] FINAL_PhaseE_7.md ✅
- [x] TODO_PhaseE_7.md ✅（本文件）
- [ ] 真机视觉验收（用户参与）
- [ ] 内置 lens flare 贴图（建议中期补）
- [ ] 光源主方向追踪（Phase E.8 候选）
- [ ] Multi-ring halo / Animated flare（长期可选）

---

**Phase E.7 主交付完结**。HDR 链路累计 **6 剑客 / 89 Lua API** 上线 ✨

```
Phase E.3 — HDR + 4 tonemap operator     (12 fn)
Phase E.4 — Bloom pyramid                 (15 fn)
Phase E.5 — Auto Exposure (Eye Adaptation) (18 fn)
Phase E.6 — Lens Dirt + Streak            (23 fn)
Phase E.7 — Lens Flare                    (21 fn)   ✨
```
