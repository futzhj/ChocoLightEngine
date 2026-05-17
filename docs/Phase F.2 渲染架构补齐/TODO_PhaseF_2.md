# Phase F.2 渲染架构补齐 — TODO (后续待办) 文档

> **创建日期**：2026-05-17
> **基线**：F.2 完结后

---

## 1. 渲染管线已知未完成项

### 1.1 [可选] F.1.2 Velocity Nearest-Filter
- 触发条件: 真机测试显示 ghost 严重
- 设计: TAA shader 中 velocity 采样从 bilinear 改为 nearest, 防止 dilated motion vectors 被插值平滑

### 1.2 [可选] F.0.11.6 MP4 录屏 worker thread
- 现状: 主线程同步软件 H.264 编码 (~30-50ms/frame @ 1080p)
- 改造: PBO 读取后丢入 worker thread 队列编码

## 2. 本期 F.2 完结后的"下一步可选优化"

### 2.1 5 模块多实例 GetState/SetState 反射
- 与 BloomRenderer 的 GetState/SetState (F.0.10.9.x.3/x.4) 对齐
- 工作量: M (5 × 30 行 = 150 行)
- 价值: 调试 / serialize / split-screen 一键 setup

### 2.2 LensFlare/Streak Render-Res 自适应
- 现状: 多实例已支持, 但 Lens Flare/Streak 内部 ping-pong RT 是 srcW/srcH 入参
- 触发条件: 真机 TAAU 启用时 lens flare/streak 是否视觉缩水？需测试
- 修复: 若 TAAU 启用 + 用户已 Enable, OnHDRResized 时按 renderRes 重建; 关闭后按 outputRes
- 注: 本期 G1 已让 OnHDRResized 收到 renderW/H, 模块内部已经自动重建 RT, 应无需额外改动 (待真机确认)

### 2.3 GetMemoryStats Lua API
- 现状: 5 模块多实例后, 全局 RT 数量翻倍, VRAM 追踪需求上升
- 设计: `Light.Graphics.GetMemoryStats() -> { vramBytes, fboCount, texCount, instances={ HDR=N, TAA=N, ... } }`
- 工作量: M

## 3. 转入非渲染基础架构 (用户已要求)

### 3.1 异步资源加载 (Async Asset Management)
- 现状: Assimp / stb_image 同步阻塞主线程
- 需求: 线程池后台 I/O + 主线程 GL 上传 (或 Shared Context 后台上传)
- 工作量: L

### 3.2 内存与显存管理优化 (VRAM Profiling)
- 同 §2.3, 但需加入 Lua GC ↔ C++ 资源生命周期绑定加固
- 工作量: M-L

### 3.3 多线程与逻辑/渲染解耦 (Tick vs Render)
- 游戏 Tick 60Hz 与渲染解耦 (不限帧 / VSync)
- 未来插帧 / 网络同步的架构基础
- 工作量: L

### 3.4 Lua API 健壮性 + 热重载
- C++ 侧错误传参容错 (避免 crash)
- Lua 脚本热重载 (不仅是 LUT, 包括逻辑代码)
- 工作量: M

## 4. 转入建议优先级

按用户原话 "渲染处理完了再开始非渲染" + HANDOFF §2 顺序:

1. **异步资源加载** (基础)
2. **VRAM 追踪** (诊断必备)
3. **Tick/Render 解耦** (架构基础)
4. **Lua 健壮性 + 热重载** (开发体验)
