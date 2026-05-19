# Phase G.3 — 资源格式插件集成 (ALIGNMENT)

> **创建日期**: 2026-05-19  
> **状态**: Align 阶段  
> **目标**: 将外部 WDF/WPK/FLS/IGS/TCP/MAP 解析能力以 ChocoLight 插件形式纳入引擎

---

## 一. 项目上下文

### 1.1 ChocoLight 现有插件结构

当前引擎已有 `Light.Plugins` 命名空间，相关入口包括：

```lua
require("Light.Plugins.WDFData")
require("Light.Plugins.NEMData")
require("Light.Plugins.Codec")
require("Light.Plugins.Path")
require("Light.Plugins.UUID")
require("Light.Plugins.JSON")
require("Light.Plugins.Compress")
```

现有约定来自 `docs/api/Light_Plugins.md`：

- 二进制输入/输出优先使用 Lua string。
- 运行时失败返回 `nil, err`。
- 参数类型错误使用 Lua 原生参数检查抛错。
- `require("Light.Plugins.Xxx")` 返回的表与 `Light.Plugins.Xxx` 是同一个表。

已有 `Light.Plugins.WDFData` 仍保留兼容，不作为本阶段强制替换对象。

### 1.2 现有可复用基础设施

- `third_party/tiny_aes.c`：可复用 AES-128-ECB。
- `miniz.c / miniz_tdef.c / miniz_tinfl.c`：可复用 zlib inflate/deflate。
- `third_party/stb_impl.c`：可复用图像解码能力。
- `light_lua_helpers.h`：已有 userdata magic/type-safety 约定。
- `lumen-master/src/light/light.cpp`：需要新增 Lua 模块注册项。
- `ChocoLight/include/light.h`：需要新增 `luaopen_Light_Plugins_*` 声明。
- `ChocoLight/CMakeLists.txt`：需要新增源文件。

### 1.3 必须遵守的模块注册约束

新增 `Light.Plugins.*` 模块时，不使用：

```cpp
luaL_register(L, "Light.Plugins.Xxx", funcs)
```

原因是 ChocoLight 的 `_G.Light` 是特殊 OOP/static module 对象，点号注册会触发 `object is a static module`。正确方式是创建/取得表后对栈顶 table 调 `luaL_setfuncs`，或复用项目现有 helper。

---

## 二. 外部解析参考

### 2.1 tcp_viewer0302 Python 项目

路径：

```text
D:\TCP_view\tcp_viewer0302\tcp
```

关键参考文件：

| 格式 | 文件 | 作用 |
|---|---|---|
| WDF | `package_preview.py`, `wdf_crypto.py`, `wdf_packer.py` | WDF 子类型识别、索引解密、条目数据解密 |
| WPK | `wpk_format.py`, `wpk_unpacker.py`, `xyq_decrypt.py` | IDX/WPK 分卷读取、AC/XC 解密、THX 辅助 |
| FLS | `formats/mx_fls.py` | `0SLF` 包头、索引解密、数据读取 |
| IGS | `formats/mx_igs.py` | IGS 头、帧表、调色板、RLE 解码 |
| TCP | `convert_spr.py`, `lazy_decoder.py` | SP/TP/RP 头、帧偏移表、调色板、RLE/overlay 解码 |
| MAP | `ui/map_core.py`, `ui/mapx_jpeg.py` | M1.0/MAPX/M2.5/M3.0/ROL0 解析、tile block、JPEG 修复 |

### 2.2 gmx_reconstructed C++ 项目

路径：

```text
E:\jinyiNew\mx_parser\lib\gmx_reconstructed
```

关键参考文件：

| 文件 | 作用 |
|---|---|
| `gmx_common.h` | FLS/IGS/MAP 结构体与 magic 定义 |
| `gmx_fls.cpp` | gmx 风格 FLS Lua C 扩展还原 |
| `gmx_igs.cpp` | gmx 风格 IGS Lua C 扩展还原 |
| `gmx_map.cpp` | gmx 风格 MAP tile/JPEG/MASK/CELL 解析与 SDL surface 输出 |

`gmx_map.cpp` 与 `tcp_viewer0302/ui/map_core.py` 是两条不同 MAP 线索。前者固定 320x240 tile，后者支持 M1.0/MAPX/M2.5/M3.0/ROL0。

---

## 三. 测试资源上下文

用户提供测试资源目录：

```text
E:\jinyiNew\Light\assets
```

当前静态探测结果：

| 文件 | 前 16 字节 / magic | 覆盖点 |
|---|---|---|
| `wzife.wdf` | `50 46 44 57` / `PFDW` | WDF/PFDW 容器读取 |
| `addon.idx` | `53 4B 50 57` / `SKPW` | WPK IDX 索引 |
| `addon0.wpk` | 分卷数据 | WPK archive 数据读取 |
| `interface.fls` | `30 53 4C 46` / `0SLF` | FLS 容器读取 |
| `magic.fls` | `30 53 4C 46` / `0SLF` | FLS 容器读取 |
| `剑侠客.tcp` 等 | `53 50` / `SP` | TCP-SP 解码 |
| `1001.map` | `30 2E 31 4D` / `0.1M` | M1.0/map_core 路径 |

重要结论：`1001.map` 不是 `gmx_map.cpp` 里的 PNAM/MANP 风格样本，而是 `M1.0` 的小端/反序表现。当前 assets 目录暂未发现可验证 `gmx_map` 的 PNAM/MANP 样本。

---

## 四. 原始需求与边界

### 4.1 用户目标

阅读并掌握外部项目中 WDF、WPK、TCP、MAP、FLS、IGS 等格式解析逻辑，然后集成到 ChocoLight 引擎中，形成稳定 Lua 插件 API。

### 4.2 本阶段做

- 新增统一资源包读取层：WDF/WPK/FLS。
- 新增精灵/图像解码层：TCP-SP/TP/RP、IGS。
- 新增 MAP 解析设计：优先覆盖 assets 中的 M1.0，保留 gmx_map 子类型扩展点。
- 复用已有 AES、zlib、stb/miniz 依赖。
- 使用 `E:\jinyiNew\Light\assets` 做本地测试资源。
- 运行时错误统一返回 `nil, err`。
- 编写 smoke 测试，测试资源不存在时跳过大样本验证。

### 4.3 本阶段不做

- 不支持 NXPK。
- 不支持 MHWD。
- 不把 Python 代码作为运行时依赖。
- 不引入新大型第三方依赖。
- 不在第一阶段实现完整 MAP 编辑器能力。
- 不在第一阶段把所有解码结果直接上传 GPU。
- 不提交 `assets/` 大文件到 git。

---

## 五. 已确认决策

### D1 ｜ NXPK/MHWD 范围

**决策**：不支持。  
**处理方式**：如果探测到对应 magic，返回明确错误：

```lua
nil, "unsupported WDF subtype: NXPK"
```

### D2 ｜ SFDW/WDFS 关系

**决策**：实现上合并，元数据上保留原始 magic。

- `SFDW` 与 `WDFS` 使用同一索引/数据解密逻辑。
- `GetInfo().subtype` 返回实际文件头。
- 内部 parser kind 使用 `SFDWLike`。

### D3 ｜ API 分层

**决策**：采用分层插件，而不是把全部功能塞进现有 `WDFData`。

- `Light.Plugins.Package`：容器读取。
- `Light.Plugins.TCP`：TCP 解码。
- `Light.Plugins.IGS`：IGS 解码。
- `Light.Plugins.Map`：MAP 探测与解析。

### D4 ｜ 现有 `WDFData` 兼容

**决策**：不破坏 `Light.Plugins.WDFData` 现有 API。本阶段新增 `Package` 作为更通用入口。

### D5 ｜ 二进制返回类型

**决策**：新 API 优先返回 Lua string。理由：与 `docs/api/Light_Plugins.md` 的二进制数据约定一致，便于组合 `Codec/Compress/Crypto`。

### D6 ｜ MAP 实现顺序

**决策**：先实现可由 assets 验证的 M1.0 基础读取，再保留 gmx_map 子类型扩展。

- `1001.map` 用于 M1.0 探测和 tile block 解析验证。
- gmx_map 的 PNAM/MANP 路径需要用户后续提供样本或从其他资源中定位样本。

### D7 ｜ 测试资源路径

**决策**：smoke 使用环境变量优先：

```text
LIGHT_TEST_ASSETS=E:\jinyiNew\Light\assets
```

未设置时尝试项目根目录 `assets/`；目录不存在时跳过大资源验证。

---

## 六. 验收标准

- `Package.Open` 可识别并打开 `PFDW/SKPW/0SLF` 测试资源。
- `Package.List` 能返回条目数量和基础元数据。
- `Package.Read*` 能读取条目 bytes，并按格式执行必要解密。
- `TCP.Probe` 能识别 assets 中 `.tcp` 的 SP 类型。
- `TCP.Decode` 至少能解出 SP 样本的帧元数据和 RGBA bytes。
- `Map.Probe` 能识别 `1001.map` 为 M1.0。
- NXPK/MHWD 返回明确 unsupported 错误。
- 新模块能通过 `require("Light.Plugins.Xxx")` 加载。
- smoke 在无 assets 环境不失败，在有 assets 环境执行完整样本验证。

---

## 七. 风险与约束

- `map_core.py` 的 MAP 支持范围大，M1.0/MAPX/M2.5/M3.0/ROL0 不应一次性全量实现。
- `gmx_map.cpp` 的 magic 注释和常量存在字节序疑点，必须用真实样本验证。
- WDFX/WDFH/SFDW/WDFS 当前 assets 未覆盖，需要后续补样本。
- IGS 当前 assets 未发现直接 `.igs` 文件，可能需要从 FLS/WDF 中提取样本再验证。
- WPK 的 AC/XC 解密与压缩自动探测必须保守，避免误解密正常数据。
