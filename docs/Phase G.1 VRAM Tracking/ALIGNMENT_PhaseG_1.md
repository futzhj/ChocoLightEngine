# Phase G.1 — VRAM Tracking ALIGNMENT

## 一. 原始需求

> **HANDOFF §3 选项 A.1**: VRAM tracking + `Light.Graphics.GetMemoryStats()` — 多 instance HDR / TAA history / Dilation RT 显存占用追踪

用户场景:
1. 多个 HDR instance + TAA + SSR + Dilation 时, 用户**不知道**自己开了多少显存
2. 截图 / 录屏 instance 也分配 RT, 易爆显存
3. 调多 instance demo 时缺乏诊断手段
4. mobile 平台显存敏感, 需要预算管控

## 二. 项目上下文分析

### 2.1 GPU 资源分类 (经代码分析确认)

**A. Render Target (FBO + 关联 texture)** — 占 90%+ 显存
- HDR FBO: RGBA16F sceneTex + (可选) RG16F normalTex + RG16F/RG8 velocityTex + (可选) cameraVelocityTex + Depth24 RBO
- TAA history RT: RGBA16F × 2 (ping-pong, full or half-res)
- SSR: R32F depth + RGBA16F reflect + RGBA16F blur×2 + RGBA16F history×2
- Velocity Dilate RT: RG16F/RG8 (combined + camera-only)
- TAAU output sceneTex: RGBA16F
- Bloom mipmap chain (TBD, 占用大)
- Lens Dirt / Streak (小, 优先级低)

**B. Texture 资源** — 占 5~30% (取决于游戏)
- 用户上传 Image (RGBA8 主)
- ImageData
- Font glyph atlas
- LUT 3D texture (16x16x16 RGBA8 = 16KB, 少量)

**C. Mesh VBO/EBO/VAO** — 占 1~10%
- CreateMesh / CreateSkinnedMesh / CreateSkinnedMorphMesh
- Worker upload (asset_loader.cpp)
- BatchRenderer dynamic VBO (固定 ~2MB)
- Lit2D dynamic + static VBO

**D. UBO** — 占 < 1MB
- GPU Skinning joint matrices: 4096 bytes × 2

### 2.2 现有架构特点
- `IRenderBackend` 接口提供 Create/Delete*RT 等方法 (统一入口)
- HDR/TAA/SSR Renderer 是高层 wrapper (`hdr_renderer.cpp` / `taa_renderer.cpp` / `ssr_renderer.cpp`), 调 backend 接口
- backend 内部 `glGenTextures` + `glTexImage2D` 路径多 (50+ 处)
- 已有 `Phase F.0.10.6 multi-HDR-instance` (multi-instance 是常态)

### 2.3 风险点
- 不同 backend 不同 GL feature (RG8 vs RG16F, depth_component24 vs 32F) → bytes 公式要按 internalFormat 选
- mipmap chain 存在 (Bloom / luminance) → 公式 = base × 4/3
- Backend 内部 hashmap 已 track resource (e.g. `hdrFboNormalTex[fbo]`) → 复用这些 cache 可零侵入
- 用户对 texture 不敏感, 对 RT 敏感 → 优先级排序: RT > UBO > Mesh > Texture

## 三. 边界确认 (任务范围)

### ✅ 必做 (v1, ~3h)
- 实现 `GpuMemTracker` 单例 (`light_gpumem.cpp`, ~200 行)
- 跟踪 5 类**高价值 RT**:
  1. HDR FBO (sceneTex + normalTex + velocityTex + cameraVelocityTex + depthRBO)
  2. TAA history (×2)
  3. SSR (depth + reflect + blur×2 + history×2)
  4. Velocity Dilate (combined + camera-only)
  5. UBO Skin (4096×2)
- 暴露 `Light.Graphics.GetMemoryStats()` Lua API
- 返回结构: `{total_bytes, render_targets={count, bytes}, ubos={count, bytes}, items={{name,bytes,count}, ...}}`
- smoke 验证 baseline + RT 升降变化

### ❌ 不做 (留 v2)
- 用户 Image / ImageData / Font glyph 跟踪 (50+ 处分配点, 改动面广)
- Mesh VBO / EBO 跟踪 (用户分配的, 不是引擎管理的优先级低)
- BatchRenderer dynamic VBO (固定 ~2MB, 不变)
- Bloom mipmap chain (TBD 后续 phase)
- Lens Dirt / Streak (体积小)
- TAAU output sceneTex (实现不熟, 单 instance 只有 1 张)
- 实时 GL3 query (不靠操作系统 API, 完全引擎自计)

### ❌ 不做 (留 v3+)
- VRAM 预算 limit (超过抛错)
- 实时 OSD 显存表
- Tracy / RenderDoc 集成

## 四. 需求理解

### 4.1 核心目标
让用户**知道**自己当前 VRAM 占用, 据此优化:
- 关闭不必要 HDR instance
- 改用 half-res TAA history
- 改用 RG8 velocity format
- 不开 dilation pass

### 4.2 API 形态 (用户视角)
```lua
local m = Light.Graphics.GetMemoryStats()
print(string.format("Total: %.1f MB", m.total_bytes / 1024 / 1024))
print(string.format("  Render Targets: %.1f MB (%d items)",
    m.render_targets.bytes / 1024 / 1024, m.render_targets.count))
for _, item in ipairs(m.items) do
    print(string.format("    %s: %.1f MB ×%d",
        item.name, item.bytes / 1024 / 1024, item.count))
end
```

### 4.3 跟踪粒度决策
**简化策略**: 不在底层 `glGenTextures` hook, 而在**高层 Renderer 的 RT 创建/释放点** hook:
- HDRRenderer.CreateRT / ReleaseRT — 1 处
- TAARenderer.AllocateRT / ReleaseRT — 1 处
- SSRRenderer.AllocateResources / DestroyResources — 1 处
- HDRRenderer.CreateRT 内的 dilation 创建 — 1 处
- render_gl33.InitGPUSkinning UBO — 1 处

每处只加 1 行 `Tracker::Instance().Update("HDR sceneTex", w, h, FORMAT_RGBA16F, +1/-1)`

**好处**: 改动 ~10 处 (而非 50+), 风险低, 易回滚

### 4.4 多 instance 处理
当前 HDR/TAA/SSR 是 instance-aware, 每个 instance 各自 CreateRT.
Tracker 用**累计 count + 累计 bytes**, 不区分 instance ID.
显示 `HDR sceneTex × 4 = 50 MB` 用户能看出多 instance 占用.

## 五. 疑问澄清 (主动决策)

### 决策 1: 是否区分 instance?
**自动决策: 不区分 v1**
原因: 多 instance 实际只需用户看 "RT 总数 +1 显存涨了多少". 区分 instance ID 复杂度高, 收益低. v2 可加.

### 决策 2: bytes 公式精度?
**自动决策: 按 internalFormat 标准 bytes/pixel, 不算 mipmap**
原因: 跟踪的 RT 都不开 mipmap. 用户 texture 算 mipmap 是 v2.

### 决策 3: depth RBO 算什么?
**自动决策: 算到 HDR FBO 的 sceneTex 一起**
原因: depth RBO 与 sceneTex 同生命周期, 用户从"HDR FBO"角度想就够了.

### 决策 4: API 名字?
**自动决策: `Light.Graphics.GetMemoryStats()`**
对齐 RecordMP4 用 `Gfx.GetRecordStats()` 的命名风格.

### 决策 5: 返回 table 字段?
**自动决策**:
```lua
{
    total_bytes = N,
    render_targets = {count = N, bytes = N},   -- 汇总
    ubos = {count = N, bytes = N},              -- 汇总
    items = {                                    -- 详情
        {name = "HDR sceneTex", count = N, bytes = N, w = W, h = H, format = "RGBA16F"},
        {name = "TAA history",  count = N, bytes = N, ...},
        ...
    }
}
```

## 六. 关键假设 (需用户确认)

1. **跟踪范围**: 仅高层 RT (HDR/TAA/SSR/Dilate/UBO), 不跟踪用户 Image/ImageData/Mesh — **OK?**
2. **API 名**: `Light.Graphics.GetMemoryStats()` — **OK?**
3. **不区分 instance**: 多 instance 只看汇总 count — **OK?**
4. **桌面 + mobile 都生效**: tracker 是引擎自计, 不依赖 OS API, 全平台一致 — **OK?**

(若不确认, 默认按 §五 决策实施)

## 七. 验收标准

| 标准 | 数据 |
|------|------|
| `Light.Graphics.GetMemoryStats` 暴露 | API 存在 + 返 table |
| baseline (启动后无 HDR/TAA): total > 0 (UBO Skin 4KB×2) | total ≥ 8192 |
| 启用 HDR 1080p: total 增 ~16 MB | sceneTex 8MB + depth 8MB |
| 启用 TAA history: total 再增 ~16 MB | history×2 RGBA16F |
| 启用 SSR: total 再增 ~30 MB | depth+reflect+blur×2+history×2 |
| 关闭 HDR: 对应 bytes 归零 | ReleaseRT 清 |
| smoke `gpumem.lua` 无 FAIL | ≥ 10 用例 |
| 全 8 套 smoke 0 退化 | screenshot 81 / hdr 141 / etc. |

## 八. 与已交付 phase 的关系

| Phase | 关系 |
|-------|------|
| F.0.10.6 multi-HDR-instance | **依赖**: 多 instance 是核心场景 |
| E.18 Dilation pass | **依赖**: dilatedVelocityFbo/Tex 跟踪 |
| F.1 TAAU | **可选**: outputSceneTex 跟踪 (v1 跳过) |
| G.0 Lua 热重载 | 无关 |

## 九. 实施估时

| 任务 | 时间 |
|------|------|
| Align (本文档) | 0.5h |
| Design (公式表 + 接接点) | 0.5h |
| `light_gpumem.cpp` 实现 (~200 行) | 1h |
| 5 个接接点 hook | 0.5h |
| smoke + 验证 | 0.5h |
| FINAL + commit | 0.5h |
| **合计** | **3.5h** (估时 3h, +0.5h buffer) |
