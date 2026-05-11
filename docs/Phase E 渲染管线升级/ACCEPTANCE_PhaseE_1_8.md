# ACCEPTANCE — Phase E.1.8 · API 文档同步

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.1.8**：把 Phase E.1.3 + E.1.4 + E.1.5 + E.1.6 沉淀的 API 写进 `docs/api/`，与既有模块文档风格统一。

---

## 1. 改动摘要

| 文件 | 类型 | 改动量 | 关键内容 |
|------|------|--------|----------|
| `docs/api/Light_Lighting2D.md` | 新建 | ~260 行 | 11 Lua API + 3 常量 + 设计契约（坐标空间 / slot id / Update partial 语义 / 角度单位 / dir 归一化）+ ECS Light2D / LitSprite component + 渲染管线集成顺序 + 性能与限制 + 全 7 个 ACCEPTANCE 文档引用 |
| `docs/api/Light_Graphics.md` | 修改 | +75 行 | 在 `DrawQuad` 与 `Print` 之间插入 `DrawLit` + `DrawLitQuad` 完整文档（参数表 + 行为 + 示例） |
| `docs/api/MODULE_INDEX.md` | 修改 | +2 行 | 图形/渲染表追加 `Light.Lighting2D` 条目 + 引用提示 |

---

## 2. 文档结构对照

| 内容 | 位置 |
|------|------|
| **顶层 Lua API**（`L2D.AddPointLight` / `DrawLit` / ...） | `Light_Lighting2D.md` 主体 + `Light_Graphics.md` § DrawLit/DrawLitQuad |
| **ECS component**（`Light2D` / `LitSprite` 字段表） | `Light_Lighting2D.md` § ECS 集成 |
| **设计契约**（坐标空间 / id 空间 / partial update / 角度 / 归一化） | `Light_Lighting2D.md` § 设计契约 |
| **渲染管线集成**（`Render()` 2D 阶段顺序） | `Light_Lighting2D.md` § ECS 集成 → 渲染管线集成 |
| **性能与限制**（16 灯硬上限 / 1 lit sprite = 1 draw / dirty bit 未做） | `Light_Lighting2D.md` § 性能与限制 |
| **跨文档导航** | `MODULE_INDEX.md` 图形/渲染表 |

---

## 3. 验收清单（对齐 `TASK_PhaseE.md` § E.1.8）

| 验收标准 | 状态 | 证据 |
|----------|------|------|
| 11 + 2 Lua API 全文档覆盖 | ✅ | `Light_Lighting2D.md` 每个 API 独立 `## ` 段 + 参数表 + 返回值；`Light_Graphics.md` `DrawLit/DrawLitQuad` 同风格 |
| 2 ECS component 字段表 | ✅ | `Light_Lighting2D.md` § ECS 集成内 `Light2D` / `LitSprite` 字段表 |
| 设计契约（坐标 / id / partial / 角度 / 归一化）独立成节 | ✅ | `Light_Lighting2D.md` § 设计契约 |
| `MODULE_INDEX.md` 加 `Light.Lighting2D` | ✅ | 图形/渲染表追加；末尾提示语补 `Light_Lighting2D.md` |
| 风格与既有 `Light_*.md` 一致 | ✅ | 沿用 `## fnName` + `### 参数 / 返回值 / 示例` 三段模板（对照 `Light_Graphics.md`） |

---

## 4. 不在本任务范围

| 项 | 说明 |
|----|------|
| C++ 接口文档 | 未为 `RenderBackend::DrawLit2DQuad` / `UploadLighting2D` / `MeshGPU` 等 C++ 虚接口单独写文档；现有头文件 Doxygen 注释已足够 |
| Shader 内部文档 | `VS_LIT2D` / `FS_LIT2D` 的 uniform 布局在 `render_gl33.cpp` line 627-654 注释中详细说明，未单独抽出 |
| 性能 benchmark 报告 | "1000 lit quad < 16ms" 等性能数据待 perf_benchmark 任务（Phase E.x 外） |

---

## 5. 与 Phase E.1 整体收尾

E.1.8 是 Phase E.1（2D Forward 多光照系统）的最后一个原子任务。8 个原子任务全部完成后，进入 `FINAL_PhaseE_1.md` 总结报告。

| 任务 | 状态 | 交付物 |
|------|------|--------|
| E.1.1 | ✅ | `RenderVertex2DLit` + VAO/VBO/EBO 基础 |
| E.1.2 | ✅ | `sprite_lit_2d` shader (GL 3.3 + GLES 3.0) + uniform 缓存 |
| E.1.3 | ✅ | `Lighting2D::State` C++ 模块（16 slot + ambient + enabled） |
| E.1.4 | ✅ | Lua API binding（11 fns + 3 const）+ smoke |
| E.1.5 | ✅ | `DrawLit2DQuad` GL33 实现 + `Light.Graphics.DrawLit / DrawLitQuad` |
| E.1.6 | ✅ | ECS `Light2D` / `LitSprite` component + `_UploadLights2D` |
| E.1.7 | ✅ | `samples/demo_2d_lighting/` (8×6 grid + 3 灯 + 交互) |
| **E.1.8** | ✅ | API 文档同步（本任务） |
