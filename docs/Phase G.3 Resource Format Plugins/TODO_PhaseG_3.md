# Phase G.3 — 资源格式插件集成 (TODO)

> **创建日期**: 2026-05-19  
> **用途**: 记录实现阶段待办、样本缺口、后续扩展边界

---

## 一. 当前待办总览

| 优先级 | 待办 | 状态 | 说明 |
|---|---|---:|---|
| P0 | 实现 Package MVP | ⏳ | PFDW/WPK/FLS 容器读取 |
| P0 | 建立 smoke 框架 | ⏳ | 无 assets 时跳过，有 assets 时完整验证 |
| P0 | 实现 TCP-SP Decode | ⏳ | assets 有多份 SP `.tcp` 样本 |
| P1 | 实现 Map M1.0 基础读取 | ⏳ | assets 有 `1001.map` |
| P1 | 完善 WPK AC/XC 解密 | ⏳ | 需确认具体条目是否加密 |
| P1 | 完善 FLS 数据解密 | ⏳ | 需对照真实条目输出验证 |
| P2 | IGS Decode 真实样本验证 | ⏳ | 当前未直接发现 `.igs` 样本 |
| P2 | WDFX/WDFH/SFDW/WDFS 样本验证 | ⏳ | 当前 assets 只有 PFDW |
| P2 | MAPX/M2.5/M3.0/ROL0 | ⏳ | 需要更多 MAP 样本 |
| P2 | gmx_map PNAM/MANP | ⏳ | 当前缺少样本 |

---

## 二. 明确不做

### 2.1 NXPK/MHWD

用户已确认：

- `NXPK` 不属于本阶段通用格式支持范围。
- `MHWD` 不属于本阶段通用格式支持范围。

实现要求：

- 能识别则返回 unsupported。
- 不实现索引解密。
- 不实现条目读取。
- 不在文档中宣传支持。

### 2.2 Python 运行时依赖

- 不把 `tcp_viewer0302` Python 文件作为运行时依赖。
- Python 只作为格式参考。
- C++ 实现必须独立可编译。

### 2.3 大测试资源提交

- `assets/` 是本地测试资源目录。
- 不提交大资源到 git。
- smoke 必须允许 assets 缺失时跳过。

---

## 三. 样本缺口

### 3.1 WDF 子类型

当前有：

- `wzife.wdf`：PFDW。

缺少：

- WDFX。
- WDFH。
- SFDW。
- WDFS。

下一步：

- 从现有资源或外部样本中补齐。
- 每个子类型至少需要一个小样本和一个可验证条目。

### 3.2 TCP 子类型

当前有：

- SP：多份 `.tcp`。

缺少：

- TP。
- RP。

下一步：

- 在 WDF/WPK/FLS 条目中按 magic 搜索 `0x5054/0x5052`。
- 找到后加入 smoke 的可选验证。

### 3.3 IGS

当前未直接发现 `.igs` 文件。

下一步：

- 从 FLS/WDF 条目中扫描 IGS magic：
  - `0x3049`
  - `0x3054`
  - `0x3149`
  - `0x3154`
  - `0x3549`
  - `0x3554`
- 若找到，记录 hash/file_id，并加入 smoke。

### 3.4 MAP

当前有：

- `1001.map`：前 4 字节 `30 2E 31 4D`，对应 M1.0/`0.1M`。

缺少：

- MAPX。
- M2.5。
- M3.0。
- ROL0。
- gmx_map PNAM/MANP。

下一步：

- 优先完成 M1.0 基础读取。
- gmx_map 需要用户提供真实 PNAM/MANP 样本后再实现完整解码。

---

## 四. 实现注意事项

### 4.1 Lua 模块注册

禁止：

```cpp
luaL_register(L, "Light.Plugins.Xxx", funcs);
```

应使用：

```cpp
lua_newtable(L);
luaL_setfuncs(L, funcs, 0);
return 1;
```

或使用项目已有安全 helper。

### 4.2 二进制输出

新模块统一用 Lua string 输出 bytes，不使用 userdata 输出裸 buffer，除非后续需要与 `ImageData` 做零拷贝集成。

### 4.3 读取安全

每个 parser 都必须处理：

- 文件长度不足。
- count 过大。
- offset 越界。
- size 越界。
- offset + size 溢出。
- 解压输出过大。

### 4.4 解密安全

- 自动解密仅在 magic 明确时执行。
- `raw=true` 必须允许用户取原始 bytes。
- 无样本格式不要声称完成。

---

## 五. 本地验证建议

设置测试资源路径：

```powershell
$env:LIGHT_TEST_ASSETS="E:\jinyiNew\Light\assets"
```

后续实现后建议运行：

```powershell
# 示例，具体命令以项目 smoke 入口为准
.\build\light.exe scripts\smoke\resource_formats.lua
```

若 assets 不存在：

- smoke 只验证 require/API surface/synthetic probe。
- 输出 skip 信息。
- 不算失败。

---

## 六. 后续增强候选

### 6.1 ImageData 直接输出

当前 TCP/IGS/MAP 返回 RGBA bytes。后续可增加：

```lua
TCP.DecodeImageData(data, frameIndex)
IGS.DecodeImageData(data, frameIndex)
Map.GetTileImageData(tileId)
```

### 6.2 Package 资源自动解码

后续可以提供：

```lua
pkg:Decode(key)
```

自动根据条目 magic 派发 TCP/IGS/图片/音频。

### 6.3 Map 渲染和 Mask

后续可实现：

- MAPX JPEG/JPGH 修复。
- M1.0/MAPX mask decode。
- gmx_map LZO mask decode。
- tile atlas 或整图拼接。

### 6.4 异步加载

后续可接入已有 AssetLoader：

```lua
Package.OpenAsync(path, cb)
TCP.DecodeAsync(data, cb)
Map.OpenAsync(path, cb)
```

本阶段先不做，避免把格式解析和异步调度耦合。

---

## 七. 用户待提供/确认

- 是否能提供 gmx_map PNAM/MANP 真实样本。
- 是否能提供 WDFX/WDFH/SFDW/WDFS 样本。
- 是否需要第一阶段就把 TCP/IGS 输出封装成 `ImageData`。
- 是否需要 `Package.OpenData(data)` 在第一阶段实现，还是先仅支持文件路径。

---

## 八. 当前结论

Phase G.3 的第一实施目标应是：

1. `Package` 容器读取跑通 PFDW/WPK/FLS。
2. `TCP` 跑通 assets 中 SP 样本。
3. `Map` 跑通 assets 中 M1.0 基础信息和 block 读取。
4. `IGS` 先完成 Probe 和接口，等待真实样本补全 Decode 验证。

这样能最大化利用现有 `assets` 样本，同时避免在缺少样本的格式上做不可验证实现。
