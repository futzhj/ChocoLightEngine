# Phase F.0.10.2 — 真物理 Split-Screen 技术分析 + 备选路径

> 6A 工作流 · 阶段 1 (Align) · 决策点
> 关联：F.0.10 (multi-instance API) / F.0.10.1 (demo cycle 演示)

---

## 1. 现状摸排 (Ground Truth)

### 1.1 backend 已有能力

| 能力 | 位置 | 暴露 Lua | 备注 |
|------|------|---------|------|
| `g_render->SetViewport(x, y, w, h)` | `render_gl33.cpp:7181` | ❌ **未暴露** | 仅 backend 内部 + light_ui.cpp 全屏复位用 |
| `Graphics.SetCanvas(canvas)` Lua API | `light_graphics.cpp:238` | ✅ | 创建独立 LDR FBO, 切换 render target |
| `Graphics.PushCanvas/PopCanvas` | `light_graphics.cpp` | ✅ | Canvas 栈, 嵌套 FBO 管理 |
| `HDRRenderer::BeginScene/EndScene` | `hdr_renderer.cpp:392/402` | ❌ 不直接, 自动 hook BeginFrame | 单帧仅 1 次 |
| `TAARenderer::Process(hdrFbo, hdrTex)` | `taa_renderer.cpp:294` | ❌ 不暴露, 由 HDR.EndScene 调 | line 514 in hdr_renderer.cpp |

### 1.2 关键约束

#### 约束 A — HDR 单帧 1 次 BeginScene/EndScene
```cpp
// hdr_renderer.cpp:392
void BeginScene() {
    if (!g.enabled || g.paused || !g.backend || !g.fbo) return;
    g.backend->BindFBO(g.fbo);
    g.backend->SetViewport(0, 0, g.width, g.height);
    g.backend->ClearCurrent(0.0f, 0.0f, 0.0f, 0.0f);  // ← 全屏清屏
}
```
单帧若调 2 次 BeginScene, 第二次会 **全屏清屏破坏第一次的左半内容**。

#### 约束 B — TAA 不暴露 Lua, 由 HDR 链路自动调
```cpp
// hdr_renderer.cpp:514
TAARenderer::Process(g.fbo, g.sceneTex);  // 全屏 process, 用 active instance history
```
demo 无法手动控制 TAA 何时 process / process 哪个区域。

#### 约束 C — TAA history RT 是 instance 私有的全屏 (W × H)
TAA shader 用 sceneTex / history 全屏纹理坐标采样, 若强行 viewport(0,0,W/2,H) 让 TAA 处理左半, history 仍按全屏寻址, 会出现 cross-talk + 错误纹理坐标。

#### 约束 D — Canvas (LDR) 与 HDR 链路互斥
```cpp
// light_graphics.cpp::SetCanvas
if (HDRRenderer::IsEnabled() && HDRRenderer::IsPaused()) {
    HDRRenderer::Resume();   // SetCanvas(nil) 才恢复 HDR
}
```
SetCanvas 会 Pause HDR; Canvas 上渲染**不走 HDR/TAA 链路**, 仅普通 LDR forward。

---

## 2. 真物理 Split-Screen 完整方案 (理论)

要在同一帧内让左半 viewport 用 instance 1 的 TAA、右半 viewport 用 instance 2 的 TAA, 必须满足:

```
玩家 1 视角 → 左 (W/2, H) sceneTex → instance 1 TAA history (W/2, H) → 屏幕左半
玩家 2 视角 → 右 (W/2, H) sceneTex → instance 2 TAA history (W/2, H) → 屏幕右半
                                                                     │
                                       composite to default fb ──────┘
```

需要的 backend 改动:
1. **HDR 多实例化**: 每个 instance 独立 fbo + sceneTex (与 TAA 多实例配对) — **2-4h**
2. **TAA 真区域 process**: shader 接收 (uvOffset, uvScale) + history 多实例区域写入 — **2-3h**
3. **Lua API 暴露**: `Graphics.SetViewport()` / `HDR.CreateInstance()` / `HDR.SetActiveInstance()` / `TAA.ProcessRegion(x, y, w, h)` — **1-2h**
4. **demo 改造**: 双 BeginScene + 双 instance switch + composite — **1-2h**

**总计**: **6-11h**, 超出 4-6h 预算 1.5-2x.

---

## 3. 备选路径 (按工作量升序)

### 路径 A — 仅暴露 SetViewport Lua API (1-2h)

**改动**:
- `light_graphics.cpp` 加 `l_SetViewport(x, y, w, h)` + `l_GetViewport()` (~30 行)
- demo_taa_split 升级: 用 SetViewport 控制相机渲染区域, 每帧:
  - SetViewport(0, 0, W/2, H) + 渲染玩家 1 场景
  - SetViewport(W/2, 0, W/2, H) + 渲染玩家 2 场景
- HDR 仍单次 EndScene (TAA 全屏 process, 但 instance 0 always)

**展示**:
- ✅ 双视口渲染能力
- ❌ TAA 仍是单 instance 全屏 process, **multi-instance 不参与 split-screen**
- ❌ 不算"真物理 split-screen with TAA"

**验收**:
- demo: 用户能看到左半玩家 1 视角 / 右半玩家 2 视角
- smoke: SetViewport round-trip + 边界 clamp

### 路径 B — Canvas 双视口 + LDR 分屏 (3-4h) **【推荐】**

**改动**:
- `light_graphics.cpp` 加 `l_SetViewport` Lua API (~30 行)
- demo_taa_split 升级:
  - 创建 2 个 Canvas (W/2 × H 各一个) → player1_canvas, player2_canvas
  - 每帧:
    1. SetCanvas(player1_canvas) + 渲染玩家 1 场景 + SetCanvas(nil)
    2. SetCanvas(player2_canvas) + 渲染玩家 2 场景 + SetCanvas(nil)
    3. SetViewport(0, 0, W/2, H) + 把 player1_canvas blit 到屏幕左半
    4. SetViewport(W/2, 0, W/2, H) + 把 player2_canvas blit 到屏幕右半
  - HDR/TAA **不参与** (Canvas 走 LDR forward 路径)

**展示**:
- ✅ 真物理双视口分屏 (左右半屏完全独立内容)
- ✅ 双相机视角同帧渲染
- ❌ TAA 不在分屏中工作 (Canvas LDR 路径不走 TAA)
- ✅ 诚实限制说明: "TAA + split-screen 需 HDR 多实例化, 留 F.0.10.3"

**验收**:
- demo: 两个独立场景同帧分屏显示
- smoke: SetViewport + Canvas 联动测试

### 路径 C — HDR + TAA 完整多实例化 (8-12h)

**改动**: 见 §2 完整方案 (HDR 多实例 + TAA shader 区域 process + Lua API + demo)

**展示**:
- ✅ 真物理 split-screen with 独立 TAA per player
- ✅ 完成 F.0.10 multi-instance 设计的最终愿景

**风险**:
- HDR 多实例化牵动 9 个 sub-phase (E.3 ~ E.13 整套)
- shader 改动需 6 平台兼容验证 (windows/linux/macos/android/ios/web)
- 工作量 > 2x 预算

---

## 4. 推荐 + 决策请求

**推荐**: **路径 B (Canvas 双视口 + LDR)**, 原因:
1. 工作量 3-4h 在预算 4-6h 内
2. 实现真物理双视口分屏 (用户能看到真实双玩家分屏画面)
3. 复用现有 Canvas API (无需 HDR/TAA backend 改动)
4. 诚实标注 "TAA + split-screen 联动需 F.0.10.3"
5. SetViewport Lua API 一次性暴露, 后续 phase 可复用

**决策请求**:
- 选 A (1-2h, 仅 SetViewport, 视觉差异有限)?
- 选 B (3-4h, 真双视口 LDR, 推荐)?
- 选 C (8-12h, 完整方案, 超预算)?
- 改 plan, 留 F.0.10.2 给后续 (现在切非渲染方向)?
