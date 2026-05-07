# ACCEPTANCE — Phase A 验收记录

> 阶段: Phase A — Sprite Batcher + EBO + SDL_HINT
> 工期目标: 3-4 天 | 实际: 1 个会话 | 分支: `feature/sdl3-perf-release`

---

## 一、子任务完成度

| # | 任务 | 状态 | 提交 | 备注 |
|:-:|------|:----:|------|------|
| A0 | worktree 创建 + 基线验证 | ✅ | (基础设施) | 基于 main `acaca82`(规划文档) |
| A1 | SDL3 release-3.2.0 → release-3.2.30 | ✅ | `ce018fb` | 升级最新稳定补丁 |
| A2 | SDL_HINT 6 项配置 | ✅ | `ce018fb` | IME/AppID/VSync/screensaver/clickthrough/double_buffer |
| A3 | RenderBackend.DrawIndexed 接口 | ✅ | `a5ae5ea` | 默认实现展开为 DrawArrays(Triangles),向后兼容 |
| A4 | BatchRenderer 模块完整实现 | ✅ | `a5ae5ea` | 298 行,后端无关,静态 EBO 一次生成 |
| A5 | GL33Backend EBO 适配 | ✅ | `7a817d4` | 重载 DrawIndexed 走 glDrawElements |
| A6 | LegacyBackend 兜底 | ✅ | (无修改) | 直接复用 RenderBackend 默认实现 |
| A7 | 各模块接入 BatchRenderer | ✅ | `7a817d4` | light_ui+particles+tilemap+graphics |
| A8 | perf_benchmark + smoke + ACCEPTANCE | ✅ | (本提交) | 含本文档 |

---

## 二、文件级改动清单

### 新增文件

| 文件 | 行数 | 说明 |
|------|:----:|------|
| `ChocoLight/include/batch_renderer.h` | 121 | BatchRenderer 接口声明 |
| `ChocoLight/src/batch_renderer.cpp` | 297 | BatchRenderer 实现 |
| `scripts/smoke/perf_smoke.lua` | 81 | 性能模块烟雾测试 |
| `samples/perf_benchmark/main.lua` | 148 | 性能压力场景(粒子/sprite/text/tilemap) |
| `docs/SDL3性能释放/ACCEPTANCE_PhaseA.md` | 本文档 | Phase A 验收 |

### 修改文件

| 文件 | 改动 | 说明 |
|------|------|------|
| `ChocoLight/CMakeLists.txt` | +3 行 | SDL3 升级 + batch_renderer.cpp 加入源列表 |
| `ChocoLight/src/platform_window_sdl3.cpp` | +25 行 | SDL_HINT 配置 |
| `ChocoLight/include/render_backend.h` | +25 行 | DrawIndexed 虚方法 + 默认实现 |
| `ChocoLight/src/render_gl33.cpp` | +50 行 | EBO 成员 + Init/Shutdown + DrawIndexed 重载 |
| `ChocoLight/src/light_ui.cpp` | +5 行 | BatchRenderer 生命周期 + BeginFrame/EndFrame |
| `ChocoLight/src/light_particles.cpp` | +8 行 | EmitterDraw 走 BatchRenderer |
| `ChocoLight/src/light_tilemap.cpp` | +10 行 | DrawLayer 走 BatchRenderer |
| `ChocoLight/src/light_graphics.cpp` | +30 行 | SubmitOrDraw helper + 4 个高频函数接入 |

### 改动汇总

- 新增代码: ~647 行
- 修改代码: ~156 行
- 文件变更: 10 个(5 新增 + 5 修改)

---

## 三、设计决策记录

### D-A1: SDL3 GIT_TAG 锁定 release-3.2.30

- 当前最新稳定补丁,SDL3 在 3.2.x 系列保持 ABI 兼容
- 升级 30 个补丁版本,获取 bug 修复

### D-A2: SDL_HINT 必须在 SDL_Init 之前

- 否则 hint 不生效
- 6 项配置都放在 PlatformWindow::Init() 函数顶部

### D-A3: DrawIndexed 默认实现退化为 DrawArrays

- 通过基类默认实现,所有现有 backend 不需修改即编译通过
- 正确性保证: 索引展开顶点 → DrawArrays(Triangles)
- 性能优化: 子类(GL33)重载为 glDrawElements 高性能路径

### D-A4: BatchRenderer 静态共享 EBO

- 启动时一次生成 [0,1,2,0,2,3, 4,5,6,4,6,7, ...]
- 16384 quad 上限(uint16 索引最大 65536)
- 永久不修改,不分配,纯 quad 批次零索引上传成本

### D-A5: 状态匹配判断仅含纹理 ID

- Phase A 仅以 textureId 作为批次状态键
- blend mode / scissor / shader 切换由调用方主动 NotifyStateChange
- 后续 Phase 可扩展为完整状态指纹

### D-A6: GL33Backend 的 EBO 与 VAO 绑定

- VAO 状态包括 EBO 绑定
- BindVertexArray(vao) 自动恢复 EBO 关联
- DrawIndexed 内 BindBuffer(EBO) 仍调用以确保正确,容量增长走 glBufferData,数据上传走 glBufferSubData

### D-A7: 接入策略 — 4 个高频函数 + helper

- light_graphics.cpp 引入 SubmitOrDraw helper,合并 BindTexture+DrawArrays+UnbindTexture
- 仅接入最高频 4 个函数: l_Draw / l_DrawQuad / l_Print / l_DrawSprite
- 其他几何函数(Line/Triangle/Rectangle 等)调用频率低,沿用 g_render->DrawArrays
- light_particles 全部接入(粒子是最大瓶颈)
- light_tilemap 全部接入(瓦片地图)

---

## 四、性能预期(待真机/RenderDoc 验证)

| 场景 | 当前预期 draw call | 验证方式 |
|------|:------------------:|---------|
| 1024 粒子 | 1 | RenderDoc 抓帧 |
| 1000 字符同字体 | 1 | RenderDoc 抓帧 |
| Tilemap 一屏(20×15) | 1 | RenderDoc 抓帧 |
| 多 sprite 同纹理 | 1 | RenderDoc 抓帧 |
| 多 sprite N 纹理 | N | RenderDoc 抓帧 |

---

## 五、验收标准核对

| # | 验收项 | 状态 |
|:-:|--------|:----:|
| A-V1 | 1024 粒子 draw call/帧 ≤ 3 | ⏳ 待 RenderDoc 真机验证 |
| A-V2 | 1000 字符文本 draw call/帧 ≤ 5 | ⏳ 待真机验证 |
| A-V3 | 现有 24 个 Lua API 签名零变化 | ✅ 仅 Phase B 后会增 GetBackendName |
| A-V4 | 现有 sample/example 渲染像素一致 | ⏳ 待本地运行回归 |
| A-V5 | 6 平台 GitHub Actions 全绿 | ⏳ 待 PR 触发 |
| A-V6 | SDL3 升级到 release-3.2.x 最新 | ✅ release-3.2.30 |
| A-V7 | SDL_HINT 配置生效 | ✅ 启动日志可见 |
| A-V8 | perf_smoke.lua lightc -p 通过 | ⏳ 待 CI |
| A-V9 | samples/perf_benchmark/ 已新建 | ✅ |

---

## 六、已知遗留(转 Phase B/C)

- BatchRenderer 状态指纹仅含纹理:Phase B 引入 SDL_GPU 时扩展为完整指纹(含 pipeline state)
- light_graphics.cpp 几何函数(Line/Triangle/Rectangle/Polygon/Arc/Circle/RoundedRectangle/Quad)未接入:调用频率低,Phase A 不强求,后续可视需求接入
- video_backend_*.cpp 视频纹理 quad 未接入:视频帧通常 1 个 quad/帧,无显著收益
- light_ui_widget.cpp UI 控件未审查接入:Phase 3 已实现,可在 Phase B 后端切换时统一改造

---

## 七、提交历史

```
acaca82 docs: plan SDL3 performance release (3-phase closure)        [main]
ce018fb perf(sdl3): A1+A2 upgrade SDL3 to 3.2.30 + HINT tuning      [feature]
a5ae5ea perf(render): A3+A4 RenderBackend.DrawIndexed + BatchRenderer
7a817d4 perf(render): A5+A6+A7 GL33 EBO + Legacy default + integrate
<this>  perf(bench): A8 perf_benchmark + perf_smoke + ACCEPTANCE
```

---

## 八、下一步

- 推送 feature/sdl3-perf-release 到 origin
- 在 GitHub UI 创建 PR(target: main)
- 等待 6 平台 GitHub Actions 全绿
- 合并 main → 进入 Phase B(SDL_GPU 后端)
