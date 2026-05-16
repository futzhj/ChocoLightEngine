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

-- RecordPNGSequence + stop
local _,rs = pcall(Gfx.RecordPNGSequence,'f/',3); p(rs==true,'RecordPNGSequence(3) true')
local _,a2 = pcall(Gfx.IsRecording); p(a2==true,'IsRecording active after start')
local _,ns = pcall(Gfx.StopRecord); p(type(ns)=='number','StopRecord returns number')
p(ns==0,'StopRecord headless count=0')
local _,a3 = pcall(Gfx.IsRecording); p(a3==false,'IsRecording inactive after stop')

print(string.format("screenshot smoke: %d pass / %d fail", pass, fail))
if fail>0 then error("screenshot smoke FAIL") end
