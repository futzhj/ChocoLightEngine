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
-- C-T3: NetworkSync + _SyncToRoom
-- ============================================================
do
    local w = NewWorld()
    w:RegisterComponent("Pos",   {x=0, y=0}, {networked=true})
    w:RegisterComponent("Debug", {tag=""})    -- 非 networked

    -- mock room: 记录最后一次 PatchState 调用
    local mock_room = {
        last_set = nil,
        last_del = nil,
        PatchState = function(self, set, del)
            self.last_set = set
            self.last_del = del
        end,
    }

    w:NetworkSync(mock_room)
    if w._sync_room ~= mock_room then fail("NetworkSync should set _sync_room") end
    pass("C-T3: NetworkSync binds room ok")

    -- 创建 entity 并同步
    local e = w:CreateEntity()
    e:Add("Pos", {x=5, y=10})
    e:Add("Debug", {tag="hello"})

    -- Update 应自动调 _SyncToRoom
    w:Update(0)
    if mock_room.last_set == nil then fail("Update should trigger PatchState") end
    if type(mock_room.last_set.entities) ~= "table" then
        fail("PatchState set should contain entities table")
    end

    local row = mock_room.last_set.entities[tostring(e._id)]
    if not row then fail("entity row should be in patch") end
    if row.Pos == nil then fail("Pos (networked) should be in row") end
    if row.Pos.x ~= 5 or row.Pos.y ~= 10 then fail("Pos values incorrect") end
    if row.Debug ~= nil then fail("Debug (non-networked) should NOT be in row") end
    pass("C-T3: PatchState set contains networked components only ok")

    if w._has_changes then fail("_has_changes should be cleared after sync") end
    pass("C-T3: _has_changes cleared after sync ok")

    -- 解绑
    w:NetworkSync(nil)
    mock_room.last_set = nil
    e:Set("Pos", {x=99})
    w:Update(0)
    if mock_room.last_set ~= nil then fail("After unbind, PatchState should not be called") end
    pass("C-T3: NetworkSync(nil) unbinds ok")

    -- 空 patch (无 dirty 时不调用)
    w:NetworkSync(mock_room)
    mock_room.last_set = nil
    w._has_changes = false
    w:Update(0)
    if mock_room.last_set ~= nil then fail("Update with no changes should NOT call PatchState") end
    pass("C-T3: no-changes Update skips sync ok")
end

-- ============================================================
-- C-T4: MirrorFromRoom + _ApplyState
-- ============================================================
do
    -- mock room: 记录 OnState 注册的回调
    local mock_room = {
        on_state_cb = nil,
        OnState = function(self, cb) self.on_state_cb = cb end,
    }

    local mirror = ECS.MirrorFromRoom(mock_room)
    if not mirror._is_mirror then fail("mirror should have _is_mirror=true") end
    if mock_room.on_state_cb == nil then fail("MirrorFromRoom should hook OnState") end
    pass("C-T4: MirrorFromRoom hooks OnState ok")

    -- 模拟 server 推 state
    mock_room.on_state_cb({
        entities = {
            ["1"] = { Pos = {x=10, y=20} },
            ["2"] = { Pos = {x=30, y=40}, Sprite = {img="hero.png"} },
        }
    }, 1)

    local e1 = mirror._mirror_by_id[1]
    if not e1 then fail("mirror should have entity 1") end
    if e1.Pos.x ~= 10 or e1.Pos.y ~= 20 then fail("entity 1 Pos incorrect") end
    pass("C-T4: mirror creates entity 1 from state ok")

    local e2 = mirror._mirror_by_id[2]
    if not e2 then fail("mirror should have entity 2") end
    if e2.Sprite.img ~= "hero.png" then fail("entity 2 Sprite incorrect") end
    pass("C-T4: mirror creates entity 2 with multiple components ok")

    -- Query 应正常工作
    local pos_ents = mirror:Query("Pos")
    if #pos_ents ~= 2 then fail("Query Pos should return 2 entities, got "..#pos_ents) end
    pass("C-T4: mirror:Query works on synced entities ok")

    -- 引用稳定: 保存当前 Pos 引用, 之后浅覆盖 server 应保持同一 table
    local pos1_ref = e1.Pos

    -- 模拟 server 修改 entity 1 + 销毁 entity 2
    mock_room.on_state_cb({
        entities = {
            ["1"] = { Pos = {x=99, y=20} },
            -- entity 2 消失
        }
    }, 2)

    if e1.Pos.x ~= 99 then fail("entity 1 Pos.x should update to 99") end
    if pos1_ref ~= e1.Pos then fail("Pos table reference should be stable (R5)") end
    if pos1_ref.x ~= 99 then fail("Stale ref should reflect new value (shallow merge)") end
    pass("C-T4: shallow-merge keeps Pos table reference stable ok")

    if mirror._mirror_by_id[2] ~= nil then fail("entity 2 should be removed from mirror") end
    pass("C-T4: missing entity removed from mirror ok")

    -- Query 数量更新
    local pos_ents2 = mirror:Query("Pos")
    if #pos_ents2 ~= 1 then fail("Query Pos should return 1 entity after removal") end
    pass("C-T4: mirror Query reflects entity removal ok")

    -- 第二次推同一 state 应 idempotent (no crash, 状态不变)
    mock_room.on_state_cb({
        entities = { ["1"] = { Pos = {x=99, y=20} } }
    }, 3)
    if e1.Pos.x ~= 99 then fail("idempotent re-apply should not change values") end
    if mirror:GetEntityCount() ~= 1 then fail("idempotent re-apply should not duplicate entities") end
    pass("C-T4: _ApplyState is idempotent ok")
end

print("== Light.ECS network smoke PASS ==")
