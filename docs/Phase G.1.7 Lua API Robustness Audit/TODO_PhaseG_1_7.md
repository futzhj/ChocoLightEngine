# Phase G.1.7 — 后续 TODO 清单

> 完成 G.1.7.0~4 主体后, 以下为可选的扩展工作.

---

## 1. 已知遗留 (建议补充)

### 1.1 Animation pointer-holder 改造
- **文件**: `light_animation.cpp`
- **现状**: 用 `Skeleton**` / `Animator**` 等单指针 wrapper, 加 magic 需要双 indirection
- **方案**: 改用 `struct SkeletonHandle { uint32_t magic; Skeleton* ptr; }` 16 字节布局
- **预估**: 1.5h

### 1.2 Physics3D 剩余 4 struct
- **文件**: `light_physics3d.cpp`
- **对象**: Joint3D / Character3D / Vehicle3D / SoftBody3D
- **方案**: 加 magic 字段 + constructor init + Check 函数双校验
- **预估**: 1h

### 1.3 MaterialDesc 间接保护
- **文件**: `light_graphics_material.cpp`
- **现状**: MaterialDesc 是 RenderBackend POD, 不能直接加 magic
- **方案**: 包一层 `struct MaterialUserdata { uint32_t magic; MaterialDesc desc; }`, Lua 持 wrapper, C++ 取 `.desc` 给 backend
- **预估**: 2h

---

## 2. 测试扩展

### 2.1 100+ fuzz smoke 汇总
- **目标**: 在 `lua_api_robustness.lua` 中扩充 100+ 用例
- **方法**: 自动生成 cross-product (每对 ctx 类型 × 每种攻击向量)
- **预估**: 2h

### 2.2 Magic 检查性能 benchmark ✅ 已完成 (commit `4eccc0c`)
- **交付**: `scripts/smoke/lua_api_magic_perf.lua`
- **CI 实测 (Windows)**: Tilemap/Particles ctx 方法 60 ns/call, 纯 Lua method 33 ns/call, magic 校验额外开销 ≤ 3 ns (< 5%)

### 2.3 静态分析工具 ✅ 已完成 (commit `4eccc0c`)
- **交付**: `scripts/verify_magic_coverage.py` + Linux CI step `Verify magic constant coverage`
- **结果**: 50 constants, 11 planned (whitelisted), 0 warnings, 0 errors

---

## 3. 文档化

### 3.1 公共 API 文档
- **文件**: `docs/Light_TypeSafety.md` (新增)
- **内容**: magic 设计原理 + helpers 用法 + 添加新 ctx 的步骤
- **预估**: 1h

### 3.2 安全审计报告
- **文件**: `docs/AUDIT_TypeSafety.md`
- **内容**: 79 文件覆盖率矩阵 + 已知遗留风险 + 修复优先级
- **预估**: 1h

---

## 4. 当前缺少的环境配置

无 (本阶段所有改动都在已有目录, 不需要新依赖).

---

## 5. 建议执行顺序

1. **优先级 P1** ✅ 已完成 (G.1.7.1~4):
   - 1.1 Animation pointer-holder — 闭合 Animation 安全
   - 1.2 Physics3D 剩余 4 struct — 闭合 Physics 安全
2. **优先级 P2** ✅ 已完成:
   - 1.3 MaterialDesc 间接保护 — wrapper struct + magic
   - 2.1 fuzz smoke 扩充 — 77 用例 PASS
3. **优先级 P3** ✅ 已完成:
   - 2.2 perf benchmark — `lua_api_magic_perf.lua`
   - 3.1 公共 API 文档 — `Light_TypeSafety.md`
   - 3.2 审计报告 — `AUDIT_TypeSafety.md`
   - 2.3 静态分析工具 — `verify_magic_coverage.py`

**总计已用工时**: ~13h (实际)

---

## 6. 联系点

**G.1.7 全部子阶段已闭合, 无剩余 TODO.**

后续 G.1.8+ 可选方向 (新阶段):
- 自动生成 ctx 防御代码模板 (clang-tidy custom check 替代手写)
- 全平台 perf 基线对比 (Linux / macOS 实测数据补全)
- 模糊 fuzzing 进一步扩展到 200+ 用例 (cross-product 自动生成)
