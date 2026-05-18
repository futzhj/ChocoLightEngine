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

### 2.2 Magic 检查性能 benchmark
- **目标**: 测量 magic 校验开销 < 1% 总时间
- **方法**: 
  - Lua benchmark 1000 次调用 (`image:GetWidth()` etc.)
  - C++ benchmark 100 万次 `LT::CheckInstance`
- **预估**: 1h

### 2.3 静态分析工具
- **目标**: 自动检测未加 magic 的 ctx struct
- **方法**: 编写 clang-tidy custom check 或 grep 脚本
- **预估**: 3h

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

1. **优先级 P1** (推荐立即做):
   - 1.1 Animation pointer-holder (1.5h) — 闭合 Animation 安全
   - 1.2 Physics3D 剩余 4 struct (1h) — 闭合 Physics 安全
2. **优先级 P2** (可选):
   - 1.3 MaterialDesc 间接保护 (2h) — 间接增强 Material
   - 2.1 fuzz smoke 扩充 (2h) — 完整测试覆盖
3. **优先级 P3** (锦上添花):
   - 2.2 perf benchmark (1h)
   - 3.1 公共 API 文档 (1h)
   - 3.2 审计报告 (1h)
   - 2.3 静态分析工具 (3h)

**总计可选工时**: ~13h

---

## 6. 联系点

如需后续推进, 请提供以下问题的回答:
- 是否需要补 Animation pointer-holder 改造?
- 是否需要补 Physics3D 剩余 4 struct?
- 是否需要扩展到 100+ fuzz smoke?
- 是否需要 perf benchmark?
