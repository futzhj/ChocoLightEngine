# Phase F.0.11.7 Multi-instance Screenshot — ACCEPTANCE 文档

> **阶段**：6A Workflow — 阶段 5 Acceptance（验收）
> **基线**：PLAN_PhaseF_0_11_7.md
> **实施记录**：FINAL_PhaseF_0_11_7.md
> **验收日期**：2026-05-17

---

## 1. 功能验收

### 1.1 ScreenshotHDR API 扩展
- [x] `Light.Graphics.ScreenshotHDR(path, instance_id?)` — 第 2 个可选 integer 参数
- [x] `instance_id < 0` / nil / 缺省 → 使用 active instance (F.0 行为零回归)
- [x] 临时切 instance 后保证复位 (含错误路径)
- [x] 无效 instance_id (未分配 / 越界) → 返 nil + err

### 1.2 ScreenshotEXR API 扩展
- [x] opts 表新增 `instance_id` 字段
- [x] 与 bit_depth / compression 字段共存
- [x] save/restore active 全错误路径覆盖 (readback 失败 / SaveEXR 失败 / 无效 sceneTex 都能正确复位)

### 1.3 Smoke 增量 (2 检查点)
- [x] `ScreenshotHDR(path, 999)` 在 HDR 未启用 headless 下返 nil + err
- [x] `ScreenshotEXR(path, {instance_id=999})` 同上

**Smoke 结果**: **36 PASS / 0 FAIL** (旧 34 + 2 新)

### 1.4 Demo 集成
- [x] `demo_multi_hdr_pip` 增 **V 键**: 同时调用 `ScreenshotHDR(.., g_pipId)` + `ScreenshotEXR(.., {instance_id=g_pipId})` 演示 PIP 单独截图
- [x] HUD 键位提示更新

---

## 2. 兼容性验收 (零回归)

| Demo | 启动 | warn/error/fail/undef |
|---|---|---|
| `demo_ssr` | ✅ | 0 |
| `demo_taa_split2` | ✅ | 0 |
| `demo_taau` | ✅ | 0 |
| `demo_multi_hdr_pip` | ✅ | 0 |

| Smoke | PASS | FAIL |
|---|---|---|
| TAA smoke | 171 | 0 |
| Screenshot smoke | 36 (+2 新) | 0 |

---

## 3. 设计决策回顾

### 3.1 不引入 HDRRenderer::GetSceneTextureFor(int id) 等新 API

如果引入这类 instance-scoped 查询, 需要 4-5 个新方法 (GetSceneTextureFor / GetWidthFor / GetHeightFor / IsEnabledFor 等). 改造成本大. 直接在 Lua bridge save/restore active:
- 仅 2 个调用点 (ScreenshotHDR + ScreenshotEXR), 模式一致
- HDRRenderer 内部 g_active 切换是廉价的 (只是 int 赋值)
- save/restore 模式与 demo_taa_split2 已有的 SetActiveInstance 用法一致

### 3.2 RAII 风格 restoreActive lambda (ScreenshotEXR)

ScreenshotEXR 有 4 个错误返回点 (invalid scene / readback fail / SaveEXR fail / success). 每处都需复位 active. 用 lambda + 显式调用避免漏复位:
```cpp
auto restoreActive = [&]() {
    if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);
};
```
更复杂的 RAII guard 类不必要 (本函数生命周期短, 不抛异常).

### 3.3 ScreenshotHDR 用直接 if 复位 vs lambda

ScreenshotHDR 只有 3 个错误返回点 + 1 个成功路径, 用直接 inline `if (...) HDRRenderer::SetActiveInstance(savedActive);` 更短, 对应一一直接.

### 3.4 instance_id < 0 当作"不切"

允许显式传 `instance_id = -1` 或 nil 表示 "用当前 active". Lua 用户代码可写:
```lua
local id = condition and pipId or nil
Gfx.ScreenshotHDR(path, id)   -- nil 时透传 active
```

---

## 4. 文档验收

- [x] `docs/Phase F.0.11.7 Multi-instance Screenshot/PLAN_PhaseF_0_11_7.md`
- [x] `docs/Phase F.0.11.7 Multi-instance Screenshot/ACCEPTANCE_PhaseF_0_11_7.md` (本文)
- [x] `docs/Phase F.0.11.7 Multi-instance Screenshot/FINAL_PhaseF_0_11_7.md`
- [x] `docs/HANDOFF_REMAINING_TASKS.md` 更新

---

## 5. 验收结论

**核心交付**: ScreenshotHDR/EXR 增 instance_id 参数 + save/restore active + 错误路径全覆盖 + 2 smoke + demo_multi_hdr_pip V 键集成.

**验收级别**:
- ✅ **代码层**: PASS (Release build clean, smoke 36/0)
- ✅ **兼容性**: PASS (4 demo 全零回归, 默认/缺省 instance_id 行为完全等同 F.0)
- ⏳ **真机验证**: 待用户在 demo_multi_hdr_pip 按 V 生成 `pip_only.hdr` + `pip_only.exr` 验证只含 PIP 内容

**结论**: F.0.11.7 **代码层通过验收**.

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 验收提交 |
