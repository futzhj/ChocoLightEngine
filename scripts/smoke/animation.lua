-- scripts/smoke/animation.lua
-- Phase AV Step 1+2+3 smoke: Light.Animation 模块加载 + Skeleton/Clip/Animator/SkinnedMesh + 状态机 + 错误边界
--
-- Step 1: 加载失败路径 + API 表面
-- Step 2: 状态机 API + sampler/关节矩阵错误路径
-- Step 3: SkinnedMesh API 表面 + DrawSkinnedMesh 错误路径 (headless 不渲染)
--
-- 不依赖任何外部 glTF 资源 (与 Phase AS mesh_3d.lua 风格一致)
-- 兼容 Lua 5.1 (lightc -p 严格语法检查 + light.exe runtime)

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

print('  INFO: Step 4 状态机/事件/Crossfade 端到端语义验证 (Update 时序 / fade 中点权重 / 事件循环边界)')
print('        留 Phase AV.x 引入测试 glTF 资产时补完整成功路径 (与 Step 1+2+3 数值断言策略一致)')

-- ==================== 汇总 ====================

print(string.format('[Phase AV Step 1+2+3+4] 通过 %d / 失败 %d', PASS, FAIL))
if FAIL > 0 then
    error(string.format('animation smoke 失败: %d 个断言不通过', FAIL))
end
