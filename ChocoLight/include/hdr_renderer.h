/**
 * @file hdr_renderer.h
 * @brief Phase E.3.2 — HDR 离屏渲染管线 (RGBA16F RT + ACES tonemap)
 *
 * 设计原则 (与 BatchRenderer / LitBatchRenderer 同风格, 独立命名空间):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口
 *     (SupportsHDR / CreateHDRFBO / DeleteHDRFBO / DrawTonemapFullscreen).
 *   - 显式开启: 不默认启用; Lua 调用 Light.Graphics.HDR.Enable(w, h) 才启用.
 *   - 向后兼容: 未 Enable 时所有 API 静默 no-op (LDR 路径正常工作).
 *   - Legacy 后端不支持: Enable() 返回 false + warn log.
 *
 * 管线流程 (HDR Enabled):
 *   BeginFrame → BindFBO(HDR_RT) → Clear → SetViewport(w, h)
 *     ↓ Lua Draw (所有 sprite/lit/几何绘到 HDR RT, RGBA16F, 可 > 1.0)
 *   EndScene → UnbindFBO() → DrawTonemapFullscreen(sceneTex, exposure, gamma)
 *     ↓ ACES shader: hdr × exposure → ACES fitted → sRGB encode → default fb
 *   SwapBuffers
 *
 * SetCanvas 兼容:
 *   - 用户调 Light.Graphics.SetCanvas(userCanvas): 切到 user FBO, HDR 状态保留但 "paused"
 *   - 用户调 Light.Graphics.SetCanvas(nil): 如果 HDR 启用, 自动 BindFBO(HDR_RT) 恢复
 *
 * 异常处理:
 *   - backend->SupportsHDR() = false → Init 返回 false, 后续所有 API no-op
 *   - Enable(w, h) 失败 (FBO 不完整 / OOM) → 清理后返回 false, IsEnabled = false
 *   - Resize(0,0) / Resize(负数) → 返回 false + warn
 */

#pragma once

#include <cstdint>
#include <cstddef>  // Phase F.0.10.8 CI fix: size_t (GCC/Clang strict, MSVC 隐式)
#include <vector>   // Phase G.1 — ParseCubeLUTFromString / ParseHaldLUTFile out 参数

class RenderBackend;
// Phase E.14 — 前向声明，避免拉 render_backend.h 进 header (依赖面)
enum class VelocityFormat : uint8_t;

namespace HDRRenderer {

// ==================== 生命周期 ====================

/**
 * @brief 初始化 HDR 模块, 绑定 RenderBackend
 *
 * 只做绑定, 不创建 RT. 真正创建 RT 在 Enable(w, h).
 * 仅在 backend->SupportsHDR() = true 时返回 true.
 *
 * @param backend 必须非空
 * @return true = 后端支持 HDR 且绑定成功; false = 不支持 / 已初始化
 */
bool Init(RenderBackend* backend);

/// 释放 HDR RT (若已 Enable), 解绑 backend
void Shutdown();

/// 是否已调用 Init (不等于 HDR 启用)
bool IsInited();

// ==================== HDR 开关 ====================

/**
 * @brief 启用 HDR: 创建 RGBA16F RT
 *
 * 允许在同一进程多次 Enable (先 Disable 再 Enable) 以变更 RT 尺寸;
 * 等价于 Resize(w, h).
 *
 * @return true = RT 创建成功, 主循环下一帧起走 HDR 路径;
 *         false = backend->SupportsHDR() = false / Init 未调 / FBO 不完整
 */
bool Enable(int w, int h);

/// 关闭 HDR: 释放 RT; 后续主循环走 LDR 路径
void Disable();

/// HDR 当前是否启用 (Enable 成功 + 未 Disable)
bool IsEnabled();

/// backend->SupportsHDR() 转发 (Enable 前可查询; Init 未调时返回 false)
bool IsSupported();

/**
 * @brief 调整 HDR RT 尺寸 (窗口 resize 时调用方主动调用)
 *
 * 内部实现: Disable() + Enable(w, h). 如果之前未 Enable, 这调用等价于 Enable.
 * @return true 成功; false 资源创建失败或非法尺寸
 */
bool Resize(int w, int h);

// ==================== 主循环 hook (由 light_ui.cpp::Window_Call 调) ====================

/**
 * @brief 帧开始: 绑定 HDR RT + 清空 + 设置 viewport
 *
 * 必须在 backend->BeginFrame() 之后、Lua Draw 之前调用.
 * HDR 未启用或 paused 时静默 no-op.
 */
void BeginScene();

/**
 * @brief 帧结束: 解绑 HDR RT → tonemap blit 到 default framebuffer
 *
 * 必须在 BatchRenderer/LitBatchRenderer::EndFrame() 之后、SwapBuffers 之前调用.
 * HDR 未启用或 paused 时静默 no-op.
 */
void EndScene();

// ==================== 曝光 / Gamma ====================

/// 线性曝光预乘 (默认 1.0); LDR 模式下写入值但不影响渲染
void  SetExposure(float v);
float GetExposure();

/// sRGB encode gamma (默认 2.2); 同上
void  SetGamma(float v);
float GetGamma();

// ==================== Phase E.3.4 — Tonemap Operator ====================

/// Tonemap operator 常量 (与 shader uTonemapMode int 值对齐)
enum Tonemapper {
    TONEMAP_ACES       = 0,   ///< Narkowicz 2016 fitted (默认, 电影感)
    TONEMAP_REINHARD   = 1,   ///< x/(1+x) (简单基线)
    TONEMAP_UNCHARTED2 = 2,   ///< Hable filmic (含 white scale)
    TONEMAP_LINEAR     = 3,   ///< clamp(x, 0, 1) (调试用, 等同 LDR clip)
};

/// 设置 tonemap operator (无效 mode 静默回退 ACES)
void SetTonemapper(int mode);

/// 当前 operator (0..3)
int  GetTonemapper();

// ==================== 高级查询 ====================

/// 当前 HDR RT 的颜色纹理 id (Enable 未调时 = 0)
uint32_t GetSceneTexture();

/// 当前 HDR RT 的 FBO id (Enable 未调时 = 0); Phase E.8.x — SSAO/调试拿 normal tex
uint32_t GetFBO();

/// 当前 HDR RT 的 velocity 纹理 id (Enable 未调或后端不支持时 = 0)
uint32_t GetVelocityTexture();

/// Phase E.16 — 当前 HDR RT 的 camera-only velocity 纹理 id (slot 3)
/// Enable 未调 / 后端不支持 / CreateHDRFBO 未传 outCameraVelocityTex 时 = 0
uint32_t GetCameraVelocityTexture();

/// Phase E.18 — 当前 HDR RT 的 dilated combined-velocity 纹理 id
/// EndScene 内 dilation pass 输出, 与 raw velocityTex 同尺寸 RG16F, 已 decode 的 float
/// 消费者 (MotionBlur / SSR Temporal) 优先取此 tex 走单点采样, fallback 到 GetVelocityTexture
/// 返 0: dilation pass 未支持 / dilation 未开启 / Enable 未调 / dilatedFbo 创建失败
uint32_t GetDilatedVelocityTexture();

/// Phase E.18 — 当前 HDR RT 的 dilated camera-only velocity 纹理 id
/// 与 GetDilatedVelocityTexture 同, 但对应 camera-only velocity (Phase E.16 双 MRT)
/// 返 0: dilation pass 未支持 / 未开启 / cameraVelocityTex 未创建 / dilatedCameraFbo 创建失败
uint32_t GetDilatedCameraVelocityTexture();

// ==================== Phase E.14 — Velocity dilation + 存储格式切换 ====================

/// dilation 开关：SSRTemporal 采样 velocity 时是否用 3x3 max-length 邻域（默认 ON）
/// @return true = 设置成功; false = backend 未初始化。
/// 切换不重建 RT，仅修改后端状态 + 下一帧 SSRTemporal draw 会读取。
bool          SetVelocityDilation(bool on);
bool          GetVelocityDilation();

/// Phase E.18.1 — dilation pass 半分辨率开关（默认 OFF / full-res）
/// halfRes=true: dilatedTex 尺寸 = ((W+1)/2, (H+1)/2), VRAM -75% / dilation pass perf +4×
/// halfRes=false: dilatedTex 与 raw velocityTex 同尺寸 (Phase E.18 默认行为)
/// 仅在 dilation pass 启用 (SetVelocityDilation(true) + backend 支持) 时有意义;
/// 否则字段被保存但不影响渲染 (dilation RT 未创建)。
/// 切换时若 HDR 已 Enable → 立即释放并重建 dilated RT (双 RT 同步切换)
/// @return true = 设置成功 (含 no-op 同值); false 不会返回
bool          SetVelocityDilationHalfRes(bool on);
bool          GetVelocityDilationHalfRes();

/// Phase E.18.2 — dilation pass 自动跳过单消费者场景（默认 OFF）
/// autoSkip=true: HDR EndScene 内检测 "仅 SSR Temporal 启用且 Motion Blur 未启用" 时,
///                本帧跳过 DrawVelocityDilate (consumer fallback inline 9-tap, 省 1 fetch/px)
/// autoSkip=false: 维持 Phase E.18.1 行为, 无视 consumer 数量始终运行 dilation pass
/// 受益场景: 仅 SSR Temporal 单消费者; 其他场景 (仅 MB / SSR+MB / 都不启) autoSkip 不会跳过
/// 切换不重建 RT (仅本帧执行决策, RT 与 SetVelocityDilation 生命周期一致)
/// @return true = 设置成功 (含 no-op 同值)
bool          SetVelocityDilationAutoSkip(bool on);
bool          GetVelocityDilationAutoSkip();

/// Velocity 存储格式切换（RG16F 默认 vs RG8 低精度节 VRAM）
/// @return true = 切换成功（含 RT 重建）; false = 后端未启 / 重建失败。
/// HDR 未 Enable 时仅更新 state，下次 Enable 时生效。切换会隐含重置 velocity history。
bool           SetVelocityFormat(VelocityFormat fmt);
VelocityFormat GetVelocityFormat();

// ==================== Phase F.0.10.2 — Auto-TAA 开关 (split-screen 必备) ====================

/// 是否在 EndScene 内自动调 TAARenderer::Process() 全屏处理.
/// 默认 true (零回归); 设 false 后用户需手动 TAA.Process / TAA.ProcessRegion 控制 TAA 时序.
/// 典型用法 (split-screen 多 instance):
///   HDR.SetAutoTAA(false)
///   HDR.BeginScene()
///   -- 渲染 player 1 (viewport 左半) + TAA.SetActiveInstance(1) + TAA.Process(0,0,W/2,H)
///   -- 渲染 player 2 (viewport 右半) + TAA.SetActiveInstance(2) + TAA.Process(W/2,0,W/2,H)
///   HDR.EndScene()  -- bloom/tonemap 仍跑, TAA 跳过
/// @return true 设置成功 (含 no-op 同值)
bool SetAutoTAA(bool on);
bool GetAutoTAA();

/// Phase F.0.10.3 — 是否在 EndScene 内自动调 BloomRenderer::Process() 全屏处理.
/// 默认 true (零回归); 设 false 后用户需手动 Bloom.Process / Process(rgn) 控制时序.
/// split-screen 多 player 各自独立 bloom 时必关.
bool SetAutoBloom(bool on);
bool GetAutoBloom();

/// Phase F.0.10.3 — 是否在 EndScene 内自动调 SSRRenderer::Process() 全屏处理.
/// 默认 true (零回归); 设 false 后用户需手动 SSR.Process / Process(rgn) 控制时序.
bool SetAutoSSR(bool on);
bool GetAutoSSR();

/// Phase F.0.10.3 — 是否在 EndScene 内自动调 MotionBlurRenderer::Process() 全屏处理.
/// 默认 true (零回归); 设 false 后用户需手动 MotionBlur.Process / Process(rgn) 控制时序.
bool SetAutoMotionBlur(bool on);
bool GetAutoMotionBlur();

// ==================== Phase F.0.10.6 — Auto-Tonemap + per-region Tonemap ====================

/// Phase F.0.10.6 — 是否在 EndScene 内自动调 DrawTonemapFullscreen() 全屏处理.
/// 默认 true (零回归); 设 false 后用户需手动 HDR.Tonemap(rgn) 控制时序与每 region 不同 tonemap params.
/// 典型用法 (split-screen 多 instance, P1 黄昏 vs P2 冷夜):
///   HDR.SetAutoTonemap(false)
///   HDR.BeginScene()
///   ...
///   HDR.EndScene()
///   HDR.Tonemap(0,    0, W/2, H, {exposure=1.5, tonemap="aces"})
///   HDR.Tonemap(W/2,  0, W/2, H, {exposure=0.6, tonemap="uncharted2"})
bool SetAutoTonemap(bool on);
bool GetAutoTonemap();

/// Phase F.0.10.6 — Region 限定 tonemap pass (split-screen multi-instance 必备)
/// 用全局 g.exposure / g.gamma / g.tonemap (含 AE 叠加, 与 EndScene 一致行为)
/// HDR 未启用 / sceneTex 为 0 时 silent skip
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH);

/// Phase F.0.10.6 — Region 限定 tonemap pass (params 显式版)
/// exposure / gamma / tonemapMode 完全自定义, 不叠加 AE
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH,
              float exposure, float gamma, int tonemapMode);

/// Phase F.0.10.8 — Region 限定 tonemap pass (params + LUT 完全显式版)
/// 与上一版相同, 加 lutTex / lutStrength 覆盖全局 LUT 状态.
/// lutTex=0 或 lutStrength=0 时 shader 跳过 LUT (uniform branch 短路).
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH,
              float exposure, float gamma, int tonemapMode,
              uint32_t lutTex, float lutStrength);

// ==================== Phase F.0.10.8 — 3D LUT (Color Grading) ====================

/// Phase F.0.10.8 — 创建 3D LUT 纹理 (RGB8 + LINEAR + CLAMP_TO_EDGE).
/// 内部走 backend->CreateLUT3D + 入参校验.
/// @param size      LUT 边长 [4, 64]
/// @param data      size^3 * 3 字节 RGB 数据 (R 变化最快)
/// @param dataLen   data 长度 (字节), 必须 = size^3 * 3
/// @return          GL texture id (> 0 = 成功; 0 = 失败 — backend 不支持 / 入参错 / OOM)
uint32_t CreateLUT3D(int size, const uint8_t* data, size_t dataLen);

/// Phase F.0.10.8 — 删除 LUT3D 纹理 (与 CreateLUT3D / CreateLUT3DFloat 配对). 0 = silent fail.
bool DeleteLUT3D(uint32_t lutTex);

/// Phase F.0.10.8.6 — 探测 backend 是否真支持 HDR LUT (RGB16F).
///
/// 透传 backend->SupportsLUT3DFloat(). 用途:
///   - Lua 用户 UI 显示 "HDR LUT supported" / "fallback to RGB8 (clamped)"
///   - 美术决策提供 HDR .cube vs LDR .cube
///   - 自动测试可探测是否预期 HDR 路径生效
///
/// 注: 即使返 false, LoadCubeLUT / LoadHaldLUT 对 HDR domain / 16-bit 输入仍可用,
///     内部自动 fallback 到 RGB8 + clamp (warn log 提示精度损失).
///
/// @return true 当 backend 实现 RGB16F + 3D texture 上传; false 当 legacy / backend 未初始化.
bool SupportsHDRLUT();

// ==================== Phase F.0.10.9 — Multi-Instance HDR API ====================
//
// HDRRenderer 多实例支持. 老 100+ fn 默认作用于 default instance (id=0).
// 用 SetActiveInstance(id) 切换 active 后, 老 fn 透明作用于新 instance.
//
// 设计:
//   - id=0 = default singleton, 永远占用, 不可销毁 (老 API 完全等价 F.0~F.0.10.8.6)
//   - id ∈ [1, 3] = 用户创建的 instance 槽 (split-screen 4 人足够)
//   - 每 instance 独立: FBO/sceneTex/exposure/gamma/tonemap/autoXXX/dilation RT/LUT 应用
//   - 全局共享 (跨 instance): LUT watch list / reload callback / hotReload 开关
//
// 典型用法 (split-screen 双人不同分辨率):
//   HDR.Enable(1920, 1080)             -- default instance: 主屏 1080p
//   local pip = HDR.CreateInstance()
//   HDR.SetActiveInstance(pip)
//   HDR.Enable(640, 360)               -- PIP instance: 360p
//   ...
//   HDR.SetActiveInstance(0)           -- 切回主屏

/// Phase F.0.10.9 — 创建新 HDR instance. 返 id ∈ [1, MAX_INSTANCES-1]; 槽满返 0.
/// 不调 backend; 新 instance 继承 default 的 backend/supported/inited, 但 enabled=false.
int  CreateInstance();

/// Phase F.0.10.9 — 销毁 instance. 释放该槽 RT + 标空闲. id=0 拒绝.
/// 若 active 是该 id 自动切回 default (id=0).
bool DestroyInstance(int id);

/// Phase F.0.10.9 — 切换 active instance. id 必须已分配, 否则 false.
/// 切换后所有老 fn 透明作用于新 instance.
bool SetActiveInstance(int id);

/// Phase F.0.10.9 — 当前 active instance id.
int  GetActiveInstance();

/// Phase F.0.10.9 — 已分配 instance 数 (>=1, default 永远占用).
int  GetInstanceCount();

/// Phase F.0.10.9.x.3 — 复制 srcId 全部调参字段 (exposure/tonemap_mode/lut_tex/lut_strength/...) 到新 instance,
/// scene/velocity/lutPrev RT 不复制 (新 instance 待自己 Enable). 用于 1 行 split-screen setup.
/// 失败条件: srcId 非法 / srcId 未分配 / 槽满 → 返 0.
int  CloneInstance(int srcId);

/// Phase F.0.10.8 — 设置全局 grading LUT (作用于 autoTonemap / Tonemap(rgn) 不带 lut 参数路径).
/// strength clamp [0, 1]; lutTex=0 即关 LUT.
bool SetGradingLUT(uint32_t lutTex, float strength);

/// Phase F.0.10.8 — 当前全局 LUT id (0 = 未设).
uint32_t GetGradingLUTId();

/// Phase F.0.10.8 — 当前全局 LUT strength [0, 1].
float    GetGradingLUTStrength();

// ==================== Phase F.0.10.8.1 — `.cube` LUT 文件解析 ====================

/**
 * @brief Phase F.0.10.8.1 — 从 .cube 文件加载 3D LUT (Adobe Cube LUT 1.0 标准)
 *
 * 内部: SDL_LoadFile → LoadCubeLUTFromString → CreateLUT3D
 * 支持:
 *   - LUT_3D_SIZE N (N ∈ [4, 64])
 *   - 注释 (#) + 空行 + LF/CRLF 行尾
 *   - DOMAIN_MIN/MAX 解析但本 phase clamp [0, 1]
 *   - TITLE 忽略
 * 不支持:
 *   - LUT_1D_SIZE → 立即报错
 *   - DOMAIN > 1.0 (HDR LUT) → clamp [0,1]
 *
 * 错误情况 (outErr 总写):
 *   - 文件 I/O 失败
 *   - LUT_1D_SIZE 出现
 *   - LUT_3D_SIZE 缺失或越界 [4, 64]
 *   - 数据行数 mismatch (≠ size^3)
 *   - 数据行非数字
 *
 * @param path    .cube 文件路径 (相对 CWD 或绝对)
 * @param outErr  [out] 错误描述缓冲 (失败时填; 成功时不动)
 * @param errCap  outErr 缓冲容量 (推荐 256)
 * @return        GL texture id (> 0); 0 = 失败 (查 outErr)
 */
uint32_t LoadCubeLUTFile(const char* path, char* outErr, size_t errCap);

/**
 * @brief Phase F.0.10.8.1 — 从内存字符串解析 .cube LUT
 *
 * 与 LoadCubeLUTFile 共享 parser, 仅去掉文件 I/O.
 * 用于 smoke 测试 (in-memory test fixture).
 *
 * @param text     .cube 文件文本内容
 * @param textLen  text 字节数
 * @param outErr   [out] 错误描述
 * @param errCap   outErr 缓冲容量
 * @return         GL texture id (> 0); 0 = 失败
 */
uint32_t LoadCubeLUTFromString(const char* text, size_t textLen,
                                char* outErr, size_t errCap);

/**
 * @brief Phase G.1 — 仅 parse 阶段, 不调 backend (供 worker thread 后台调用)
 *
 * 与 LoadCubeLUTFromString 共享 parser. 输出原始字节/浮点数据 + size + isHDR
 * (是否是 HDR LUT, DOMAIN_MAX > 1.0). 主线程后续根据 isHDR 调
 * backend->CreateLUT3DFloat 或 CreateLUT3D.
 *
 * 设计目的: 让 AssetLoader worker thread 可以无 GL 依赖地完成 cube 解析,
 * 主线程在 Tick 时只做 GL 上传 (~微秒级).
 *
 * @param text       .cube 文件文本
 * @param textLen    text 字节数
 * @param outSize    [out] LUT 边长 [4, 64]
 * @param outIsHDR   [out] true 当 DOMAIN_MAX 任一分量 > 1.0
 * @param outBytes   [out] LDR 路径数据 size^3 * 3 bytes (即使 isHDR=true 也填, 供 RGB16F fallback)
 * @param outFloats  [out] HDR 路径数据 size^3 * 3 floats (原始值)
 * @param outErr     [out] 错误描述
 * @param errCap     outErr 容量
 * @return           true 解析成功; false 失败 (查 outErr)
 */
bool ParseCubeLUTFromString(const char* text, size_t textLen,
                             int* outSize, bool* outIsHDR,
                             std::vector<uint8_t>* outBytes,
                             std::vector<float>* outFloats,
                             // Phase G.1.4 — 可选 domainMax 输出 (nullptr=不写),
                             // 用于 LoadCubeLUTFromString 在 backend 未初始化时
                             // 生成含 domainMax 的兼容错误消息 (与 hdr.lua smoke 契约一致)
                             float outDomainMax[3],
                             char* outErr, size_t errCap);

// ==================== Phase F.0.10.8.2 — HALD CLUT 图像 LUT 加载 ====================

/**
 * @brief Phase F.0.10.8.2 — 从 HALD CLUT 图像 (PNG/JPG/BMP/TGA) 加载 3D LUT
 *
 * HALD CLUT 标准 (ImageMagick / GIMP / Photoshop "Color Lookup"):
 *   - 图像分辨率: N³ × N³ 像素 (level N)
 *   - LUT size = N²
 *   - 像素 raster scan 顺序 = LUT byte 顺序 (R 最快变, 与 GL 3D texture 一致)
 *
 * 支持 level N ∈ [2, 8] → LUT size ∈ [4, 64] (与 F.0.10.8 CreateLUT3D 一致)
 * 主流: N=8 → 512×512 PNG → 64³ LUT (Photoshop 默认)
 *
 * 内部: stbi_load (force RGBA) → 验证方阵 + N³ → drop alpha → CreateLUT3D
 *
 * 错误情况 (outErr 总写):
 *   - 文件 I/O / 解码失败
 *   - 非方阵 (w ≠ h)
 *   - width 不是 N³ for any N ∈ [2, 8]
 *   - LUT size 越界 [4, 64]
 *
 * @param path    PNG/JPG/BMP/TGA 文件路径
 * @param outErr  [out] 错误描述
 * @param errCap  outErr 容量 (推荐 256)
 * @return        GL texture id (>0); 0 = 失败
 */
uint32_t LoadHaldLUTFile(const char* path, char* outErr, size_t errCap);

/**
 * @brief Phase G.1 — 仅 parse 阶段 HALD CLUT 图像 (供 worker thread 后台调用)
 *
 * worker 调 stbi_load(_16) + N^3 维度验证 + 归一化到 floats[].
 * 主线程后续根据 isHDR 调 backend->CreateLUT3DFloat (16-bit) 或 CreateLUT3D (8-bit).
 *
 * @param path       PNG/JPG/BMP/TGA 文件路径
 * @param outSize    [out] LUT 边长 [4, 64]
 * @param outIsHDR   [out] true 当 16-bit PNG (走 RGB16F)
 * @param outBytes   [out] LDR 路径数据 size^3 * 3 bytes
 * @param outFloats  [out] HDR 路径数据 size^3 * 3 floats (16-bit 时填; 8-bit 时空)
 * @param outErr     [out] 错误描述
 * @param errCap     outErr 容量
 * @return           true 解析成功; false 失败
 */
bool ParseHaldLUTFile(const char* path,
                       int* outSize, bool* outIsHDR,
                       std::vector<uint8_t>* outBytes,
                       std::vector<float>* outFloats,
                       char* outErr, size_t errCap);

/**
 * @brief Phase G.1 — 把已 parse 的 LUT 数据上传到 GL backend (主线程调用)
 *
 * 配套 ParseCubeLUTFromString / ParseHaldLUTFile 使用. worker 完成 parse,
 * AssetLoader::Tick 主线程调此 helper 上传 GL.
 *
 * 内部双路径自动 fallback (RGB16F → RGB8 量化).
 *
 * @return GL texture id (>0 成功; 0 失败 - backend 未初始化 / 数据空 / GL OOM)
 */
uint32_t UploadParsedLUT(int size, bool isHDR,
                          const std::vector<uint8_t>& bytes,
                          const std::vector<float>& floats,
                          char* outErr, size_t errCap);

// ==================== Phase F.0.10.8.3 — LUT 热重载 (mtime polling) ====================

/**
 * @brief Phase F.0.10.8.3 — 注册 LUT 文件到 watch list (内部加载 + 跟踪 mtime)
 *
 * 内部自动判扩展名:
 *   - .cube → LoadCubeLUTFile
 *   - .png/.jpg/.jpeg/.bmp/.tga → LoadHaldLUTFile
 *   - 其他 → 默认走 .cube parser
 *
 * 同 path 重复 Watch: 旧 entry + 旧 GL tex 自动释放, 重新注册.
 *
 * @param path    LUT 文件路径
 * @param outErr  [out] 错误描述
 * @param errCap  outErr 容量
 * @return        GL tex id (>0); 0 = 失败 (不加入 watchList)
 *
 * @note 后续调 PollLUTReloads() 触发 mtime 检查 + 自动 reload
 */
uint32_t WatchLUT(const char* path, char* outErr, size_t errCap);

/**
 * @brief 从 watch list 移除 + 删除对应 GL texture
 * @return true = 成功移除; false = id 不在 list (silent)
 * @note 如果 lutTex 是当前 grading 的 LUT, g.gradingLutId 自动清 0
 */
bool UnwatchLUT(uint32_t lutTex);

/**
 * @brief 查询 path 当前的 LUT id (reload 后 path 不变但 id 已变, 用此查最新)
 * @return GL tex id (>0); 0 = path 不在 watchList
 */
uint32_t GetWatchedLUTId(const char* path);

/**
 * @brief 遍历 watch list, 检 mtime 变化 → 自动 reload + 替换 id
 *
 * 行为:
 *   1. 若 g.lutHotReload == false 或 watchList 空 → 返 0
 *   2. 对每个 entry, SDL_GetPathInfo 取 modify_time
 *      - 失败 (文件被锁/移动) → continue, 保留 entry
 *      - mtime 未变 → continue
 *      - mtime 变化: 重 load (复用 LoadCubeLUTFile / LoadHaldLUTFile)
 *        - reload 成功 → DeleteOld + entry.lutId = newId + 若 g.gradingLutId 匹配则自动同步
 *        - reload 失败 → log + 保留 entry (下次再试)
 *
 * @return 本次 reload 成功数 (>= 0)
 *
 * @note 用户控制 poll 频率 (典型每秒 1 次, 或每帧若 watch 量少)
 */
int PollLUTReloads();

/**
 * @brief 全局热重载开关 (默认 true)
 * @note 关闭后 PollLUTReloads 立即返 0, 跳过所有 mtime 调用
 */
void SetLUTHotReload(bool enabled);

/// 查全局热重载开关 (默认 true)
bool GetLUTHotReload();

// ==================== Phase F.0.10.8.4 — LUT 热重载回调 ====================

/**
 * @brief Phase F.0.10.8.4 — LUT reload 回调签名
 *
 * 当 PollLUTReloads() 检测到 mtime 变化并成功 reload 后调用.
 *
 * @param path     已 reload 的 LUT 文件路径 (与 WatchLUT 入参一致)
 * @param oldId    reload 前的 GL tex id (已被 DeleteLUT3D 释放, 仅供日志/UI)
 * @param newId    reload 后的新 GL tex id (== GetWatchedLUTId(path))
 * @param userData 注册时透传的不透明指针 (Lua trampoline 用作 lua_State*)
 *
 * @note 回调期间不应再调 PollLUTReloads (递归); 不应 DeleteLUT3D(newId).
 * @note reload 失败 → 不触发回调 (entry 保留, 下次 Poll 再试)
 */
typedef void (*LUTReloadCallback)(const char* path, uint32_t oldId, uint32_t newId, void* userData);

/**
 * @brief 注册 LUT reload 全局回调 (单一回调, 后注册者覆盖前者)
 * @param cb        回调函数 (nullptr = 取消注册)
 * @param userData  透传给 cb 的指针 (典型: lua_State*)
 *
 * @note 仅一个全局回调位; 多次调以最后一次为准
 * @note Lua 端通过 light_graphics 的 trampoline 间接持 Lua function ref
 */
void SetLUTReloadCallback(LUTReloadCallback cb, void* userData);

/// 查询当前是否注册了 LUT reload 回调
bool HasLUTReloadCallback();

/// 当前 HDR RT 宽度 / 高度 (未 Enable 时 = 0)
int GetWidth();
int GetHeight();

// ==================== Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦 ====================
//
// 设计:
//   - HDR.Enable(w, h) 入参语义不变 (= window/output 尺寸)
//   - 新增内部 hook: TAARenderer::SetTAAUEnabled(true) 时调 OnTAAURenderScaleChanged,
//                     重建 sceneTex 为 render-res, 同时分配 outputSceneTex 为 output-res
//   - sharpen / tonemap 改读 GetSceneTexForOutput() 选择正确的 sceneTex
//   - taauActive=false 时 outputSceneTex 不分配, GetSceneTexForOutput() 返 sceneTex (零回归)

/// Phase F.1 — 由 TAARenderer 调; 通知 HDR 切换到 TAAU 模式 (sceneTex render-res + outputSceneTex output-res)
/// 失败 (OOM / backend 不支持 OutputSceneTex) 时自动回退 F.0 路径 + log error.
/// @param renderW  渲染分辨率宽 (= lround(outputW * renderScale))
/// @param renderH  渲染分辨率高
/// @param outputW  输出分辨率宽 (= 当前 g.width)
/// @param outputH  输出分辨率高 (= 当前 g.height)
/// @return true 切换成功; false 失败 (HDR 未启用 / OOM)
bool OnTAAURenderScaleChanged(int renderW, int renderH, int outputW, int outputH);

/// Phase F.1 — 由 TAARenderer 调; 切回 F.0 单尺寸路径 (sceneTex output-res, 销毁 outputSceneTex)
void OnTAAUDisabled();

/// Phase F.1 — sharpen / tonemap 应用的 sceneTex (TAAU 模式 = outputSceneTex output-res; 否则 = sceneTex)
uint32_t GetSceneTexForOutput();

/// Phase F.1 — sharpen / TAA 写入的目标 FBO (TAAU 模式 = outputSceneFbo; 否则 = HDR fbo).
/// 与 GetSceneTexForOutput 配对: GetSceneTexForOutput 返该 FBO 上挂载的 color tex.
uint32_t GetSceneFboForOutput();

// ==================== SetCanvas 兼容 (由 l_SetCanvas 内部调) ====================

/**
 * @brief 通知 HDR 模块: 用户调 SetCanvas(userFbo), HDR 暂停
 *
 * 不释放 HDR RT, 仅标记 paused; EndScene 仍会执行 tonemap (读 HDR RT 现有内容).
 */
void Pause();

/**
 * @brief 通知 HDR 模块: 用户调 SetCanvas(nil), HDR 恢复
 *
 * 如果 HDR enabled 且之前 paused, 重新 BindFBO(HDR_RT).
 */
void Resume();

/// 当前是否被 SetCanvas 暂停
bool IsPaused();

} // namespace HDRRenderer
