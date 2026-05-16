# Phase F.0.10.8.6 — HDR LUT 能力探测 PLAN

> 6A 极简 (1 doc 合并 ALIGN+DESIGN+TASK, 工作量 ~0.5h)

---

## 1. ALIGN — 目标

F.0.10.8.5 加 HDR LUT (RGB16F + DOMAIN > 1.0 + 16-bit PNG) 内部自动 fallback 透明, 但 Lua 用户无法**主动查询** backend 是否真支持 HDR LUT. 美术 / UI 需明确状态:
- 显示 "HDR LUT supported" 徽章
- 选择性提供 LDR / HDR .cube 文件
- 自动化测试可断言 HDR 路径生效

加 1 Lua fn `HDR.SupportsHDRLUT() → bool`. 透传 backend `SupportsLUT3DFloat()`, 与 F.0.10.8.5 透明 fallback 互补 (能力查询 + 自动 fallback 双轨).

## 2. DESIGN — 接口

### 2.1 backend (render_backend.h / render_gl33.cpp)
```cpp
virtual bool SupportsLUT3DFloat() const { return false; }   // Legacy 默认
bool SupportsLUT3DFloat() const override { return true; }   // gl33
```

### 2.2 hdr_renderer.h / .cpp
```cpp
bool SupportsHDRLUT();    // 透传 backend; backend null → false
```

### 2.3 Lua (light_graphics.cpp)
```lua
HDR.SupportsHDRLUT() → boolean
```
注册到 hdr_funcs[] (HDR 46 → 47 fn).

## 3. TASK

| T# | 内容 | 工作量 |
|----|------|------|
| T1 | backend SupportsLUT3DFloat 默认 false + gl33 override true | 0.05h |
| T2 | hdr_renderer SupportsHDRLUT 透传 | 0.05h |
| T3 | l_HDR_SupportsHDRLUT + hdr_funcs[] | 0.1h |
| T4 | smoke fn_names + §20 1 PASS + demo +1 PASS | 0.15h |
| T5 | docs + commit + CI 6/6 | 0.15h |
| **合计** | | **~0.5h** |

## 4. 验收

| 类型 | 标准 |
|------|-----|
| 编译 | Release 通过 + CI 6/6 |
| HDR smoke | 46 → **47 fn**, §20 1 PASS |
| 8 smoke | 零回归 |
| demo | 23 → **24 PASS** |
| Lua API | 72 → **73** (+1) |

## 5. 风险

- **极低**: 单一 bool 透传 fn; 模式与现有 `SupportsBloom()` / `IsSupported()` 完全一致
- 跨平台: GL_RGB16F + 3D texture 在 GL3.3 + GLES3 + WebGL2 + iOS GL 都支持, 一律 true
