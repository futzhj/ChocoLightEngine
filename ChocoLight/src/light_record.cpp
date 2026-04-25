/**
 * @file light_record.cpp
 * @brief Light.Record 模块 — 纯 Lua ORM (数据库记录管理)
 * @note 深度还原自 Light.dll IDA 反编译 sub_1800B03D0
 *
 * 这是一个纯 Lua 实现的模块: 整个 ORM 框架以 Lua 脚本嵌入到二进制中,
 * 通过 luaL_loadstring + lua_pcall 在初始化时执行。
 *
 * 包含的 Lua 类:
 *   Where   — SQL WHERE 条件构建器 (Same/Greater/Less/Or 变体)
 *   Record  — 单行记录实例 (Insert/Update/Delete)
 *   Records — 表级管理器 (Table/Fetch/Find/Count/Where 等)
 *              包含完整的字段类型系统:
 *              Serial=0, Int=1, AutoInt=2, BigInt=3, AutoBigInt=4,
 *              Float=5, Double=6, TinyText=7, Text=8, MediumText=9,
 *              LongText=10, Blob=11, Date=12, TimeStamp=13
 */

#include "light.h"
#include <cstring>

// ==================== 嵌入 Lua ORM 脚本 ====================
// 精确还原自 IDA 字符串 dword_1802CFA94 引用的嵌入脚本
// 原始实现通过 sub_180002D50 解密/加载此脚本

static const char s_RecordLuaScript[] = R"LUA(
local function serialize(database, fieldType, v)
  if type(fieldType) ~= 'number' then
    return ''
  end
  if fieldType < 7 then
    if type(v) == 'number' then
      return v
    else
      return tonumber(v)
    end
  elseif fieldType < 11 then
    return database.Escape(v)
  elseif fieldType < 12 then
    return database.Blob(v)
  elseif fieldType < 14 then
    if type(v) == 'number' then
      return v
    else
      return database.Escape(v)
    end
  end
  return ''
end

local Where = {}

function Where.__call(self, database)
  rawset(self, '__database', database)
end

function Where.Same(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'and `' or '`') .. k .. "` = '" .. v .. "'")
  return self
end

function Where.Greater(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'and `' or '`') .. k .. "` > '" .. v .. "'")
  return self
end

function Where.GreaterEqual(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'and `' or '`') .. k .. "` >= '" .. v .. "'")
  return self
end

function Where.Less(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'and `' or '`') .. k .. "` < '" .. v .. "'")
  return self
end

function Where.LessEqual(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'and `' or '`') .. k .. "` <= '" .. v .. "'")
  return self
end

function Where.OrSame(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'or `' or '`') .. k .. "` = '" .. v .. "'")
  return self
end

function Where.OrGreater(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'or `' or '`') .. k .. "` > '" .. v .. "'")
  return self
end

function Where.OrGreaterEqual(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'or `' or '`') .. k .. "` >= '" .. v .. "'")
  return self
end

function Where.OrLess(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'or `' or '`') .. k .. "` < '" .. v .. "'")
  return self
end

function Where.OrLessEqual(self, k, v)
  v = type(v) == 'number' and v or self.__database.Escape(v)
  table.insert(self, (#self > 0 and 'and `' or '`') .. k .. "` <= '" .. v .. "'")
  return self
end

local Record = {}

function Record.__call(self, records)
  rawset(self, '__values', {})
  rawset(self, '__records', records)
end

function Record.__index(self, k)
  return rawget(rawget(self, '__values'), k)
end

function Record.__newindex(self, k, v)
  local fieldType = rawget(rawget(rawget(self, '__records'), '__fields'), k)
  if fieldType ~= nil then
    rawset(rawget(self, '__values'), k, v)
  end
end

function Record.Insert(self)
  local values = rawget(self, '__values')
  local sql = "insert into `" .. self.__records.__table .. "`"
  local fields = self.__records.__fields
  local database = self.__records.__database
  local dataFields = {}
  local data = {}
  for k, v in pairs(values) do
    local fieldType = rawget(fields, k)
    table.insert(dataFields, k)
    table.insert(data, serialize(database, fieldType, v))
  end
  if #data == 0 then return false end
  sql = sql .. ' (`' .. table.concat(dataFields, "`, `") .. '`)'
  sql = sql .. " values ('" .. table.concat(data, "', '") .. "');"
  local ret, _ = self.__records:Execute(sql)
  return ret
end

function Record.Update(self, where)
  local values = rawget(self, '__values')
  local sql = "update `" .. self.__records.__table .. "` set "
  local fields = self.__records.__fields
  local database = self.__records.__database
  local primary = self.__records.__primary
  local data = {}
  for k, v in pairs(values) do
    if k ~= primary then
      local fieldType = rawget(fields, k)
      table.insert(data, '`' .. k .. "` = '" .. serialize(database, fieldType, v) .. "'")
    end
  end
  if #data == 0 then return false end
  if type(where) == 'table' then
    if Light(where):Is(Where) == false then
      error('type `Where` not match')
    end
  else
    if primary == '' then return false end
    local primaryValue = rawget(values, primary)
    if primaryValue == nil then return false end
    where = self.__records:Where(primary, primaryValue)
  end
  sql = sql .. table.concat(data, ", ")
  sql = sql .. ' where ' .. table.concat(where, " ")
  local ret, _ = self.__records:Execute(sql)
  return ret
end

function Record.Delete(self)
  local values = rawget(self, '__values')
  local sql = "delete from `" .. self.__records.__table .. "`"
  local primary = self.__records.__primary
  local where = nil
  local primaryValue = primary ~= '' and rawget(values, primary)
  if primaryValue then
    where = self.__records:Where(primary, primaryValue)
  else
    local first = true
    for k, v in pairs(values) do
      if first then
        first = false
        where = self.__records:Where(k, v)
      else
        where:Same(k, v)
      end
    end
  end
  if where == nil then return false end
  sql = sql .. ' where ' .. table.concat(where, " ")
  local ret, _ = self.__records:Execute(sql)
  return ret
end

local Records = {
  __fields = {},
  __primary = '',
  __table = '',
  Serial = 0,
  Int = 1,
  AutoInt = 2,
  BigInt = 3,
  AutoBigInt = 4,
  Float = 5,
  Double = 6,
  TinyText = 7,
  Text = 8,
  MediumText = 9,
  LongText = 10,
  Blob = 11,
  Date = 12,
  TimeStamp = 13
}

function Records.__call(self)
  rawset(self, '__history', {})
end

function Records.__index(t, k)
  if type(k) == 'number' and t ~= Record then
    local ret, records = t:FetchOne(k)
    if ret > 0 then
      return records[1]
    else
      return nil
    end
  end
end

function Records.Table(self)
  if self == Records then
    error('Illegal parameters')
  end
  if self.__table == '' then
    error("table is empty")
  end
  if type(self.__database) ~= 'table' then
    error('database is empty')
  end
  self = Light(self):Extends(Records)
  local sql = "create table if not exists `" .. self.__table .. "` (\n"
  local fields = {}
  for k, v in pairs(self.__fields) do
    table.insert(fields, k .. ' ' .. self.__database.TypeName(v))
  end
  if #fields == 0 then
    error("fields are empty")
  end
  if self.__primary ~= '' then
    table.insert(fields, 'primary key (' .. self.__primary .. ')')
  end
  sql = sql .. table.concat(fields, ",\n") .. "\n);"
  local ret, rows = self.__database:Execute(sql)
  if ret == -1 then
    error('table `' .. self.__table .. '` create failed')
  end
  rawset(self, '__history', {})
  return self
end

function Records.Where(self, k, v)
  local database = self.__database
  if database == nil then return nil end
  local where = Light(Where):New(database)
  return where:Same(k, v)
end

function Records.WhereGreater(self, k, v)
  local database = self.__database
  if database == nil then return nil end
  local where = Light(Where):New(database)
  return where:Greater(k, v)
end

function Records.WhereGreaterEqual(self, k, v)
  local database = self.__database
  if database == nil then return nil end
  local where = Light(Where):New(database)
  return where:GreaterEqual(k, v)
end

function Records.WhereLess(self, k, v)
  local database = self.__database
  if database == nil then return nil end
  local where = Light(Where):New(database)
  return where:Less(k, v)
end

function Records.WhereLessEqual(self, k, v)
  local database = self.__database
  if database == nil then return nil end
  local where = Light(Where):New(database)
  return where:LessEqual(k, v)
end

function Records.Fetch(self, limit, order)
  local sql = "select * from `" .. self.__table .. "`"
  if order ~= nil then
    if type(order) == 'string' then
      sql = sql .. ' order by `' .. order .. '`'
    elseif type(order) == 'table' then
      sql = sql .. ' order by `' .. order[1] .. (order[2] and '` asc' or '` desc')
    elseif type(order) == 'boolean' and self.__primary ~= '' then
      sql = sql .. ' order by `' .. self.__primary .. (order and '` asc' or '` desc')
    end
  end
  if limit ~= nil then
    if type(limit) == 'number' then
      sql = sql .. ' limit ' .. limit
    elseif type(limit) == 'table' then
      sql = sql .. ' limit ' .. limit[1] .. ', ' .. limit[2]
    end
  end
  sql = sql .. ";"
  local ret, rows = self:Execute(sql)
  local records = {}
  if ret > 0 then
    for i = 1, #rows do
      local record = self:Create()
      for k, v in pairs(rows[i]) do
        record[k] = v
      end
      table.insert(records, record)
    end
  end
  return ret, records
end

function Records.FetchOne(self, index)
  if type(index) ~= 'number' then index = 0 end
  return self:Fetch({index, 1})
end

function Records.FetchPage(self, pageIndex, pageSize)
  if type(pageIndex) ~= 'number' then pageIndex = 0 end
  if type(pageSize) ~= 'number' then pageSize = 20 end
  return self:Fetch({pageIndex * pageSize, pageSize})
end

function Records.Find(self, where, limit, order)
  local sql = "select * from `" .. self.__table .. "`"
  if type(where) == 'table' then
    if Light(where):Is(Where) == false then
      error('type `Where` not match')
    end
    sql = sql .. ' where ' .. table.concat(where, " ")
  end
  if order ~= nil then
    if type(order) == 'string' then
      sql = sql .. ' order by `' .. order .. '`'
    elseif type(order) == 'table' then
      sql = sql .. ' order by `' .. order[1] .. (order[2] and '` asc' or '` desc')
    elseif type(order) == 'boolean' and self.__primary ~= '' then
      sql = sql .. ' order by `' .. self.__primary .. (order and '` asc' or '` desc')
    end
  end
  if limit ~= nil then
    if type(limit) == 'number' then
      sql = sql .. ' limit ' .. limit
    elseif type(limit) == 'table' then
      sql = sql .. ' limit ' .. limit[1] .. ', ' .. limit[2]
    end
  end
  sql = sql .. ";"
  local ret, rows = self:Execute(sql)
  local records = {}
  if ret > 0 then
    for i = 1, #rows do
      local record = self:Create()
      for k, v in pairs(rows[i]) do
        record[k] = v
      end
      table.insert(records, record)
    end
  end
  return ret, records
end

function Records.FindOne(self, where)
  return self:Find(where, 1)
end

function Records.Begin(self)
  local sql = "begin;"
  local ret, _ = self:Execute(sql)
  return ret
end

function Records.Commit(self)
  local sql = "commit;"
  local ret, _ = self:Execute(sql)
  return ret
end

function Records.RollBack(self)
  local sql = "rollback;"
  local ret, _ = self:Execute(sql)
  return ret
end

local emptyRows = {}

function Records.Execute(self, sql)
  local history = rawget(self, '__history')
  if history == nil then
    return - 1, emptyRows
  end
  table.insert(history, sql)
  return self.__database:Execute(sql)
end

function Records.Create(self)
  if self == Record then error('Illegal parameters') end
  return Light(Record):New(self)
end

function Records.Delete(self, where)
  if self == Record then error('Illegal parameters') end
  local sql = "delete from `" .. self.__table .. "`"
  if type(where) ~= 'table' then return - 1 end
  if not Light(where):Is(Where) then return - 1 end
  sql = sql .. ' where ' .. table.concat(where, " ")
  local ret, _ = self:Execute(sql)
  return ret
end

function Records.Count(self)
  if self == Record then error('Illegal parameters') end
  local sql = "select count(*) from `" .. self.__table .. "`"
  local ret, rows = self:Execute(sql)
  if ret > 0 then
    return rows[1]["count(*)"]
  end
  return 0
end

function Records.PageCount(self, pageSize)
  local count = self:Count()
  return math.floor(count / pageSize) + 1
end

return Records
)LUA";

// ==================== luaopen_Light_Record ====================
// 还原自 sub_1800B03D0 — 加载嵌入 Lua ORM 脚本

int luaopen_Light_Record(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Record");
    lua_rawget(L, -2);

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Record");

        // 加载并执行嵌入 Lua 脚本 (原始通过 sub_180002D50 解密)
        if (!luaL_loadstring(L, s_RecordLuaScript) && !lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_rawset(L, -3);
            lua_pushstring(L, "Record");
            lua_rawget(L, -2);
        } else {
            lua_error(L);
        }
    }

    lua_remove(L, -2);
    return 1;
}
