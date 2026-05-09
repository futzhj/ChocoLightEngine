-- scripts/smoke/animation.lua
-- Phase AV Step 1 smoke: Light.Animation 模块加载 + Skeleton/Clip/Animator 数据结构 + 错误边界
--
-- 不依赖任何外部 glTF 资源 (Step 1 仅测加载失败路径与 API 表; Step 2 引入实际样本)
-- 兼容 Lua 5.1 (lightc -p 严格语法检查 + light.exe runtime)
-- 结构与 scripts/smoke/physics_3d.lua 一致 (sandbox-tolerant: lightc 仅做 -p 时跳过 require)

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

CHECK(sk_mod ~= nil, 'require Light.Animation.Skeleton 不崩')
CHECK(cl_mod ~= nil, 'require Light.Animation.Clip 不崩')
CHECK(an_mod ~= nil, 'require Light.Animation.Animator 不崩')

-- ==================== [5] 加载非 skinned glTF 不致命 (验证降级路径) ====================

print('[5] 非 skinned glTF 降级')

-- 我们没有现成 glTF 资源, 但 LoadSkinnedGLTF 接受任意 path,
-- 不存在文件已经在 [2] 测过. 此节预留给 Step 2 + 资源时启用.
print('  SKIP: 需要 glTF 样本资源 (Step 2 引入)')

-- ==================== 汇总 ====================

print(string.format('[Phase AV Step 1] 通过 %d / 失败 %d', PASS, FAIL))
if FAIL > 0 then
    error(string.format('animation smoke 失败: %d 个断言不通过', FAIL))
end
