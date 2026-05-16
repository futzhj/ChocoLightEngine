-- scripts/smoke/screenshot.lua  Phase F.0.11 API surface
local Gfx = Light.Graphics
local pass = 0; local fail = 0
local function p(c,m) if c then pass=pass+1 print("PASS "..m) else fail=fail+1 print("FAIL "..m) end end

-- API surface
p(type(Gfx.Screenshot)=='function',        'Screenshot exists')
p(type(Gfx.ScreenshotRegion)=='function',  'ScreenshotRegion exists')
p(type(Gfx.RecordPNGSequence)=='function', 'RecordPNGSequence exists')
p(type(Gfx.StopRecord)=='function',        'StopRecord exists')
p(type(Gfx.IsRecording)=='function',       'IsRecording exists')
-- F.0.11.2: PBO 异步 readback
p(type(Gfx.SetRecordAsync)=='function',    'SetRecordAsync exists')
p(type(Gfx.IsRecordAsync)=='function',     'IsRecordAsync exists')
-- F.0.11.4: HDR .hdr 截图
p(type(Gfx.ScreenshotHDR)=='function',     'ScreenshotHDR exists')

-- Screenshot headless (viewport=0 → nil+err)
local ok,r,e = pcall(Gfx.Screenshot,"out.png"); p(ok,'Screenshot no raise')
if r==nil then p(type(e)=='string','Screenshot headless nil+err') end

-- ScreenshotRegion bad w
local _,rv,re = pcall(Gfx.ScreenshotRegion,"out.png",0,0,-1,100)
p(rv==nil and type(re)=='string','ScreenshotRegion w=-1 → nil+err')

-- IsRecording initial
local _,a,n = pcall(Gfx.IsRecording); p(a==false,'IsRecording init active=false'); p(n==0,'IsRecording init count=0')

-- RecordPNGSequence max<0
local _,rv2,re2 = pcall(Gfx.RecordPNGSequence,'f/',-1); p(rv2==nil,'RecordPNGSequence(-1) nil+err')

-- F.0.11.1: frame_skip < 1 验证 (新参数边界)
local _,rv3,re3 = pcall(Gfx.RecordPNGSequence,'f/',10,0)
p(rv3==nil and type(re3)=='string','RecordPNGSequence frame_skip=0 → nil+err')
local _,rv4,re4 = pcall(Gfx.RecordPNGSequence,'f/',10,-2)
p(rv4==nil and type(re4)=='string','RecordPNGSequence frame_skip=-2 → nil+err')

-- F.0.11.1: frame_skip 默认值 (向后兼容, 不传第 3 参数)
local _,rs0 = pcall(Gfx.RecordPNGSequence,'f/',5)
p(rs0==true,'RecordPNGSequence(dir,5) backwards-compat (default frame_skip=1)')
Gfx.StopRecord()

-- F.0.11.1: frame_skip 显式传 3 (隔 3 帧写)
local _,rs1 = pcall(Gfx.RecordPNGSequence,'f/',5,3)
p(rs1==true,'RecordPNGSequence(dir,5,3) frame_skip=3 accepted')
Gfx.StopRecord()

-- F.0.11.2: SetRecordAsync 切换 (idle 时允许)
p(Gfx.IsRecordAsync()==false,'IsRecordAsync default = false')
local _,rsa1 = pcall(Gfx.SetRecordAsync,true);  p(rsa1==true,'SetRecordAsync(true) idle ok')
p(Gfx.IsRecordAsync()==true, 'IsRecordAsync after SetRecordAsync(true) = true')
local _,rsa2 = pcall(Gfx.SetRecordAsync,false); p(rsa2==true,'SetRecordAsync(false) idle ok')
p(Gfx.IsRecordAsync()==false,'IsRecordAsync after SetRecordAsync(false) = false')

-- F.0.11.2: SetRecordAsync 录屏中切换被拒
Gfx.RecordPNGSequence('f/',1)
local _,rsa3,esa3 = pcall(Gfx.SetRecordAsync,true)
p(rsa3==nil and type(esa3)=='string','SetRecordAsync during recording → nil+err')
Gfx.StopRecord()

-- F.0.11.4: ScreenshotHDR 在 HDR 未启用时 → nil+err
local _,hr1,he1 = pcall(Gfx.ScreenshotHDR,'out.hdr')
p(hr1==nil and type(he1)=='string','ScreenshotHDR no HDR → nil+err')

-- RecordPNGSequence + stop
local _,rs = pcall(Gfx.RecordPNGSequence,'f/',3); p(rs==true,'RecordPNGSequence(3) true')
local _,a2 = pcall(Gfx.IsRecording); p(a2==true,'IsRecording active after start')
local _,ns = pcall(Gfx.StopRecord); p(type(ns)=='number','StopRecord returns number')
p(ns==0,'StopRecord headless count=0')
local _,a3 = pcall(Gfx.IsRecording); p(a3==false,'IsRecording inactive after stop')

print(string.format("screenshot smoke: %d pass / %d fail", pass, fail))
if fail>0 then error("screenshot smoke FAIL") end
