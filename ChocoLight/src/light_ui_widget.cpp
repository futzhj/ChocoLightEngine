/**
 * @file light_ui_widget.cpp
 * @brief Light.UI.Widget 模块 — 轻量 UI 控件库 (纯 Lua 嵌入)
 *
 * 设计理念:
 *   - 树形结构: 根 Widget 包含子 Widget, 自动级联事件/绘制
 *   - 命中测试: 屏幕坐标 → 命中最深的可见可交互子控件
 *   - 与 Light.Input/Light.Graphics 解耦, 通过 Dispatch 接收事件
 *
 * 内置控件:
 *   Widget    — 基类 (位置/尺寸/可见性/启用/层级)
 *   Container — 容器 (透明背景, 仅做布局/事件分发)
 *   Label     — 静态文本
 *   Button    — 可点击按钮 (4 状态: normal/hover/pressed/disabled)
 *   CheckBox  — 复选框 (checked 状态)
 *   Panel     — 面板 (带背景色/边框)
 *
 * Lua API 示例:
 *   local root = Light.UI.Widget.Container.New(0, 0, 800, 600)
 *
 *   local btn = Light.UI.Widget.Button.New(100, 50, 120, 40, "Click Me")
 *   btn.OnClick = function(self) print("clicked!") end
 *   root:AddChild(btn)
 *
 *   local lbl = Light.UI.Widget.Label.New(10, 10, "Hello", { color={1,1,1,1} })
 *   root:AddChild(lbl)
 *
 *   -- 主循环
 *   root:Update(dt)
 *   root:Draw()
 *   root:Dispatch("OnMouseMove", mx, my)
 *   root:Dispatch("OnMouseDown", mx, my, btn)
 *   root:Dispatch("OnKey", key, action)
 */

#include "light.h"
#include <cstring>

static const char s_WidgetLuaScript[] = R"LUA(
-- 依赖: Light.Graphics (绘制) / 可选 Light.Input
local G = (Light and Light.Graphics) or {}

local Widget = {}
Widget.__index = Widget

-- ==================== 基类 Widget ====================
function Widget.New(x, y, w, h)
  local self = setmetatable({}, Widget)
  self.x = x or 0
  self.y = y or 0
  self.w = w or 0
  self.h = h or 0
  self.visible  = true
  self.enabled  = true
  self.children = {}
  self.parent   = nil
  return self
end

function Widget:AddChild(child)
  if not child then return end
  if child.parent then child.parent:RemoveChild(child) end
  child.parent = self
  self.children[#self.children + 1] = child
  return child
end

function Widget:RemoveChild(child)
  for i, c in ipairs(self.children) do
    if c == child then
      table.remove(self.children, i)
      child.parent = nil
      return true
    end
  end
  return false
end

function Widget:RemoveAllChildren()
  for i = #self.children, 1, -1 do
    self.children[i].parent = nil
    self.children[i] = nil
  end
end

-- 获取屏幕坐标 (累加父级偏移)
function Widget:GetAbsolutePosition()
  local ax, ay = self.x, self.y
  local p = self.parent
  while p do
    ax = ax + p.x
    ay = ay + p.y
    p = p.parent
  end
  return ax, ay
end

-- 命中测试 (使用绝对坐标)
function Widget:HitTest(px, py)
  if not self.visible or not self.enabled then return false end
  local ax, ay = self:GetAbsolutePosition()
  return px >= ax and px <= ax + self.w and py >= ay and py <= ay + self.h
end

-- 查找命中的最深子控件 (深度优先, 倒序遍历优先后添加的)
function Widget:FindHit(px, py)
  if not self.visible then return nil end
  -- 先递归查找子控件 (倒序: 后添加的在上层)
  for i = #self.children, 1, -1 do
    local hit = self.children[i]:FindHit(px, py)
    if hit then return hit end
  end
  -- 自身命中
  if self:HitTest(px, py) then return self end
  return nil
end

-- 默认绘制 (子类覆盖, 但仍需调用 DrawChildren)
function Widget:Draw()
  self:DrawChildren()
end

function Widget:DrawChildren()
  for _, c in ipairs(self.children) do
    if c.visible then c:Draw() end
  end
end

function Widget:Update(dt)
  for _, c in ipairs(self.children) do
    c:Update(dt)
  end
end

-- 事件分发 (递归到子控件, 命中或 Container 才触发)
-- 鼠标事件按命中分发, 键盘事件全员分发
function Widget:Dispatch(method, ...)
  if not self.visible or not self.enabled then return false end

  -- 鼠标类事件: 找命中目标分发
  if method == 'OnMouseMove' or method == 'OnMouseDown' or method == 'OnMouseUp' then
    local px, py = ...
    local hit = self:FindHit(px, py)
    if hit then
      self:_DispatchHover(hit, method, ...)
      if type(hit[method]) == 'function' then
        hit[method](hit, ...)
      end
      return true
    end
    -- 未命中也要清除 hover 状态
    self:_DispatchHover(nil, method, ...)
    return false
  end

  -- 其他事件 (键盘/手柄): 仅分发到自身和聚焦控件
  if type(self[method]) == 'function' then
    self[method](self, ...)
  end
  for _, c in ipairs(self.children) do
    c:Dispatch(method, ...)
  end
  return true
end

-- 内部: 更新 hover 状态 (Button 等会用)
function Widget:_DispatchHover(target, method, ...)
  for _, c in ipairs(self.children) do
    if c._OnHoverChanged then
      c:_OnHoverChanged(c == target)
    end
    c:_DispatchHover(target, method, ...)
  end
end

-- ==================== Container (透明容器) ====================
local Container = setmetatable({}, {__index = Widget})
Container.__index = Container

function Container.New(x, y, w, h)
  local self = Widget.New(x, y, w, h)
  return setmetatable(self, Container)
end

-- ==================== Label ====================
local Label = setmetatable({}, {__index = Widget})
Label.__index = Label

function Label.New(x, y, text, opts)
  opts = opts or {}
  local self = Widget.New(x, y, opts.w or 0, opts.h or 0)
  self.text  = text or ''
  self.font  = opts.font
  self.color = opts.color or {1, 1, 1, 1}
  return setmetatable(self, Label)
end

function Label:SetText(s) self.text = tostring(s or '') end
function Label:GetText()  return self.text end

function Label:Draw()
  if not self.visible or self.text == '' then
    self:DrawChildren()
    return
  end
  local ax, ay = self:GetAbsolutePosition()
  local r, g_, b, a = 1, 1, 1, 1
  if G.GetColor then r, g_, b, a = G.GetColor() end
  if G.SetColor then G.SetColor(self.color[1], self.color[2], self.color[3], self.color[4] or 1) end
  if G.Print then G.Print(self.text, self.font, ax, ay, 0) end
  if G.SetColor then G.SetColor(r, g_, b, a) end
  self:DrawChildren()
end

-- ==================== Panel (带背景的矩形) ====================
local Panel = setmetatable({}, {__index = Widget})
Panel.__index = Panel

function Panel.New(x, y, w, h, opts)
  opts = opts or {}
  local self = Widget.New(x, y, w, h)
  self.bgColor     = opts.bgColor     or {0.15, 0.15, 0.15, 0.85}
  self.borderColor = opts.borderColor or {0.4, 0.4, 0.4, 1.0}
  self.border      = (opts.border ~= false)  -- 默认绘制边框
  return setmetatable(self, Panel)
end

function Panel:Draw()
  if not self.visible then return end
  local ax, ay = self:GetAbsolutePosition()
  local r, g_, b, a = 1, 1, 1, 1
  if G.GetColor then r, g_, b, a = G.GetColor() end

  -- 背景填充 (mode=2)
  if G.SetColor and G.Rectangle then
    G.SetColor(self.bgColor[1], self.bgColor[2], self.bgColor[3], self.bgColor[4] or 1)
    G.Rectangle(2, ax, ay, self.w, self.h, 0)
    -- 边框 (mode=1)
    if self.border then
      G.SetColor(self.borderColor[1], self.borderColor[2], self.borderColor[3], self.borderColor[4] or 1)
      G.Rectangle(1, ax, ay, self.w, self.h, 0)
    end
    G.SetColor(r, g_, b, a)
  end
  self:DrawChildren()
end

-- ==================== Button ====================
local Button = setmetatable({}, {__index = Widget})
Button.__index = Button

function Button.New(x, y, w, h, text, opts)
  opts = opts or {}
  local self = Widget.New(x, y, w, h)
  self.text     = text or ''
  self.font     = opts.font
  self.state    = 'normal'  -- normal / hover / pressed / disabled
  self.colors   = opts.colors or {
    normal   = {0.25, 0.25, 0.30, 1.0},
    hover    = {0.35, 0.35, 0.45, 1.0},
    pressed  = {0.20, 0.30, 0.50, 1.0},
    disabled = {0.20, 0.20, 0.20, 0.6},
  }
  self.textColor = opts.textColor or {1, 1, 1, 1}
  self.OnClick   = opts.OnClick   -- 用户回调
  return setmetatable(self, Button)
end

function Button:SetEnabled(v)
  self.enabled = (v and true or false)
  self.state = self.enabled and 'normal' or 'disabled'
end

-- 内部 hover 状态变更
function Button:_OnHoverChanged(isHover)
  if not self.enabled then return end
  if self.state ~= 'pressed' then
    self.state = isHover and 'hover' or 'normal'
  end
end

function Button:OnMouseDown(px, py, btn)
  if not self.enabled then return end
  if btn == 1 or btn == nil then  -- 左键 (兼容无 btn 参数)
    self.state = 'pressed'
  end
end

function Button:OnMouseUp(px, py, btn)
  if not self.enabled then return end
  if self.state == 'pressed' then
    self.state = self:HitTest(px, py) and 'hover' or 'normal'
    if self.OnClick and self:HitTest(px, py) then
      local ok, err = pcall(self.OnClick, self)
      if not ok then print("[Button] OnClick error: " .. tostring(err)) end
    end
  end
end

function Button:Draw()
  if not self.visible then return end
  local ax, ay = self:GetAbsolutePosition()
  local color = self.colors[self.state] or self.colors.normal
  local r, g_, b, a = 1, 1, 1, 1
  if G.GetColor then r, g_, b, a = G.GetColor() end

  -- 背景
  if G.SetColor and G.Rectangle then
    G.SetColor(color[1], color[2], color[3], color[4] or 1)
    G.Rectangle(2, ax, ay, self.w, self.h, 0)
    -- 边框
    G.SetColor(0.5, 0.5, 0.6, 1)
    G.Rectangle(1, ax, ay, self.w, self.h, 0)
  end

  -- 文本居中 (粗略居中: 假设字符 8px 宽)
  if self.text ~= '' and G.Print and G.SetColor then
    G.SetColor(self.textColor[1], self.textColor[2], self.textColor[3], self.textColor[4] or 1)
    local tw = #self.text * 8
    local tx = ax + (self.w - tw) / 2
    local ty = ay + (self.h - 16) / 2
    G.Print(self.text, self.font, tx, ty, 0)
  end

  if G.SetColor then G.SetColor(r, g_, b, a) end
  self:DrawChildren()
end

-- ==================== CheckBox ====================
local CheckBox = setmetatable({}, {__index = Widget})
CheckBox.__index = CheckBox

function CheckBox.New(x, y, size, text, opts)
  opts = opts or {}
  size = size or 18
  local self = Widget.New(x, y, size + 8 + (text and #text * 8 or 0), size)
  self.boxSize  = size
  self.text     = text or ''
  self.font     = opts.font
  self.checked  = opts.checked or false
  self.color    = opts.color or {1, 1, 1, 1}
  self.OnChange = opts.OnChange
  return setmetatable(self, CheckBox)
end

function CheckBox:OnMouseUp(px, py, btn)
  if not self.enabled then return end
  if self:HitTest(px, py) then
    self.checked = not self.checked
    if self.OnChange then
      local ok, err = pcall(self.OnChange, self, self.checked)
      if not ok then print("[CheckBox] OnChange error: " .. tostring(err)) end
    end
  end
end

function CheckBox:Draw()
  if not self.visible then return end
  local ax, ay = self:GetAbsolutePosition()
  local r, g_, b, a = 1, 1, 1, 1
  if G.GetColor then r, g_, b, a = G.GetColor() end

  if G.SetColor and G.Rectangle then
    -- 边框
    G.SetColor(self.color[1], self.color[2], self.color[3], 1)
    G.Rectangle(1, ax, ay, self.boxSize, self.boxSize, 0)
    -- 勾选填充
    if self.checked then
      G.SetColor(0.3, 0.7, 1.0, 1)
      G.Rectangle(2, ax + 3, ay + 3, self.boxSize - 6, self.boxSize - 6, 0)
    end
  end
  -- 标签
  if self.text ~= '' and G.Print and G.SetColor then
    G.SetColor(self.color[1], self.color[2], self.color[3], self.color[4] or 1)
    G.Print(self.text, self.font, ax + self.boxSize + 6, ay, 0)
  end
  if G.SetColor then G.SetColor(r, g_, b, a) end
  self:DrawChildren()
end

-- ==================== 模块导出 ====================
return {
  Widget    = Widget,
  Container = Container,
  Label     = Label,
  Panel     = Panel,
  Button    = Button,
  CheckBox  = CheckBox,
}
)LUA";

// ==================== luaopen_Light_UI_Widget ====================
// 注册到 Light.UI.Widget

static void EnsureUITable(lua_State* L) {
    LT::EnsureLightTable(L);
    lua_pushstring(L, "UI");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "UI");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "UI");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);  // 移除 Light, 保留 UI
}

int luaopen_Light_UI_Widget(lua_State* L) {
    EnsureUITable(L);

    lua_pushstring(L, "Widget");
    lua_rawget(L, -2);

    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Widget");

        if (!luaL_loadstring(L, s_WidgetLuaScript) && !lua_pcall(L, 0, LUA_MULTRET, 0)) {
            lua_rawset(L, -3);
            lua_pushstring(L, "Widget");
            lua_rawget(L, -2);
        } else {
            lua_error(L);
        }
    }

    lua_remove(L, -2);
    return 1;
}
