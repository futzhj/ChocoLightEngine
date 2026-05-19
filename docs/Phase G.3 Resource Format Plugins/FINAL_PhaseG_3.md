# Phase G.3 — 资源格式插件集成 (FINAL)

> **创建日期**: 2026-05-19  
> **当前状态**: 设计计划已生成，尚未进入实现阶段

---

## 一. 目标

将外部项目中的 WDF/WPK/FLS/IGS/TCP/MAP 解析能力，以 ChocoLight Lua 插件形式集成到引擎中，形成可测试、可扩展、与现有 `Light.Plugins` 约定一致的资源格式系统。

---

## 二. 已完成设计交付物

| 文档 | 说明 |
|---|---|
| `ALIGNMENT_PhaseG_3.md` | 项目上下文、范围、外部参考、测试资源、已确认决策 |
| `DESIGN_PhaseG_3.md` | 总体架构、Lua API、核心结构、格式解析方案、测试设计 |
| `TASK_PhaseG_3.md` | 原子任务拆分、依赖图、每项输入输出和验收标准 |
| `ACCEPTANCE_PhaseG_3.md` | 验收矩阵、测试资源覆盖、功能与错误处理清单 |
| `TODO_PhaseG_3.md` | 待办、样本缺口、后续增强、用户待确认项 |

---

## 三. 核心设计结论

### 3.1 分层插件

新增四个插件模块：

```lua
Light.Plugins.Package
Light.Plugins.TCP
Light.Plugins.IGS
Light.Plugins.Map
```

职责划分：

- `Package`：WDF/WPK/FLS 容器读取。
- `TCP`：TCP-SP/TP/RP 精灵解码。
- `IGS`：IGS 精灵解码。
- `Map`：MAP 探测与基础读取。

### 3.2 格式范围

支持设计：

- WDF: `PFDW/WDFP/WDFX/WDFH/SFDW/WDFS`
- WPK: `SKPW IDX + WPK 分卷`
- FLS: `0SLF`
- TCP: `SP/TP/RP`
- IGS: `I0/T0/I1/T1/I5/T5`
- MAP: 优先 `M1.0`，后续扩展 MAPX/M2.5/M3.0/ROL0/gmx_map

明确不支持：

- `NXPK`
- `MHWD`

### 3.3 MAP 策略

MAP 分两条线：

- `tcp_viewer0302/ui/map_core.py`：M1.0/MAPX/M2.5/M3.0/ROL0。
- `gmx_reconstructed/gmx_map.cpp`：PNAM/MANP 风格 gmx_map。

当前 assets 中的 `1001.map` 前 4 字节为 `30 2E 31 4D`，对应 `0.1M/M1.0`，因此第一阶段优先实现 M1.0 基础读取。

### 3.4 测试资源

测试资源目录：

```text
E:\jinyiNew\Light\assets
```

主要样本：

- `wzife.wdf`：PFDW。
- `addon.idx/addon0.wpk`：WPK/IDX。
- `interface.fls/magic.fls`：FLS。
- 多份 `.tcp`：TCP-SP。
- `1001.map`：M1.0。

---

## 四. 推荐实施顺序

1. `Package` MVP：PFDW/WPK/FLS Probe/Open/List/GetData。
2. smoke 框架：无 assets 跳过，有 assets 深度验证。
3. `TCP` SP Decode：用 `.tcp` 样本验证 RGBA 帧。
4. `Map` M1.0：用 `1001.map` 验证基础信息和 block。
5. `IGS` Probe/Decode：先接口和 synthetic，等待真实样本。
6. 样本驱动补齐：WDFX/WDFH/SFDW/WDFS、TP/RP、MAPX、gmx_map。

---

## 五. 主要风险

- 缺少 WDFX/WDFH/SFDW/WDFS 样本。
- 缺少 IGS 直接样本。
- 缺少 gmx_map PNAM/MANP 样本。
- MAPX/JPGH/MASK/LZO 解码复杂，必须分阶段处理。
- AC/XC 自动解密必须保守，避免误处理正常数据。

---

## 六. 下一步

等待用户审批设计文档。审批后进入实现计划阶段，按 `TASK_PhaseG_3.md` 拆分执行。

建议第一轮实现目标限制为：

- `Package` PFDW/WPK/FLS MVP。
- `TCP` SP Decode MVP。
- `Map` M1.0 Probe/Open/GetInfo/GetBlock MVP。
- `IGS` Probe MVP。

这样可以最大化使用现有测试资源，并控制风险。
