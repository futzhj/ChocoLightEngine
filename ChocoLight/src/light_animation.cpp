/**
 * @file light_animation.cpp
 * @brief Light.Animation — 3D 骨骼动画 + 动画状态机 (Phase AV Step 1)
 *
 * 模块布局:
 *   Light.Animation              顶层 (LoadSkinnedGLTF / NewAnimator)
 *   Light.Animation.Skeleton     骨骼层级 (静态, 关节树 + 反向绑定矩阵)
 *   Light.Animation.Clip         动画时间轴 (静态, 一组 sampler)
 *   Light.Animation.Animator     运行时 (Step 1 占位, Step 2/4 完整化)
 *
 * Step 1 范围: Skeleton + Clip 数据结构 + Lua 绑定 + Animator 占位
 *   - 不实现 sampler 评估 (Step 2)
 *   - 不实现关节变换树前向计算 (Step 2)
 *   - 不实现 SkinnedMesh 渲染 (Step 3)
 *   - 不实现状态机 (Step 4)
 *
 * 见 docs/Phase AV 骨骼动画/{ALIGNMENT,CONSENSUS,DESIGN,TASK}_PhaseAV.md
 */

#include "light.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>          // Step 2: sqrt / sin / cos / acos for QuatSlerp + QuatNormalize
#include <cstdint>        // Step 2: uint8_t / uint32_t (computed flags)

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "cgltf.h"   // glTF 2.0 解析 (single-header), 已在 third_party
}

// ==================== 常量 ====================

static constexpr int MAX_JOINTS         = 64;        // 关节数硬上限 (uniform 16 KB / Mat4×64)
static constexpr int FLOATS_PER_MAT4    = 16;
static constexpr int FLOATS_T           = 3;         // translation
static constexpr int FLOATS_R           = 4;         // rotation (quat wxyz)
static constexpr int FLOATS_S           = 3;         // scale

// userdata 元表名 (与 Lua 模块名对应)
static const char* SKELETON_MT  = "Light.Animation.Skeleton";
static const char* CLIP_MT      = "Light.Animation.Clip";
static const char* ANIMATOR_MT  = "Light.Animation.Animator";

// ==================== 数据结构 ====================

namespace LT { namespace Anim {

enum class InterpMode : uint8_t {
    LINEAR      = 0,
    STEP        = 1,
    CUBICSPLINE = 2,
};

enum class ChannelTarget : uint8_t {
    TRANSLATION = 0,
    ROTATION    = 1,
    SCALE       = 2,
    UNSUPPORTED = 255,    // morph weights 等本 Phase 不支持
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
    int            jointIndex = -1;       // 索引到 Skeleton::joints
    ChannelTarget  target     = ChannelTarget::UNSUPPORTED;
    InterpMode     mode       = InterpMode::LINEAR;
    int            components = 3;        // 3 (T/S) 或 4 (R)
    std::vector<float> times;             // keyframe 时间, 升序
    std::vector<float> values;            // 对应数据, CUBICSPLINE 时每点 3*components 元素
};

struct Skeleton {
    std::vector<JointNode> joints;
    // 反向绑定矩阵 (cgltf_skin::inverse_bind_matrices), 每关节 16 floats, 列主序
    std::vector<float>     inverseBindMatrices;
    int                    rootJoint = -1;
    std::unordered_map<std::string, int> nameToIndex;
    bool                   alive = true;
};

struct AnimationClip {
    std::string          name;
    float                duration = 0.0f;     // 自动取所有 sampler max time
    std::vector<Sampler> samplers;
    bool                 alive = true;
};

// Animator (Step 2 完整化: sampler eval + 关节变换树 + 状态机基础)
struct Animator {
    Skeleton*  skeletonPtr  = nullptr;
    int        skeletonRef  = LUA_NOREF;     // 防 Skeleton GC 的强引用

    // 时间 / 速率 / 暂停
    bool       paused      = false;
    float      speed       = 1.0f;
    float      currentTime = 0.0f;

    // Step 2: 状态机基础 (单状态切换, 无 Transition; Step 4 引入 crossfade)
    std::unordered_map<std::string, AnimationClip*> states;       // name → clip 指针
    std::unordered_map<std::string, int>            stateRefs;    // name → registry ref (防 clip GC)
    std::string                                     currentState; // 空串 = 未播放
    AnimationClip*                                  activeClip = nullptr;
    bool                                            looping    = true;

    // Step 2: 关节矩阵缓存 (Update 时填充, GetJointMatrices 直接读)
    // 布局: 每关节 16 floats (列主序 mat4), 共 N×16 floats
    std::vector<float> jointMatrices;

    bool alive = true;
};

} } // namespace LT::Anim

using LT::Anim::Skeleton;
using LT::Anim::AnimationClip;
using LT::Anim::Sampler;
using LT::Anim::JointNode;
using LT::Anim::Animator;
using LT::Anim::InterpMode;
using LT::Anim::ChannelTarget;

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
        float raw[4];
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
        float a[4], b[4];
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
    float v0[4], o0[4], v1[4], i1[4];
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

    float raw[4];
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
// 对每个关节: 在 clip 中找匹配 sampler 求 local TRS;
// 然后 DFS world[i] = world[parent] * local[i];
// 最后 skinning[i] = world[i] * inverseBind[i].
// 输出 outMatrices 大小 = jointCount * 16, 列主序.
static void ComputeJointMatrices(Skeleton* sk, AnimationClip* clip, float t,
                                  std::vector<float>& outMatrices) {
    int N = (int)sk->joints.size();
    outMatrices.assign((size_t)N * 16, 0.0f);
    if (N == 0) return;

    // 临时缓冲: 每关节 local mat4 + world mat4
    std::vector<float> localMats(N * 16, 0.0f);
    std::vector<float> worldMats(N * 16, 0.0f);
    std::vector<uint8_t> computed(N, 0);

    // 构造每关节的 local TRS → mat4 (sampler 覆盖优先, 否则 bind pose)
    for (int i = 0; i < N; ++i) {
        const JointNode& jn = sk->joints[i];
        float trans[3] = { jn.local_t[0], jn.local_t[1], jn.local_t[2] };
        float rot  [4] = { jn.local_r[0], jn.local_r[1], jn.local_r[2], jn.local_r[3] };  // wxyz
        float scl  [3] = { jn.local_s[0], jn.local_s[1], jn.local_s[2] };

        if (clip) {
            for (const Sampler& s : clip->samplers) {
                if (s.jointIndex != i) continue;
                if (s.target == ChannelTarget::TRANSLATION) {
                    EvaluateSampler(s, t, trans, 3);
                } else if (s.target == ChannelTarget::ROTATION) {
                    EvaluateSampler(s, t, rot, 4);
                } else if (s.target == ChannelTarget::SCALE) {
                    EvaluateSampler(s, t, scl, 3);
                }
            }
        }
        TRSToMat4(trans, rot, scl, &localMats[i * 16]);
    }

    // DFS 计算 world 矩阵 (拓扑顺序保证: 父先于子)
    // 用迭代式: 关节按 0..N-1 多次扫描直到全部 computed (典型骨骼<64, 一两次扫描就完)
    // 由于 cgltf 输出的关节列表通常已是 BFS/DFS 顺序, 也就是父索引<子索引,
    // 一次扫描通常足够, 但为安全起见循环扫描.
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

} // anonymous namespace

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

// ==================== cgltf → AnimationClip 提取 ====================

// 把 cgltf_animation_path 转为我们的 ChannelTarget
static ChannelTarget ConvertChannelTarget(cgltf_animation_path_type p) {
    switch (p) {
        case cgltf_animation_path_type_translation: return ChannelTarget::TRANSLATION;
        case cgltf_animation_path_type_rotation:    return ChannelTarget::ROTATION;
        case cgltf_animation_path_type_scale:       return ChannelTarget::SCALE;
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

// 从一个 cgltf_animation 构造 AnimationClip (skin 用于映射 channel 目标 node → joint 索引)
static AnimationClip* BuildClip(const cgltf_animation* anim, const cgltf_skin* skin) {
    AnimationClip* clip = new AnimationClip();
    clip->name = anim->name ? anim->name : "(unnamed)";

    for (cgltf_size c = 0; c < anim->channels_count; ++c) {
        const cgltf_animation_channel& ch = anim->channels[c];
        if (!ch.target_node || !ch.sampler) continue;

        // 找 channel.target_node 对应的关节
        int jointIdx = FindJointInSkin(skin, ch.target_node);
        if (jointIdx < 0) {
            // 该 channel 作用于 skin 之外的 node, Step 1 跳过
            continue;
        }

        ChannelTarget tgt = ConvertChannelTarget(ch.target_path);
        if (tgt == ChannelTarget::UNSUPPORTED) continue;

        const cgltf_animation_sampler* gs = ch.sampler;
        if (!gs->input || !gs->output) continue;

        Sampler s;
        s.jointIndex = jointIdx;
        s.target     = tgt;
        s.mode       = ConvertInterpolation(gs->interpolation);
        s.components = (tgt == ChannelTarget::ROTATION) ? FLOATS_R : FLOATS_T;

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
        AnimationClip* c = BuildClip(&data->animations[i], skin);
        clips.push_back(c);
    }

    cgltf_free(data);    // skin/animation 指针之后不可访问 (我们已经拷贝完所需数据)

    // 输出 table
    lua_newtable(L);

    // pack.skeleton
    PushSkeletonUserdata(L, sk);
    lua_setfield(L, -2, "skeleton");

    // pack.clips = { [name] = clip, ... } + 整数索引数组形式 (兼容 ipairs)
    lua_newtable(L);
    for (size_t i = 0; i < clips.size(); ++i) {
        // map by name
        PushClipUserdata(L, clips[i]);
        lua_setfield(L, -2, clips[i]->name.c_str());
        // 也按索引 (1-based) 复用 - 推同样的 userdata 引用 (但 userdata 是单一所有权,
        // 多次 push 同一指针会让多个 __gc 都 delete, 导致 double free)
        // 故索引数组只存 name 字符串便于遍历:
    }
    lua_setfield(L, -2, "clips");

    // pack.clipNames = { name1, name2, ... } 数组顺序
    lua_newtable(L);
    for (size_t i = 0; i < clips.size(); ++i) {
        lua_pushstring(L, clips[i]->name.c_str());
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "clipNames");

    // pack.mesh: Step 3 填充 (Step 1 用 nil)
    lua_pushnil(L);
    lua_setfield(L, -2, "mesh");

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

// Step 2: 推进时间 + 处理 looping + 重新计算关节矩阵
static int l_Animator_Update(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    float dt = (float)luaL_checknumber(L, 2);

    if (!an->paused) {
        an->currentTime += dt * an->speed;
    }

    // 处理 looping / clamping
    if (an->activeClip && an->activeClip->duration > 1e-6f) {
        float dur = an->activeClip->duration;
        if (an->looping) {
            // wrap to [0, duration); 处理负 speed (回退播放) 也要正确
            float t = an->currentTime;
            t = std::fmod(t, dur);
            if (t < 0) t += dur;
            an->currentTime = t;
        } else {
            // 不循环: clamp 到 [0, duration]; speed<0 时 clamp 到 [0, duration]
            if (an->currentTime < 0)   an->currentTime = 0;
            if (an->currentTime > dur) an->currentTime = dur;
        }
    }

    // 重新计算关节矩阵 (即使 paused 也要算一次, 因为 SetCurrentTime 后用户可能手动 Update(0))
    if (an->skeletonPtr && an->skeletonPtr->alive) {
        ComputeJointMatrices(an->skeletonPtr, an->activeClip, an->currentTime, an->jointMatrices);
    }
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
    {"IsAlive",          l_Clip_IsAlive},
    {"Delete",           l_Clip_Delete},
    {"__gc",             l_Clip_GC},
    {"__tostring",       l_Clip_ToString},
    {nullptr, nullptr},
};

static const luaL_Reg kAnimatorMethods[] = {
    {"Update",            l_Animator_Update},
    {"GetSkeleton",       l_Animator_GetSkeleton},
    {"GetCurrentTime",    l_Animator_GetCurrentTime},
    {"SetCurrentTime",    l_Animator_SetCurrentTime},
    {"SetSpeed",          l_Animator_SetSpeed},
    {"Pause",             l_Animator_Pause},
    {"Resume",            l_Animator_Resume},
    {"IsPaused",          l_Animator_IsPaused},
    {"GetJointMatrices",  l_Animator_GetJointMatrices},
    // Step 2: 状态机基础
    {"AddState",          l_Animator_AddState},
    {"Play",              l_Animator_Play},
    {"Stop",              l_Animator_Stop},
    {"GetCurrentState",   l_Animator_GetCurrentState},
    {"GetStateCount",     l_Animator_GetStateCount},
    {"HasState",          l_Animator_HasState},
    {"SetLooping",        l_Animator_SetLooping},
    {"IsLooping",         l_Animator_IsLooping},
    // 生命周期
    {"IsAlive",           l_Animator_IsAlive},
    {"Delete",            l_Animator_Delete},
    {"__gc",              l_Animator_GC},
    {"__tostring",        l_Animator_ToString},
    {nullptr, nullptr},
};

static const luaL_Reg kAnimationModule[] = {
    {"LoadSkinnedGLTF", l_Anim_LoadSkinnedGLTF},
    {"NewAnimator",     l_Anim_NewAnimator},
    {nullptr, nullptr},
};

// ==================== Lua 模块入口 (5 个 luaopen, 全部 LIGHT_API) ====================

extern "C" LIGHT_API int luaopen_Light_Animation(lua_State* L) {
    // 注册三个元表 (即使本模块已加载多次也无碍, luaL_newmetatable 是幂等的)
    RegisterMetatable(L, SKELETON_MT, kSkeletonMethods);
    RegisterMetatable(L, CLIP_MT,     kClipMethods);
    RegisterMetatable(L, ANIMATOR_MT, kAnimatorMethods);

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
