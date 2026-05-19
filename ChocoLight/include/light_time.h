// =============================================================================
//  light_time.h - Phase H.0 Tick-Render 解耦
// =============================================================================
//
// 文件用途:
//   暴露 LT::TickRender 命名空间给主循环 (light_ui.cpp) 与物理子系统调用.
//   实现位于 light_time.cpp 末尾 (与 Phase AR Light.Time SDL3 timer 共存).
//
// 设计要点 (详见 docs/Phase H.0 Tick-Render Decouple/DESIGN_PhaseH_0.md):
//   1) 全局单例 (主循环只有一个), 非线程安全 — Lua VM 单线程.
//   2) accumulator 累积 wall-clock; 每超过 fixedDt 触发一次 fixed step.
//   3) maxFixedStepsPerFrame / frameTimeClamp 双重 spiral guard.
//   4) alpha = accumulator/fixedDt 供 OnRender 做状态 lerp.
//
// 用例:
//   主循环每帧入口调 BeginFrame(); 之后 while(ShouldStepFixed()) { 物理 +
//   Lua OnFixedUpdate; ConsumeFixedStep(); } 然后 FinalizeFrame() 计算 alpha.

#ifndef CHOCOLIGHT_LIGHT_TIME_H
#define CHOCOLIGHT_LIGHT_TIME_H

namespace LT {
namespace TickRender {

// ---------------------------------------------------------------------------
// 编译期常量 — 默认值与边界 (DESIGN §7.1)
// ---------------------------------------------------------------------------
constexpr double kDefaultFixedDt              = 1.0 / 60.0;   // 60 Hz
constexpr int    kDefaultMaxFixedStepsPerFrame = 8;            // spiral guard
constexpr double kDefaultFrameTimeClamp       = 0.25;          // 250 ms
constexpr int    kFixedHzMin                  = 1;
constexpr int    kFixedHzMax                  = 1000;
constexpr int    kMaxStepMin                  = 1;
constexpr int    kMaxStepMax                  = 64;
constexpr double kFrameClampMin               = 0.01;          // 10 ms
constexpr double kFrameClampMax               = 1.0;           // 1 s
constexpr double kAccumulatorMaxFactor        = 4.0;           // acc 上限 = fixedDt * factor

// ---------------------------------------------------------------------------
// 主循环驱动接口 (light_ui.cpp 调)
// ---------------------------------------------------------------------------

// 重置全部状态 (Window:Open 时调一次, 防止上次会话残留).
void Init();

// 主循环退出时调; 当前实现等同 Init() (无资源释放).
void Shutdown();

// 每帧主循环入口调一次. 内部:
//   - 计算 frameTime = now - lastTime (首帧 0)
//   - clamp(frameTime, 0, frameTimeClamp)
//   - accumulator += frameTime
//   - 重置 lastFixedStepCount = 0
void BeginFrame();

// 是否还需要再做一次 fixed step?
//   返回 true 当且仅当 accumulator >= fixedDt && lastFixedStepCount < maxFixedStepsPerFrame
bool ShouldStepFixed();

// 消费一次 fixed step: accumulator -= fixedDt; lastFixedStepCount++
// 用户在调完 Lua OnFixedUpdate 之后调.
void ConsumeFixedStep();

// 渲染阶段开始前调; 计算 alpha = accumulator/fixedDt 并 clamp 累积器.
// 触发 / 退出 spiral 时打 WARN log (节流, 每次状态切换打一次).
void FinalizeFrame();

// ---------------------------------------------------------------------------
// 查询 / 配置 (Lua wrapper 也调这些)
// ---------------------------------------------------------------------------

double GetFixedDt();
int    GetFixedHz();                          // round(1.0 / fixedDt)
int    GetMaxFixedStepsPerFrame();
double GetFrameTimeClamp();
double GetAlpha();                            // [0, 1)
double GetAccumulator();
int    GetLastStepCount();
double GetLastFrameTime();                    // 上一帧 frameTime, 已 clamp

// 越界值会被 clamp 并 log WARN (友好降级, 不 raise);
// hz=0 / 负数 → clamp 到 kFixedHzMin
void   SetFixedHz(int hz);
void   SetMaxFixedStepsPerFrame(int n);
void   SetFrameTimeClamp(double s);

// ---------------------------------------------------------------------------
// HUD overlay (Phase H.0.1) — 引擎内置调试 HUD 状态
// ---------------------------------------------------------------------------
// 仅存储状态; 实际 DrawHUD 在 Lua 端走 Light.Graphics.DrawText 实现.
// C++ 侧只负责 enabled / position 状态的 round-trip + 默认值.
//
// 默认: enabled=false, x=10, y=10
void   SetHUDEnabled(bool enabled);
bool   GetHUDEnabled();
void   SetHUDPosition(float x, float y);
void   GetHUDPosition(float* outX, float* outY);  // 出参; 任一可为 nullptr

}  // namespace TickRender

// =============================================================================
//  PhysicsRegistry — 全局物理 World 列表 (Phase H.0 T4A/T4B)
// =============================================================================
//
// 用例:
//   light_physics.cpp  / light_physics3d.cpp 在 World create/destroy 时调
//   RegisterWorld / UnregisterWorld; 主循环调 StepAllAuto(fixedDt).
//
//   stepFn 是类型擦除的回调: void (*)(void* worldPtr, double dt)
//   各物理子系统提供自己的 thunk (内部 cast 回 b2World* / btDynamicsWorld*).
//
//   autoStep 默认 false — 老 sample 零回归; 用户主动 SetAutoStep(true) 启用.
namespace PhysicsRegistry {

using StepFn = void (*)(void* world, double dt);

// 注册一个 World. world 指针作为 key. 重复注册同一指针 → 更新 stepFn (idempotent).
void RegisterWorld(void* world, StepFn stepFn);

// 注销 World. 不存在的 world 静默忽略 (idempotent).
void UnregisterWorld(void* world);

// 设置 autoStep 标志. world 不存在 → 静默忽略.
void SetAutoStep(void* world, bool on);
bool GetAutoStep(void* world);

// 主循环调; 遍历 autoStep=true 的 World 调 stepFn(world, dt).
// 列表为空 / 全部 autoStep=false 时立即返回 (零开销).
void StepAllAuto(double dt);

// 运行时统计 (诊断用)
int  GetWorldCount();
int  GetAutoStepWorldCount();

}  // namespace PhysicsRegistry

}  // namespace LT

#endif  // CHOCOLIGHT_LIGHT_TIME_H
