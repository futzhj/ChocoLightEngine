# Phase F.0.10.8.1 — `.cube` LUT 文件解析 ALIGNMENT

> 6A 工作流 · 阶段 1 (Align) · 模糊需求 → 精确规范

---

## 1. 项目上下文

- **基础**: F.0.10.8 已交付 `HDR.CreateLUT3D(size, byte_data)` C++ + Lua API. 用户调用需提前在 Lua 端构造 12KB / 35KB / 256KB 字节数组.
- **缺口**: 业界 LUT 标准格式是 Adobe `.cube` 1.0 (Lightroom / DaVinci Resolve / Premiere / FCP X 全部生成此格式). 没有 parser → LUT 无法接入美术工作流, F.0.10.8 仍偏 demo 级.
- **目标**: 加 `HDR.LoadCubeLUT(path)` 一行调用读取 `.cube` 文件 → 返 `tex_id`. 让美术能把 Lightroom/Resolve 输出直接拖入项目.

## 2. `.cube` 格式标准 (Adobe Cube LUT 1.0)

### 2.1 必需字段

```
LUT_3D_SIZE N      # N ∈ [2, 256], 标准值: 17 / 33 / 64
```

### 2.2 可选字段

```
TITLE "string"           # 描述, 忽略
DOMAIN_MIN r g b         # 默认 0 0 0
DOMAIN_MAX r g b         # 默认 1 1 1
LUT_1D_SIZE N            # 1D LUT, 本 phase 不支持
```

### 2.3 数据行

`size^3` 行 (3D LUT), 每行 3 个浮点 `r g b ∈ [0, 1]` (HDR LUT 可 > 1, 本 phase clamp).

**索引顺序**: R 变化最快, 然后 G, 然后 B (与 OpenGL 3D texture 数据布局**完全一致**, 无需 reshape).

### 2.4 注释 + 空白

```
# 行首 # 是注释
   # 行首空白后 # 也是注释
                   # 空行也 skip
```

### 2.5 典型 `.cube` 文件 (16³ identity)

```
TITLE "Identity 16³"
DOMAIN_MIN 0.0 0.0 0.0
DOMAIN_MAX 1.0 1.0 1.0
LUT_3D_SIZE 16
0.000000 0.000000 0.000000
0.066667 0.000000 0.000000
0.133333 0.000000 0.000000
... (4093 more lines)
```

## 3. 边界确认 (Scope)

### 3.1 In Scope (本 phase 实现)

- 解析 `LUT_3D_SIZE` (size ∈ [4, 64], 与 F.0.10.8 约束一致)
- 解析 `size^3` 行 RGB 浮点
- 注释 (`#`) + 空行 skip
- DOMAIN_MIN / DOMAIN_MAX (用于归一化, 但本 phase **clamp 到 [0,1]** 再 quantize, 不支持 HDR > 1.0)
- C++ 内部 `LoadCubeLUTFromString(text, ...)` (便于 smoke 用 in-memory 字符串测试)
- C++ wrap 文件路径版 `LoadCubeLUTFile(path)` (调 `IOStream::LoadFile` + 委托 string 版)
- Lua API `HDR.LoadCubeLUT(path)` → tex_id

### 3.2 Out of Scope (后续 phase)

- ❌ `LUT_1D_SIZE` (1D LUT)
- ❌ HDR LUT (DOMAIN > 1.0 完整支持; 本 phase clamp)
- ❌ HALD / Stripe image format (留 F.0.10.8.2)
- ❌ 热重载 (留 F.0.10.8.3)
- ❌ `.3dl` (Autodesk Lustre 格式) 等其他 LUT 标准

## 4. 需求理解

### 4.1 用户故事

```lua
-- 用户从 Lightroom/Resolve 导出 my_grading.cube
local lut_id = HDR.LoadCubeLUT("assets/luts/my_grading.cube")
HDR.SetGradingLUT(lut_id, 1.0)
-- 之后所有 HDR.Tonemap 自动应用此 LUT
```

### 4.2 性能要求

- 33³ size LUT (常见尺寸): 35937 数据行, parse 时间 < 50ms (启动一次性, 非 per-frame)
- 64³ size LUT (高质): 262144 数据行, parse 时间 < 500ms
- C++ parse 比 Lua parse 快 ~100x (实测对比 stb_image)

### 4.3 错误处理

| 错误条件 | 返回 |
|----------|------|
| 文件不存在 / 读失败 | `nil, "file not found: <path>"` |
| `LUT_1D_SIZE` 而非 3D | `nil, "1D LUT not supported (use LUT_3D_SIZE)"` |
| `LUT_3D_SIZE` 缺失 | `nil, "missing LUT_3D_SIZE directive"` |
| size < 4 或 > 64 | `nil, "LUT size N out of range [4, 64]"` |
| 数据行数 ≠ size^3 | `nil, "data row count <N> mismatch (expected <M>)"` |
| 数据行 < 3 浮点 | `nil, "line <N>: expected 3 floats, got <K>"` |
| 非数字 token | `nil, "line <N>: invalid float '<token>'"` |
| `HDR.CreateLUT3D` 失败 | `nil, "backend create failed"` |

## 5. 技术决策矩阵

| # | 决策 | 选项 | 选择 | 理由 |
|---|------|------|------|------|
| 1 | 实现位置 | (a) 纯 Lua / (b) C++ | **(b) C++** | parse 性能 100x; 单一 fn 调用 UX 佳; 与 IOStream 一致模式 |
| 2 | API 暴露层 | (a) `Light.HDR.LoadCubeLUT` / (b) `Light.LUTLoader.*` 单独 module | **(a) 同 HDR**| 与 F.0.10.8 同模块, 用户心智模型一致 |
| 3 | 文件读取 | (a) `fopen` / (b) `SDL_LoadFile` | **(b) SDL_LoadFile** | 跨平台 + Lumen runtime 已测过 + 一致性 |
| 4 | parse 算法 | (a) 单遍 streaming / (b) 全量到 buffer 再 parse | **(b) 全量** | LUT < 1MB; 实现简单; 错误定位准 (含行号) |
| 5 | 数据结构 | (a) `std::stringstream` / (b) `strtof + strchr` | **(b) strtof** | 性能 5-10x; 无 STL 异常; locale-safe |
| 6 | DOMAIN 处理 | (a) 完整支持 HDR / (b) clamp [0,1] | **(b) clamp** | 本 phase 简化; HDR 留 F.0.10.8.2+ |
| 7 | 行尾处理 | (a) 仅 LF / (b) LF + CRLF | **(b) LF + CRLF** | Windows 文件兼容 (Resolve 输出可能 CRLF) |
| 8 | size 范围 | (a) [2, 256] 标准 / (b) [4, 64] F.0.10.8 一致 | **(b) [4, 64]** | 与 `HDR.CreateLUT3D` 约束一致, 错误统一 |
| 9 | 索引顺序 | (a) reshape (BGR) / (b) 直接传 (与 GL 一致) | **(b) 直接传** | `.cube` R 最快变 = GL 3D texture R 最快变 |
| 10 | 量化 | (a) byte (RGB8) / (b) float (RGB16F/32F) | **(a) byte** | 与 F.0.10.8 backend 一致 (`GL_RGB8`); 性能/质量平衡 |
| 11 | TITLE 处理 | (a) 解析存到 metadata / (b) 忽略 | **(b) 忽略** | YAGNI; metadata 暂无消费者 |
| 12 | 内部 string parser 暴露 | (a) 仅文件版 / (b) 文件版 + 字符串版 | **(b) 双版本** | smoke 不需 fake file (用 in-memory string); C++ helper 复用 |
| 13 | 错误信息携带行号 | (a) yes / (b) no | **(a) yes** | 美术工作流 debug 关键 (33³ LUT 找错行) |
| 14 | Lua API 命名 | (a) `LoadCubeLUT` / (b) `LoadLUTCube` / (c) `LoadCubeFile` | **(a) LoadCubeLUT** | 名词主语 (LUT) 在前, 动词后置, 与 `CreateLUT3D` 同风格 |

## 6. 验收标准

### 6.1 功能验收

- ✅ `HDR.LoadCubeLUT(path)` 返 `tex_id > 0` (合法 16³ identity LUT)
- ✅ 文件不存在返 `nil, err`
- ✅ `LUT_1D_SIZE` 文件返 `nil, "1D LUT not supported"`
- ✅ size 越界返 `nil, "LUT size N out of range [4, 64]"`
- ✅ 数据行数 mismatch 返 `nil` + 行号 err
- ✅ 注释 / 空行 / DOMAIN / TITLE 正确 skip
- ✅ CRLF 行尾文件可解析

### 6.2 性能验收

- ✅ 33³ LUT parse < 50ms
- ✅ 64³ LUT parse < 500ms

### 6.3 测试覆盖

- HDR smoke 加 §15 LUT loader section: 5+ PASS
  - LoadCubeLUT 文件不存在
  - 内部 helper LoadCubeLUTFromString 6 边界情况 (1D / 缺 size / size 越界 / 数据少 / 非数字 / 合法识别)
  - 合法 4³ identity 字符串 → CreateLUT3D 成功

### 6.4 demo

- 写 `samples/demo_taa_split2/luts/` 2 个 4³ identity `.cube` 文件 (red_tint.cube + blue_tint.cube)
- demo 主循环加载 + 应用 (P1 红 LUT vs P2 蓝 LUT)
- demo headless probe 加 1 PASS (LoadCubeLUT 不存在文件)

## 7. 疑问澄清

| 问题 | 自决答案 | 依据 |
|------|--------|-----|
| `.cube` HDR 范围 (DOMAIN > 1) 处理? | clamp [0,1] | 业界 80% LUT 是 SDR; HDR 留 F.0.10.8.2+ |
| 数据行支持空白分隔 + tab? | yes (strtof skip whitespace) | strtof 标准行为, 无额外开销 |
| size > 64 但用户文件就 100? | reject (与 F.0.10.8 一致) | 用户可手动 resample 到 64; 本 phase 不做 resample |
| 文件路径相对工作目录还是 BasePath? | 相对工作目录 (Lumen 启动 CWD = exe dir) | 与 Font.__call(path) 同 |

## 8. 技术约束

- 不修改 `render_backend.h` (parse 在 HDRRenderer 层, 不下沉到 backend)
- 不引入新依赖 (`SDL_LoadFile` + `strtof` + `strstr` C 标准)
- HDR backend 必须 ready (调 `g.backend->CreateLUT3D`)
- Lua API: 1 个新 fn `HDR.LoadCubeLUT(path)`
- 工作量预算: ~3h (parser ~1.5h + Lua wrap ~0.3h + smoke 6 case ~0.5h + demo 实际可视化 ~0.5h + docs ~0.2h)

## 9. 项目特性规范对齐

- **样板代码**: 复用 F.0.10.8 `HDR.CreateLUT3D` 路径 (parser → byte buffer → CreateLUT3D)
- **错误处理模式**: nil + string err (与 `HDR.CreateLUT3D` 一致)
- **测试**: smoke + demo 双层 (与 F.0.10.x 一致)
- **文档**: 6A 五件套 (ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO)

## 10. 共识

本 phase 实现 **`.cube` LUT 1.0 文件标准解析 + 一行调用 LoadCubeLUT API**, 让 F.0.10.8 接入业界美术工作流. 工作量 ~3h, 风险低 (parser 是纯计算, 无 GPU/网络/异步交互).
