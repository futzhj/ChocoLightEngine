# TODO — Phase E.6 · Lens Dirt + Streak (交付后未尽事项)

> 6A 工作流 · 阶段 6 · Assess 衍生物

---

## 1. 必要补完

### 1.1 真机视觉验收 [可选 - 用户参与]

**做法**：
1. 编译 `Light.dll` 或拿 CI 产物（`gh run download <id> --name windows-template`）
2. 跑 demo：
   ```powershell
   ./light.exe samples/demo_lens_fx/main.lua
   ```
3. 验证：
   - 默认启动：bloom 亮光 + 1×1 白 dirt（= bloom × intensity）+ 水平 streak
   - **L 切换**：LD 开时亮点辉光范围略变化（乘白 1.0 时差异小）
   - **K 切换**：ST 开时亮点水平长条纹明显
   - **H/V/G 切换**：条纹方向切水平 / 垂直 / 45° 斜
   - **7/8 调 Iterations**：step=1 只 30% 长；step=8 非常长但可能失真
4. 可选截图归档到 `docs/Phase E 渲染管线升级/assets/`

---

### 1.2 自定义 Dirt 纹理测试 [可选]

**做法**：准备一张 lens dirt PNG（GitHub 有公开素材），在 demo 中调：
```lua
local dirtImg = Light(Gfx.Image):New('assets/lens_dirt.png')
Gfx.LensDirt.SetDirtTexture(dirtImg)
```
观察亮点 bloom 处的"划痕/斑点"是否叠加正确。

---

### 1.3 性能基线 [可选]

**问题**：LensFx 管线增加了 ~N+2 个 fragment pass（LensDirt 1 + Streak N+2），未测实际开销。

**预估**（1920×1080，Streak iter=5）：
- LensDirt:  +~0.2 ms（1 个全屏 quad）
- Streak:    +~1.5 ms（bright + 5 blur + composite，半分辨率）

---

## 2. 未来扩展

### 2.1 内置 dirt 纹理包 [中优先]

**动机**：目前 LensDirt.Enable 后视觉无差别（乘白色）。引擎应提供 1-2 张标准 dirt 纹理。

**做法**：
- `ChocoLight/assets/textures/lens_dirt_default.png`（512×512 灰度噪点）
- `LensDirt.LoadDefault()` Lua helper 自动加载 + SetDirtTexture
- 或 `InitLensFx` 时在引擎内部编码嵌入 stb_image（+1 字段 builtinDirtTex）

---

### 2.2 Animated dirt (UV scroll / 雨滴) [低优先]

**动机**：`SetDirtScrollRate(speedX, speedY)` 让 dirt 纹理随时间流动，模拟雨水滑过。

**接口**：
```cpp
void SetDirtScrollRate(float sx, float sy);
```
Shader 加 `uDirtUVOffset` uniform，每帧 += dt × rate。

---

### 2.3 Streak 多方向 (Star flare) [低优先]

**动机**：真实 anamorphic 可同时有水平 + 45° + -45° 多条纹（"星芒"效果）。

**做法**：
- 单次 Process 可选多 direction 数组
- 或 Lua 端多次 Process，每次不同方向混合
- v1 留用户手动处理（multi-frame trick）

---

### 2.4 Lens Flare (Ghost 鬼影) [中-大工程]

**动机**：真实相机主光源会产生多段镜像内的光圈鬼影。

**做法**：
- 识别画面中最亮像素位置（类似 AE 的 luminance reduction）
- 沿画面中心方向反投若干个不同尺寸 / 颜色的光圈
- 独立 `LensFlareRenderer` namespace → Phase E.7 候选

---

## 3. 引擎基础设施扩展

### 3.1 Image 引用计数 / GC [高优先]

**问题**：`LD.SetDirtTexture(img)` 只存 `img:GetTextureId()` uint32，不持有 img 引用。若 Lua 侧 `img` 被 GC，tex 被后端释放，LD 下次 Draw 就采样无效 tex。

**建议**：
- LensDirt 内部存 LuaRef 弱引用或 pin 住 img（借助 `LUA_REGISTRYINDEX`）
- 或 Lua 端文档明确"用户须保持 Image 存活"

---

## 4. 验证脚本速查

```powershell
gh run list --limit 5 --branch main
gh run view <id> --log-failed

lua -e "loadfile('scripts/smoke/lens_fx.lua')"
lua -e "loadfile('samples/demo_lens_fx/main.lua')"
```

---

## 5. 完结清单

- [x] E.6.1 backend ✅ CI 6/6
- [x] E.6.2 modules（含 Bloom GetPyramidTopTex）✅ CI 5/6 (iOS 一贯慢)
- [x] E.6.3 Lua + smoke + demo + CI ✅
- [x] ACCEPTANCE_PhaseE_6.md ✅
- [x] FINAL_PhaseE_6.md ✅
- [x] TODO_PhaseE_6.md ✅（本文件）
- [ ] 真机视觉验收（用户参与）
- [ ] 内置 dirt 纹理（建议中期补）
- [ ] Animated dirt / multi-direction streak（长期可选）

---

**Phase E.6 主交付完结**。HDR 链路累计 **5 剑客 / 68 Lua API** 上线。

- Phase E.3 HDR + 4 tonemap operator
- Phase E.4 Bloom pyramid
- Phase E.5 Auto Exposure（眼睛适应）
- Phase E.6 Lens Dirt + Streak（电影感后处理）
