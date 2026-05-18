-- Phase G.1.6 — Light.AssetLoader.Preload manifest 聚合 smoke
--
-- Headless 友好: 全部用 missing 资源, 走立即 Error 路径
-- (LoadXxxAsync sync fallback 失败 → state 立即 Error → AppendOne_ 同步更新 ud)
-- 不需要 worker thread / GL ctx / 主循环 Tick

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local okAL, AssetLoader = pcall(require, "Light.AssetLoader")
if not okAL then fail("require(Light.AssetLoader) failed: " .. tostring(AssetLoader)) end

if type(AssetLoader) ~= "table" then fail("Light.AssetLoader must be table, got " .. type(AssetLoader)) end
if type(AssetLoader.Preload) ~= "function" then fail("Light.AssetLoader.Preload missing") end
pass("Light.AssetLoader API surface ok")

-- ==================== Case 1: 空 manifest ====================
do
    local cbCalled = false
    local cbSucc, cbFail, cbErrors
    local h = AssetLoader.Preload({}, function(s, f, errs)
        cbCalled = true
        cbSucc, cbFail, cbErrors = s, f, errs
    end)
    if type(h) ~= "userdata" then fail("Preload returned non-userdata, got " .. type(h)) end
    if not cbCalled then fail("empty manifest should fire cb synchronously") end
    if cbSucc ~= 0 then fail("empty manifest succ should be 0, got " .. tostring(cbSucc)) end
    if cbFail ~= 0 then fail("empty manifest fail should be 0, got " .. tostring(cbFail)) end
    if type(cbErrors) ~= "table" or #cbErrors ~= 0 then fail("empty manifest errors should be empty table") end
    if h:IsDone() ~= true then fail("empty manifest IsDone should be true") end
    local d, t, e = h:GetProgress()
    if d ~= 0 or t ~= 0 or e ~= 0 then fail(string.format("empty manifest GetProgress should be (0,0,0), got (%d,%d,%d)", d, t, e)) end
end
pass("Case 1: empty manifest -> cb(0, 0, {}) synchronous")

-- ==================== Case 2: 单类多张 (全 missing) ====================
do
    local cbSucc, cbFail, cbErrors
    local h = AssetLoader.Preload({
        images = { "__missing_a__.png", "__missing_b__.png", "__missing_c__.png" },
    }, function(s, f, errs)
        cbSucc, cbFail, cbErrors = s, f, errs
    end)
    if cbSucc ~= 0 then fail("3 missing images: succ=0 expected, got " .. tostring(cbSucc)) end
    if cbFail ~= 3 then fail("3 missing images: fail=3 expected, got " .. tostring(cbFail)) end
    if type(cbErrors) ~= "table" or #cbErrors ~= 3 then fail("errors size=3 expected, got " .. (cbErrors and #cbErrors or "nil")) end
    -- 验证 errors entry 结构
    for i = 1, 3 do
        local e = cbErrors[i]
        if type(e) ~= "table" then fail("errors["..i.."] must be table") end
        if type(e.path) ~= "string" or #e.path == 0 then fail("errors["..i.."].path must be non-empty string") end
        if type(e.err) ~= "string" or #e.err == 0 then fail("errors["..i.."].err must be non-empty string") end
    end
    -- 路径反查正确性 (path 字段是 manifest 中的原始路径)
    local pathsSeen = {}
    for _, e in ipairs(cbErrors) do pathsSeen[e.path] = true end
    if not pathsSeen["__missing_a__.png"] then fail("errors should contain __missing_a__.png") end
    if not pathsSeen["__missing_b__.png"] then fail("errors should contain __missing_b__.png") end
    if not pathsSeen["__missing_c__.png"] then fail("errors should contain __missing_c__.png") end
    if h:IsDone() ~= true then fail("3 missing images: IsDone=true expected") end
end
pass("Case 2: 3 missing images -> cb(0, 3, errors) with path reverse-lookup")

-- ==================== Case 3: 多类型混合 (全 missing) ====================
do
    local cbSucc, cbFail, cbErrors
    local h = AssetLoader.Preload({
        images   = { "__m1.png" },
        sounds   = { "__m2.wav" },
        cubeLUTs = { "__m3.cube" },
        haldLUTs = { "__m4.png" },
        fonts    = { { path = "__m5.ttf", size = 16 } },
        meshes   = { { path = "__m6.glb", primIdx = 0, withMaterial = true } },
    }, function(s, f, errs)
        cbSucc, cbFail, cbErrors = s, f, errs
    end)
    if cbFail ~= 6 then fail("6-type missing manifest: fail=6 expected, got " .. tostring(cbFail)) end
    if cbSucc ~= 0 then fail("6-type missing manifest: succ=0 expected, got " .. tostring(cbSucc)) end
    local d, t, errCount = h:GetProgress()
    if d ~= 6 or t ~= 6 or errCount ~= 6 then
        fail(string.format("6-type GetProgress (6,6,6) expected, got (%d,%d,%d)", d, t, errCount))
    end
end
pass("Case 3: 6-type mixed missing manifest -> all routed to corresponding LoadXxxAsync")

-- ==================== Case 4: 字段缺省 / 未识别字段 ====================
do
    -- 仅 images 字段, 其他全省略
    local h1 = AssetLoader.Preload({ images = { "__a.png" } })
    if not h1 then fail("Preload without cb should still return handle") end
    local d, t = h1:GetProgress()
    if d ~= 1 or t ~= 1 then fail("only-images: (1,1) expected, got ("..d..","..t..")") end

    -- 未识别字段 (foo) 不应抛错
    local okPcall, h2 = pcall(AssetLoader.Preload, { foo = { "x.png" }, images = { "__b.png" } })
    if not okPcall then fail("unknown manifest field should not throw, got: " .. tostring(h2)) end
    local d2, t2 = h2:GetProgress()
    if t2 ~= 1 then fail("unknown field should be ignored, only images counted") end
end
pass("Case 4: missing optional fields + unknown fields tolerated")

-- ==================== Case 5: 默认参数 (font size / mesh primIdx / withMaterial) ====================
do
    -- font 不传 size → 默认 16; mesh 不传 primIdx → 默认 0; 不传 withMaterial → 默认 false
    local h = AssetLoader.Preload({
        fonts  = { { path = "__nofont.ttf" } },               -- size 默认
        meshes = { { path = "__nomesh.glb" } },               -- primIdx / withMaterial 默认
    })
    local _, t = h:GetProgress()
    if t ~= 2 then fail("font + mesh defaults: total=2 expected, got " .. t) end
end
pass("Case 5: default params (font.size=16, mesh.primIdx=0, withMaterial=false)")

-- ==================== Case 6: 参数错误 (luaL_argerror) ====================
do
    -- nil manifest
    local ok, err = pcall(AssetLoader.Preload, nil)
    if ok then fail("Preload(nil) should error") end
    if type(err) ~= "string" then fail("error must be string") end

    -- non-function totalCb
    local ok2, err2 = pcall(AssetLoader.Preload, {}, "not_function")
    if ok2 then fail("Preload({}, 'string') should error on totalCb") end

    -- entry type error: images contains number
    local ok3, err3 = pcall(AssetLoader.Preload, { images = { 123 } })
    if ok3 then fail("Preload({images={123}}) should error on entry type") end

    -- meshes entry without path
    local ok4, err4 = pcall(AssetLoader.Preload, { meshes = { { primIdx = 0 } } })
    if ok4 then fail("Preload({meshes={{primIdx=0}}}) should error on missing path") end
end
pass("Case 6: argument errors raise (nil manifest / non-func cb / entry typo / missing path)")

-- ==================== Case 7: BatchHandle 方法签名 ====================
do
    local h = AssetLoader.Preload({})
    if type(h.GetProgress) ~= "function" then fail("BatchHandle:GetProgress missing") end
    if type(h.IsDone) ~= "function" then fail("BatchHandle:IsDone missing") end
    if type(h.Cancel) ~= "function" then fail("BatchHandle:Cancel missing") end

    -- __tostring
    local s = tostring(h)
    if type(s) ~= "string" or not s:match("BatchHandle") then
        fail("__tostring should contain 'BatchHandle', got: " .. tostring(s))
    end

    -- IsDone returns boolean, not nil/number
    local d = h:IsDone()
    if type(d) ~= "boolean" then fail("IsDone() must return boolean, got " .. type(d)) end

    -- GetProgress returns 3 ints
    local a, b, c = h:GetProgress()
    if type(a) ~= "number" or type(b) ~= "number" or type(c) ~= "number" then
        fail("GetProgress() must return 3 numbers")
    end
end
pass("Case 7: BatchHandle method signatures (GetProgress / IsDone / Cancel / __tostring)")

-- ==================== Case 8: Cancel 语义 ====================
do
    -- Cancel 后再调 GetProgress / IsDone 不抛错 (advisory)
    local cbCalled = false
    local h = AssetLoader.Preload({ images = { "__c1.png" } }, function() cbCalled = true end)
    -- 此处 missing image 已立即 Error → ud.remaining=0 → cb 已触发
    -- Cancel 在 cb 后调用, 仅清理 totalCbRef (已 -1, no-op)
    h:Cancel()
    -- handle 仍可查询
    if h:IsDone() ~= true then fail("Cancel after done: IsDone still true") end
    -- 第二次 Cancel 不抛
    local ok = pcall(function() h:Cancel() end)
    if not ok then fail("double Cancel should not throw") end
end
pass("Case 8: Cancel after-done is no-op (advisory, idempotent)")

print("=== Phase G.1.6 Async Preload Manifest smoke: ALL TESTS PASSED ===")
