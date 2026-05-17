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
-- F.0.11.5: EXR HDR 截图 (OpenEXR half/float)
p(type(Gfx.ScreenshotEXR)=='function',     'ScreenshotEXR exists')
-- F.0.11.6: MP4 录屏 (FFmpeg H.264)
p(type(Gfx.RecordMP4)=='function',         'RecordMP4 exists')
p(type(Gfx.GetRecordMode)=='function',     'GetRecordMode exists')

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

-- F.0.11.5: ScreenshotEXR 在 HDR 未启用时 → nil+err
local _,exr1,exr1e = pcall(Gfx.ScreenshotEXR,'out.exr')
p(exr1==nil and type(exr1e)=='string','ScreenshotEXR no HDR → nil+err')

-- F.0.11.5: ScreenshotEXR 非法 bit_depth (期望 16 或 32)
local _,exr2,exr2e = pcall(Gfx.ScreenshotEXR,'out.exr', { bit_depth = 24 })
p(exr2==nil and type(exr2e)=='string' and exr2e:find('bit_depth'),
  'ScreenshotEXR bit_depth=24 → nil+err')

-- F.0.11.5: ScreenshotEXR 非法 compression
local _,exr3,exr3e = pcall(Gfx.ScreenshotEXR,'out.exr', { compression = 'lzma' })
p(exr3==nil and type(exr3e)=='string' and exr3e:find('compression'),
  'ScreenshotEXR compression=lzma → nil+err')

-- F.0.11.7: ScreenshotHDR 非法 instance_id (HDR 未启用 + invalid id 都返 nil+err)
-- 注意: HDR 未启用时 IsEnabled() 检查先失败, 测不到 instance_id 路径 → 跳过 invalid id check
-- 但可测 instance_id=999 时 luaL_checkinteger 不抛 (大整数合法), 后续 IsEnabled 失败
local _,h7a,h7ae = pcall(Gfx.ScreenshotHDR,'out.hdr', 999)
p(h7a==nil and type(h7ae)=='string',
  'ScreenshotHDR(path, 999) headless → nil+err (HDR not enabled)')

-- F.0.11.7: ScreenshotEXR 含 instance_id 字段
local _,e7a,e7ae = pcall(Gfx.ScreenshotEXR,'out.exr', { instance_id = 999 })
p(e7a==nil and type(e7ae)=='string',
  'ScreenshotEXR(path, {instance_id=999}) headless → nil+err')

-- F.0.11.6: RecordMP4 默认状态
p(Gfx.GetRecordMode()==0,                  'GetRecordMode default = 0 (PNG / idle)')

-- F.0.11.6: RecordMP4 非法 fps
local _,mp4a,mp4ae = pcall(Gfx.RecordMP4,'out.mp4', { fps = 0 })
p(mp4a==nil and type(mp4ae)=='string' and mp4ae:find('fps'),
  'RecordMP4(fps=0) → nil+err')

-- F.0.11.6: RecordMP4 非法 frame_skip
local _,mp4b,mp4be = pcall(Gfx.RecordMP4,'out.mp4', { frame_skip = 0 })
p(mp4b==nil and type(mp4be)=='string' and mp4be:find('frame_skip'),
  'RecordMP4(frame_skip=0) → nil+err')

-- F.0.11.6: RecordMP4 在 headless (无窗口) 下应失败 (window size 0x0)
local _,mp4c,mp4ce = pcall(Gfx.RecordMP4,'out.mp4')
p(mp4c==nil and type(mp4ce)=='string',
  'RecordMP4 headless → nil+err (no window / FFmpeg DLL)')

-- RecordPNGSequence + stop
local _,rs = pcall(Gfx.RecordPNGSequence,'f/',3); p(rs==true,'RecordPNGSequence(3) true')
local _,a2 = pcall(Gfx.IsRecording); p(a2==true,'IsRecording active after start')
local _,ns = pcall(Gfx.StopRecord); p(type(ns)=='number','StopRecord returns number')
p(ns==0,'StopRecord headless count=0')
local _,a3 = pcall(Gfx.IsRecording); p(a3==false,'IsRecording inactive after stop')

print(string.format("screenshot smoke: %d pass / %d fail", pass, fail))
if fail>0 then error("screenshot smoke FAIL") end
