---@diagnostic disable: undefined-field
-- ============================================================================
-- ChocoLight Phase G.1.5 — Async GLTF Material Demo (callback-model)
-- ============================================================================
-- 演示 Mesh.LoadGLTFAsync 异步加载 glTF + PBR material + embedded textures.
-- 用 fixture (test_box_textured.glb, 4 顶点 quad + 1×1 红色 PNG) 验证完整管线.
--
-- 画面:
--   中心: 加载完成的 mesh + material 渲染 (旋转, 朝向相机)
--   底部: HUD 实时显示加载状态 / 主线程帧时间 P95 / texture id 列表
--
-- 控制:
--   R   : 重新发起异步加载
--   M   : 切换 with_material true/false (重新加载生效)
--   F   : 切换 Future poll 风格 vs Callback 风格
--   ESC : 退出
-- ============================================================================

local function safe_require(n)
    local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil
end
local UI   = safe_require('Light.UI')
local Gfx  = safe_require('Light.Graphics')
local Time = safe_require('Light.Time')

if not Gfx then print('[demo_gltf_async] Light.Graphics 不可用'); print('demo_gltf_async ok (no graphics)'); return end
local Mesh = Gfx.Mesh
if type(Mesh) ~= 'table' or type(Mesh.LoadGLTFAsync) ~= 'function' then
    print('[demo_gltf_async] Mesh.LoadGLTFAsync 不可用 (need Phase G.1.5)')
    print('demo_gltf_async ok (legacy build)'); return
end

local FIXTURE = 'scripts/smoke/assets_g1_5/test_box_textured.glb'

print('==== ChocoLight Phase G.1.5 Async GLTF Demo (callback-model) ====')
print('[demo_gltf_async] Backend  = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_gltf_async] Fixture  = ' .. FIXTURE)

-- Headless 探测: 测 API 表面后退出
if not UI or not UI.Window then
    print('[demo_gltf_async] UI.Window 不可用, 仅 API 探测')
    print('  Mesh.LoadGLTFAsync = ' .. tostring(Mesh.LoadGLTFAsync))
    print('  Mesh.LoadGLTF      = ' .. tostring(Mesh.LoadGLTF))
    print('demo_gltf_async ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_gltf_async] Light global 不可用')
    print('demo_gltf_async ok (no Light global)'); return
end

-- ============================================================================
-- 常量 + 工具
-- ============================================================================
local WIN_W, WIN_H  = 960, 540
local FRAME_BUDGET  = 16.7   -- ms

-- 帧时间统计 ring buffer (固定容量 P95 计算)
local FRAME_RING_CAP = 240
local frame_ring = {}
local frame_ring_head = 1
local function record_frame_ms(ms)
    frame_ring[frame_ring_head] = ms
    frame_ring_head = (frame_ring_head % FRAME_RING_CAP) + 1
end
local function frame_p95()
    local n = #frame_ring
    if n < 10 then return 0.0 end
    local sorted = {}
    for i = 1, n do sorted[i] = frame_ring[i] end
    table.sort(sorted)
    local idx = math.max(1, math.floor(n * 0.95))
    return sorted[idx]
end

-- ============================================================================
-- Demo 状态
-- ============================================================================
local Demo = Light(Light.UI.Window):New()

local state = {
    future       = nil,       -- 当前未完成的 Future (Future poll 风格)
    mesh         = nil,       -- 加载完成的 mesh userdata
    material     = nil,       -- 加载完成的 material userdata (with_material=true 才有)
    err          = nil,       -- 错误信息
    angle        = 0.0,
    with_mat     = true,      -- M 键切换
    use_callback = false,     -- F 键切换
    last_dispatch_ms = 0.0,   -- 发起加载的时间戳
    ready_ms     = 0.0,       -- 加载完成的时间戳
}

local function clear_loaded()
    state.mesh = nil; state.material = nil; state.err = nil; state.future = nil
end

local function dispatch_load()
    clear_loaded()
    state.last_dispatch_ms = Time and Time.GetTicksMS and Time.GetTicksMS() or 0.0
    if state.use_callback then
        -- Callback 风格: cb(mesh, material, err) 或 cb(mesh, err)
        if state.with_mat then
            Mesh.LoadGLTFAsync(FIXTURE, 0, true, function(mesh, mat, err)
                state.mesh = mesh; state.material = mat; state.err = err
                state.ready_ms = Time and Time.GetTicksMS and Time.GetTicksMS() or 0.0
            end)
        else
            Mesh.LoadGLTFAsync(FIXTURE, 0, function(mesh, err)
                state.mesh = mesh; state.material = nil; state.err = err
                state.ready_ms = Time and Time.GetTicksMS and Time.GetTicksMS() or 0.0
            end)
        end
    else
        -- Future poll 风格
        state.future = Mesh.LoadGLTFAsync(FIXTURE, 0, state.with_mat)
    end
    print(string.format('[demo_gltf_async] dispatch: with_mat=%s style=%s',
        tostring(state.with_mat), state.use_callback and 'callback' or 'future'))
end

-- ============================================================================
-- Window 生命周期
-- ============================================================================
function Demo:OnOpen()
    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
    end
    dispatch_load()
end

function Demo:Update(dt)
    state.angle = state.angle + dt * 1.5

    -- Future poll: 每帧检查
    if state.future then
        if state.future:IsReady() then
            if state.with_mat then
                state.mesh, state.material = state.future:Get()
            else
                state.mesh = state.future:Get()
            end
            state.err = nil
            state.future = nil
            state.ready_ms = Time and Time.GetTicksMS and Time.GetTicksMS() or 0.0
            print(string.format('[demo_gltf_async] LOADED mesh=%s mat=%s in %dms',
                tostring(state.mesh), tostring(state.material),
                state.ready_ms - state.last_dispatch_ms))
        elseif state.future:IsError() then
            local _, err = state.future:Get()
            state.err = err or 'unknown error'
            state.future = nil
            print(string.format('[demo_gltf_async] ERROR: %s', tostring(state.err)))
        end
    end
end

-- 键位监听 — OnKey(key, scancode, action, mods); action == 1 表示按下
function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()             -- ESC
    elseif key == string.byte('R') then dispatch_load()
    elseif key == string.byte('M') then
        state.with_mat = not state.with_mat
        dispatch_load()
    elseif key == string.byte('F') then
        state.use_callback = not state.use_callback
        dispatch_load()
    end
end

-- ============================================================================
-- 渲染
-- ============================================================================
function Demo:Draw()
    local frame_start = Time and Time.GetTicksMS and Time.GetTicksMS() or 0.0

    if type(Gfx.SetCamera) == 'function' then Gfx.SetCamera(0.0, 1.5, 4.0, 0.0, 0.0, 0.0) end

    -- 中心 mesh: 加载完成才渲染
    if state.mesh then
        Gfx.Push()
        Gfx.Rotate(math.deg(state.angle), 0, 1, 0)
        Gfx.Scale(1.5, 1.5, 1.5)
        Gfx.SetColor(1, 1, 1, 1)
        if state.material then
            state.mesh:Draw(state.material)
        else
            state.mesh:Draw(0)
        end
        Gfx.Pop()
    end

    -- HUD
    local lines = {}
    table.insert(lines, '== Phase G.1.5 Async GLTF Material Demo ==')
    table.insert(lines, string.format('R: re-dispatch | M: with_mat=%s | F: style=%s | ESC: quit',
        tostring(state.with_mat), state.use_callback and 'callback' or 'future'))
    if state.err then
        table.insert(lines, string.format('Status: ERROR (%s)', tostring(state.err)))
    elseif state.future then
        table.insert(lines, string.format('Status: LOADING (%dms elapsed)',
            (Time and Time.GetTicksMS and Time.GetTicksMS() - state.last_dispatch_ms) or 0))
    elseif state.mesh then
        local elapsed = state.ready_ms - state.last_dispatch_ms
        table.insert(lines, string.format('Status: READY (loaded in %dms)', elapsed))
        if state.material then
            table.insert(lines, '  material: userdata (5 PBR textures uploaded)')
        else
            table.insert(lines, '  material: nil (with_mat=false)')
        end
    else
        table.insert(lines, 'Status: idle')
    end
    table.insert(lines, string.format('Frame P95: %.2f ms (budget=%.1f ms)', frame_p95(), FRAME_BUDGET))

    if Gfx.Print then
        for i, line in ipairs(lines) do
            Gfx.SetColor(1, 1, 1, 1)
            Gfx.Print(line, 12, 12 + (i - 1) * 18, 0)
        end
    end

    -- 帧时间记录
    local frame_end = Time and Time.GetTicksMS and Time.GetTicksMS() or 0.0
    record_frame_ms(frame_end - frame_start)
end

function Demo:OnClose()
    print(string.format('[demo_gltf_async] exit, P95=%.2fms', frame_p95()))
    clear_loaded()
end

-- ============================================================================
-- 启动 + 主循环
-- ============================================================================
Demo:Open(WIN_W, WIN_H, 'demo_gltf_async (Phase G.1.5)')
while Light.UI.Loop() do Light.UI.Resume() end
print('demo_gltf_async ok')
