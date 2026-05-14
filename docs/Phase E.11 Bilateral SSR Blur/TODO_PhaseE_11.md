# Phase E.11 Bilateral SSR Blur — TODO（待办与外部依赖）

> **目的**：精简明确哪些事项待办、哪些缺少配置，方便用户直接寻找支持。
> **状态约定**：🔴 阻塞 ｜ 🟡 推荐 ｜ 🟢 可选

---

## 1. 立即可执行（Phase E.11 收尾）

### 1.1 🟡 等 CI 当前 run green
- **Run**：`25862930468` (commit `ebd069b`)
- **预期**：6/6 平台 + Windows runtime smoke 全 green
- **操作命令**：
  ```bash
  gh run watch 25862930468
  # 或
  gh run view 25862930468 --json status,conclusion
  ```
- **green 后操作**：无需额外提交，Phase E.11 自动收官

### 1.2 🟢 真实窗口下手测视觉
- **依赖**：本地有桌面 GL 3.3 显卡 + lumen-master runtime
- **命令**：
  ```bash
  cd lumen-master/build/src/light/Release
  ./light.exe ../../../../../samples/demo_ssr/main.lua
  ```
- **测试要点**：
  - 按 `B` 开启 blur → 按 `V` 切换 Bilateral on/off
  - 观察跨深度边（cube 与地面交界）：bilateral on 应锐利保边，off 应有 leak
  - 按 `,` / `.` 调整 σ：σ↓ 行为接近 Gaussian，σ↑ 跨边断开更激进
- **预期结果**：Bilateral on + σ=200 视觉最干净；Bilateral off 复现 Phase E.10 行为

---

## 2. Phase E.12+ 候选（中期）

### 2.1 🟢 Normal-aware bilateral
- **现状**：仅 depth-aware（DESIGN §3.4.1）
- **限制**：跨大法线变化（如 cube 棱边法线突变）可能略糊
- **方案**：在 bilateral 权重中追加 normal dot product 项
  ```glsl
  float wDepth  = exp(-abs(cDepth - d) * uDepthSigma);
  float wNormal = pow(max(dot(cN, n), 0.0), uNormalSharpness);
  float w = W_i * wDepth * wNormal;
  ```
- **依赖**：复用 Phase E.8.x G-buffer view-space normal RG16F（已存在）
- **成本预算**：+0.05 ms GPU @ 1080p；+1 sampler slot；+2 uniform
- **优先级**：低（当前 depth-aware 已能解 90% 跨边 leak）

### 2.2 🟢 移动端真机 perf 实测
- **现状**：估算 +0.1 ms @ 1080p，未在 GLES3 真机验证
- **建议机型**：Adreno 6xx / Mali G-7x（中端） + Adreno 5xx（低端）
- **测试场景**：demo_ssr 高强度反射场景，Blur on + Bilateral on
- **预期**：低端机 +0.3~0.5 ms，中端 +0.1~0.2 ms
- **如果超预算**：考虑动态降级（FOV 中央 bilateral，边缘 Gaussian）

### 2.3 🟢 Roughness-aware blur radius
- **现状**：统一 blur radius（demo 用 1.5）
- **方案**：从 PBR material roughness 派生 per-pixel σ_spatial
- **依赖**：需先有 PBR 材质系统（Phase F.x 候选）
- **优先级**：低（依赖未来 phase）

---

## 3. Phase E.x 通用增强

### 3.1 🟢 自适应 σ（depth range scaling）
- **现状**：σ 经验值 [50, 500]，对 scene depth 不敏感
- **问题**：远距离场景 σ=200 可能门控过紧；近距离场景同 σ 又过松
- **方案**：从 SSRRenderer.Process 时动态根据 (zFar - zNear) 缩放 σ
- **优先级**：低（用户可通过 SetBlurDepthSigma 手动适配）

### 3.2 🟢 视觉自动回归（perceptual diff）
- **现状**：CI 仅验 smoke（API/数值），不验视觉
- **方案**：引入 `pixel-diff` 工具（如 `pixelmatch`） + 基线图比对
- **依赖**：需要 headless GL 渲染 + 截图工具链
- **优先级**：低（成本高，收益短期不显著）

---

## 4. 已知技术债务（不影响功能）

| 债务 | 影响 | 修复优先级 |
|------|------|-----------|
| `uniform int uBilateral` 用 int 而非 bool | 0（GLSL 标准做法） | 不修 |
| Phase E.11 默认 `BilateralEnabled=true` 但 `BlurEnabled=false` | 0（BlurEnabled 关时无影响） | 不修 |
| `samples/demo_ssr/main.lua` headless 报 Open() table 错误 | 0（已正确捕获 + exit 0） | 不修 |
| 移除 18 份历史 phase docs（已在 ebd069b 完成） | 已解决 | — |

---

## 5. 外部依赖与配置（无）

**本任务无新增外部依赖**：
- ✅ 不需要新增第三方库（不需要 vcpkg / cmake fetch）
- ✅ 不需要 API key / 环境变量
- ✅ 不需要修改 CMake 配置
- ✅ 不需要修改 CI workflow

**已存在的依赖**（继承自 Phase E.10）：
- 桌面：OpenGL 3.3 Core
- 移动：OpenGL ES 3.0
- 编译器：MSVC 19.x / Clang 12+

---

## 6. 用户最可能的下一步建议

按优先级排序：

### 优先级 1：等 CI green（被动等待）
```bash
gh run watch 25862930468
```

### 优先级 2：真实窗口视觉验证（5 分钟）
```bash
cd lumen-master/build/src/light/Release
./light.exe ../../../../../samples/demo_ssr/main.lua
# 按 B 开 blur, 按 V 切 bilateral, 按 ,/. 调 sigma
```

### 优先级 3：启动下一 Phase（推荐）
候选 Phase（按设计成本由低到高）：
- **Phase E.12 Normal-aware bilateral**：依赖 Phase E.8.x normal RT（已就绪）
- **Phase F.1 PBR Material 系统**：开启 roughness-aware 后处理路线
- **Phase E.13 SSR Temporal accumulation**：跨帧累积减噪（依赖 motion vector，成本高）

---

## 7. 一键操作指引（PowerShell）

### 7.1 看 CI 进度
```powershell
gh run watch 25862930468
```

### 7.2 真机视觉验证
```powershell
$exe = "e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe"
$lua = "e:\jinyiNew\Light\samples\demo_ssr\main.lua"
& $exe $lua
```

### 7.3 重跑 smoke（任何时候）
```powershell
$exe = "e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe"
& $exe "e:\jinyiNew\Light\scripts\smoke\ssr.lua"
```

### 7.4 启动下一 Phase（如选 E.12）
```
直接对 AI 说："启动 6A 工作流，开始 Phase E.12 Normal-aware Bilateral SSR Blur"
AI 会自动从 ALIGNMENT 开始
```

---

## 8. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseE_11.md` | 决策点 + 项目上下文清单 |
| `CONSENSUS_PhaseE_11.md` | 拍板结果锁定 |
| `DESIGN_PhaseE_11.md` | 架构 + shader 完整代码 + 数据流 |
| `TASK_PhaseE_11.md` | 12 原子任务 + 依赖图 |
| `ACCEPTANCE_PhaseE_11.md` | 验收矩阵（6 硬指标） |
| `FINAL_PhaseE_11.md` | 项目总结（实施过程 + 量化指标） |
| `TODO_PhaseE_11.md` (本文件) | 待办 + 外部依赖 + 一键操作 |

---

> **TODO 文档结束**
> 本文件作为 Phase E.11 收官后的唯一行动指南。
