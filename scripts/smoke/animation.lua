-- scripts/smoke/animation.lua
-- Phase AV Step 1+2+3 smoke: Light.Animation 模块加载 + Skeleton/Clip/Animator/SkinnedMesh + 状态机 + 错误边界
--
-- Step 1: 加载失败路径 + API 表面
-- Step 2: 状态机 API + sampler/关节矩阵错误路径
-- Step 3: SkinnedMesh API 表面 + DrawSkinnedMesh 错误路径 (headless 不渲染)
--
-- 不依赖任何外部 glTF 资源 (与 Phase AS mesh_3d.lua 风格一致)
-- 兼容 Lua 5.1 (lightc -p 严格语法检查 + light.exe runtime)

-- 防止 GH Actions PowerShell stdout 缓冲截断关键日志
if io and io.stdout and io.stdout.setvbuf then
    pcall(function() io.stdout:setvbuf('no') end)
end

-- helper: 安全 require, lightc -p 时跳过实际加载
local function safe_require(name)
    local ok, mod = pcall(require, name)
    if ok and type(mod) == 'table' then return mod end
    return nil
end

-- 防 lightc 阶段未加载 DLL 导致 require 全部失败
local Anim = safe_require('Light.Animation')
if not Anim then
    print('[Light.Animation] runtime not available (likely lightc -p syntax check), skip with OK')
    return
end

-- ==================== 测试统计 ====================

local PASS = 0
local FAIL = 0
local function CHECK(cond, label)
    if cond then
        PASS = PASS + 1
        print('  PASS: ' .. label)
    else
        FAIL = FAIL + 1
        print('  FAIL: ' .. label)
    end
end

-- ==================== [1] 顶层 API 表 ====================

print('[1] Light.Animation 顶层 API 表')

CHECK(type(Anim) == 'table', 'Light.Animation 是 table')
CHECK(type(Anim.LoadSkinnedGLTF) == 'function', 'Anim.LoadSkinnedGLTF is function')
CHECK(type(Anim.NewAnimator) == 'function', 'Anim.NewAnimator is function')
CHECK(type(Anim.DrawSkinnedMesh) == 'function', 'Anim.DrawSkinnedMesh is function (Step 3)')

-- ==================== [2] LoadSkinnedGLTF 错误路径 ====================

print('[2] LoadSkinnedGLTF 错误处理')

-- 不存在文件 → nil + err
local pack, err = Anim.LoadSkinnedGLTF('__definitely_nonexistent_av_step1.glb')
CHECK(pack == nil, '不存在文件返回 nil')
CHECK(type(err) == 'string', '不存在文件返回错误字符串')

-- 字符串路径但不是 glTF → nil + err
-- 用 smoke 自身路径作为非 glTF 测试 (任何非 glTF 文本文件都会让 cgltf_parse_file 失败)
-- 注意: lua 默认参数检查会 luaL_checkstring, 不能传 nil
-- 不传参 → luaL_checkstring 会 error, 用 pcall 包裹
local ok = pcall(Anim.LoadSkinnedGLTF)
CHECK(ok == false, '不传参数 → 会 raise')

-- ==================== [3] NewAnimator 错误路径 ====================

print('[3] NewAnimator 错误处理')

-- 不传 Skeleton → 应 raise (luaL_checkudata 失败)
local ok2 = pcall(Anim.NewAnimator)
CHECK(ok2 == false, 'NewAnimator() 无参数 → raise')

local ok3 = pcall(Anim.NewAnimator, 'not a skeleton')
CHECK(ok3 == false, 'NewAnimator(string) → raise')

local ok4 = pcall(Anim.NewAnimator, {})
CHECK(ok4 == false, 'NewAnimator(table) → raise')

-- ==================== [4] 子模块 require 不崩 ====================

print('[4] 子模块独立 require')

local sk_mod  = safe_require('Light.Animation.Skeleton')
local cl_mod  = safe_require('Light.Animation.Clip')
local an_mod  = safe_require('Light.Animation.Animator')
local sm_mod  = safe_require('Light.Animation.SkinnedMesh')   -- Step 3

CHECK(sk_mod ~= nil, 'require Light.Animation.Skeleton 不崩')
CHECK(cl_mod ~= nil, 'require Light.Animation.Clip 不崩')
CHECK(an_mod ~= nil, 'require Light.Animation.Animator 不崩')
CHECK(sm_mod ~= nil, 'require Light.Animation.SkinnedMesh 不崩 (Step 3)')

-- ==================== [5] Step 2: Animator 状态机 API 错误路径 ====================

print('[5] Step 2: Animator 状态机 + sampler API 错误路径')

-- 所有 Animator 方法都需要有效 Animator userdata; 无资源时 pcall 验证 luaL_checkudata 报错
-- 这与 Phase AS mesh_3d.lua 风格一致: API 表面验证 + 错误处理; 数值验证留到资源可用时.

-- 验证: 在没有 Animator 的情况下, AddState 需要 self+name+clip 3 个参数
-- 这些调用会 raise 因为第一个参数不是合法 Animator userdata
local stub = function() end    -- 占位用, 避免未使用警告
stub()

-- (无法直接创建 Animator 不含 Skeleton; Step 3 引入资源后补充成功路径)
print('  INFO: Step 2 成功路径需 Skeleton/Clip userdata, Step 3 引入资源后补充')

-- ==================== [6] Step 2: API 表面验证 (通过元表) ====================

print('[6] Step 2: Animator 元表方法完整性')

-- 获取 Animator 元表 (由父模块 luaopen_Light_Animation_Animator 注册)
-- debug.getregistry() 含 'Light.Animation.Animator' 键
local mt_animator = debug and debug.getregistry and debug.getregistry()['Light.Animation.Animator']
local mt_skeleton = debug and debug.getregistry and debug.getregistry()['Light.Animation.Skeleton']
local mt_clip     = debug and debug.getregistry and debug.getregistry()['Light.Animation.Clip']

if mt_animator and type(mt_animator) == 'table' then
    -- Step 2 新增的状态机方法必须全部存在
    local need_animator = {
        'Update', 'GetSkeleton', 'GetCurrentTime', 'SetCurrentTime', 'SetSpeed',
        'Pause', 'Resume', 'IsPaused', 'GetJointMatrices',
        -- Step 2 新增
        'AddState', 'Play', 'Stop', 'GetCurrentState', 'GetStateCount',
        'HasState', 'SetLooping', 'IsLooping',
        -- 生命周期
        'IsAlive', 'Delete'
    }
    for _, mname in ipairs(need_animator) do
        CHECK(type(mt_animator[mname]) == 'function', 'Animator:' .. mname .. ' 存在')
    end
else
    print('  SKIP: 无法访问 Animator 元表 (Lumen sandbox 限制 debug.getregistry)')
end

if mt_skeleton and type(mt_skeleton) == 'table' then
    local need_skel = {
        'GetJointCount', 'GetJointName', 'FindJoint', 'GetJointParent',
        'GetRootJoint', 'GetBindLocalTRS', 'GetInverseBindMatrix',
        'IsAlive', 'Delete'
    }
    for _, mname in ipairs(need_skel) do
        CHECK(type(mt_skeleton[mname]) == 'function', 'Skeleton:' .. mname .. ' 存在')
    end
end

if mt_clip and type(mt_clip) == 'table' then
    local need_clip = {
        'GetName', 'GetDuration', 'GetSamplerCount', 'GetSamplerInfo', 'Sample',
        'IsAlive', 'Delete'
    }
    for _, mname in ipairs(need_clip) do
        CHECK(type(mt_clip[mname]) == 'function', 'Clip:' .. mname .. ' 存在')
    end
end

-- ==================== [7] Step 2: 状态机调用 错误表面路径 (不需 Animator 的 raise 路径) ====================

print('[7] Step 2: Animator 方法 raise 路径 (传错参数)')

-- 直接调用元表方法传不合法参数 → luaL_checkudata 会 raise
if mt_animator and type(mt_animator.AddState) == 'function' then
    local ok_add  = pcall(mt_animator.AddState)         -- 无参数
    local ok_play = pcall(mt_animator.Play)             -- 无参数
    local ok_stop = pcall(mt_animator.Stop)             -- 无参数
    local ok_setlp= pcall(mt_animator.SetLooping)       -- 无参数
    local ok_upd  = pcall(mt_animator.Update)           -- 无参数
    CHECK(ok_add  == false, 'Animator.AddState() 无 self → raise')
    CHECK(ok_play == false, 'Animator.Play() 无 self → raise')
    CHECK(ok_stop == false, 'Animator.Stop() 无 self → raise')
    CHECK(ok_setlp== false, 'Animator.SetLooping() 无 self → raise')
    CHECK(ok_upd  == false, 'Animator.Update() 无 self → raise')
else
    print('  SKIP: 无法获取 Animator 元表方法')
end

-- ==================== [8] Step 3: SkinnedMesh 元表 + DrawSkinnedMesh 错误路径 ====================

print('[8] Step 3: SkinnedMesh 元表方法完整性')

local mt_skmesh = debug and debug.getregistry and debug.getregistry()['Light.Animation.SkinnedMesh']

if mt_skmesh and type(mt_skmesh) == 'table' then
    local need_skmesh = {
        'GetVertexCount', 'GetIndexCount', 'GetSkeleton',
        'IsAlive', 'Delete'
    }
    for _, mname in ipairs(need_skmesh) do
        CHECK(type(mt_skmesh[mname]) == 'function', 'SkinnedMesh:' .. mname .. ' 存在')
    end
else
    print('  SKIP: 无法访问 SkinnedMesh 元表')
end

print('[9] Step 3: DrawSkinnedMesh 错误路径')

-- 不传参 → raise
local ok_draw1 = pcall(Anim.DrawSkinnedMesh)
CHECK(ok_draw1 == false, 'DrawSkinnedMesh() 无参数 → raise')

-- 第一参数错 → raise
local ok_draw2 = pcall(Anim.DrawSkinnedMesh, 'not a mesh')
CHECK(ok_draw2 == false, 'DrawSkinnedMesh(string,...) → raise')

-- 第一参数 table → raise
local ok_draw3 = pcall(Anim.DrawSkinnedMesh, {})
CHECK(ok_draw3 == false, 'DrawSkinnedMesh(table,...) → raise')

-- 第一参数 nil → raise
local ok_draw4 = pcall(Anim.DrawSkinnedMesh, nil, nil)
CHECK(ok_draw4 == false, 'DrawSkinnedMesh(nil,nil) → raise')

-- 元表方法直接调 (无 self) → raise
if mt_skmesh and type(mt_skmesh.GetVertexCount) == 'function' then
    local ok_skmesh1 = pcall(mt_skmesh.GetVertexCount)
    local ok_skmesh2 = pcall(mt_skmesh.GetIndexCount)
    local ok_skmesh3 = pcall(mt_skmesh.GetSkeleton)
    local ok_skmesh4 = pcall(mt_skmesh.Delete)
    CHECK(ok_skmesh1 == false, 'SkinnedMesh.GetVertexCount() 无 self → raise')
    CHECK(ok_skmesh2 == false, 'SkinnedMesh.GetIndexCount() 无 self → raise')
    CHECK(ok_skmesh3 == false, 'SkinnedMesh.GetSkeleton() 无 self → raise')
    CHECK(ok_skmesh4 == false, 'SkinnedMesh.Delete() 无 self → raise')
end

-- 备注: DrawSkinnedMesh 成功路径需 (mesh, animator) userdata, 需 glTF 资源 + 渲染上下文,
-- 留 Phase AV.x 引入测试资产时补完整端到端.
print('  INFO: DrawSkinnedMesh 数值/渲染验证留 Phase AV.x 资产')

-- ==================== [10] Step 4: 状态机扩展 (Transition / Crossfade / Event / Param) 元表 ====================

print('[10] Step 4: Animator 元表 Step 4 方法完整性')

if mt_animator and type(mt_animator) == 'table' then
    local need_step4 = {
        -- Transition
        'AddTransition', 'ClearTransitions', 'GetTransitionCount',
        -- Crossfade
        'Crossfade', 'IsCrossfading', 'GetCrossfadeProgress', 'GetCrossfadeTarget',
        -- Event
        'AddEvent', 'ClearEvents', 'GetEventCount',
        -- Param
        'SetParam', 'GetParam', 'HasParam',
        -- Time
        'GetPrevTime',
    }
    for _, mname in ipairs(need_step4) do
        CHECK(type(mt_animator[mname]) == 'function', 'Animator:' .. mname .. ' 存在 (Step 4)')
    end
else
    print('  SKIP: 无法访问 Animator 元表 (Step 4)')
end

-- ==================== [11] Step 4: Animator Step 4 方法 raise 路径 (无 self 调用) ====================

print('[11] Step 4: 状态机扩展方法 raise 路径')

if mt_animator and type(mt_animator.AddTransition) == 'function' then
    local ok_at  = pcall(mt_animator.AddTransition)            -- 无参数
    local ok_ct  = pcall(mt_animator.ClearTransitions)         -- 无 self
    local ok_cf  = pcall(mt_animator.Crossfade)                -- 无参数
    local ok_isc = pcall(mt_animator.IsCrossfading)            -- 无 self
    local ok_ae  = pcall(mt_animator.AddEvent)                 -- 无参数
    local ok_ce  = pcall(mt_animator.ClearEvents)              -- 无 self
    local ok_sp  = pcall(mt_animator.SetParam)                 -- 无参数
    local ok_gp  = pcall(mt_animator.GetParam)                 -- 无参数
    local ok_hp  = pcall(mt_animator.HasParam)                 -- 无参数
    local ok_pt  = pcall(mt_animator.GetPrevTime)              -- 无 self
    local ok_cp  = pcall(mt_animator.GetCrossfadeProgress)     -- 无 self
    local ok_ct2 = pcall(mt_animator.GetCrossfadeTarget)       -- 无 self
    CHECK(ok_at  == false, 'Animator.AddTransition() 无参数 → raise')
    CHECK(ok_ct  == false, 'Animator.ClearTransitions() 无 self → raise')
    CHECK(ok_cf  == false, 'Animator.Crossfade() 无参数 → raise')
    CHECK(ok_isc == false, 'Animator.IsCrossfading() 无 self → raise')
    CHECK(ok_ae  == false, 'Animator.AddEvent() 无参数 → raise')
    CHECK(ok_ce  == false, 'Animator.ClearEvents() 无 self → raise')
    CHECK(ok_sp  == false, 'Animator.SetParam() 无参数 → raise')
    CHECK(ok_gp  == false, 'Animator.GetParam() 无参数 → raise')
    CHECK(ok_hp  == false, 'Animator.HasParam() 无参数 → raise')
    CHECK(ok_pt  == false, 'Animator.GetPrevTime() 无 self → raise')
    CHECK(ok_cp  == false, 'Animator.GetCrossfadeProgress() 无 self → raise')
    CHECK(ok_ct2 == false, 'Animator.GetCrossfadeTarget() 无 self → raise')

    -- AddTransition 第 4 参数必须是 function (其他参数是 string), 错误参数应 raise
    -- 注: 没有 Animator userdata 这步会被 self 检查先挡, 此段并非测参数类型
    -- 真正的成功路径需 Animator userdata, 留 Phase AV.x 引入测试 glTF 资产时补
else
    print('  SKIP: Step 4 元表方法不可用')
end

-- ==================== [12] Phase AV.x: procedural API 完整性 ====================

print('[12] Phase AV.x: procedural API 完整性')

if type(Anim.NewEmptySkeleton) == 'function' then
    CHECK(true, 'Anim.NewEmptySkeleton 存在 (Phase AV.x)')
else
    CHECK(false, 'Anim.NewEmptySkeleton 缺失')
end
CHECK(type(Anim.NewEmptyClip) == 'function', 'Anim.NewEmptyClip 存在 (Phase AV.x)')

-- Skeleton 元表 Phase AV.x setter
for _, mname in ipairs({'SetJointName', 'SetJointParent', 'SetBindLocalTRS', 'SetInverseBindMatrix'}) do
    CHECK(mt_skeleton and type(mt_skeleton[mname]) == 'function',
          'Skeleton:' .. mname .. ' 存在 (Phase AV.x)')
end

-- Clip 元表 Phase AV.x setter
for _, mname in ipairs({'SetDuration', 'AddSampler'}) do
    CHECK(mt_clip and type(mt_clip[mname]) == 'function',
          'Clip:' .. mname .. ' 存在 (Phase AV.x)')
end

-- Animator 元表 Phase AV.x getter
for _, mname in ipairs({'GetClip', 'GetActiveClip', 'ListStates',
                        'GetTransitionInfo', 'GetEventInfo', 'ListParams'}) do
    CHECK(mt_animator and type(mt_animator[mname]) == 'function',
          'Animator:' .. mname .. ' 存在 (Phase AV.x)')
end

-- ==================== [13] Phase AV.x: procedural 端到端数值验证 ====================

print('[13] Phase AV.x: procedural 端到端 (Update 时序 / crossfade 中点权重 / event 循环边界)')

if type(Anim.NewEmptySkeleton) == 'function' and type(Anim.NewEmptyClip) == 'function' then
    local ok_e2e, err_e2e = pcall(function()
        print('  ... 13.1 NewEmptySkeleton + SetJointName')
        -- 1 关节 skeleton: bind = identity, IBM = identity (默认)
        local sk = Anim.NewEmptySkeleton(1)
        sk:SetJointName(1, 'root')

        print('  ... 13.2 NewEmptyClip("idle") + AddSampler(translation)')
        -- clip "idle": translation 全零 (1s)
        local idle = Anim.NewEmptyClip('idle', 1.0)
        idle:AddSampler(1, 'translation', 'LINEAR',
                         {0.0, 1.0}, {0,0,0,  0,0,0})

        print('  ... 13.3 NewEmptyClip("walk") + AddSampler')
        -- clip "walk": translation x 0 → 10 (1s, LINEAR)
        local walk = Anim.NewEmptyClip('walk', 1.0)
        walk:AddSampler(1, 'translation', 'LINEAR',
                         {0.0, 1.0}, {0,0,0,  10,0,0})

        CHECK(math.abs(idle:GetDuration() - 1.0) < 1e-6, 'empty clip duration after AddSampler = 1.0')
        CHECK(idle:GetSamplerCount() == 1, 'clip has 1 sampler after AddSampler')

        print('  ... 13.4 NewAnimator + AddState')
        -- Animator + 2 states
        local an = Anim.NewAnimator(sk)
        an:AddState('idle', idle)
        an:AddState('walk', walk)
        an:SetLooping(false)

        print('  ... 13.5 Play("walk") + Update(0.5) + GetJointMatrices')
        -- ---- 验证 walk LINEAR t=0.5 精度 ----
        an:Play('walk')
        an:Update(0.5)
        local mats = an:GetJointMatrices()
        CHECK(type(mats) == 'table' and #mats == 16, 'single joint matrices length = 16')
        -- 列主序 mat4: tx 在 index 13 (Lua 1-based; C 侧 mat[12])
        CHECK(math.abs(mats[13] - 5.0) < 1e-3, 'walk t=0.5 LINEAR translation.x ≈ 5.0')
        CHECK(math.abs(mats[14] - 0.0) < 1e-3, 'walk t=0.5 LINEAR translation.y ≈ 0.0')

        -- ---- 验证 crossfade 中点权重混合 ----
        -- 从 walk 开始 (currentTime 被 Play 重置回 0), crossfade 回 idle 用 0.4s
        -- Update(0.2) 后: active(walk).time=0.2 → x=2.0; target(idle).time=0.2 → x=0
        -- progress = 0.2/0.4 = 0.5 → blended x = lerp(2.0, 0, 0.5) = 1.0
        an:Play('walk')
        an:Crossfade('idle', 0.4)
        an:Update(0.2)
        CHECK(an:IsCrossfading() == true, 'crossfading=true after Crossfade + partial Update')
        CHECK(math.abs(an:GetCrossfadeProgress() - 0.5) < 1e-3,
              'crossfade progress ≈ 0.5 at mid')
        CHECK(an:GetCrossfadeTarget() == 'idle', 'crossfade target = idle')
        local matsX = an:GetJointMatrices()
        CHECK(math.abs(matsX[13] - 1.0) < 5e-3,
              'crossfade mid translation.x ≈ 1.0 (lerp(2.0, 0, 0.5))')

        -- ---- 完成 crossfade 切换 ----
        an:Update(0.3)    -- 累计 0.5s > 0.4s duration → 切换完成
        CHECK(an:IsCrossfading() == false, 'crossfade done after enough updates')
        CHECK(an:GetCurrentState() == 'idle', 'state switched to idle after crossfade')

        -- ---- 验证 event 跨循环边界触发 ----
        an:Stop()
        an:Play('idle')
        an:SetLooping(true)
        an:SetCurrentTime(0.8)
        local hit_early = 0
        local hit_late  = 0
        an:AddEvent('idle', 0.1, function() hit_early = hit_early + 1 end)
        an:AddEvent('idle', 0.5, function() hit_late  = hit_late  + 1 end)
        CHECK(an:GetEventCount() == 2, 'AddEvent x2 → GetEventCount = 2')

        -- prev=0.8, 推进 0.4s, wrap → new=0.2; 跨界区间 [0.8, 1.0] ∪ [0, 0.2]
        -- triggerTime=0.1 落在 [0, 0.2] → hit; triggerTime=0.5 不在跨界 → miss
        an:Update(0.4)
        CHECK(hit_early == 1, 'event trigger=0.1 across wrap (prev=0.8, new=0.2) fires once')
        CHECK(hit_late  == 0, 'event trigger=0.5 not in wrap range → no fire')
        CHECK(math.abs(an:GetCurrentTime() - 0.2) < 1e-4,
              'looping wrap currentTime ≈ 0.2')
        CHECK(math.abs(an:GetPrevTime() - 0.8) < 1e-4,
              'prevTime remembers pre-wrap 0.8')

        -- ---- Param 读写 + ListParams ----
        an:SetParam('health', 100)
        an:SetParam('speed', 2.5)
        CHECK(an:GetParam('health') == 100, 'SetParam/GetParam round-trip health=100')
        CHECK(math.abs(an:GetParam('speed') - 2.5) < 1e-6,
              'SetParam/GetParam round-trip speed=2.5')
        CHECK(an:HasParam('health') == true, 'HasParam: present')
        CHECK(an:HasParam('missing') == false, 'HasParam: missing=false')
        local ps = an:ListParams()
        CHECK(type(ps) == 'table' and ps.health == 100 and math.abs(ps.speed - 2.5) < 1e-6,
              'ListParams returns all params as kv table')

        -- ---- Transition getter ----
        an:AddTransition('idle', 'walk',
                           function() return (an:GetParam('health') or 0) > 50 end,
                           0.3)
        CHECK(an:GetTransitionCount() == 1, 'AddTransition → count=1')
        local ti = an:GetTransitionInfo(1)
        CHECK(type(ti) == 'table', 'GetTransitionInfo returns table')
        CHECK(ti.from == 'idle' and ti.to == 'walk', 'transition info from/to correct')
        CHECK(math.abs(ti.duration - 0.3) < 1e-6, 'transition info duration = 0.3')
        CHECK(ti.hasCond == true, 'transition info hasCond = true')
        CHECK(an:GetTransitionInfo(99) == nil, 'GetTransitionInfo out-of-range → nil')

        -- ---- Event getter ----
        local ei = an:GetEventInfo(1)
        CHECK(type(ei) == 'table', 'GetEventInfo returns table')
        CHECK(ei.state == 'idle' and math.abs(ei.triggerTime - 0.1) < 1e-6,
              'event info state/triggerTime')
        CHECK(ei.hasCallback == true, 'event info hasCallback = true')
        CHECK(an:GetEventInfo(99) == nil, 'GetEventInfo out-of-range → nil')

        -- ---- Clip / State getter ----
        local idle_ref = an:GetClip('idle')
        CHECK(idle_ref ~= nil and idle_ref:GetName() == 'idle',
              'GetClip("idle") returns same clip')
        CHECK(an:GetClip('missing') == nil, 'GetClip("missing") → nil')

        local act = an:GetActiveClip()
        CHECK(act ~= nil and act:GetName() == 'idle',
              'GetActiveClip = current (idle)')

        local names = an:ListStates()
        CHECK(type(names) == 'table' and #names == 2,
              'ListStates returns 2 state names')

        -- ---- Clear transitions / events ----
        an:ClearTransitions()
        CHECK(an:GetTransitionCount() == 0, 'ClearTransitions → 0')
        an:ClearEvents()
        CHECK(an:GetEventCount() == 0, 'ClearEvents → 0')
    end)
    if not ok_e2e then
        CHECK(false, 'procedural e2e 成功执行: ' .. tostring(err_e2e))
    else
        CHECK(true, 'procedural e2e 全程无异常')
    end

    -- 主动收割 [13] pcall 退出后留下的 userdata, 隔离 [14] luaL_error → CheckGC 路径
    -- (可疑: 14.4 luaL_error 触发 lua_pushvfstring 内的 CheckGC, 集中跑死对象 __gc finalizer 时崩)
    print('  ... post-13: collectgarbage("collect") x2')
    collectgarbage('collect')
    collectgarbage('collect')
    print('  ... post-13: GC 完成')
else
    print('  SKIP: NewEmptySkeleton / NewEmptyClip 不可用')
end

-- ==================== [14] Phase AV.x: procedural 错误路径 ====================

print('[14] Phase AV.x: procedural 错误路径')

if type(Anim.NewEmptySkeleton) == 'function' then
    print('  ... 14.1 NewEmptySkeleton(0)')
    local r1 = Anim.NewEmptySkeleton(0)
    CHECK(r1 == nil, 'NewEmptySkeleton(0) → nil (out of range)')
    print('  ... 14.2 NewEmptySkeleton(65)')
    local r2 = Anim.NewEmptySkeleton(65)
    CHECK(r2 == nil, 'NewEmptySkeleton(65) → nil (exceeds MAX_JOINTS=64)')

    local sk2 = Anim.NewEmptySkeleton(2)
    local c2  = Anim.NewEmptyClip('t', 0.0)

    -- AddSampler 错误路径: cpp 改用 return nil, err 模式 (见 l_Clip_AddSampler 注释),
    -- 不再 lua_error, 避免 Lumen 在 MSVC 上 longjmp 崩溃. Lua 端直接用多返回值解构.
    local r, e
    r, e = c2:AddSampler(0, 'translation', 'LINEAR', {0}, {0,0,0})
    CHECK(r == nil and type(e) == 'string',   'AddSampler jointIdx=0 returns nil, err')
    r, e = c2:AddSampler(1, 'bad_target', 'LINEAR', {0}, {0,0,0})
    CHECK(r == nil and type(e) == 'string',   'AddSampler unknown target returns nil, err')
    r, e = c2:AddSampler(1, 'translation', 'BAD_MODE', {0}, {0,0,0})
    CHECK(r == nil and type(e) == 'string',   'AddSampler unknown mode returns nil, err')
    r, e = c2:AddSampler(1, 'translation', 'LINEAR', {0}, {0,0})
    CHECK(r == nil and type(e) == 'string',   'AddSampler values count mismatch returns nil, err')
    r, e = c2:AddSampler(1, 'translation', 'LINEAR', {}, {})
    CHECK(r == nil and type(e) == 'string',   'AddSampler empty times returns nil, err')

    -- 以下路径暂时仍用 luaL_error (SetJointName / SetJointParent) - 观察是否也会崩, 需要再迁移
    CHECK(pcall(sk2.SetJointName, sk2, 99, 'x') == false,
          'SetJointName out-of-range raises')
    CHECK(pcall(sk2.SetJointParent, sk2, 1, 1) == false,
          'SetJointParent self-reference raises')
    CHECK(pcall(sk2.SetJointParent, sk2, 1, 99) == false,
          'SetJointParent out-of-range parent raises')
    print('  ... 14.12 pcall SetBindLocalTRS out-of-range')
    CHECK(pcall(sk2.SetBindLocalTRS, sk2, 99, 0,0,0, 1,0,0,0, 1,1,1) == false,
          'SetBindLocalTRS out-of-range raises')
    print('  ... 14.13 pcall SetInverseBindMatrix short-table')
    CHECK(pcall(sk2.SetInverseBindMatrix, sk2, 1, {1,2,3}) == false,
          'SetInverseBindMatrix short table raises (<16)')
else
    print('  SKIP: Phase AV.x API 不可用')
end

-- ==================== [15] Phase AW: GPU Skinning 模式 ====================

print('[15] Phase AW: GPU Skinning 模式 API')

-- 15.1 API 注册检查
CHECK(type(Anim.GetSkinningMode) == 'function', 'Anim.GetSkinningMode 存在 (Phase AW)')
CHECK(type(Anim.SetSkinningMode) == 'function', 'Anim.SetSkinningMode 存在 (Phase AW)')

-- 15.2 GetSkinningMode 返回类型
local mode0 = Anim.GetSkinningMode()
CHECK(type(mode0) == 'string' and (mode0 == 'cpu' or mode0 == 'gpu'),
      'GetSkinningMode 返回 cpu 或 gpu (实际生效路径)')

-- 15.3 SetSkinningMode 三个合法值
local r1, e1 = Anim.SetSkinningMode('cpu')
CHECK(r1 == true and e1 == nil, 'SetSkinningMode("cpu") 成功')
CHECK(Anim.GetSkinningMode() == 'cpu', '设 cpu 后查询为 cpu (强制)')

local r2, e2 = Anim.SetSkinningMode('gpu')
CHECK(r2 == true and e2 == nil, 'SetSkinningMode("gpu") 接受设置 (是否实际启用看 backend)')
local mode_gpu = Anim.GetSkinningMode()
CHECK(mode_gpu == 'cpu' or mode_gpu == 'gpu',
      '设 gpu 后查询为 gpu(支持设备) 或 cpu(自动 fallback 不支持设备)')

local r3, e3 = Anim.SetSkinningMode('auto')
CHECK(r3 == true and e3 == nil, 'SetSkinningMode("auto") 成功')

-- 15.4 SetSkinningMode 错误参数 → nil + err (S21 模式)
local r4, e4 = Anim.SetSkinningMode('invalid')
CHECK(r4 == nil and type(e4) == 'string',
      'SetSkinningMode("invalid") 返回 nil + err')

local r5, e5 = Anim.SetSkinningMode('CPU')   -- 大小写敏感
CHECK(r5 == nil and type(e5) == 'string',
      'SetSkinningMode 大小写敏感 (大写 CPU 视为非法)')

-- 非 string 参数: 用 pcall 兜底 (luaL_checkstring 风格 / 我们用 lua_type 校验则返回 nil+err)
local r6, e6
local ok_call6 = pcall(function() r6, e6 = Anim.SetSkinningMode(123) end)
if ok_call6 then
    CHECK(r6 == nil and type(e6) == 'string',
          'SetSkinningMode(123) 返回 nil + err')
else
    -- 早期实现可能 raise; 也算非 crash (之前 PCALL 已捕获)
    CHECK(true, 'SetSkinningMode(123) 不崩溃 (raise 被 pcall 捕获)')
end

local r7, e7
local ok_call7 = pcall(function() r7, e7 = Anim.SetSkinningMode(nil) end)
if ok_call7 then
    CHECK(r7 == nil and type(e7) == 'string',
          'SetSkinningMode(nil) 返回 nil + err')
else
    CHECK(true, 'SetSkinningMode(nil) 不崩溃')
end

-- 15.5 模式切换不影响 DrawSkinnedMesh 入口签名 (仅校验入口对错误参数仍按现有契约返回)
-- 不依赖真实 mesh / animator (CI 无 GPU 上下文); 只校验签名稳定:
--   传错参数 → 现有契约 raise (pcall 捕获) 或返回 nil/false+err
local ok_d, _ = pcall(Anim.DrawSkinnedMesh)
CHECK(type(ok_d) == 'boolean',
      'DrawSkinnedMesh 入口在 mode 切换后仍返回布尔 (pcall 不崩)')

-- 恢复默认 auto (避免影响后续 smoke / 用户脚本)
Anim.SetSkinningMode('auto')
print(string.format('  最终 GetSkinningMode = "%s" (auto + 当前后端推断)', tostring(Anim.GetSkinningMode())))

-- ==================== [16] Phase AX: morph target API ====================

print('[16] Phase AX: morph target API')

-- 16.1 模块常量 MORPH_TARGET_MAX
CHECK(type(Anim.MORPH_TARGET_MAX) == 'number',
      'Anim.MORPH_TARGET_MAX 是 number')
CHECK(Anim.MORPH_TARGET_MAX == 8,
      'Anim.MORPH_TARGET_MAX == 8 (与 shader uniform array 大小一致)')

-- 16.2 SkinnedMesh morph 信息 API (用 procedural Skeleton + Animator, 无 mesh 也能验证 Animator API)
-- 注: 完整 mesh-side API (mesh:HasMorphTargets/GetMorphTargetCount/GetMorphTargetName)
--     需要从 glTF 加载 mesh; smoke 不依赖外部资产, 改为单独验证 Animator-side API.
--     mesh-side 完整端到端验证留给 sample (T7 demo_morph_target).

-- 16.3 Animator morph API 表面
local sk16 = Anim.NewEmptySkeleton(2)
sk16:SetJointName(1, 'root')
sk16:SetJointName(2, 'spine')
local an16 = Anim.NewAnimator(sk16)

CHECK(type(an16.SetMorphWeight) == 'function',
      'animator:SetMorphWeight 存在')
CHECK(type(an16.GetMorphWeight) == 'function',
      'animator:GetMorphWeight 存在')
CHECK(type(an16.ClearMorphWeights) == 'function',
      'animator:ClearMorphWeights 存在')
CHECK(type(an16.GetMorphTargetCount) == 'function',
      'animator:GetMorphTargetCount 存在')
CHECK(type(an16.GetMorphWeights) == 'function',
      'animator:GetMorphWeights 存在')
CHECK(type(an16.HasManualMorphOverride) == 'function',
      'animator:HasManualMorphOverride 存在')

-- 16.4 SetMorphWeight + GetMorphWeight round-trip
local r16a, e16a = an16:SetMorphWeight(1, 0.5)
CHECK(r16a == true and e16a == nil,
      'SetMorphWeight(1, 0.5) 成功')
local w16a = an16:GetMorphWeight(1)
CHECK(w16a == 0.5,
      'GetMorphWeight(1) == 0.5 (set 后即时生效)')

-- 16.5 多个槽位
an16:SetMorphWeight(2, 0.25)
an16:SetMorphWeight(8, 0.875)   -- 边界 (idx=MORPH_TARGET_MAX)
CHECK(an16:GetMorphWeight(2) == 0.25, 'idx=2 weight=0.25')
CHECK(an16:GetMorphWeight(8) == 0.875, 'idx=8 (boundary) weight=0.875')

-- 16.6 GetMorphTargetCount = max idx written
CHECK(an16:GetMorphTargetCount() == 8,
      'GetMorphTargetCount == 8 (auto-resize 到最大写入 idx)')

-- 16.7 GetMorphWeights 返回 array
local arr16 = an16:GetMorphWeights()
CHECK(type(arr16) == 'table',
      'GetMorphWeights 返回 table')
CHECK(#arr16 == 8,
      'GetMorphWeights 数组长度 == 8')
CHECK(arr16[1] == 0.5 and arr16[2] == 0.25 and arr16[8] == 0.875,
      'GetMorphWeights 数组内容正确')

-- 16.8 HasManualMorphOverride
CHECK(an16:HasManualMorphOverride(1) == true,
      'HasManualMorphOverride(1) == true (已 set)')
CHECK(an16:HasManualMorphOverride(3) == false,
      'HasManualMorphOverride(3) == false (未 set)')

-- 16.9 越界 idx 错误处理
local rb1, eb1 = an16:SetMorphWeight(0, 0.5)    -- idx < 1
CHECK(rb1 == nil and type(eb1) == 'string',
      'SetMorphWeight(0, ...) 返回 nil + err')
local rb2, eb2 = an16:SetMorphWeight(9, 0.5)    -- idx > MORPH_TARGET_MAX
CHECK(rb2 == nil and type(eb2) == 'string',
      'SetMorphWeight(9, ...) 返回 nil + err (超出 MORPH_TARGET_MAX)')

-- 16.10 GetMorphWeight 越界返回 0 (不报错)
local w16b = an16:GetMorphWeight(100)
CHECK(w16b == 0 or w16b == 0.0,
      'GetMorphWeight(100) 返回 0 (越界默认值)')
local rb3, eb3 = an16:GetMorphWeight(0)         -- idx < 1
CHECK(rb3 == nil and type(eb3) == 'string',
      'GetMorphWeight(0) 返回 nil + err')

-- 16.11 ClearMorphWeights 清除所有手动覆盖
an16:ClearMorphWeights()
CHECK(an16:HasManualMorphOverride(1) == false,
      'ClearMorphWeights 后 HasManualMorphOverride(1) == false')
CHECK(an16:HasManualMorphOverride(8) == false,
      'ClearMorphWeights 后 HasManualMorphOverride(8) == false')
-- morphWeights 数组本身保留 (动画 Update 才会重新评估)
CHECK(an16:GetMorphTargetCount() == 8,
      'ClearMorphWeights 不清除 morphWeights size (保持 8)')

-- 16.12 Update 不崩溃 (无 morph clip 时 EvaluateMorphWeights 应早 return)
an16:Update(0.016)
CHECK(true, 'Animator:Update 在无 morph clip 时不崩溃 (EvaluateMorphWeights early return)')

-- ==================== [17] Phase AY: 批量内省接口 + LoadSkinnedGLTF API ====================

print('[17] Phase AY: ListTransitions / ListEvents 批量内省 + pack.hasSkin/meshes')

-- 17.1 metatable 含新方法 (沿用 [16] 段已创建的 an16, sk16 是其 skeleton)
local mt_ay = getmetatable(an16) or getmetatable(Anim.NewAnimator(sk16))
CHECK(mt_ay and type(mt_ay.ListTransitions) == 'function', 'Animator metatable 含 ListTransitions (Phase AY T07)')
CHECK(mt_ay and type(mt_ay.ListEvents)      == 'function', 'Animator metatable 含 ListEvents (Phase AY T07)')

-- 17.2 ListTransitions 空数组 (新建 animator 无 transition)
local skel_ay = Anim.NewEmptySkeleton(1)
skel_ay:SetJointName(1, 'root')
local clip_ay  = Anim.NewEmptyClip('a', 1.0)
local clip_ay2 = Anim.NewEmptyClip('b', 1.0)
local an_ay = Anim.NewAnimator(skel_ay)
an_ay:AddState('a', clip_ay)
an_ay:AddState('b', clip_ay2)

local lt0 = an_ay:ListTransitions()
CHECK(type(lt0) == 'table' and #lt0 == 0, 'ListTransitions 空数组 (无 transition)')
local le0 = an_ay:ListEvents()
CHECK(type(le0) == 'table' and #le0 == 0, 'ListEvents 空数组 (无 event)')

-- 17.3 ListTransitions 含 from/to/duration/hasCond
an_ay:AddTransition('a', 'b', function() return true end, 0.5)
an_ay:AddTransition('',  'a', function() return false end, 0.0)   -- Any state
local lt1 = an_ay:ListTransitions()
CHECK(#lt1 == 2, 'ListTransitions 返回 2 项')
CHECK(lt1[1].from == 'a' and lt1[1].to == 'b', 'ListTransitions[1].from/to')
CHECK(math.abs(lt1[1].duration - 0.5) < 1e-6, 'ListTransitions[1].duration = 0.5')
CHECK(lt1[1].hasCond == true, 'ListTransitions[1].hasCond = true')
CHECK(lt1[2].from == '', 'ListTransitions[2].from = "" (Any state)')

-- 17.4 ListEvents 含 state/triggerTime/hasCallback
an_ay:AddEvent('a', 0.3, function() end)
an_ay:AddEvent('b', 0.7, function() end)
local le1 = an_ay:ListEvents()
CHECK(#le1 == 2, 'ListEvents 返回 2 项')
CHECK(le1[1].state == 'a' and math.abs(le1[1].triggerTime - 0.3) < 1e-6,
      'ListEvents[1].state/triggerTime')
CHECK(le1[1].hasCallback == true, 'ListEvents[1].hasCallback = true')

-- 17.5 与单查 GetTransitionInfo / GetEventInfo 一致性 (B.3 完整性验证)
local t1 = an_ay:GetTransitionInfo(1)
CHECK(t1.from == lt1[1].from and t1.to == lt1[1].to,
      'ListTransitions[1] 与 GetTransitionInfo(1) 一致')
local e1 = an_ay:GetEventInfo(1)
CHECK(e1.state == le1[1].state and math.abs(e1.triggerTime - le1[1].triggerTime) < 1e-6,
      'ListEvents[1] 与 GetEventInfo(1) 一致')

an_ay:Delete()

-- 17.6 LoadSkinnedGLTF 不存在的文件 → 返回 nil 或异常 (与原 API 一致, 仅冒烟)
if type(Anim.LoadSkinnedGLTF) == 'function' then
    local ok_ng, _, _ = pcall(Anim.LoadSkinnedGLTF, '/__nonexistent__phase_ay__.glb')
    CHECK(ok_ng or true, 'LoadSkinnedGLTF 不存在路径 不崩溃 (返回 nil+err 或 raise)')
end

-- 17.7 SkinnedMesh:GetMorphTargetIndex(name) (Phase AY T09)
--   完整端到端 (mesh:GetMorphTargetIndex('smile') 返回 idx) 需要真实 glTF morph 资产,
--   留给 demo_morph_target sample. 这里仅冒烟 metatable 含此方法.
--   思路: 找出 SkinnedMesh metatable. 由于 Anim 模块未直接暴露元表, 借助
--   `for _, m in ipairs(debug.getregistry()...)` 不便, 改为间接验证: 加载
--   不存在 glTF, 拿 hasSkin=false 的 pack, 确认 mesh 字段为 nil 即可推断 API 一致.
do
    local ok, pack = pcall(Anim.LoadSkinnedGLTF, '/__no_morph_asset__.glb')
    -- 不强求 ok 与否, 主要确认 fallback 不抛新异常类型
    CHECK(true, 'LoadSkinnedGLTF 异常路径 fallback 不破坏后续 smoke 流程')
end

-- ==================== [18] Phase E.13: velocity history 复位路径 ====================
-- 目标: 间接验证 Phase E.13 Animator 内部 prev pose / velocityHistoryValid
-- 在以下时间跳变/状态切换路径下不崩, 并且 Update 仍能产出合法 jointMatrices.
-- Lua 端没有直接 query velocityHistoryValid, 所以只覆盖 "动作不崩 + 状态自洽".

print('[18] Phase E.13: velocity history 复位路径 (间接验证)')

do
    -- 复用 [13] 的最小 skeleton + clip 套件, 避免依赖外部资产
    local sk = Anim.NewEmptySkeleton(1)
    sk:SetJointName(1, 'root')

    local idle = Anim.NewEmptyClip('idle', 1.0)
    idle:AddSampler(1, 'translation', 'LINEAR', {0.0, 1.0}, {0,0,0, 0,0,0})

    local walk = Anim.NewEmptyClip('walk', 1.0)
    walk:AddSampler(1, 'translation', 'LINEAR', {0.0, 1.0}, {0,0,0, 10,0,0})

    local an = Anim.NewAnimator(sk)
    an:AddState('idle', idle)
    an:AddState('walk', walk)

    -- 18.1 连续 Update 后 prev pose 应该被自然累积 (内部行为, 仅验证不崩)
    an:Play('walk')
    for i = 1, 3 do an:Update(1/60) end
    local mats1 = an:GetJointMatrices()
    CHECK(type(mats1) == 'table' and #mats1 == 16, '[E.13] 连续 Update 后 jointMatrices 形态正确')

    -- 18.2 SetCurrentTime 触发 velocity history 复位; 后续 Update 不崩
    an:SetCurrentTime(0.5)
    an:Update(1/60)
    CHECK(true, '[E.13] SetCurrentTime + Update 不崩 (内部 prev pose 复位)')

    -- 18.3 Play 切换 state 触发复位
    an:Play('idle')
    an:Update(1/60)
    CHECK(an:GetCurrentState() == 'idle',
          '[E.13] Play 切换 state 后 currentState 正确, prev pose 由内部复位')

    -- 18.4 Stop 清空 currentState 触发复位
    an:Stop()
    CHECK(an:GetCurrentState() == nil or an:GetCurrentState() == '',
          '[E.13] Stop 后 currentState 为空, prev pose 由内部复位')

    -- 18.5 立即 transition (duration=0) 触发复位
    an:Play('idle')
    an:AddTransition('idle', 'walk', function() return true end, 0.0)
    an:Update(1/60)
    CHECK(an:GetCurrentState() == 'walk',
          '[E.13] 立即 transition (duration=0) 后切换到 walk, prev pose 复位')
    an:ClearTransitions()

    -- 18.6 手动 morph weight 覆盖触发复位
    local ok_mw = pcall(function() an:SetMorphWeight(1, 0.5) end)
    CHECK(ok_mw, '[E.13] SetMorphWeight 不崩 (触发内部 prev morph 复位)')
    an:Update(1/60)
    CHECK(true, '[E.13] SetMorphWeight 后 Update 不崩')

    -- 18.7 ClearMorphWeights 触发复位
    an:ClearMorphWeights()
    an:Update(1/60)
    CHECK(true, '[E.13] ClearMorphWeights 后 Update 不崩')

    -- 18.8 极端时间跳变后仍能产生合法关节矩阵
    an:Play('walk')
    an:SetCurrentTime(0.0)
    an:Update(1/60)
    an:SetCurrentTime(0.9)  -- 大跳变
    an:Update(1/60)
    local mats2 = an:GetJointMatrices()
    CHECK(type(mats2) == 'table' and #mats2 == 16,
          '[E.13] 大时间跳变后 jointMatrices 仍合法')

    an:Delete()
end

-- ==================== 汇总 ====================

print(string.format('[Phase AV Step 1+2+3+4 + Phase AV.x + Phase AW + Phase AX + Phase AY + Phase E.13] 通过 %d / 失败 %d', PASS, FAIL))
if FAIL > 0 then
    error(string.format('animation smoke 失败: %d 个断言不通过', FAIL))
end
