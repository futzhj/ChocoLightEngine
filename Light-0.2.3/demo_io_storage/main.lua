-- ChocoLight Sample: Light.IO + Light.Storage
--
-- 演示文件 / 目录操作 + 跨平台用户数据路径.
-- 可在任何环境运行 (无 GUI 依赖).

local IO = require 'Light.IO'
local Storage = require 'Light.Storage'

print("==== Light.IO ====")
print("ExePath: ", IO.GetExePath())
print("CWD:     ", IO.GetCwd())
print("TempDir: ", IO.GetTempPath())

-- 创建临时目录 + 写入测试
local tmp = IO.GetTempPath()
local subdir = tmp .. "/chocolight_demo_io"
IO.MakeDir(subdir)
print("Created:  ", subdir, "exists=", IO.PathExists(subdir))

local fpath = subdir .. "/hello.txt"
local f = io.open(fpath, "w")
if f then
    f:write("hello chocolight\n")
    f:close()
    print("Wrote:    ", fpath, "isFile=", IO.IsFile(fpath))
end

-- 列目录
local list, err = IO.ListDir(subdir)
if list then
    print("ListDir:  ", subdir)
    for _, name in ipairs(list) do print("    " .. name) end
end

-- 清理
IO.RemoveFile(fpath)
IO.RemoveDir(subdir)
print("Cleanup:  ok")

print("\n==== Light.Storage ====")
print("BasePath: ", Storage.GetBasePath())
local user = Storage.GetUserPath("ChocoLight", "demo_io_storage")
print("UserPath: ", user)
-- UserPath 平台举例:
--   Windows: C:/Users/<U>/AppData/Roaming/ChocoLight/demo_io_storage/
--   macOS:   /Users/<U>/Library/Application Support/ChocoLight/demo_io_storage/
--   Linux:   /home/<U>/.local/share/ChocoLight/demo_io_storage/

print("\ndemo_io_storage ok")
