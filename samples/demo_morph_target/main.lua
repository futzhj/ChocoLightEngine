-- ============================================================================
-- ChocoLight Phase AX Morph Target 表情/形状变形 demo
-- ============================================================================
-- 用途:
--   1. 加载含 morph target 的 glTF 资产
--   2. 自动播放 weights animation channel (如有)
--   3. 屏上 OSD 显示每个 morph target 的当前权重 + 是否手动覆盖
--   4. 键盘控制 weight: 切换激活槽位 + 增减 weight + 清除手动覆盖
--   5. 验证 CPU vs GPU 路径 (启 SetSkinningMode 切换观察视觉一致性)
--
-- 控制:
--   1-8       : 选择激活的 morph target slot (1-based)
--   ↑ / ↓     : 增加 / 减少当前激活 slot 的 weight (步长 0.1)
--   Q / W     : 当前 slot 设为 0 / 1 (快捷键)
--   C         : ClearMorphWeights (清除所有手动覆盖, 恢复动画驱动)
--   G / N     : SetSkinningMode -> gpu / cpu (观察视觉一致性)
--   SPACE     : 暂停 / 恢复 Animator
--   ESC       : 退出
--
-- 资产:
--   优先路径: samples/demo_morph_target/assets/morph.glb
--   运行 setup.ps1 / setup.sh 一键下载默认 (AnimatedMorphCube)
--
-- 注意事项 (Phase AX 范围):
--   ChocoLight Phase AX 只支持 SKIN+MORPH 共存的 mesh.
--   AnimatedMorphCube 是无 skin 的纯 morph mesh, LoadSkinnedGLTF 会把它的 mesh 设为 nil.
--   这里我们仍可演示 Animator 的 morph weights API 和动画通道驱动 (无视觉渲染).
--   要完整 SKIN+MORPH 视觉演示, 用 Blender 导出含 skin 的 morph 模型替换 morph.glb.
--
-- 兼容: Lua 5.1 + ChocoLight Light.Animation/Time/Graphics/UI.Window
-- ============================================================================

-- ==================== 1. 模块加载 ====================

local Anim, Time, UI, Gfx
do
    local function safe_require(n)
        local ok, m = pcall(require, n)
        if ok and type(m) == 'table' then return m end
        return nil
    end
    Anim = safe_require('Light.Animation')
    Time = safe_require('Light.Time')
    UI   = safe_require('Light.UI')
    Gfx  = safe_require('Light.Graphics')
end

if not (Anim and Time and Gfx) then
    print('[demo_morph_target] 必需模块缺失 (Animation/Time/Graphics)')
    print('  Anim=' .. tostring(Anim) .. ' Time=' .. tostring(Time) .. ' Gfx=' .. tostring(Gfx))
    print('demo_morph_target ok (modules unavailable)')
    return
end

-- ==================== 2. 资产路径探测 ====================

local function file_exists(p)
    local f = io.open(p, 'rb')
    if f then f:close(); return true end
    return false
end

local CANDIDATES = {
    'samples/demo_morph_target/assets/morph.glb',
    'samples/demo_animation/assets/character.glb',
    'samples/demo_skinning_perf/assets/character.glb',
    'assets/morph.glb',
}

local function find_asset()
    for _, p in ipairs(CANDIDATES) do
        if file_exists(p) then return p end
    end
    return nil
end

local assetPath = find_asset()
if not assetPath then
    print('[demo_morph_target] 未找到 glTF 资产')
    print('  期望路径之一:')
    for _, p in ipairs(CANDIDATES) do print('    ' .. p) end
    print('  解决方法 (任选其一):')
    print('    Windows : powershell .\\samples\\demo_morph_target\\setup.ps1')
    print('    Unix    : ./samples/demo_morph_target/setup.sh')
    print('    Manual  : 下载任一含 morph target 的 glTF 2.0 文件 -> assets/morph.glb')
    print('demo_morph_target ok (no asset, exiting normally)')
    return
end

print(string.format('[demo_morph_target] 加载资产: %s', assetPath))

-- ==================== 3. 加载 glTF + 提取 morph 信息 ====================

local pack, errMsg = Anim.LoadSkinnedGLTF(assetPath)
if not pack then
    print('[demo_morph_target] LoadSkinnedGLTF 失败: ' .. tostring(errMsg))
    print('demo_morph_target ok (load failed)')
    return
end

local skeleton = pack.skeleton
local mesh     = pack.mesh         -- 可能为 nil (非 skinned mesh; AnimatedMorphCube 即如此)
local clips    = pack.clips or {}
local clipNames = pack.clipNames or {}

print(string.format('[demo_morph_target] skeleton=%s mesh=%s clips=%d',
    tostring(skeleton), tostring(mesh), #clipNames))

-- 打印 morph 信息 (mesh-side, 如有)
local meshMorphCount = 0
local meshMorphNames = {}
if mesh and mesh.HasMorphTargets then
    if mesh:HasMorphTargets() then
        meshMorphCount = mesh:GetMorphTargetCount()
        for i = 1, meshMorphCount do
            meshMorphNames[i] = mesh:GetMorphTargetName(i) or ('target_' .. (i - 1))
        end
        print(string.format('[demo_morph_target] mesh morph targets: %d', meshMorphCount))
        for i = 1, meshMorphCount do
            print(string.format('  [%d] %s', i, meshMorphNames[i]))
        end
    else
        print('[demo_morph_target] mesh 无 morph target')
    end
else
    print('[demo_morph_target] 无 SkinnedMesh (可能是无 skin 的 morph-only 资产)')
end

-- ==================== 4. 创建 Animator + 启动第一个 clip ====================

local animator
local activeClipName
if skeleton then
    animator = Anim.NewAnimator(skeleton)
    if animator then
        for _, cn in ipairs(clipNames) do
            local c = clips[cn]
            if c then
                animator:AddState(cn, c)
                if not activeClipName then
                    activeClipName = cn
                    animator:Play(cn)
                end
            end
        end
        animator:SetLooping(true)
        print(string.format('[demo_morph_target] animator created, active clip = %s',
            tostring(activeClipName)))
    end
end

-- ==================== 5. 检查 UI / Window 可用性 ====================

if not UI or not UI.Window then
    print('[demo_morph_target] UI.Window 不可用, 仅 API 验证模式')
    -- 仅验证 API, 不开窗
    if animator then
        print('[demo_morph_target] Animator API check:')
        print('  GetMorphTargetCount=' .. animator:GetMorphTargetCount())
        animator:SetMorphWeight(1, 0.5)
        print('  SetMorphWeight(1, 0.5); Get=' .. animator:GetMorphWeight(1))
        animator:ClearMorphWeights()
        print('  ClearMorphWeights done')
    end
    print('demo_morph_target ok (headless API check)')
    return
end

-- ==================== 6. 主交互循环 ====================

local Window = UI.Window
local win, openErr = Window.Open(800, 600, 'Phase AX — Morph Target Demo')
if not win then
    print('[demo_morph_target] Window.Open 失败: ' .. tostring(openErr))
    print('demo_morph_target ok (no window)')
    return
end

-- 状态
local activeSlot = 1                      -- 当前选中的 morph slot (1-based)
local paused    = false
local lastTime  = (Time and Time.GetSeconds and Time.GetSeconds()) or 0

-- 帮助函数: clamp [0, 1]
local function clamp01(v)
    if v < 0 then return 0 end
    if v > 1 then return 1 end
    return v
end

-- 主循环
while win:IsOpen() do
    -- 时间步进
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt  = now - lastTime
    lastTime  = now
    if dt > 0.1 then dt = 0.1 end

    -- 事件处理
    win:PollEvents()

    if win:IsKeyPressed('escape') then
        win:Close()
        break
    end
    -- 数字键 1-8: 切换激活 slot
    for i = 1, 8 do
        if win:IsKeyPressed(tostring(i)) then activeSlot = i end
    end
    -- ↑ / ↓: 调整当前 slot weight
    if animator and win:IsKeyPressed('up') then
        local w = animator:GetMorphWeight(activeSlot) or 0
        animator:SetMorphWeight(activeSlot, clamp01(w + 0.1))
    end
    if animator and win:IsKeyPressed('down') then
        local w = animator:GetMorphWeight(activeSlot) or 0
        animator:SetMorphWeight(activeSlot, clamp01(w - 0.1))
    end
    -- Q/W: 快捷设 0/1
    if animator and win:IsKeyPressed('q') then
        animator:SetMorphWeight(activeSlot, 0)
    end
    if animator and win:IsKeyPressed('w') then
        animator:SetMorphWeight(activeSlot, 1)
    end
    -- C: ClearMorphWeights (恢复动画驱动)
    if animator and win:IsKeyPressed('c') then
        animator:ClearMorphWeights()
    end
    -- SPACE: 暂停切换
    if animator and win:IsKeyPressed('space') then
        paused = not paused
        if paused then animator:Pause() else animator:Resume() end
    end
    -- G/N: 切换 skinning mode
    if win:IsKeyPressed('g') then Anim.SetSkinningMode('gpu') end
    if win:IsKeyPressed('n') then Anim.SetSkinningMode('cpu') end

    -- Update animator
    if animator then animator:Update(dt) end

    -- 渲染
    win:BeginFrame(0.10, 0.12, 0.15, 1.0)

    -- 3D mesh 渲染 (仅当 skin+morph 共存时有效)
    if mesh and animator then
        local transform = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, -3, 1,
        }
        Anim.DrawSkinnedMesh(mesh, animator, transform, nil)
    end

    -- OSD: 屏上文字
    local backendName = (Gfx.GetBackendName and Gfx.GetBackendName()) or '?'
    local skinMode    = Anim.GetSkinningMode()
    local lines = {
        string.format('Backend: %s | SkinMode: %s | Paused: %s',
            backendName, skinMode, tostring(paused)),
        string.format('Active slot: [%d]   Asset: %s', activeSlot, assetPath),
    }
    if mesh and meshMorphCount > 0 then
        lines[#lines + 1] = string.format('Mesh morph targets: %d', meshMorphCount)
    elseif not mesh then
        lines[#lines + 1] = '(no SkinnedMesh - morph-only asset; API tested headless)'
    end
    if animator then
        local animN = animator:GetMorphTargetCount() or 0
        lines[#lines + 1] = string.format('Animator weights (%d slots):', animN)
        for i = 1, math.max(animN, meshMorphCount) do
            local w     = animator:GetMorphWeight(i) or 0
            local manu  = animator:HasManualMorphOverride(i) and ' [MANUAL]' or ''
            local nm    = meshMorphNames[i] or string.format('slot_%d', i)
            local mark  = (i == activeSlot) and ' <--' or ''
            lines[#lines + 1] = string.format('  [%d] %s = %.2f%s%s', i, nm, w, manu, mark)
        end
    end
    lines[#lines + 1] = ''
    lines[#lines + 1] = 'Keys: 1-8=slot  Up/Down=+-0.1  Q/W=0/1  C=clear  G/N=gpu/cpu  Space=pause  Esc=quit'

    -- 简易文字渲染 (Window 提供, 否则跳过)
    if win.DrawText then
        for i, txt in ipairs(lines) do
            win:DrawText(8, 8 + (i - 1) * 16, txt, 1, 1, 1, 1)
        end
    end

    win:EndFrame()
end

if win.Close and win:IsOpen() then win:Close() end
print('demo_morph_target ok')
