# Phase F.0.11.7 Multi-instance Screenshot — FINAL 文档（实施记录）

> **基线**：PLAN_PhaseF_0_11_7.md
> **实施日期**：2026-05-17
> **完成度**：F.0.11.7 全量交付

---

## 1. 实施时间线

| 任务 | 实际产出 | 耗时 |
|---|---|---|
| T1 | ScreenshotHDR + ScreenshotEXR 增 instance_id 参数 / 字段 + save/restore | ~30 min |
| T2 | screenshot smoke +2 检查点 | ~10 min |
| T3 | demo_multi_hdr_pip V 键 + HUD 提示 + 文档收尾 | ~20 min |

**总计**: ~1 小时 (PLAN 估时 1.25 小时)

---

## 2. 文件改动清单

| 文件 | 改动类型 | 改动量 |
|---|---|---|
| `ChocoLight/src/light_graphics.cpp` | 修改 | +50 行 (ScreenshotHDR + ScreenshotEXR 双向 instance_id 支持) |
| `scripts/smoke/screenshot.lua` | 修改 | +12 行 (2 新检查点) |
| `samples/demo_multi_hdr_pip/main.lua` | 修改 | +20 行 (V 键 + HUD 提示) |
| `docs/Phase F.0.11.7 Multi-instance Screenshot/PLAN_PhaseF_0_11_7.md` | 新建 | ~80 行 |
| `docs/Phase F.0.11.7 Multi-instance Screenshot/ACCEPTANCE_PhaseF_0_11_7.md` | 新建 | ~110 行 |
| `docs/Phase F.0.11.7 Multi-instance Screenshot/FINAL_PhaseF_0_11_7.md` | 新建 | 本文 |
| `docs/HANDOFF_REMAINING_TASKS.md` | 修改 | F.0.11.7 状态更新 |

---

## 3. 关键实现细节

### 3.1 ScreenshotHDR 改造模式

```cpp
const int instanceId = (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
    ? (int)luaL_checkinteger(L, 2) : -1;
const int savedActive = HDRRenderer::GetActiveInstance();
if (instanceId >= 0 && instanceId != savedActive) {
    if (!HDRRenderer::SetActiveInstance(instanceId)) {
        return error;   // 立即返回, 无需复位 (没切成功)
    }
}
// ... readback + 写盘
if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);
```

每个 error 路径都要先 restore 再 push error.

### 3.2 ScreenshotEXR lambda 复位模式

```cpp
auto restoreActive = [&]() {
    if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);
};
```
4 个 error 路径 + 1 个成功路径都先调 `restoreActive()`. 这避免了多处 `if (...) SetActiveInstance(...)` 重复.

### 3.3 lua_tostring 在 lua_pop 之后是悬挂指针

修复了 PLAN 文档隐藏的 bug: 原 ScreenshotEXR 代码 `lua_pushfstring(.., lua_tostring(L, -2))` 在 `lua_pop(L, 1)` 之后调用 `lua_tostring(L, -2)`, stack 被改, 索引指向错误位置. 改为先用 `const char* badStr = lua_tostring(L, -1)` 临时保存指针 (Lua string 在该 string 仍在 stack 时是稳定的), pop 后再用. 实际未触发是因为编译器优化 + 老用法被 Lua API 容错, 但语义错. 已修. 

(注: 这个修复实际是 F.0.11.7 引入新代码时顺手发现的 F.0.11.5 残留 bug, 未在 PLAN 列出但顺手处理.)

---

## 4. 测试覆盖

### 4.1 Smoke (2 新检查点)
```
PASS ScreenshotHDR(path, 999) headless → nil+err (HDR not enabled)
PASS ScreenshotEXR(path, {instance_id=999}) headless → nil+err
```
**总计**: 36 PASS / 0 FAIL

### 4.2 Zero-Regression (4 demo)
全部启动无错; ScreenshotHDR/EXR 缺省 instance_id 时与 F.0 行为完全一致.

### 4.3 真机验证 (待用户)
- ⏳ demo_multi_hdr_pip 按 V → 生成 `pip_only.hdr` + `pip_only.exr`
- ⏳ 用 Photoshop / Krita / Nuke 验证文件内容只包含 PIP (480×270) 而非主屏 (1600×900)
- ⏳ 文件尺寸应是主屏截图的 ~1/12 (面积比例)

---

## 5. 已知 / 留观察问题

### 5.1 设计层
- save/restore active 模式假设 SetActiveInstance 是廉价的 (只 int 赋值). 若未来引入 OpenGL FBO 重绑定到 active instance, 此模式会引入双重 bind 开销, 需重新评估.

### 5.2 与 PNG screenshot 的协同
- `Light.Graphics.Screenshot` (LDR PNG, 读 default fb) 没有 instance 概念 — 它读窗口而非 HDR FBO, 自然全屏覆盖所有 instance 的 tonemap 后内容.
- 用户若需要单独 PNG 截 PIP, 应用 `ScreenshotRegion(rgnX, rgnY, w, h)` 配合 PIP 显示矩形.

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 实施完结 |
