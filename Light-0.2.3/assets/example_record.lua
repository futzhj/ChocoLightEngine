
-- 创建用户表
local Users = Light.Record.Table({
  __database = Light(Light.DB.SQLite):New('users.db'), -- 打开 users 数据库
  __fields = {
    id = Light.Record.AutoInt,
    name = Light.Record.Text,
    age = Light.Record.Int
  },
  __primary = 'id',
  __table = 'users'
})

-- 教程：增

local xiaoMing = Users:Create()
xiaoMing.name = 'xiaoMing'
xiaoMing.age = 10
xiaoMing:Insert()

local xiaoQiang = Users:Create()
xiaoQiang.name = 'xiaoQiang'
xiaoQiang.age = 20
xiaoQiang:Insert()

-- 教程：改 & 查

-- 查找 1 条数据，然后 【修改】
local ret, xiaoQiangFound = Users:FindOne(Users:Where('name', 'xiaoQiang'))

-- 智障教程：ret < 0 出错，ret == 0 没找到, ret > 0 找到 N 条记录
if ret > 0 then
  xiaoQiang = xiaoQiangFound[1] -- 拿第一条，反正也就只有一条
  xiaoQiang.age = 99
  xiaoQiang:Update()
end

-- 教程：删

-- 查找 1 条数据，然后 【删除】
local ret, xiaoMingFound = Users:FindOne(Users:Where('name', 'xiaoMing'))

if ret > 0 then
  xiaoMing = xiaoMingFound[1] -- 拿第一条，反正也就只有一条
  xiaoMing:Delete() -- 删掉 小明
end

-- 教程：批量删除
Users:Delete(Users:WhereLess('age', 10)) -- 删除 10 岁以下的

-- 教程：看看还删到剩下几条
print(Users:Count())

-- 教程：遍历整张数据表
-- 记得从 0 开始的！到 最后一个 - 1 (因为 Lua 的缺陷，它迭代包含了最后一个)
for i = 0, Users:Count() - 1 do
  local who = Users[i]
  print(who.name) -- 逐个取出来，打印名字
end
