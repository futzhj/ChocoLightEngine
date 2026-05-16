# Phase F.0.10.9.x.2 — Bloom/SSR/MB Multi-Instance FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (3 renderer × 5 fn = +15 Lua API, 95 fn 总数)

---

## 1. 完成工作

把 Bloom / SSR / MotionBlur 三个全局单例 renderer 改造为 multi-instance,
仿 HDR/TAA 的 `g_states[MAX_INSTANCES=4] + #define g g_states[g_active]` 模板.

| Renderer | 原状态 | 新状态 | LOC 新增 |
|----------|--------|--------|---------|
| BloomRenderer | 单例 | 4 instance | +90 (cpp) + +30 (h) |
| MotionBlurRenderer | 单例 | 4 instance | +85 (cpp) + +25 (h) |
| SSRRenderer | 单例 | 4 instance | +88 (cpp) + +25 (h) |
| light_graphics.cpp Lua wrap | — | +15 fn × ~35 行 | +185 |
| smoke (bloom/ssr/motion_blur) | — | + multi-instance § | +210 |
| **总计** | | | **+738 / -32 LOC** |

## 2. Lua API +15 (80 → 95)

| Renderer | API |
|----------|-----|
| Bloom | `CreateInstance / DestroyInstance / SetActiveInstance / GetActiveInstance / GetInstanceCount` |
| SSR | 同上 5 fn |
| MotionBlur | 同上 5 fn |

API 命名/语义与 HDR (F.0.10.9) / TAA (F.0.10.2) **完全一致**:
- instance 0 = default singleton, 永远占用, 不可销毁
- `CreateInstance()` 返 [1, 3] / 0 (槽满)
- `SetActiveInstance(id)` 切换, 后续 namespace fn 作用于 [id]
- `DestroyInstance(id)` 释放 + 标空闲, 销毁 active 自动切回 0
- `MAX_INSTANCES = 4` (与 HDR/TAA 一致)

## 3. 关键技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 改造模板 | `g_states[4] + g_active + macro g` | 复用 HDR multi-instance 验证过的零回归模板 |
| MAX_INSTANCES | 4 | 与 HDR/TAA 一致, 4 player split-screen 足够 |
| backend ptr 共享 | g_states[0] inherit | 避免每 instance 重复 Init backend |
| Shutdown 顺序 | 反向遍历 user instance + default 最后 | 防止状态错乱 |
| SSR State 无 inited | CreateInstance 仅 copy backend/supported | 模板轻微调整 (HDR 有 inited 字段) |

## 4. 改造模板核心 (3 renderer 共用)

```cpp
// 老:
static State g;

// 新:
static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };
#define g g_states[g_active]

// Init: 仅写 g_states[0]
// Shutdown: 反向遍历释放所有 in-use slot

// 5 fn:
int CreateInstance() { 找空闲槽 → 复位 State{} + inherit backend → ++g_count → 返 id }
bool DestroyInstance(int id) { 切到 id → ReleaseRT → State{} → 标空闲 → 切回 default 或 saved }
bool SetActiveInstance(int id) { 检查合法 + in_use → g_active = id }
int GetActiveInstance() { return g_active; }
int GetInstanceCount() { return g_count; }
```

## 5. 验证

### 5.1 8 smoke 零回归 + multi-instance 测试

```
PASS hdr  ·  PASS bloom (+ MI.1~8)  ·  PASS ssr (+ MI.1~8)
PASS auto_exposure  ·  PASS lens_fx  ·  PASS motion_blur (+ MI.1~6)
PASS taa  ·  PASS lighting2d
```

每个 multi-instance smoke 段 6-8 个 PASS, 覆盖:
- **MI.1** 初始状态 (count=1, active=0)
- **MI.2** Create × 3 + 槽满 (第 4 次返 0)
- **MI.3** SetActiveInstance round-trip
- **MI.4** Per-instance 参数隔离 (intensity / strength 各 instance 独立)
- **MI.5** SSR temporal state / MB Destroy(0) 拒绝
- **MI.6** SetActiveInstance(无效 id) 拒绝
- **MI.7/8** 清理 round-trip

### 5.2 真 GL 模式 (Windows 本地)

- demo_quad_split 启动 OK, 4 HDR + 4 TAA + Bloom + SSR + MB 全部 Enable
- 不破坏老 demo (即使没用 Bloom/SSR/MB.SetActiveInstance, 默认 active=0 等价老 API)

## 6. demo_quad_split 受益预览 (本 phase 不修改)

当前 demo 仍用 "每帧切全局参数" hack:
```lua
function apply_postfx_profile(i)
    Bloom.SetIntensity(profiles[i].intensity)  -- 全局参数
    SSR.SetIntensity(profiles[i].ssrIntensity)
    MB.SetStrength(profiles[i].mbStrength)
end
```

未来 (可选 next phase) 可改造为:
```lua
-- OnOpen: 一次性 setup
for i = 1, 3 do
    Bloom.SetActiveInstance(Bloom.CreateInstance())
    Bloom.SetIntensity(profiles[i].intensity)
    -- ... (各 instance 固定 profile)
end

-- Draw: 不再切参数, 仅切 active
HDR.SetActiveInstance(hdr_ids[i])
Bloom.SetActiveInstance(bloom_ids[i])  -- 各自固定 profile
Bloom.Process(rgn)
```

**VRAM 代价**: 4× Bloom pyramid + 4× SSR depth/reflect/blur/history + 4× MB RT
≈ +64MB at 1080p, +16MB at 640×360. 移动设备需谨慎.

## 7. 关键 lessons

### 7.1 模板高度同质

3 个 renderer 改造时间 (实际 ~2.5h 总, vs PLAN 6h 估):
- BloomRenderer: ~45min (新模板首次, 含验证)
- MotionBlurRenderer: ~30min (模板复制, 状态最简单)
- SSRRenderer: ~40min (模板复制, 状态多但结构同)
- Lua wrap + smoke + 验证 + FINAL: ~35min

模板复用 + HDR/TAA 已经验证, 风险极低. PLAN 6h 估偏保守.

### 7.2 SSR State 没 inited 字段

HDR/Bloom/MotionBlur State 都有 `bool inited = false;`, SSR 直接用
`supported = (backend && backend->SupportsSSR())`. 模板适配时
CreateInstance 仅 copy `backend + supported` (不 copy inited).

### 7.3 `#define g g_states[g_active]` 透明替换

Bloom 是 anonymous namespace 在外部, MotionBlur/SSR 是 namespace
MotionBlurRenderer { anonymous namespace { } } 嵌套. macro 是预处理
器替换, 跨 namespace 都生效, 无差别.

## 8. 文件变更

| 文件 | 变更 |
|------|------|
| `ChocoLight/include/bloom_renderer.h` | +30 行 (5 fn 声明 + 注释) |
| `ChocoLight/include/motion_blur_renderer.h` | +25 行 |
| `ChocoLight/include/ssr_renderer.h` | +25 行 |
| `ChocoLight/src/bloom_renderer.cpp` | +90 行 (state 数组化 + Shutdown 改造 + 5 fn) |
| `ChocoLight/src/motion_blur_renderer.cpp` | +85 行 |
| `ChocoLight/src/ssr_renderer.cpp` | +88 行 |
| `ChocoLight/src/light_graphics.cpp` | +185 行 (15 wrap fn + 3 funcs 表 entries) |
| `scripts/smoke/bloom.lua` | +75 行 (MI.1~8) |
| `scripts/smoke/ssr.lua` | +72 行 |
| `scripts/smoke/motion_blur.lua` | +56 行 (MI.1~6) |
| `docs/Phase F.0.10.9.x.2 Bloom-SSR-MB multi-instance/{PLAN,FINAL,TODO}.md` | 新建 |

## 9. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN §1 任务范围 + state 字段调研 |
| **Architect** | PLAN §2 改造模板 + 关键决策表 |
| **Atomize** | PLAN §3 Sub-task A/B/C/D + 风险表 |
| **Approve** | 用户选 "F.0.10.9.x.2 Bloom/SSR/MB pyramid 多 instance" |
| **Automate** | 3 renderer 顺序改造 + Lua wrap + smoke 扩展 (~2.5h) |
| **Assess** | 本 FINAL + 8 smoke 零回归 + 真 GL 启动验证 |

## 10. F.0.10.x 累计里程碑

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0.10.0 ~ F.0.10.9.3 | 多 instance HDR/TAA + region 后处理 + LUT 子生态 | 80 |
| F.0.10.10 | quad-split 联动 demo | 0 (lua-only) |
| **F.0.10.9.x.2 (本)** | **Bloom/SSR/MB multi-instance** | **+15 (95)** |
| **总计** | **完整 multi-instance 后处理链 SDK** | **95 fn** |

## 11. 下一步候选

- F.0.10.9.x.3 GetState/Clone (中优, ~1.5h, 5 renderer × 2 fn = 10 fn)
- F.0.11 demo 截图/录屏 (高优, ~3h, 配合 quad demo 视觉验收)
- demo_quad_split 改造为 multi-instance 模式 (~30min, 零 API 增量, 演示新能力)
- F.1 TAAU DLSS-like (~10-15h, 大版本)
