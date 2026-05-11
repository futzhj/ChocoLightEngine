-- Phase D.x.4 smoke: Light.ECS × Light.Animation 集成
-- ASCII-only.
--
-- 单进程验证 SkinnedMeshRenderer + AnimationState component 注册 + 字段桥接 + 渲染调度.
-- 用 mock Light.Animation 替代真实模块, 验证调用顺序和参数.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end
local function eq(a, b, msg) if a ~= b then fail(msg .. " expected="..tostring(b).." got="..tostring(a)) end end

local ok, ECS = pcall(require, "Light.ECS")
if not ok then fail("require(Light.ECS): " .. tostring(ECS)) end
pass("require(Light.ECS) ok")
local World = ECS.World

-- ============================================================
-- 准备 mock Animator + Light.Animation 全局
-- ============================================================
local function makeMockAnimator()
    local calls = {}
    local an = {
        currentTime = 0,
        _state = '',
    }
    function an:Update(dt)         calls[#calls+1] = {fn='Update', dt=dt}; self.currentTime = (self.currentTime or 0) + dt end
    function an:Play(name)         calls[#calls+1] = {fn='Play', name=name}; self._state = name end
    function an:Crossfade(name, d) calls[#calls+1] = {fn='Crossfade', name=name, dur=d}; self._state = name end
    function an:SetSpeed(s)        calls[#calls+1] = {fn='SetSpeed', s=s} end
    function an:Pause()            calls[#calls+1] = {fn='Pause'} end
    function an:Resume()           calls[#calls+1] = {fn='Resume'} end
    function an:SetParam(k, v)     calls[#calls+1] = {fn='SetParam', k=k, v=v} end
    function an:SetMorphWeight(i, w) calls[#calls+1] = {fn='SetMorphWeight', i=i, w=w} end
    function an:AddState(name, clip) end  -- no-op
    function an:AddEvent(s, t, cb)   end  -- no-op
    an._calls = calls
    return an
end

local function makeMockAnimGlobal()
    local drawCalls = {}
    local origLight = Light
    Light = {Animation = {
        DrawSkinnedMesh = function(mesh, animator, model, mat)
            drawCalls[#drawCalls+1] = {mesh=mesh, animator=animator, model=model, material=mat}
        end,
    }}
    return drawCalls, origLight
end

local function restoreLight(origLight) Light = origLight end

local function countFn(calls, fn)
    local n = 0
    for _, c in ipairs(calls) do if c.fn == fn then n = n + 1 end end
    return n
end

local function lastCall(calls, fn)
    for i = #calls, 1, -1 do if calls[i].fn == fn then return calls[i] end end
    fail('missing call '..fn)
    return {}
end

-- ============================================================
-- Dx4-AC1: SkinnedMeshRenderer + AnimationState 注册
-- ============================================================
do
    local w = World.new()
    if not w._components.SkinnedMeshRenderer then fail("Dx4-AC1: SkinnedMeshRenderer not registered") end
    if not w._components.AnimationState     then fail("Dx4-AC1: AnimationState not registered") end
    if not w._builtin_render_comps.SkinnedMeshRenderer then fail("Dx4-AC1: SMR not in builtin marker") end
    if not w._builtin_render_comps.AnimationState     then fail("Dx4-AC1: AS  not in builtin marker") end
    pass("Dx4-AC1: 2 new builtin components registered")

    -- 默认 networked=false
    eq(w._networked_comps.SkinnedMeshRenderer, nil, "Dx4-AC1: SMR default not networked")
    eq(w._networked_comps.AnimationState,     nil, "Dx4-AC1: AS  default not networked")
    pass("Dx4-AC1: defaults networked=false")
end

-- ============================================================
-- Dx4-AC9: MarkRenderNetworked 排除 SkinnedMeshRenderer (含 userdata, 不可序列化)
-- ============================================================
do
    local w = World.new()
    w:MarkRenderNetworked()
    if w._networked_comps.SkinnedMeshRenderer then
        fail("Dx4-AC9: SkinnedMeshRenderer must NOT be networked (userdata)")
    end
    if not w._networked_comps.AnimationState then
        fail("Dx4-AC9: AnimationState should be networked")
    end
    -- Phase D 既有 6 个内置仍 networked
    for _, name in ipairs({"Transform2D","Sprite","Camera2D","Transform3D","MeshRenderer","Camera3D"}) do
        if not w._networked_comps[name] then fail("Dx4-AC9: "..name.." should still be networked") end
    end
    pass("Dx4-AC9: MarkRenderNetworked excludes SkinnedMeshRenderer correctly")
end

-- ============================================================
-- Dx4-AC2: world:Update 自动调 animator:Update(dt)
-- ============================================================
do
    local _, origLight = makeMockAnimGlobal()
    local w = World.new()
    local an = makeMockAnimator()
    w:CreateEntity():Add('Transform3D', {})
                    :Add('SkinnedMeshRenderer', {mesh='m', animator=an})
                    :Add('AnimationState',      {state=''})

    eq(countFn(an._calls, 'Update'), 0, "Dx4-AC2: Update not called before world:Update")
    w:Update(0.016)
    eq(countFn(an._calls, 'Update'), 1, "Dx4-AC2: Update called once after world:Update")
    local up = lastCall(an._calls, 'Update')
    eq(up.dt, 0.016, "Dx4-AC2: dt forwarded correctly")
    restoreLight(origLight)
    pass("Dx4-AC2: world:Update auto-calls animator:Update(dt)")
end

-- ============================================================
-- Dx4-AC3: state 改变 → Play (默认 crossfade=0)
-- ============================================================
do
    local _, origLight = makeMockAnimGlobal()
    local w = World.new()
    local an = makeMockAnimator()
    local e = w:CreateEntity():Add('Transform3D', {})
                              :Add('SkinnedMeshRenderer', {mesh='m', animator=an})
                              :Add('AnimationState',      {state='Idle'})
    w:Update(0.016)
    eq(countFn(an._calls, 'Play'), 1, "Dx4-AC3: Play called for initial state=Idle")
    eq(lastCall(an._calls, 'Play').name, 'Idle', "Dx4-AC3: Play(Idle)")

    -- 切到 Run (crossfade=0 → 仍 Play)
    e:Set('AnimationState', {state='Run'})
    w:Update(0.016)
    eq(countFn(an._calls, 'Play'), 2, "Dx4-AC3: Play called twice (Idle then Run)")
    eq(lastCall(an._calls, 'Play').name, 'Run', "Dx4-AC3: Play(Run)")

    -- 同 state 不重复
    w:Update(0.016)
    eq(countFn(an._calls, 'Play'), 2, "Dx4-AC3: same state does not re-Play")
    restoreLight(origLight)
    pass("Dx4-AC3: state change triggers Play (no crossfade)")
end

-- ============================================================
-- Dx4-AC3+: state 改变 + crossfade>0 → Crossfade
-- ============================================================
do
    local _, origLight = makeMockAnimGlobal()
    local w = World.new()
    local an = makeMockAnimator()
    local e = w:CreateEntity():Add('Transform3D', {})
                              :Add('SkinnedMeshRenderer', {mesh='m', animator=an})
                              :Add('AnimationState',      {state='Idle'})
    w:Update(0.016)
    e:Set('AnimationState', {state='Attack', crossfade=0.3})
    w:Update(0.016)
    eq(countFn(an._calls, 'Crossfade'), 1, "Dx4-AC3+: Crossfade called")
    local cf = lastCall(an._calls, 'Crossfade')
    eq(cf.name, 'Attack', "Dx4-AC3+: Crossfade target=Attack")
    eq(cf.dur,  0.3,     "Dx4-AC3+: Crossfade dur=0.3")
    restoreLight(origLight)
    pass("Dx4-AC3+: crossfade>0 triggers Crossfade")
end

-- ============================================================
-- Dx4-AC4: params diff apply → SetParam
-- ============================================================
do
    local _, origLight = makeMockAnimGlobal()
    local w = World.new()
    local an = makeMockAnimator()
    local e = w:CreateEntity():Add('Transform3D', {})
                              :Add('SkinnedMeshRenderer', {mesh='m', animator=an})
                              :Add('AnimationState',      {state='', params={isMoving=1, speed=2.5}})
    w:Update(0.016)
    eq(countFn(an._calls, 'SetParam'), 2, "Dx4-AC4: SetParam called for 2 params")

    -- 改 1 个值 → 只调 1 次
    e:Set('AnimationState', {state='', params={isMoving=0, speed=2.5}})  -- speed 不变, isMoving 0
    w:Update(0.016)
    eq(countFn(an._calls, 'SetParam'), 3, "Dx4-AC4: only changed param triggers SetParam")
    restoreLight(origLight)
    pass("Dx4-AC4: params diff-apply correct")
end

-- ============================================================
-- Dx4-AC5: morphWeights diff apply → SetMorphWeight
-- ============================================================
do
    local _, origLight = makeMockAnimGlobal()
    local w = World.new()
    local an = makeMockAnimator()
    local e = w:CreateEntity():Add('Transform3D', {})
                              :Add('SkinnedMeshRenderer', {mesh='m', animator=an})
                              :Add('AnimationState',      {state='', morphWeights={[1]=0.5, [2]=0.8}})
    w:Update(0.016)
    eq(countFn(an._calls, 'SetMorphWeight'), 2, "Dx4-AC5: SetMorphWeight called for 2 weights")
    restoreLight(origLight)
    pass("Dx4-AC5: morphWeights diff-apply correct")
end

-- ============================================================
-- Dx4-AC6: speed / paused 桥接
-- ============================================================
do
    local _, origLight = makeMockAnimGlobal()
    local w = World.new()
    local an = makeMockAnimator()
    local e = w:CreateEntity():Add('Transform3D', {})
                              :Add('SkinnedMeshRenderer', {mesh='m', animator=an})
                              :Add('AnimationState',      {state='', speed=2.0, paused=false})
    w:Update(0.016)
    eq(countFn(an._calls, 'SetSpeed'), 1, "Dx4-AC6: SetSpeed called")
    eq(lastCall(an._calls, 'SetSpeed').s, 2.0, "Dx4-AC6: speed=2.0")

    -- Pause
    e:Set('AnimationState', {state='', speed=2.0, paused=true})
    w:Update(0.016)
    eq(countFn(an._calls, 'Pause'), 1, "Dx4-AC6: Pause called")

    -- Resume
    e:Set('AnimationState', {state='', speed=2.0, paused=false})
    w:Update(0.016)
    eq(countFn(an._calls, 'Resume'), 1, "Dx4-AC6: Resume called")
    restoreLight(origLight)
    pass("Dx4-AC6: speed/paused bridged correctly")
end

-- ============================================================
-- Dx4-AC8: world:Render 调 DrawSkinnedMesh (visible=true 时)
-- ============================================================
do
    local drawCalls, origLight = makeMockAnimGlobal()
    -- 也给 Graphics mock 让 _DrawMesh/Render 不崩
    Light.Graphics = {
        Push=function() end, Pop=function() end,
        Translate=function() end, Rotate=function() end, Scale=function() end,
        SetColor=function() end, Draw=function() end, DrawQuad=function() end,
        SetPerspective=function() end, SetCamera=function() end, SetDepthTest=function() end,
    }
    local w = World.new()
    local an = makeMockAnimator()
    w:CreateEntity():Add('Transform3D', {x=10, y=20, z=30, ry=90})
                    :Add('SkinnedMeshRenderer', {mesh='m1', animator=an, material='mat1'})
                    :Add('AnimationState', {state=''})
    -- Camera3D 让 3D 阶段启用
    w:CreateEntity():Add('Transform3D', {x=0,y=5,z=10})
                    :Add('Camera3D',    {active=true})

    eq(#drawCalls, 0, "Dx4-AC8: DrawSkinnedMesh not called before Render")
    w:Render()
    eq(#drawCalls, 1, "Dx4-AC8: DrawSkinnedMesh called once")
    eq(drawCalls[1].mesh, 'm1', "Dx4-AC8: mesh forwarded")
    eq(drawCalls[1].material, 'mat1', "Dx4-AC8: material forwarded")
    eq(drawCalls[1].animator, an, "Dx4-AC8: animator forwarded")
    if type(drawCalls[1].model) ~= 'table' or #drawCalls[1].model ~= 16 then
        fail("Dx4-AC8: model should be 16-element table")
    end
    pass("Dx4-AC8: DrawSkinnedMesh invoked with correct args")

    restoreLight(origLight)
end

-- ============================================================
-- Dx4-AC8+: visible=false 时不调 DrawSkinnedMesh
-- ============================================================
do
    local drawCalls, origLight = makeMockAnimGlobal()
    Light.Graphics = {
        Push=function() end, Pop=function() end,
        Translate=function() end, Rotate=function() end, Scale=function() end,
        SetColor=function() end, Draw=function() end, DrawQuad=function() end,
        SetPerspective=function() end, SetCamera=function() end, SetDepthTest=function() end,
    }
    local w = World.new()
    local an = makeMockAnimator()
    w:CreateEntity():Add('Transform3D', {})
                    :Add('SkinnedMeshRenderer', {mesh='m', animator=an, visible=false})
                    :Add('AnimationState', {state=''})
    w:CreateEntity():Add('Transform3D', {}):Add('Camera3D', {active=true})

    w:Render()
    eq(#drawCalls, 0, "Dx4-AC8+: visible=false skips DrawSkinnedMesh")
    restoreLight(origLight)
    pass("Dx4-AC8+: visible=false skip ok")
end

-- ============================================================
-- 兼容性: Phase C/C.x.1/D 现有 API 不破坏
-- ============================================================
do
    local w = World.new()
    if type(w.NetworkSync) ~= 'function'        then fail("Compat: NetworkSync gone") end
    if type(w.MarkFullResync) ~= 'function'     then fail("Compat: MarkFullResync gone") end
    if type(w.Render) ~= 'function'             then fail("Compat: Render gone") end
    if type(w.MarkRenderNetworked) ~= 'function' then fail("Compat: MarkRenderNetworked gone") end
    if type(ECS.MirrorFromRoom) ~= 'function'   then fail("Compat: MirrorFromRoom gone") end
    -- Phase D 6 个内置仍存在
    for _, name in ipairs({"Transform2D","Sprite","Camera2D","Transform3D","MeshRenderer","Camera3D"}) do
        if not w._components[name] then fail("Compat: "..name.." gone") end
    end
    pass("Compat: Phase C/C.x.1/D API all preserved")
end

print("")
print("Phase D.x.4 SkinnedMesh ECS smoke: ALL PASS")
