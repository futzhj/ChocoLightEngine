# Phase F.0.10.8.5 — HDR LUT 完整 PLAN

> 6A 简化 (ALIGN + DESIGN + TASK 合并, 工作量 ~2.5h)

---

## 1. ALIGN — 任务对齐

### 目标

支持 HDR-domain LUT (`.cube` 中 `DOMAIN_MAX > 1.0` + 16-bit PNG HALD), 让 ChocoLight 与 Resolve / ACES workflow 兼容. 美术从 Resolve 导出 ACEScct/Rec.2020 项目时, LUT 输出范围会 > 1.0 (如 [0, 4]); 当前 8-bit byte 路径直接 clamp 到 [0,1] 损失高光信息.

### 边界

**In**:
- `IRenderBackend::CreateLUT3DFloat(size, float*)` 默认 0 + gl33 GL_RGB16F 实现
- `.cube` parser 解析 `DOMAIN_MIN` / `DOMAIN_MAX` (3 floats), max > 1.0 → 走 float 路径
- HALD `.png` 检测 16-bit (`stbi_is_16_bit_from_callbacks` / `stbi_is_16_bit`) → stbi_load_16 → float
- `HDRRenderer::CreateLUT3DFloat(size, float*, dataLen)` Lua 友好包装 (可选, 推迟)
- 自动透明: 用户调 `LoadCubeLUT(path)` / `LoadHaldLUT(path)` 不需变, parser 自动选 byte / float 路径

**Out**:
- 32-bit float LUT (RGB32F) — 仅高端桌面, GLES3 不普及
- shader HDR 路径 — 已无 clamp 输出, sampler3D 通透 (RGB8 / RGB16F 透明), 不需改
- 多 backend (Metal/Vulkan/WebGPU 当前未实现 LUT3D)
- 新 Lua fn — `LoadCube/HaldLUT` 自动判 HDR, 用户透明

### 决策矩阵 (10 项, 全自动)

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | backend 接口扩展 | 加新 fn `CreateLUT3DFloat` | 0 破坏现有 RGB8 路径 |
| 2 | float 类型 | `const float*` (vs uint16_t) | API 简洁, 内部转换 |
| 3 | GL 内部格式 | `GL_RGB16F` | 移动端 (GLES3 / EXT_color_buffer_half_float) 普及 |
| 4 | DOMAIN 检测 | parser 解析 max(R/G/B), > 1.0 即 HDR | 标准 .cube 1.0 定义 |
| 5 | HDR 路径数据存储 | `std::vector<float>` (vs 双缓冲) | 简单, 解析时只填 floats; LDR 路径仍 bytes |
| 6 | LDR fallback | DOMAIN_MAX <= 1.0 → 仍走 byte 路径 (已支持) | 性能 + 兼容老 backend |
| 7 | HALD 16-bit 检测 | `stbi_is_16_bit(path)` | stb_image 内置, 一行 |
| 8 | HALD 16-bit 转 float | `(uint16) / 65535.0f` 归一化 | 16-bit PNG 默认范围 [0, 1], 不 HDR |
| 9 | Lua API | 不加新 fn (透明) | KISS, parser 自动判 |
| 10 | shader 改动 | 不需 (RGB16F sampler 通透) | 现有 tonemap_fs.glsl 已无输出 clamp |

---

## 2. DESIGN — 接口设计

### 2.1 render_backend.h (扩 IRenderBackend)

```cpp
/**
 * @brief Phase F.0.10.8.5 — 创建 HDR float 3D LUT (RGB16F)
 *
 * 与 CreateLUT3D 同语义但 data 为 float* (HDR 范围, e.g. DOMAIN > 1.0).
 * 默认实现 (Legacy): no-op 返 0u (老 backend 不支持 HDR LUT 时 hdr_renderer 自动 fallback).
 *
 * @param size 边长 [4, 64], 与 CreateLUT3D 一致
 * @param data size^3 * 3 floats (R 最快变, GL byte order)
 * @return GL texture id (>0 成功; 0 失败 — 不支持 RGB16F / OOM)
 */
virtual uint32_t CreateLUT3DFloat(int /*size*/, const float* /*data*/) { return 0; }
```

### 2.2 render_gl33.cpp 实现

```cpp
uint32_t CreateLUT3DFloat(int size, const float* data) override {
    if (size < 1 || !data) return 0u;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return 0u;
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F,    // ← float 内部格式
                 size, size, size,
                 0, GL_RGB, GL_FLOAT, data);     // ← GL_FLOAT 上传
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    const GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_3D, 0);
    if (err != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        CC::Log(CC::LOG_WARN,
                "GL33: CreateLUT3DFloat failed (size=%d, gl err=0x%x)", size, err);
        return 0u;
    }
    return (uint32_t)tex;
}
```

### 2.3 hdr_renderer.cpp 解析

**.cube parser HDR 路径**:
- 解析 `DOMAIN_MIN x y z` / `DOMAIN_MAX x y z` 三 float
- 默认 min = (0,0,0), max = (1,1,1)
- max(domainMax.r/g/b) > 1.0 → 标记 isHDR = true
- 数据行: 同时填 `bytes` (clamp[0,1] 量化) + `floats` (原值)
- step 9: isHDR → CreateLUT3DFloat(size, floats.data()); else → CreateLUT3D(size, bytes.data())
- LDR 时只填 bytes, 节省内存 (64³ float = 3 MB vs byte = 0.78 MB)

**HALD 16-bit 路径**:
- stbi_is_16_bit(path) → 16-bit PNG
- stbi_load_16(path, ..., 4) → uint16_t* (RGBA, 0-65535)
- 转 std::vector<float>: `f = u16 / 65535.0f` (归一化 [0, 1])
- 调 CreateLUT3DFloat (HALD 16-bit 也走 float 路径, 即使范围 [0,1])
- 8-bit 路径不变

### 2.4 数据流图

```
.cube file
   ↓ LoadCubeLUTFile
   ↓ SDL_LoadFile
   ↓ LoadCubeLUTFromString
   ↓ parse: SIZE / DOMAIN_MIN / DOMAIN_MAX / data rows
   ↓
   ├─ DOMAIN_MAX > 1.0 ── true ──→ floats[size³*3] ──→ CreateLUT3DFloat ──→ GL_RGB16F
   │                                                                         tex id
   └─ DOMAIN_MAX <= 1.0 ── (LDR) ──→ bytes[size³*3] ──→ CreateLUT3D ─────→ GL_RGB8
                                                                            tex id
HALD .png file
   ↓ LoadHaldLUTFile
   ↓ stbi_is_16_bit(path)
   │
   ├─ true ──→ stbi_load_16 → uint16_t* → floats / 65535.0 ──→ CreateLUT3DFloat
   └─ false ──→ stbi_load → uint8_t* → bytes (drop alpha) ───→ CreateLUT3D
```

---

## 3. TASK — 原子任务

| T# | 内容 | 工作量 | 依赖 |
|----|------|------|------|
| T1 | render_backend.h 加 CreateLUT3DFloat 默认 0u + render_gl33.cpp GL_RGB16F impl | 0.3h | - |
| T2 | hdr_renderer.cpp .cube DOMAIN parsing + isHDR 双路径 + LoadCubeLUTFromString floats vector | 0.6h | T1 |
| T3 | LoadHaldLUTFile 16-bit 检测 + stbi_load_16 + float 转换 | 0.4h | T1 |
| T4 | smoke §19 HDR LUT 5 PASS (DOMAIN_MAX < 1.0 LDR path / DOMAIN_MAX > 1.0 HDR path / 16-bit PNG path / 错误恢复 / 跨平台 fallback) | 0.5h | T2+T3 |
| T5 | demo headless probe (检测 API 可用 / 测 DOMAIN > 1.0 LUT) + commit + CI 6/6 | 0.4h | T4 |
| docs | PLAN + FINAL + TODO | 0.3h | T5 |
| **合计** | | **~2.5h** | |

---

## 4. 验收标准

| 类型 | 标准 |
|------|-----|
| 编译 | Release 通过 (Win 本地 + 6 平台 CI) |
| HDR smoke | 46 → **46 fn (无新 Lua API)**, §19 **5+ PASS** |
| 8 smoke | 零回归 |
| demo headless | 22 → **24+ PASS** |
| Lua API | 72 (不变, 透明扩展) |

---

## 5. 风险

- **中**: GL_RGB16F 浮点上传格式校验 — 需测 `GL_FLOAT` source format + `GL_RGB` external format 兼容
- **低**: stbi_load_16 在 stb_image 标准 (15+ 年稳定 API)
- **低**: SDL3 / size_t / cstddef 之前已修, 已经稳

风险缓解: T1 完成后单独跑 1 次 CI 验 6 平台 GL_RGB16F 兼容 — 但流程上等 T5 commit 一次性 push 即可.
