# Phase F.0.10.9.x.2 — Bloom/SSR/MB Multi-Instance PLAN

> 6A · ALIGN + ARCHITECT + ATOMIZE
> 目标: 仿 multi-HDR 把 BloomRenderer / SSRRenderer / MotionBlurRenderer 也做 multi-instance,
>       不再依赖 demo "每帧切 profile" hack, 真正 SDK 级解耦

## 1. 对齐 (ALIGN)

### 1.1 当前状态
- HDRRenderer: ✅ multi-instance (4 instance, F.0.10.9)
- TAARenderer: ✅ multi-instance (4 instance, F.0.10.2)
- **BloomRenderer**: ❌ 全局单例 (`static State g;`)
- **SSRRenderer**: ❌ 全局单例
- **MotionBlurRenderer**: ❌ 全局单例

### 1.2 任务范围
- 3 个 renderer × `g_states[MAX_INSTANCES=4] + g_active + g_count + g_slot_in_use[]`
- 3 个 renderer × `#define g g_states[g_active]` 透明改造
- 3 个 renderer × 5 个新 fn (Create/Destroy/SetActive/GetActive/GetCount) = **15 fn**
- 15 个 Lua wrap (light_graphics.cpp) = **+15 Lua API (80 → 95)**
- 3 个 smoke 扩展 (bloom.lua / ssr.lua / motion_blur.lua) — 每个加 multi-instance 测试段
- demo_quad_split 受益: 可移除 `apply_postfx_profile()` 切换, 改为 per-quad instance 各自固定参数 (但本 phase 不修改 demo, 保留兼容)
- **零回归约束**: instance 0 默认 = 老单例行为, 所有现有 API 不变

### 1.3 成功标准
- 3 renderer 各自支持 4 instance (1 default + 3 user)
- multi-instance API 命名/语义与 HDR/TAA 完全一致
- 8 smoke 零回归 (instance 0 兼容)
- 3 smoke 加 multi-instance section (Create/SetActive/Destroy round-trip)
- CI 6/6 全绿

## 2. 架构 (ARCHITECT)

### 2.1 改造模板 (3 个 renderer 共用)

```cpp
// 老:
static State g;

// 新 (Phase F.0.10.9.x.2):
static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };
#define g g_states[g_active]

int CreateInstance() {
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            g_states[i] = State{};
            g_states[i].backend   = g_states[0].backend;
            g_states[i].supported = g_states[0].supported;
            g_states[i].inited    = g_states[0].inited;
            g_slot_in_use[i] = true; ++g_count;
            return i;
        }
    }
    return 0;   // 槽满
}

bool DestroyInstance(int id) {
    if (id <= 0 || id >= MAX_INSTANCES) return false;
    if (!g_slot_in_use[id]) return false;
    const int saved = g_active;
    g_active = id;
    ReleaseRT();              // renderer-specific: ReleasePyramid / ReleaseRT / DestroyResources
    g_states[id] = State{};
    g_slot_in_use[id] = false; --g_count;
    g_active = (saved == id) ? 0 : saved;
    return true;
}

bool SetActiveInstance(int id) {
    if (id < 0 || id >= MAX_INSTANCES) return false;
    if (!g_slot_in_use[id]) return false;
    g_active = id;
    return true;
}

int GetActiveInstance() { return g_active; }
int GetInstanceCount()  { return g_count; }
```

### 2.2 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| MAX_INSTANCES | 4 | 与 HDR/TAA 一致, 4 player split-screen 足够 |
| instance 0 语义 | default singleton, 永远占用, 不可销毁 | 与 HDR 一致, 老 API 透明兼容 |
| backend ptr 共享 | g_states[0] inherit | 避免每 instance 重复 Init backend |
| Init 时写入 | 仅 g_states[0] + 复位所有 slot | 保证零回归 |
| Shutdown 时 | 释放所有 slot in use, 复位 g_active=0, g_count=1 | 与 HDR 一致 |
| state 字段 | 直接复用 (RT handle / params 都 instance-local) | 模板透明 |

### 2.3 SSRRenderer 特殊点
SSRRenderer state 含 `prevViewProj[16] + hasPrevViewProj + frameCounter` (temporal SSR 状态). 这些是 instance-local (各自 history reproject), 改造后 4 instance 各自累积. 其余字段 (depth RT / reflect RT / blur ping-pong / history ping-pong) 全 instance-local.

### 2.4 demo_quad_split 受益预览 (本 phase 不改)
当前 demo 帧流程:
```lua
apply_postfx_profile(i)       -- 每帧切全局 Bloom/SSR/MB 参数
HDR.SetActiveInstance(hdr_ids[i])
Bloom.Process(rgn)            -- 用刚切的全局参数
```

改造后可以:
```lua
Bloom.SetActiveInstance(bloom_ids[i])  -- 切到 instance i, 各自固定参数
HDR.SetActiveInstance(hdr_ids[i])
Bloom.Process(rgn)                     -- instance i 自带 profile
```

但 4× pyramid VRAM (Bloom 5 mip × 4 instance = ~64MB extra at 1080p). 取舍:
- **本 phase**: 仅交付能力, demo 不动 (保留 "切 profile" 模式作 baseline)
- **后续可选**: 如果用户需要, 单独 phase 改造 demo

## 3. 原子化 (ATOMIZE)

### 3.1 Sub-task 拆分 (3 + 1 final)

#### Sub-task A: BloomRenderer multi-instance (~2h)
- [x] 探 BloomRenderer state (~10 字段, 含 fbos[8] + texs[8])
- [ ] bloom_renderer.h: 加 5 fn declaration
- [ ] bloom_renderer.cpp: state 数组化 + 5 fn 实现
- [ ] light_graphics.cpp: +5 Lua wrap (BloomRenderer.{CreateInstance, ..., GetInstanceCount})
- [ ] scripts/smoke/bloom.lua: + multi-instance §
- [ ] build + 真 GL 单 demo_quad_split 测试

#### Sub-task B: MotionBlurRenderer multi-instance (~1.5h)
- [x] 探 state (~10 字段)
- [ ] motion_blur_renderer.h: +5 fn
- [ ] motion_blur_renderer.cpp: state 数组化 + 5 fn
- [ ] light_graphics.cpp: +5 wrap
- [ ] smoke/motion_blur.lua: + multi-instance §

#### Sub-task C: SSRRenderer multi-instance (~2.5h, 最复杂)
- [x] 探 state (~30 字段, 含 8 RT id + 16 float prevViewProj + 4 ping-pong)
- [ ] ssr_renderer.h: +5 fn
- [ ] ssr_renderer.cpp: state 数组化 + 5 fn (DestroyResources 替代 ReleaseRT)
- [ ] light_graphics.cpp: +5 wrap
- [ ] smoke/ssr.lua: + multi-instance §

#### Sub-task D: 整合验证 + FINAL (~1h)
- [ ] 8 smoke 零回归 (instance 0 兼容)
- [ ] demo_quad_split 真 GL 启动验证 (老路径不破坏)
- [ ] FINAL_PhaseF_0_10_9_x_2.md
- [ ] TODO_PhaseF_0_10_9_x_2.md
- [ ] 1 commit (一次性) + CI 6/6

### 3.2 风险 & 缓解

| 风险 | 缓解 |
|------|------|
| `#define g g_states[g_active]` 替换误伤其他变量 | 仅 cpp 内部 (anonymous namespace 之后), header 不受影响 |
| Init 时机问题 (instance 0 必须先 Init 才能 CreateInstance) | 与 HDR 一致, headless 也允许 (backend=nullptr 时 supported=false 自然失败) |
| RT handle 残留 (Destroy 后 fbo/tex 没清) | `g_states[id] = State{}` 默认值清零 |
| Shutdown 时多 instance 资源泄漏 | Shutdown 改为遍历所有 in-use slot 释放 |
| backend 共享 (instance 1 backend == instance 0 backend) 一处崩全崩 | OK: backend 本来就是单例, 多 instance 也共享 |

## 4. 提交策略

- **1 commit** (减少 CI 次数), 内部 3 sub-task 顺序完成
- 文件变更预计:
  - `ChocoLight/include/{bloom,ssr,motion_blur}_renderer.h` 各 +30 行
  - `ChocoLight/src/{bloom,ssr,motion_blur}_renderer.cpp` 各 +80 行
  - `ChocoLight/src/light_graphics.cpp` +250 行 (15 Lua wrap)
  - `scripts/smoke/{bloom,ssr,motion_blur}.lua` 各 +60 行
  - `docs/Phase F.0.10.9.x.2 Bloom-SSR-MB multi-instance/{PLAN,FINAL,TODO}.md`
- 总计: ~14 files, +900/-30 LOC
- CI 风险: 中等 (C++ 改动, 但模板高度同质 + 已有 HDR/TAA 模板验证)
