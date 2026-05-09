-- ChocoLight Sample: Light.Process + Light.MessageBox
--
-- 演示子进程派生 (跨平台) 与系统消息框.
-- 注: MessageBox 需要 GUI 环境; CI/headless 上跳过.

local Process = require 'Light.Process'
local MB      = require 'Light.MessageBox'
local Sys     = require 'Light.System'

print("==== Light.Process ====")

-- 跨平台 echo: Windows 用 cmd, *nix 用 sh
local platform = Sys.GetPlatform()
local args
if platform == "Windows" then
    args = { "cmd", "/c", "echo hello from chocolight" }
else
    args = { "sh", "-c", "echo hello from chocolight" }
end

local proc, err = Process.Create(args, { stdout = "redirect" })
if proc then
    -- 等子进程结束 (block=true)
    local code, werr = Process.Wait(proc, true)
    print("exit code:", code, werr or "")

    -- 读 stdout
    local out = Process.Read(proc, "stdout") or ""
    print("stdout:   ", out:gsub("[\r\n]+$", ""))

    Process.Destroy(proc)
else
    print("Process.Create err:", err)
end

print("\n==== Light.MessageBox ====")

-- MessageBox 在 CI/headless 上会阻塞或失败.
-- 这里只演示 API, 默认不真正弹窗.
local DEMO_SHOW = false  -- 设为 true 在桌面环境真实弹窗

if DEMO_SHOW then
    MB.ShowSimple("info", "ChocoLight Demo", "Hello from MessageBox!")

    local btn, mberr = MB.Show("warning",
        "确认操作",
        "你要继续吗?",
        { "继续", "取消", "稍后" })
    if btn then
        print(string.format("user clicked button #%d", btn))
    else
        print("MB.Show err:", mberr)
    end
else
    print("(MessageBox demo skipped; set DEMO_SHOW=true on a desktop)")
end

print("\ndemo_process_messagebox ok")
