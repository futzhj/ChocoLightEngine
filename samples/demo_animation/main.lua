-- ChocoLight Sample: Light.Animation (Phase AV)
--
-- 演示骨骼动画 + 状态机 + Crossfade + 事件帧。
-- 资源策略：本 demo 不强依赖外部 glTF。
--   - 若 assets/character.glb 存在 → 完整加载 → 状态机+蒙皮渲染演示
--   - 否则 → 输出降级提示并测试纯逻辑接口（API 表面 + 错误路径）
-- 跨 6 平台 console 友好（Windows runtime smoke 可直接跑）。

local ok, Anim = pcall(require, "Light.Animation")
if not ok or type(Anim) ~= "table" then
    print("Light.Animation 不可用,跳过 (Anim=" .. tostring(Anim) .. ")")
    print("\ndemo_animation ok (no animation)")
    return
end

print("==== Light.Animation demo ====")

-- ==================== 1. 顶层 API 探查 ====================

print("[1] Light.Animation API:")
print("    LoadSkinnedGLTF = " .. tostring(type(Anim.LoadSkinnedGLTF)))
print("    NewAnimator     = " .. tostring(type(Anim.NewAnimator)))
print("    DrawSkinnedMesh = " .. tostring(type(Anim.DrawSkinnedMesh)))

-- ==================== 2. glTF 资源探测（选择性加载） ====================

local function file_exists(path)
    local f = io.open(path, "rb")
    if f then f:close(); return true end
    return false
end

-- 候选路径：sample 目录下 / assets 共享目录
local candidates = {
    "samples/demo_animation/assets/character.glb",
    "samples/demo_animation/character.glb",
    "assets/character.glb",
    "Light-0.2.3/assets/character.glb",
}
local glb_path = nil
for _, p in ipairs(candidates) do
    if file_exists(p) then glb_path = p; break end
end

local pack, err
if glb_path then
    print(string.format("[2] 加载 glTF: %s", glb_path))
    pack, err = Anim.LoadSkinnedGLTF(glb_path)
    if not pack then
        print("    LoadSkinnedGLTF 失败: " .. tostring(err))
        glb_path = nil
    end
end

if not pack then
    print("[2] 未找到 glTF 资源 (尝试过): ")
    for _, p in ipairs(candidates) do print("       - " .. p) end
    print("    将仅演示 API 表面 / 错误路径 (不影响 demo 退出码)")
end

-- ==================== 3. 资源在场: Skeleton / Clip / Animator ====================

if pack and pack.skeleton then
    local skel  = pack.skeleton
    print(string.format("[3] Skeleton: %d 关节",      skel:GetJointCount()))

    local clipNames = pack.clipNames or {}
    print(string.format("    Clips: %d (%s)", #clipNames,
                        table.concat(clipNames, ", ")))

    local animator = Anim.NewAnimator(skel)

    -- 把所有 clip 加为 state
    for _, name in ipairs(clipNames) do
        local clip = pack.clips[name]
        if clip then animator:AddState(name, clip) end
    end

    -- 默认播放第一个 clip
    if clipNames[1] then
        animator:Play(clipNames[1])
        print(string.format("    Play: %s (duration=%.2fs)",
                            clipNames[1], pack.clips[clipNames[1]]:GetDuration()))
    end

    -- ==================== 4. Step 4 状态机演示 ====================

    -- 有 ≥2 个 clip 时演示 Transition + Crossfade
    if #clipNames >= 2 then
        local from = clipNames[1]
        local to   = clipNames[2]
        print(string.format("[4] Transition: %s --(speed>0.5)--> %s, fade=0.3s", from, to))

        animator:AddTransition(from, to, function(a)
            return (a:GetParam("speed") or 0) > 0.5
        end, 0.3)

        animator:SetParam("speed", 0.0)
        animator:Update(1/60)
        print(string.format("    speed=0.0 → state=%s isFading=%s",
                            tostring(animator:GetCurrentState()),
                            tostring(animator:IsCrossfading())))

        animator:SetParam("speed", 1.0)
        animator:Update(1/60)
        print(string.format("    speed=1.0 → state=%s isFading=%s target=%s progress=%.2f",
                            tostring(animator:GetCurrentState()),
                            tostring(animator:IsCrossfading()),
                            tostring(animator:GetCrossfadeTarget()),
                            animator:GetCrossfadeProgress()))

        -- 推进到 fade 完成
        for _ = 1, 30 do animator:Update(1/60) end
        print(string.format("    fade 完成后 → state=%s isFading=%s",
                            tostring(animator:GetCurrentState()),
                            tostring(animator:IsCrossfading())))
    else
        print("[4] 仅 1 个 clip,跳过 Transition 演示")
    end

    -- ==================== 5. 事件帧演示 ====================

    if clipNames[1] then
        local trigger_count = 0
        local clip0_dur     = pack.clips[clipNames[1]]:GetDuration()
        local trig_t        = (clip0_dur > 0.2) and (clip0_dur * 0.5) or 0.1
        animator:AddEvent(clipNames[1], trig_t, function(a)
            trigger_count = trigger_count + 1
        end)
        animator:Play(clipNames[1])
        animator:SetCurrentTime(0)

        -- 推进 1 个完整周期
        local steps = math.max(1, math.floor((clip0_dur + 0.05) / (1/60)))
        for _ = 1, steps do animator:Update(1/60) end
        print(string.format("[5] Event 触发 %d 次 (期望 1, trig=%.3fs of %.3fs)",
                            trigger_count, trig_t, clip0_dur))
    end

    -- ==================== 6. SkinnedMesh (CPU 蒙皮) ====================

    if pack.mesh then
        local sm = pack.mesh
        print(string.format("[6] SkinnedMesh: %d 顶点 / %d 索引",
                            sm:GetVertexCount(), sm:GetIndexCount()))

        -- DrawSkinnedMesh 在 headless (无渲染上下文) 会返回 false + err, 不崩
        local trans = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}     -- identity
        local mat   = nil                                       -- 默认材质
        local ok2, msg = pcall(Anim.DrawSkinnedMesh, sm, animator, trans, mat)
        print(string.format("    DrawSkinnedMesh: pcall_ok=%s msg=%s",
                            tostring(ok2), tostring(msg)))
    else
        print("[6] glTF 不含 skinned mesh primitive (仅骨骼动画数据)")
    end

    -- 7. 清理
    animator:ClearTransitions()
    animator:ClearEvents()
    animator:Stop()
    print("[7] cleanup 完成 (ClearTransitions / ClearEvents / Stop)")
else
    -- ==================== 资源缺失分支: 仅做 API 表面验证 ====================

    print("[3-7] 跳过状态机演示 (无 Skeleton/Clip/Mesh 资源)")
    print("       Phase AV C++ 实现仍通过, smoke 由 scripts/smoke/animation.lua 覆盖")

    -- 错误路径: LoadSkinnedGLTF 必然失败
    local p, e = Anim.LoadSkinnedGLTF("__nonexistent_demo_av.glb")
    if not p then
        print(string.format("       Anim.LoadSkinnedGLTF (期望 nil): nil err=%s", tostring(e)))
    end
end

print("\ndemo_animation ok")
