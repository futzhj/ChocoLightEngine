# Phase F.0.7 TAA Compare Demo — PLAN

> 6A 工作流 · 阶段 1-3 (Align + Architect + Atomize) 合并精简
> 关联：`ACCEPTANCE_PhaseF_0_7.md` / `FINAL_PhaseF_0_7.md` / `TODO_PhaseF_0_7.md`
> 基线：F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 master TAA pipeline (commit `103bc07`)
> 目标工作量：2h

---

## 1. 背景与目标

Phase F.0 主管线 + F.0.1/0.2/0.3/0.4/0.5 五个优化点已全部上线，**Lua API 累计 23 fn，shader / pass / VRAM 全方位覆盖**。但目前缺少**直观对比工具**：

- `demo_ssr` 是综合 demo，TAA 仅作为 SSR 的 partner 出现，无法专门对比 TAA 配置
- smoke 测试只验 API round-trip，看不到画质差异
- 无快速切换预设方案，用户无法 1 键比对 6 个 Phase 的视觉影响

**目标**：构建轻量级 TAA 对比工具，按数字键 1-8 一键切换 8 个预设配置，HUD 直观显示当前配置的算法说明 + 关键参数 + history 稳定状态，配合高对比 specular 场景让 6 个 Phase 的画质差异肉眼可见。

---

## 2. 任务范围与边界

### 包含

- 新建 `samples/demo_taa_compare/main.lua` 单一 demo 文件
- 8 个预设配置（1=OFF, 2=F.0, 3=F.0.1, 4=F.0.2, 5=F.0.3, 6=F.0.4, 7=F.0.5, 8=ALL）
- HUD: 当前 preset 名 + 算法说明 + 5 个关键参数 + frame counter + history stabilization 进度
- 高对比场景: specular 球 + 旋转细薄物体 (易看 firefly / aliasing / ghosting)
- 6A 文档 4 件套 + demo README
- `lightc -p` 语法验证

### 不包含

- ❌ 真正双 viewport 左右屏渲染（TAA 是单一全局状态，shaderprogramming 约束下不可行；详见 §3）
- ❌ 截图 / 录屏功能（超工作量）
- ❌ 新增 Lua API / backend / shader（Phase F.0.7 是纯 demo Phase，零代码改动）
- ❌ 新增 smoke 测试（demo 不在 CI smoke 链）
- ❌ FLIP / SSIM 自动 perceptual 测试

---

## 3. 决策矩阵（5/5 全自动决策）

| # | 决策点 | 候选 | 选择 | 理由 |
|---|--------|------|------|------|
| **D1** | split-screen 实现方式 | (a) 真双 viewport 双 TAA 实例 / (b) 单 viewport 预设切换 / (c) 截屏对比 | **(b) 预设切换** | 真 split-screen 需 TAARenderer 多实例化（重构 ~6h，超工作量）；TAA 是连续帧累积，单帧无法对比；Preset 切换 + frame stabilization 提示是最务实方案 |
| **D2** | 切换时 history 处理 | (a) 立即切（脏 history） / (b) Disable + Enable 重建（白屏一帧） / (c) 切配置 + frame counter 显示稳定进度 | **(c) 切 + 进度显示** | 利用 TAA history alpha=0.92 自然收敛特性，~30 帧后 history 完全替换；HUD 显示 "stabilizing N/30" 提示用户 |
| **D3** | 场景内容 | (a) 静态 cube 阵列 / (b) 旋转高对比 specular 球 + 细薄物 / (c) 复杂 PBR 场景 | **(b) 旋转 specular 球 + 细薄物** | (a) 看不出 firefly 差异；(c) 干扰因素太多；(b) 是教科书 TAA 测试场景：HDR 高光 + 1px 细线 + 运动 → 同时考验 anti-flicker / ghosting / aliasing |
| **D4** | preset 数量与映射 | (a) 4 个 preset / (b) 8 个 (1-8) / (c) 切换 + 滑条 | **(b) 8 个数字键** | 1-8 直接对应 OFF + 6 个 Phase + ALL，最直观；与 demo_ssr 数字键习惯一致 |
| **D5** | demo 是否进 CI | (a) 入 CI workflow / (b) 仅 `lightc -p` 语法检查 | **(b) 仅语法检查** | demo 需 GUI 窗口 + 用户交互，CI headless 无法跑；现有 demo 都仅做语法检查（与 demo_ssr / demo_hdr 一致） |

---

## 4. 8 个预设配置矩阵

| Key | Preset 名 | TAA | RGB clip | YCoCg | Variance | AntiFlicker | Sharpness | HalfRes | 算法描述 |
|-----|-----------|-----|----------|-------|----------|-------------|-----------|---------|---------|
| **1** | TAA OFF (baseline) | ❌ | - | - | - | - | - | - | 无 TAA — 看原始 aliasing / firefly |
| **2** | F.0 base | ✅ | RGB AABB | ❌ | ❌ | ❌ | 0 | ❌ | jitter + reproject + RGB AABB clip + alpha blend (Phase F.0 原始) |
| **3** | F.0.1 + sharpening | ✅ | RGB AABB | ❌ | ❌ | ❌ | **0.5** | ❌ | F.0 + 4-tap unsharp mask |
| **4** | F.0.2 YCoCg clip | ✅ | YCoCg AABB | ✅ | ❌ | ❌ | 0.5 | ❌ | YCoCg 空间 9-tap clip (色彩边缘鲁棒) |
| **5** | F.0.3 variance | ✅ | YCoCg + variance | ✅ | ✅ (γ=1.0) | ❌ | 0.5 | ❌ | mean ± γσ clip (Salvi 2016 / UE5) |
| **6** | F.0.4 anti-flicker | ✅ | YCoCg + variance | ✅ | ✅ | ✅ (Karis) | 0.5 | ❌ | Karis luma weighted blend (压制 firefly) |
| **7** | F.0.5 half-res | ✅ | YCoCg + variance | ✅ | ✅ | ✅ | 0.5 | **✅** | history RT (W/2, H/2), VRAM -75% |
| **8** | ALL (推荐) | ✅ | YCoCg + variance | ✅ | ✅ (γ=1.0) | ✅ | 0.8 | ✅ | 完整管线 (推荐生产配置 / 移动 4K) |

**渐进式排序设计**：每个 preset 在前一个基础上叠加一个特性，让用户可以看到每个 Phase 的边际贡献。

---

## 5. 场景设计

### 5.1 几何体

```
旋转高对比 specular 球 (中央, R=1.0, 16x16 segments)
  - albedo = (1.0, 0.9, 0.7)  -- 暖金色
  - HDR 高光 (从光源反射 → firefly 触发器)
  - 自转 (绕 Y, 30 deg/s) → ghosting 触发器

旋转细薄物 (8 根, 围绕中心球, R=2, 距 0.5)
  - 1×1 像素厚度 → aliasing 触发器
  - 不同颜色 (红/绿/蓝/黄/紫/青/白/橙)
  - 旋转 (绕中心 60 deg/s) → 高速运动 trail 触发器

地面 plane (10×10 黑色) → 反衬高光
```

### 5.2 相机

- 透视投影 (60° FOV)
- 静态相机 (eye=(0, 2, 5), at=(0, 0.5, 0))
- 不旋转 → 让用户专注观察 TAA 对运动物的处理

### 5.3 光照

- 1 个 HDR point light (intensity=10, 在 (3, 5, 3))
- ambient = (0.05, 0.05, 0.08)

---

## 6. HUD 设计

```
[Preset 5/8] F.0.3 Variance clip
Algorithm: clip = mean ± 1.0σ (Salvi 2016 / UE5 default)
TAA: ON | alpha=0.92 | clip=ON/variance(γ=1.0) | jitter=ON
Sharpness: 0.5 (sharpen pass) | AntiFlicker: OFF | HalfRes: OFF
History: stabilizing 12/30 frames | Frame: 4521
Keys: 1-8 = preset | R = reset history | ESC = exit
```

**关键 HUD 元素**：
1. Preset 索引 + 名（突出显示）
2. Algorithm 一行说明（教学价值）
3. 当前 TAA 全部参数（透明度高）
4. **history stabilization 进度** (重要！告诉用户"切换后 30 帧才能稳定对比")
5. Frame counter (用于回放对比)
6. Keys 帮助行

---

## 7. 实施顺序

### T0 (15 min) — PLAN 文档（本文件）

### T1 (60 min) — demo 实现 `samples/demo_taa_compare/main.lua`

- L1-50: header + module loading + headless guard
- L51-150: 几何 (球 + 细线 + plane) 构建
- L151-200: 8 preset 表 + apply_preset() 函数
- L201-280: 主循环 (rotation + camera + draw + apply_preset on key + HUD)
- L281-300: 反向清理

### T2 (30 min) — 6A 文档 4 件套 + README

- ACCEPTANCE: 验收清单 + 决策对齐验证
- FINAL: 项目总结
- TODO: 待办 + CI 状态预填
- README: demo 用户指引

### T3 (15 min) — 提交

- `lightc -p` 语法验证
- git add + commit + push
- CI 6/6 监控（仅 build + 语法检查，无 runtime smoke）
- CI 状态回填

---

## 8. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| Window.Open 失败 (headless) | 启动崩 | pcall + 现有 demo_ssr 同样 guard |
| TAA Enable 失败 (no GL) | preset 切换无效 | 每 preset apply 前 `if TAA.IsSupported()` 防御 |
| keyTap 重复触发 | 数字键被持续读取 | 沿用 demo_ssr 的 keyTap 实现 (down -> up edge detect) |
| history stabilization 计数错误 | HUD 误导 | 用 frame counter 差值，每次切 preset 记录 baseline frame |
| ALL preset 移动 4K 模拟 | 桌面 1080p 看不出 VRAM 节省 | HUD 显示 "VRAM 节省 -75%" 文字描述（无法实测） |

---

## 9. 验收标准

### 功能
- [ ] 启动后默认 preset = 1 (OFF)
- [ ] 数字键 1-8 立即切换 preset
- [ ] HUD 实时反映当前 preset 名 + 参数 + history stabilization 进度
- [ ] R 键重置 history (Disable + Enable)
- [ ] ESC 退出

### 文档
- [ ] PLAN/ACCEPTANCE/FINAL/TODO 4 文档齐
- [ ] README 含完整 keys 列表 + 8 preset 说明 + 推荐观察点

### CI
- [ ] `lightc -p main.lua` Exit 0
- [ ] GitHub Actions 6/6 平台 success
- [ ] 不影响其他 smoke (零回归)
