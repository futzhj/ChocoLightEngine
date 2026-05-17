# Phase F.0.11.7 Multi-instance Screenshot — PLAN 文档

> **阶段**：6A Workflow 合并版 (ALIGNMENT + DESIGN + TASK)
> **目标**：让 `ScreenshotHDR` 与 `ScreenshotEXR` 支持显式指定 HDR instance, 避免必须先 SetActiveInstance 才能截 PIP/split-screen 子实例
> **基线**：Phase F.0.11.5 完结 (commit pending, 2026-05-17)
> **创建日期**：2026-05-17
> **配套**：FINAL_PhaseF_0_11_7.md (实施记录) + ACCEPTANCE_PhaseF_0_11_7.md (验收)

---

## 1. 背景与设计

Multi-HDR-instance (Phase F.0.10.9) 场景: 用户有主屏 (id=0) + PIP (id=1+) 两个 HDR FBO. 想单独截 PIP, 当前需要:
```lua
local prevActive = HDR.GetActiveInstance()
HDR.SetActiveInstance(pipId)
Gfx.ScreenshotHDR('pip.hdr')
HDR.SetActiveInstance(prevActive)
```
繁琐 + 容易遗漏还原. 改造:
```lua
Gfx.ScreenshotHDR('pip.hdr', pipId)
Gfx.ScreenshotEXR('pip.exr', { instance_id = pipId })
```

### 1.1 改造范围
| API | 改动 |
|---|---|
| `ScreenshotHDR(path)` | 增第 2 个可选 integer 参数 `instance_id` |
| `ScreenshotEXR(path[, opts])` | opts 表新增 `instance_id` 字段 |

### 1.2 实现策略

不引入 `HDRRenderer::GetSceneTextureFor(int id)` 等新 API. 直接在 Lua bridge 临时切 active + 读取 + 复位:

```cpp
static int l_Graphics_ScreenshotHDR(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const int instanceId = (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
        ? (int)luaL_checkinteger(L, 2) : -1;
    // ... 既有错误处理

    const int savedActive = HDRRenderer::GetActiveInstance();
    if (instanceId >= 0 && instanceId != savedActive) {
        if (!HDRRenderer::SetActiveInstance(instanceId)) {
            lua_pushnil(L);
            lua_pushfstring(L, "ScreenshotHDR: invalid instance_id=%d", instanceId);
            return 2;
        }
    }
    // 既有 readback + 写盘逻辑
    // ...
    if (instanceId >= 0 && instanceId != savedActive) {
        HDRRenderer::SetActiveInstance(savedActive);   // 复位
    }
    return ret;
}
```

### 1.3 边界

| 输入 | 行为 |
|---|---|
| 缺省 instance_id (老用法) | 用当前 active instance (F.0 行为, 零回归) |
| `instance_id = -1` 显式 | 等同缺省 |
| `instance_id < -1` | luaL_checkinteger 自动 cast, 透传到 SetActiveInstance 失败时返 nil + err |
| `instance_id >= MAX_INSTANCES` 或未分配 | SetActiveInstance 返 false → 返 nil + err |
| `instance_id` 当前未 enabled | 切换成功但 GetSceneTexture 返 0 → 走既有 "invalid HDR scene" 路径 |

## 2. 任务拆分

| 任务 | 内容 | 估时 |
|---|---|---|
| T1 | l_Graphics_ScreenshotHDR / ScreenshotEXR 改造 (save/restore active + 错误处理) | 30 min |
| T2 | smoke 增 2 检查点 (无效 instance_id 拒绝, valid 缺省路径不变) | 15 min |
| T3 | demo_multi_hdr_pip 增 V 键演示 PIP 单独截图 + 文档收尾 | 30 min |

**总预计**: 1.25 小时

## 3. 验收门槛

- ✅ Release build 通过
- ✅ Screenshot smoke 通过新增检查点
- ✅ 4 demo 零回归
- ⏳ 真机 demo_multi_hdr_pip 按 V 生成只含 PIP 内容的截图
