/**
 * @file light_ecs.cpp
 * @brief Light.ECS — 纯 Lua Entity-Component-System
 *
 * Lua API (基础, Phase 2):
 *   world = Light(Light.ECS.World):New()
 *   world:RegisterComponent(name, defaults [, opts])   -- opts.networked (Phase C)
 *   entity = world:CreateEntity()
 *   entity:Add(compName, data)
 *   entity:Remove(compName)
 *   entity:Set(compName, data)            -- 修改已有 component (Phase C)
 *   entity:Get(compName) → table
 *   entity:Has(compName) → bool
 *   world:AddSystem(name, requiredComponents, updateFunc)
 *   world:Update(dt)
 *   world:Query(comp1, comp2, ...) → entities
 *   world:DestroyEntity(entity)
 *
 * 网络同步扩展 (Phase C, server-authoritative):
 *   world:NetworkSync(room)               -- 绑定 Room.Host (传 nil 解绑)
 *   networked component 的修改自动通过 room:PatchState 广播
 *
 * 客户端 mirror (Phase C, 由 C-T4 提供):
 *   mirror = Light.ECS.MirrorFromRoom(room)   -- 自动 OnState reconcile
 *
 * 内置渲染 component + 渲染 system (Phase D):
 *   world.new() 自动注册 6 个内置渲染 component:
 *     Transform2D / Sprite / Camera2D / Transform3D / MeshRenderer / Camera3D
 *   world:Render()             -- 跑 2D camera setup → sprite (z-sort) → 3D camera → mesh
 *   world:MarkRenderNetworked() -- 把内置渲染 component 重标为 networked=true (需 NetworkSync 前调)
 */

#include "light.h"

// 内嵌 Lua 脚本: 轻量 ECS 实现
static const char g_ecsScript[] = R"LUA(
-- ==================== ECS World ====================
local ECSWorld = {}
ECSWorld.__index = ECSWorld

function ECSWorld.new()
    local w = setmetatable({}, ECSWorld)
    w._components = {}       -- 注册的组件定义 {name → defaults}
    w._entities = {}         -- 活跃实体 {entity}
    w._systems = {}          -- 系统 {name, required, func}
    w._nextId = 1
    -- Phase C: 网络化同步状态
    w._networked_comps = {}  -- {[name] = true} 标记同步的 component
    w._dirty_entities = {}   -- {[id] = true} 待同步 entity ID
    w._destroyed_ids = {}    -- {[id] = true} 待广播销毁
    w._sync_room = nil       -- 绑定的 Room.Host (Phase C NetworkSync)
    w._has_changes = false   -- 任何 networked 字段变化的总开关
    -- Phase C.x.1: per-component dirty 跟踪 + full-resync 标志
    w._dirty_comps = {}      -- {[id] = {[comp_name] = true}} 细粒度 dirty (新/改)
    w._removed_comps = {}    -- {[id] = {[comp_name] = true}} Remove 触发的删除
    w._needs_full_resync = false  -- OnJoin 触发, 下次 sync 把所有 entity 所有 networked comp 标 dirty
    -- Phase D: 内置渲染 component 自动注册 (具体定义在 Phase 2 raw string)
    w._builtin_render_comps = {}
    if w._RegisterBuiltinRenderComponents then
        w:_RegisterBuiltinRenderComponents()
    end
    return w
end

-- 注册组件类型
-- @param name string  component 名称
-- @param defaults table?  默认字段
-- @param opts table?  {networked=true} 标记为网络化 (Phase C)
function ECSWorld:RegisterComponent(name, defaults, opts)
    self._components[name] = defaults or {}
    if opts ~= nil then
        if type(opts) ~= 'table' then
            error("RegisterComponent: opts must be a table or nil, got "..type(opts))
        end
        if opts.networked then
            self._networked_comps[name] = true
        end
    end
end

-- 深拷贝表
local function deepcopy(orig)
    if type(orig) ~= 'table' then return orig end
    local copy = {}
    for k, v in pairs(orig) do copy[k] = deepcopy(v) end
    return copy
end

-- Phase C.x.1: 标记 component dirty (新/改)
local function _markDirtyComp(world, id, compName)
    local set = world._dirty_comps[id]
    if not set then
        set = {}
        world._dirty_comps[id] = set
    end
    set[compName] = true
    world._dirty_entities[id] = true    -- 保留旧字段兼容
    world._has_changes = true
    -- 取消 removed 标记 (Remove 后又 Add 的场景)
    local rmSet = world._removed_comps[id]
    if rmSet then rmSet[compName] = nil end
end

-- Phase C.x.1: 标记 component 被 Remove
local function _markRemovedComp(world, id, compName)
    local set = world._removed_comps[id]
    if not set then
        set = {}
        world._removed_comps[id] = set
    end
    set[compName] = true
    -- 同时清 dirty (避免既 dirty 又 removed)
    local dirtySet = world._dirty_comps[id]
    if dirtySet then dirtySet[compName] = nil end
    world._has_changes = true
end

-- 创建实体
function ECSWorld:CreateEntity()
    local e = { _id = self._nextId, _comps = {} }
    self._nextId = self._nextId + 1

    function e:Add(compName, data)
        local world = self._world
        local defaults = world._components[compName]
        local comp = defaults and deepcopy(defaults) or {}
        if data then
            for k, v in pairs(data) do comp[k] = v end
        end
        self._comps[compName] = comp
        -- 创建便捷访问: entity.Position 等
        self[compName] = comp
        -- Phase C.x.1: 若 networked, 标记细粒度 dirty
        if world._networked_comps[compName] then
            _markDirtyComp(world, self._id, compName)
        end
        -- Phase D.x.5.1: sprite 相关 component 增减触发 sprite list invalidate
        if compName == 'Sprite' or compName == 'Transform2D' then
            world._sprite_list_dirty = true
        end
        return self
    end

    function e:Remove(compName)
        local world = self._world
        -- Phase C.x.1: 若 networked 且存在, 标记为被移除 (wire 中会发 "__removed__")
        if world._networked_comps[compName] and self._comps[compName] then
            _markRemovedComp(world, self._id, compName)
        end
        self._comps[compName] = nil
        self[compName] = nil
        -- Phase D.x.5.1: sprite 相关 component 移除触发 sprite list invalidate
        if compName == 'Sprite' or compName == 'Transform2D' then
            world._sprite_list_dirty = true
        end
        return self
    end

    -- Phase C: 显式修改 component 数据 (要求已 Add)
    -- 浅 merge data 到现有 component table, 保持引用稳定 (用户可缓存 entity.Position)
    function e:Set(compName, data)
        local existing = self._comps[compName]
        if not existing then
            error("Set: component '"..tostring(compName).."' not added on entity "..tostring(self._id))
        end
        if data then
            for k, v in pairs(data) do existing[k] = v end
        end
        if self._world._networked_comps[compName] then
            _markDirtyComp(self._world, self._id, compName)
        end
        -- Phase D.x.5.1: Set 改 sprite 相关字段时自动 invalidate sprite list
        if compName == 'Sprite' or compName == 'Transform2D' then
            self._world._sprite_list_dirty = true
        end
        return self
    end

    function e:Get(compName)
        return self._comps[compName]
    end

    function e:Has(compName)
        return self._comps[compName] ~= nil
    end

    e._world = self
    table.insert(self._entities, e)
    return e
end

-- 销毁实体
function ECSWorld:DestroyEntity(entity)
    for i, e in ipairs(self._entities) do
        if e._id == entity._id then
            -- Phase C: 标记销毁, 下次 sync 通过 entities 全量替换自然清除
            self._destroyed_ids[entity._id] = true
            self._has_changes = true
            -- Phase D.x.5.1: 若被销毁 entity 有 sprite, invalidate cache
            if e._comps.Sprite or e._comps.Transform2D then
                self._sprite_list_dirty = true
            end
            table.remove(self._entities, i)
            return
        end
    end
end

-- 添加系统
function ECSWorld:AddSystem(name, required, func)
    table.insert(self._systems, {
        name = name,
        required = required,
        func = func,
    })
end

-- 查询匹配组件的实体
function ECSWorld:Query(...)
    local comps = {...}
    local result = {}
    for _, e in ipairs(self._entities) do
        local match = true
        for _, c in ipairs(comps) do
            if not e._comps[c] then match = false; break end
        end
        if match then table.insert(result, e) end
    end
    return result
end

-- 更新所有系统
function ECSWorld:Update(dt)
    local _unpack = table.unpack or unpack  -- Lua 5.1: 全局 unpack; Lua 5.2+: table.unpack
    for _, sys in ipairs(self._systems) do
        local entities = self:Query(_unpack(sys.required))
        sys.func(entities, dt)
    end
    -- Phase D.x.4: 内置动画系统 (auto animator:Update + AnimationState 字段桥接)
    if self._AnimationSystem then self:_AnimationSystem(dt) end
    -- Phase C: 末尾自动同步 (若有 networked 变化且已绑 room)
    if self._sync_room and self._has_changes then
        self:_SyncToRoom()
    end
end

-- 实体总数
function ECSWorld:GetEntityCount()
    return #self._entities
end

-- ==================== Phase C: 网络同步 ====================

-- 绑定/解绑 Room.Host. 调用后 networked component 修改自动同步.
-- @param room Room.Host 或 nil (解绑)
function ECSWorld:NetworkSync(room)
    self._sync_room = room
end

-- 私有: 把单个 entity 的 networked component 打成 wire 表 (浅拷贝)
-- 返回 row 表; 若该 entity 没有任何 networked component, 返回 nil
function ECSWorld:_BuildEntityState(entity)
    local row = nil
    for compName, _ in pairs(self._networked_comps) do
        local comp = entity._comps[compName]
        if comp then
            local copy = {}
            for k, v in pairs(comp) do copy[k] = v end
            row = row or {}
            row[compName] = copy
        end
    end
    return row
end
)LUA" R"LUA(
-- Phase C.x.1: 按 id 查找 entity (O(n), MVP 够用)
function ECSWorld:_FindById(id)
    for _, e in ipairs(self._entities) do
        if e._id == id then return e end
    end
    return nil
end

-- Phase C.x.1: 用户在 room:OnJoin 回调里调用,
-- 下帧 _SyncToRoom 会把所有 networked entity 的所有 networked comp 标 dirty 广播,
-- 让新 peer 能拿到完整快照.
function ECSWorld:MarkFullResync()
    self._needs_full_resync = true
    self._has_changes = true
end

-- Phase C.x.1: 增量同步 — 走 room:Broadcast('ecs_delta', {set, del}) 而非 PatchState.
-- wire 格式: {set = {[id] = {[comp] = data 或 "__removed__"}}, del = [id1, id2]}
-- set 中不出现的 component 表示"未变化", mirror 不删除.
function ECSWorld:_SyncToRoom()
    -- 0) full resync: 把全量 entity 的全量 networked comp 标 dirty (OnJoin 时触发)
    if self._needs_full_resync then
        for _, e in ipairs(self._entities) do
            for compName, _ in pairs(self._networked_comps) do
                if e._comps[compName] then
                    _markDirtyComp(self, e._id, compName)
                end
            end
        end
        self._needs_full_resync = false
    end

    -- 1) 构造 set (dirty 新/改 + removed 标记"__removed__")
    local set_patch = nil
    for id, dirtySet in pairs(self._dirty_comps) do
        local entity = self:_FindById(id)
        if entity then
            local row = {}
            for compName, _ in pairs(dirtySet) do
                local comp = entity._comps[compName]
                if comp then
                    local copy = {}
                    for k, v in pairs(comp) do copy[k] = v end
                    row[compName] = copy
                end
            end
            -- 合并 removed (同一 entity 可能既改了 A 又 Remove 了 B)
            local rmSet = self._removed_comps[id]
            if rmSet then
                for compName, _ in pairs(rmSet) do
                    row[compName] = "__removed__"
                end
            end
            if next(row) then
                set_patch = set_patch or {}
                set_patch[tostring(id)] = row
            end
        end
    end
    -- 补上 removed 但 没走过 dirty_comps 的 id (仅 Remove 的场景)
    for id, rmSet in pairs(self._removed_comps) do
        if not self._dirty_comps[id] then
            local row = {}
            for compName, _ in pairs(rmSet) do
                row[compName] = "__removed__"
            end
            if next(row) then
                set_patch = set_patch or {}
                set_patch[tostring(id)] = row
            end
        end
    end

    -- 2) 构造 del (销毁的 entity id)
    local del_patch = nil
    for id, _ in pairs(self._destroyed_ids) do
        del_patch = del_patch or {}
        table.insert(del_patch, tostring(id))
    end

    -- 3) 广播 (仅当有变化)
    if set_patch or del_patch then
        self._sync_room:Broadcast('ecs_delta', {
            set = set_patch,
            del = del_patch,
        })
    end

    -- 4) 清空跟踪
    self._dirty_comps = {}
    self._removed_comps = {}
    self._destroyed_ids = {}
    self._dirty_entities = {}
    self._has_changes = false
end

-- ==================== Phase C: Client Mirror ====================

-- Phase C.x.1: 应用一次 server 推来的 delta (Broadcast('ecs_delta') 的 payload)
-- wire: {set = {[id] = {[comp] = data 或 "__removed__"}}, del = [id1, id2]}
function ECSWorld:_ApplyDelta(delta)
    if not self._is_mirror then
        error("_ApplyDelta only valid on mirror world")
    end
    local set = delta and delta.set or {}
    local del = delta and delta.del or {}

    -- 1) 应用 set (新增/更新/component 删除)
    for keyStr, row in pairs(set) do
        local id = tonumber(keyStr) or keyStr   -- JSON key 是 string, 转回 number
        local e = self._mirror_by_id[id]
        if not e then
            e = self:_CreateMirrorEntity(id)
            self._mirror_by_id[id] = e
        end

        for compName, compData in pairs(row) do
            if compData == "__removed__" then
                -- 显式删除该 component
                e._comps[compName] = nil
                e[compName] = nil
            else
                local target = e._comps[compName]
                if target then
                    -- 浅 merge 保持引用稳定 (R5); delta 语义: compData 是该 comp 完整新值
                    for k, v in pairs(compData) do target[k] = v end
                    -- 删除 incoming 没有的字段 (跟 v1 _ApplyState 一致)
                    for k, _ in pairs(target) do
                        if compData[k] == nil then target[k] = nil end
                    end
                else
                    -- 新增 component (浅拷贝)
                    local copy = {}
                    for k, v in pairs(compData) do copy[k] = v end
                    e._comps[compName] = copy
                    e[compName] = copy
                end
            end
        end
    end

    -- 2) 应用 del (销毁 entity)
    for _, keyStr in ipairs(del) do
        local id = tonumber(keyStr) or keyStr
        local e = self._mirror_by_id[id]
        if e then
            self._mirror_by_id[id] = nil
            for i, ent in ipairs(self._entities) do
                if ent._id == e._id then
                    table.remove(self._entities, i)
                    break
                end
            end
        end
    end
end

-- 私有: 用指定 ID 创建 mirror entity (复用 CreateEntity 但绕过 _nextId 自增)
-- 临时改 _nextId 让 CreateEntity 用我们的 ID, 之后还原
function ECSWorld:_CreateMirrorEntity(id)
    local saved = self._nextId
    self._nextId = id
    local e = self:CreateEntity()
    self._nextId = saved   -- 还原 (CreateEntity 内部已 ++, 但我们直接覆盖)
    return e
end

-- 私有: 应用一次 server 推来的 state, 增量更新 mirror world
-- 浅覆盖现有 component table 保持引用稳定, 删除 incoming 没有的 component / entity
function ECSWorld:_ApplyState(state)
    if not self._is_mirror then
        error("_ApplyState only valid on mirror world")
    end
    local incoming = state and state.entities or {}
    local seen = {}

    -- 1. 新增 / 更新
    for keyStr, row in pairs(incoming) do
        local id = tonumber(keyStr) or keyStr   -- JSON key 是 string, 转回 number
        seen[id] = true

        local e = self._mirror_by_id[id]
        if not e then
            e = self:_CreateMirrorEntity(id)
            self._mirror_by_id[id] = e
        end

        -- 同步每个 incoming component (浅覆盖, 引用稳定)
        for compName, compData in pairs(row) do
            local target = e._comps[compName]
            if target then
                -- 已有: 浅覆盖 + 删除 incoming 没有的字段
                for k, v in pairs(compData) do target[k] = v end
                for k, _ in pairs(target) do
                    if compData[k] == nil then target[k] = nil end
                end
            else
                -- 新增 component (浅拷贝 incoming)
                local copy = {}
                for k, v in pairs(compData) do copy[k] = v end
                e._comps[compName] = copy
                e[compName] = copy
            end
        end

        -- 删除本地有但 incoming 没有的 component
        local to_unset = {}
        for compName, _ in pairs(e._comps) do
            if not row[compName] then
                table.insert(to_unset, compName)
            end
        end
        for _, compName in ipairs(to_unset) do
            e._comps[compName] = nil
            e[compName] = nil
        end
    end

    -- 2. 销毁: 本地有但 incoming 没有的 entity
    local to_remove = {}
    for id, _ in pairs(self._mirror_by_id) do
        if not seen[id] then
            table.insert(to_remove, id)
        end
    end
    for _, id in ipairs(to_remove) do
        local e = self._mirror_by_id[id]
        self._mirror_by_id[id] = nil
        -- 从 _entities 数组中移除 (不调 DestroyEntity 避免触发 dirty)
        for i, ent in ipairs(self._entities) do
            if ent._id == e._id then
                table.remove(self._entities, i)
                break
            end
        end
    end
end

-- ==================== Module: Light.ECS ====================

-- Phase C.x.1: 创建 client mirror world, 自动 hook room.OnEvent('ecs_delta', ...)
-- 不再依赖 OnState; server 应用 world:NetworkSync(room) + world:MarkFullResync()
-- @param room Room.Client (必须有 :OnEvent 方法)
-- @return mirror ECSWorld 实例 (可调 :Query, 但不应调 :CreateEntity / Set 等)
local function MirrorFromRoom(room)
    local mirror = ECSWorld.new()
    mirror._is_mirror = true
    mirror._mirror_by_id = {}    -- {[id] = entity}, O(1) 查找
    mirror._source_room = room

    -- Phase C.x.1: hook OnEvent('ecs_delta') 替代 Phase C v1 的 OnState
    if room and type(room.OnEvent) == 'function' then
        room:OnEvent(function(name, args)
            if name == 'ecs_delta' and type(args) == 'table' then
                mirror:_ApplyDelta(args)
            end
        end)
    end

    return mirror
end
)LUA" R"LUA(
-- ==================== Phase D: 内置渲染 component + 渲染调度 ====================

-- 内置 6 个渲染 component 的默认值表
-- 用户若已注册同名 component, 引擎跳过该项以尊重用户优先
function ECSWorld:_RegisterBuiltinRenderComponents()
    local builtins = {
        -- Phase D.x.1: Transform2D/3D 加 parent 字段 (entity 引用) 支持层级
        {name='Transform2D',  defaults={x=0, y=0, z=0, rot=0, sx=1, sy=1, ox=0, oy=0, parent=nil}},
        {name='Sprite',       defaults={color={r=1,g=1,b=1,a=1}, visible=true,
                                         anchor={ax=0, ay=0}, flipX=false, flipY=false}},
        -- Phase D.x.6: SpriteBatch (共享 image + color + transform 的批量 quad 容器)
        -- quads = {{x=, y=, qx=, qy=, qw=, qh=}, ...}; 每个 quad 1 个 DrawQuad 调用
        -- 节省: 全局共享 Push/Pop + SetColor (vs N 次)
        {name='SpriteBatch',  defaults={image=nil, quads={}, color={r=1,g=1,b=1,a=1}, visible=true}},
        {name='Camera2D',     defaults={active=true, zoom=1.0, viewportW=0, viewportH=0}},
        {name='Transform3D',  defaults={x=0, y=0, z=0, rx=0, ry=0, rz=0, sx=1, sy=1, sz=1, parent=nil}},
        {name='MeshRenderer', defaults={visible=true}},
        {name='Camera3D',     defaults={active=true, fovY=60, aspect=1.333, nearZ=0.1, farZ=1000,
                                         targetX=0, targetY=0, targetZ=0,
                                         upX=0, upY=1, upZ=0}},
        -- Phase D.x.4: 骨骼动画 (mesh/animator 是 userdata, 不可序列化; 故不标 networked)
        {name='SkinnedMeshRenderer', defaults={visible=true}},
        -- Phase D.x.4: 动画状态 (state/speed/paused/params/morphWeights 可同步)
        {name='AnimationState', defaults={state='', speed=1.0, paused=false, time=0,
                                           crossfade=0, looping=true, params={}, morphWeights={}}},
    }
    for _, c in ipairs(builtins) do
        if not self._components[c.name] then
            self:RegisterComponent(c.name, c.defaults, {networked = false})
            self._builtin_render_comps[c.name] = true
        end
    end
    -- Phase D.x.4: 标记不可序列化的内置 component (含 userdata 字段)
    self._builtin_no_network = self._builtin_no_network or {}
    self._builtin_no_network['SkinnedMeshRenderer'] = true
end

-- 把内置渲染 component 重标 networked=true. 必须在 NetworkSync 前调用.
-- Phase D.x.4: 自动跳过 _builtin_no_network 标记的 component (含 userdata, 不能 JSON 序列化)
function ECSWorld:MarkRenderNetworked()
    local skip = self._builtin_no_network or {}
    for name, _ in pairs(self._builtin_render_comps) do
        if self._components[name] and not skip[name] then
            self._networked_comps[name] = true
        end
    end
end

-- 找首个 active=true 的相机 entity (同时拥有指定 cam comp 和 tf comp)
-- Phase D.x.5: self-validating cache - 缓存命中 O(1), 失效时重扫 O(n) + 更新缓存
-- 缓存失效条件 (自动检测, 无需手动调用):
--   1. 缓存的 entity 已销毁 (_comps 字段被清空)
--   2. 缓存的 entity active 字段变 false
function ECSWorld:_FindActiveCamera(camComp, tfComp)
    self._cam_cache = self._cam_cache or {}
    local cacheKey = camComp .. '|' .. tfComp
    local cached = self._cam_cache[cacheKey]
    if cached then
        local c = cached._comps[camComp]
        local t = cached._comps[tfComp]
        if c and t and c.active then return cached end
        -- 缓存失效, fall through 重扫
    end
    -- 全表扫描 (amortized O(1) — 仅在相机切换 active 时触发)
    for _, e in ipairs(self._entities) do
        local c = e._comps[camComp]
        local t = e._comps[tfComp]
        if c and t and c.active then
            self._cam_cache[cacheKey] = e
            return e
        end
    end
    self._cam_cache[cacheKey] = nil
    return nil
end

-- 收集所有可见 sprite, 按 Transform2D.z 升序 (画家算法)
-- Phase D.x.5.1: 加 cache. 失效条件:
--   1. entity 数量变化 (CreateEntity/DestroyEntity 自动触发)
--   2. _sprite_list_dirty 标志 true (用户 MarkSpriteListDirty 触发)
function ECSWorld:_CollectSprites()
    local n = #self._entities
    if self._sprite_cache and not self._sprite_list_dirty
       and self._sprite_cache_n == n then
        return self._sprite_cache
    end
    local list = {}
    for _, e in ipairs(self._entities) do
        local tf = e._comps.Transform2D
        local sp = e._comps.Sprite
        if tf and sp and sp.visible ~= false and sp.image then
            list[#list + 1] = {entity=e, tf=tf, sprite=sp, _z=tf.z or 0}
        end
    end
    -- 稳定排序 (Lua table.sort 非稳定, 但 z 不同时无歧义)
    table.sort(list, function(a, b) return a._z < b._z end)
    self._sprite_cache = list
    self._sprite_cache_n = n
    self._sprite_list_dirty = false
    return list
end

-- Phase D.x.5.1: 用户改 sprite z / visible / parent 后调用, 强制下次 Render 重建 list
function ECSWorld:MarkSpriteListDirty()
    self._sprite_list_dirty = true
end

-- Phase D.x.5.2: 计算 active Camera2D 视口在 world 坐标的 AABB
-- 返回 nil 表示不 cull (viewport 未设, fallback 全部渲染)
function ECSWorld:_FrustumCull2D(cam2d_entity)
    if not cam2d_entity then return nil end
    local ctf = cam2d_entity._comps.Transform2D
    local cc  = cam2d_entity._comps.Camera2D
    if not ctf or not cc then return nil end
    local vw = cc.viewportW or 0
    local vh = cc.viewportH or 0
    if vw <= 0 or vh <= 0 then return nil end
    local zoom = cc.zoom or 1
    if zoom == 0 then zoom = 1 end
    -- camera 视口在 world 的半宽/半高
    local hw = vw / 2 / zoom
    local hh = vh / 2 / zoom
    local cx = ctf.x or 0
    local cy = ctf.y or 0
    return {
        minX = cx - hw, maxX = cx + hw,
        minY = cy - hh, maxY = cy + hh,
    }
end

-- Phase D.x.5.3: 计算 sprite 的 world AABB (含 parent chain translation + scale)
-- 返回 nil 表示难计算 (self/parent 有 rotation), 调用方应不 cull (安全 fallback)
function ECSWorld:_GetSpriteWorldAABB(tf, s)
    -- self rotation: rotated AABB 计算复杂, fallback
    if (tf.rot or 0) ~= 0 then return nil end
    -- 累加 parent chain: world_pos = parent.scale * self.pos + parent.pos
    local wx = tf.x or 0
    local wy = tf.y or 0
    local wsx = math.abs(tf.sx or 1)
    local wsy = math.abs(tf.sy or 1)
    if tf.parent then
        local visited = {}
        local cur = tf.parent
        local depth = 0
        while cur and not visited[cur] and depth < 32 do
            visited[cur] = true
            local ptf = cur._comps and cur._comps.Transform2D
            if not ptf then break end
            -- parent rotation: rotated AABB 复杂, fallback
            if (ptf.rot or 0) ~= 0 then return nil end
            local psx = math.abs(ptf.sx or 1)
            local psy = math.abs(ptf.sy or 1)
            -- world 累加: scale 自下而上 multiply, translate 自下而上 add
            wx = wx * psx + (ptf.x or 0)
            wy = wy * psy + (ptf.y or 0)
            wsx = wsx * psx
            wsy = wsy * psy
            cur = ptf.parent
            depth = depth + 1
        end
    end
    -- 估算 sprite 内在大小 (优先 quad, 否则 image GetWidth/Height, 默认 64)
    local iw, ih = 64, 64
    local img = s.image
    if type(img) == 'table' then
        if type(img.GetWidth) == 'function' then iw = img:GetWidth() end
        if type(img.GetHeight) == 'function' then ih = img:GetHeight() end
    end
    local q = s.quad
    if q and (q.qw and q.qw > 0) then
        iw = q.qw
        ih = q.qh or q.qw
    end
    -- 累加 scale 后的世界大小
    local pw = iw * wsx
    local ph = ih * wsy
    local anc = s.anchor or {}
    local ax = anc.ax or 0
    local ay = anc.ay or 0
    return {
        minX = wx - ax * pw, maxX = wx - ax * pw + pw,
        minY = wy - ay * ph, maxY = wy - ay * ph + ph,
    }
end

-- Phase D.x.5.2/5.3: 检查 sprite 是否在 viewport bounds 内 (parent-aware)
function ECSWorld:_SpriteInBounds(tf, s, bounds)
    local aabb = self:_GetSpriteWorldAABB(tf, s)
    -- 难计算 (有 rotation) 时 fallback 不 cull (安全)
    if not aabb then return true end
    return not (aabb.maxX < bounds.minX or aabb.minX > bounds.maxX or
                aabb.maxY < bounds.minY or aabb.minY > bounds.maxY)
end

)LUA" R"LUA(
-- Phase D.x.1: Push parent transform chain (不含 self), 返回 push 次数
-- 调用方在 self draw 之后, 必须 Pop 返回的次数才能平衡 stack
-- 循环引用保护: visited 表 + 32 层 max depth
function ECSWorld:_PushParentChain2D(entity, gfx)
    local tf = entity and entity._comps and entity._comps.Transform2D
    if not tf or not tf.parent then return 0 end
    -- 1. 从 leaf 向 root 收集 chain (跳过 self)
    local chain = {}
    local visited = {}
    local cur = tf.parent
    while cur and not visited[cur] and #chain < 32 do
        visited[cur] = true
        local ptf = cur._comps and cur._comps.Transform2D
        if not ptf then break end
        chain[#chain + 1] = ptf
        cur = ptf.parent
    end
    -- 2. 反向 push (root 最外层, leaf 最内层)
    for i = #chain, 1, -1 do
        local ptf = chain[i]
        gfx.Push()
        gfx.Translate(ptf.x or 0, ptf.y or 0, 0)
        if (ptf.rot or 0) ~= 0 then gfx.Rotate(ptf.rot, 0, 0, 1) end
        local sx, sy = ptf.sx or 1, ptf.sy or 1
        if sx ~= 1 or sy ~= 1 then gfx.Scale(sx, sy, 1) end
    end
    return #chain
end

-- Phase D.x.1.2: 3D 版本 parent chain (gfx stack 模式, 用于 _DrawMesh)
-- 与 2D 版本对称, 但用 Translate(x,y,z) + Rotate*3 + Scale(x,y,z) 三轴
function ECSWorld:_PushParentChain3D(entity, gfx)
    local tf = entity and entity._comps and entity._comps.Transform3D
    if not tf or not tf.parent then return 0 end
    local chain = {}
    local visited = {}
    local cur = tf.parent
    while cur and not visited[cur] and #chain < 32 do
        visited[cur] = true
        local ptf = cur._comps and cur._comps.Transform3D
        if not ptf then break end
        chain[#chain + 1] = ptf
        cur = ptf.parent
    end
    for i = #chain, 1, -1 do
        local ptf = chain[i]
        gfx.Push()
        gfx.Translate(ptf.x or 0, ptf.y or 0, ptf.z or 0)
        if (ptf.rx or 0) ~= 0 then gfx.Rotate(ptf.rx, 1, 0, 0) end
        if (ptf.ry or 0) ~= 0 then gfx.Rotate(ptf.ry, 0, 1, 0) end
        if (ptf.rz or 0) ~= 0 then gfx.Rotate(ptf.rz, 0, 0, 1) end
        local sx, sy, sz = ptf.sx or 1, ptf.sy or 1, ptf.sz or 1
        if sx ~= 1 or sy ~= 1 or sz ~= 1 then gfx.Scale(sx, sy, sz) end
    end
    return #chain
end

-- Phase D.x.6: 绘制 SpriteBatch (共享 image + transform + color 的 quad 列表)
-- 节省: 全局 1 次 Push/Pop + 1 次 SetColor (vs N 次)
function ECSWorld:_DrawSpriteBatch(tf, batch, gfx)
    if not batch.image or not batch.quads then return end
    gfx.Push()
    gfx.Translate(tf.x or 0, tf.y or 0, 0)
    if (tf.rot or 0) ~= 0 then gfx.Rotate(tf.rot, 0, 0, 1) end
    local sx, sy = tf.sx or 1, tf.sy or 1
    if sx ~= 1 or sy ~= 1 then gfx.Scale(sx, sy, 1) end
    local col = batch.color or {}
    gfx.SetColor(col.r or 1, col.g or 1, col.b or 1, col.a or 1)
    for i = 1, #batch.quads do
        local q = batch.quads[i]
        if q.qw and q.qw > 0 then
            gfx.DrawQuad(batch.image, q.x or 0, q.y or 0, 0,
                         q.qx or 0, q.qy or 0, q.qw, q.qh or q.qw)
        else
            gfx.Draw(batch.image, q.x or 0, q.y or 0, 0)
        end
    end
    gfx.Pop()
end

-- 绘制单个 sprite (在 Render() 中循环调用)
function ECSWorld:_DrawSprite(tf, s, gfx)
    gfx.Push()
    gfx.Translate(tf.x or 0, tf.y or 0, 0)
    if (tf.rot or 0) ~= 0 then gfx.Rotate(tf.rot, 0, 0, 1) end
    local sx = tf.sx or 1
    local sy = tf.sy or 1
    -- 翻转通过负 scale 实现 (anchor 已固定为图片左上偏移, 翻转后需补偿)
    if s.flipX then sx = -sx end
    if s.flipY then sy = -sy end
    if sx ~= 1 or sy ~= 1 then gfx.Scale(sx, sy, 1) end

    local col = s.color or {}
    gfx.SetColor(col.r or 1, col.g or 1, col.b or 1, col.a or 1)

    -- 取图像尺寸 (defensive: image 可能是 mock table 或 真实 userdata)
    local img = s.image
    local iw, ih = 64, 64
    if type(img) == 'table' then
        if type(img.GetWidth) == 'function' then iw = img:GetWidth() end
        if type(img.GetHeight) == 'function' then ih = img:GetHeight() end
    end

    -- anchor 偏移: anchor (0,0)=左上, (0.5,0.5)=中心, (1,1)=右下
    local anc = s.anchor or {}
    local ax = anc.ax or 0
    local ay = anc.ay or 0
    local drawX = -ax * iw
    local drawY = -ay * ih

    local q = s.quad
    if q and (q.qw and q.qw > 0) then
        gfx.DrawQuad(img, drawX, drawY, 0,
                     q.qx or 0, q.qy or 0, q.qw, q.qh or q.qw)
    else
        gfx.Draw(img, drawX, drawY, 0)
    end
    gfx.Pop()
end

-- 绘制单个 mesh (3D)
function ECSWorld:_DrawMesh(tf, mr, gfx)
    gfx.Push()
    gfx.Translate(tf.x or 0, tf.y or 0, tf.z or 0)
    if (tf.rx or 0) ~= 0 then gfx.Rotate(tf.rx, 1, 0, 0) end
    if (tf.ry or 0) ~= 0 then gfx.Rotate(tf.ry, 0, 1, 0) end
    if (tf.rz or 0) ~= 0 then gfx.Rotate(tf.rz, 0, 0, 1) end
    local sx = tf.sx or 1
    local sy = tf.sy or 1
    local sz = tf.sz or 1
    if sx ~= 1 or sy ~= 1 or sz ~= 1 then gfx.Scale(sx, sy, sz) end

    local mesh = mr.mesh
    if mesh and type(mesh.Draw) == 'function' then
        if mr.material then
            mesh:Draw(mr.material)
        else
            mesh:Draw(0)
        end
    end
    gfx.Pop()
end

-- 渲染入口: 用户在 window.Draw 中调一次
-- 自动跑 2D camera → sprite (z-sort) → 3D camera → mesh 四阶段
function ECSWorld:Render()
    -- 取 Light.Graphics 模块 (server-only 进程无, 静默跳过)
    local gfx = Light and Light.Graphics
    if not gfx or type(gfx.Push) ~= 'function' then
        if not self._render_warned then
            self._render_warned = true
            print('[ECS] Light.Graphics not available, world:Render() is a no-op')
        end
        return
    end

    -- ============ 2D 阶段 ============
    local cam2d = self:_FindActiveCamera('Camera2D', 'Transform2D')
    if cam2d then
        local ctf = cam2d._comps.Transform2D
        local cc  = cam2d._comps.Camera2D
        local zoom = cc.zoom or 1.0
        gfx.Push()
        if zoom ~= 1.0 then gfx.Scale(zoom, zoom, 1) end
        gfx.Translate(-(ctf.x or 0), -(ctf.y or 0), 0)
    end

    local sprites = self:_CollectSprites()
    -- Phase D.x.5.2: 计算 camera frustum, 跳过出场 sprite
    local bounds = self:_FrustumCull2D(cam2d)
    local culled = 0
    for i = 1, #sprites do
        local item = sprites[i]
        if bounds and not self:_SpriteInBounds(item.tf, item.sprite, bounds) then
            culled = culled + 1
        else
            -- Phase D.x.1: parent chain push (root → leaf 外侧), pop 平衡
            local pushCount = self:_PushParentChain2D(item.entity, gfx)
            self:_DrawSprite(item.tf, item.sprite, gfx)
            for k = 1, pushCount do gfx.Pop() end
        end
    end
    self._cull_stats_2d = {total = #sprites, culled = culled, drawn = #sprites - culled}

    -- Phase D.x.6: SpriteBatch 渲染 (在 sprite 之后, 共享 camera transform)
    for _, e in ipairs(self._entities) do
        local tf = e._comps.Transform2D
        local sb = e._comps.SpriteBatch
        if tf and sb and sb.visible ~= false and sb.image and sb.quads and #sb.quads > 0 then
            local pushCount = self:_PushParentChain2D(e, gfx)
            self:_DrawSpriteBatch(tf, sb, gfx)
            for k = 1, pushCount do gfx.Pop() end
        end
    end

    if cam2d then gfx.Pop() end

    -- ============ 3D 阶段 (仅在有 active Camera3D 时启用) ============
    local cam3d = self:_FindActiveCamera('Camera3D', 'Transform3D')
    if cam3d then
        local ctf = cam3d._comps.Transform3D
        local cc  = cam3d._comps.Camera3D
        if type(gfx.SetPerspective) == 'function' then
            gfx.SetPerspective(cc.fovY or 60, cc.aspect or 1.333,
                               cc.nearZ or 0.1, cc.farZ or 1000)
        end
        if type(gfx.SetCamera) == 'function' then
            gfx.SetCamera(ctf.x or 0, ctf.y or 0, ctf.z or 0,
                          cc.targetX or 0, cc.targetY or 0, cc.targetZ or 0,
                          cc.upX or 0, cc.upY or 1, cc.upZ or 0)
        end
        if type(gfx.SetDepthTest) == 'function' then gfx.SetDepthTest(true) end

        for _, e in ipairs(self._entities) do
            local tf = e._comps.Transform3D
            local mr = e._comps.MeshRenderer
            if tf and mr and mr.visible ~= false and mr.mesh then
                -- Phase D.x.1.2: parent chain push (gfx stack 模式)
                local pushCount = self:_PushParentChain3D(e, gfx)
                self:_DrawMesh(tf, mr, gfx)
                for k = 1, pushCount do gfx.Pop() end
            end
        end

        -- Phase D.x.4: 蒙皮 mesh 渲染 (在普通 mesh 之后, 仍在 depth test 内)
        -- Phase D.x.1.1: 传 entity 触发 parent chain matrix multiply (matrix 模式)
        for _, e in ipairs(self._entities) do
            local tf  = e._comps.Transform3D
            local smr = e._comps.SkinnedMeshRenderer
            if tf and smr and smr.visible ~= false and smr.mesh and smr.animator then
                self:_DrawSkinnedMesh(e, smr)
            end
        end

        if type(gfx.SetDepthTest) == 'function' then gfx.SetDepthTest(false) end
    end
end
)LUA" R"LUA(
-- ==================== Phase D.x.4: 骨骼动画 ECS 集成 ====================

-- 4x4 列主序矩阵乘法 r = a * b
local function _Mat4Multiply(a, b)
    local r = {}
    for col = 0, 3 do
        for row = 0, 3 do
            local s = 0
            for k = 0, 3 do
                s = s + a[k*4 + row + 1] * b[col*4 + k + 1]
            end
            r[col*4 + row + 1] = s
        end
    end
    return r
end

-- 仅 local TRS, 不考虑 parent (Phase D.x.4.1: 完整 ZYX Euler)
function ECSWorld:_LocalMatrix3D(tf)
    local rx = (tf.rx or 0) * math.pi / 180
    local ry = (tf.ry or 0) * math.pi / 180
    local rz = (tf.rz or 0) * math.pi / 180
    local cx, sx = math.cos(rx), math.sin(rx)
    local cy, sy = math.cos(ry), math.sin(ry)
    local cz, sz = math.cos(rz), math.sin(rz)
    local scx = tf.sx or 1
    local scy = tf.sy or 1
    local scz = tf.sz or 1
    local m00 = cy*cz
    local m10 = cy*sz
    local m20 = -sy
    local m01 = -cx*sz + sx*sy*cz
    local m11 = cx*cz + sx*sy*sz
    local m21 = sx*cy
    local m02 = sx*sz + cx*sy*cz
    local m12 = -sx*cz + cx*sy*sz
    local m22 = cx*cy
    return {
        m00*scx, m10*scx, m20*scx, 0,
        m01*scy, m11*scy, m21*scy, 0,
        m02*scz, m12*scz, m22*scz, 0,
        tf.x or 0, tf.y or 0, tf.z or 0, 1,
    }
end

-- 构造 world matrix.
-- Phase D.x.1.1: 接受 entity (递归 parent chain) 或 tf table (向后兼容)
-- 检测方式: 若 arg 含 _comps.Transform3D, 视为 entity; 否则视为 tf table 直接
function ECSWorld:_BuildModelMatrix3D(arg)
    -- 兼容旧 tf-table 调用 (smoke ecs_skinned Dx4.1-AC3)
    if type(arg) ~= 'table' or arg._comps == nil then
        return self:_LocalMatrix3D(arg)
    end
    local entity = arg
    local tf = entity._comps.Transform3D
    if not tf then return self:_LocalMatrix3D({}) end  -- 单位矩阵
    local local_mat = self:_LocalMatrix3D(tf)
    if not tf.parent then return local_mat end
    -- 递归累乘 parent world matrix (含循环保护)
    local pworld = self:_GetWorldMatrix3D(tf.parent, {}, 0)
    if not pworld then return local_mat end
    return _Mat4Multiply(pworld, local_mat)
end

-- 递归取 entity 的 world matrix (含 parent chain)
-- 安全: visited 表 + 32 深度限制
function ECSWorld:_GetWorldMatrix3D(entity, visited, depth)
    if depth >= 32 or visited[entity] then return nil end
    visited[entity] = true
    local tf = entity._comps and entity._comps.Transform3D
    if not tf then return nil end
    local local_mat = self:_LocalMatrix3D(tf)
    if not tf.parent then return local_mat end
    local pworld = self:_GetWorldMatrix3D(tf.parent, visited, depth + 1)
    if not pworld then return local_mat end
    return _Mat4Multiply(pworld, local_mat)
end

-- Phase D.x.4.1: LookAt helper (列主序 view matrix), 用于 Camera 或角色朝向计算
-- 返回 16-element table; 用户可以从中取出朝向角度或直接传给 Light.Graphics.SetCamera
local function _LookAtMatrix(eyeX, eyeY, eyeZ, tgtX, tgtY, tgtZ, upX, upY, upZ)
    -- forward = normalize(target - eye)
    local fx, fy, fz = tgtX - eyeX, tgtY - eyeY, tgtZ - eyeZ
    local fl = math.sqrt(fx*fx + fy*fy + fz*fz)
    if fl < 1e-10 then fl = 1 end
    fx, fy, fz = fx/fl, fy/fl, fz/fl
    -- right = normalize(forward x up)
    local rx = fy*upZ - fz*upY
    local ry = fz*upX - fx*upZ
    local rz = fx*upY - fy*upX
    local rl = math.sqrt(rx*rx + ry*ry + rz*rz)
    if rl < 1e-10 then rl = 1 end
    rx, ry, rz = rx/rl, ry/rl, rz/rl
    -- up' = right x forward (重新正交化)
    local ux = ry*fz - rz*fy
    local uy = rz*fx - rx*fz
    local uz = rx*fy - ry*fx
    -- 列主序 view matrix
    return {
        rx, ux, -fx, 0,
        ry, uy, -fy, 0,
        rz, uz, -fz, 0,
        -(rx*eyeX + ry*eyeY + rz*eyeZ),
        -(ux*eyeX + uy*eyeY + uz*eyeZ),
         (fx*eyeX + fy*eyeY + fz*eyeZ),
        1,
    }
end

-- 绘制蒙皮 mesh 单次. 内部调 Light.Animation.DrawSkinnedMesh (CPU/GPU 分流由后端决定)
-- Phase D.x.1.1: 接 entity (而非仅 tf), 触发 parent chain matrix multiply
function ECSWorld:_DrawSkinnedMesh(entity, smr)
    local Anim = Light and Light.Animation
    if not (Anim and type(Anim.DrawSkinnedMesh) == 'function') then return end
    local model = self:_BuildModelMatrix3D(entity)
    pcall(Anim.DrawSkinnedMesh, smr.mesh, smr.animator, model, smr.material)
end

-- 动画系统: 在 world:Update(dt) 末尾自动跑 (sync 前)
-- 职责:
--   1. 遍历所有 SkinnedMeshRenderer + AnimationState entity
--   2. 检测 AnimationState 字段变化, 桥接到 animator 对应方法
--   3. 推进 animator:Update(dt)
function ECSWorld:_AnimationSystem(dt)
    local Anim = Light and Light.Animation
    if not Anim then return end
    self._anim_cache = self._anim_cache or {}
    for _, e in ipairs(self._entities) do
        local smr = e._comps.SkinnedMeshRenderer
        local as  = e._comps.AnimationState
        if smr and smr.animator and as then
            local cache = self._anim_cache[e._id]
            if not cache then
                cache = {lastState='', lastSpeed=1.0, lastPaused=false, lastLooping=true,
                          lastParams={}, lastMorph={}}
                self._anim_cache[e._id] = cache
            end
            local an = smr.animator
            -- state 桥接 (crossfade>0 走 Crossfade, 否则 Play)
            if as.state ~= cache.lastState and as.state and as.state ~= '' then
                if (as.crossfade or 0) > 0 and type(an.Crossfade) == 'function' then
                    pcall(an.Crossfade, an, as.state, as.crossfade)
                elseif type(an.Play) == 'function' then
                    pcall(an.Play, an, as.state)
                end
                cache.lastState = as.state
            end
            -- speed 桥接
            if as.speed ~= cache.lastSpeed and type(an.SetSpeed) == 'function' then
                pcall(an.SetSpeed, an, as.speed or 1)
                cache.lastSpeed = as.speed
            end
            -- paused 桥接
            if as.paused ~= cache.lastPaused then
                if as.paused and type(an.Pause) == 'function' then pcall(an.Pause, an)
                elseif type(an.Resume) == 'function' then pcall(an.Resume, an) end
                cache.lastPaused = as.paused
            end
            -- Phase D.x.4.1: looping 桥接
            if as.looping ~= cache.lastLooping and type(an.SetLooping) == 'function' then
                pcall(an.SetLooping, an, as.looping and true or false)
                cache.lastLooping = as.looping
            end
            -- params 桥接 (diff apply)
            if as.params then
                for k, v in pairs(as.params) do
                    if cache.lastParams[k] ~= v and type(an.SetParam) == 'function' then
                        pcall(an.SetParam, an, k, v)
                        cache.lastParams[k] = v
                    end
                end
            end
            -- morphWeights 桥接 (1-based key)
            if as.morphWeights then
                for i, w in pairs(as.morphWeights) do
                    if cache.lastMorph[i] ~= w and type(an.SetMorphWeight) == 'function' then
                        pcall(an.SetMorphWeight, an, i, w)
                        cache.lastMorph[i] = w
                    end
                end
            end
            -- 推进时间
            if type(an.Update) == 'function' then pcall(an.Update, an, dt) end
        end
    end
end

-- 返回模块表 (luaopen_Light_ECS 把它设为 Light.ECS)
return {
    World           = ECSWorld,
    MirrorFromRoom  = MirrorFromRoom,
    -- Phase D.x.4.1: helper functions
    LookAt          = _LookAtMatrix,
}
)LUA";

// ==================== luaopen 注册 ====================

int luaopen_Light_ECS(lua_State* L) {
    LT::EnsureLightTable(L);

    // 执行 ECS Lua 脚本, 返回 module table {World, MirrorFromRoom}
    if (luaL_loadstring(L, g_ecsScript) || lua_pcall(L, 0, 1, 0)) {
        CC::Log(CC::LOG_ERROR, "ECS init error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_createtable(L, 0, 0);   // 失败时返回空表, 避免 require 崩溃
    }
    // 此时栈顶 = module table

    // 把 module table 挂到 Light.ECS.
    // 注意: 必须用 lua_rawset 绕过 Light OOP framework metatable 的 __newindex,
    // 否则触发 fallback("object is a static module"). 参考 light_animation.cpp 注释.
    lua_getfield(L, LUA_REGISTRYINDEX, "Light");  // [..., module, Light]
    lua_pushstring(L, "ECS");                      // [..., module, Light, "ECS"]
    lua_pushvalue(L, -3);                          // [..., module, Light, "ECS", module]
    lua_rawset(L, -3);                              // Light.ECS = module (rawset 绕过 metatable)
    lua_pop(L, 1);                                  // pop Light; [..., module]

    return 1;  // 返回 module table 给 require
}
