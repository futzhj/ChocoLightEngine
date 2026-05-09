# demo_animation — Phase AV 骨骼动画 + 状态机示例

演示 `Light.Animation` 模块的完整能力链：

- glTF 2.0 加载（`Anim.LoadSkinnedGLTF`）
- Skeleton / AnimationClip 数据结构
- Animator 状态机（AddState / Play / Update）
- Step 4 完整状态机：Transition / Crossfade / Event / Param
- SkinnedMesh + DrawSkinnedMesh（CPU 蒙皮）

## 资源说明

本 demo **不内置** glTF 资源（避免仓库膨胀）。若你想看到完整的状态机+蒙皮渲染演示，需自行准备一份角色 glTF（带 skeleton + 至少 2 个 animation clip）。

### 推荐资源

| 来源 | URL | 说明 |
|------|-----|------|
| Khronos glTF samples | https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedFigure | `RiggedFigure.glb` 含简单骨骼 |
| Khronos glTF samples | https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/CesiumMan | `CesiumMan.glb` 含动画 |
| Mixamo | https://www.mixamo.com/ | 含 idle/walk/run 等 clip 集合 |

把 glb 文件放到以下任一路径（demo 会按顺序探测）：

1. `samples/demo_animation/assets/character.glb`
2. `samples/demo_animation/character.glb`
3. `assets/character.glb`
4. `Light-0.2.3/assets/character.glb`

## 资源缺失时的行为

demo **优雅降级**：

- 不会崩溃
- 不会返回非 0 退出码
- 输出明确的"未找到 glTF 资源"提示并枚举尝试过的路径
- 仍会执行 `Light.Animation` API 表面验证 + 错误路径调用

这与所有 Phase A* sample 的策略一致（`demo_physics3d` / `demo_haptic` 等），保证 6 平台 CI smoke 始终绿。

## 运行

```
light.exe samples/demo_animation/main.lua
```

或在 Windows runtime smoke 中一并跑（已注册到 `.github/workflows/build-templates.yml`）。

## 演示输出（含 glTF 时）

```
==== Light.Animation demo ====
[1] Light.Animation API: ...
[2] 加载 glTF: samples/demo_animation/character.glb
[3] Skeleton: 23 关节
    Clips: 3 (Idle, Walk, Run)
    Play: Idle (duration=2.00s)
[4] Transition: Idle --(speed>0.5)--> Walk, fade=0.3s
    speed=0.0 → state=Idle isFading=false
    speed=1.0 → state=Idle isFading=true target=Walk progress=0.06
    fade 完成后 → state=Walk isFading=false
[5] Event 触发 1 次 (期望 1, trig=1.000s of 2.000s)
[6] SkinnedMesh: 3450 顶点 / 17280 索引
    DrawSkinnedMesh: pcall_ok=true msg=true
[7] cleanup 完成 (ClearTransitions / ClearEvents / Stop)

demo_animation ok
```

## 演示输出（无 glTF 时）

```
==== Light.Animation demo ====
[1] Light.Animation API: ...
[2] 未找到 glTF 资源 (尝试过):
       - samples/demo_animation/assets/character.glb
       - samples/demo_animation/character.glb
       - assets/character.glb
       - Light-0.2.3/assets/character.glb
    将仅演示 API 表面 / 错误路径 (不影响 demo 退出码)
[3-7] 跳过状态机演示 (无 Skeleton/Clip/Mesh 资源)
       Phase AV C++ 实现仍通过, smoke 由 scripts/smoke/animation.lua 覆盖
       Anim.LoadSkinnedGLTF (期望 nil): nil err=cgltf_parse_file failed (err 6)

demo_animation ok
```

## 相关文档

- `docs/api/Light_Animation.md` — API 完整参考
- `docs/Phase AV 骨骼动画/` — 6A 工作流文档
- `scripts/smoke/animation.lua` — 跨平台 smoke 测试
