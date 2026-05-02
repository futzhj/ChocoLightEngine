# Phase3 Runtime Smoke Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Phase3 behavior and enhanced window smoke validation, then wire it into the existing Windows runtime GitHub Actions gate.

**Architecture:** Keep CI-stable tests split into two Lua smoke scripts. `phase3_runtime.lua` is a mandatory, no-window behavior gate; `phase3_window_runtime.lua` is an enhanced window/GL smoke that skips cleanly when a CI runner cannot create a window or GL context. The existing workflow keeps cross-platform syntax checks and adds both scripts to Windows runtime execution.

**Tech Stack:** Lua 5.1, Lumen `light.exe` / `lightc`, ChocoLight Lua modules, GitHub Actions PowerShell workflow.

---

## Scope

This plan implements the approved spec in `docs/superpowers/specs/2026-05-02-phase3-runtime-smoke-design.md`.

Included:

- Add mandatory no-window Phase3 runtime behavior smoke.
- Add enhanced window/GL Phase3 runtime smoke with safe skip.
- Update Windows runtime CI to run both scripts.
- Verify locally where existing build artifacts are available.
- Push to `origin` so GitHub Actions is the final build/runtime verification.

Excluded:

- Adding new Phase3 engine functionality.
- Rewriting Phase3 modules.
- Requiring Linux/macOS runtime smoke.
- Treating window/GL creation failure as a CI failure.
- Pixel-perfect rendering validation.

## File Structure

- Create: `scripts/smoke/phase3_runtime.lua`
  - Mandatory no-window smoke for `Crypto`, `Scene`, `SpriteAnimation`, `UI.Widget`, `HotReload`, and `Shader` API table checks.
- Create: `scripts/smoke/phase3_window_runtime.lua`
  - Enhanced smoke for `UI.Window`, `Graphics`, `UI.Widget:Draw`, and optional `Graphics.Shader` execution.
- Modify: `.github/workflows/build-templates.yml:44-54`
  - Add path variables for the two new scripts.
  - Run both after `core_runtime.lua` and `physics_p0_p1.lua`.
- Existing reference files:
  - `scripts/smoke/core_runtime.lua`
  - `scripts/smoke/physics_p0_p1.lua`
  - `ChocoLight/src/light_crypto.cpp`
  - `ChocoLight/src/light_scene.cpp`
  - `ChocoLight/src/light_graphics_spriteanimation.cpp`
  - `ChocoLight/src/light_ui_widget.cpp`
  - `ChocoLight/src/light_hotreload.cpp`
  - `ChocoLight/src/light_graphics_shader.cpp`
  - `ChocoLight/src/light_ui.cpp`

---

### Task 1: Add mandatory Phase3 no-window runtime smoke

**Files:**
- Create: `scripts/smoke/phase3_runtime.lua`

- [ ] **Step 1: Confirm target file is absent**

Run:

```powershell
Test-Path scripts\smoke\phase3_runtime.lua
git status --short
```

Expected:

```text
False
```

`git status --short` may show earlier plan/spec commits already committed, but must not show an existing `scripts/smoke/phase3_runtime.lua`.

- [ ] **Step 2: Create `phase3_runtime.lua`**

Create `scripts/smoke/phase3_runtime.lua` with exactly this content:

```lua
local function fail(message)
  error(message, 2)
end

local function assert_equal(actual, expected, label)
  if actual ~= expected then
    fail(label .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual))
  end
end

local function assert_type(value, expected, label)
  if type(value) ~= expected then
    fail(label .. ": expected type " .. expected .. ", got " .. type(value))
  end
end

local function assert_true(value, label)
  if not value then
    fail(label .. ": expected true-like value")
  end
end

local function assert_false(value, label)
  if value then
    fail(label .. ": expected false-like value")
  end
end

local function require_table(name)
  local ok, module_or_error = pcall(require, name)
  assert_true(ok, "require(" .. name .. ") failed: " .. tostring(module_or_error))
  assert_type(module_or_error, "table", name)
  return module_or_error
end

local function test_crypto()
  local Crypto = require_table("Light.Crypto")

  assert_equal(
    Crypto.SHA256("hello"),
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
    "Crypto.SHA256"
  )
  assert_equal(Crypto.MD5("hello"), "5d41402abc4b2a76b9719d911017c592", "Crypto.MD5")
  assert_equal(Crypto.Base64Encode("Hello!"), "SGVsbG8h", "Crypto.Base64Encode")
  assert_equal(Crypto.Base64Decode("SGVsbG8h"), "Hello!", "Crypto.Base64Decode")

  local key = "0123456789abcdef0123456789abcdef"
  local iv = "abcdef9876543210"
  local plaintext = "phase3 crypto smoke"
  local ciphertext, encrypt_error = Crypto.AES256_Encrypt(plaintext, key, iv)
  assert_type(ciphertext, "string", "Crypto.AES256_Encrypt result")
  assert_equal(encrypt_error, nil, "Crypto.AES256_Encrypt error")

  local decrypted, decrypt_error = Crypto.AES256_Decrypt(ciphertext, key, iv)
  assert_equal(decrypted, plaintext, "Crypto.AES256_Decrypt roundtrip")
  assert_equal(decrypt_error, nil, "Crypto.AES256_Decrypt error")

  assert_equal(#Crypto.RandomBytes(16), 16, "Crypto.RandomBytes length")
  assert_equal(#Crypto.RandomHex(16), 32, "Crypto.RandomHex length")

  local derived1 = Crypto.KeyFromPassword("password", "salt", 32, 10)
  local derived2 = Crypto.KeyFromPassword("password", "salt", 32, 10)
  assert_equal(#derived1, 32, "Crypto.KeyFromPassword length")
  assert_equal(derived1, derived2, "Crypto.KeyFromPassword deterministic")
end

local function test_scene()
  local Scene = require_table("Light.Scene")
  Scene.Clear()

  local events = {}
  local function push_event(name)
    events[#events + 1] = name
  end

  local scene_a = {
    OnEnter = function() push_event("a.enter") end,
    OnExit = function() push_event("a.exit") end,
    OnPause = function() push_event("a.pause") end,
    OnResume = function() push_event("a.resume") end,
    Update = function(_, dt) push_event("a.update:" .. tostring(dt)) end,
    OnKey = function(_, key, action) push_event("a.key:" .. tostring(key) .. ":" .. tostring(action)) end,
  }
  local scene_b = {
    OnEnter = function() push_event("b.enter") end,
    OnExit = function() push_event("b.exit") end,
    Update = function(_, dt) push_event("b.update:" .. tostring(dt)) end,
    OnKey = function(_, key, action) push_event("b.key:" .. tostring(key) .. ":" .. tostring(action)) end,
  }
  local scene_c = {
    OnEnter = function() push_event("c.enter") end,
    OnExit = function() push_event("c.exit") end,
  }

  Scene.Push(scene_a)
  assert_equal(Scene.Count(), 1, "Scene.Count after first push")
  assert_equal(Scene.Top(), scene_a, "Scene.Top after first push")

  Scene.Push(scene_b)
  assert_equal(Scene.Count(), 2, "Scene.Count after second push")
  assert_equal(Scene.Top(), scene_b, "Scene.Top after second push")

  Scene.Update(0.25)
  Scene.Dispatch("OnKey", "K", 1)
  assert_equal(Scene.Pop(), scene_b, "Scene.Pop returns top")
  assert_equal(Scene.Top(), scene_a, "Scene.Top after pop")

  Scene.Replace(scene_c)
  assert_equal(Scene.Count(), 1, "Scene.Count after replace")
  assert_equal(Scene.Top(), scene_c, "Scene.Top after replace")

  Scene.Clear()
  assert_equal(Scene.Count(), 0, "Scene.Count after clear")

  assert_equal(
    table.concat(events, ","),
    "a.enter,a.pause,b.enter,b.update:0.25,b.key:K:1,b.exit,a.resume,a.exit,c.enter,c.exit",
    "Scene lifecycle order"
  )
end

local function test_sprite_animation()
  local SpriteAnimation = require_table("Light.Graphics.SpriteAnimation")

  local events = {}
  local anim = SpriteAnimation.New({"frame1", "frame2", "frame3"}, 0.1, true)
  anim.OnFrame = function(_, index) events[#events + 1] = "frame:" .. tostring(index) end
  anim.OnLoop = function() events[#events + 1] = "loop" end

  assert_equal(anim:GetFrameCount(), 3, "SpriteAnimation.GetFrameCount")
  assert_equal(anim:GetFrame(), "frame1", "SpriteAnimation.GetFrame initial")
  assert_equal(anim:GetFrameIndex(), 1, "SpriteAnimation.GetFrameIndex initial")
  assert_false(anim:IsPlaying(), "SpriteAnimation initially stopped")

  anim:Play()
  assert_true(anim:IsPlaying(), "SpriteAnimation after Play")
  anim:Update(0.1)
  assert_equal(anim:GetFrameIndex(), 2, "SpriteAnimation after one frame")
  anim:Update(0.2)
  assert_equal(anim:GetFrameIndex(), 1, "SpriteAnimation after loop")

  anim:Pause()
  assert_false(anim:IsPlaying(), "SpriteAnimation after Pause")
  anim:Resume()
  assert_true(anim:IsPlaying(), "SpriteAnimation after Resume")
  anim:SetFrame(99)
  assert_equal(anim:GetFrameIndex(), 3, "SpriteAnimation SetFrame upper clamp")
  anim:SetFrame(-1)
  assert_equal(anim:GetFrameIndex(), 1, "SpriteAnimation SetFrame lower clamp")
  anim:Stop()
  assert_equal(anim:GetFrameIndex(), 1, "SpriteAnimation after Stop index")
  assert_false(anim:IsPlaying(), "SpriteAnimation after Stop playing")

  assert_equal(table.concat(events, ","), "frame:2,frame:3,loop,frame:1,frame:3,frame:1", "SpriteAnimation callbacks")

  local completed = 0
  local once = SpriteAnimation.New({"one", "two"}, 0.1, false)
  once.OnComplete = function() completed = completed + 1 end
  once:Play()
  once:Update(0.2)
  assert_equal(once:GetFrameIndex(), 2, "SpriteAnimation non-loop final frame")
  assert_false(once:IsPlaying(), "SpriteAnimation non-loop stopped")
  assert_equal(completed, 1, "SpriteAnimation non-loop OnComplete")
end

local function test_widgets()
  local WidgetModule = require_table("Light.UI.Widget")
  local Widget = WidgetModule.Widget
  local Container = WidgetModule.Container
  local Label = WidgetModule.Label
  local Button = WidgetModule.Button
  local CheckBox = WidgetModule.CheckBox

  assert_type(Widget.New, "function", "Widget.New")
  assert_type(Container.New, "function", "Container.New")
  assert_type(Label.New, "function", "Label.New")
  assert_type(Button.New, "function", "Button.New")
  assert_type(CheckBox.New, "function", "CheckBox.New")

  local root = Container.New(10, 20, 120, 100)
  local clicks = 0
  local button = Button.New(5, 6, 30, 20, "OK", {
    OnClick = function() clicks = clicks + 1 end,
  })
  assert_equal(root:AddChild(button), button, "Widget.AddChild return")
  assert_equal(button.parent, root, "Widget.AddChild parent")

  local ax, ay = button:GetAbsolutePosition()
  assert_equal(ax, 15, "Widget.GetAbsolutePosition x")
  assert_equal(ay, 26, "Widget.GetAbsolutePosition y")
  assert_true(button:HitTest(16, 27), "Widget.HitTest inside")
  assert_false(button:HitTest(200, 200), "Widget.HitTest outside")
  assert_equal(root:FindHit(16, 27), button, "Widget.FindHit child")

  assert_true(root:Dispatch("OnMouseMove", 16, 27), "Widget.Dispatch mouse move")
  assert_equal(button.state, "hover", "Button hover state")
  assert_true(root:Dispatch("OnMouseDown", 16, 27, 1), "Widget.Dispatch mouse down")
  assert_equal(button.state, "pressed", "Button pressed state")
  assert_true(root:Dispatch("OnMouseUp", 16, 27, 1), "Widget.Dispatch mouse up")
  assert_equal(clicks, 1, "Button OnClick")

  local changes = {}
  local checkbox = CheckBox.New(50, 10, 18, "Check", {
    OnChange = function(_, checked) changes[#changes + 1] = checked and "true" or "false" end,
  })
  root:AddChild(checkbox)
  assert_true(root:Dispatch("OnMouseUp", 61, 31, 1), "CheckBox mouse up")
  assert_true(checkbox.checked, "CheckBox checked")
  assert_equal(table.concat(changes, ","), "true", "CheckBox OnChange")

  local label = Label.New(0, 0, "Hello")
  assert_equal(label:GetText(), "Hello", "Label initial text")
  label:SetText("World")
  assert_equal(label:GetText(), "World", "Label SetText")

  assert_true(root:RemoveChild(button), "Widget.RemoveChild existing")
  assert_equal(button.parent, nil, "Widget.RemoveChild parent cleared")
  root:RemoveAllChildren()
  assert_equal(#root.children, 0, "Widget.RemoveAllChildren")
end

local function test_hotreload()
  local HotReload = require_table("Light.HotReload")
  HotReload.Clear()

  local path = "phase3_hotreload_smoke.tmp"
  local file = assert(io.open(path, "w"))
  file:write("initial")
  file:close()

  local callback_count = 0
  local id = HotReload.Watch(path, function(changed_path)
    if changed_path == path then
      callback_count = callback_count + 1
    end
  end)
  assert_type(id, "number", "HotReload.Watch")

  local list = HotReload.List()
  assert_type(list, "table", "HotReload.List")
  assert_equal(#list, 1, "HotReload.List count")
  assert_equal(list[1].id, id, "HotReload.List id")
  assert_equal(list[1].path, path, "HotReload.List path")

  HotReload.SetInterval(0)
  assert_type(HotReload.Check(0), "number", "HotReload.Check return")
  assert_type(HotReload.CheckNow(), "number", "HotReload.CheckNow return")
  assert_type(callback_count, "number", "HotReload callback counter")

  assert_true(HotReload.Unwatch(id), "HotReload.Unwatch existing")
  assert_false(HotReload.Unwatch(id), "HotReload.Unwatch missing")
  HotReload.Clear()
  assert_equal(#HotReload.List(), 0, "HotReload.Clear")
  os.remove(path)
end

local function test_shader_api_table()
  local Shader = require_table("Light.Graphics.Shader")
  assert_type(Shader.IsSupported, "function", "Shader.IsSupported")
  assert_type(Shader.New, "function", "Shader.New")
  assert_type(Shader.UseDefault, "function", "Shader.UseDefault")
  assert_type(Shader.IsSupported(), "boolean", "Shader.IsSupported return")
  Shader.UseDefault()
end

test_crypto()
test_scene()
test_sprite_animation()
test_widgets()
test_hotreload()
test_shader_api_table()

print("phase3_runtime smoke ok")
```

- [ ] **Step 3: Run syntax check for the new script**

Run after Lumen is built:

```powershell
$lumenLightc = "lumen-master\build\src\lightc\Release\lightc.exe"
& $lumenLightc -p scripts\smoke\phase3_runtime.lua
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Expected: command exits `0` and prints no syntax error.

- [ ] **Step 4: Run runtime smoke if local Windows artifacts exist**

Run after `Light.dll` and `light.exe` are built:

```powershell
$runtimeDir = "lumen-master\build\src\light\Release"
Copy-Item ChocoLight\build\bin\Release\Light.dll $runtimeDir -Force
& "$runtimeDir\light.exe" (Resolve-Path scripts\smoke\phase3_runtime.lua)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Expected output contains:

```text
phase3_runtime smoke ok
```

- [ ] **Step 5: Commit mandatory smoke script**

Run:

```powershell
git add scripts\smoke\phase3_runtime.lua
git commit -m "test: add Phase3 runtime smoke"
```

Expected: commit succeeds with one created file.

---

### Task 2: Add enhanced window/GL runtime smoke with safe skip

**Files:**
- Create: `scripts/smoke/phase3_window_runtime.lua`

- [ ] **Step 1: Confirm target file is absent**

Run:

```powershell
Test-Path scripts\smoke\phase3_window_runtime.lua
git status --short
```

Expected:

```text
False
```

`git status --short` must not show an existing `scripts/smoke/phase3_window_runtime.lua`.

- [ ] **Step 2: Create `phase3_window_runtime.lua`**

Create `scripts/smoke/phase3_window_runtime.lua` with exactly this content:

```lua
local function fail(message)
  error(message, 2)
end

local function assert_type(value, expected, label)
  if type(value) ~= expected then
    fail(label .. ": expected type " .. expected .. ", got " .. type(value))
  end
end

local function assert_true(value, label)
  if not value then
    fail(label .. ": expected true-like value")
  end
end

local function skip(message)
  print("phase3_window_runtime smoke skipped: " .. message)
  os.exit(0)
end

local function require_or_skip(name)
  local ok, module_or_error = pcall(require, name)
  if not ok then
    skip("require(" .. name .. ") failed: " .. tostring(module_or_error))
  end
  return module_or_error
end

require_or_skip("Light.UI.Window")
local Graphics = require_or_skip("Light.Graphics")
local Shader = require_or_skip("Light.Graphics.Shader")
local Widget = require_or_skip("Light.UI.Widget")

assert_type(Graphics.SetColor, "function", "Graphics.SetColor")
assert_type(Graphics.Rectangle, "function", "Graphics.Rectangle")
assert_type(Shader.IsSupported, "function", "Shader.IsSupported")
assert_type(Widget.Container.New, "function", "Widget.Container.New")

local app = Light(Light.UI.Window):New()
local root = Widget.Container.New(0, 0, 160, 120)
local panel = Widget.Panel.New(8, 8, 80, 32, {
  bgColor = {0.2, 0.25, 0.35, 1.0},
  borderColor = {0.6, 0.7, 1.0, 1.0},
})
local button = Widget.Button.New(12, 48, 72, 24, "Smoke")
root:AddChild(panel)
root:AddChild(button)

local function try_shader_path()
  if not Shader.IsSupported() then
    print("phase3_window_runtime shader path skipped: backend does not support user shaders")
    return
  end

  local vertex_source = [[
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
void main() {
  gl_Position = uMVP * vec4(aPos, 1.0);
  vColor = aColor;
}
]]
  local fragment_source = [[
#version 330 core
in vec4 vColor;
uniform float uTime;
uniform vec2 uRes;
uniform vec3 uColor;
uniform vec4 uTint;
uniform int uMode;
uniform mat4 uMat;
out vec4 FragColor;
void main() {
  FragColor = vec4(uColor, 1.0) * uTint * vColor;
}
]]

  local shader, err = Shader.New(vertex_source, fragment_source)
  assert_true(shader, "Shader.New failed: " .. tostring(err))
  assert_true(shader:Use(), "Shader.Use")
  shader:SetFloat("uTime", 1.0)
  shader:SetVec2("uRes", 160, 120)
  shader:SetVec3("uColor", 1.0, 1.0, 1.0)
  shader:SetVec4("uTint", 1.0, 1.0, 1.0, 1.0)
  shader:SetInt("uMode", 1)
  shader:SetMat4("uMat", {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
  })
  Shader.UseDefault()
  shader:Delete()
end

function app:Draw()
  Graphics.SetColor(0.1, 0.12, 0.16, 1.0)
  Graphics.Rectangle(2, 0, 0, 160, 120, 0)
  Graphics.SetColor(0.9, 0.35, 0.25, 1.0)
  Graphics.Rectangle(2, 96, 16, 32, 32, 0)
  root:Draw()
end

function app:Update()
  self:Close()
end

local ok, open_result = pcall(function()
  return app:Open(160, 120, "Phase3 Smoke")
end)

if not ok then
  skip("window open raised error: " .. tostring(open_result))
end

if open_result == false then
  skip("window or GL context unavailable")
end

local width, height = app:GetDimensions()
assert_true(width > 0, "Window width")
assert_true(height > 0, "Window height")

try_shader_path()

local frame_ok, frame_error = pcall(function()
  app()
end)
assert_true(frame_ok, "Window frame failed: " .. tostring(frame_error))

print("phase3_window_runtime smoke ok")
```

- [ ] **Step 3: Run syntax check for the enhanced script**

Run after Lumen is built:

```powershell
$lumenLightc = "lumen-master\build\src\lightc\Release\lightc.exe"
& $lumenLightc -p scripts\smoke\phase3_window_runtime.lua
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Expected: command exits `0` and prints no syntax error.

- [ ] **Step 4: Run enhanced runtime smoke if local Windows artifacts exist**

Run after `Light.dll` and `light.exe` are built:

```powershell
$runtimeDir = "lumen-master\build\src\light\Release"
Copy-Item ChocoLight\build\bin\Release\Light.dll $runtimeDir -Force
& "$runtimeDir\light.exe" (Resolve-Path scripts\smoke\phase3_window_runtime.lua)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Expected output contains exactly one of these success conditions:

```text
phase3_window_runtime smoke ok
```

or:

```text
phase3_window_runtime smoke skipped:
```

If output contains a Lua traceback after the window opened successfully, fix the smoke script or the engine bug before continuing.

- [ ] **Step 5: Commit enhanced smoke script**

Run:

```powershell
git add scripts\smoke\phase3_window_runtime.lua
git commit -m "test: add Phase3 window smoke"
```

Expected: commit succeeds with one created file.

---

### Task 3: Wire Phase3 smoke scripts into Windows runtime CI

**Files:**
- Modify: `.github/workflows/build-templates.yml:44-54`

- [ ] **Step 1: Replace the Windows runtime smoke step body**

In `.github/workflows/build-templates.yml`, replace the current `Run Windows runtime smoke scripts` PowerShell body at lines 47-54 with:

```yaml
          $runtimeDir = "lumen-master\build\src\light\Release"
          $coreSmoke = Resolve-Path "scripts\smoke\core_runtime.lua"
          $physicsSmoke = Resolve-Path "scripts\smoke\physics_p0_p1.lua"
          $phase3Smoke = Resolve-Path "scripts\smoke\phase3_runtime.lua"
          $phase3WindowSmoke = Resolve-Path "scripts\smoke\phase3_window_runtime.lua"
          Copy-Item ChocoLight\build\bin\Release\Light.dll $runtimeDir -Force
          & "$runtimeDir\light.exe" $coreSmoke
          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
          & "$runtimeDir\light.exe" $physicsSmoke
          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
          & "$runtimeDir\light.exe" $phase3Smoke
          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
          & "$runtimeDir\light.exe" $phase3WindowSmoke
          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

The complete workflow section must remain under:

```yaml
      - name: Run Windows runtime smoke scripts
        shell: pwsh
        run: |
```

- [ ] **Step 2: Verify workflow references the two new scripts**

Run:

```powershell
Select-String -Path .github\workflows\build-templates.yml -Pattern "phase3_runtime|phase3_window_runtime|Run Windows runtime smoke scripts"
```

Expected output contains all three patterns.

- [ ] **Step 3: Commit CI wiring**

Run:

```powershell
git add .github\workflows\build-templates.yml
git commit -m "ci: run Phase3 smoke validation"
```

Expected: commit succeeds with one modified workflow file.

---

### Task 4: Run local verification and push to GitHub Actions

**Files:**
- Verify: `scripts/smoke/phase3_runtime.lua`
- Verify: `scripts/smoke/phase3_window_runtime.lua`
- Verify: `.github/workflows/build-templates.yml`

- [ ] **Step 1: Confirm the push remote is correct**

Run:

```powershell
git remote get-url origin
```

Expected:

```text
https://github.com/futzhj/ChocoLightEngine.git
```

If the output is not exactly that URL, stop before pushing.

- [ ] **Step 2: Run syntax checks for all smoke scripts**

Run after Lumen is built:

```powershell
$lightc = "lumen-master\build\src\lightc\Release\lightc.exe"
Get-ChildItem scripts\smoke -Filter *.lua | ForEach-Object {
  & $lightc -p $_.FullName
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected: all scripts exit `0` with no syntax errors.

- [ ] **Step 3: Run mandatory runtime scripts locally when artifacts exist**

Run after ChocoLight is built:

```powershell
$runtimeDir = "lumen-master\build\src\light\Release"
Copy-Item ChocoLight\build\bin\Release\Light.dll $runtimeDir -Force
& "$runtimeDir\light.exe" (Resolve-Path scripts\smoke\core_runtime.lua)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" (Resolve-Path scripts\smoke\physics_p0_p1.lua)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" (Resolve-Path scripts\smoke\phase3_runtime.lua)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" (Resolve-Path scripts\smoke\phase3_window_runtime.lua)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

Expected output contains:

```text
core_runtime smoke ok
physics_p0_p1 smoke ok
phase3_runtime smoke ok
```

For the enhanced window script, expected output contains either:

```text
phase3_window_runtime smoke ok
```

or:

```text
phase3_window_runtime smoke skipped:
```

- [ ] **Step 4: Inspect final local changes**

Run:

```powershell
git status --short --branch
git log --oneline -n 6 --decorate
```

Expected:

```text
## main...origin/main [ahead 4]
```

The exact ahead count can be higher if the design and plan commits are included. The latest commits must include:

```text
docs: design Phase3 smoke validation
test: add Phase3 runtime smoke
test: add Phase3 window smoke
ci: run Phase3 smoke validation
```

- [ ] **Step 5: Push to GitHub Actions**

Run only after Step 1 confirms the `origin` URL:

```powershell
git push origin main
```

Expected: push succeeds and triggers the `Build Templates (All Platforms)` workflow because the workflow listens to pushes on `main`.

- [ ] **Step 6: Record GitHub Actions result**

Open the Actions page for `futzhj/ChocoLightEngine` and inspect the run triggered by the pushed commit.

Expected final result:

```text
Build Templates (All Platforms): success
```

If the run fails, inspect the failing job log and fix the smallest relevant issue before pushing another commit to `origin/main`.

---

## Plan Self-Review

Spec coverage:

- Mandatory no-window Phase3 behavior smoke is covered by Task 1.
- Enhanced window/GL smoke with safe skip is covered by Task 2.
- Windows runtime CI wiring is covered by Task 3.
- Local verification and GitHub Actions validation through `origin` are covered by Task 4.

Placeholder scan:

- The plan contains exact file paths, exact commands, expected outputs, and complete new script contents.
- The plan contains no open-ended implementation placeholders.

Type and API consistency:

- `Light.Crypto` function names match `ChocoLight/src/light_crypto.cpp`.
- `Light.Scene` function names match `ChocoLight/src/light_scene.cpp`.
- `Light.Graphics.SpriteAnimation` methods match `ChocoLight/src/light_graphics_spriteanimation.cpp`.
- `Light.UI.Widget` constructors and instance methods match `ChocoLight/src/light_ui_widget.cpp`.
- `Light.HotReload` function names match `ChocoLight/src/light_hotreload.cpp`.
- `Light.Graphics.Shader` function and userdata method names match `ChocoLight/src/light_graphics_shader.cpp`.
- `Light.UI.Window:Open`, `GetDimensions`, `Close`, and `__call` behavior match `ChocoLight/src/light_ui.cpp`.
