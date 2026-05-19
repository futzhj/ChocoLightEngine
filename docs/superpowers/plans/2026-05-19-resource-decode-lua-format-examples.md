# Resource Decode Lua Format Examples Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 TCP/SP、MAP M1.0、MAP 0.1M/mx_map 三类资源各提供一个独立 Lua smoke 测试例子，并保留聚合入口。

**Architecture:** 每个 smoke 文件只负责一种资源格式，重复的检查逻辑保持局部、简单、可读。`scripts/smoke/resource_decode.lua` 作为聚合入口，按固定顺序执行三个独立 smoke，便于 CI 继续使用原命令。

**Tech Stack:** Lua 5.1、`Light.Plugins.TCP`、`Light.Plugins.Map`、`lightc -p` 语法验证、GitHub Actions 跨平台 CI。

---

### Task 1: 新增 TCP/SP 独立 smoke

**Files:**
- Create: `scripts/smoke/resource_decode_tcp_sp.lua`

- [ ] **Step 1: 创建 TCP/SP 测试文件**

写入 `scripts/smoke/resource_decode_tcp_sp.lua`：

```lua
local TCP = require("Light.Plugins.TCP")

local function check(cond, msg)
    if not cond then
        error("[resource_decode_tcp_sp] " .. tostring(msg), 2)
    end
end

local function exists(path)
    local f = io.open(path, "rb")
    if f then
        f:close()
        return true
    end
    return false
end

local sep = package.config:sub(1, 1)
local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"
local path = assets .. sep .. "鸿鸣.tcp"

check(type(TCP.Open) == "function", "TCP.Open missing")

if not exists(path) then
    print("[resource_decode_tcp_sp] skip asset: " .. path)
    return
end

local tcp, err = TCP.Open(path)
check(tcp ~= nil, "TCP.Open failed: " .. tostring(err))

local info = tcp:GetInfo()
check(type(info) == "table", "GetInfo must return table")
check(info.kind == "SP", "kind must be SP")
check(info.frameCount > 0, "frameCount must be > 0")
check(info.width > 0 and info.height > 0, "logical size invalid")

local frame = tcp:DecodeFrame(0)
check(type(frame) == "table", "DecodeFrame must return table")
check(type(frame.rgba) == "string", "frame rgba must be string")
check(#frame.rgba == frame.width * frame.height * 4, "frame rgba length mismatch")

local atlas = tcp:DecodeAtlas()
check(type(atlas) == "table", "DecodeAtlas must return table")
check(type(atlas.rgba) == "string", "atlas rgba must be string")
check(#atlas.rgba == atlas.width * atlas.height * 4, "atlas rgba length mismatch")

tcp:Close()
print("[resource_decode_tcp_sp] PASS")
```

- [ ] **Step 2: 语法验证**

Run: `E:\jinyiNew\Light\lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\resource_decode_tcp_sp.lua`
Expected: exit code 0。

### Task 2: 新增 MAP M1.0 独立 smoke

**Files:**
- Create: `scripts/smoke/resource_decode_map_m10.lua`

- [ ] **Step 1: 创建 MAP M1.0 测试文件**

测试 `assets/1001.map`，验证 `GetInfo()`、`DecodePreview(2048)`、RGBA 长度和 tile 数。

- [ ] **Step 2: 语法验证**

Run: `E:\jinyiNew\Light\lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\resource_decode_map_m10.lua`
Expected: exit code 0。

### Task 3: 新增 MAP 0.1M/mx_map 独立 smoke

**Files:**
- Create: `scripts/smoke/resource_decode_map_01m.lua`

- [ ] **Step 1: 创建 MAP 0.1M/mx_map 测试文件**

测试 `assets/mx_map/1001.map` 和 `assets/mx_map/1002.map`，存在即验证，不存在即 skip。

- [ ] **Step 2: 语法验证**

Run: `E:\jinyiNew\Light\lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\resource_decode_map_01m.lua`
Expected: exit code 0。

### Task 4: 改造聚合入口并验证

**Files:**
- Modify: `scripts/smoke/resource_decode.lua`

- [ ] **Step 1: 聚合执行三个独立 smoke**

`resource_decode.lua` 只负责 `dofile()`：

```lua
local sep = package.config:sub(1, 1)
local base = "scripts" .. sep .. "smoke" .. sep

dofile(base .. "resource_decode_tcp_sp.lua")
dofile(base .. "resource_decode_map_m10.lua")
dofile(base .. "resource_decode_map_01m.lua")

print("[resource_decode] PASS")
```

- [ ] **Step 2: 最终验证**

Run:
- `E:\jinyiNew\Light\lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\resource_decode.lua`
- `git diff --check`

Expected: 全部 exit code 0。

- [ ] **Step 3: Commit**

Run:

```powershell
git add scripts\smoke\resource_decode.lua scripts\smoke\resource_decode_tcp_sp.lua scripts\smoke\resource_decode_map_m10.lua scripts\smoke\resource_decode_map_01m.lua docs\superpowers\plans\2026-05-19-resource-decode-lua-format-examples.md
git commit -m "test: split resource decode smoke by format"
```

---

## Self-Review

- **覆盖性:** 覆盖 TCP/SP、MAP M1.0、MAP 0.1M/mx_map 三个格式/变体。
- **无占位:** 文件名、资源路径、验证命令均明确。
- **一致性:** 所有脚本使用 `LIGHT_TEST_ASSETS` 或默认 `assets`，缺资源 graceful skip。
