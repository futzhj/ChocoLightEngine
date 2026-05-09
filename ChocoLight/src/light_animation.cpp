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

// Animator 在 Step 1 仅占位 (Step 2 填充实现)
struct Animator {
    Skeleton*  skeletonPtr  = nullptr;
    int        skeletonRef  = LUA_NOREF;     // 防 Skeleton GC 的强引用

    // Step 1 仅保留接口字段 (Step 2/4 填充语义)
    bool       paused      = false;
    float      speed       = 1.0f;
    float      currentTime = 0.0f;

    bool       alive = true;
};

} } // namespace LT::Anim

using LT::Anim::Skeleton;
using LT::Anim::AnimationClip;
using LT::Anim::Sampler;
using LT::Anim::JointNode;
using LT::Anim::Animator;
using LT::Anim::InterpMode;
using LT::Anim::ChannelTarget;

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

static int l_Animator_Update(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    float dt = (float)luaL_checknumber(L, 2);
    if (!an->paused) {
        an->currentTime += dt * an->speed;
    }
    // Step 1: 仅推进时间; Step 2 添加状态机 + 关节矩阵更新
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

// Step 1 占位: 返回空 table (Step 2 实现真正的关节矩阵计算)
static int l_Animator_GetJointMatrices(lua_State* L) {
    Animator* an = CheckAnimator(L, 1);
    (void)an;
    lua_newtable(L);
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
