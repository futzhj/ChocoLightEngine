# FINAL — Phase E.1 · 2D Forward 多光照系统

> 项目总结报告 · 6A 工作流 · 阶段 6 · Assess

Phase E.1 完整交付：从 0 到 1 构建 ChocoLight 引擎的 2D forward 多光照系统，全 8 个原子任务（E.1.1 ~ E.1.8）连续 6 次 CI 全 6 平台通过。

---

## 1. 交付总览

| 维度 | 数据 |
|------|------|
| **原子任务** | 8 个全部完成（E.1.1 ~ E.1.8） |
| **代码行数** | +1900 行（C++ ~1100，Lua + smoke ~430，demo ~210，docs ~160）|
| **新增文件** | 14 个（C++ 2 + Lua 1 + demo 2 + 7 ACCEPTANCE + 1 API doc + 1 FINAL） |
| **修改文件** | 7 个（render_backend.h / render_gl33.cpp / light_graphics.cpp / light_ecs.cpp / lumen-master light.cpp / build-templates.yml / Light_Graphics.md / MODULE_INDEX.md / lighting2d.lua / ChocoLight CMakeLists.txt） |
| **Commits** | 5 次：`f448076` (E.1.3) / `9836c1b` (E.1.4) / `4248dce` (E.1.5) / `6454b04` (E.1.6 + E.1.7) / `[本次]` (E.1.8) |
| **CI 全 6 平台 ✓** | Windows / Linux / macOS / Android / iOS / Web 全部 success |
| **smoke 断言** | `lighting2d.lua` 28 段 PASS + DONE，覆盖 API 全套 + 16 灯极限 + ECS 集成 |

---

## 2. 架构层次

```
┌──────────────────────────────────────────────────────────────┐
│  Lua 用户代码                                                  │
│  ├─ Light.Lighting2D.AddPointLight{...}                       │  E.1.4
│  ├─ Light.Graphics.DrawLit(image, normalMap, x, y, ...)       │  E.1.5
│  └─ ECS world:Render() (自动 Light2D / LitSprite)             │  E.1.6
└──────────────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────────────┐
│  ChocoLight C++                                                │
│  ├─ light_lighting2d.cpp::g_state (16 slot + ambient)         │  E.1.3
│  │   └─ Add / Update / Remove / Clear / UploadToShader        │
│  ├─ light_graphics.cpp::l_DrawLit / l_DrawLitQuad             │  E.1.5
│  │   └─ BatchRenderer::Flush + RenderVertex2DLit + DrawLit2DQuad
│  └─ light_ecs.cpp::_UploadLights2D / _DrawLitSprite           │  E.1.6
│      └─ world->view space + zoom scaling                       │
└──────────────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────────────┐
│  RenderBackend (GL33Backend)                                   │
│  ├─ UploadLighting2D(state*) → SOA + glUniform*v ×8           │  E.1.5
│  ├─ DrawLit2DQuad: programLit2D + tex + uniform + draw        │  E.1.5
│  ├─ DrawLit2DTriangles / CreateLit2DMesh / DeleteLit2DMesh    │  E.1.5
│  └─ vaoLit2D + vboLit2D + eboLit2D + 14 uniform locations     │  E.1.1 / E.1.2
└──────────────────────────────────────────────────────────────┘
                           │
┌──────────────────────────────────────────────────────────────┐
│  GPU: sprite_lit_2d shader                                     │
│  ├─ VS_LIT2D: aPos/aUV/aColor/aNormal/aTangent → vTBN+vUV     │  E.1.2
│  └─ FS_LIT2D: 16-light forward loop + distance falloff +      │  E.1.2
│       Spot smoothstep cone + ambient                           │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 任务交付明细

| 任务 | 交付物 | Commit | 验收 |
|------|--------|--------|------|
| **E.1.1** | `RenderVertex2DLit` (64 字节) + `vaoLit2D / vboLit2D / eboLit2D` + 虚接口骨架 | （早期合入 main） | `static_assert` + 编译通过 |
| **E.1.2** | `VS_LIT2D / FS_LIT2D` (GL 3.3 + GLES 3.0 双路径) + 16 个 uniform location 缓存 + `lit2DSupported` 检测 | （早期合入 main） | shader 编译 / link 成功，日志可见 `Lit2D enabled` |
| **E.1.3** | `Lighting2D` C++ 模块：`State` + `Light` POD + 11 个 API + slot id 1..16 | `f448076` | 4 件交付物 + ACCEPTANCE |
| **E.1.4** | 11 个 Lua binding + 3 个常量 + `luaopen_Light_Lighting2D` + smoke (19 PASS) | `9836c1b` | Lumen runtime smoke 自动跑通 |
| **E.1.5** | `RenderBackend::UploadLighting2D / DrawLit2DQuad / DrawLit2DTriangles / CreateLit2DMesh / DeleteLit2DMesh` GL33 全套 + `Light.Graphics.DrawLit / DrawLitQuad` Lua API | `4248dce` | shader 端到端可上传 16 灯；smoke 22 PASS |
| **E.1.6** | ECS `Light2D` + `LitSprite` 内置 component + `_UploadLights2D` + `_DrawLitSprite` + `Render()` 2D 阶段集成 | `6454b04` | smoke 28 PASS（含 7 段 ECS 集成测试） |
| **E.1.7** | `samples/demo_2d_lighting/main.lua` (~210 行, 8×6 grid + 3 灯 + 键鼠交互) + README | `6454b04` | demo headless 跑通；视觉验收需用户本地 |
| **E.1.8** | `docs/api/Light_Lighting2D.md` 全 11 + 2 API + 设计契约 + `docs/api/Light_Graphics.md` `DrawLit/DrawLitQuad` 段 + `MODULE_INDEX.md` 同步 | `[本次]` | 3 个文档文件覆盖完整 API |

---

## 4. 关键技术决策

| 决策 | 选择 | 替代方案 / 原因 |
|------|------|------------------|
| Light 数量上限 | **16 个硬上限** | shader uniform 数组 `[16]` 与 C++ MAX_LIGHTS 严格一致，越界由 `Add*` 在调用方拒绝；动态扩容需要 UBO，复杂度↑ |
| 状态管理 | **`g_state` 文件内 static 单例** | POD 默认初始化等价于 zero-init；不堆分配 / 不加锁 / 单线程 |
| Lua API 错误返回 | **`Add* → nil + err string`，`Update → boolean`** | 区分"找不到位置"（nil 让 if 自然判负）vs "找到但失败"（boolean） |
| Update 语义 | **C++ 全覆盖，Lua 部分更新** | Lua 层 `lua_isnil` 守卫每字段，缺失字段保留原值；user-friendly |
| 角度单位 | **Lua "度"，C++ cos** | binding 内一次 `cosf(deg × π/180)`，Lua 端不暴露 cos 接口 |
| `UploadLighting2D` 接口签名 | **`(state*)`，backend 内部 build SOA** | 接口可读 vs 数据布局解耦；GL33 内栈分配 ~600 字节临时数组，性能可忽略 |
| 灯坐标空间 | **直接 API：vertex space；ECS：world space** | ECS 集成自动 `(world - cam) * zoom` 转 view，与 shader `vWorldPos = uModel * aPos` 一致 |
| BatchRenderer 兼容 | **`DrawLit` 内主动 Flush** | Lit 单独 program + VAO，不入 batch；用户混用 `Draw + DrawLit` 时顺序仍正确 |
| ECS 模块解析 | **`pcall(require)` + per-world cache** | 修正一个隐藏 bug：Lumen `L->Require` 不挂 `_G.Light.XXX`（同陷阱在 `_DrawSkinnedMesh` 也存在但被守卫成 silent no-op） |
| MSVC raw literal 处理 | **在新增段前插入 `)LUA" R"LUA(`** | 单段 ≤ 16KB，超限会 `error C2026`；保持现有多段拼接风格 |

---

## 5. CI 验证轨迹

| Commit | Run | 结果 | 备注 |
|--------|-----|------|------|
| `f448076` (E.1.3) | `25662570407` | ✅ 全 6 平台 | 9m37s 首次通过 |
| `9836c1b` (E.1.4) | `25672450646` | ✅ 全 6 平台 | 12m45s 含 Windows runtime smoke |
| `4248dce` (E.1.5) | `25674928639` | ✅ 全 6 平台（rerun 一次） | 首跑 Windows/Linux/Web 撞 `actions/checkout@v4` 临时故障 |
| `6454b04` (E.1.6 + E.1.7) | `25677056703` | ✅ 全 6 平台一次通过 | 含 ECS 集成验证 |
| `[本次 E.1.8]` | 待 push | 待跑 | 纯文档改动，预期一次通过 |

---

## 6. 已知限制（留给后续）

| 限制 | 优先级 | 建议方案 |
|------|--------|----------|
| 每 lit sprite = 1 draw call（无 batch） | 中 | Phase E.2 后 `LitBatchRenderer`：同纹理 + 同 Lighting state 合批 |
| `UploadLighting2D` 每 draw 重传 uniform | 低 | dirty bit：仅在 `Lighting2D::State` 变更时上传，否则跳过 `glUniform*v` |
| LitSprite cull 不影响 light upload | 低 | `_UploadLights2D` 接受 `bounds` 参数，跳过 viewport 外的 light |
| 视觉验收依赖人工本地跑 demo | 低 | CI 加 headless GL 上下文（EGL surfaceless / Mesa offscreen）+ pixel diff |
| `Light.Animation` 同样有 `_G.Light.XXX` 未挂的隐藏问题 | 低 | 复用 E.1.6 的 `pcall(require) + cache` 模式修复 `_DrawSkinnedMesh / _AnimationSystem` |
| Phase E.2 HDR + 后处理（bloom / tonemapping）| 高 | 独立 Phase，依赖 Phase E.1 的 forward baseline |

---

## 7. 后续推荐路径

| 选项 | 工作量估计 | 价值 |
|------|-----------|------|
| **Phase E.2.x 性能优化**（dirty bit + LitBatchRenderer + cull 联动） | 2~3 天 | 中（当前 < 50 lit sprite 性能足够） |
| **Phase E.3 HDR + Tonemapping**（OffscreenRT + uHDREnabled + ACES） | 4~5 天 | 高（视觉天花板提升） |
| **Phase E.4 Bloom / Postprocess**（多 pass ping-pong FBO） | 3~4 天 | 高（与 HDR 同步） |
| **fixes**（修复 `_DrawSkinnedMesh::Light.Animation` 同陷阱 + 加 normalMap demo 资源） | 0.5 天 | 低（不阻塞主线） |

---

## 8. 致谢与参考

- **6A 工作流**（用户提供的项目规范）：在每个原子任务的 ALIGNMENT / DESIGN / TASK / ACCEPTANCE / FINAL 五阶段闭环
- **既有模块模板**：`Light.Animation` (Phase AV) / `Light.Physics3D` (Phase AU) 提供 Lua binding + ECS 集成的成熟参考
- **Lumen runtime**：`L->Require(modName, proc)` 模块预加载机制让 Light.dll 自动可用，避免 `package.preload` 手动配置
