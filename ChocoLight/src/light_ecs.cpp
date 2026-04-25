/**
 * @file light_ecs.cpp
 * @brief Light.ECS — 纯 Lua Entity-Component-System
 *
 * Lua API:
 *   world = Light(Light.ECS.World):New()
 *   world:RegisterComponent(name, defaults)
 *   entity = world:CreateEntity()
 *   entity:Add(compName, data)
 *   entity:Remove(compName)
 *   entity:Get(compName) → table
 *   entity:Has(compName) → bool
 *   world:AddSystem(name, requiredComponents, updateFunc)
 *   world:Update(dt)
 *   world:Query(comp1, comp2, ...) → entities
 *   world:DestroyEntity(entity)
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
    return w
end

-- 注册组件类型
function ECSWorld:RegisterComponent(name, defaults)
    self._components[name] = defaults or {}
end

-- 深拷贝表
local function deepcopy(orig)
    if type(orig) ~= 'table' then return orig end
    local copy = {}
    for k, v in pairs(orig) do copy[k] = deepcopy(v) end
    return copy
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
        return self
    end

    function e:Remove(compName)
        self._comps[compName] = nil
        self[compName] = nil
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
    for _, sys in ipairs(self._systems) do
        local entities = self:Query(table.unpack(sys.required))
        sys.func(entities, dt)
    end
end

-- 实体总数
function ECSWorld:GetEntityCount()
    return #self._entities
end

return ECSWorld
)LUA";

// ==================== luaopen 注册 ====================

int luaopen_Light_ECS(lua_State* L) {
    LT::EnsureLightTable(L);

    // 执行 ECS Lua 脚本, 返回 ECSWorld class
    if (luaL_loadstring(L, g_ecsScript) || lua_pcall(L, 0, 1, 0)) {
        CC::Log(CC::LOG_ERROR, "ECS init error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_createtable(L, 0, 0);
    }

    // 创建 Light.ECS = { World = ECSWorldClass }
    lua_getfield(L, LUA_REGISTRYINDEX, "Light");
    lua_pushstring(L, "ECS");
    lua_createtable(L, 0, 2);

    // ECS.World = ECSWorld class (from script)
    lua_pushstring(L, "World");
    lua_pushvalue(L, -5);  // ECSWorld class
    lua_rawset(L, -3);

    lua_rawset(L, -3);  // Light.ECS = table
    lua_pop(L, 1);      // pop Light

    // 返回 Light.ECS
    lua_getfield(L, LUA_REGISTRYINDEX, "Light");
    lua_pushstring(L, "ECS");
    lua_rawget(L, -2);
    lua_remove(L, -2);
    return 1;
}
