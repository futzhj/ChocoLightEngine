-- ============================================================================
-- ChocoLight Phase AX — Morph Target Demo (callback-model)
-- ============================================================================
-- 用途: 加载 glTF morph mesh, 播 weights animation, 键盘控制 morph weight 切换/暂停.
--
-- 控制 (GLFW 键码):
--   1-8   : 选择 morph slot
--   ↑/↓   : 当前 slot weight ±0.1
--   Q/W   : 快捷设 0/1
--   C     : ClearMorphWeights (恢复动画驱动)
--   G/N   : SetSkinningMode -> gpu / cpu
--   SPACE : 暂停 / 恢复 Animator
--   ESC   : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local Anim = safe_require('Light.Animation')
local Time = safe_require('Light.Time')
local UI   = safe_require('Light.UI')
local Gfx  = safe_require('Light.Graphics')

if not (Anim and Time and Gfx) then
    print('[demo_morph_target] 必需模块缺失 (Animation/Time/Graphics)')
    print('  Anim=' .. tostring(Anim) .. ' Time=' .. tostring(Time) .. ' Gfx=' .. tostring(Gfx))
    print('demo_morph_target ok (modules unavailable)')
    return
end

-- 资产探测
local function file_exists(p) local f = io.open(p, 'rb'); if f then f:close(); return true end; return false end
local CANDIDATES = {
    'samples/demo_morph_target/assets/morph.glb',
    'samples/demo_animation/assets/character.glb',
    'samples/demo_skinning_perf/assets/character.glb',
    'assets/morph.glb',
}
local function find_asset()
    for _, p in ipairs(CANDIDATES) do if file_exists(p) then return p end end
    return nil
end
local assetPath = find_asset()
if not assetPath then
    print('[demo_morph_target] 未找到 glTF 资产')
    print('  期望路径之一:'); for _, p in ipairs(CANDIDATES) do print('    ' .. p) end
    print('  解决: powershell .\\samples\\demo_morph_target\\setup.ps1')
    print('demo_morph_target ok (no asset)')
    return
end
print(string.format('[demo_morph_target] 加载资产: %s', assetPath))

-- 加载 glTF
local pack, errMsg = Anim.LoadSkinnedGLTF(assetPath)
if not pack then
    print('[demo_morph_target] LoadSkinnedGLTF 失败: ' .. tostring(errMsg))
    print('demo_morph_target ok (load failed)')
    return
end
local skeleton, mesh, clips, clipNames = pack.skeleton, pack.mesh, pack.clips or {}, pack.clipNames or {}
print(string.format('[demo_morph_target] skeleton=%s mesh=%s clips=%d',
    tostring(skeleton), tostring(mesh), #clipNames))

-- mesh-side morph 信息
local meshMorphCount, meshMorphNames = 0, {}
if mesh and mesh.HasMorphTargets then
    if mesh:HasMorphTargets() then
        meshMorphCount = mesh:GetMorphTargetCount()
        for i = 1, meshMorphCount do meshMorphNames[i] = mesh:GetMorphTargetName(i) or ('target_' .. (i - 1)) end
        print(string.format('[demo_morph_target] mesh morph targets: %d', meshMorphCount))
        for i = 1, meshMorphCount do print(string.format('  [%d] %s', i, meshMorphNames[i])) end
        if mesh.GetMorphTargetIndex and #meshMorphNames > 0 then
            local first = meshMorphNames[1]
            local idx = mesh:GetMorphTargetIndex(first)
            if idx == 1 then print(string.format("[demo_morph_target] T09 helper OK: GetMorphTargetIndex('%s')=%d", first, idx)) end
            if mesh:GetMorphTargetIndex('__no_such_morph__') == nil then
                print('[demo_morph_target] T09 helper OK: missing name -> nil')
            end
        end
    else print('[demo_morph_target] mesh 无 morph target') end
else
    print('[demo_morph_target] 无 SkinnedMesh (morph-only 资产)')
end

-- 创建 Animator + 启第一个 clip
local animator, activeClipName
if skeleton then
    animator = Anim.NewAnimator(skeleton)
    if animator then
        for _, cn in ipairs(clipNames) do
            local c = clips[cn]
            if c then
                animator:AddState(cn, c)
                if not activeClipName then activeClipName = cn; animator:Play(cn) end
            end
        end
        animator:SetLooping(true)
        print(string.format('[demo_morph_target] animator created, active clip = %s', tostring(activeClipName)))
    end
end

-- Headless probe
if not UI or not UI.Window then
    print('[demo_morph_target] UI.Window 不可用, 仅 API 验证模式')
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
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_morph_target] Light global 不可用'); print('demo_morph_target ok (no Light global)'); return
end

local function clamp01(v) if v < 0 then return 0 end; if v > 1 then return 1 end; return v end

local Demo = Light(Light.UI.Window):New()
local g_activeSlot = 1
local g_paused     = false

function Demo:OnOpen()
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    if animator then animator:Update(dt) end
end

function Demo:Draw()
    if mesh and animator then
        local transform = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,-3,1 }
        Anim.DrawSkinnedMesh(mesh, animator, transform, nil)
    end
    if Gfx.Print then
        local backendName = (Gfx.GetBackendName and Gfx.GetBackendName()) or '?'
        local skinMode    = Anim.GetSkinningMode()
        local lines = {
            string.format('Backend: %s | SkinMode: %s | Paused: %s', backendName, skinMode, tostring(g_paused)),
            string.format('Active slot: [%d]   Asset: %s', g_activeSlot, assetPath),
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
                local w    = animator:GetMorphWeight(i) or 0
                local manu = animator:HasManualMorphOverride(i) and ' [MANUAL]' or ''
                local nm   = meshMorphNames[i] or string.format('slot_%d', i)
                local mark = (i == g_activeSlot) and ' <--' or ''
                lines[#lines + 1] = string.format('  [%d] %s = %.2f%s%s', i, nm, w, manu, mark)
            end
        end
        lines[#lines + 1] = ''
        lines[#lines + 1] = 'Keys: 1-8=slot  Up/Down=+-0.1  Q/W=0/1  C=clear  G/N=gpu/cpu  Space=pause  Esc=quit'
        for i, txt in ipairs(lines) do Gfx.Print(txt, 8, 8 + (i - 1) * 16, 0) end
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()                                        -- ESC
    elseif key >= string.byte('1') and key <= string.byte('8') then        -- '1'..'8' = 49..56
        g_activeSlot = key - string.byte('0')
    elseif key == 265 then                                                 -- UP
        if animator then
            local w = animator:GetMorphWeight(g_activeSlot) or 0
            animator:SetMorphWeight(g_activeSlot, clamp01(w + 0.1))
        end
    elseif key == 264 then                                                 -- DOWN
        if animator then
            local w = animator:GetMorphWeight(g_activeSlot) or 0
            animator:SetMorphWeight(g_activeSlot, clamp01(w - 0.1))
        end
    elseif key == string.byte('Q') then if animator then animator:SetMorphWeight(g_activeSlot, 0) end
    elseif key == string.byte('W') then if animator then animator:SetMorphWeight(g_activeSlot, 1) end
    elseif key == string.byte('C') then if animator then animator:ClearMorphWeights() end
    elseif key == 32 then                                                  -- SPACE
        if animator then
            g_paused = not g_paused
            if g_paused then animator:Pause() else animator:Resume() end
        end
    elseif key == string.byte('G') then Anim.SetSkinningMode('gpu')
    elseif key == string.byte('N') then Anim.SetSkinningMode('cpu')
    end
end

Demo:Open(800, 600, 'Phase AX - Morph Target Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
print('demo_morph_target ok')
