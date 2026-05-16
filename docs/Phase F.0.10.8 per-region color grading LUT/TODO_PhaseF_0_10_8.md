# Phase F.0.10.8 — per-region color grading LUT TODO

> 6A 工作流 · 阶段 6 (Assess) · 待办事项
> 关联: `FINAL_PhaseF_0_10_8.md`

---

## 1. 强制 (必须解决)

### 1.1 ⏳ CI 6/6 验证

**状态**: 待 push (commit `4be2598` + SP3 docs commit) GitHub Actions 完成所有平台 build.

**期望平台**:
- ubuntu-latest (Linux GL 3.3)
- macos-latest (macOS GL 3.3 / Metal)
- windows-latest (Windows GL 3.3)
- emscripten (Web GLES 3.0)
- iOS (mobile GLES 3.0)
- Android (mobile GLES 3.0)

**用户操作**: 等待 5-10 分钟后, 检查 https://github.com/futzhj/ChocoLightEngine/actions

**风险点**:
- **GLES 3.0 sampler3D 兼容性**: GLES 3.0 核心规格支持 (无需扩展), 应通过
- **Metal transpile**: 苹果 GLSL → Metal 转换需正确处理 sampler3D (历史无此用例)
- **WebGL 3D texture**: `glTexImage3D` 在 WebGL 2 (GLES 3.0 over Web) 是核心 API

---

## 2. 可选 (建议但非阻塞)

### 2.1 demo 实际 LUT 演示 (推荐)

**目标**: 用 procedural LUT 在 demo_taa_split2 实际渲染中演示 P1 红偏移 vs P2 蓝偏移.

**实施**: 在 main loop 之前 Lua 端构造 2 个 16³×3 字节 LUT:

```lua
-- 红偏移 LUT (P1 黄昏增强)
local function make_red_tint_lut(size, tint)
    local bytes = {}
    for b = 0, size - 1 do
        for g = 0, size - 1 do
            for r = 0, size - 1 do
                bytes[#bytes + 1] = math.min(255, math.floor(r * 255 / (size-1) + tint * 30))
                bytes[#bytes + 1] = math.floor(g * 255 / (size-1))
                bytes[#bytes + 1] = math.floor(b * 255 / (size-1))
            end
        end
    end
    return bytes
end

local p1_lut = HDR.CreateLUT3D(16, make_red_tint_lut(16, 1.0))
local p2_lut = HDR.CreateLUT3D(16, make_blue_tint_lut(16, 1.0))

-- 主循环
HDR.Tonemap(0,    0, W/2, H, {exposure=1.5, tonemap='aces',       lut=p1_lut, lutStrength=0.7})
HDR.Tonemap(W/2,  0, W/2, H, {exposure=0.6, tonemap='uncharted2', lut=p2_lut, lutStrength=0.7})

-- cleanup
HDR.DeleteLUT3D(p1_lut)
HDR.DeleteLUT3D(p2_lut)
```

**工作量**: ~1h

### 2.2 .cube 文件解析 (留下个 phase)

**目标**: Adobe / Resolve LUT 标准格式支持 (`.cube` 文本格式).

**实施**: Lua 端纯文本解析 (parse `LUT_3D_SIZE N` + `r g b` 浮点数行), 转换成 byte array 喂给 `CreateLUT3D`.

**好处**:
- 美术工作流: 用 Lightroom / Resolve 生成 LUT → 直接加载
- 无需新 C++ 接口

**工作量**: ~3h (Lua 端, 不需 C++ 改动)

### 2.3 性能 benchmark

**目标**: 验证 LUT 启用 vs 不启用 perf 差 < 5%.

**工具**: RenderDoc + frame timer

**测试**:
- A: `HDR.Tonemap(rgn)` 无 LUT (uLUTEnabled=0)
- B: `HDR.Tonemap(rgn, {lut=id, lutStrength=0.5})` 含 LUT (sampler3D fetch)

**预期**: 差异 < 5% (现代 GPU uniform branch 零成本, sampler3D trilinear ≈ 1.5x sampler2D)

---

## 3. 用户支持需求

### 3.1 配置 / 环境

- **无新配置**: F.0.10.8 是 API 增量, 不引入新依赖 / 编译标志
- **GLES 3.0+ 默认支持**: 3D texture 是核心规格, 不需扩展

### 3.2 文档使用指引

| 角色 | 推荐阅读 |
|------|---------|
| 业务开发者 (集成 LUT) | `FINAL_PhaseF_0_10_8.md` §2.3 Lua API + TODO §2.1 demo 示例 |
| 引擎开发者 (扩展 .cube/HALD) | `DESIGN_PhaseF_0_10_8.md` + `ALIGNMENT_PhaseF_0_10_8.md` §3 |
| 美术 (LUT 工作流) | 留待 F.0.10.8.1 后写 美术指南 |

### 3.3 调试 / 排查

如 LUT 应用出现问题:

```lua
-- 1. 验证 backend 支持 (CreateLUT3D 返非 0)
local id = HDR.CreateLUT3D(16, identity_data)
print("LUT id =", id)  -- 应该 > 0

-- 2. 验证 SetGradingLUT 状态
HDR.SetGradingLUT(id, 0.7)
print("LUT id/strength =", HDR.GetGradingLUTId(), HDR.GetGradingLUTStrength())

-- 3. 验证 Tonemap 路径
local ok, err = HDR.Tonemap(0, 0, W, H, {lut=id, lutStrength=0.7})
if not ok then print("Tonemap failed:", err) end
```

C++ 侧可在 `uploadTonemapLUTUniforms_` 加日志:

```cpp
CC::Log(CC::LOG_INFO, "LUT bind: tex=%u strength=%.2f useLUT=%d",
        lutTex, lutStrength, useLUT);
```

### 3.4 LUT 标识 LUT 数据生成

LUT 数据布局: `data[((b*size + g)*size + r) * 3 + ch]`, R 变化最快.

```python
# Python 生成 identity LUT (无效果, 用于测试)
size = 16
data = bytearray()
for b in range(size):
    for g in range(size):
        for r in range(size):
            data.append(r * 255 // (size - 1))
            data.append(g * 255 // (size - 1))
            data.append(b * 255 // (size - 1))
# data 长度 = 16*16*16*3 = 12288 bytes
```

---

## 4. 后续 Phase 候选

详见 `FINAL_PhaseF_0_10_8.md` §8.

| Phase | 主题 | 工作量 | 优先级 |
|-------|------|-------|-------|
| **F.0.10.8.1** | `.cube` 文件解析 (美术工作流) | ~3h | 高 (实际可用性) |
| F.0.10.8.2 | HALD / stripe → 3D LUT | ~4h | 中 |
| F.0.10.8.3 | LUT 热重载 | ~2h | 低 |
| **F.1** | DLSS-like TAAU | ~10-15h | 高 (TAA 终极) |
| F.0.10.9 | 真多 HDR target (RT pool) | ~8-10h | 中 |

---

## 5. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_8.md` | 需求 / 假设 / scope / 14 决策矩阵 |
| `DESIGN_PhaseF_0_10_8.md` | 接口契约 + shader 改动 + 数据流 |
| `TASK_PhaseF_0_10_8.md` | 8 原子任务 + 依赖图 + 3 sub-phase |
| `FINAL_PhaseF_0_10_8.md` | 项目总结 + 6 关键决策 + 模板复用 |
| **`TODO_PhaseF_0_10_8.md`** | 本文件 (强制 CI + 可选 demo + 用户支持) |
