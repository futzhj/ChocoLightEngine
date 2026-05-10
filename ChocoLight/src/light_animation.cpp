/**
 * @file light_animation.cpp
 * @brief Light.Animation — 3D 骨骼动画 + 动画状态机 (Phase AV Step 1/2/3)
 *
 * 模块布局:
 *   Light.Animation               顶层 (LoadSkinnedGLTF / NewAnimator / DrawSkinnedMesh)
 *   Light.Animation.Skeleton      骨骼层级 (静态, 关节树 + 反向绑定矩阵)
 *   Light.Animation.Clip          动画时间轴 (静态, 一组 sampler)
 *   Light.Animation.Animator      运行时 (sampler 评估 + 关节变换树 + 状态机基础)
 *   Light.Animation.SkinnedMesh   蒙皮网格 (CPU 蒙皮 + 复用 backend CreateMesh) — Step 3
 *
 * 当前覆盖: Step 1 + Step 2 + Step 3
 *   - Step 4 状态机完整化 (Transition/Crossfade/事件帧) 待实施
 *
 * 见 docs/Phase AV 骨骼动画/{ALIGNMENT,CONSENSUS,DESIGN,TASK}_PhaseAV.md
 */

#include "light.h"
#include "render_backend.h"   // Step 3: g_render + RenderVertex3D + MaterialDesc

#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cmath>          // Step 2: sqrt / sin / cos / acos for QuatSlerp + QuatNormalize
#include <cstdint>        // Step 2: uint8_t / uint32_t (computed flags)

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "cgltf.h"   // glTF 2.0 解析 (single-header), 已在 third_party
}

// Step 3: 复用 light_graphics_material.cpp 暴露的 Material userdata 检查
extern "C" const MaterialDesc* CheckMaterialUserdata(lua_State* L, int idx);

// ==================== 常量 ====================

static constexpr int MAX_JOINTS         = 64;        // 关节数硬上限 (uniform 16 KB / Mat4×64)
static constexpr int FLOATS_PER_MAT4    = 16;
static constexpr int FLOATS_T           = 3;         // translation
static constexpr int FLOATS_R           = 4;         // rotation (quat wxyz)
static constexpr int FLOATS_S           = 3;         // scale
static constexpr int MORPH_TARGET_MAX   = 8;         // Phase AX: morph target 上限 (与 shader uniform array 大小一致)

// userdata 元表名 (与 Lua 模块名对应)
static const char* SKELETON_MT      = "Light.Animation.Skeleton";
static const char* CLIP_MT          = "Light.Animation.Clip";
static const char* ANIMATOR_MT      = "Light.Animation.Animator";
static const char* SKINNED_MESH_MT  = "Light.Animation.SkinnedMesh";   // Step 3

// ==================== 数据结构 ====================

namespace LT { namespace Anim {

enum class InterpMode : uint8_t {
    LINEAR      = 0,
    STEP        = 1,
    CUBICSPLINE = 2,
};

enum class ChannelTarget : uint8_t {
    TRANSLATION   = 0,
    ROTATION      = 1,
    SCALE         = 2,
    MORPH_WEIGHTS = 3,    // Phase AX: morph target 权重 (mesh-level, 非 joint)
    UNSUPPORTED   = 255,
};

struct JointNode {
    std::string  name;
    int          parent = -1;        // 父关节索引 (-1=根)
    std::vector<int> children;
    // 绑定姿态本地 TRS (cgltf_node 默认值; 采样器没覆盖时使用)
    float        local_t[FLOATS_T] = {0, 0, 0};
    float        local_r[FLOATS_R] = {0, 0, 0, 1};   // 四元数 wxyz
    float        local_s[FLOATS_S] = {1, 1, 1};
};

struct Sampler {
    int            jointIndex  = -1;       // 索引到 Skeleton::joints (TRS 路径使用)
    int            meshNodeIdx = -1;       // Phase AX: cgltf node 索引 (MORPH_WEIGHTS 路径使用)
    ChannelTarget  target      = ChannelTarget::UNSUPPORTED;
    InterpMode     mode        = InterpMode::LINEAR;
    int            components  = 3;        // 3 (T/S) / 4 (R) / N (MORPH_WEIGHTS, N <= 8)
    std::vector<float> times;              // keyframe 时间, 升序
    std::vector<float> values;             // 对应数据, CUBICSPLINE 时每点 3*components 元素
};

struct Skeleton {
    std::vector<JointNode> joints;
    // 反向绑定矩阵 (cgltf_skin::inverse_bind_matrices), 每关节 16 floats, 列主序
    std::vector<float>     inverseBindMatrices;
    int                    rootJoint = -1;
    std::unordered_map<std::string, int> nameToIndex;
    bool                   alive = true;
};

// Phase AX: morph target delta 数据 (POSITION + 可选 NORMAL/TANGENT delta)
struct MorphTarget {
    std::vector<float> posDelta;   // vCount × 3 floats (必须存在)
    std::vector<float> nrmDelta;   // vCount × 3 floats (可空)
    std::vector<float> tanDelta;   // vCount × 3 floats (可空, glTF spec 要求 vec3)
};

struct AnimationClip {
    std::string          name;
    float                duration = 0.0f;     // 自动取所有 sampler max time
    std::vector<Sampler> samplers;
    bool                 alive = true;
};

// Step 4: 状态转换定义 (按 priority 顺序遍历, 第一个 condFn 返回 true 的触发 crossfade)
struct TransitionDef {
    std::string fromState;     // 空 = 任意 state (Any state); 否则只在该 state 时检查
    std::string toState;
    int         condFnRef  = LUA_NOREF;   // Lua function ref; pcall(animator) → bool
    float       duration   = 0.0f;        // crossfade 时长 (秒); 0 = 立即切换 (无 fade)
};

// Step 4: 事件帧定义 (在 Update 推进 currentTime 时, 判断 triggerTime 是否被跨过)
struct EventDef {
    std::string state;             // 关联 state 名 (必须已 AddState)
    float       triggerTime = 0.0f;
    int         callbackRef = LUA_NOREF;   // Lua function ref; pcall(animator)
};

// Animator (Step 2 完整化: sampler eval + 关节变换树 + 状态机基础)
// Step 4: + Transition / Crossfade / Event / Param
struct Animator {
    Skeleton*  skeletonPtr  = nullptr;
    int        skeletonRef  = LUA_NOREF;     // 防 Skeleton GC 的强引用

    // 时间 / 速率 / 暂停
    bool       paused      = false;
    float      speed       = 1.0f;
    float      currentTime = 0.0f;
    float      prevTime    = 0.0f;     // Step 4: 上一帧 currentTime (供 event 跨帧检测)

    // Step 2: 状态机基础 (单状态切换, 无 Transition; Step 4 引入 crossfade)
    std::unordered_map<std::string, AnimationClip*> states;       // name → clip 指针
    std::unordered_map<std::string, int>            stateRefs;    // name → registry ref (防 clip GC)
    std::string                                     currentState; // 空串 = 未播放
    AnimationClip*                                  activeClip = nullptr;
    bool                                            looping    = true;

    // Step 4: 状态转换 (全局列表, 按 AddTransition 顺序遍历)
    std::vector<TransitionDef> transitions;

    // Step 4: 当前 crossfade (如果在 fade 中)
    std::string    crossfadeTarget;          // 目标 state 名; 空 = 无 crossfade
    AnimationClip* crossfadeClip      = nullptr;
    float          crossfadeClipTime  = 0.0f;     // target clip 的独立 time
    float          crossfadeProgress  = 0.0f;     // [0, 1] 已完成的 fade 比例
    float          crossfadeDuration  = 0.0f;     // 总时长 (秒)

    // Step 4: 同帧 transition 锁 (防止一帧多次切换)
    bool                       transitionedThisFrame = false;

    // Step 4: 事件帧 (按 AddEvent 顺序; Update 时按 prevTime → currentTime 区间触发)
    std::vector<EventDef>      events;

    // Step 4: 参数表 (number 类型; 供 transition condFn 读取)
    std::unordered_map<std::string, float> params;

    // Step 2: 关节矩阵缓存 (Update 时填充, GetJointMatrices 直接读)
    // 布局: 每关节 16 floats (列主序 mat4), 共 N×16 floats
    std::vector<float> jointMatrices;

    // Phase AX: morph target 权重运行时状态
    //   morphWeights[i]       = 当前生效权重 (动画评估值或手动覆盖值; 写到 GPU 的就是这个)
    //   morphWeightsManual[i] = 手动覆盖值; NaN 表示"未覆盖, 用动画值"
    std::vector<float> morphWeights;          // size = mesh.morphTargetCount
    std::vector<float> morphWeightsManual;    // size = mesh.morphTargetCount

    bool alive = true;
};

// Step 3: 蒙皮网格资产 (CPU 蒙皮路径 + Phase AW GPU 蒙皮路径并存)
//   - 持原始顶点 (POSITION/NORMAL/UV/COLOR) 备份;
//   - CPU 路径: 每帧 jointMatrices 变换 baseVertices -> skinnedVertices -> CreateMesh -> DrawMeshMaterial
//   - GPU 路径 (Phase AW): 首次 Draw 时把 baseVertices+joints+weights 一次性上传到 gpuSkinnedMeshId,
//                          之后每帧只上传 jointMatrices UBO
struct SkinnedMeshAsset {
    // 原始顶点 (绑定姿态; 每帧蒙皮变换的输入)
    std::vector<RenderVertex3D> baseVertices;
    std::vector<uint32_t>       indices;

    // 蒙皮属性: 每顶点 4 关节索引 (小端 packed) + 4 权重
    std::vector<uint32_t> jointIndicesPacked; // 每元素打包 4 个 uint8 关节 idx
    std::vector<float>    weights;            // 4 floats / 顶点 (按顺序排列)

    // 关联的骨骼 (强引用 via registry ref)
    Skeleton* skeletonPtr = nullptr;
    int       skeletonRef = LUA_NOREF;

    // CPU 路径: 每帧 DeleteMesh + CreateMesh 重传
    uint32_t gpuMeshId = 0;
    std::vector<RenderVertex3D> skinnedVertices;     // 蒙皮后的顶点 (复用 buffer)

    // Phase AW GPU 路径: 首次 Draw 时一次性上传, 永不重传
    uint32_t gpuSkinnedMeshId = 0;
    bool     gpuMeshUploaded  = false;

    // Phase AX: morph target 数据
    //   morphTargets[t]      = 第 t 个 target 的 (POSITION/NORMAL/TANGENT delta)
    //   morphTargetCount     = 实际 target 数量, 上限 MORPH_TARGET_MAX
    //   morphDefaultWeights  = mesh.weights[] (glTF spec); 默认初始化用
    //   morphTargetNames     = mesh.target_names[] 或 fallback "target_<i>"
    std::vector<MorphTarget>      morphTargets;
    int                           morphTargetCount = 0;
    std::vector<float>            morphDefaultWeights;
    std::vector<std::string>      morphTargetNames;

    // Phase AX GPU 路径: morph mesh ID (与 gpuSkinnedMeshId 互斥; 同时启 morph+skin 时使用)
    uint32_t gpuSkinnedMorphMeshId       = 0;
    bool     gpuSkinnedMorphMeshUploaded = false;

    bool alive = true;
};

// Phase AW — 全局蒙皮模式枚举
//   AUTO: 桌面 GL33 + GPU skinning 支持 -> GPU; LegacyBackend / Web -> CPU
//   CPU : 强制 CPU (用于调试 / 设备对比)
//   GPU : 优先 GPU (不支持时 fallback CPU)
enum class SkinningMode : uint8_t {
    AUTO = 0,
    CPU  = 1,
    GPU  = 2,
};

} } // namespace LT::Anim

// 全局蒙皮模式 (file-scope 静态; 仅 light_animation.cpp 用)
static LT::Anim::SkinningMode g_skinningMode = LT::Anim::SkinningMode::AUTO;

using LT::Anim::Skeleton;
using LT::Anim::AnimationClip;
using LT::Anim::Sampler;
using LT::Anim::JointNode;
using LT::Anim::Animator;
using LT::Anim::SkinnedMeshAsset;
using LT::Anim::TransitionDef;     // Step 4
using LT::Anim::EventDef;          // Step 4
using LT::Anim::InterpMode;
using LT::Anim::ChannelTarget;
using LT::Anim::SkinningMode;      // Phase AW
using LT::Anim::MorphTarget;       // Phase AX

// ==================== Step 2: 数学库 + sampler 评估 + 关节变换树 ====================
// 矩阵格式: 16 floats 列主序 (column-major), 与 OpenGL/glm/cgltf 一致.
//   m[col*4 + row]: m[0]=m00 m[1]=m10 m[2]=m20 m[3]=m30  (第 0 列)
//                   m[4]=m01 m[5]=m11 ...                (第 1 列)
// 四元数格式: (w, x, y, z), 与 light_animation.cpp Step 1 JointNode::local_r 一致.

namespace { // anonymous: 仅本编译单元可见, 避免符号冲突

// ---------- 矩阵 ----------

inline void Mat4Identity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (列主序: out[col][row] = sum a[k][row] * b[col][k])
inline void Mat4Mul(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = s;
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

// 由 Translation / Rotation (quat wxyz) / Scale 构造列主序 mat4.
// 等价于 M = T * R * S (常见骨骼动画约定).
inline void TRSToMat4(const float* t, const float* qWxyz, const float* s, float* outMat) {
    const float w = qWxyz[0], x = qWxyz[1], y = qWxyz[2], z = qWxyz[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    // R 旋转矩阵 (列主序), 已应用 scale 在每列
    outMat[0]  = (1.0f - 2.0f * (yy + zz)) * s[0];
    outMat[1]  = (2.0f * (xy + wz))        * s[0];
    outMat[2]  = (2.0f * (xz - wy))        * s[0];
    outMat[3]  = 0.0f;

    outMat[4]  = (2.0f * (xy - wz))        * s[1];
    outMat[5]  = (1.0f - 2.0f * (xx + zz)) * s[1];
    outMat[6]  = (2.0f * (yz + wx))        * s[1];
    outMat[7]  = 0.0f;

    outMat[8]  = (2.0f * (xz + wy))        * s[2];
    outMat[9]  = (2.0f * (yz - wx))        * s[2];
    outMat[10] = (1.0f - 2.0f * (xx + yy)) * s[2];
    outMat[11] = 0.0f;

    // T 平移列
    outMat[12] = t[0];
    outMat[13] = t[1];
    outMat[14] = t[2];
    outMat[15] = 1.0f;
}

// ---------- 四元数 ----------

inline void QuatNormalize(float* q) {
    float len2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (len2 <= 1e-20f) {
        q[0] = 1; q[1] = 0; q[2] = 0; q[3] = 0;     // 退化为单位四元数
        return;
    }
    float inv = 1.0f / std::sqrt(len2);
    q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
}

// slerp: a/b 假设已归一化, t ∈ [0,1]; 含最短路径翻转 (dot<0 时翻转 b)
inline void QuatSlerp(const float* a, const float* b, float t, float* out) {
    float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    float dot = a[0] * bx + a[1] * by + a[2] * bz + a[3] * bw;
    if (dot < 0.0f) {
        bx = -bx; by = -by; bz = -bz; bw = -bw;
        dot = -dot;
    }
    // 几乎平行 → 退化为 lerp + 归一化 (避免 sin(0) 除零)
    if (dot > 0.9995f) {
        out[0] = a[0] + t * (bx - a[0]);
        out[1] = a[1] + t * (by - a[1]);
        out[2] = a[2] + t * (bz - a[2]);
        out[3] = a[3] + t * (bw - a[3]);
        QuatNormalize(out);
        return;
    }
    float theta_0     = std::acos(dot);
    float theta       = theta_0 * t;
    float sin_theta_0 = std::sin(theta_0);
    float sin_theta   = std::sin(theta);
    float k0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
    float k1 = sin_theta / sin_theta_0;
    out[0] = a[0] * k0 + bx * k1;
    out[1] = a[1] * k0 + by * k1;
    out[2] = a[2] * k0 + bz * k1;
    out[3] = a[3] * k0 + bw * k1;
}

// ---------- Sampler 评估 ----------
// outComps: 调用方期望的 float 数 (TRANSLATION/SCALE=3, ROTATION=4)
// 失败 (sampler 空 / outComps 不匹配) 则填默认值: ROTATION=单位四元数 wxyz, SCALE=1, TRANSLATION=0
static void EvaluateDefault(ChannelTarget tgt, float* out, int outComps) {
    if (tgt == ChannelTarget::ROTATION && outComps == 4) {
        out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 0;     // 单位四元数 wxyz
    } else if (tgt == ChannelTarget::SCALE && outComps == 3) {
        out[0] = 1; out[1] = 1; out[2] = 1;
    } else if (outComps == 3) {
        out[0] = 0; out[1] = 0; out[2] = 0;
    } else {
        std::memset(out, 0, sizeof(float) * outComps);
    }
}

// glTF 旋转四元数布局: cgltf 给的是 (x,y,z,w), 但我们存的是 (w,x,y,z).
// AnimationClip 构建时已经按"原样"存了 cgltf 提供的浮点 (x,y,z,w 顺序),
// 而 JointNode::local_r 是 (w,x,y,z). 这里采样时要转换回 (w,x,y,z).
static inline void GltfQuatXyzwToWxyz(const float* xyzw, float* outWxyz) {
    outWxyz[0] = xyzw[3]; // w
    outWxyz[1] = xyzw[0]; // x
    outWxyz[2] = xyzw[1]; // y
    outWxyz[3] = xyzw[2]; // z
}

// 在 sampler.times 中找最大的 i 使得 times[i] <= t. t 在 [0, duration] 内.
static int FindKeyframeIndex(const std::vector<float>& times, float t) {
    if (times.empty()) return -1;
    if (t <= times.front()) return 0;
    if (t >= times.back())  return (int)times.size() - 1;
    // 二分
    int lo = 0, hi = (int)times.size() - 1;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (times[mid] <= t) lo = mid;
        else                 hi = mid;
    }
    return lo;
}

// 评估一个 sampler 在时间 t 处的值, 写到 out (outComps 个 float).
// LINEAR: TRS 用线性插值, ROTATION 用 slerp.
// STEP:   取左 keyframe 值.
// CUBICSPLINE: cgltf 标准 Hermite (每帧 3*components: in_tan, value, out_tan).
//              ROTATION 在 CUBICSPLINE 后必须归一化.
static void EvaluateSampler(const Sampler& s, float t, float* out, int outComps) {
    if (s.times.empty()) {
        EvaluateDefault(s.target, out, outComps);
        return;
    }
    int idx = FindKeyframeIndex(s.times, t);
    if (idx < 0) {
        EvaluateDefault(s.target, out, outComps);
        return;
    }
    int lastIdx = (int)s.times.size() - 1;
    int comps   = s.components;     // glTF 原始 components (3 或 4)

    auto readKey = [&](int k, int part, float* dst) {
        // CUBICSPLINE: data[k] 布局 = (in_tan, value, out_tan), 每段 comps 个 float
        // STEP/LINEAR: data[k] 布局 = value, comps 个 float
        size_t base = (s.mode == InterpMode::CUBICSPLINE)
                          ? ((size_t)k * comps * 3 + (size_t)part * comps)
                          : ((size_t)k * comps);
        std::memcpy(dst, &s.values[base], sizeof(float) * comps);
    };

    // 边界 / STEP / 单帧 → 直接取左 keyframe value
    if (idx >= lastIdx || s.mode == InterpMode::STEP || s.times[idx] >= t) {
        float raw[MORPH_TARGET_MAX];     // Phase AX: 扩容以支持 MORPH_WEIGHTS 路径 (components 上限 8)
        readKey(idx, 1 /*value*/, raw);
        if (s.target == ChannelTarget::ROTATION && outComps == 4 && comps == 4) {
            GltfQuatXyzwToWxyz(raw, out);
            QuatNormalize(out);
        } else {
            std::memcpy(out, raw, sizeof(float) * outComps);
        }
        return;
    }

    // 区间 [idx, idx+1]
    float t0 = s.times[idx];
    float t1 = s.times[idx + 1];
    float dt = t1 - t0;
    float u  = (dt > 1e-20f) ? (t - t0) / dt : 0.0f;
    if (u < 0) u = 0;
    if (u > 1) u = 1;

    if (s.mode == InterpMode::LINEAR) {
        float a[MORPH_TARGET_MAX], b[MORPH_TARGET_MAX];   // Phase AX: 扩容到 8
        readKey(idx,     1, a);
        readKey(idx + 1, 1, b);
        if (s.target == ChannelTarget::ROTATION && outComps == 4 && comps == 4) {
            float aWxyz[4], bWxyz[4];
            GltfQuatXyzwToWxyz(a, aWxyz);
            GltfQuatXyzwToWxyz(b, bWxyz);
            QuatNormalize(aWxyz);
            QuatNormalize(bWxyz);
            QuatSlerp(aWxyz, bWxyz, u, out);
        } else {
            for (int k = 0; k < outComps; ++k) {
                out[k] = a[k] + u * (b[k] - a[k]);
            }
        }
        return;
    }

    // CUBICSPLINE: p(u) = (2u³-3u²+1)*v0 + (u³-2u²+u)*dt*o0 + (-2u³+3u²)*v1 + (u³-u²)*dt*i1
    //              o0 = out_tangent_idx, i1 = in_tangent_(idx+1)
    float v0[MORPH_TARGET_MAX], o0[MORPH_TARGET_MAX];   // Phase AX: 扩容到 8
    float v1[MORPH_TARGET_MAX], i1[MORPH_TARGET_MAX];
    readKey(idx,     1, v0);
    readKey(idx,     2, o0);
    readKey(idx + 1, 0, i1);
    readKey(idx + 1, 1, v1);

    float u2 = u * u;
    float u3 = u2 * u;
    float h00 =  2 * u3 - 3 * u2 + 1;
    float h10 =      u3 - 2 * u2 + u;
    float h01 = -2 * u3 + 3 * u2;
    float h11 =      u3 -     u2;

    float raw[MORPH_TARGET_MAX];   // Phase AX: 扩容到 8
    for (int k = 0; k < comps; ++k) {
        raw[k] = h00 * v0[k] + h10 * dt * o0[k] + h01 * v1[k] + h11 * dt * i1[k];
    }

    if (s.target == ChannelTarget::ROTATION && outComps == 4 && comps == 4) {
        GltfQuatXyzwToWxyz(raw, out);
        QuatNormalize(out);
    } else {
        std::memcpy(out, raw, sizeof(float) * outComps);
    }
}

// ---------- 前向变换树 ----------

// vec3 线性插值 (Step 4 crossfade 用)
inline void Vec3Lerp(const float* a, const float* b, float t, float* out) {
    out[0] = a[0] + t * (b[0] - a[0]);
    out[1] = a[1] + t * (b[1] - a[1]);
    out[2] = a[2] + t * (b[2] - a[2]);
}

// 评估单关节 local TRS: 默认用 bind pose, sampler 覆盖优先
//   jn:        关节 bind pose
//   clip:      动画 clip (nullptr → 完全 bind pose)
//   jointIdx:  关节在 skeleton 中的索引 (sampler 用 jointIndex 匹配)
//   t:         clip 时间
//   out{T,R,S}: 输出 (3, 4, 3 floats; R 为 wxyz 四元数)
static void EvaluateLocalTRS(const JointNode& jn, AnimationClip* clip, int jointIdx, float t,
                              float* outT, float* outR, float* outS) {
    outT[0] = jn.local_t[0]; outT[1] = jn.local_t[1]; outT[2] = jn.local_t[2];
    outR[0] = jn.local_r[0]; outR[1] = jn.local_r[1]; outR[2] = jn.local_r[2]; outR[3] = jn.local_r[3];
    outS[0] = jn.local_s[0]; outS[1] = jn.local_s[1]; outS[2] = jn.local_s[2];

    if (!clip) return;
    for (const Sampler& s : clip->samplers) {
        if (s.jointIndex != jointIdx) continue;
        if (s.target == ChannelTarget::TRANSLATION)      EvaluateSampler(s, t, outT, 3);
        else if (s.target == ChannelTarget::ROTATION)    EvaluateSampler(s, t, outR, 4);
        else if (s.target == ChannelTarget::SCALE)       EvaluateSampler(s, t, outS, 3);
    }
}

// 公共: DFS world 矩阵 + skinning 矩阵 (cgltf 关节顺序通常已是父先于子, N+4 迭代上限保安全)
static void ComputeWorldAndSkinning(Skeleton* sk,
                                     const std::vector<float>& localMats,
                                     std::vector<float>& outMatrices) {
    int N = (int)sk->joints.size();
    std::vector<float> worldMats((size_t)N * 16, 0.0f);
    std::vector<uint8_t> computed((size_t)N, 0);

    int safetyIter = 0;
    while (safetyIter++ < N + 4) {
        bool allDone = true;
        for (int i = 0; i < N; ++i) {
            if (computed[i]) continue;
            int p = sk->joints[i].parent;
            if (p < 0) {
                std::memcpy(&worldMats[i * 16], &localMats[i * 16], sizeof(float) * 16);
                computed[i] = 1;
            } else if (computed[p]) {
                Mat4Mul(&worldMats[i * 16], &worldMats[p * 16], &localMats[i * 16]);
                computed[i] = 1;
            } else {
                allDone = false;
            }
        }
        if (allDone) break;
    }

    // skinning[i] = world[i] * inverseBind[i]
    for (int i = 0; i < N; ++i) {
        const float* invBind = &sk->inverseBindMatrices[i * 16];
        Mat4Mul(&outMatrices[i * 16], &worldMats[i * 16], invBind);
    }
}

// 单 clip 关节矩阵 (Step 2; Step 4 内部走 EvaluateLocalTRS + ComputeWorldAndSkinning)
static void ComputeJointMatrices(Skeleton* sk, AnimationClip* clip, float t,
                                  std::vector<float>& outMatrices) {
    int N = (int)sk->joints.size();
    outMatrices.assign((size_t)N * 16, 0.0f);
    if (N == 0) return;

    std::vector<float> localMats((size_t)N * 16, 0.0f);
    for (int i = 0; i < N; ++i) {
        float trans[3], rot[4], scl[3];
        EvaluateLocalTRS(sk->joints[i], clip, i, t, trans, rot, scl);
        TRSToMat4(trans, rot, scl, &localMats[i * 16]);
    }
    ComputeWorldAndSkinning(sk, localMats, outMatrices);
}

// Step 4: 双 clip 混合 (crossfade) 关节矩阵
//   weight ∈ [0, 1]: 0 = 完全 clipA, 1 = 完全 clipB
//   按关节做 (T,R,S) 各自混合 (T/S lerp, R slerp), 再走 TRSToMat4 + 同 DFS 拓扑
static void ComputeJointMatricesBlended(Skeleton* sk,
                                          AnimationClip* clipA, float tA,
                                          AnimationClip* clipB, float tB,
                                          float weight,
                                          std::vector<float>& outMatrices) {
    int N = (int)sk->joints.size();
    outMatrices.assign((size_t)N * 16, 0.0f);
    if (N == 0) return;

    if (weight < 0.0f) weight = 0.0f;
    if (weight > 1.0f) weight = 1.0f;

    std::vector<float> localMats((size_t)N * 16, 0.0f);
    for (int i = 0; i < N; ++i) {
        const JointNode& jn = sk->joints[i];
        float tAv[3], rAv[4], sAv[3];
        float tBv[3], rBv[4], sBv[3];
        EvaluateLocalTRS(jn, clipA, i, tA, tAv, rAv, sAv);
        EvaluateLocalTRS(jn, clipB, i, tB, tBv, rBv, sBv);

        float trans[3], rot[4], scl[3];
        Vec3Lerp(tAv, tBv, weight, trans);
        Vec3Lerp(sAv, sBv, weight, scl);
        QuatSlerp(rAv, rBv, weight, rot);     // 内部含归一化

        TRSToMat4(trans, rot, scl, &localMats[i * 16]);
    }
    ComputeWorldAndSkinning(sk, localMats, outMatrices);
}

// ==================== Phase AX: morph weights 评估 ====================

// 在 clip.samplers 中评估所有 MORPH_WEIGHTS sampler, 输出最大 components 个值
// 返回值: 评估到的最大 components (= 该 clip 控制的 morph target 数量上限); 0 表示无 weights sampler
//   out:   最大 MORPH_TARGET_MAX 个 float (调用方提供 stack array)
//   注意: 同一 clip 中可能有多个 weights channel (来自不同 mesh node), 但本阶段只支持 1 个
//         (取第一个 sampler.components > 0 的, 与 Animator/SkinnedMesh 1:1 关系一致)
static int EvaluateClipMorphWeights(AnimationClip* clip, float t, float* out) {
    if (!clip) return 0;
    for (const Sampler& s : clip->samplers) {
        if (s.target != ChannelTarget::MORPH_WEIGHTS) continue;
        int comps = s.components;
        if (comps <= 0) continue;
        if (comps > MORPH_TARGET_MAX) comps = MORPH_TARGET_MAX;
        // EvaluateSampler 已被扩容到支持 components <= 8
        EvaluateSampler(s, t, out, comps);
        return comps;
    }
    return 0;
}

// Phase AX: 评估 Animator 的当前 morph weights
//   语义:
//     1. 从 activeClip 评估 evalA[N]
//     2. (crossfade 中) 从 crossfadeClip 评估 evalB[N], 按 progress 混合到 evalA
//     3. 按手动覆盖 (NaN sentinel) 写入最终 morphWeights
//   不需要 mesh 引用: morphTargetCount 由 sampler.components 决定
//   morphWeights / morphWeightsManual 自动 lazy resize (跟随评估到的 N)
static void EvaluateMorphWeights(Animator* an) {
    if (!an) return;
    float evalA[MORPH_TARGET_MAX] = {0};
    int   nA = EvaluateClipMorphWeights(an->activeClip, an->currentTime, evalA);

    float evalB[MORPH_TARGET_MAX] = {0};
    int   nB = 0;
    bool inCrossfade = (!an->crossfadeTarget.empty() && an->crossfadeClip);
    if (inCrossfade) {
        nB = EvaluateClipMorphWeights(an->crossfadeClip, an->crossfadeClipTime, evalB);
    }

    // 取 max(nA, nB) 作为本帧 N (容许 active/crossfade clip 控制的 morph target 数不同)
    int N = (nA > nB) ? nA : nB;
    if (N == 0) return;   // 两个 clip 都无 weights sampler

    // lazy resize (尽可能延迟分配)
    if ((int)an->morphWeights.size() < N)       an->morphWeights.assign(N, 0.0f);
    if ((int)an->morphWeightsManual.size() < N) an->morphWeightsManual.assign(N, std::nanf(""));

    // 混合 + 手动覆盖
    float w = inCrossfade ? an->crossfadeProgress : 0.0f;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    for (int i = 0; i < N; ++i) {
        float a = (i < nA) ? evalA[i] : 0.0f;
        float b = (i < nB) ? evalB[i] : 0.0f;
        float merged = inCrossfade ? (a + (b - a) * w) : a;

        float manual = an->morphWeightsManual[i];
        // NaN: 用动画值; 否则用手动值 (用户显式 SetMorphWeight 后)
        an->morphWeights[i] = std::isnan(manual) ? merged : manual;
    }
}

// ==================== Step 3: CPU 蒙皮 ====================

// 把 mat4 (列主序) 应用到点 (w=1) 或向量 (w=0)
inline void Mat4ApplyPoint(const float* m, const float* in3, float* out3) {
    out3[0] = m[0] * in3[0] + m[4] * in3[1] + m[8]  * in3[2] + m[12];
    out3[1] = m[1] * in3[0] + m[5] * in3[1] + m[9]  * in3[2] + m[13];
    out3[2] = m[2] * in3[0] + m[6] * in3[1] + m[10] * in3[2] + m[14];
}

inline void Mat4ApplyDir(const float* m, const float* in3, float* out3) {
    // 仅 3x3 部分 (忽略平移); 注意法线严格应该用 inverse-transpose, 但常规均匀缩放下足够
    out3[0] = m[0] * in3[0] + m[4] * in3[1] + m[8]  * in3[2];
    out3[1] = m[1] * in3[0] + m[5] * in3[1] + m[9]  * in3[2];
    out3[2] = m[2] * in3[0] + m[6] * in3[1] + m[10] * in3[2];
}

// 对单顶点做 4 关节加权变换 (CPU 蒙皮核心)
//   matrices: 关节蒙皮矩阵 (N*16, 列主序)
//   joints:   该顶点 4 个关节索引
//   weights:  该顶点 4 个权重 (期望和约 1.0; 不做强制归一化以匹配 glTF 数据)
//   in/out: 顶点位置 + 法线 (3 floats each)
static void CpuSkinVertex(const float* matrices, int jointCount,
                          const uint8_t* joints, const float* weights,
                          const float* posIn, const float* nrmIn,
                          float* posOut, float* nrmOut) {
    // 计算 weighted blend matrix (4x4 加权和)
    float blend[16] = {0};
    float wsum = 0;
    for (int k = 0; k < 4; ++k) {
        float w = weights[k];
        if (w <= 0) continue;
        int j = joints[k];
        if (j < 0 || j >= jointCount) continue;     // 越界保护
        const float* M = &matrices[j * 16];
        for (int i = 0; i < 16; ++i) {
            blend[i] += M[i] * w;
        }
        wsum += w;
    }
    // 权重和为 0 (异常: 顶点未绑定任何关节) → 退化为单位矩阵
    if (wsum <= 1e-6f) {
        std::memset(blend, 0, sizeof(blend));
        blend[0] = blend[5] = blend[10] = blend[15] = 1.0f;
    }
    Mat4ApplyPoint(blend, posIn, posOut);
    Mat4ApplyDir(blend,  nrmIn, nrmOut);
}

// ==================== Step 4: Event 触发判定 ====================

// 在 [prev, cur] 区间内是否跨过 trigger (含 looping 跨边界)
//   prev <= cur:  单周期内, 直接判定 trigger ∈ [prev, cur]
//   prev > cur:   跨循环 (currentTime 已 fmod 回滚), 触发条件 trigger ∈ [prev, dur] ∪ [0, cur]
inline bool EventTriggered(float prev, float cur, float trigger, float duration, bool looping) {
    if (prev <= cur) {
        return (prev <= trigger && trigger <= cur);
    }
    if (looping && duration > 1e-6f) {
        return (trigger >= prev && trigger <= duration) ||
               (trigger >= 0.0f && trigger <= cur);
    }
    return false;
}

} // anonymous namespace

// ==================== Step 4: Lua callback helpers ====================

// pcall transition condFn(animator) → bool. 出错记录到 stderr 视为 false.
//   animatorIdx: animator userdata 在 Lua 栈上的位置 (Update 调用时是 1)
//   condFnRef:   registry ref to Lua function
static bool CallTransitionCond(lua_State* L, int animatorIdx, int condFnRef) {
    if (condFnRef == LUA_NOREF) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, condFnRef);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushvalue(L, animatorIdx);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "[Light.Animation] transition cond error: %s\n", err ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    bool ret = (lua_type(L, -1) != LUA_TNIL) && (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return ret;
}

// pcall event callback(animator). 出错记录到 stderr.
static void CallEventCallback(lua_State* L, int animatorIdx, int callbackRef) {
    if (callbackRef == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    lua_pushvalue(L, animatorIdx);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "[Light.Animation] event callback error: %s\n", err ? err : "?");
        lua_pop(L, 1);
    }
}

// ==================== userdata 检查辅助 ====================

static Skeleton* CheckSkeleton(lua_State* L, int idx) {
    Skeleton** pp = (Skeleton**)luaL_checkudata(L, idx, SKELETON_MT);
    if (!pp || !*pp) {
        luaL_error(L, "Skeleton: invalid userdata");
        return nullptr;
    }
    return *pp;
}

static AnimationClip* CheckClip(lua_State* L, int idx) {
    AnimationClip** pp = (AnimationClip**)luaL_checkudata(L, idx, CLIP_MT);
    if (!pp || !*pp) {
        luaL_error(L, "AnimationClip: invalid userdata");
        return nullptr;
    }
    return *pp;
}

static Animator* CheckAnimator(lua_State* L, int idx) {
    Animator** pp = (Animator**)luaL_checkudata(L, idx, ANIMATOR_MT);
    if (!pp || !*pp) {
        luaL_error(L, "Animator: invalid userdata");
        return nullptr;
    }
    return *pp;
}

// Step 3: SkinnedMesh userdata 检查
static SkinnedMeshAsset* CheckSkinnedMesh(lua_State* L, int idx) {
    SkinnedMeshAsset** pp = (SkinnedMeshAsset**)luaL_checkudata(L, idx, SKINNED_MESH_MT);
    if (!pp || !*pp) {
        luaL_error(L, "SkinnedMesh: invalid userdata");
        return nullptr;
    }
    return *pp;
}

// 创建 Skeleton userdata (持指针, __gc 时 delete)
static void PushSkeletonUserdata(lua_State* L, Skeleton* sk) {
    Skeleton** pp = (Skeleton**)lua_newuserdata(L, sizeof(Skeleton*));
    *pp = sk;
    luaL_getmetatable(L, SKELETON_MT);
    lua_setmetatable(L, -2);
}
static void PushClipUserdata(lua_State* L, AnimationClip* clip) {
    AnimationClip** pp = (AnimationClip**)lua_newuserdata(L, sizeof(AnimationClip*));
    *pp = clip;
    luaL_getmetatable(L, CLIP_MT);
    lua_setmetatable(L, -2);
}
static void PushAnimatorUserdata(lua_State* L, Animator* an) {
    Animator** pp = (Animator**)lua_newuserdata(L, sizeof(Animator*));
    *pp = an;
    luaL_getmetatable(L, ANIMATOR_MT);
    lua_setmetatable(L, -2);
}
static void PushSkinnedMeshUserdata(lua_State* L, SkinnedMeshAsset* sm) {
    SkinnedMeshAsset** pp = (SkinnedMeshAsset**)lua_newuserdata(L, sizeof(SkinnedMeshAsset*));
    *pp = sm;
    luaL_getmetatable(L, SKINNED_MESH_MT);
    lua_setmetatable(L, -2);
}

// ==================== cgltf → Skeleton 提取 ====================

// 在 cgltf_skin 内查找 node 对应的关节索引; 找不到返回 -1
static int FindJointInSkin(const cgltf_skin* skin, const cgltf_node* node) {
    for (cgltf_size i = 0; i < skin->joints_count; ++i) {
        if (skin->joints[i] == node) return (int)i;
    }
    return -1;
}

// 把 cgltf_node 局部 TRS 拷贝到 JointNode (考虑 has_translation/has_rotation/has_scale)
static void CopyNodeBindTRS(const cgltf_node* node, JointNode& jn) {
    if (node->has_translation) {
        jn.local_t[0] = node->translation[0];
        jn.local_t[1] = node->translation[1];
        jn.local_t[2] = node->translation[2];
    }
    if (node->has_rotation) {
        // cgltf 存 (x, y, z, w); 我们存 (w, x, y, z) 以与 DESIGN 文档对齐
        jn.local_r[0] = node->rotation[3];   // w
        jn.local_r[1] = node->rotation[0];   // x
        jn.local_r[2] = node->rotation[1];   // y
        jn.local_r[3] = node->rotation[2];   // z
    }
    if (node->has_scale) {
        jn.local_s[0] = node->scale[0];
        jn.local_s[1] = node->scale[1];
        jn.local_s[2] = node->scale[2];
    }
}

// 从 cgltf_skin 构造 Skeleton; 失败返回 nullptr 并填 errOut
static Skeleton* BuildSkeleton(const cgltf_skin* skin, std::string& errOut) {
    if (!skin || skin->joints_count == 0) {
        errOut = "skin has no joints";
        return nullptr;
    }
    if (skin->joints_count > (cgltf_size)MAX_JOINTS) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "skeleton joint count %d exceeds limit %d",
                      (int)skin->joints_count, MAX_JOINTS);
        errOut = buf;
        return nullptr;
    }

    Skeleton* sk = new Skeleton();
    sk->joints.resize(skin->joints_count);
    sk->inverseBindMatrices.resize(skin->joints_count * FLOATS_PER_MAT4, 0.0f);

    // 第一遍: 填关节名 + 本地 TRS
    for (cgltf_size i = 0; i < skin->joints_count; ++i) {
        const cgltf_node* node = skin->joints[i];
        JointNode& jn = sk->joints[i];
        jn.name = node->name ? node->name : ("joint_" + std::to_string(i));
        sk->nameToIndex[jn.name] = (int)i;
        CopyNodeBindTRS(node, jn);
    }

    // 第二遍: 填 parent / children (限于 skin->joints 数组内)
    for (cgltf_size i = 0; i < skin->joints_count; ++i) {
        const cgltf_node* node = skin->joints[i];
        if (node->parent) {
            int parentIdx = FindJointInSkin(skin, node->parent);
            // parent 不在 skin.joints 列表 → 视为根
            sk->joints[i].parent = parentIdx;
            if (parentIdx >= 0) {
                sk->joints[parentIdx].children.push_back((int)i);
            }
        }
    }

    // 找根关节 (第一个 parent==-1)
    sk->rootJoint = -1;
    for (size_t i = 0; i < sk->joints.size(); ++i) {
        if (sk->joints[i].parent < 0) {
            sk->rootJoint = (int)i;
            break;
        }
    }

    // 反向绑定矩阵: cgltf 提供 mat4 数组, 直接 unpack
    if (skin->inverse_bind_matrices) {
        cgltf_accessor_unpack_floats(skin->inverse_bind_matrices,
                                      sk->inverseBindMatrices.data(),
                                      sk->joints.size() * FLOATS_PER_MAT4);
    } else {
        // 无 IBM, 用单位矩阵 (绑定姿态即原姿态)
        for (size_t i = 0; i < sk->joints.size(); ++i) {
            float* m = &sk->inverseBindMatrices[i * FLOATS_PER_MAT4];
            std::memset(m, 0, sizeof(float) * FLOATS_PER_MAT4);
            m[0] = m[5] = m[10] = m[15] = 1.0f;     // 单位矩阵 (列主序)
        }
    }

    return sk;
}

// ==================== Step 3: cgltf → SkinnedMeshAsset 提取 ====================

// 在 cgltf_primitive::attributes 中查找指定 name 的 attribute, 找不到返回 nullptr
static const cgltf_attribute* FindAttr(const cgltf_primitive* prim, const char* name) {
    for (cgltf_size i = 0; i < prim->attributes_count; ++i) {
        if (prim->attributes[i].name && std::strcmp(prim->attributes[i].name, name) == 0) {
            return &prim->attributes[i];
        }
    }
    return nullptr;
}

// 从 cgltf_primitive 提取蒙皮所需所有数据 (POSITION/NORMAL/UV/COLOR + JOINTS_0 + WEIGHTS_0 + indices)
// 失败时填 errOut 返回 false. 若 prim 缺 JOINTS_0/WEIGHTS_0 视为非蒙皮 mesh 也返回失败.
static bool ExtractSkinMesh(const cgltf_primitive* prim,
                             SkinnedMeshAsset* outMesh,
                             std::string& errOut) {
    if (!prim) { errOut = "primitive is null"; return false; }
    if (prim->type != cgltf_primitive_type_triangles) {
        errOut = "primitive is not triangles";
        return false;
    }

    // POSITION (必须 vec3 float)
    const cgltf_attribute* posAttr = FindAttr(prim, "POSITION");
    if (!posAttr || !posAttr->data) {
        errOut = "skinned primitive missing POSITION";
        return false;
    }
    cgltf_size vCount = posAttr->data->count;
    if (vCount == 0) {
        errOut = "skinned primitive has 0 vertices";
        return false;
    }

    // JOINTS_0 + WEIGHTS_0 (蒙皮必须)
    const cgltf_attribute* joinAttr = FindAttr(prim, "JOINTS_0");
    const cgltf_attribute* wAttr    = FindAttr(prim, "WEIGHTS_0");
    if (!joinAttr || !joinAttr->data || !wAttr || !wAttr->data) {
        errOut = "primitive missing JOINTS_0 or WEIGHTS_0 (not a skinned mesh)";
        return false;
    }
    if (joinAttr->data->count != vCount || wAttr->data->count != vCount) {
        errOut = "JOINTS_0 / WEIGHTS_0 count mismatches POSITION count";
        return false;
    }

    // 可选 attributes
    const cgltf_attribute* nrmAttr   = FindAttr(prim, "NORMAL");
    const cgltf_attribute* uvAttr    = FindAttr(prim, "TEXCOORD_0");
    const cgltf_attribute* colorAttr = FindAttr(prim, "COLOR_0");

    // 解包 POSITION (3 floats / vertex)
    std::vector<float> pos(vCount * 3);
    cgltf_accessor_unpack_floats(posAttr->data, pos.data(), pos.size());

    // 解包可选 attributes (默认值)
    std::vector<float> nrm(vCount * 3, 0.0f);
    if (nrmAttr && nrmAttr->data && nrmAttr->data->count == vCount) {
        cgltf_accessor_unpack_floats(nrmAttr->data, nrm.data(), nrm.size());
    } else {
        for (cgltf_size i = 0; i < vCount; ++i) nrm[i * 3 + 1] = 1.0f;     // 默认 +Y
    }

    std::vector<float> uv(vCount * 2, 0.0f);
    if (uvAttr && uvAttr->data && uvAttr->data->count == vCount) {
        cgltf_accessor_unpack_floats(uvAttr->data, uv.data(), uv.size());
    }

    std::vector<float> color(vCount * 4, 1.0f);    // 默认白
    if (colorAttr && colorAttr->data && colorAttr->data->count == vCount) {
        // cgltf_num_components 给当前 accessor type 的分量数 (3 或 4)
        cgltf_size cc = cgltf_num_components(colorAttr->data->type);
        std::vector<float> tmp(vCount * cc);
        cgltf_accessor_unpack_floats(colorAttr->data, tmp.data(), tmp.size());
        for (cgltf_size i = 0; i < vCount; ++i) {
            color[i * 4 + 0] = tmp[i * cc + 0];
            color[i * 4 + 1] = tmp[i * cc + 1];
            color[i * 4 + 2] = tmp[i * cc + 2];
            color[i * 4 + 3] = (cc >= 4) ? tmp[i * cc + 3] : 1.0f;
        }
    }

    // 解包 JOINTS_0: 用 cgltf_accessor_read_uint 逐顶点读 4 个 uint
    outMesh->jointIndicesPacked.resize(vCount);
    for (cgltf_size i = 0; i < vCount; ++i) {
        cgltf_uint vec[4] = {0, 0, 0, 0};
        cgltf_accessor_read_uint(joinAttr->data, i, vec, 4);
        // 关节索引上限 64 → 单字节足够; 越界保护
        uint8_t b0 = (uint8_t)((vec[0] < (cgltf_uint)MAX_JOINTS) ? vec[0] : 0);
        uint8_t b1 = (uint8_t)((vec[1] < (cgltf_uint)MAX_JOINTS) ? vec[1] : 0);
        uint8_t b2 = (uint8_t)((vec[2] < (cgltf_uint)MAX_JOINTS) ? vec[2] : 0);
        uint8_t b3 = (uint8_t)((vec[3] < (cgltf_uint)MAX_JOINTS) ? vec[3] : 0);
        outMesh->jointIndicesPacked[i] = (uint32_t)b0 | ((uint32_t)b1 << 8) |
                                          ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
    }

    // 解包 WEIGHTS_0: 4 floats / vertex (cgltf 自动处理 normalized uint8/uint16)
    outMesh->weights.resize(vCount * 4);
    cgltf_accessor_unpack_floats(wAttr->data, outMesh->weights.data(), outMesh->weights.size());

    // 拼装 RenderVertex3D 数组 (蒙皮基础数据, 每顶点 12 floats)
    outMesh->baseVertices.resize(vCount);
    for (cgltf_size i = 0; i < vCount; ++i) {
        RenderVertex3D& v = outMesh->baseVertices[i];
        v.x  = pos[i * 3 + 0]; v.y  = pos[i * 3 + 1]; v.z  = pos[i * 3 + 2];
        v.nx = nrm[i * 3 + 0]; v.ny = nrm[i * 3 + 1]; v.nz = nrm[i * 3 + 2];
        v.u  = uv[i * 2 + 0];  v.v  = uv[i * 2 + 1];
        v.r  = color[i * 4 + 0]; v.g = color[i * 4 + 1];
        v.b  = color[i * 4 + 2]; v.a = color[i * 4 + 3];
    }

    // 索引: cgltf_accessor_unpack_indices 直接解包到 uint32
    if (prim->indices && prim->indices->count > 0) {
        outMesh->indices.resize(prim->indices->count);
        // cgltf v1.13: cgltf_accessor_unpack_indices(accessor, out, sizeof_index, count)
        cgltf_accessor_unpack_indices(prim->indices, outMesh->indices.data(),
                                       sizeof(uint32_t), outMesh->indices.size());
    } else {
        // 无索引: 顺序索引 (适用于 cgltf 解析后的非索引 triangles)
        outMesh->indices.resize(vCount);
        for (cgltf_size i = 0; i < vCount; ++i) outMesh->indices[i] = (uint32_t)i;
    }

    // 蒙皮后顶点缓冲: 与 baseVertices 同大小, 内容初始化为 baseVertices
    outMesh->skinnedVertices = outMesh->baseVertices;
    return true;
}

// 在 cgltf_data 中查找第一个有 mesh 的 node 关联的 primitive (优先 skin->joints[0]->mesh)
// 返回 nullptr 表示该 glTF 无蒙皮 mesh
static const cgltf_primitive* FindFirstSkinnedPrimitive(const cgltf_data* data,
                                                         const cgltf_skin* skin) {
    // 策略 1: 找第一个 skin == 当前 skin 的 node 上的 mesh primitive
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& n = data->nodes[i];
        if (n.skin == skin && n.mesh && n.mesh->primitives_count > 0) {
            return &n.mesh->primitives[0];
        }
    }
    // 策略 2: 退而求其次, 找任何有 JOINTS_0 attribute 的 mesh primitive
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        const cgltf_mesh& m = data->meshes[i];
        for (cgltf_size p = 0; p < m.primitives_count; ++p) {
            if (FindAttr(&m.primitives[p], "JOINTS_0")) return &m.primitives[p];
        }
    }
    return nullptr;
}

// Phase AX: 找包含给定 primitive 的 mesh 的 mesh-level weights/target_names
// 顺序: 在 data->meshes 中找哪个 mesh 的 primitives 数组包含 prim, 返回该 mesh
static const cgltf_mesh* FindMeshForPrimitive(const cgltf_data* data,
                                                const cgltf_primitive* prim) {
    if (!data || !prim) return nullptr;
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        const cgltf_mesh& m = data->meshes[i];
        if (m.primitives_count == 0) continue;
        // 指针落在 [&m.primitives[0], &m.primitives[count]) 区间内
        const cgltf_primitive* pBeg = &m.primitives[0];
        const cgltf_primitive* pEnd = pBeg + m.primitives_count;
        if (prim >= pBeg && prim < pEnd) return &m;
    }
    return nullptr;
}

// Phase AX: 从 cgltf_primitive::targets[] 提取 morph delta 数据并填充 SkinnedMeshAsset
// prim:  可能持有 morph target 的 primitive (与 ExtractSkinMesh 同源)
// mesh:  primitive 所属 mesh (用于 mesh.weights[] / mesh.target_names[])
// 返回 true 表示提取成功 (即使 N==0 也成功; 只在解析逻辑错时 return false)
static bool ExtractMorphTargets(const cgltf_primitive* prim,
                                  const cgltf_mesh* mesh,
                                  SkinnedMeshAsset* outMesh,
                                  std::string& errOut) {
    if (!prim || !outMesh) {
        errOut = "ExtractMorphTargets: null prim or mesh";
        return false;
    }
    cgltf_size N = prim->targets_count;
    if (N == 0) {
        outMesh->morphTargetCount = 0;
        return true;   // 不视为错误: 没有 morph 也是合法 mesh
    }

    // 截断到 MORPH_TARGET_MAX = 8
    if (N > (cgltf_size)MORPH_TARGET_MAX) {
        std::fprintf(stderr,
                     "[Phase AX] glTF mesh has %zu morph targets, truncating to %d\n",
                     (size_t)N, MORPH_TARGET_MAX);
        N = (cgltf_size)MORPH_TARGET_MAX;
    }

    cgltf_size vCount = outMesh->baseVertices.size();
    outMesh->morphTargets.resize(N);

    for (cgltf_size t = 0; t < N; ++t) {
        const cgltf_morph_target& mt = prim->targets[t];
        MorphTarget& dst = outMesh->morphTargets[t];

        for (cgltf_size a = 0; a < mt.attributes_count; ++a) {
            const cgltf_attribute& attr = mt.attributes[a];
            if (!attr.name || !attr.data) continue;

            // 必须 vertex count 一致, 否则跳过该 attribute (不 fail mesh)
            if (attr.data->count != vCount) continue;

            std::vector<float>* dstVec = nullptr;
            if      (std::strcmp(attr.name, "POSITION") == 0) dstVec = &dst.posDelta;
            else if (std::strcmp(attr.name, "NORMAL")   == 0) dstVec = &dst.nrmDelta;
            else if (std::strcmp(attr.name, "TANGENT")  == 0) dstVec = &dst.tanDelta;
            else continue;   // 未知 attribute (例如 COLOR_0 / TEXCOORD_0 delta) 暂不支持

            dstVec->resize(vCount * 3);
            cgltf_accessor_unpack_floats(attr.data, dstVec->data(), dstVec->size());
        }
    }

    outMesh->morphTargetCount = (int)N;

    // 默认权重 (mesh.weights[])
    outMesh->morphDefaultWeights.assign(N, 0.0f);
    if (mesh && mesh->weights && mesh->weights_count > 0) {
        cgltf_size wN = (mesh->weights_count < N) ? mesh->weights_count : N;
        for (cgltf_size i = 0; i < wN; ++i) {
            outMesh->morphDefaultWeights[i] = mesh->weights[i];
        }
    }

    // 名称 (mesh.target_names[]); fallback "target_<i>"
    outMesh->morphTargetNames.resize(N);
    for (cgltf_size i = 0; i < N; ++i) {
        if (mesh && mesh->target_names && i < mesh->target_names_count
            && mesh->target_names[i]) {
            outMesh->morphTargetNames[i] = mesh->target_names[i];
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "target_%zu", (size_t)i);
            outMesh->morphTargetNames[i] = buf;
        }
    }
    return true;
}

// Phase AX: 在 cgltf_data->nodes[] 中找 node 的索引 (cgltf 已 fixup, 直接用指针差)
//   返回 -1 表示未找到 (node 不在 data 内)
static int FindNodeIndex(const cgltf_data* data, const cgltf_node* node) {
    if (!data || !node || data->nodes_count == 0) return -1;
    const cgltf_node* base = &data->nodes[0];
    if (node < base || node >= base + data->nodes_count) return -1;
    return (int)(node - base);
}

// Phase AX: 取给定 node 关联 mesh 的 morph target count (用于 weights channel sampler.components)
//   返回 0 表示 node 无 mesh 或 mesh 无 morph target
static int GetNodeMorphTargetCount(const cgltf_node* node) {
    if (!node || !node->mesh || node->mesh->primitives_count == 0) return 0;
    cgltf_size n = node->mesh->primitives[0].targets_count;
    if (n > (cgltf_size)MORPH_TARGET_MAX) n = (cgltf_size)MORPH_TARGET_MAX;
    return (int)n;
}

// ==================== cgltf → AnimationClip 提取 ====================

// 把 cgltf_animation_path 转为我们的 ChannelTarget
static ChannelTarget ConvertChannelTarget(cgltf_animation_path_type p) {
    switch (p) {
        case cgltf_animation_path_type_translation: return ChannelTarget::TRANSLATION;
        case cgltf_animation_path_type_rotation:    return ChannelTarget::ROTATION;
        case cgltf_animation_path_type_scale:       return ChannelTarget::SCALE;
        case cgltf_animation_path_type_weights:     return ChannelTarget::MORPH_WEIGHTS;   // Phase AX
        default: return ChannelTarget::UNSUPPORTED;
    }
}

// 把 cgltf_interpolation_type 转为我们的 InterpMode
static InterpMode ConvertInterpolation(cgltf_interpolation_type i) {
    switch (i) {
        case cgltf_interpolation_type_linear:       return InterpMode::LINEAR;
        case cgltf_interpolation_type_step:         return InterpMode::STEP;
        case cgltf_interpolation_type_cubic_spline: return InterpMode::CUBICSPLINE;
        default: return InterpMode::LINEAR;
    }
}

// 从一个 cgltf_animation 构造 AnimationClip
//   skin: 用于映射 TRS channel 目标 node → joint 索引
//   data: Phase AX 新增, 用于 MORPH_WEIGHTS channel 的 nodeIdx 查找
static AnimationClip* BuildClip(const cgltf_animation* anim,
                                  const cgltf_skin* skin,
                                  const cgltf_data* data) {
    AnimationClip* clip = new AnimationClip();
    clip->name = anim->name ? anim->name : "(unnamed)";

    for (cgltf_size c = 0; c < anim->channels_count; ++c) {
        const cgltf_animation_channel& ch = anim->channels[c];
        if (!ch.target_node || !ch.sampler) continue;

        ChannelTarget tgt = ConvertChannelTarget(ch.target_path);
        if (tgt == ChannelTarget::UNSUPPORTED) continue;

        const cgltf_animation_sampler* gs = ch.sampler;
        if (!gs->input || !gs->output) continue;

        // Phase AX: 按 target 类型分流 — TRS 走 jointIndex 路径, MORPH_WEIGHTS 走 meshNodeIdx 路径
        Sampler s;
        s.target = tgt;
        s.mode   = ConvertInterpolation(gs->interpolation);

        if (tgt == ChannelTarget::MORPH_WEIGHTS) {
            // weights channel: target_node 通常是 mesh node (非 skin 关节)
            int nodeIdx  = FindNodeIndex(data, ch.target_node);
            int morphCnt = GetNodeMorphTargetCount(ch.target_node);
            if (morphCnt <= 0) continue;   // 该 node 无 morph mesh, 跳过
            s.meshNodeIdx = nodeIdx;
            s.jointIndex  = -1;
            s.components  = morphCnt;       // glTF spec: 每帧 N 个 weight
        } else {
            // TRS channel: 找 channel.target_node 对应的关节
            int jointIdx = FindJointInSkin(skin, ch.target_node);
            if (jointIdx < 0) continue;     // 该 channel 作用于 skin 之外的 node
            s.jointIndex  = jointIdx;
            s.meshNodeIdx = -1;
            s.components  = (tgt == ChannelTarget::ROTATION) ? FLOATS_R : FLOATS_T;
        }

        // input: keyframe 时间
        size_t timeCount = gs->input->count;
        s.times.resize(timeCount);
        cgltf_accessor_unpack_floats(gs->input, s.times.data(), timeCount);

        // output: 数据 (CUBICSPLINE 时 3*components/帧)
        size_t outputFloats = gs->output->count * cgltf_num_components(gs->output->type);
        s.values.resize(outputFloats);
        cgltf_accessor_unpack_floats(gs->output, s.values.data(), outputFloats);

        // duration = max(times)
        if (!s.times.empty() && s.times.back() > clip->duration) {
            clip->duration = s.times.back();
        }

        clip->samplers.push_back(std::move(s));
    }

    return clip;
}

// ==================== Light.Animation.LoadSkinnedGLTF(path) ====================

// 返回值:
//   成功: table { skeleton, clips = {[name] = clip, ...}, mesh = nil (Step 3 填充) }
//   失败: nil, err_string
static int l_Anim_LoadSkinnedGLTF(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&options, path, &data);
    if (r != cgltf_result_success) {
        lua_pushnil(L);
        lua_pushfstring(L, "cgltf_parse_file failed (err %d)", (int)r);
        return 2;
    }
    r = cgltf_load_buffers(&options, data, path);
    if (r != cgltf_result_success) {
        cgltf_free(data);
        lua_pushnil(L);
        lua_pushfstring(L, "cgltf_load_buffers failed (err %d)", (int)r);
        return 2;
    }

    // 取第一个 skin (典型 glTF 角色仅一个 skin, 多 skin 留 Phase AV.x)
    if (data->skins_count == 0) {
        cgltf_free(data);
        // 不视为致命错误: 允许加载非 skinned glTF (mesh=nil)
        lua_newtable(L);
        lua_pushnil(L);
        lua_setfield(L, -2, "skeleton");
        lua_newtable(L);
        lua_setfield(L, -2, "clips");
        lua_pushnil(L);
        lua_setfield(L, -2, "mesh");
        return 1;
    }

    const cgltf_skin* skin = &data->skins[0];

    std::string err;
    Skeleton* sk = BuildSkeleton(skin, err);
    if (!sk) {
        cgltf_free(data);
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }

    // 构建所有 AnimationClip
    std::vector<AnimationClip*> clips;
    clips.reserve(data->animations_count);
    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        // Phase AX: BuildClip 现在需要 data 参数 (用于 MORPH_WEIGHTS channel 的 nodeIdx 查找)
        AnimationClip* c = BuildClip(&data->animations[i], skin, data);
        clips.push_back(c);
    }

    // Step 3: 提取 SkinnedMesh (在 cgltf_free 前, 因为 prim 指针指向 cgltf_data 内部)
    SkinnedMeshAsset* skMesh = nullptr;
    const cgltf_primitive* prim = FindFirstSkinnedPrimitive(data, skin);
    if (prim) {
        skMesh = new SkinnedMeshAsset();
        std::string meshErr;
        if (!ExtractSkinMesh(prim, skMesh, meshErr)) {
            // 提取失败: 仅释放 mesh 资产, 不视为致命错误 (允许仅骨骼/动画的 glTF)
            delete skMesh;
            skMesh = nullptr;
        } else {
            skMesh->skeletonPtr = sk;     // 关联骨骼 (registry ref 在下面 push 后设)

            // Phase AX: 提取 morph target (找 prim 所属 mesh 取 weights[]/target_names[])
            const cgltf_mesh* gltfMesh = FindMeshForPrimitive(data, prim);
            std::string morphErr;
            if (!ExtractMorphTargets(prim, gltfMesh, skMesh, morphErr)) {
                // morph 提取失败 → 视为无 morph (skMesh 已 0 初始化), 仅打印 warning
                std::fprintf(stderr,
                             "[Phase AX] ExtractMorphTargets failed: %s (mesh kept without morph)\n",
                             morphErr.c_str());
                skMesh->morphTargetCount = 0;
                skMesh->morphTargets.clear();
                skMesh->morphDefaultWeights.clear();
                skMesh->morphTargetNames.clear();
            }
        }
    }

    cgltf_free(data);    // skin/animation/primitive 指针之后不可访问 (我们已经拷贝完所需数据)

    // 输出 table
    lua_newtable(L);

    // pack.skeleton
    PushSkeletonUserdata(L, sk);
    int skeletonStackIdx = lua_gettop(L);     // 记下 Skeleton userdata 在栈上的位置 (供 mesh ref 用)
    lua_pushvalue(L, skeletonStackIdx);
    lua_setfield(L, -3, "skeleton");          // table.skeleton = skeleton

    // pack.clips = { [name] = clip, ... }
    lua_newtable(L);
    for (size_t i = 0; i < clips.size(); ++i) {
        PushClipUserdata(L, clips[i]);
        lua_setfield(L, -2, clips[i]->name.c_str());
    }
    lua_setfield(L, -3, "clips");

    // pack.clipNames = { name1, name2, ... } 数组顺序
    lua_newtable(L);
    for (size_t i = 0; i < clips.size(); ++i) {
        lua_pushstring(L, clips[i]->name.c_str());
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -3, "clipNames");

    // pack.mesh: Step 3 — SkinnedMesh userdata 或 nil
    if (skMesh) {
        // 先把 Skeleton userdata 放到 registry, 防止被 GC (mesh 保活 skeleton)
        lua_pushvalue(L, skeletonStackIdx);
        skMesh->skeletonRef = luaL_ref(L, LUA_REGISTRYINDEX);
        PushSkinnedMeshUserdata(L, skMesh);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -3, "mesh");

    // 弹出栈顶的 Skeleton userdata (复制保留, 字段已 setfield)
    lua_pop(L, 1);
    return 1;
}

// ==================== Light.Animation.NewAnimator(skeleton) ====================

static int l_Anim_NewAnimator(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    if (!sk->alive) {
        lua_pushnil(L);
        lua_pushstring(L, "skeleton is dead");
        return 2;
    }

    Animator* an = new Animator();
    an->skeletonPtr = sk;

    // 增加 Skeleton userdata 强引用 (防 GC)
    lua_pushvalue(L, 1);
    an->skeletonRef = luaL_ref(L, LUA_REGISTRYINDEX);

    PushAnimatorUserdata(L, an);
    return 1;
}

// ==================== Phase AV.x: Procedural Skeleton / Clip 构造 ====================
// 动机: 不依赖 glTF 资产即可构造 Skeleton + Clip 用于单元测试 / 程序化动画.
// 使用模式:
//   local sk  = Light.Animation.NewEmptySkeleton(2)
//   sk:SetJointName(1, "root"); sk:SetJointParent(1, 0)      -- 0 = 无 parent
//   sk:SetJointName(2, "tip");  sk:SetJointParent(2, 1)      -- parent = root
//   sk:SetBindLocalTRS(1, 0,0,0,  1,0,0,0,  1,1,1)           -- (T, R_wxyz, S)
//
//   local clip = Light.Animation.NewEmptyClip("walk", 1.0)
//   clip:AddSampler(1, "rotation", "LINEAR",
//                    { 0.0, 0.5, 1.0 },
//                    { 0,0,0,1,  0,1,0,0,  0,0,0,1 })        -- 注意: rotation 采样值按 glTF xyzw 输入
// 旋转输入采用 glTF xyzw 约定 (与 LoadSkinnedGLTF 一致), 内部转 wxyz 存储.
static int l_Anim_NewEmptySkeleton(lua_State* L) {
    int n = (int)luaL_checkinteger(L, 1);
    if (n <= 0 || n > MAX_JOINTS) {
        lua_pushnil(L);
        lua_pushfstring(L, "joint count out of range: %d (must be 1..%d)", n, MAX_JOINTS);
        return 2;
    }
    Skeleton* sk = new Skeleton();
    sk->joints.resize((size_t)n);
    sk->inverseBindMatrices.assign((size_t)n * FLOATS_PER_MAT4, 0.0f);
    // 默认: 所有关节 parent=-1 (全部是根), bind = 单位变换, IBM = 单位矩阵
    for (int i = 0; i < n; ++i) {
        JointNode& jn = sk->joints[i];
        jn.name   = "joint_" + std::to_string(i);
        jn.parent = -1;
        jn.children.clear();
        jn.local_t[0] = jn.local_t[1] = jn.local_t[2] = 0.0f;
        jn.local_r[0] = 1.0f; jn.local_r[1] = jn.local_r[2] = jn.local_r[3] = 0.0f;
        jn.local_s[0] = jn.local_s[1] = jn.local_s[2] = 1.0f;
        sk->nameToIndex[jn.name] = i;
        float* m = &sk->inverseBindMatrices[(size_t)i * FLOATS_PER_MAT4];
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
    sk->rootJoint = 0;    // 默认第一个关节为根 (SetJointParent 后会重算)
    PushSkeletonUserdata(L, sk);
    return 1;
}

static int l_Anim_NewEmptyClip(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    float dur        = (float)luaL_optnumber(L, 2, 0.0);
    if (dur < 0.0f) dur = 0.0f;
    AnimationClip* c = new AnimationClip();
    c->name     = name;
    c->duration = dur;
    PushClipUserdata(L, c);
    return 1;
}

// ==================== Skeleton 方法 ====================

static int l_Skeleton_GetJointCount(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    lua_pushinteger(L, (lua_Integer)sk->joints.size());
    return 1;
}

static int l_Skeleton_GetJointName(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)sk->joints.size()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, sk->joints[idx - 1].name.c_str());
    return 1;
}

static int l_Skeleton_FindJoint(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    const char* name = luaL_checkstring(L, 2);
    auto it = sk->nameToIndex.find(name);
    if (it == sk->nameToIndex.end()) {
        lua_pushnil(L);
        lua_pushfstring(L, "joint not found: %s", name);
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(it->second + 1));    // 1-based
    return 1;
}

static int l_Skeleton_GetJointParent(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)sk->joints.size()) {
        lua_pushinteger(L, 0);    // 越界返回 0 (表示无 parent)
        return 1;
    }
    int p = sk->joints[idx - 1].parent;
    lua_pushinteger(L, p < 0 ? 0 : (lua_Integer)(p + 1));    // 1-based, 0 = 无 parent
    return 1;
}

static int l_Skeleton_GetRootJoint(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    lua_pushinteger(L, sk->rootJoint < 0 ? 0 : (lua_Integer)(sk->rootJoint + 1));
    return 1;
}

static int l_Skeleton_GetBindLocalTRS(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)sk->joints.size()) {
        lua_pushnil(L);
        lua_pushstring(L, "joint index out of range");
        return 2;
    }
    const JointNode& jn = sk->joints[idx - 1];
    // 推 10 个 float: tx, ty, tz, qw, qx, qy, qz, sx, sy, sz
    lua_pushnumber(L, jn.local_t[0]);
    lua_pushnumber(L, jn.local_t[1]);
    lua_pushnumber(L, jn.local_t[2]);
    lua_pushnumber(L, jn.local_r[0]);
    lua_pushnumber(L, jn.local_r[1]);
    lua_pushnumber(L, jn.local_r[2]);
    lua_pushnumber(L, jn.local_r[3]);
    lua_pushnumber(L, jn.local_s[0]);
    lua_pushnumber(L, jn.local_s[1]);
    lua_pushnumber(L, jn.local_s[2]);
    return 10;
}

static int l_Skeleton_GetInverseBindMatrix(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)sk->joints.size()) {
        lua_pushnil(L);
        lua_pushstring(L, "joint index out of range");
        return 2;
    }
    const float* m = &sk->inverseBindMatrices[(idx - 1) * FLOATS_PER_MAT4];
    lua_newtable(L);
    for (int k = 0; k < FLOATS_PER_MAT4; ++k) {
        lua_pushnumber(L, m[k]);
        lua_rawseti(L, -2, k + 1);
    }
    return 1;
}

// ---------- Phase AV.x: Skeleton procedural setter ----------

// SetJointName(idx1, name): 重命名关节; 同步更新 nameToIndex (删旧名, 添新名)
static int l_Skeleton_SetJointName(lua_State* L) {
    Skeleton* sk   = CheckSkeleton(L, 1);
    int idx        = (int)luaL_checkinteger(L, 2);    // 1-based
    const char* nm = luaL_checkstring(L, 3);
    if (idx < 1 || idx > (int)sk->joints.size()) {
        return luaL_error(L, "joint index out of range: %d", idx);
    }
    JointNode& jn = sk->joints[idx - 1];
    sk->nameToIndex.erase(jn.name);
    jn.name = nm;
    sk->nameToIndex[jn.name] = idx - 1;
    return 0;
}

// SetJointParent(idx1, parentIdx1_or_0): 0 = 无 parent; 自动重算 rootJoint + children 不维护
// (children 列表仅本阶段不依赖, ComputeJointMatrices DFS 用 parent 向上拓扑排序)
static int l_Skeleton_SetJointParent(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx      = (int)luaL_checkinteger(L, 2);     // 1-based
    int pidx     = (int)luaL_checkinteger(L, 3);     // 1-based, 0 = 无 parent
    int N        = (int)sk->joints.size();
    if (idx < 1 || idx > N) {
        return luaL_error(L, "joint index out of range: %d", idx);
    }
    if (pidx < 0 || pidx > N) {
        return luaL_error(L, "parent index out of range: %d", pidx);
    }
    if (pidx == idx) {
        return luaL_error(L, "joint cannot be its own parent (idx=%d)", idx);
    }
    sk->joints[idx - 1].parent = (pidx == 0) ? -1 : (pidx - 1);
    // 重算 rootJoint (第一个 parent==-1)
    sk->rootJoint = -1;
    for (size_t i = 0; i < sk->joints.size(); ++i) {
        if (sk->joints[i].parent < 0) { sk->rootJoint = (int)i; break; }
    }
    return 0;
}

// SetBindLocalTRS(idx1, tx,ty,tz, qw,qx,qy,qz, sx,sy,sz)
// 旋转四元数格式: wxyz (ChocoLight 内部约定)
static int l_Skeleton_SetBindLocalTRS(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx      = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)sk->joints.size()) {
        return luaL_error(L, "joint index out of range: %d", idx);
    }
    JointNode& jn = sk->joints[idx - 1];
    jn.local_t[0] = (float)luaL_checknumber(L, 3);
    jn.local_t[1] = (float)luaL_checknumber(L, 4);
    jn.local_t[2] = (float)luaL_checknumber(L, 5);
    jn.local_r[0] = (float)luaL_checknumber(L, 6);   // w
    jn.local_r[1] = (float)luaL_checknumber(L, 7);   // x
    jn.local_r[2] = (float)luaL_checknumber(L, 8);   // y
    jn.local_r[3] = (float)luaL_checknumber(L, 9);   // z
    jn.local_s[0] = (float)luaL_checknumber(L, 10);
    jn.local_s[1] = (float)luaL_checknumber(L, 11);
    jn.local_s[2] = (float)luaL_checknumber(L, 12);
    return 0;
}

// SetInverseBindMatrix(idx1, table_m16): 覆盖指定关节的逆绑定矩阵 (16 floats, 列主序)
static int l_Skeleton_SetInverseBindMatrix(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    int idx      = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)sk->joints.size()) {
        return luaL_error(L, "joint index out of range: %d", idx);
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    float* m = &sk->inverseBindMatrices[(size_t)(idx - 1) * FLOATS_PER_MAT4];
    for (int k = 0; k < FLOATS_PER_MAT4; ++k) {
        lua_rawgeti(L, 3, k + 1);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "inverse bind matrix element %d is not a number", k + 1);
        }
        m[k] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return 0;
}

static int l_Skeleton_IsAlive(lua_State* L) {
    Skeleton* sk = CheckSkeleton(L, 1);
    lua_pushboolean(L, sk->alive ? 1 : 0);
    return 1;
}

static int l_Skeleton_Delete(lua_State* L) {
    Skeleton** pp = (Skeleton**)luaL_checkudata(L, 1, SKELETON_MT);
    if (pp && *pp) {
        (*pp)->alive = false;
        delete *pp;
        *pp = nullptr;
    }
    return 0;
}

static int l_Skeleton_GC(lua_State* L) {
    return l_Skeleton_Delete(L);    // 与显式 Delete 等价
}

static int l_Skeleton_ToString(lua_State* L) {
    Skeleton** pp = (Skeleton**)luaL_checkudata(L, 1, SKELETON_MT);
    if (!pp || !*pp) {
        lua_pushstring(L, "Skeleton(dead)");
    } else {
        lua_pushfstring(L, "Skeleton(joints=%d)", (int)(*pp)->joints.size());
    }
    return 1;
}

// ==================== AnimationClip 方法 ====================

static int l_Clip_GetName(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    lua_pushstring(L, c->name.c_str());
    return 1;
}

static int l_Clip_GetDuration(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    lua_pushnumber(L, c->duration);
    return 1;
}

static int l_Clip_GetSamplerCount(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    lua_pushinteger(L, (lua_Integer)c->samplers.size());
    return 1;
}

// 返回 sampler 元数据 table {jointIndex, target, mode, keyframes}
static int l_Clip_GetSamplerInfo(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);    // 1-based
    if (idx < 1 || idx > (int)c->samplers.size()) {
        lua_pushnil(L);
        lua_pushstring(L, "sampler index out of range");
        return 2;
    }
    const Sampler& s = c->samplers[idx - 1];

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)(s.jointIndex + 1));    // 1-based
    lua_setfield(L, -2, "jointIndex");

    const char* targetStr = "unsupported";
    switch (s.target) {
        case ChannelTarget::TRANSLATION: targetStr = "translation"; break;
        case ChannelTarget::ROTATION:    targetStr = "rotation";    break;
        case ChannelTarget::SCALE:       targetStr = "scale";       break;
        default: break;
    }
    lua_pushstring(L, targetStr);
    lua_setfield(L, -2, "target");

    const char* modeStr = "linear";
    switch (s.mode) {
        case InterpMode::LINEAR:      modeStr = "linear";      break;
        case InterpMode::STEP:        modeStr = "step";        break;
        case InterpMode::CUBICSPLINE: modeStr = "cubicspline"; break;
    }
    lua_pushstring(L, modeStr);
    lua_setfield(L, -2, "mode");

    lua_pushinteger(L, (lua_Integer)s.times.size());
    lua_setfield(L, -2, "keyframes");

    lua_pushinteger(L, (lua_Integer)s.components);
    lua_setfield(L, -2, "components");

    return 1;
}

// Sample(t, jointIndex, target_str) → 按 target 不同返回 3 (T/S) 或 4 (R) floats
// Step 1 简化实现: 仅返回最近的 keyframe (无插值);
// Step 2 升级为 LINEAR/STEP/CUBICSPLINE 完整插值
static int l_Clip_Sample(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    float t          = (float)luaL_checknumber(L, 2);
    int jointIdx     = (int)luaL_checkinteger(L, 3) - 1;    // → 0-based
    const char* tgt  = luaL_checkstring(L, 4);

    ChannelTarget targetEnum = ChannelTarget::UNSUPPORTED;
    if (std::strcmp(tgt, "translation") == 0)      targetEnum = ChannelTarget::TRANSLATION;
    else if (std::strcmp(tgt, "rotation") == 0)    targetEnum = ChannelTarget::ROTATION;
    else if (std::strcmp(tgt, "scale") == 0)       targetEnum = ChannelTarget::SCALE;
    if (targetEnum == ChannelTarget::UNSUPPORTED) {
        lua_pushnil(L);
        lua_pushfstring(L, "unsupported target: %s", tgt);
        return 2;
    }

    // 找匹配 sampler
    const Sampler* found = nullptr;
    for (const Sampler& s : c->samplers) {
        if (s.jointIndex == jointIdx && s.target == targetEnum) {
            found = &s;
            break;
        }
    }
    if (!found || found->times.empty()) {
        // 无 sampler: 返回 bind pose 默认 (Step 2 时由 Animator 处理; Step 1 直接返回身份值)
        if (targetEnum == ChannelTarget::ROTATION) {
            lua_pushnumber(L, 1); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
            return 4;    // 单位四元数 wxyz
        } else if (targetEnum == ChannelTarget::SCALE) {
            lua_pushnumber(L, 1); lua_pushnumber(L, 1); lua_pushnumber(L, 1);
            return 3;
        } else {
            lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
            return 3;
        }
    }

    // Step 1: 找最接近 t 的 keyframe (无插值)
    size_t pick = 0;
    if (t <= found->times.front()) {
        pick = 0;
    } else if (t >= found->times.back()) {
        pick = found->times.size() - 1;
    } else {
        for (size_t i = 0; i + 1 < found->times.size(); ++i) {
            if (found->times[i] <= t && t < found->times[i + 1]) {
                // 更接近 i 还是 i+1
                pick = (t - found->times[i] < found->times[i + 1] - t) ? i : (i + 1);
                break;
            }
        }
    }

    int comps = found->components;
    // CUBICSPLINE 时数据布局 (in_tan, value, out_tan), Step 1 取 value 部分
    size_t baseOff = (found->mode == InterpMode::CUBICSPLINE)
                         ? (pick * comps * 3 + comps)    // skip in_tan
                         : (pick * comps);
    for (int k = 0; k < comps; ++k) {
        lua_pushnumber(L, found->values[baseOff + k]);
    }
    return comps;
}

// ---------- Phase AV.x: Clip procedural setter ----------

// SetDuration(sec): 显式设置时长; AddSampler 会自动 max(duration, sampler.times.back()).
static int l_Clip_SetDuration(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    float dur = (float)luaL_checknumber(L, 2);
    if (dur < 0.0f) dur = 0.0f;
    c->duration = dur;
    return 0;
}

// AddSampler(jointIdx1, target_str, mode_str, times_table, values_table)
//   target:  "translation" | "rotation" | "scale"
//   mode:    "LINEAR" | "STEP" | "CUBICSPLINE" (大小写不敏感)
//   times:   Lua array of seconds (升序; 不强制校验)
//   values:  Lua flat array of floats
//            T/S:        3 floats / keyframe  (LINEAR/STEP)   或 3*3 (CUBICSPLINE: in,value,out)
//            R:          4 floats / keyframe  (xyzw, glTF 约定; 内部转 wxyz)
// 自动更新 clip->duration = max(duration, times.back()).
// 返回 nil + err 对(Lua 端: ok, err = c:AddSampler(...))
// 不调用 lua_error, 不走 longjmp, 完全避开 Lumen 在 MSVC 上观察到的 raise 路径不稳定性.
// 当 ErrorReturn 返回后, C 栈上所有非 trivial 析构仍会正常调用, 完全 C++ safe.
static int ErrorReturn(lua_State* L, const char* msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}
static int ErrorReturnF(lua_State* L, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    return ErrorReturn(L, buf);
}

static int l_Clip_AddSampler(lua_State* L) {
    AnimationClip* c   = CheckClip(L, 1);
    int jointIdx       = (int)luaL_checkinteger(L, 2) - 1;    // 1-based → 0-based
    // 设计决策: 不用 luaL_error/lua_error (longjmp), 改用 return nil, err 模式.
    // 原因: 在 Lumen + MSVC 上多次观测 lua_error 路径崩溃 (可能与 longjmp 跳过 MSVC /GS cookie
    //       校验 + 栈 char[] 布局有关, 无法根治, 只能绕开).
    // Lua 端调用约定: local ok, err = c:AddSampler(...)
    //                ok == nil 表示失败, err 为描述字符串; ok == nothing(nil) 也代表成功时不 push 返回值.
    //   为了明确区分 "成功无返回值" 和 "失败 nil+err", 成功时 push true + nil, 失败时 nil + err.
    const char* tgtRaw = luaL_checkstring(L, 3);
    const char* modRaw = luaL_checkstring(L, 4);
    char tgtBuf[32]; char modBuf[32];
    {
        size_t n = std::strlen(tgtRaw); if (n >= sizeof(tgtBuf)) n = sizeof(tgtBuf) - 1;
        std::memcpy(tgtBuf, tgtRaw, n); tgtBuf[n] = '\0';
    }
    {
        size_t n = std::strlen(modRaw); if (n >= sizeof(modBuf)) n = sizeof(modBuf) - 1;
        std::memcpy(modBuf, modRaw, n); modBuf[n] = '\0';
    }
    luaL_checktype(L, 5, LUA_TTABLE);
    luaL_checktype(L, 6, LUA_TTABLE);

    if (jointIdx < 0) {
        return ErrorReturnF(L, "joint index must be >= 1 (got %d)", jointIdx + 1);
    }

    ChannelTarget target = ChannelTarget::UNSUPPORTED;
    if (std::strcmp(tgtBuf, "translation") == 0)      target = ChannelTarget::TRANSLATION;
    else if (std::strcmp(tgtBuf, "rotation") == 0)    target = ChannelTarget::ROTATION;
    else if (std::strcmp(tgtBuf, "scale") == 0)       target = ChannelTarget::SCALE;
    else {
        return ErrorReturnF(L, "unsupported target: %s (expected translation/rotation/scale)", tgtBuf);
    }

    InterpMode mode = InterpMode::LINEAR;
    // 手动小写比较 (避免引入 tolower 依赖)
    auto ieq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 'a' + 'A') : *a;
            char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 'a' + 'A') : *b;
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == *b;
    };
    if (ieq(modBuf, "LINEAR"))           mode = InterpMode::LINEAR;
    else if (ieq(modBuf, "STEP"))        mode = InterpMode::STEP;
    else if (ieq(modBuf, "CUBICSPLINE")) mode = InterpMode::CUBICSPLINE;
    else {
        return ErrorReturnF(L, "unsupported mode: %s (expected LINEAR/STEP/CUBICSPLINE)", modBuf);
    }

    int comps = (target == ChannelTarget::ROTATION) ? FLOATS_R : FLOATS_T;

    int nTimes = (int)lua_objlen(L, 5);
    if (nTimes <= 0) {
        return ErrorReturn(L, "times table is empty");
    }
    int perKey   = (mode == InterpMode::CUBICSPLINE) ? (comps * 3) : comps;
    int nValsExp = nTimes * perKey;
    int nValsGot = (int)lua_objlen(L, 6);
    if (nValsGot != nValsExp) {
        return ErrorReturnF(L, "values count mismatch: expected %d (%d keys * %d), got %d",
                            nValsExp, nTimes, perKey, nValsGot);
    }

    // 内容预扫: 所有元素都是 number (失败也走 return nil, err)
    for (int i = 0; i < nTimes; ++i) {
        lua_rawgeti(L, 5, i + 1);
        int isNum = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (!isNum) {
            return ErrorReturnF(L, "times[%d] is not a number", i + 1);
        }
    }
    for (int i = 0; i < nValsGot; ++i) {
        lua_rawgeti(L, 6, i + 1);
        int isNum = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (!isNum) {
            return ErrorReturnF(L, "values[%d] is not a number", i + 1);
        }
    }

    // -- 成功路径: 分配 Sampler 并 push_back --
    Sampler s;
    s.jointIndex = jointIdx;
    s.target     = target;
    s.mode       = mode;
    s.components = comps;
    s.times.resize((size_t)nTimes);
    s.values.resize((size_t)nValsGot);
    for (int i = 0; i < nTimes; ++i) {
        lua_rawgeti(L, 5, i + 1);
        s.times[(size_t)i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    for (int i = 0; i < nValsGot; ++i) {
        lua_rawgeti(L, 6, i + 1);
        s.values[(size_t)i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    // Rotation 输入 xyzw (glTF 约定), 存储统一转 wxyz + 归一化的逻辑在 EvaluateSampler
    // 中统一处理 (与 LoadSkinnedGLTF 一致, 采样时转换, 存储保持原状).
    c->samplers.push_back(std::move(s));

    // 自动推进 duration (取所有 sampler times.back() 的 max)
    float tBack = c->samplers.back().times.back();
    if (tBack > c->duration) c->duration = tBack;
    return 0;
}

static int l_Clip_IsAlive(lua_State* L) {
    AnimationClip* c = CheckClip(L, 1);
    lua_pushboolean(L, c->alive ? 1 : 0);
    return 1;
}

static int l_Clip_Delete(lua_State* L) {
    AnimationClip** pp = (AnimationClip**)luaL_checkudata(L, 1, CLIP_MT);
    if (pp && *pp) {
        (*pp)->alive = false;
        delete *pp;
        *pp = nullptr;
    }
    return 0;
}

static int l_Clip_GC(lua_State* L) {
    return l_Clip_Delete(L);
}

static int l_Clip_ToString(lua_State* L) {
    AnimationClip** pp = (AnimationClip**)luaL_checkudata(L, 1, CLIP_MT);
    if (!pp || !*pp) {
        lua_pushstring(L, "AnimationClip(dead)");
    } else {
        lua_pushfstring(L, "AnimationClip(name=%s, dur=%.3f, samplers=%d)",
                        (*pp)->name.c_str(), (double)(*pp)->duration,
                        (int)(*pp)->samplers.size());
    }
    return 1;
}

// ==================== Animator 方法 (Step 1 占位; Step 2/4 完整化) ====================

static int l_Animator_GetSkeleton(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    // 通过 registry ref 取出原 Skeleton userdata (保持身份一致)
    if (an->skeletonRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, an->skeletonRef);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// Helper: clip time wrap/clamp 一次 (用于 active 与 crossfade clip 共用)
static float WrapClipTime(float t, float dur, bool looping) {
    if (dur <= 1e-6f) return t;
    if (looping) {
        float r = std::fmod(t, dur);
        if (r < 0) r += dur;
        return r;
    }
    if (t < 0)   t = 0;
    if (t > dur) t = dur;
    return t;
}

// Step 2/4: 推进时间 + 处理 looping + crossfade + transitions + events + 重新计算关节矩阵
static int l_Animator_Update(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    float dt = (float)luaL_checknumber(L, 2);

    an->transitionedThisFrame = false;
    an->prevTime              = an->currentTime;

    // 推进 active clip 时间
    if (!an->paused) {
        an->currentTime += dt * an->speed;
    }
    if (an->activeClip) {
        an->currentTime = WrapClipTime(an->currentTime, an->activeClip->duration, an->looping);
    }

    // Step 4: crossfade 推进
    if (!an->crossfadeTarget.empty() && an->crossfadeClip && an->crossfadeDuration > 1e-6f) {
        if (!an->paused) {
            an->crossfadeProgress += dt / an->crossfadeDuration;
            an->crossfadeClipTime += dt * an->speed;
        }
        an->crossfadeClipTime = WrapClipTime(an->crossfadeClipTime,
                                              an->crossfadeClip->duration, an->looping);
        // 完成 → 切换到目标 state
        if (an->crossfadeProgress >= 1.0f) {
            an->currentState     = an->crossfadeTarget;
            an->activeClip       = an->crossfadeClip;
            an->currentTime      = an->crossfadeClipTime;
            an->prevTime         = an->currentTime;     // 防止事件被立即触发
            an->crossfadeTarget.clear();
            an->crossfadeClip    = nullptr;
            an->crossfadeProgress = 0.0f;
            an->crossfadeDuration = 0.0f;
            an->crossfadeClipTime = 0.0f;
        }
    }

    // Step 4: 检查 transitions (同帧最多一次, 不在 crossfade 中检查)
    if (!an->transitionedThisFrame && an->crossfadeTarget.empty()) {
        for (TransitionDef& tr : an->transitions) {
            // fromState 检查 (空 = Any state)
            if (!tr.fromState.empty() && tr.fromState != an->currentState) continue;
            if (tr.toState.empty())                     continue;
            if (tr.toState == an->currentState)         continue;       // idle→idle 拒绝
            auto it = an->states.find(tr.toState);
            if (it == an->states.end())                 continue;
            if (!CallTransitionCond(L, 1, tr.condFnRef)) continue;

            // 触发: 立即切换 (duration<=0) 或启动 crossfade
            if (tr.duration < 1e-6f) {
                an->currentState = tr.toState;
                an->activeClip   = it->second;
                an->currentTime  = 0.0f;
                an->prevTime     = 0.0f;
            } else {
                an->crossfadeTarget   = tr.toState;
                an->crossfadeClip     = it->second;
                an->crossfadeClipTime = 0.0f;
                an->crossfadeProgress = 0.0f;
                an->crossfadeDuration = tr.duration;
            }
            an->transitionedThisFrame = true;
            break;     // 同帧最多一次
        }
    }

    // Step 4: 触发 events (基于 prevTime → currentTime 区间)
    if (an->activeClip && !an->currentState.empty()) {
        float dur = an->activeClip->duration;
        // 复制一份 events 防止 callback 改 events 列表 (e.g. AddEvent / ClearEvents)
        // 但这里 callback 通常不会改, 性能优先不复制
        for (size_t k = 0; k < an->events.size(); ++k) {
            const EventDef& ev = an->events[k];
            if (ev.state != an->currentState) continue;
            if (EventTriggered(an->prevTime, an->currentTime, ev.triggerTime, dur, an->looping)) {
                CallEventCallback(L, 1, ev.callbackRef);
                // callback 可能 Delete animator 或修 events 数组; 若 alive 标志变 false 则停止
                if (!an->alive) return 0;
            }
        }
    }

    // 重新计算关节矩阵 (即使 paused 也要算一次, 因为 SetCurrentTime 后用户可能手动 Update(0))
    if (an->skeletonPtr && an->skeletonPtr->alive) {
        if (!an->crossfadeTarget.empty() && an->crossfadeClip) {
            ComputeJointMatricesBlended(an->skeletonPtr,
                                          an->activeClip,    an->currentTime,
                                          an->crossfadeClip, an->crossfadeClipTime,
                                          an->crossfadeProgress,
                                          an->jointMatrices);
        } else {
            ComputeJointMatrices(an->skeletonPtr, an->activeClip,
                                   an->currentTime, an->jointMatrices);
        }
    }

    // Phase AX: 评估 morph weights (动画通道驱动 + 手动覆盖)
    EvaluateMorphWeights(an);

    return 0;
}

static int l_Animator_GetCurrentTime(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushnumber(L, an->currentTime);
    return 1;
}

static int l_Animator_SetCurrentTime(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    an->currentTime = (float)luaL_checknumber(L, 2);
    return 0;
}

static int l_Animator_SetSpeed(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    an->speed = (float)luaL_checknumber(L, 2);
    return 0;
}

static int l_Animator_Pause(lua_State* L)  { Animator* an = CheckAnimator(L, 1); an->paused = true;  return 0; }
static int l_Animator_Resume(lua_State* L) { Animator* an = CheckAnimator(L, 1); an->paused = false; return 0; }
static int l_Animator_IsPaused(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushboolean(L, an->paused ? 1 : 0);
    return 1;
}

// Step 2: 返回 N*16 floats table (列主序). 若从未调用 Update, 自动计算一次绑定姿态.
static int l_Animator_GetJointMatrices(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);

    // 如果 jointMatrices 为空, 自动用 bind pose 计算一次 (避免用户忘了 Update)
    if (an->jointMatrices.empty() && an->skeletonPtr && an->skeletonPtr->alive) {
        ComputeJointMatrices(an->skeletonPtr, an->activeClip, an->currentTime, an->jointMatrices);
    }

    int N = (int)an->jointMatrices.size();
    lua_createtable(L, N, 0);
    for (int i = 0; i < N; ++i) {
        lua_pushnumber(L, an->jointMatrices[i]);
        lua_rawseti(L, -2, i + 1);    // 1-based
    }
    return 1;
}

// ---------- Step 2: 状态机基础 (单状态切换) ----------

// AddState(name, clip): 把 clip 加入状态表; 持有 clip 强引用
static int l_Animator_AddState(lua_State* L) {
    Animator* an    = CheckAnimator(L, 1);
    const char* nm  = luaL_checkstring(L, 2);
    AnimationClip* c = CheckClip(L, 3);
    if (!c->alive) {
        lua_pushnil(L);
        lua_pushstring(L, "clip is dead");
        return 2;
    }

    // 已有同名 state? unref 旧的
    auto itRef = an->stateRefs.find(nm);
    if (itRef != an->stateRefs.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, itRef->second);
    }

    // 持 clip 强引用
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    an->states[nm]    = c;
    an->stateRefs[nm] = ref;

    lua_pushboolean(L, 1);
    return 1;
}

// Play(name): 切换到指定状态; 重置 currentTime=0; 找不到 → nil + err
static int l_Animator_Play(lua_State* L) {
    Animator* an   = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);

    auto it = an->states.find(nm);
    if (it == an->states.end()) {
        lua_pushnil(L);
        lua_pushfstring(L, "state not found: %s", nm);
        return 2;
    }
    an->currentState = nm;
    an->activeClip   = it->second;
    an->currentTime  = 0.0f;

    // 立即计算一帧关节矩阵 (允许用户 Play() 后立即 GetJointMatrices)
    if (an->skeletonPtr && an->skeletonPtr->alive) {
        ComputeJointMatrices(an->skeletonPtr, an->activeClip, 0.0f, an->jointMatrices);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_Animator_Stop(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    an->currentState.clear();
    an->activeClip = nullptr;
    an->currentTime = 0.0f;
    an->prevTime    = 0.0f;     // Step 4: 重置, 防止后续 Update 触发跨周期 event
    // Step 4: 清 crossfade
    an->crossfadeTarget.clear();
    an->crossfadeClip     = nullptr;
    an->crossfadeProgress = 0.0f;
    an->crossfadeDuration = 0.0f;
    an->crossfadeClipTime = 0.0f;
    // 关节矩阵保留 bind pose: 用 skeleton 重新算一次
    if (an->skeletonPtr && an->skeletonPtr->alive) {
        ComputeJointMatrices(an->skeletonPtr, nullptr, 0.0f, an->jointMatrices);
    }
    return 0;
}

static int l_Animator_GetCurrentState(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    if (an->currentState.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, an->currentState.c_str());
    }
    return 1;
}

static int l_Animator_GetStateCount(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushinteger(L, (lua_Integer)an->states.size());
    return 1;
}

static int l_Animator_HasState(lua_State* L) {
    Animator* an   = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);
    lua_pushboolean(L, an->states.count(nm) ? 1 : 0);
    return 1;
}

static int l_Animator_SetLooping(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    an->looping = lua_toboolean(L, 2) != 0;
    return 0;
}

static int l_Animator_IsLooping(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushboolean(L, an->looping ? 1 : 0);
    return 1;
}

// ---------- Step 4: 状态转换 (Transition) ----------

// AddTransition(fromState_or_empty, toState, condFn, duration_or_0)
//   fromState: string 或 "" (Any state); condFn: function(animator) -> bool
//   duration: 秒, 0 = 立即切换 (无 fade)
static int l_Animator_AddTransition(lua_State* L) {
    Animator* an   = CheckAnimator(L, 1);
    const char* from = luaL_checkstring(L, 2);     // 可空串
    const char* to   = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    float dur = (float)luaL_optnumber(L, 5, 0.0);
    if (dur < 0) dur = 0;

    // 持 condFn 强引用
    lua_pushvalue(L, 4);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    TransitionDef tr;
    tr.fromState  = from;
    tr.toState    = to;
    tr.condFnRef  = ref;
    tr.duration   = dur;
    an->transitions.push_back(std::move(tr));

    lua_pushinteger(L, (lua_Integer)an->transitions.size());     // 返回新 transition 的索引 (1-based)
    return 1;
}

// ClearTransitions(): 清所有 transition + 释放 condFn ref
static int l_Animator_ClearTransitions(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    for (TransitionDef& tr : an->transitions) {
        if (tr.condFnRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, tr.condFnRef);
            tr.condFnRef = LUA_NOREF;
        }
    }
    an->transitions.clear();
    return 0;
}

static int l_Animator_GetTransitionCount(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushinteger(L, (lua_Integer)an->transitions.size());
    return 1;
}

// ---------- Step 4: Crossfade (手动触发) ----------

// Crossfade(targetState, duration): 立即启动 crossfade 到目标 state
//   找不到目标 state → nil + err
//   duration <= 0 → 立即切换 (无 fade)
//   已在 crossfade 中 → 覆盖原 crossfade
static int l_Animator_Crossfade(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);
    float dur = (float)luaL_optnumber(L, 3, 0.3);
    if (dur < 0) dur = 0;

    auto it = an->states.find(nm);
    if (it == an->states.end()) {
        lua_pushnil(L);
        lua_pushfstring(L, "state not found: %s", nm);
        return 2;
    }
    // 已在该 state 且无 crossfade → 拒绝 (避免抖动)
    if (an->currentState == nm && an->crossfadeTarget.empty()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "already in target state");
        return 2;
    }

    if (dur < 1e-6f) {
        // 立即切换
        an->currentState = nm;
        an->activeClip   = it->second;
        an->currentTime  = 0.0f;
        an->prevTime     = 0.0f;
        an->crossfadeTarget.clear();
        an->crossfadeClip = nullptr;
        an->crossfadeProgress = 0.0f;
        an->crossfadeDuration = 0.0f;
        an->crossfadeClipTime = 0.0f;
    } else {
        an->crossfadeTarget   = nm;
        an->crossfadeClip     = it->second;
        an->crossfadeClipTime = 0.0f;
        an->crossfadeProgress = 0.0f;
        an->crossfadeDuration = dur;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_Animator_IsCrossfading(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    bool fading = !an->crossfadeTarget.empty() && an->crossfadeClip &&
                  an->crossfadeDuration > 1e-6f;
    lua_pushboolean(L, fading ? 1 : 0);
    return 1;
}

static int l_Animator_GetCrossfadeProgress(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushnumber(L, an->crossfadeProgress);
    return 1;
}

static int l_Animator_GetCrossfadeTarget(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    if (an->crossfadeTarget.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, an->crossfadeTarget.c_str());
    }
    return 1;
}

// ---------- Step 4: 事件帧 (Event) ----------

// AddEvent(state, triggerTime, callbackFn): 关联到指定 state
//   state: string (必须已 AddState; 不强校验, 后续 Update 时按 currentState 过滤)
//   triggerTime: 秒
//   callbackFn: function(animator)
static int l_Animator_AddEvent(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    const char* st = luaL_checkstring(L, 2);
    float tt = (float)luaL_checknumber(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    lua_pushvalue(L, 4);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    EventDef ev;
    ev.state       = st;
    ev.triggerTime = tt;
    ev.callbackRef = ref;
    an->events.push_back(std::move(ev));

    lua_pushinteger(L, (lua_Integer)an->events.size());
    return 1;
}

// ClearEvents(): 清所有 event + 释放 callback ref
static int l_Animator_ClearEvents(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    for (EventDef& ev : an->events) {
        if (ev.callbackRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ev.callbackRef);
            ev.callbackRef = LUA_NOREF;
        }
    }
    an->events.clear();
    return 0;
}

static int l_Animator_GetEventCount(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushinteger(L, (lua_Integer)an->events.size());
    return 1;
}

// ---------- Step 4: 参数 (Param) ----------

// SetParam(name, value): 设 number 参数 (供 transition condFn 读取)
static int l_Animator_SetParam(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    an->params[nm] = v;
    return 0;
}

// GetParam(name) → number 或 nil (未设置)
static int l_Animator_GetParam(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);
    auto it = an->params.find(nm);
    if (it == an->params.end()) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, it->second);
    }
    return 1;
}

static int l_Animator_HasParam(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);
    lua_pushboolean(L, an->params.count(nm) ? 1 : 0);
    return 1;
}

static int l_Animator_GetPrevTime(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushnumber(L, an->prevTime);
    return 1;
}

// ---------- Phase AV.x: Animator 读取 (调试/内省) ----------

// GetClip(name) → Clip userdata 或 nil (通过 registry ref 取, 保持身份一致)
static int l_Animator_GetClip(lua_State* L) {
    Animator* an   = CheckAnimator(L, 1);
    const char* nm = luaL_checkstring(L, 2);
    auto it = an->stateRefs.find(nm);
    if (it == an->stateRefs.end() || it->second == LUA_NOREF) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
    return 1;
}

// GetActiveClip() → Clip userdata 或 nil (当前 state 的 clip)
static int l_Animator_GetActiveClip(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    if (an->currentState.empty()) {
        lua_pushnil(L);
        return 1;
    }
    auto it = an->stateRefs.find(an->currentState);
    if (it == an->stateRefs.end() || it->second == LUA_NOREF) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
    return 1;
}

// ListStates() → { "idle", "walk", ... } (无序 table array; 顺序未指定)
static int l_Animator_ListStates(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_newtable(L);
    int i = 1;
    for (const auto& kv : an->states) {
        lua_pushstring(L, kv.first.c_str());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// GetTransitionInfo(idx1) → { from, to, duration, hasCond } 或 nil (越界)
static int l_Animator_GetTransitionInfo(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    int i = (int)luaL_checkinteger(L, 2);    // 1-based
    if (i < 1 || i > (int)an->transitions.size()) {
        lua_pushnil(L);
        return 1;
    }
    const TransitionDef& tr = an->transitions[(size_t)(i - 1)];
    lua_newtable(L);
    lua_pushstring(L, tr.fromState.c_str());
    lua_setfield(L, -2, "from");
    lua_pushstring(L, tr.toState.c_str());
    lua_setfield(L, -2, "to");
    lua_pushnumber(L, tr.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, tr.condFnRef != LUA_NOREF ? 1 : 0);
    lua_setfield(L, -2, "hasCond");
    return 1;
}

// GetEventInfo(idx1) → { state, triggerTime, hasCallback } 或 nil (越界)
static int l_Animator_GetEventInfo(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    int i = (int)luaL_checkinteger(L, 2);    // 1-based
    if (i < 1 || i > (int)an->events.size()) {
        lua_pushnil(L);
        return 1;
    }
    const EventDef& ev = an->events[(size_t)(i - 1)];
    lua_newtable(L);
    lua_pushstring(L, ev.state.c_str());
    lua_setfield(L, -2, "state");
    lua_pushnumber(L, ev.triggerTime);
    lua_setfield(L, -2, "triggerTime");
    lua_pushboolean(L, ev.callbackRef != LUA_NOREF ? 1 : 0);
    lua_setfield(L, -2, "hasCallback");
    return 1;
}

// ListParams() → { [name] = value, ... } (键值 table)
static int l_Animator_ListParams(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_newtable(L);
    for (const auto& kv : an->params) {
        lua_pushnumber(L, kv.second);
        lua_setfield(L, -2, kv.first.c_str());
    }
    return 1;
}

static int l_Animator_IsAlive(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    lua_pushboolean(L, an->alive ? 1 : 0);
    return 1;
}

static int l_Animator_Delete(lua_State* L) {
    Animator** pp = (Animator**)luaL_checkudata(L, 1, ANIMATOR_MT);
    if (pp && *pp) {
        Animator* an = *pp;
        an->alive = false;
        if (an->skeletonRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, an->skeletonRef);
            an->skeletonRef = LUA_NOREF;
        }
        // Step 2: 释放所有 clip 强引用
        for (auto& kv : an->stateRefs) {
            if (kv.second != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, kv.second);
            }
        }
        an->stateRefs.clear();
        an->states.clear();
        an->activeClip = nullptr;

        // Step 4: 释放 transition condFn refs + event callback refs
        for (TransitionDef& tr : an->transitions) {
            if (tr.condFnRef != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, tr.condFnRef);
                tr.condFnRef = LUA_NOREF;
            }
        }
        an->transitions.clear();
        for (EventDef& ev : an->events) {
            if (ev.callbackRef != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, ev.callbackRef);
                ev.callbackRef = LUA_NOREF;
            }
        }
        an->events.clear();
        an->params.clear();
        an->crossfadeClip = nullptr;

        delete an;
        *pp = nullptr;
    }
    return 0;
}

static int l_Animator_GC(lua_State* L) {
    return l_Animator_Delete(L);
}

static int l_Animator_ToString(lua_State* L) {
    Animator** pp = (Animator**)luaL_checkudata(L, 1, ANIMATOR_MT);
    if (!pp || !*pp) {
        lua_pushstring(L, "Animator(dead)");
    } else {
        lua_pushfstring(L, "Animator(t=%.3f, paused=%s)",
                        (double)(*pp)->currentTime,
                        (*pp)->paused ? "true" : "false");
    }
    return 1;
}

// ==================== Step 3: SkinnedMesh 方法 ====================

static int l_SkinnedMesh_GetVertexCount(lua_State* L) {
    SkinnedMeshAsset* sm = CheckSkinnedMesh(L, 1);
    lua_pushinteger(L, (lua_Integer)sm->baseVertices.size());
    return 1;
}

static int l_SkinnedMesh_GetIndexCount(lua_State* L) {
    SkinnedMeshAsset* sm = CheckSkinnedMesh(L, 1);
    lua_pushinteger(L, (lua_Integer)sm->indices.size());
    return 1;
}

// 返回 Skeleton userdata (复用 mesh 持的 registry ref, 保持身份一致)
static int l_SkinnedMesh_GetSkeleton(lua_State* L) {
    SkinnedMeshAsset* sm = CheckSkinnedMesh(L, 1);
    if (sm->skeletonRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sm->skeletonRef);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_SkinnedMesh_IsAlive(lua_State* L) {
    SkinnedMeshAsset* sm = CheckSkinnedMesh(L, 1);
    lua_pushboolean(L, sm->alive ? 1 : 0);
    return 1;
}

static int l_SkinnedMesh_Delete(lua_State* L) {
    SkinnedMeshAsset** pp = (SkinnedMeshAsset**)luaL_checkudata(L, 1, SKINNED_MESH_MT);
    if (pp && *pp) {
        SkinnedMeshAsset* sm = *pp;
        sm->alive = false;
        // 释放 CPU 路径 GPU mesh (若已创建)
        if (sm->gpuMeshId && g_render) {
            g_render->DeleteMesh(sm->gpuMeshId);
            sm->gpuMeshId = 0;
        }
        // Phase AW: 释放 GPU skinning 路径的 mesh (若已上传)
        if (sm->gpuSkinnedMeshId && g_render) {
            g_render->DeleteMesh(sm->gpuSkinnedMeshId);
            sm->gpuSkinnedMeshId = 0;
            sm->gpuMeshUploaded  = false;
        }
        // 释放 Skeleton 强引用
        if (sm->skeletonRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, sm->skeletonRef);
            sm->skeletonRef = LUA_NOREF;
        }
        delete sm;
        *pp = nullptr;
    }
    return 0;
}

static int l_SkinnedMesh_GC(lua_State* L) {
    return l_SkinnedMesh_Delete(L);
}

static int l_SkinnedMesh_ToString(lua_State* L) {
    SkinnedMeshAsset** pp = (SkinnedMeshAsset**)luaL_checkudata(L, 1, SKINNED_MESH_MT);
    if (!pp || !*pp) {
        lua_pushstring(L, "SkinnedMesh(dead)");
    } else {
        lua_pushfstring(L, "SkinnedMesh(verts=%d, idx=%d)",
                        (int)(*pp)->baseVertices.size(),
                        (int)(*pp)->indices.size());
    }
    return 1;
}

// ==================== Step 3: Light.Animation.DrawSkinnedMesh ====================

// 从 Lua table (16 floats, 列主序) 读 mat4. 返回 false 表示不是 16 元 table.
static bool ReadMat4FromTable(lua_State* L, int idx, float* outMat) {
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    for (int i = 0; i < 16; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_type(L, -1) != LUA_TNUMBER) {
            lua_pop(L, 1);
            return false;
        }
        outMat[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

// Phase AW — 决定本次 DrawSkinnedMesh 走哪条路径
//   返回 true => GPU skinning; false => CPU skinning (现有路径).
static bool ShouldUseGPUSkinning() {
    if (!g_render) return false;
    switch (g_skinningMode) {
        case SkinningMode::CPU: return false;
        case SkinningMode::GPU: return g_render->SupportsGPUSkinning();
        case SkinningMode::AUTO:
        default:
#if defined(__EMSCRIPTEN__)
            // Q7: Web (Emscripten) 默认 CPU 路径; 用户可 SetSkinningMode("gpu") 强开
            return false;
#else
            return g_render->SupportsGPUSkinning();
#endif
    }
}

// CPU 蒙皮主体 (Phase AV Step 3 路径; Phase AW 抽函数, 算法不变)
//   语义: 每帧 CPU 加权变换 baseVertices 到 skinnedVertices, 烘焙 modelMat,
//         然后 DeleteMesh + CreateMesh 全量重传 -> DrawMeshMaterial
static int DrawSkinnedMeshCPU(lua_State* L, SkinnedMeshAsset* sm, Animator* an,
                                const float* modelMat, const MaterialDesc* matDesc) {
    int N    = (int)sm->baseVertices.size();
    int jCnt = (int)(an->jointMatrices.size() / 16);
    if (sm->skinnedVertices.size() != (size_t)N) sm->skinnedVertices.resize(N);

    for (int i = 0; i < N; ++i) {
        const RenderVertex3D& vBase = sm->baseVertices[i];
        RenderVertex3D&       vOut  = sm->skinnedVertices[i];
        // 拷贝 UV / color (蒙皮不变)
        vOut.u = vBase.u; vOut.v = vBase.v;
        vOut.r = vBase.r; vOut.g = vBase.g; vOut.b = vBase.b; vOut.a = vBase.a;

        uint32_t packed = sm->jointIndicesPacked[i];
        uint8_t joints[4] = {
            (uint8_t)(packed & 0xFF),
            (uint8_t)((packed >> 8)  & 0xFF),
            (uint8_t)((packed >> 16) & 0xFF),
            (uint8_t)((packed >> 24) & 0xFF),
        };
        const float* w = &sm->weights[(size_t)i * 4];
        float posIn[3] = { vBase.x, vBase.y, vBase.z };
        float nrmIn[3] = { vBase.nx, vBase.ny, vBase.nz };
        float posOut[3], nrmOut[3];
        CpuSkinVertex(an->jointMatrices.data(), jCnt, joints, w,
                       posIn, nrmIn, posOut, nrmOut);

        // 应用 modelMat (transform) 在蒙皮之上
        vOut.x  = modelMat[0] * posOut[0] + modelMat[4] * posOut[1] + modelMat[8]  * posOut[2] + modelMat[12];
        vOut.y  = modelMat[1] * posOut[0] + modelMat[5] * posOut[1] + modelMat[9]  * posOut[2] + modelMat[13];
        vOut.z  = modelMat[2] * posOut[0] + modelMat[6] * posOut[1] + modelMat[10] * posOut[2] + modelMat[14];
        vOut.nx = modelMat[0] * nrmOut[0] + modelMat[4] * nrmOut[1] + modelMat[8]  * nrmOut[2];
        vOut.ny = modelMat[1] * nrmOut[0] + modelMat[5] * nrmOut[1] + modelMat[9]  * nrmOut[2];
        vOut.nz = modelMat[2] * nrmOut[0] + modelMat[6] * nrmOut[1] + modelMat[10] * nrmOut[2];
    }

    // 重建 GPU mesh: 每帧 DeleteMesh + CreateMesh
    if (sm->gpuMeshId) {
        g_render->DeleteMesh(sm->gpuMeshId);
        sm->gpuMeshId = 0;
    }
    sm->gpuMeshId = g_render->CreateMesh(sm->skinnedVertices.data(), N,
                                           sm->indices.data(), (int)sm->indices.size());
    if (!sm->gpuMeshId) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "CreateMesh failed (GPU upload error)");
        return 2;
    }

    g_render->DrawMeshMaterial(sm->gpuMeshId, matDesc);
    lua_pushboolean(L, 1);
    return 1;
}

// Phase AW — GPU 蒙皮主体 (一次性上传顶点 + 每帧上传 jointMatrices UBO)
//   - 首次调用时构建 RenderVertex3DSkin 数组 + CreateSkinnedMesh -> gpuSkinnedMeshId
//   - 把 modelMat 前乘到每个 jointMatrix (烘焙 transform), 上传 UBO + 渲染
//   - 失败时 fallback 到 CPU 路径
static int DrawSkinnedMeshGPU(lua_State* L, SkinnedMeshAsset* sm, Animator* an,
                                const float* modelMat, const MaterialDesc* matDesc) {
    // 1. 首次调用: 构建 skin verts 并上传
    if (!sm->gpuMeshUploaded) {
        int N = (int)sm->baseVertices.size();
        if (N <= 0) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "empty skinned mesh");
            return 2;
        }
        std::vector<RenderVertex3DSkin> skinVerts(N);
        for (int i = 0; i < N; ++i) {
            const RenderVertex3D& v = sm->baseVertices[i];
            RenderVertex3DSkin& vs = skinVerts[i];
            vs.x  = v.x;  vs.y  = v.y;  vs.z  = v.z;
            vs.nx = v.nx; vs.ny = v.ny; vs.nz = v.nz;
            vs.u  = v.u;  vs.v  = v.v;
            vs.r  = v.r;  vs.g  = v.g;  vs.b = v.b; vs.a = v.a;
            vs.joints_packed = sm->jointIndicesPacked[i];
            vs.weights[0] = sm->weights[(size_t)i * 4 + 0];
            vs.weights[1] = sm->weights[(size_t)i * 4 + 1];
            vs.weights[2] = sm->weights[(size_t)i * 4 + 2];
            vs.weights[3] = sm->weights[(size_t)i * 4 + 3];
        }
        sm->gpuSkinnedMeshId = g_render->CreateSkinnedMesh(skinVerts.data(), N,
                                                            sm->indices.data(),
                                                            (int)sm->indices.size());
        if (!sm->gpuSkinnedMeshId) {
            // GPU 上传失败 -> fallback CPU (避免用户感知)
            return DrawSkinnedMeshCPU(L, sm, an, modelMat, matDesc);
        }
        sm->gpuMeshUploaded = true;
    }

    // 2. 准备最终的 jointMatrices (modelMat × jointMatrix[j])
    int jCnt = (int)(an->jointMatrices.size() / 16);
    if (jCnt <= 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no joint matrices");
        return 2;
    }
    if (jCnt > 64) jCnt = 64;       // 与 GL33 SKIN_MAX_JOINTS 一致

    // identity 检测: 跳过乘法
    bool isIdentity = (modelMat[0]==1 && modelMat[5]==1 && modelMat[10]==1 && modelMat[15]==1
                      && modelMat[1]==0 && modelMat[2]==0 && modelMat[3]==0
                      && modelMat[4]==0 && modelMat[6]==0 && modelMat[7]==0
                      && modelMat[8]==0 && modelMat[9]==0 && modelMat[11]==0
                      && modelMat[12]==0 && modelMat[13]==0 && modelMat[14]==0);

    const float* jointPtr;
    std::vector<float> finalJoints;
    if (isIdentity) {
        jointPtr = an->jointMatrices.data();
    } else {
        finalJoints.resize((size_t)jCnt * 16);
        Mat4 model;
        std::memcpy(model.m, modelMat, sizeof(model.m));
        for (int j = 0; j < jCnt; ++j) {
            Mat4 J;
            std::memcpy(J.m, &an->jointMatrices[j * 16], sizeof(J.m));
            Mat4 R = model * J;     // result = modelMat × jointMat
            std::memcpy(&finalJoints[j * 16], R.m, sizeof(R.m));
        }
        jointPtr = finalJoints.data();
    }

    // 3. 调用 backend 渲染
    g_render->DrawSkinnedMeshMaterial(sm->gpuSkinnedMeshId, matDesc, jointPtr, jCnt);
    lua_pushboolean(L, 1);
    return 1;
}

// Light.Animation.DrawSkinnedMesh(mesh, animator, transform_mat4_or_nil, material_or_nil)
//   - mesh:           SkinnedMesh userdata (必需)
//   - animator:       Animator userdata (必需; 提供 jointMatrices)
//   - transform_mat4: 16-element table (列主序) 或 nil (单位矩阵)
//   - material:       Material userdata (Phase AS.4) 或 nil (默认白底 PBR)
// 返回: ok (bool), err (string 或 nil)
//
// Phase AW: 入口分流 — 通用校验 + 解析后, 按 ShouldUseGPUSkinning() 决定走 CPU 或 GPU 路径
static int l_Anim_DrawSkinnedMesh(lua_State* L) {
    SkinnedMeshAsset* sm = CheckSkinnedMesh(L, 1);
    Animator* an = CheckAnimator(L, 2);
    if (!sm->alive || !an->alive) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "skinned mesh or animator is dead");
        return 2;
    }
    if (!sm->skeletonPtr || !sm->skeletonPtr->alive) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "skeleton is dead");
        return 2;
    }

    // 检查渲染后端
    if (!g_render) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "graphics not initialized");
        return 2;
    }
    if (!g_render->Supports3D()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "render backend does not support 3D mesh");
        return 2;
    }

    // 获取 transform mat4 (可选)
    float modelMat[16];
    {
        bool hasTransform = (lua_type(L, 3) == LUA_TTABLE);
        if (hasTransform) {
            if (!ReadMat4FromTable(L, 3, modelMat)) {
                lua_pushboolean(L, 0);
                lua_pushstring(L, "transform must be a 16-element table or nil");
                return 2;
            }
        } else {
            // 单位矩阵
            std::memset(modelMat, 0, sizeof(modelMat));
            modelMat[0] = modelMat[5] = modelMat[10] = modelMat[15] = 1.0f;
        }
    }

    // 获取 Material (可选)
    const MaterialDesc* matDesc = nullptr;
    MaterialDesc fallbackMat = {};
    if (lua_type(L, 4) == LUA_TUSERDATA) {
        matDesc = CheckMaterialUserdata(L, 4);
        if (!matDesc) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "material must be a Material userdata or nil");
            return 2;
        }
    } else {
        // 默认 PBR 白底
        fallbackMat.mode = 1;     // PBR
        fallbackMat.color[0] = fallbackMat.color[1] = fallbackMat.color[2] = fallbackMat.color[3] = 1.0f;
        fallbackMat.metallic = 0.0f;
        fallbackMat.roughness = 0.8f;
        fallbackMat.normalScale = 1.0f;
        fallbackMat.occlusionStrength = 1.0f;
        fallbackMat.alphaMode = 0;
        fallbackMat.alphaCutoff = 0.5f;
        matDesc = &fallbackMat;
    }

    // 确保 animator 有最新关节矩阵 (若用户未调 Update, 自动用 bind pose 计算)
    if (an->jointMatrices.empty()) {
        ComputeJointMatrices(an->skeletonPtr, an->activeClip, an->currentTime, an->jointMatrices);
    }

    // Phase AW: 入口分流
    if (ShouldUseGPUSkinning()) {
        return DrawSkinnedMeshGPU(L, sm, an, modelMat, matDesc);
    }
    return DrawSkinnedMeshCPU(L, sm, an, modelMat, matDesc);
}

// ==================== Phase AW — Lua API: Set/GetSkinningMode ====================

// Anim.SetSkinningMode("auto"|"cpu"|"gpu") -> true 或 nil + err
//   仅修改 g_skinningMode; 实际生效与否由 GetSkinningMode 反映 (它会调 ShouldUseGPUSkinning)
static int l_Anim_SetSkinningMode(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        return ErrorReturn(L, "mode must be a string ('auto', 'cpu', or 'gpu')");
    }
    const char* s = lua_tostring(L, 1);
    if (!s) return ErrorReturn(L, "mode must be a non-nil string");
    if (std::strcmp(s, "auto") == 0)      g_skinningMode = SkinningMode::AUTO;
    else if (std::strcmp(s, "cpu") == 0)  g_skinningMode = SkinningMode::CPU;
    else if (std::strcmp(s, "gpu") == 0)  g_skinningMode = SkinningMode::GPU;
    else {
        return ErrorReturn(L, "mode must be 'auto', 'cpu', or 'gpu'");
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Anim.GetSkinningMode() -> "cpu" 或 "gpu" (实际生效路径)
//   注: 与用户设置值不同 — 例如设了 "gpu" 但 backend 不支持, 返回 "cpu"
static int l_Anim_GetSkinningMode(lua_State* L) {
    lua_pushstring(L, ShouldUseGPUSkinning() ? "gpu" : "cpu");
    return 1;
}

// ==================== 元表注册辅助 ====================

static void RegisterMetatable(lua_State* L, const char* mtName, const luaL_Reg* methods) {
    luaL_newmetatable(L, mtName);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");           // mt.__index = mt (实例方法)
    luaL_register(L, nullptr, methods);
    lua_pop(L, 1);
}

static const luaL_Reg kSkeletonMethods[] = {
    {"GetJointCount",         l_Skeleton_GetJointCount},
    {"GetJointName",          l_Skeleton_GetJointName},
    {"FindJoint",             l_Skeleton_FindJoint},
    {"GetJointParent",        l_Skeleton_GetJointParent},
    {"GetRootJoint",          l_Skeleton_GetRootJoint},
    {"GetBindLocalTRS",       l_Skeleton_GetBindLocalTRS},
    {"GetInverseBindMatrix",  l_Skeleton_GetInverseBindMatrix},
    // Phase AV.x: procedural setter
    {"SetJointName",          l_Skeleton_SetJointName},
    {"SetJointParent",        l_Skeleton_SetJointParent},
    {"SetBindLocalTRS",       l_Skeleton_SetBindLocalTRS},
    {"SetInverseBindMatrix",  l_Skeleton_SetInverseBindMatrix},
    {"IsAlive",               l_Skeleton_IsAlive},
    {"Delete",                l_Skeleton_Delete},
    {"__gc",                  l_Skeleton_GC},
    {"__tostring",            l_Skeleton_ToString},
    {nullptr, nullptr},
};

static const luaL_Reg kClipMethods[] = {
    {"GetName",          l_Clip_GetName},
    {"GetDuration",      l_Clip_GetDuration},
    {"GetSamplerCount",  l_Clip_GetSamplerCount},
    {"GetSamplerInfo",   l_Clip_GetSamplerInfo},
    {"Sample",           l_Clip_Sample},
    // Phase AV.x: procedural setter
    {"SetDuration",      l_Clip_SetDuration},
    {"AddSampler",       l_Clip_AddSampler},
    {"IsAlive",          l_Clip_IsAlive},
    {"Delete",           l_Clip_Delete},
    {"__gc",             l_Clip_GC},
    {"__tostring",       l_Clip_ToString},
    {nullptr, nullptr},
};

static const luaL_Reg kAnimatorMethods[] = {
    {"Update",                l_Animator_Update},
    {"GetSkeleton",           l_Animator_GetSkeleton},
    {"GetCurrentTime",        l_Animator_GetCurrentTime},
    {"SetCurrentTime",        l_Animator_SetCurrentTime},
    {"GetPrevTime",           l_Animator_GetPrevTime},          // Step 4
    {"SetSpeed",              l_Animator_SetSpeed},
    {"Pause",                 l_Animator_Pause},
    {"Resume",                l_Animator_Resume},
    {"IsPaused",              l_Animator_IsPaused},
    {"GetJointMatrices",      l_Animator_GetJointMatrices},
    // Step 2: 状态机基础
    {"AddState",              l_Animator_AddState},
    {"Play",                  l_Animator_Play},
    {"Stop",                  l_Animator_Stop},
    {"GetCurrentState",       l_Animator_GetCurrentState},
    {"GetStateCount",         l_Animator_GetStateCount},
    {"HasState",              l_Animator_HasState},
    {"SetLooping",            l_Animator_SetLooping},
    {"IsLooping",             l_Animator_IsLooping},
    // Step 4: Transition / Crossfade / Event / Param
    {"AddTransition",         l_Animator_AddTransition},
    {"ClearTransitions",      l_Animator_ClearTransitions},
    {"GetTransitionCount",    l_Animator_GetTransitionCount},
    {"Crossfade",             l_Animator_Crossfade},
    {"IsCrossfading",         l_Animator_IsCrossfading},
    {"GetCrossfadeProgress",  l_Animator_GetCrossfadeProgress},
    {"GetCrossfadeTarget",    l_Animator_GetCrossfadeTarget},
    {"AddEvent",              l_Animator_AddEvent},
    {"ClearEvents",           l_Animator_ClearEvents},
    {"GetEventCount",         l_Animator_GetEventCount},
    {"SetParam",              l_Animator_SetParam},
    {"GetParam",              l_Animator_GetParam},
    {"HasParam",              l_Animator_HasParam},
    // Phase AV.x: 读取/内省接口
    {"GetClip",               l_Animator_GetClip},
    {"GetActiveClip",         l_Animator_GetActiveClip},
    {"ListStates",            l_Animator_ListStates},
    {"GetTransitionInfo",     l_Animator_GetTransitionInfo},
    {"GetEventInfo",          l_Animator_GetEventInfo},
    {"ListParams",            l_Animator_ListParams},
    // 生命周期
    {"IsAlive",               l_Animator_IsAlive},
    {"Delete",                l_Animator_Delete},
    {"__gc",                  l_Animator_GC},
    {"__tostring",            l_Animator_ToString},
    {nullptr, nullptr},
};

// Step 3: SkinnedMesh 元表方法
static const luaL_Reg kSkinnedMeshMethods[] = {
    {"GetVertexCount",  l_SkinnedMesh_GetVertexCount},
    {"GetIndexCount",   l_SkinnedMesh_GetIndexCount},
    {"GetSkeleton",     l_SkinnedMesh_GetSkeleton},
    {"IsAlive",         l_SkinnedMesh_IsAlive},
    {"Delete",          l_SkinnedMesh_Delete},
    {"__gc",            l_SkinnedMesh_GC},
    {"__tostring",      l_SkinnedMesh_ToString},
    {nullptr, nullptr},
};

static const luaL_Reg kAnimationModule[] = {
    {"LoadSkinnedGLTF",    l_Anim_LoadSkinnedGLTF},
    {"NewAnimator",        l_Anim_NewAnimator},
    {"DrawSkinnedMesh",    l_Anim_DrawSkinnedMesh},     // Step 3
    // Phase AV.x: procedural
    {"NewEmptySkeleton",   l_Anim_NewEmptySkeleton},
    {"NewEmptyClip",       l_Anim_NewEmptyClip},
    // Phase AW: GPU Skinning mode
    {"SetSkinningMode",    l_Anim_SetSkinningMode},
    {"GetSkinningMode",    l_Anim_GetSkinningMode},
    {nullptr, nullptr},
};

// ==================== Lua 模块入口 (5 个 luaopen, 全部 LIGHT_API) ====================

extern "C" LIGHT_API int luaopen_Light_Animation(lua_State* L) {
    // 注册四个元表 (即使本模块已加载多次也无碍, luaL_newmetatable 是幂等的)
    RegisterMetatable(L, SKELETON_MT,     kSkeletonMethods);
    RegisterMetatable(L, CLIP_MT,         kClipMethods);
    RegisterMetatable(L, ANIMATOR_MT,     kAnimatorMethods);
    RegisterMetatable(L, SKINNED_MESH_MT, kSkinnedMeshMethods);    // Step 3

    // 注意: 不能用 luaL_register(L, "Light.Animation", ...) 因为 ChocoLight 的 Light
    // 是 OOP 框架特殊全局, 其 __index/__newindex 拦截会触发 'object is a static module'.
    // 与 Phase AU light_physics3d.cpp 的 luaopen 保持一致: lua_newtable + luaL_setfuncs.
    lua_newtable(L);
    luaL_setfuncs(L, kAnimationModule, 0);
    return 1;
}

// 子模块 luaopen 仅用于元表注册 (不返回独立模块表), 兼容 g_lightModules 加载顺序
extern "C" LIGHT_API int luaopen_Light_Animation_Skeleton(lua_State* L) {
    RegisterMetatable(L, SKELETON_MT, kSkeletonMethods);
    lua_newtable(L);    // 返回空 stub 表 (Lua 端 require 不会崩)
    return 1;
}

extern "C" LIGHT_API int luaopen_Light_Animation_Clip(lua_State* L) {
    RegisterMetatable(L, CLIP_MT, kClipMethods);
    lua_newtable(L);
    return 1;
}

extern "C" LIGHT_API int luaopen_Light_Animation_Animator(lua_State* L) {
    RegisterMetatable(L, ANIMATOR_MT, kAnimatorMethods);
    lua_newtable(L);
    return 1;
}

// Step 3: SkinnedMesh 子模块入口
extern "C" LIGHT_API int luaopen_Light_Animation_SkinnedMesh(lua_State* L) {
    RegisterMetatable(L, SKINNED_MESH_MT, kSkinnedMeshMethods);
    lua_newtable(L);
    return 1;
}
