# demo_morph_target — Phase AX Morph Target 表情/形状变形 演示

ChocoLight Phase AX 的 morph target (blend shape) 功能演示与验证 demo。

## 快速开始

### Windows

```powershell
# 1. 下载默认资产 (AnimatedMorphCube)
.\samples\demo_morph_target\setup.ps1

# 2. 运行 demo (路径相对于项目根目录)
.\Light-0.2.3\windows-x64\light.exe samples\demo_morph_target\main.lua
```

### Linux / macOS

```bash
# 1. 下载默认资产
./samples/demo_morph_target/setup.sh

# 2. 运行 demo
./Light-0.2.3/<platform>/light samples/demo_morph_target/main.lua
```

## 控制键

| 按键 | 作用 |
|------|------|
| **1 - 8** | 选择激活的 morph target slot (1-based) |
| **↑ / ↓** | 增加 / 减少当前激活 slot 的 weight (步长 0.1, clamp [0, 1]) |
| **Q / W** | 当前 slot 设为 0 / 1 (快捷键) |
| **C** | `ClearMorphWeights` — 清除所有手动覆盖, 恢复动画驱动 |
| **G / N** | `SetSkinningMode("gpu")` / `("cpu")` — 切换路径观察视觉一致性 |
| **SPACE** | 暂停 / 恢复 Animator |
| **ESC** | 退出 |

## OSD 显示内容

- **Backend**: 当前渲染后端 (GL33Core / LegacyGL)
- **SkinMode**: 当前蒙皮模式 (cpu / gpu / auto 实际生效)
- **Paused**: 是否暂停
- **Active slot**: 当前键盘选中的 morph slot
- **Mesh morph targets**: 资产中的 morph target 数量与名称
- **Animator weights**: 每个 slot 的当前权重值，`[MANUAL]` 标记表示该 slot 已被手动覆盖

## 期望效果

### 完整 SKIN+MORPH 资产 (推荐)

- 3D 模型在屏幕中心渲染
- 动画通道驱动 morph weights，模型形状随时间变化
- 按 ↑↓ 键可观察手动覆盖立即生效
- 按 G / N 切换 GPU / CPU 路径，视觉应一致 (Phase AX shader 与 CPU 路径数学等价)

### 仅 morph 资产 (如默认 AnimatedMorphCube)

- 无 3D 渲染（ChocoLight Phase AX 只支持 SKIN+MORPH 共存路径）
- OSD 仍显示 morph target 名称、动画驱动的权重变化
- API 完整可测：`SetMorphWeight` / `GetMorphWeight` / `ClearMorphWeights` 等

## 推荐资产源

| 资产 | 描述 | 含 skin | 链接 |
|------|------|---------|------|
| AnimatedMorphCube | 8 个基本形变 (默认下载) | ❌ | [Khronos](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/AnimatedMorphCube) |
| AnimatedMorphSphere | 球面变形 | ❌ | [Khronos](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/AnimatedMorphSphere) |
| MorphPrimitivesTest | 多 primitive morph 测试 | ❌ | [Khronos](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/MorphPrimitivesTest) |
| **自制角色 + 表情 blend shape** | 推荐用 Blender 导出 | ✅ | 教程见下方 |

### Blender 导出 SKIN+MORPH 资产 (完整视觉演示)

1. 在 Blender 中创建/导入含 armature 的角色模型
2. 在 Object Data Properties → Shape Keys 中添加 morph target (称为 Shape Keys)
3. 创建 Action 给 Shape Keys 添加 keyframe 动画
4. File → Export → glTF 2.0 (.glb / .gltf)，确保勾选:
   - **Animations → Shape Keys**
   - **Skinning → Include Armatures**
5. 导出后将 .glb 文件改名为 `morph.glb` 放入 `samples/demo_morph_target/assets/`

## 自定义资产路径

main.lua 按以下优先级查找资产 (找到第一个即用)：

1. `samples/demo_morph_target/assets/morph.glb`
2. `samples/demo_animation/assets/character.glb`
3. `samples/demo_skinning_perf/assets/character.glb`
4. `assets/morph.glb`

## 故障排查

### "未找到 glTF 资产"

→ 运行 `setup.ps1` / `setup.sh` 下载默认资产，或参考"自定义资产路径"

### "LoadSkinnedGLTF 失败"

可能原因：
- 资产不是 glTF 2.0 (检查文件 magic = `glTF`)
- 资产已损坏 (重新运行 `setup.ps1 -Force`)
- 含 morph 但目标数 > 8 (Phase AX 上限，超出会被截断 + 控制台 warning)

### "OSD 显示 mesh = nil"

→ 资产无 skin (如 AnimatedMorphCube)。Phase AX 只支持 SKIN+MORPH 共存路径，
仅 morph 的资产无法渲染。OSD 仍可演示 Animator API 与动画驱动。
请用 Blender 导出 SKIN+MORPH 资产以获得完整视觉效果。

### GPU / CPU 路径视觉不一致

按设计应完全一致 (CPU/GPU 算法等价 — 见 `docs/Phase AX/DESIGN_PhaseAX.md` §3.6 / §3.7)。
若发现不一致，请提交 issue + 提供：
- 资产 `morph.glb`
- `glxinfo` (Linux) / GL_RENDERER (`Gfx.GetBackendName()` 输出)
- CPU vs GPU 截图对比

## 实现细节

| 维度 | 说明 |
|------|------|
| Morph target 上限 | 8 (`Light.Animation.MORPH_TARGET_MAX`) |
| CPU 路径 | `DrawSkinnedMorphMeshCPU` — base + Σ(w·Δ) → skin → modelMat |
| GPU 路径 (GL33) | `VS3D_SKIN_MORPH` shader + RGB32F 2D texture (vCount × N) |
| 数据存储 | morph delta 在 `MorphTarget.posDelta/nrmDelta/tanDelta` (vCount × 3 floats) |
| 权重优先级 | manual override > animation channel > default 0 (NaN sentinel) |
| 与 GPU Skinning | 共存：同时启用时使用 `VS3D_SKIN_MORPH`(morph 在 skin 之前应用) |

## 许可

ChocoLight 引擎: 见项目根目录 LICENSE。

下载的 Khronos sample 资产: CC0 / Royalty-Free (KhronosGroup/glTF-Sample-Models LICENSE)。
