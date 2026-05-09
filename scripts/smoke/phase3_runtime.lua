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
