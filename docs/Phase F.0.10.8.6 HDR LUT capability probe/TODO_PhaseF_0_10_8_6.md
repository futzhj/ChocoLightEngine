# Phase F.0.10.8.6 — HDR LUT 能力探测 TODO

---

## 1. 强制

### 1.1 ⏳ CI 6/6 验证

**等**: GitHub Actions 完成 6 平台.

**风险**: **极低**. 单一 bool 透传 fn, 模式与现有 `SupportsBloom()` 完全一致. GL_RGB16F + 3D texture 在 GL3.3 / GLES3 / WebGL2 / iOS GL 均支持, gl33 override 一律 return true 安全.

**用户操作**: 检查 [GitHub Actions](https://github.com/futzhj/futzhj/ChocoLightEngine/actions)

---

## 2. 可选

无.

F.0.10.8 LUT 子生态在 F.0.10.8.6 完整闭环, 13 sub-phase 工业级覆盖.

---

## 3. 用户支持

### 3.1 何时调 SupportsHDRLUT

- **启动时**: 启动后调一次, 缓存结果决定提供 LDR 还是 HDR LUT 资源
- **UI 渲染时**: 显示 "HDR LUT ✓" / "LDR fallback" 徽章

### 3.2 注意

- backend 未初始化时返 false (HDR.IsSupported 也是 false)
- 即使返 false, `LoadCubeLUT(hdr_cube_path)` 仍可用 (自动 quantize fallback)
- 不是 "GPU 是否 GL_RGB16F" 检测; 是 "当前 backend 实现是否启用 HDR LUT 路径"

### 3.3 一行决策

```lua
local cube = HDR.SupportsHDRLUT() and 'aces_hdr.cube' or 'portrait_ldr.cube'
HDR.SetGradingLUT(HDR.LoadCubeLUT(cube), 1.0)
```

---

## 4. 文档导航

| 文档 | 用途 |
|------|------|
| `PLAN_PhaseF_0_10_8_6.md` | 6A 极简 (1 doc, ~50 行) |
| `FINAL_PhaseF_0_10_8_6.md` | 项目总结 + LUT 子生态最终态图 |
| **`TODO_PhaseF_0_10_8_6.md`** | 本文件 |
