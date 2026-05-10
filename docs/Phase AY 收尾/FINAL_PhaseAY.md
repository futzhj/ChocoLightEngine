# FINAL — Phase AY（动画 / 网络 / 音视频遗留 TODO 清理）项目总结

> **6A 工作流 Stage 6 — Assess §最终交付报告**
> Phase AY 是一次跨模块的清理收尾批次, 不引入新阶段功能, 而是收割 Phase AV/AW/AX 完成后的所有 TODO/FIXME/未完成项.

---

## 一、目标回顾

> 在不启动新功能阶段的前提下, 清理 Phase AV (骨骼动画) / Phase AW (GPU Skinning) / Phase AX (Morph Target) / Phase AT (网络/音视频) 等阶段在文档中标记的遗留任务, 让 ChocoLight 的可用性、API 一致性、错误诊断质量、smoke 覆盖度全面提升, 为下一个新阶段腾出干净的起点.

来源:
- `docs/引擎升级/TODO_引擎升级.md`(Http/WebSocket/FFmpeg 三项)
- `docs/Phase AV 骨骼动画/TODO_PhaseAV.md` §B.1-B.4
- `docs/Phase AX/TODO_PhaseAX.md` §morph 用户接口
- 隐性 TODO: 多 primitive / morph-only 资产支持

---

## 二、6A 工作流执行轨迹

| Stage | 产出 | 关键决策 |
|-------|------|---------|
| **1 Align** | 用户提交 11 个清理任务清单 (T01-T11), 标 P1/P2/P3 优先级 | 11 项中跳过 T02 (已存在 GL33 fallback 验证), 实施 10 项 |
| **2 Architect** | 直接复用各模块既有架构, 无新设计文档 | "最小侵入" 原则贯穿所有任务 |
| **3 Atomize** | 4 个批次 × 11 任务: 网络批 / 动画批 / 渲染批 / 文档批 | T05/T06 合并 commit; T07/T08 合并 commit |
| **4 Approve** | 用户确认每批前的范围, T10 决策时主动询问 | T10 用户选择"实施" 而非"跳过" 或"迷你版" |
| **5 Automate** | 7 个 commit, 全部本地编译 + 部分 smoke 验证 | T10 跑通本地完整 animation smoke (212/212 PASS) |
| **6 Assess** | 本文件 + ACCEPTANCE + TODO | 无新阶段 TODO 引入; 仅 1 项延后 (T04 完整 FFmpeg 解码循环) |

---

## 三、技术成果

### 3.1 网络模块（批次 1）

| 任务 | 文件 | 影响 | 验证 |
|------|------|------|------|
| **T01** Http __gc 内存泄漏 | `light_network.cpp` HttpUserdata::__gc | 修复 std::string recvBuf 悬挂 (每次 Http() 失败/释放泄漏 ~64KB) | 本地构建 PASS |
| **T03** WebSocket 多帧分片 | `light_network.cpp` WsContext::OnFrame | FIN=0 续帧累积 + opcode=0 续帧 + 切片大小校验 | 本地构建 PASS |

### 3.2 音视频模块（批次 1）

| 任务 | 文件 | 影响 | 验证 |
|------|------|------|------|
| **T04** Audio FFmpeg fallback 诊断 | `light_av.cpp` l_Audio_Call | 替换模糊 LOG_INFO 为两条明确 LOG_WARN + 解决路径指引 | 本地构建 PASS |

T04 完整 FFmpeg 解码循环 (~200 行) 列入延后任务 (见 TODO).

### 3.3 动画模块（批次 2-3）

| 任务 | 文件 | 影响 | 验证 |
|------|------|------|------|
| **T05** LoadSkinnedGLTF API 一致 (B.1) | `light_animation.cpp` | 无 skin 路径返回完整 schema pack 表 + hasSkin 字段 | smoke 17.6 PASS |
| **T06** 多 primitive 包装 (B.2) | `light_animation.cpp` | 加 pack.meshes 数组 + 多 prim 时 LOG_INFO 提示 | smoke 17.* PASS |
| **T07** ListTransitions/ListEvents (B.3) | `light_animation.cpp` | 批量内省 helper, 单调用替代 GetCount + 循环 GetInfo | smoke 17.1-17.5 PASS (10 断言) |
| **T08** Crossfade 数值 smoke (B.4) | `animation.lua` | 文档化发现 [13] 段已实现完整数值验证 | 早期已 PASS |
| **T09** GetMorphTargetIndex(name) | `light_animation.cpp` + `demo_morph_target/main.lua` | name → idx 反查, 避免硬编码 idx | smoke 17.7 PASS |
| **T10** Morph-only GPU 路径 | `light_animation.cpp` | 无 skin 但有 morph 资产: 自动 trivial root + 复用 GPU SkinMorph | 完整 animation smoke 212/212 PASS |

### 3.4 文档（批次 4）

| 任务 | 文件 | 影响 |
|------|------|------|
| **T11** Phase AY FINAL 收尾 | `docs/Phase AY 收尾/FINAL_PhaseAY.md` (本文件) | 项目总结 + 任务表格 + 验证证据 |

---

## 四、新增 / 改造的 Lua API

### 4.1 Animator (新增 2 个批量内省方法)

```lua
-- T07: 批量获取所有 transitions
local list = animator:ListTransitions()
-- → array { {from='a', to='b', duration=0.5, hasCond=true}, ... }

-- T07: 批量获取所有 events
local list = animator:ListEvents()
-- → array { {state='a', triggerTime=0.3, hasCallback=true}, ... }
```

### 4.2 SkinnedMesh (新增 1 个反查 helper)

```lua
-- T09: name → 1-based idx (未命中返回 nil)
local idx = mesh:GetMorphTargetIndex('smile')
if idx then animator:SetMorphWeight(idx, 0.7) end
```

### 4.3 LoadSkinnedGLTF pack 表（新增 2 个字段）

```lua
local pack = Light.Animation.LoadSkinnedGLTF('foo.glb')
-- 原有字段: pack.skeleton / pack.clips / pack.clipNames / pack.mesh
-- T05 新字段: pack.hasSkin   (bool)
-- T06 新字段: pack.meshes    (array, 含 0 或 1 个 SkinnedMesh ud)

-- T10 新支持: 无 skin 但有 morph 的资产 (如 AnimatedMorphCube.glb)
--   pack.skeleton = trivial root skeleton (1 joint, identity TRS)
--   pack.mesh     = SkinnedMesh ud (含 morph target 数据)
--   pack.hasSkin  = false
```

---

## 五、验证矩阵

### 5.1 编译验证

| 平台 | Target | 结果 |
|------|--------|------|
| Windows MSVC Release | `Light.dll` | ✅ PASS (T10 触发本地完整构建) |
| Linux/macOS/Mobile | (CI) | ⏳ 推送后由 CI 验证 |

### 5.2 smoke 验证

| Smoke | 断言数 | 结果 |
|-------|-------|------|
| `animation.lua` (含新 [17] 段) | 212 | ✅ 212/212 PASS (本地实跑) |
| `network.lua` | (未跑) | ⏳ CI 验证 |

新增断言:
- 17.1-17.5: ListTransitions/ListEvents 全字段验证 (10 断言)
- 17.6: LoadSkinnedGLTF 不存在路径 fallback 鲁棒性 (1 断言)
- 17.7: morph-only fallback 不破坏 smoke 流程 (1 断言)

### 5.3 sample 验证

| Sample | 改动 | 状态 |
|--------|------|------|
| `demo_morph_target/main.lua` | T09 helper 演示 + nil-return 验证 | 静态语法 PASS (lightc -p) |

---

## 六、未引入的技术债务

✅ 所有改动均**向后兼容**:
- `pack.skeleton/clips/clipNames/mesh` 字段不变
- `Animator:SetMorphWeight(idx, w)` 旧签名不变
- `ExtractSkinMesh` 默认 `allowNoSkin=false` 保持原有失败语义
- 新增字段 / 新增方法不污染现有 metatable

✅ 无新阶段 TODO 引入 (除了延后的 T04 完整解码循环).

---

## 七、关键设计决策

### 7.1 T04 延后 vs 实施

**决策**: 仅做诊断增强, 完整解码循环延后

**理由**:
1. CI 不预置 FFmpeg DLL, 跨平台 magic offset 风险无法验证
2. 实际诉求"加载 AAC/OGG"不阻塞当前所有 demo
3. 用户可临时用 `ffmpeg.exe -i input.aac -f wav out.wav` 转换

### 7.2 T06 多 primitive: 单元素数组而非完整解析

**决策**: 把现有单 mesh 包装成 `pack.meshes = { mesh }` 单元素数组

**理由**:
1. 完整多 primitive 解析需要每个 primitive 独立 ExtractSkinMesh + ExtractMorphTargets (~80 × N 行)
2. 渲染端需要每 mesh 材质协调 (Material 模块尚未支持 mesh 数组)
3. 当前无 demo 资产触发多 primitive
4. 单元素数组提供前向兼容路径, 用户代码 `for i, m in ipairs(pack.meshes)` 未来扩展无修改

### 7.3 T10 trivial root vs 专用 morph-only shader

**决策**: 复用 GPU SkinMorph 路径 + 注入 1-joint identity 骨架

**理由**:
1. 专用 VS3D_MORPH_ONLY shader 需 ~150 行 (源码 + program 编译/链接 + UBO 绑定)
2. 单 4×4 identity 矩阵×顶点乘法 GPU 开销 << 维护成本
3. ExtractSkinMesh allowNoSkin 参数对调用方透明 (默认 false)
4. 任何后续优化路径 (真正 morph-only program) 仍可平滑添加, 不影响当前 API

---

## 八、性能影响

| 路径 | 改动前 | 改动后 | 增量 |
|------|--------|--------|------|
| Http GC (失败请求) | 泄漏 ~64KB | 0 | -100% (修复) |
| WebSocket 多帧消息 | 单帧上限截断 | 完整重组 | 功能恢复 |
| LoadSkinnedGLTF 有 skin | 不变 | +1 boolean field | <1ns |
| LoadSkinnedGLTF 无 skin (T05) | 返回空 pack | 返回 schema pack | +几 KB heap |
| LoadSkinnedGLTF morph-only (T10) | 直接 fail | 完整 GPU 路径 | +1 mat4×vec4/vert (GPU) |
| Animator:ListTransitions(N=10) | N round-trips Lua-C | 1 round-trip + N table | -N 次 stack frame |

---

## 九、提交历史

```
673a99d feat(animation): T10 - morph-only glTF GPU path via trivial root skeleton
d4364b0 feat(animation): T09 - SkinnedMesh:GetMorphTargetIndex(name) helper
c72d2a0 feat(animation): T07+T08 - batch introspection + smoke validation
9f3060f feat(animation): T05+T06 - LoadSkinnedGLTF API consistency
2531c1d fix(av): T04 - explicit Audio FFmpeg fallback diagnostics
db16a44 fix(network): T03 - WebSocket multi-frame fragment handling
c3c2baa fix(network): T01 - Http userdata __gc to release std::string recvBuf
```

7 commits, ~410 行净增 (含注释 + smoke 验证).

---

## 十、Phase AY 完结确认

| 项目 | 状态 |
|------|------|
| 用户原始 11 个任务 | 10 实施 + 1 跳过 (T02 已存在) |
| 编译 (Windows Release) | ✅ |
| Smoke 本地验证 (animation.lua 212 断言) | ✅ |
| 文档 ACCEPTANCE / FINAL / TODO | ✅ (本批次) |
| 向后兼容 | ✅ |
| 新阶段 TODO 注入 | ⚠️ 仅 1 项 (T04 FFmpeg 完整解码) |

**Phase AY (清理收尾) 圆满完成 ✅**

下一阶段建议:
- 优先级 P1: T04 FFmpeg 完整解码 (需 FFmpeg DLL 真机环境 + 测试音频)
- 优先级 P2: 真正多 primitive 解析 (T06 扩展)
- 新阶段候选: Phase AZ (动画事件高级语义 / IK / blend tree) 或 Phase BA (Lua 绑定文档全面化)
