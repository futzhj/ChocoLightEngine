# CONSENSUS — Phase AW.x（GPU Skinning 真机验证工具链）

> **6A 工作流 Stage 2 — Architect §最终共识**：所有不确定性已解决；明确需求 / 验收标准 / 技术约束 / 任务边界。

**生成时间**: 2026-05-10
**前置文档**: `ALIGNMENT_PhaseAWx.md`
**用户决策**: AQ1=是 / AQ2=A+C 组合（setup 脚本下载 + README 指引）/ AQ3=是

---

## 1. 明确需求描述

### 1.1 一句话

**为 Phase AW GPU Skinning 提供一个开箱可用的真机性能验证 sample，让用户在 5 分钟内得到具体的 CPU/GPU 性能对比数据。**

### 1.2 用户故事

```
作为 一个 ChocoLight Engine 用户/开发者
我想 在我的桌面 GPU 机器上看到 Phase AW GPU skinning 的实际性能收益
通过 运行一个独立 sample (samples/demo_skinning_perf)
能够 在控制台看到 CPU vs GPU 的 frame timing 对比数字 (e.g. "GPU 0.06ms vs CPU 1.48ms = 23x")
并且 通过键盘 G/C/A 在运行时切换模式实时验证 OSD 显示
而无需 编写任何代码 / 修改引擎 / 处理大量配置
```

### 1.3 边界

- **技术边界**：纯 Lua sample + 一个 C++ Lua API 增量（`Light.Graphics.GetBackendName`）
- **资源边界**：不污染 git 仓库二进制；setup 脚本下载到本地（开发者个人目录）
- **平台边界**：sample 主要面向桌面 GPU 机器；headless / Web 自动 graceful fallback

---

## 2. 验收标准

| # | 标准 | 验证方法 |
|---|------|---------|
| **A1** | 新增 `Light.Graphics.GetBackendName()` 返回当前 backend 名称字符串（"GL33" / "Legacy" 等） | smoke 段 + demo 调用 |
| **A2** | 新增 `samples/demo_skinning_perf/main.lua` (OOP Window 风格) | 6 平台 build 通过；本地有资产时跑通 |
| **A3** | demo 内置 frame timing helper（rolling 60-frame avg/min/max，纯 Lua + `Light.Time.GetTicksNS()`）| demo OSD 显示 + 启动 baseline 输出 |
| **A4** | demo 支持运行时键盘切换：G=gpu / C=cpu / A=auto / R=re-baseline / ESC=quit | 键盘事件触发 + OSD 模式更新 |
| **A5** | 启动时自动 baseline（CPU 60 帧 + GPU 60 帧 + 1 秒预热），打印对比表 | 控制台输出 + speedup 数字 |
| **A6** | 资产缺失时 friendly fallback（提示路径列表 + 退出码 0）| 不携带资产直接跑 |
| **A7** | headless 模式（无 Window）退出码 0 | 与 demo_animation 行为一致 |
| **A8** | 提供 `setup.ps1` (Windows) + `setup.sh` (Linux/macOS) 一键下载默认资产 | 用户运行后 demo 立即可跑 |
| **A9** | `samples/README.md` 登记新 sample 一行 | 文档表格更新 |
| **A10** | `docs/api/Light_Animation.md` Phase AW 章节增加"如何在真机验证收益"段，引用新 sample | 文档章节存在 + 操作命令可复制 |
| **A11** | `TODO_PhaseAW.md` 标记 §1.1 §1.2 §3.3 完成 | TODO 文档更新 |
| **A12** | 6 平台 CI 全绿 + windows runtime smoke 不退化（≥ 170 PASS）| GitHub Actions 全绿 + smoke log |

---

## 3. 技术实现方案

### 3.1 整体方案

```
┌────────────────────────────────────────────────────────────────────────┐
│ Phase AW.x 交付物层级                                                   │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  ┌─ C++ 层 ─────────────────────────────────────────────────────┐      │
│  │ light_graphics.cpp:                                          │      │
│  │   + l_Graphics_GetBackendName(L)                              │      │
│  │     → 调 g_render->GetName() (新增 virtual API)               │      │
│  │   + 注册到 kGraphicsModule[]                                  │      │
│  │                                                              │      │
│  │ render_backend.h:                                            │      │
│  │   + virtual const char* GetName() const = 0                   │      │
│  │                                                              │      │
│  │ render_gl33.cpp:        GetName()  → "GL33"                  │      │
│  │ render_legacy.cpp:      GetName()  → "Legacy"                │      │
│  │ render_software.cpp:    GetName()  → "Software" (如存在)      │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                        │
│  ┌─ Lua sample 层 ──────────────────────────────────────────────┐      │
│  │ samples/demo_skinning_perf/                                   │      │
│  │   ├── main.lua          (OOP Window + frame timing + OSD)     │      │
│  │   ├── README.md         (使用说明 + setup + 故障排查)          │      │
│  │   ├── setup.ps1         (Windows: curl 下载 RiggedSimple.glb) │      │
│  │   ├── setup.sh          (Linux/macOS: curl 下载 + 同路径)     │      │
│  │   └── assets/           (gitignore; setup 脚本下载到此)        │      │
│  │       └── character.glb  (Khronos RiggedSimple.glb ~80KB)     │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                        │
│  ┌─ smoke 层 ───────────────────────────────────────────────────┐      │
│  │ scripts/smoke/graphics.lua (或 anim 内增量):                  │      │
│  │   + 段 [N]: GetBackendName API + 返回值非空校验                │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                        │
│  ┌─ 文档 / TODO 同步 ───────────────────────────────────────────┐      │
│  │ docs/api/Light_Animation.md  + 真机验证操作指引段             │      │
│  │ samples/README.md             + 新 sample 一行表项             │      │
│  │ docs/Phase AW.x/*.md         (本阶段产物)                     │      │
│  │ docs/Phase AW GPU Skinning/TODO_PhaseAW.md  + 完成标记         │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.2 关键 API 设计

#### **C++ 端**

```cpp
// render_backend.h
class RenderBackend {
public:
    virtual const char* GetName() const = 0;   // 新增；返回静态字符串
    // ... 其他既有 API 不变
};
```

```cpp
// render_gl33.cpp
class GL33Backend : public RenderBackend {
    const char* GetName() const override { return "GL33"; }
    // ...
};
```

```cpp
// light_graphics.cpp
static int l_Graphics_GetBackendName(lua_State* L) {
    if (!g_render) {
        lua_pushliteral(L, "None");
        return 1;
    }
    const char* name = g_render->GetName();
    lua_pushstring(L, name ? name : "Unknown");
    return 1;
}

static const luaL_Reg kGraphicsModule[] = {
    // ... existing ...
    { "GetBackendName", l_Graphics_GetBackendName },
    // ...
};
```

#### **Lua sample 端**

```lua
-- samples/demo_skinning_perf/main.lua (摘要; 详见 DESIGN_PhaseAWx.md)

local Anim = require 'Light.Animation'
local Time = require 'Light.Time'

local Game = Light(Light.UI.Window):New()

-- frame timing helper (60-frame ring buffer)
local FrameStat = {}
function FrameStat.new(window_size) ... end
function FrameStat:Push(ms)         ... end
function FrameStat:Avg()            ... end
function FrameStat:Min()            ... end
function FrameStat:Max()            ... end

-- baseline 自动跑
local function RunBaseline(mode_name, frames)
    Anim.SetSkinningMode(mode_name)
    -- 跑 frames 帧并测时
end

function Game:OnOpen()
    -- 1. 加载资产 (多路径检测; 缺失则提示退出)
    -- 2. 初始化 Animator + 播放第一个 clip
    -- 3. 自动 baseline (CPU/GPU 各 60 帧 + 预热)
    -- 4. 打印 baseline 表
    -- 5. 进入交互模式: 默认 GPU
end

function Game:OnKey(key, ...)
    -- G/C/A: SetSkinningMode + 重置 stat
    -- R: 重新 baseline
    -- ESC: 退出
end

function Game:Update(dt)
    -- 推进 animator + 测当前帧 ms
end

function Game:Draw()
    -- 1. DrawSkinnedMesh (内部按当前 mode 走 GPU/CPU)
    -- 2. OSD 文字: Mode / Backend / FPS / frame avg/min/max / 提示
end
```

### 3.3 集成方案

| 现有模块 | 改动方式 |
|---------|---------|
| `render_backend.h` | 新增 1 个 pure virtual `GetName()` (向上兼容增量) |
| `render_gl33.cpp` | override `GetName()` → 返回静态字面量 |
| `render_legacy.cpp` | override `GetName()` → 返回静态字面量 |
| `light_graphics.cpp` | 新增 `l_Graphics_GetBackendName` + 注册到 `kGraphicsModule[]` |
| `samples/README.md` | 表格新增一行 |
| `docs/api/Light_Animation.md` | Phase AW 章节末尾增加"真机验证"段 |
| `scripts/smoke/graphics.lua` | 新增段验证 `GetBackendName` API |

**完全无破坏**：所有改动均是新增，零修改既有 API 行为。

---

## 4. 技术约束

| 约束 | 说明 |
|------|------|
| 不引入新依赖 | sample 只用 `Light.Animation` / `Light.Time` / `Light.UI` / `Light.Graphics` 既有 API + `Light.Graphics.GetBackendName` 新增 |
| 不破坏 Phase AW | sample 100% 使用 Phase AW 公开 API（`SetSkinningMode` / `GetSkinningMode` / `DrawSkinnedMesh`）|
| 跨平台 | sample 主要面向桌面；mobile/Web 也能编译跑通（headless fallback）|
| 资产体积 | git 不预置 binary；setup 脚本按需下载 ~80KB 的 Khronos `RiggedSimple.glb`（公开 CDN）|
| 编码规范 | sample 遵循 `samples/perf_benchmark/main.lua` OOP 框架风格 |
| 文档语言 | 中文优先；操作命令保留英文 |
| Lua 版本 | Lua 5.1 兼容（项目锁定）|

---

## 5. 关键假设（已确认）

| # | 假设 | 状态 |
|---|------|------|
| ✅ | `Light.Time.GetTicksNS()` 可直接用于 ns 精度 frame timing | 已勘察源码确认 |
| ✅ | `samples/perf_benchmark/main.lua` 的 OOP 框架范式可复用 | 已勘察源码确认 |
| ✅ | `Anim.SetSkinningMode("gpu") + GetSkinningMode() == "gpu"` 可作为运行时 GPU 支持检测 | Phase AW T5 已实现此语义 |
| ✅ | Khronos `RiggedSimple.glb` 可公开下载（GitHub raw URL）| 公开资产（CC0 协议）|
| ✅ | `Light(Light.UI.Window):New()` OOP 框架在 6 平台都可用 | `perf_benchmark` 已在 CI 编译通过证明 |
| ✅ | 不需要 procedural skinned mesh API（资产缺失走 fallback 即可）| 用户接受 friendly exit 而非引擎层扩展 |

---

## 6. 任务边界限制

### 6.1 范围内（最终）

1. C++ Lua API：`Light.Graphics.GetBackendName()` + 3 个 backend 实现
2. C++ smoke：在 `scripts/smoke/graphics.lua`（或 `init.lua`）中新增段验证
3. Lua sample：`samples/demo_skinning_perf/{main.lua, README.md, setup.ps1, setup.sh, .gitignore}`
4. 文档同步：`samples/README.md` / `docs/api/Light_Animation.md` / `TODO_PhaseAW.md`
5. 6A 流程产物：`docs/Phase AW.x/*.md`

### 6.2 范围外（最终）

1. 不修改 `light_animation.cpp` / `render_gl33.cpp` 中的 Phase AW 实现
2. 不引入 procedural skinned mesh API
3. 不实现关节上限提升 / Web 默认值切换 / 增量 UBO 上传
4. 不在 git 仓库中提交 glTF 二进制资产
5. 不实现像素级 GPU vs CPU 数值对比工具
6. 不改 `samples/demo_animation/main.lua`（保持其 console-mode 用途）
7. 不实现 CI 自动跑 sample（headless 跑 sample 仅退出码 0 校验）

---

## 7. 共识签字

✅ 所有 ALIGNMENT 阶段不确定性已解决（DQ1-DQ8 自决，AQ1-AQ3 用户决策）。
✅ 验收标准 A1-A12 具体可测试。
✅ 技术方案与现有架构（Phase AS/AV/AW）100% 兼容。
✅ 任务边界清晰，范围可控。

→ 进入 **Stage 2 §设计** (`DESIGN_PhaseAWx.md`)
