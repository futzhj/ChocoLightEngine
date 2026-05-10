# TODO — Phase AX（Morph Target 表情/形状变形）后续待办

> **6A 工作流 Stage 6 — Assess §进一步代办**
> 列出 Phase AX 完成后的剩余待办与用户操作指引。

---

## A. 用户必须确认的待办（Phase AX 关闭前）

### A.1 GitHub Actions CI 6 平台验证 ⏳

**状态**：T1-T8 所有 commit 已提交，待 push + CI 跑完。

**需要你做的**：
```powershell
git push origin main
```

push 完后访问：https://github.com/futzhj/ChocoLightEngine/actions 查看：
- Windows / Linux / macOS 编译 ✅
- Android / iOS 交叉编译 ✅
- Web (Emscripten) WASM 构建 ✅
- Windows runtime smoke 跑通 (期望 196 PASS / 0 FAIL)

如有平台编译失败，根据日志我可以补救（典型：GLSL 跨版本兼容、平台特定头文件、ABI 字段对齐）。

---

### A.2 桌面机器视觉验证 demo_morph_target ⏳

**状态**：sample 文件齐全，未在真机渲染验证。

**需要你做的**：

#### Windows

```powershell
# 1. 等 CI 跑完后下载最新 Light.dll (或本地 cmake build)
# 2. 下载默认资产
.\samples\demo_morph_target\setup.ps1

# 3. 启动 demo
.\Light-0.2.3\windows-x64\light.exe samples\demo_morph_target\main.lua
```

#### Linux / macOS

```bash
chmod +x samples/demo_morph_target/setup.sh
./samples/demo_morph_target/setup.sh
./Light-0.2.3/<platform>/light samples/demo_morph_target/main.lua
```

**期望看到**：

- AnimatedMorphCube 是无 skin 的纯 morph mesh，**Phase AX 不渲染它**（mesh = nil），但 OSD 仍会显示 morph target 名称（8 个 cube 形变）和 weights 变化
- 按数字键 `1-8` 切换 active slot；按 `↑/↓` 调整 weight；按 `C` 清除手动覆盖
- 按 `G` / `N` 切换 GPU/CPU 路径

**如要看 3D 视觉效果**：

需要 SKIN+MORPH 一体的 glTF 资产。Khronos sample 库里没有现成的，建议：

1. 用 Blender 创建/导入含 armature 的角色模型
2. Object Data Properties → Shape Keys 添加 morph target
3. 创建动画给 Shape Keys 加 keyframe
4. File → Export → glTF 2.0 (.glb)，勾选 **Animations → Shape Keys** + **Skinning → Include Armatures**
5. 导出文件命名为 `morph.glb` 放入 `samples/demo_morph_target/assets/`

---

### A.3 是否启动下一阶段？

Phase AX 完成后，Phase AV TODO §C.2 中剩余 2 项候选：

| 选项 | 业务价值 | 实现复杂度 | 推荐度 |
|------|---------|---------|--------|
| Layer（base + override + additive） | 中 | 中 | ⭐⭐⭐ 表情系统会用到 |
| IK（two-bone / look-at） | 高 | 高 | ⭐⭐⭐⭐ 脚踩地形 / 手抓物体场景必需 |

**等你确认**：要启动 Layer 还是 IK，还是其他需求？

---

## B. 我可独立完成的优化（不需用户介入）

### B.1 仅 morph 无 skin 的 GPU 路径

**问题**：当前 Phase AX 仅支持 SKIN+MORPH 共存的 GPU 路径。仅 morph 无 skin 的 mesh（如 AnimatedMorphCube）走 CPU。

**改进**：加新 shader `VS3D_MORPH`（无 skin 部分），扩展 backend `CreateMorphMesh` / `DrawMorphMeshMaterial`，让纯 morph 资产也能 GPU 加速。

**工作量**：~250 行（与 T4 类似但更简单）。

**等你确认**：是否在 Phase AX.x 加这个？

---

### B.2 SetMorphWeight 支持 string name

**问题**：当前 API 只接 number idx，按 name 设置需要用户写 `for i=1,mesh:GetMorphTargetCount() do if mesh:GetMorphTargetName(i)=="smile" ...`。

**改进**：在 `Light.Animation` 模块加 helper：

```lua
-- 新增模块函数
Light.Animation.FindMorphIdxByName(mesh, name) -> integer | nil
```

或在 Animator 加：

```lua
-- 让 Animator 持 weak ref to mesh
animator:BindMesh(mesh)
animator:SetMorphWeight("smile", 0.5)   -- 用 name 也能设
```

**工作量**：~80 行 + smoke 6 PASS。

**等你确认**：是否需要？

---

### B.3 morph delta 后台线程上传

**问题**：当前 `gpuSkinnedMorphMeshUploaded` 是 lazy upload，第一帧 hitch（CPU→GPU 拷贝）可能可见（特别大型模型）。

**改进**：用专门的资源上传线程预加载，主线程绘制时只切换 ID。

**工作量**：~150 行 + 多线程同步代码。

**等你确认**：当前规模下视觉无感，建议**不做**，留作未来性能优化候选。

---

### B.4 TANGENT delta GPU 路径

**问题**：CPU 路径已保留 `tanDelta` 数据但 shader 未消费。当前光照不依赖 tangent（normal 足够），但加 normal map 后会需要。

**改进**：扩展 `VS3D_SKIN_MORPH` 加 `uMorphTanDelta` sampler + 输出 vTangentW。

**工作量**：~30 行 GLSL + ~30 行 backend bind。

**等你确认**：与 normal map 功能同步实施（建议 Phase AX 之后某个 Phase 一并做）。

---

### B.5 性能 benchmark

**问题**：Phase AX 没有性能数据（CPU vs GPU 对比）。

**改进**：参照 `samples/demo_skinning_perf/` 加自动 baseline 模式（60 帧 CPU + 60 帧 GPU + 各 30 帧预热），打印 speedup。

**工作量**：~150 行（沿用 demo_skinning_perf 的 FrameStat 模式）。

**等你确认**：建议用户跑出真机数据后再做，避免空 benchmark。

---

## C. 文档可补充项（我可独立完成）

### C.1 morph target 与 SpriteAnimation 对比

**改进**：在 `docs/api/Light_Animation.md` 加一节"何时用 morph target vs 帧动画 vs Sprite Sheet"。

**工作量**：~50 行 markdown。

---

### C.2 Phase AX troubleshooting 指南

**改进**：常见问题（CPU / GPU 视觉不一致 / glTF 加载失败 / N > 8 截断警告）的故障排查。

**工作量**：~80 行 markdown，可放入 `samples/demo_morph_target/README.md` 或 `docs/api/Light_Animation.md`。

---

## D. 总结

### 现状

- **Phase AX 核心交付完成** ✅（T1-T8 全部 commit）
- **既有功能 0 退化**（smoke 196 PASS / 0 FAIL）
- **设计文档完整**（6A 7 个文档，~5000 行）
- **CI / 视觉验证待外部确认**（A.1 + A.2）

### 优先级

| 优先级 | 项目 | 时机 |
|--------|------|------|
| 🔴 高 | A.1 push 到 CI | 立即 |
| 🟡 中 | A.2 视觉验证 sample | CI 通过后 |
| 🟢 低 | A.3 启动下一 Phase | 视觉验证后 |
| 🟢 低 | B/C 优化项 | 业务真用到时 |

### 下一步

请按以下顺序确认：

1. **执行 `git push origin main`**（A.1）
2. **观察 GitHub Actions 结果**，如有失败截图给我
3. **桌面机器跑 demo_morph_target**（A.2），如视觉异常给我截图
4. **决定下一 Phase**（A.3）：Layer / IK / 其他需求？
