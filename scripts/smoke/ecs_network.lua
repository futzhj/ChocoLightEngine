-- Phase C smoke: Light.ECS 网络化 (C-T1 ~ C-T4)
-- ASCII-only.
--
-- 单进程验证 ECS networked component + dirty 跟踪 + NetworkSync + Mirror.
-- 用 mock room 替代真实 Room, 不依赖网络.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- 1) require
local ok, ECS = pcall(require, "Light.ECS")
if not ok then fail("require(Light.ECS): " .. tostring(ECS)) end
pass("require(Light.ECS) ok")

if type(ECS.World) ~= "table" then fail("Light.ECS.World missing") end
if type(ECS.MirrorFromRoom) ~= "function" then fail("Light.ECS.MirrorFromRoom missing") end
pass("ECS module exports present")

-- 2) ECS.World.new (Lua-style, 不走 Light(...):New() 元类包装)
--   现有 Light.ECS 暴露 ECSWorld class table; ECSWorld.new() 是构造函数
local World = ECS.World
local function NewWorld() return World.new() end

-- ============================================================
-- C-T1: RegisterComponent opts 扩展
-- ============================================================
do
    local w = NewWorld()
    -- 旧 API: 不传 opts (向后兼容)
    w:RegisterComponent("LegacyComp", {n=0})
    if w._networked_comps["LegacyComp"] ~= nil then
        fail("LegacyComp should not be networked")
    end
    pass("C-T1: legacy 2-arg RegisterComponent ok")

    -- 新 API: opts.networked = true
    w:RegisterComponent("NetComp", {x=0}, {networked=true})
    if w._networked_comps["NetComp"] ~= true then
        fail("NetComp should be marked networked")
    end
    pass("C-T1: opts.networked=true ok")

    -- 空 opts 表 = 非 networked
    w:RegisterComponent("EmptyOpts", {y=0}, {})
    if w._networked_comps["EmptyOpts"] ~= nil then
        fail("EmptyOpts should not be networked")
    end
    pass("C-T1: empty opts = non-networked ok")

    -- 错误的 opts 类型应抛错
    local ok_err = pcall(function()
        w:RegisterComponent("Bad", {}, "not a table")
    end)
    if ok_err then fail("RegisterComponent should reject non-table opts") end
    pass("C-T1: invalid opts type rejected ok")
end

-- ============================================================
-- C-T2: Entity hook + dirty 跟踪
-- ============================================================
do
    local w = NewWorld()
    w:RegisterComponent("Pos", {x=0, y=0}, {networked=true})
    w:RegisterComponent("Local", {n=0})    -- 非 networked

    if w._has_changes then fail("fresh world should have no changes") end

    local e = w:CreateEntity()
    e:Add("Pos", {x=10})
    if not w._has_changes then fail("Add networked should set _has_changes") end
    if not w._dirty_entities[e._id] then fail("entity id should be in _dirty_entities") end
    pass("C-T2: Add networked component triggers dirty ok")

    -- 非 networked component 不触发
    w._has_changes = false
    w._dirty_entities = {}
    e:Add("Local", {n=99})
    if w._has_changes then fail("Add non-networked should NOT set _has_changes") end
    pass("C-T2: Add non-networked does not trigger ok")

    -- Set: 必须先 Add
    local set_ok, set_err = pcall(function() e:Set("Missing", {x=1}) end)
    if set_ok then fail("Set on missing component should error") end
    if not set_err:match("not added") then fail("Set error should mention 'not added'") end
    pass("C-T2: Set on missing component errors ok")

    -- Set 已存在 component 触发 dirty
    w._has_changes = false
    e:Set("Pos", {x=20})
    if e.Pos.x ~= 20 then fail("Set should update component value") end
    if not w._has_changes then fail("Set networked should set _has_changes") end
    pass("C-T2: Set networked component triggers dirty ok")

    -- Remove networked 触发 dirty
    w._has_changes = false
    e:Remove("Pos")
    if not w._has_changes then fail("Remove networked should set _has_changes") end
    if e:Has("Pos") then fail("Remove should clear component") end
    pass("C-T2: Remove networked triggers dirty ok")

    -- DestroyEntity 触发 destroyed_ids
    w._has_changes = false
    w._destroyed_ids = {}
    local e2 = w:CreateEntity()
    e2:Add("Pos", {x=5})
    w._has_changes = false      -- 清除 Add 的 dirty, 单测 Destroy
    w._destroyed_ids = {}
    w:DestroyEntity(e2)
    if not w._has_changes then fail("DestroyEntity should set _has_changes") end
    if not w._destroyed_ids[e2._id] then fail("DestroyEntity should record id in _destroyed_ids") end
    pass("C-T2: DestroyEntity tracks destroyed id ok")
end

-- ============================================================
-- C-T3 (Phase C.x.1): NetworkSync + _SyncToRoom via Broadcast('ecs_delta')
-- ============================================================
do
    local w = NewWorld()
    w:RegisterComponent("Pos",   {x=0, y=0}, {networked=true})
    w:RegisterComponent("Vel",   {vx=0, vy=0}, {networked=true})
    w:RegisterComponent("Debug", {tag=""})    -- 非 networked

    -- mock room: 记录 Broadcast 调用 (Phase C.x.1 协议)
    local mock_room = {
        last_name = nil,
        last_args = nil,
        Broadcast = function(self, name, args)
            self.last_name = name
            self.last_args = args
        end,
    }

    w:NetworkSync(mock_room)
    if w._sync_room ~= mock_room then fail("NetworkSync should set _sync_room") end
    pass("C-T3: NetworkSync binds room ok")

    -- 创建 entity 并同步
    local e = w:CreateEntity()
    e:Add("Pos", {x=5, y=10})
    e:Add("Vel", {vx=1, vy=2})
    e:Add("Debug", {tag="hello"})

    -- per-component dirty 断言 (Phase C.x.1 新)
    if not w._dirty_comps[e._id] then fail("entity should be in _dirty_comps") end
    if w._dirty_comps[e._id].Pos ~= true then fail("Pos should be dirty") end
    if w._dirty_comps[e._id].Vel ~= true then fail("Vel should be dirty") end
    if w._dirty_comps[e._id].Debug ~= nil then fail("Debug (non-net) should NOT be dirty") end
    pass("C-T3: per-component dirty tracked ok")

    -- Update 应自动调 _SyncToRoom → Broadcast('ecs_delta', ...)
    w:Update(0)
    if mock_room.last_name ~= "ecs_delta" then
        fail("Broadcast name should be 'ecs_delta', got "..tostring(mock_room.last_name))
    end
    pass("C-T3: Broadcast('ecs_delta') invoked ok")

    if type(mock_room.last_args) ~= "table" then fail("ecs_delta args should be table") end
    if type(mock_room.last_args.set) ~= "table" then fail("ecs_delta.set missing") end

    local row = mock_room.last_args.set[tostring(e._id)]
    if not row then fail("entity row should be in set") end
    if row.Pos == nil or row.Pos.x ~= 5 or row.Pos.y ~= 10 then fail("Pos wire value incorrect") end
    if row.Vel == nil or row.Vel.vx ~= 1 then fail("Vel wire value incorrect") end
    if row.Debug ~= nil then fail("Debug (non-networked) should NOT be in row") end
    pass("C-T3: set row contains only networked comps ok")

    if w._has_changes then fail("_has_changes should be cleared after sync") end
    if next(w._dirty_comps) ~= nil then fail("_dirty_comps should be cleared after sync") end
    pass("C-T3: dirty state cleared after sync ok")

    -- 第二次 sync: 只改 Pos, 不改 Vel → wire 只含 Pos
    e:Set("Pos", {x=77})
    w:Update(0)
    local row2 = mock_room.last_args.set[tostring(e._id)]
    if row2.Pos == nil or row2.Pos.x ~= 77 then fail("updated Pos not in set") end
    if row2.Vel ~= nil then fail("Vel should NOT be in set (not dirty this frame)") end
    pass("C-T3: per-component delta sends only dirty comps ok")

    -- Remove networked comp → wire 中 __removed__
    e:Remove("Vel")
    w:Update(0)
    local row3 = mock_room.last_args.set[tostring(e._id)]
    if row3.Vel ~= "__removed__" then
        fail("Removed comp should appear as '__removed__', got "..tostring(row3.Vel))
    end
    pass("C-T3: Remove encodes '__removed__' in wire ok")

    -- DestroyEntity → wire 中 del
    local e_kill = w:CreateEntity()
    e_kill:Add("Pos", {x=0})
    w:Update(0)   -- 先同步创建
    mock_room.last_args = nil
    w:DestroyEntity(e_kill)
    w:Update(0)
    if not mock_room.last_args or not mock_room.last_args.del then fail("del array should be present") end
    local del_found = false
    for _, idStr in ipairs(mock_room.last_args.del) do
        if idStr == tostring(e_kill._id) then del_found = true; break end
    end
    if not del_found then fail("destroyed id should be in del array") end
    pass("C-T3: DestroyEntity encodes id in del ok")

    -- 解绑
    w:NetworkSync(nil)
    mock_room.last_name = nil
    e:Set("Pos", {x=999})
    w:Update(0)
    if mock_room.last_name ~= nil then fail("After unbind, Broadcast should NOT be called") end
    pass("C-T3: NetworkSync(nil) unbinds ok")

    -- 空 dirty 不调用
    w:NetworkSync(mock_room)
    mock_room.last_name = nil
    w._has_changes = false
    w._dirty_comps = {}
    w:Update(0)
    if mock_room.last_name ~= nil then fail("Update with no dirty should NOT call Broadcast") end
    pass("C-T3: no-changes Update skips broadcast ok")
end

-- ============================================================
-- C-T4 (Phase C.x.1): MirrorFromRoom + _ApplyDelta via OnEvent('ecs_delta')
-- ============================================================
do
    -- mock room: 记录 OnEvent 注册的回调
    local mock_room = {
        on_event_cb = nil,
        OnEvent = function(self, cb) self.on_event_cb = cb end,
    }

    local mirror = ECS.MirrorFromRoom(mock_room)
    if not mirror._is_mirror then fail("mirror should have _is_mirror=true") end
    if mock_room.on_event_cb == nil then fail("MirrorFromRoom should hook OnEvent") end
    pass("C-T4: MirrorFromRoom hooks OnEvent ok")

    -- 推一个 delta: 新增 2 个 entity
    mock_room.on_event_cb('ecs_delta', {
        set = {
            ["1"] = { Pos = {x=10, y=20} },
            ["2"] = { Pos = {x=30, y=40}, Sprite = {img="hero.png"} },
        }
    })

    local e1 = mirror._mirror_by_id[1]
    if not e1 then fail("mirror should have entity 1") end
    if e1.Pos.x ~= 10 or e1.Pos.y ~= 20 then fail("entity 1 Pos incorrect") end
    pass("C-T4: delta creates entity 1 ok")

    local e2 = mirror._mirror_by_id[2]
    if not e2 or e2.Sprite.img ~= "hero.png" then fail("entity 2 multi-comp incorrect") end
    pass("C-T4: delta creates entity 2 with multi components ok")

    local pos_ents = mirror:Query("Pos")
    if #pos_ents ~= 2 then fail("Query Pos should return 2") end
    pass("C-T4: mirror Query works on delta-synced entities ok")

    -- 引用稳定: 缓存 Pos, 再推一个只改 Pos 的 delta
    local pos1_ref = e1.Pos
    mock_room.on_event_cb('ecs_delta', {
        set = {["1"] = { Pos = {x=99, y=20} }}
    })
    if e1.Pos.x ~= 99 then fail("entity 1 Pos.x should update to 99") end
    if pos1_ref ~= e1.Pos then fail("Pos reference should be stable (R5)") end
    pass("C-T4: shallow-merge preserves component table reference ok")

    -- Phase C.x.1 新语义: 只含 Pos 的 delta **不**删除 entity 2
    if mirror._mirror_by_id[2] == nil then fail("entity 2 should remain (delta preserves unlisted)") end
    if e2.Sprite.img ~= "hero.png" then fail("entity 2 Sprite should remain untouched") end
    pass("C-T4: delta preserves entities NOT in set (unlike v1 OnState) ok")

    -- __removed__ 标记应显式删除 component
    mock_room.on_event_cb('ecs_delta', {
        set = {["2"] = { Sprite = "__removed__" }}
    })
    if e2:Has("Sprite") then fail("Sprite should be removed on '__removed__' marker") end
    if not e2:Has("Pos") then fail("Pos should remain on e2 after Sprite remove") end
    pass("C-T4: __removed__ marker deletes component ok")

    -- del 数组应销毁 entity
    mock_room.on_event_cb('ecs_delta', {
        del = {"2"}
    })
    if mirror._mirror_by_id[2] ~= nil then fail("entity 2 should be removed via del") end
    pass("C-T4: del array removes entity ok")

    -- 空 delta 不崩
    mock_room.on_event_cb('ecs_delta', {})
    mock_room.on_event_cb('ecs_delta', {set={}})
    mock_room.on_event_cb('ecs_delta', {del={}})
    pass("C-T4: empty/partial delta safe ok")

    -- 非 ecs_delta 事件被忽略
    mock_room.on_event_cb('some_other_event', {set={["1"]={Pos={x=-1}}}})
    if e1.Pos.x ~= 99 then fail("non-ecs_delta event should be ignored") end
    pass("C-T4: non-ecs_delta event ignored ok")
end

-- ============================================================
-- CX1-T5: MarkFullResync 行为
-- ============================================================
do
    local w = NewWorld()
    w:RegisterComponent("Pos", {x=0, y=0}, {networked=true})
    w:RegisterComponent("Vel", {vx=0, vy=0}, {networked=true})

    local mock_room = {
        last_args = nil,
        Broadcast = function(self, name, args) self.last_args = args end,
    }
    w:NetworkSync(mock_room)

    local e1 = w:CreateEntity()
    e1:Add("Pos", {x=1, y=2})
    e1:Add("Vel", {vx=3, vy=4})
    local e2 = w:CreateEntity()
    e2:Add("Pos", {x=5, y=6})

    -- 首次 sync 后全部变 clean
    w:Update(0)
    mock_room.last_args = nil

    -- idle update (无 dirty) → 不广播
    w:Update(0)
    if mock_room.last_args ~= nil then fail("idle Update should not broadcast") end
    pass("CX1-T5: idle Update skips broadcast ok")

    -- 调 MarkFullResync → 下次 Update 重发全量
    w:MarkFullResync()
    w:Update(0)
    if not mock_room.last_args then fail("full resync should broadcast") end
    local set = mock_room.last_args.set
    if not set then fail("full resync set missing") end
    if not set[tostring(e1._id)] then fail("e1 should be in full resync set") end
    if not set[tostring(e2._id)] then fail("e2 should be in full resync set") end
    if set[tostring(e1._id)].Pos == nil then fail("e1.Pos missing in full resync") end
    if set[tostring(e1._id)].Vel == nil then fail("e1.Vel missing in full resync") end
    if set[tostring(e2._id)].Pos == nil then fail("e2.Pos missing in full resync") end
    pass("CX1-T5: MarkFullResync broadcasts all entities all networked comps ok")

    -- resync 标志应被 Update 消费
    if w._needs_full_resync then fail("_needs_full_resync should be cleared") end
    pass("CX1-T5: _needs_full_resync cleared after consume ok")

    -- 再次 idle → 不重复广播
    mock_room.last_args = nil
    w:Update(0)
    if mock_room.last_args ~= nil then fail("post-resync idle should not broadcast") end
    pass("CX1-T5: full-resync is one-shot ok")
end

print("== Light.ECS network smoke PASS ==")
