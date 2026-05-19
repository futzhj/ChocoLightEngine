# Phase G.3 — 资源格式插件集成 (ACCEPTANCE)

> **依赖**: [ALIGNMENT](ALIGNMENT_PhaseG_3.md) · [DESIGN](DESIGN_PhaseG_3.md) · [TASK](TASK_PhaseG_3.md)  
> **状态**: 设计阶段创建，执行阶段逐项回填

---

## 一. 验收总览

| 项目 | 状态 | 说明 |
|---|---:|---|
| 设计范围确认 | ✅ | NXPK/MHWD 排除；SFDW/WDFS 合并；assets 作为测试资源 |
| 模块 API 设计 | ✅ | Package/TCP/IGS/Map 四模块 |
| 代码实现 | ⏳ | 待进入实现阶段 |
| smoke 测试 | ⏳ | 待实现 `resource_formats.lua` |
| API 文档 | ⏳ | 待实现后同步 `docs/api/Light_Plugins.md` |
| 样本验证 | ⏳ | 待实现后用 `E:\jinyiNew\Light\assets` 验证 |

---

## 二. 测试资源矩阵

测试资源路径：

```text
E:\jinyiNew\Light\assets
```

| 文件 | 类型 | 当前探测 | 验收目标 |
|---|---|---|---|
| `wzife.wdf` | WDF | `PFDW` | Package.Open/List/GetData |
| `addon.idx` | WPK IDX | `SKPW` | Package.Open/List/GetData，自动定位 `addon0.wpk` |
| `addon0.wpk` | WPK 分卷 | 数据分卷 | 被 IDX 条目引用读取 |
| `interface.fls` | FLS | `0SLF` | Package.Open/List/GetData |
| `magic.fls` | FLS | `0SLF` | Package.Open/List/GetData |
| `剑侠客.tcp` 等 | TCP | `SP` | TCP.Probe/Decode 前 1 帧 |
| `1001.map` | MAP | `0.1M` / M1.0 | Map.Probe/Open/GetInfo/GetBlock |

---

## 三. 功能验收清单

### 3.1 Package 模块

- [ ] `require("Light.Plugins.Package")` 成功。
- [ ] `Package.Probe("assets/wzife.wdf")` 返回 `kind="WDF"`。
- [ ] `Package.Open("assets/wzife.wdf")` 返回 handle。
- [ ] `handle:GetInfo()` 返回 `count > 0`。
- [ ] `handle:List()` 返回非空条目表。
- [ ] `handle:GetData(firstKey)` 返回非空 Lua string。
- [ ] `Package.Probe("assets/addon.idx")` 返回 `kind="WPK"`。
- [ ] `Package.Open("assets/addon.idx")` 能定位 `addon0.wpk`。
- [ ] `Package.Probe("assets/interface.fls")` 返回 `kind="FLS"`。
- [ ] `Package.Open("assets/magic.fls")` 成功。
- [ ] `NXPK/MHWD` 样本或 synthetic magic 返回 unsupported。

### 3.2 TCP 模块

- [ ] `require("Light.Plugins.TCP")` 成功。
- [ ] `TCP.Probe(data)` 能识别 SP。
- [ ] `TCP.Decode(data, {maxFrames=1})` 返回 sprite table。
- [ ] 第一帧 `pixels` 长度等于 `width * height * 4`。
- [ ] 非 TCP 数据返回 `nil, err`，不崩溃。

### 3.3 IGS 模块

- [ ] `require("Light.Plugins.IGS")` 成功。
- [ ] synthetic IGS header Probe 成功。
- [ ] 真实样本存在时 Decode 前 1 帧成功。
- [ ] 无真实样本时 smoke 标记 skip，不阻塞整体。

### 3.4 Map 模块

- [ ] `require("Light.Plugins.Map")` 成功。
- [ ] `Map.Probe("assets/1001.map")` 返回 `subtype="M1.0"`。
- [ ] `Map.Open("assets/1001.map")` 成功。
- [ ] `handle:GetInfo()` 返回正宽高、row/col/tile 数。
- [ ] `handle:GetBlock(1)` 返回 block table。
- [ ] `handle:GetCell()` 有 CELL 时返回 string，无 CELL 时返回 `nil, err`。
- [ ] GMX PNAM/MANP 未有样本时返回明确 unsupported/sample required。

---

## 四. 错误处理验收

- [ ] 文件不存在：`nil, "open failed: ..."`。
- [ ] 未知 magic：`nil, "unknown ... format"`。
- [ ] 索引越界：`nil, "invalid index offset"` 或等价明确错误。
- [ ] 条目不存在：`nil, "entry not found"`。
- [ ] unsupported 子类型：错误包含 `unsupported` 与 subtype。
- [ ] 参数类型错误：Lua 原生参数错误。

---

## 五. 安全与鲁棒性验收

- [ ] 所有 count/size 乘法有溢出保护。
- [ ] 所有 offset/size 在文件范围内验证。
- [ ] 解压输出有上限。
- [ ] AES/解密只在确认格式后执行。
- [ ] `Close()` 幂等。
- [ ] `__gc` 能释放资源。
- [ ] 无 assets 时 smoke 不失败。

---

## 六. 文档验收

- [ ] `docs/api/Light_Plugins.md` 新增 Package/TCP/IGS/Map 小节。
- [ ] 文档示例与实际函数签名一致。
- [ ] 明确列出不支持 NXPK/MHWD。
- [ ] 明确说明 `SFDW/WDFS` 合并实现。
- [ ] 明确说明 assets 测试路径和 skip 规则。

---

## 七. 执行记录

> 实现阶段回填。

| 日期 | 项目 | 结果 | 证据 |
|---|---|---|---|
| 2026-05-19 | 设计文档生成 | ✅ | ALIGNMENT/DESIGN/TASK/ACCEPTANCE/TODO |

---

## 八. 最终验收结论

当前仍处于设计阶段，尚未进入代码实现。最终验收必须在实现后补充：

- 本地 smoke 输出。
- assets 样本验证结果。
- CI 结果。
- 未覆盖格式的 TODO 与后续样本需求。
