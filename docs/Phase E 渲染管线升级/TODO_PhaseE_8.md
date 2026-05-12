# TODO — Phase E.8 · SSAO (交付后未尽事项)

> 6A 工作流 · 阶段 6 · Assess 衍生物
> 列出 Phase E.8 交付后的待办事宜 + 缺少的配置 + 后续候选 phase 入口。

---

## 1. 必要补完

### 1.1 真机视觉验收 [可选 — 用户参与]

**做法**：

1. 编译 `Light.dll` 或拿 CI 产物（`gh run download <id> --name windows-template`）
2. 跑 demo：
   ```powershell
   ./light.exe samples/demo_ssao/main.lua
   ```
3. 验证视觉（关键对比）：
   - **F 切换 SSAO**：开/关 AO 对比；接触阴影应明显出现 / 消失
   - **1/2 改 Radius**：0.1 → 紧贴接触点；2.0 → 大范围软阴影
   - **3/4 改 Bias**：0.0 → 自遮蔽伪影（点状黑斑）；0.05 → 过度补偿白点
   - **5/6 改 Intensity**：0 → 无 AO（与 F off 等价）；3 → 过强（角落全黑）
   - **7/8 改 Power**：0.5 → 软；4.0 → 硬对比
   - **B 切 BlurEnabled**：on → 平滑；off → 噪点（stylized）
   - **K 切 KernelSize**：8 → 性能；16 → 质量
   - **R reset 默认**
4. 可选截图归档到 `docs/Phase E 渲染管线升级/assets/`

---

### 1.2 性能基线 [可选]

**问题**：SSAO 增加 4-5 个 fragment pass。

**预估**（1920×1080，HDR depth 24bit）：

| 配置 | 开销（近似） |
|------|-------------|
| KernelSize=8 + blur off | ~0.4 ms（raw + composite） |
| KernelSize=8 + blur on | ~0.7 ms（raw + 2 blur + composite） |
| KernelSize=16 + blur off | ~0.6 ms |
| **KernelSize=16 + blur on（默认）** | **~1.0 ms** |
| blit depth overhead | ~0.1 ms |

→ 与 Bloom / Streak / LensFlare 同量级，可接受。

---

## 2. 未来扩展

### 2.1 G-buffer normal RT [中-高优先]

**动机**：当前 normal 用 `cross(ddx, ddy)` 重建，极端角度精度差（45° 边缘出现条纹噪声）。

**做法**：

- HDR FBO 加 MRT：`COLOR0=RGBA16F scene`, `COLOR1=RG16F view-normal`
- PBR / Lit2D shader 改 `out vec4 FragColor0` + `out vec2 FragColor1 = vec2(N.x, N.y)`（z 重建）
- DrawSSAO shader 改读 `uNormalTex` 替代 `dFdy/dFdx`
- 工程量：~1 天（涉及 MRT 路径 + 现有 shader 修改）

---

### 2.2 HBAO+ / GTAO 算法升级 [中优先]

**动机**：经典 SSAO 半球采样质量与 HBAO 相比有差距。

**做法**：

- HBAO：水平/垂直方向取多个 horizon 角度，做半球积分
- GTAO：现代游戏标准；需要 compute shader（Phase F 候选）
- 工程量：HBAO 改 shader（~半天）；GTAO 需要 CS pipeline（>1 周）

---

### 2.3 Temporal SSAO (TAA-style) [中-大工程]

**动机**：half-res + bilateral blur 后仍可能闪烁；temporal 平滑能消除。

**做法**：

- 保存上一帧 AO RT
- 当前帧 AO 与历史帧加权混合（指数衰减 0.9 + reproject 校验）
- 工程量：需要 view-projection 矩阵历史 + 帧间 jitter，~1 周

---

### 2.4 SSAO 在 Bloom 之前的管线重排 [低优先]

**动机**：当前 Composite 在 LensFlare 之后；理论上 AO 暗部应在 bright pass 前完成，否则被 bloom 提亮抹平。

**做法**：

- 移动 `SSAORenderer::Process` 到 `BloomRenderer::Process` 之前
- 可能需重排 SetCanvas / Pause 逻辑确保 HDR depth 已就绪
- 工程量：几行代码，但需要重测所有 Phase E demo

---

### 2.5 HDR depth blit 兼容性 Init 探测 [低优先]

**动机**：极少数老 GLES 驱动可能不支持 `glBlitFramebuffer + GL_DEPTH_BUFFER_BIT`，当前未检测，遇到失败时 SSAO 会输出错误结果。

**做法**：

- `InitLensFx` 内创建 1×1 测试 FBO + blit 探测
- glGetError 失败 → `ssaoSupported = false`
- 工程量：~20 行代码

---

## 3. 引擎基础设施扩展（共性）

### 3.1 GL state 副作用警示（CI 测试）

SSAO Composite 使用 `glBlitFramebuffer` 切换 `GL_READ_FRAMEBUFFER` / `GL_DRAW_FRAMEBUFFER`，若后续用户代码假设 `glBindFramebuffer(GL_FRAMEBUFFER, ...)` 默认 read=draw，可能出现意外。

**建议**：CI smoke 加 SSAO Process 后续 glReadPixels / glBindFramebuffer 假设的回归测试。

---

## 4. 验证脚本速查

```powershell
gh run list --limit 5 --branch main
gh run view <id> --log-failed

lua -e "loadfile('scripts/smoke/ssao.lua')"
lua -e "loadfile('samples/demo_ssao/main.lua')"
```

---

## 5. 完结清单

- [x] E.8.1 backend ✅ CI 6/6（含 fix1 `c4e7d35` + fix2 `a6c2a78`）
- [x] E.8.2 module ✅ CI 6/6
- [x] E.8.3 Lua + smoke + demo + CI ✅ CI 6/6（含 fix `a52130e` epsilon）
- [x] ACCEPTANCE_PhaseE_8.md ✅
- [x] FINAL_PhaseE_8.md ✅
- [x] TODO_PhaseE_8.md ✅（本文件）
- [x] **最终 head `a52130e` CI run 25705912000 全 6/6 绿** ✨
- [ ] 真机视觉验收（用户参与）
- [ ] G-buffer normal RT（建议中期补；视觉质量↑大）
- [ ] HBAO/GTAO 升级（长期可选）
- [ ] Temporal SSAO（长期可选）

---

## 6. 缺少的配置 / 用户操作指引

### 6.1 demo 跑不起来？

如果 `light.exe samples/demo_ssao/main.lua` 报错：
- 检查 `Light.Graphics.Mesh.New` 是否可用：必须 ChocoLight build with Phase AS.2+
- 检查 `Light.Graphics.SetPerspective` 可用：必须 Phase AS.2+
- 检查 HDR.Enable 返回 true：必须 OpenGL 3.3+ / GLES 3.0+

### 6.2 SSAO 看不到效果？

- 必须 `SetDepthTest(true)` 启用深度测试
- 必须用 `SetPerspective + SetCamera` 而非 2D 正交
- 必须有几何体相邻 / 接触（孤立物体 AO 极弱）
- `Radius` 太小（< 0.1）AO 几乎无影响

### 6.3 SSAO 出现伪影 / 黑斑？

- `Bias` 太小（< 0.01）触发自遮蔽
- 提高 `Bias` 到 0.025-0.05

### 6.4 GLES3 平台失败？

已修复 `glDrawBuffer → glDrawBuffers`（commit `c4e7d35`）。
若仍有问题，查 CI run 日志。

---

**Phase E.8 主交付完结**。HDR 链路累计 **7 剑客 / ~130 Lua API** 上线 ✨

```
Phase E.3 — HDR + 4 tonemap operator      (12 fn)
Phase E.4 — Bloom pyramid                  (15 fn)
Phase E.5 — Auto Exposure (Eye Adaptation) (18 fn)
Phase E.6 — Lens Dirt + Streak             (23 fn)
Phase E.7 — Lens Flare + FlareTexture      (23 fn)
Phase E.8 — SSAO                            (19 fn)   ✨
```
