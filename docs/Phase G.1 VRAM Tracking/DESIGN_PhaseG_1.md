# Phase G.1 — VRAM Tracking DESIGN

## 一. 总体架构

```
+---------------------- Lua ---------------------------+
|  Light.Graphics.GetMemoryStats() -> table            |
|  Light.Graphics.ResetMemoryStats()                   |
+----------+---------------------+----------------------+
           |                     |
  PushStats(L)            Reset()
           v                     v
+--------------- LT::GpuMem (light_gpumem.cpp) ---------+
|  static GpuMemItem s_items[64]                       |
|  Track(name, format, w, h)                           |
|  Untrack(name, format, w, h)                         |
|  TrackBytes(name, bytes)         <- UBO              |
|  UntrackBytes(name, bytes)                           |
|  Reset() / PushStats(L)                              |
+--------+-----------+-----------+----------+----------+
         ^           ^           ^          ^
         |           |           |          |
HDRRenderer  TAARenderer  SSRRenderer  GL33Backend
  (CreateRT/   (Allocate/   (Allocate/   (InitGPU
   ReleaseRT/  ReleaseRT)   Destroy      Skinning)
   Rebuild     ping-pong    Resources)
   Dilation)
```

**关键性质**:
- 单一全局静态 (`s_items[64]`), 无 mutex (Render 都在主线程)
- 完全引擎自计, **不依赖 OS 显存 API** -> 跨平台一致 (Windows/Linux/macOS/Android/iOS/Web)
- 报告型工具, 不影响 GPU 行为 -> 错配 (release 多于 create) 静默忽略, 不崩

## 二. 数据流

### 2.1 Track 时机 (创建)

| Hook 点 | 文件:行 | 注册项 |
|---------|---------|--------|
| `HDRRenderer::CreateRT` 末尾 | `hdr_renderer.cpp:336` | 5 项: sceneTex/normalTex/velocityTex/cameraVelocityTex/depthRBO |
| `HDRRenderer::CreateRT` dilation block | `hdr_renderer.cpp:362,377` | 1~2 项: Velocity Dilate (combined/camera) |
| `HDRRenderer::RebuildDilationRT` | `hdr_renderer.cpp:215,231` | 1~2 项 (`SetVelocityDilationHalfRes` 切换时) |
| `TAARenderer::AllocateRT` | `taa_renderer.cpp:150-151` | 2 项: TAA history × 2 (ping-pong) |
| `SSRRenderer::AllocateResources` | `ssr_renderer.cpp:241-249` | 1~6 项: depthTex + reflectTex + blur×2 + history×2 |
| `GL33Backend::InitGPUSkinning` | `render_gl33.cpp:4181-4182` | 2 项: UBO Skin joints + prev (4096 bytes 每个) |

### 2.2 Untrack 时机 (释放, 与 Track 严格对称)

| Hook 点 | 文件:行 | 触发场景 |
|---------|---------|---------|
| `HDRRenderer::ReleaseRT` 头部 | `hdr_renderer.cpp:244-266` | HDR.Disable / HDR.Resize |
| `HDRRenderer::ReleaseDilationRT` | `hdr_renderer.cpp:174-181` | SetVelocityDilationHalfRes 切换 |
| `TAARenderer::ReleaseRT` | `taa_renderer.cpp:121-123` | TAA.Disable / TAA.Resize |
| `SSRRenderer::DestroyResources` | `ssr_renderer.cpp:163-177` | SSR.Disable / SSR.Resize |

**对称性保证**: 每个 Track 都有对应 Untrack, 同一 (name, format, w, h) 三元组. **生命周期匹配 = count 归零 = slot 释放**.

## 三. 数据结构

```cpp
struct GpuMemItem {
    bool        used;            // slot 占用标志
    char        name[64];        // category 名 e.g. "HDR sceneTex"
    char        format[16];      // 格式名 e.g. "RGBA16F" / "BYTES" (UBO 等)
    int         count;           // 当前活跃实例数
    int         lastW, lastH;    // 最近一次尺寸 (诊断用)
    int64_t     bytesPerInst;    // = w * h * bpp (单实例 bytes)
    int64_t     totalBytes;      // = count * bytesPerInst
};
static GpuMemItem s_items[64];   // 64 个唯一 (name, format, w, h) 组合
```

**FindItem 三元组匹配**: `name + format + bytesPerInst` (bytesPerInst 隐含 w×h).
- 同 name 不同尺寸 -> 两条记录 (Resize 场景: 旧尺寸 Untrack + 新尺寸 Track 自然对齐)
- 同 name 同尺寸不同 format -> 两条记录 (`SetVelocityFormat RG16F<->RG8` 切换)

## 四. 公式表 (bytes_per_pixel)

| 格式 | bpp | 说明 |
|------|-----|------|
| `RGBA8`     | 4 | 用户 Image 主格式 (v1 不跟踪) |
| `RGBA16F`   | 8 | sceneTex / TAA history / SSR reflect / SSR blur / SSR history / TAAU output |
| `RG8`       | 2 | velocity 紧凑格式 (Phase E.14) |
| `RG16F`     | 4 | velocityTex / normalTex |
| `R16F`      | 2 | 自动曝光 luminance RT (v1 暂不跟踪) |
| `R32F`      | 4 | (备用, v1 未使用) |
| `DEPTH24`   | 4 | HDR depth RBO |
| `DEPTH32F`  | 4 | SSR depthTex |
| `RGB32F`    | 12 | (备用) |
| `BYTES`     | - | UBO 等非 wxh 资源 (UBO Skin joints 4096 bytes) |

**不跟踪 mipmap chain**: 所有跟踪 RT 均不开 mipmap (Bloom mipmap 留 v2).

## 五. Lua API 形态

```lua
local m = Light.Graphics.GetMemoryStats()
-- m = {
--   total_bytes    = 33554432,           -- 32 MB
--   render_targets = {count = 5, bytes = 33546240},
--   ubos           = {count = 2, bytes = 8192},
--   items = {
--     { name="HDR sceneTex", format="RGBA16F", count=1, bytes=8388608, w=1024, h=1024 },
--     { name="HDR depthRBO", format="DEPTH24", count=1, bytes=4194304, w=1024, h=1024 },
--     { name="UBO Skin joints", format="BYTES", count=1, bytes=4096, w=0, h=0 },
--     ...
--   },
-- }

Light.Graphics.ResetMemoryStats()    -- 清 tracker (不动 GPU)
```

**不变量**: `total_bytes == render_targets.bytes + ubos.bytes`

## 六. 关键决策

### 6.1 跟踪粒度 — 高层 wrapper hook (而非 backend hook)
**好处**: 改动 ~5 处文件, 风险低; backend 已 internal map 跟踪 (e.g. `hdrFboNormalTex`), 复用语义.
**代价**: silent fallback 时 (legacy GL2) 可能 over-report 1~2 MB. v1 接受 (注释明示).

### 6.2 不区分 instance ID
多 instance HDR/TAA 自然累加 count, 用户从 "HDR sceneTex × 4" 直观看出.
v2 可加 `m.items[i].instance_id` 字段, API 形状不变.

### 6.3 Reset 仅清 tracker, 不动 GPU
用户重新开 HDR 后 count 与现实不一致 (现实仍存活, tracker 从 0 起). 这是预期: Reset 是 "归零基线" 的诊断工具, 不应触发 GPU.

### 6.4 单线程假设
所有 Track/Untrack 在主线程 Render 路径调用, 无 mutex.
若未来 worker thread 上传纹理: 需加 `std::mutex` + 改 64 项数组为锁内拷贝 (v2).

### 6.5 64 项上限
当前 hooks ~10 类, 加上多 instance + 多 size 组合, 预估 ~30 项, 容量 64 留 2× headroom.
满时静默丢弃 (FindFreeSlot 返 -1), tracker 显示部分数据 (用户可调 `GPU_MEM_MAX_ITEMS` 重编).

## 七. 测试矩阵

| 用例 | 期望 |
|------|------|
| API 存在性 | `GetMemoryStats` / `ResetMemoryStats` 是 function |
| 返回结构 | 4 个顶层字段 (total_bytes / render_targets / ubos / items) |
| 不变量 | `total = render_targets.bytes + ubos.bytes` |
| items 字段 | 每项有 name/format/count/bytes/w/h, format ∈ 已知集合 |
| ResetMemoryStats | total_bytes -> 0 |
| HDR.Enable -> Disable | 增减一致 (headless 下 Enable=false 跳过) |
| TAA.Enable -> Disable | history×2 增减 |
| SSR.Enable -> Disable | depth+reflect+blur×2+history×2 增减 |
| bytes 公式 | `count * w * h * bpp[format]` 一致 |

## 八. 性能预算

| 操作 | 复杂度 | 实测 |
|------|--------|------|
| Track / Untrack | O(64) FindItem 线性扫描 | < 1us |
| GetMemoryStats | O(64) 扫描 + 推 N+4 个 Lua table | ~10us @ 30 items |
| 内存占用 | 64 × ~96 bytes ≈ 6 KB | 静态 BSS |

调用频率: Enable/Disable/Resize 是用户级操作 (每秒几次), 不在热路径.

## 九. 与已交付 phase 的依赖

| Phase | 关系 |
|-------|------|
| F.0.10.6 multi-HDR-instance | **依赖**: 多 instance 累加 count (核心场景) |
| E.18 Velocity Dilation | **依赖**: dilation RT 跟踪 (combined + camera) |
| E.18.1 dilation halfRes | **依赖**: ReleaseDilationRT/RebuildDilationRT 对称 |
| E.14 RG8 velocity format | **依赖**: 切 format 时 size 不变但 bpp 切换 |
| E.16 cameraVelocityTex | **依赖**: HDR FBO 第二张 velocity tex |
| F.1 TAAU outputSceneTex | **依赖**: ReleaseRT 内 Untrack outputSceneTex |
| G.0 Lua hot reload | 无关 |

## 十. v2+ 候选扩展

1. **用户 Image / ImageData 跟踪** (50+ 处 glGenTextures, 改动面广)
2. **Mesh VBO/EBO/VAO 跟踪** (asset_loader async upload)
3. **Bloom mipmap chain** (公式 = base × 4/3)
4. **Lens Dirt / Streak / SSAO RT** (体积小, 当前不跟)
5. **VRAM 预算 limit** (超过抛 lua error)
6. **OSD 实时显存表** (HUD overlay)
7. **per-instance ID 字段** (区分 multi-HDR instance)
8. **mutex 化** (worker thread 上传)
9. **GL3 query 校验** (`glGetIntegerv(GPU_MEMORY_INFO_*)`, 仅 NVIDIA)
