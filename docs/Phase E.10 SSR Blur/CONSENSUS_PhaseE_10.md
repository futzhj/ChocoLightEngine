# Phase E.10 SSR Blur — 共识文档（CONSENSUS）

> 日期：2026-05-12  
> 6A 阶段：**阶段 1 Align（最终共识）**  
> 用户拍板：**half-res blur ping-pong（移动端优化方案）**

---

## 1. 用户决策记录

用户在 ALIGNMENT 询问中选择：
> "blur RT 降为 half-res（移动端优化）"

即在保留 Phase E.9 反射 RT (full-res RGBA16F) 不变的前提下，**仅 blur 中间产物使用 half-res ping-pong RT**，省 ~75% 显存（1080p 8MB→2MB）。
最终采样回 composite 时由硬件 bilinear filter 自动 upscale。

其他参数沿用 ALIGNMENT 推荐默认值。

---

## 2. 最终方案锁定

### 2.1 核心规格

| 维度 | 选定方案 | 备注 |
|------|---------|------|
| **blur 算法** | 5-tap separable Gaussian | sigma ≈ 1.6, 中心权重 0.227 |
| **blur RT 分辨率** | **half-res ping-pong (w/2 × h/2)** | ★ 用户拍板 |
| **blur RT 格式** | RGBA16F | 与反射 RT 同精度 |
| **blur RT 数量** | 2（ping-pong：H pass + V pass） | full-res reflectTex 不动 |
| **radius 控制** | 全局参数 `SetBlurRadius` | per-pixel roughness 留 Phase E.11+ |
| **radius 范围** | `[0.5, 4.0]`, 默认 `1.5` | texel-space 单位 |
| **BlurEnabled 默认值** | `false`（保持 Phase E.9 兼容） | 用户主动 SetBlurEnabled(true) 启用 |
| **采样模式** | 硬件 bilinear filter | 配合 half-res 自动 upscale |
| **depth-aware** | ❌ 不做 | 反射模糊就是要"糊掉" |

### 2.2 内存预算（1080p）

| 资源 | Phase E.9 | Phase E.10 增量 | Phase E.10 总计 |
|------|-----------|----------------|----------------|
| reflectTex (full-res RGBA16F) | ~8 MB | 0 | ~8 MB |
| compTempTex (full-res RGBA16F) | ~8 MB | 0 | ~8 MB |
| blurTex[0] (half-res RGBA16F) | — | ~2 MB | ~2 MB |
| blurTex[1] (half-res RGBA16F) | — | ~2 MB | ~2 MB |
| **SSR 子系统合计** | **~16 MB** | **+4 MB** | **~20 MB** |

### 2.3 性能预算（1080p, RTX 3060 估计）

| Pass | 分辨率 | 预估 GPU 时间 |
|------|--------|--------------|
| DrawSSR (raw, 64 step) | full-res 1920×1080 | ~3.0 ms |
| DrawSSRBlur H（启用 blur 时） | half-res 960×540 | ~0.15 ms |
| DrawSSRBlur V（启用 blur 时） | half-res 960×540 | ~0.15 ms |
| DrawSSRComposite | full-res 1920×1080 | ~0.3 ms |
| **合计（blur 开）** | — | **~3.6 ms** |
| **合计（blur 关，Phase E.9）** | — | **~3.3 ms** |

Blur 开销 ~0.3 ms（< 10% 总 SSR 开销），符合"轻量延续"目标。

---

## 3. API 设计（最终）

### 3.1 Lua API 新增（从 22→24 函数）

```lua
-- 新增 2 函数 (位置: 紧邻 SetBlurEnabled/GetBlurEnabled 后)
Light.Graphics.SSR.SetBlurRadius(float [0.5, 4.0])   -- default 1.5
Light.Graphics.SSR.GetBlurRadius() -> float
```

### 3.2 API 行为升级（无 break）

```lua
-- Phase E.9: no-op，仅记录标志位
-- Phase E.10: 真正生效，控制 blur pass 是否执行
SSR.SetBlurEnabled(true)
```

### 3.3 Backend 接口新增（2 个）

```cpp
/// 创建 half-res blur ping-pong RT (RGBA16F × 2)
/// @param wFull, hFull   full-res 输入（内部自动除 2）
/// @param outBlurFbos[2] 输出 FBO 数组
/// @param outBlurTexs[2] 输出 tex 数组
/// @param outBlurW       输出实际 half-res 宽（用户可读）
/// @param outBlurH       输出实际 half-res 高
virtual bool CreateSSRBlurRT(int wFull, int hFull,
                              uint32_t outBlurFbos[2], uint32_t outBlurTexs[2],
                              int* outBlurW, int* outBlurH) { return false; }
virtual void DeleteSSRBlurRT(uint32_t fbos[2], uint32_t texs[2]) {}

/// SSR blur pass: srcTex -> dstFbo (separable Gaussian, axis=0 水平/1 垂直)
/// @param dstW, dstH 目标 RT 尺寸（half-res）
/// @param radius    [0.5, 4.0] texel 半径乘子
virtual void DrawSSRBlur(uint32_t srcTex, uint32_t dstFbo,
                          int dstW, int dstH,
                          int axis, float radius) {}
```

### 3.4 新增 GLSL Shader（1 个，双 profile）

```glsl
// FS_SSR_BLUR (separable Gaussian, 5-tap)
// uniform: uSrcTex (slot 0) + uTexel + uAxis + uRadius
// 不依赖 depth，纯像素空间模糊
```

---

## 4. 集成位置

### 4.1 SSRRenderer::Process 改造

```cpp
void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // ... 前置检查 + blit depth + 取 normalTex (Phase E.9 不变)
    
    backend->DrawSSR(... -> g.reflectFbo);       // raw reflection (full-res)
    
    // ★ Phase E.10 新增：blur pass（条件：blurEnabled && half-res RT 已分配）
    uint32_t finalReflectTex = g.reflectTex;     // 默认无 blur，直接用 full-res
    if (g.blurEnabled && g.blurFbos[0] && g.blurFbos[1]) {
        backend->DrawSSRBlur(g.reflectTex, g.blurFbos[0],
                             g.blurW, g.blurH, 0, g.blurRadius);  // H
        backend->DrawSSRBlur(g.blurTexs[0], g.blurFbos[1],
                             g.blurW, g.blurH, 1, g.blurRadius);  // V
        finalReflectTex = g.blurTexs[1];          // half-res blurred
    }
    
    backend->DrawSSRComposite(finalReflectTex, hdrFbo, w, h, g.intensity);
}
```

### 4.2 Enable / Disable / Resize 联动

- `Enable(w,h)`：创建 reflect RT + **half-res blur RT × 2**（即使 BlurEnabled=false 也分配，简化生命周期）
- `Disable()`：释放 reflect RT + blur RT
- `Resize(w,h)`：触发 reflect + blur RT 全部重建

---

## 5. 任务边界 / 验收标准

### 5.1 任务边界（IN/OUT scope 重申）

| IN | OUT |
|----|-----|
| 1 shader + 2 backend + Process 改造 | per-pixel roughness（需 G-buffer 扩展） |
| 2 新 Lua 函数 + 1 行为升级 | hierarchical mip chain（Unreal 风格） |
| smoke 扩展（+ blur 检查点） | temporal accumulation（motion vector） |
| demo 键位（+B/+ -/+ 调 radius） | 反射阴影遮挡（contact hardening） |
| 文档：6 个 6A 文档完整 | 物理 BRDF GGX cone trace |

### 5.2 验收标准

| 验收项 | 通过条件 |
|--------|---------|
| 编译 | 0 error / 0 new warning（4 平台 incremental） |
| ssr.lua smoke | 全通过；新增 BlurRadius 检查 ≥ 4 条（默认值 + 2 boundary clamp + round-trip） |
| 全量回归 | 8 核心渲染 smoke 全通过 |
| demo headless | exit 0，pcall 防御 OOP self 异常 |
| demo 交互 | B 键切 blur，- / = 调 radius，OSD 实时显示 |
| CI 6 平台 build | 全部 success（Linux/iOS/Web/Android/macOS/Windows） |
| Backward compatibility | Phase E.9 旧代码 `SetBlurEnabled(true)` 行为从 no-op 变生效（视觉差异允许） |
| 视觉验证 | demo 中开启 blur 后反射边缘明显柔化 |

---

## 6. 不确定性已解决清单

| 不确定性 | 解决方式 |
|---------|---------|
| blur RT 分辨率 | 用户拍板 half-res |
| blur 算法 | 默认 separable Gaussian 5-tap |
| blur 范围/默认 | [0.5, 4.0] / 1.5 |
| BlurEnabled 默认 | false（保留 Phase E.9 行为） |
| API 增量 | +2（总 24） |
| backend 增量 | +2 接口 + 1 shader |
| 内存代价 | 1080p +4 MB（可接受） |
| 性能代价 | ~0.3 ms 1080p（< 10%） |
| 集成点 | Process 中 DrawSSR 与 DrawSSRComposite 之间 |
| 资源生命周期 | Enable 时分配 / Disable 时释放（即使 BlurEnabled=false） |
| Legacy backend | 自动 no-op（SupportsSSR()=false） |
| 反射 upscale 方式 | 硬件 bilinear filter（composite 阶段自动） |

---

## 7. 启动信号

✅ 所有决策点已确认  
✅ 任务边界清晰  
✅ 验收标准可测  
✅ 与现有 Phase E.9 架构对齐

**下一步**：进入阶段 2 Architect → 生成 DESIGN_PhaseE_10.md
