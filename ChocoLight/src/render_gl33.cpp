/**
 * @file render_gl33.cpp
 * @brief OpenGL 3.3 Core Profile 渲染后端 (GL33Backend)
 * @note VAO/VBO + shader pipeline, 自管理矩阵栈
 */

#include "render_backend.h"
#include "light.h"
#include "light_lighting2d.h"  // Phase E.1.5 — Lighting2D::State / UploadToShader

// GL 头文件: GLES3 (Web/移动) vs glad (桌面)
#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#elif defined(__ANDROID__)
#include <GLES3/gl3.h>
#elif defined(CHOCO_PLATFORM_IOS)
#include <OpenGLES/ES3/gl.h>
#else
#include <glad/gl.h>
#endif

#include "platform_window.h"
#include <vector>
#include <cstring>
#include <cmath>          // Phase E.8.1 — sqrtf for SSAO noise LCG (Linux gcc 严格要求)
#include <unordered_map>

// ==================== 内嵌 Shader 源码 ====================

// GLES3 / GL33 共用 Shader, 仅版本声明不同
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
static const char* VS_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

static const char* FS_SOURCE = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
in vec4 vColor;
uniform sampler2D uTexture;
uniform int uUseTexture;
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: 2D batch 默认 朝相机
void main() {
    if (uUseTexture == 1) {
        FragColor = vColor * texture(uTexture, vTexCoord);
    } else {
        FragColor = vColor;
    }
    FragNormal = vec2(0.5, 0.5);   // 代表 view-space (0,0,1)
}
)";
#else
static const char* VS_SOURCE = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

static const char* FS_SOURCE = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;

uniform sampler2D uTexture;
uniform int uUseTexture;

layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: 2D batch 默认 朝相机

void main() {
    if (uUseTexture == 1) {
        FragColor = vColor * texture(uTexture, vTexCoord);
    } else {
        FragColor = vColor;
    }
    FragNormal = vec2(0.5, 0.5);   // view-space (0,0,1)
}
)";
#endif

// ==================== Phase AS.4 — Unlit + PBR 双 3D shader ====================
//
// VS3D 为 Unlit 和 PBR 共用 (输出 pos/normal/uv/color/worldPos).
// FS_UNLIT  — 仅 baseColor*texture*vColor + emissive + alphaMode 处理
// FS_PBR    — Lambert diffuse + Schlick-Fresnel + GGX D⋅G specular + 1 dir + 4 point + emissive + AO
//
// 法线贴图用 derivatives (dFdx/dFdy) 计算 TBN, 不需要 Tangent 顶点属性.
// 多光源采用固定 4 个点光数组 uniform (MAX_POINT_LIGHTS=4).

#define MAX_POINT_LIGHTS 4

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)

// ---- VS3D (GLES 3.0) ----
static const char* VS3D_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uPrevViewProj;
uniform mat4 uCurViewProj;   // Phase F.0 — unjittered cur viewProj (vCurClip 用, 保证 velocity 不含 TAA jitter)
uniform mat4 uPrevModel;
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
out vec4 vCurClip;
out vec4 vPrevClip;
out vec4 vPrevClipCameraOnly;  // Phase E.16: prevVP * curModel * pos (camera-only velocity)
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormalW = mat3(uModel) * aNormal;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vTexCoord = aUV;
    vColor = aColor;
    vCurClip = uCurViewProj * (uModel * vec4(aPos, 1.0));   // Phase F.0 — unjittered (避免 gl_Position 含 TAA jitter)
    vPrevClip = uPrevViewProj * (uPrevModel * vec4(aPos, 1.0));
    // Phase E.16: 假设物体未动 (uModel = curModel)，仅留相机运动到 prevVP
    vPrevClipCameraOnly = uPrevViewProj * (uModel * vec4(aPos, 1.0));
}
)";

// ---- VS3D SKIN (GLES 3.0, Phase AW GPU Skinning) ----
// 与 VS3D 等价但加上骨骼蒙皮: skinnedPos = sum(weight[i] * jointMats[joints[i]]) * aPos
static const char* VS3D_SKIN_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aColor;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4  aWeights;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uPrevViewProj;
uniform mat4 uCurViewProj;   // Phase F.0 — unjittered cur viewProj (vCurClip 用, 保证 velocity 不含 TAA jitter)
uniform mat4 uPrevModel;
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
layout(std140) uniform PrevJointBlock {
    mat4 uPrevJointMats[64];
};
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
out vec4 vCurClip;
out vec4 vPrevClip;
out vec4 vPrevClipCameraOnly;  // Phase E.16: prevVP * curModel * pos (camera-only velocity)
void main() {
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    mat4 prevBlend = aWeights.x * uPrevJointMats[aJoints.x]
                   + aWeights.y * uPrevJointMats[aJoints.y]
                   + aWeights.z * uPrevJointMats[aJoints.z]
                   + aWeights.w * uPrevJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(aPos, 1.0);
    vec4 prevSkinnedPos = prevBlend * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * aNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
    vCurClip    = uCurViewProj * (uModel * skinnedPos);   // Phase F.0 — unjittered (避免 gl_Position 含 TAA jitter)
    vPrevClip   = uPrevViewProj * (uPrevModel * prevSkinnedPos);
    // Phase E.16: 用当前帧 skinned·morph 后位置 (物体未动假设)，仅 prevVP 取上一帧
    vPrevClipCameraOnly = uPrevViewProj * (uModel * skinnedPos);
}
)";

// ---- VS3D SKIN MORPH (GLES 3.0, Phase AX) ----
// 顺序: base + Σ(weight[i] × delta_i[gl_VertexID]) -> skinning
// morph delta 数据存在 RGB32F 2D texture 中:
//   width  = vCount       (顶点维度, 用 gl_VertexID 索引)
//   height = morphCount   (target 维度, 用 i 索引)
// 此设计避开 uniform array 大小限制 (vCount * N 通常远超 uniform 上限)
static const char* VS3D_SKIN_MORPH_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aColor;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4  aWeights;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uPrevViewProj;
uniform mat4 uCurViewProj;   // Phase F.0 — unjittered cur viewProj (vCurClip 用, 保证 velocity 不含 TAA jitter)
uniform mat4 uPrevModel;
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
layout(std140) uniform PrevJointBlock {
    mat4 uPrevJointMats[64];
};
const int MORPH_MAX = 8;
uniform float     uMorphWeights[MORPH_MAX];
uniform float     uPrevMorphWeights[MORPH_MAX];
uniform int       uMorphCount;
uniform int       uHasMorphNormal;
uniform sampler2D uMorphPosDelta;
uniform sampler2D uMorphNrmDelta;
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
out vec4 vCurClip;
out vec4 vPrevClip;
out vec4 vPrevClipCameraOnly;  // Phase E.16: prevVP * curModel * pos (camera-only velocity)
void main() {
    // 1. Morph: base + Σ (weight × delta)
    vec3 morphedPos    = aPos;
    vec3 prevMorphedPos = aPos;
    vec3 morphedNormal = aNormal;
    for (int i = 0; i < MORPH_MAX; ++i) {
        if (i >= uMorphCount) break;
        float w = uMorphWeights[i];
        float pw = uPrevMorphWeights[i];
        vec3 dPos = texelFetch(uMorphPosDelta, ivec2(gl_VertexID, i), 0).xyz;
        morphedPos += w * dPos;
        prevMorphedPos += pw * dPos;
        if (uHasMorphNormal == 1) {
            morphedNormal += w * texelFetch(uMorphNrmDelta, ivec2(gl_VertexID, i), 0).xyz;
        }
    }
    // 2. Skin: 4 joint blend
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    mat4 prevBlend = aWeights.x * uPrevJointMats[aJoints.x]
                   + aWeights.y * uPrevJointMats[aJoints.y]
                   + aWeights.z * uPrevJointMats[aJoints.z]
                   + aWeights.w * uPrevJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(morphedPos, 1.0);
    vec4 prevSkinnedPos = prevBlend * vec4(prevMorphedPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * morphedNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
    vCurClip    = uCurViewProj * (uModel * skinnedPos);   // Phase F.0 — unjittered (避免 gl_Position 含 TAA jitter)
    vPrevClip   = uPrevViewProj * (uPrevModel * prevSkinnedPos);
    // Phase E.16: 用当前帧 skinned·morph 后位置 (物体未动假设)，仅 prevVP 取上一帧
    vPrevClipCameraOnly = uPrevViewProj * (uModel * skinnedPos);
}
)";

// ---- FS Unlit (GLES 3.0) ----
static const char* FS_UNLIT_SOURCE = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
in vec4 vColor;
in vec3 vNormalW;                // Phase E.8.x: 供 MRT normal 输出
in vec4 vCurClip;
in vec4 vPrevClip;
in vec4 vPrevClipCameraOnly;     // Phase E.16: camera-only velocity 另一路 varying
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform sampler2D uTexBaseColor;
uniform sampler2D uTexEmissive;
uniform int uHasBaseColorTex;
uniform int uHasEmissiveTex;
uniform int uAlphaMode;       // 0=opaque, 1=blend, 2=mask
uniform float uAlphaCutoff;
uniform mat3 uViewMat3;          // Phase E.8.x: world->view 3x3
uniform int uHasVelocityHistory;
// Phase E.14 — velocity 编码双格式支持
uniform int   uVelocityFormat;     // 0 = RG16F (raw); 1 = RG8 (encoded with uVelocityScale)
uniform float uVelocityScale;      // RG8 模式下的编码尺度; 默认 0.25
// Phase F.1.1 — Mipmap LOD bias (TAAU 启用时偏负 ~ -1 让纹理更锐); 0.0 = 默认零回归
uniform float uMipBias;
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: view-space normal MRT
layout(location=2) out vec2 FragVelocity;
layout(location=3) out vec2 FragCameraVelocity;   // Phase E.16: camera-only velocity (slot 3)
void main() {
    vec4 base = uColor * vColor;
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord, uMipBias);
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord, uMipBias).rgb;
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(base.rgb + emissive, outAlpha);
    // Phase E.8.x: encode view-space normal 到 MRT slot 1
    vec3 nView = normalize(uViewMat3 * vNormalW);
    FragNormal = nView.xy * 0.5 + 0.5;
    if (uHasVelocityHistory == 1) {
        vec2 curUV = (vCurClip.xy / max(vCurClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 prevUV = (vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 raw = curUV - prevUV;
        // Phase E.16 — camera-only velocity (prevVP × curModel × pos)
        vec2 prevUVCam = (vPrevClipCameraOnly.xy / max(vPrevClipCameraOnly.w, 1e-6)) * 0.5 + 0.5;
        vec2 rawCam    = curUV - prevUVCam;
        // Phase E.14 — 按 velocity format 选编码路径
        //   RG16F 直接存 UV delta; RG8 用 bias/scale 压缩到 [0, 1] UNORM
        if (uVelocityFormat == 1) {
            FragVelocity       = clamp(raw    / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
            FragCameraVelocity = clamp(rawCam / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
        } else {
            FragVelocity       = raw;
            FragCameraVelocity = rawCam;
        }
    } else {
        // RG8 模式下 0.5 表示零速度 (UNORM 中点); RG16F 模式下零速度即 0
        FragVelocity       = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
        FragCameraVelocity = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
    }
}
)";

// ---- FS PBR (GLES 3.0, 简化 Cook-Torrance) ----
static const char* FS_PBR_SOURCE = R"(#version 300 es
precision highp float;
in vec3 vNormalW;
in vec3 vWorldPos;
in vec2 vTexCoord;
in vec4 vColor;
in vec4 vCurClip;
in vec4 vPrevClip;
in vec4 vPrevClipCameraOnly;     // Phase E.16: camera-only velocity 另一路 varying
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform float uMetallic;
uniform float uRoughness;
uniform float uNormalScale;
uniform float uOcclusionStrength;
uniform sampler2D uTexBaseColor;
uniform sampler2D uTexMetallicRoughness;
uniform sampler2D uTexNormal;
uniform sampler2D uTexEmissive;
uniform sampler2D uTexOcclusion;
uniform int uHasBaseColorTex;
uniform int uHasMetallicRoughnessTex;
uniform int uHasNormalTex;
uniform int uHasEmissiveTex;
uniform int uHasOcclusionTex;
uniform int uAlphaMode;
uniform float uAlphaCutoff;
uniform vec3 uCameraPos;
uniform int  uDirLightEnabled;
uniform vec3 uDirLightDir;
uniform vec3 uDirLightColor;
uniform vec3 uAmbient;
uniform int   uPointLightCount;
uniform vec3  uPointLightPos[4];
uniform vec3  uPointLightColor[4];
uniform float uPointLightRange[4];
uniform mat3  uViewMat3;        // Phase E.8.x: world->view 3x3 (为 G-buffer normal MRT 用)
uniform int   uHasVelocityHistory;
// Phase E.14 — velocity 编码双格式支持
uniform int   uVelocityFormat;     // 0 = RG16F (raw); 1 = RG8 (encoded with uVelocityScale)
uniform float uVelocityScale;      // RG8 模式下的编码尺度; 默认 0.25
// Phase F.1.1 — Mipmap LOD bias (TAAU 启用时偏负 ~ -1 让纹理更锐); 0.0 = 默认零回归
uniform float uMipBias;
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: view-space normal MRT (encode xy [0,1])
layout(location=2) out vec2 FragVelocity;
layout(location=3) out vec2 FragCameraVelocity;   // Phase E.16: camera-only velocity (slot 3)
const float PI = 3.14159265;

vec3 BRDF(vec3 N, vec3 V, vec3 L, vec3 baseColor, float metallic, float roughness, vec3 F0) {
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * denom * denom);
    float k = (roughness + 1.0); k = k * k / 8.0;
    float G1V = NdotV / (NdotV * (1.0 - k) + k);
    float G1L = NdotL / (NdotL * (1.0 - k) + k);
    float G = G1V * G1L;
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * baseColor / PI;
    return diff + spec;
}

void main() {
    vec4 base = uColor * vColor;
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord, uMipBias);
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessTex == 1) {
        vec3 mr = texture(uTexMetallicRoughness, vTexCoord, uMipBias).rgb;
        metallic *= mr.b;
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = normalize(vNormalW);
    if (uHasNormalTex == 1) {
        vec3 mappedN = texture(uTexNormal, vTexCoord, uMipBias).rgb * 2.0 - 1.0;
        mappedN.xy *= uNormalScale;
        // derivatives 计算 TBN
        vec3 dp1 = dFdx(vWorldPos);
        vec3 dp2 = dFdy(vWorldPos);
        vec2 duv1 = dFdx(vTexCoord);
        vec2 duv2 = dFdy(vTexCoord);
        vec3 dp2perp = cross(dp2, N);
        vec3 dp1perp = cross(N, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
        float invmax = inversesqrt(max(dot(T,T), dot(B,B)));
        mat3 TBN = mat3(T * invmax, B * invmax, N);
        N = normalize(TBN * mappedN);
    }

    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), base.rgb, metallic);
    vec3 lightSum = vec3(0.0);

    if (uDirLightEnabled == 1) {
        vec3 L = uDirLightDir;
        float NdotL = max(dot(N, L), 0.0);
        lightSum += BRDF(N, V, L, base.rgb, metallic, roughness, F0) * uDirLightColor * NdotL;
    }
    for (int i = 0; i < 4; i++) {
        if (i >= uPointLightCount) break;
        vec3 toLight = uPointLightPos[i] - vWorldPos;
        float d = length(toLight);
        if (d > uPointLightRange[i]) continue;
        vec3 L = toLight / max(d, 0.0001);
        float NdotL = max(dot(N, L), 0.0);
        float atten = pow(max(1.0 - d / uPointLightRange[i], 0.0), 2.0);
        lightSum += BRDF(N, V, L, base.rgb, metallic, roughness, F0) * uPointLightColor[i] * NdotL * atten;
    }

    float ao = 1.0;
    if (uHasOcclusionTex == 1) ao = mix(1.0, texture(uTexOcclusion, vTexCoord, uMipBias).r, uOcclusionStrength);
    vec3 ambient = uAmbient * base.rgb * ao;
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord, uMipBias).rgb;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(ambient + lightSum + emissive, outAlpha);
    // Phase E.8.x: 世界法线 -> view-space 法线, encode 到 RG16F (xy [-1,1] -> [0,1])
    vec3 nView = normalize(uViewMat3 * N);
    FragNormal = nView.xy * 0.5 + 0.5;
    if (uHasVelocityHistory == 1) {
        vec2 curUV = (vCurClip.xy / max(vCurClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 prevUV = (vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 raw = curUV - prevUV;
        // Phase E.16 — camera-only velocity (prevVP × curModel × pos)
        vec2 prevUVCam = (vPrevClipCameraOnly.xy / max(vPrevClipCameraOnly.w, 1e-6)) * 0.5 + 0.5;
        vec2 rawCam    = curUV - prevUVCam;
        // Phase E.14 — 按 velocity format 选编码路径
        //   RG16F 直接存 UV delta; RG8 用 bias/scale 压缩到 [0, 1] UNORM
        if (uVelocityFormat == 1) {
            FragVelocity       = clamp(raw    / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
            FragCameraVelocity = clamp(rawCam / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
        } else {
            FragVelocity       = raw;
            FragCameraVelocity = rawCam;
        }
    } else {
        // RG8 模式下 0.5 表示零速度 (UNORM 中点); RG16F 模式下零速度即 0
        FragVelocity       = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
        FragCameraVelocity = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
    }
}
)";

#else  // 桌面 GL 3.3 Core

// ---- VS3D (GL 3.3) ----
static const char* VS3D_SOURCE = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uPrevViewProj;
uniform mat4 uCurViewProj;   // Phase F.0 — unjittered cur viewProj (vCurClip 用, 保证 velocity 不含 TAA jitter)
uniform mat4 uPrevModel;
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
out vec4 vCurClip;
out vec4 vPrevClip;
out vec4 vPrevClipCameraOnly;  // Phase E.16: prevVP * curModel * pos (camera-only velocity)
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormalW = mat3(uModel) * aNormal;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vTexCoord = aUV;
    vColor = aColor;
    vCurClip = uCurViewProj * (uModel * vec4(aPos, 1.0));   // Phase F.0 — unjittered (避免 gl_Position 含 TAA jitter)
    vPrevClip = uPrevViewProj * (uPrevModel * vec4(aPos, 1.0));
    // Phase E.16: 假设物体未动 (uModel = curModel)，仅留相机运动到 prevVP
    vPrevClipCameraOnly = uPrevViewProj * (uModel * vec4(aPos, 1.0));
}
)";

// ---- VS3D SKIN (GL 3.3, Phase AW GPU Skinning) ----
// 与 VS3D 等价但加上骨骼蒙皮: skinnedPos = sum(weight[i] * jointMats[joints[i]]) * aPos
static const char* VS3D_SKIN_SOURCE = R"(
#version 330 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aColor;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4  aWeights;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uPrevViewProj;
uniform mat4 uCurViewProj;   // Phase F.0 — unjittered cur viewProj (vCurClip 用, 保证 velocity 不含 TAA jitter)
uniform mat4 uPrevModel;
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
layout(std140) uniform PrevJointBlock {
    mat4 uPrevJointMats[64];
};
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
out vec4 vCurClip;
out vec4 vPrevClip;
out vec4 vPrevClipCameraOnly;  // Phase E.16: prevVP * curModel * pos (camera-only velocity)
void main() {
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    mat4 prevBlend = aWeights.x * uPrevJointMats[aJoints.x]
                   + aWeights.y * uPrevJointMats[aJoints.y]
                   + aWeights.z * uPrevJointMats[aJoints.z]
                   + aWeights.w * uPrevJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(aPos, 1.0);
    vec4 prevSkinnedPos = prevBlend * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * aNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
    vCurClip    = uCurViewProj * (uModel * skinnedPos);   // Phase F.0 — unjittered (避免 gl_Position 含 TAA jitter)
    vPrevClip   = uPrevViewProj * (uPrevModel * prevSkinnedPos);
    // Phase E.16: 用当前帧 skinned·morph 后位置 (物体未动假设)，仅 prevVP 取上一帧
    vPrevClipCameraOnly = uPrevViewProj * (uModel * skinnedPos);
}
)";

// ---- VS3D SKIN MORPH (GL 3.3, Phase AX) ----
// 顺序: base + Σ(weight[i] × delta_i[gl_VertexID]) -> skinning
// morph delta 数据存在 RGB32F 2D texture 中 (width=vCount, height=morphCount)
static const char* VS3D_SKIN_MORPH_SOURCE = R"(
#version 330 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aColor;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4  aWeights;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uPrevViewProj;
uniform mat4 uCurViewProj;   // Phase F.0 — unjittered cur viewProj (vCurClip 用, 保证 velocity 不含 TAA jitter)
uniform mat4 uPrevModel;
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
layout(std140) uniform PrevJointBlock {
    mat4 uPrevJointMats[64];
};
const int MORPH_MAX = 8;
uniform float     uMorphWeights[MORPH_MAX];
uniform float     uPrevMorphWeights[MORPH_MAX];
uniform int       uMorphCount;
uniform int       uHasMorphNormal;
uniform sampler2D uMorphPosDelta;
uniform sampler2D uMorphNrmDelta;
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
out vec4 vCurClip;
out vec4 vPrevClip;
out vec4 vPrevClipCameraOnly;  // Phase E.16: prevVP * curModel * pos (camera-only velocity)
void main() {
    vec3 morphedPos    = aPos;
    vec3 prevMorphedPos = aPos;
    vec3 morphedNormal = aNormal;
    for (int i = 0; i < MORPH_MAX; ++i) {
        if (i >= uMorphCount) break;
        float w = uMorphWeights[i];
        float pw = uPrevMorphWeights[i];
        vec3 dPos = texelFetch(uMorphPosDelta, ivec2(gl_VertexID, i), 0).xyz;
        morphedPos += w * dPos;
        prevMorphedPos += pw * dPos;
        if (uHasMorphNormal == 1) {
            morphedNormal += w * texelFetch(uMorphNrmDelta, ivec2(gl_VertexID, i), 0).xyz;
        }
    }
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    mat4 prevBlend = aWeights.x * uPrevJointMats[aJoints.x]
                   + aWeights.y * uPrevJointMats[aJoints.y]
                   + aWeights.z * uPrevJointMats[aJoints.z]
                   + aWeights.w * uPrevJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(morphedPos, 1.0);
    vec4 prevSkinnedPos = prevBlend * vec4(prevMorphedPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * morphedNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
    vCurClip    = uCurViewProj * (uModel * skinnedPos);   // Phase F.0 — unjittered (避免 gl_Position 含 TAA jitter)
    vPrevClip   = uPrevViewProj * (uPrevModel * prevSkinnedPos);
    // Phase E.16: 用当前帧 skinned·morph 后位置 (物体未动假设)，仅 prevVP 取上一帧
    vPrevClipCameraOnly = uPrevViewProj * (uModel * skinnedPos);
}
)";

// ---- FS Unlit (GL 3.3) ----
static const char* FS_UNLIT_SOURCE = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
in vec3 vNormalW;                // Phase E.8.x
in vec4 vCurClip;
in vec4 vPrevClip;
in vec4 vPrevClipCameraOnly;     // Phase E.16: camera-only velocity 另一路 varying
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform sampler2D uTexBaseColor;
uniform sampler2D uTexEmissive;
uniform int uHasBaseColorTex;
uniform int uHasEmissiveTex;
uniform int uAlphaMode;
uniform float uAlphaCutoff;
uniform mat3 uViewMat3;          // Phase E.8.x: world->view 3x3
uniform int uHasVelocityHistory;
// Phase E.14 — velocity 编码双格式支持
uniform int   uVelocityFormat;     // 0 = RG16F (raw); 1 = RG8 (encoded with uVelocityScale)
uniform float uVelocityScale;      // RG8 模式下的编码尺度; 默认 0.25
// Phase F.1.1 — Mipmap LOD bias (TAAU 启用时偏负 ~ -1 让纹理更锐); 0.0 = 默认零回归
uniform float uMipBias;
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: view-space normal MRT
layout(location=2) out vec2 FragVelocity;
layout(location=3) out vec2 FragCameraVelocity;   // Phase E.16: camera-only velocity (slot 3)
void main() {
    vec4 base = uColor * vColor;
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord, uMipBias);
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord, uMipBias).rgb;
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(base.rgb + emissive, outAlpha);
    vec3 nView = normalize(uViewMat3 * vNormalW);
    FragNormal = nView.xy * 0.5 + 0.5;
    if (uHasVelocityHistory == 1) {
        vec2 curUV = (vCurClip.xy / max(vCurClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 prevUV = (vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 raw = curUV - prevUV;
        // Phase E.16 — camera-only velocity (prevVP × curModel × pos)
        vec2 prevUVCam = (vPrevClipCameraOnly.xy / max(vPrevClipCameraOnly.w, 1e-6)) * 0.5 + 0.5;
        vec2 rawCam    = curUV - prevUVCam;
        // Phase E.14 — 按 velocity format 选编码路径
        //   RG16F 直接存 UV delta; RG8 用 bias/scale 压缩到 [0, 1] UNORM
        if (uVelocityFormat == 1) {
            FragVelocity       = clamp(raw    / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
            FragCameraVelocity = clamp(rawCam / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
        } else {
            FragVelocity       = raw;
            FragCameraVelocity = rawCam;
        }
    } else {
        // RG8 模式下 0.5 表示零速度 (UNORM 中点); RG16F 模式下零速度即 0
        FragVelocity       = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
        FragCameraVelocity = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
    }
}
)";

// ---- FS PBR (GL 3.3) ----
static const char* FS_PBR_SOURCE = R"(
#version 330 core
in vec3 vNormalW;
in vec3 vWorldPos;
in vec2 vTexCoord;
in vec4 vColor;
in vec4 vCurClip;
in vec4 vPrevClip;
in vec4 vPrevClipCameraOnly;     // Phase E.16: camera-only velocity 另一路 varying
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform float uMetallic;
uniform float uRoughness;
uniform float uNormalScale;
uniform float uOcclusionStrength;
uniform sampler2D uTexBaseColor;
uniform sampler2D uTexMetallicRoughness;
uniform sampler2D uTexNormal;
uniform sampler2D uTexEmissive;
uniform sampler2D uTexOcclusion;
uniform int uHasBaseColorTex;
uniform int uHasMetallicRoughnessTex;
uniform int uHasNormalTex;
uniform int uHasEmissiveTex;
uniform int uHasOcclusionTex;
uniform int uAlphaMode;
uniform float uAlphaCutoff;
uniform vec3 uCameraPos;
uniform int  uDirLightEnabled;
uniform vec3 uDirLightDir;
uniform vec3 uDirLightColor;
uniform vec3 uAmbient;
uniform int   uPointLightCount;
uniform vec3  uPointLightPos[4];
uniform vec3  uPointLightColor[4];
uniform float uPointLightRange[4];
uniform mat3  uViewMat3;        // Phase E.8.x: world->view 3x3
uniform int   uHasVelocityHistory;
// Phase E.14 — velocity 编码双格式支持
uniform int   uVelocityFormat;     // 0 = RG16F (raw); 1 = RG8 (encoded with uVelocityScale)
uniform float uVelocityScale;      // RG8 模式下的编码尺度; 默认 0.25
// Phase F.1.1 — Mipmap LOD bias (TAAU 启用时偏负 ~ -1 让纹理更锐); 0.0 = 默认零回归
uniform float uMipBias;
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: view-space normal MRT
layout(location=2) out vec2 FragVelocity;
layout(location=3) out vec2 FragCameraVelocity;   // Phase E.16: camera-only velocity (slot 3)
const float PI = 3.14159265;

vec3 BRDF(vec3 N, vec3 V, vec3 L, vec3 baseColor, float metallic, float roughness, vec3 F0) {
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * denom * denom);
    float k = (roughness + 1.0); k = k * k / 8.0;
    float G1V = NdotV / (NdotV * (1.0 - k) + k);
    float G1L = NdotL / (NdotL * (1.0 - k) + k);
    float G = G1V * G1L;
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diff = kD * baseColor / PI;
    return diff + spec;
}

void main() {
    vec4 base = uColor * vColor;
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord, uMipBias);
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessTex == 1) {
        vec3 mr = texture(uTexMetallicRoughness, vTexCoord, uMipBias).rgb;
        metallic *= mr.b;
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = normalize(vNormalW);
    if (uHasNormalTex == 1) {
        vec3 mappedN = texture(uTexNormal, vTexCoord, uMipBias).rgb * 2.0 - 1.0;
        mappedN.xy *= uNormalScale;
        vec3 dp1 = dFdx(vWorldPos);
        vec3 dp2 = dFdy(vWorldPos);
        vec2 duv1 = dFdx(vTexCoord);
        vec2 duv2 = dFdy(vTexCoord);
        vec3 dp2perp = cross(dp2, N);
        vec3 dp1perp = cross(N, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
        float invmax = inversesqrt(max(dot(T,T), dot(B,B)));
        mat3 TBN = mat3(T * invmax, B * invmax, N);
        N = normalize(TBN * mappedN);
    }

    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), base.rgb, metallic);
    vec3 lightSum = vec3(0.0);

    if (uDirLightEnabled == 1) {
        vec3 L = uDirLightDir;
        float NdotL = max(dot(N, L), 0.0);
        lightSum += BRDF(N, V, L, base.rgb, metallic, roughness, F0) * uDirLightColor * NdotL;
    }
    for (int i = 0; i < 4; i++) {
        if (i >= uPointLightCount) break;
        vec3 toLight = uPointLightPos[i] - vWorldPos;
        float d = length(toLight);
        if (d > uPointLightRange[i]) continue;
        vec3 L = toLight / max(d, 0.0001);
        float NdotL = max(dot(N, L), 0.0);
        float atten = pow(max(1.0 - d / uPointLightRange[i], 0.0), 2.0);
        lightSum += BRDF(N, V, L, base.rgb, metallic, roughness, F0) * uPointLightColor[i] * NdotL * atten;
    }

    float ao = 1.0;
    if (uHasOcclusionTex == 1) ao = mix(1.0, texture(uTexOcclusion, vTexCoord, uMipBias).r, uOcclusionStrength);
    vec3 ambient = uAmbient * base.rgb * ao;
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord, uMipBias).rgb;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(ambient + lightSum + emissive, outAlpha);
    // Phase E.8.x: 世界法线 -> view-space 法线, encode 到 RG16F
    vec3 nView = normalize(uViewMat3 * N);
    FragNormal = nView.xy * 0.5 + 0.5;
    if (uHasVelocityHistory == 1) {
        vec2 curUV = (vCurClip.xy / max(vCurClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 prevUV = (vPrevClip.xy / max(vPrevClip.w, 1e-6)) * 0.5 + 0.5;
        vec2 raw = curUV - prevUV;
        // Phase E.16 — camera-only velocity (prevVP × curModel × pos)
        vec2 prevUVCam = (vPrevClipCameraOnly.xy / max(vPrevClipCameraOnly.w, 1e-6)) * 0.5 + 0.5;
        vec2 rawCam    = curUV - prevUVCam;
        // Phase E.14 — 按 velocity format 选编码路径
        //   RG16F 直接存 UV delta; RG8 用 bias/scale 压缩到 [0, 1] UNORM
        if (uVelocityFormat == 1) {
            FragVelocity       = clamp(raw    / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
            FragCameraVelocity = clamp(rawCam / (2.0 * uVelocityScale) + 0.5, 0.0, 1.0);
        } else {
            FragVelocity       = raw;
            FragCameraVelocity = rawCam;
        }
    } else {
        // RG8 模式下 0.5 表示零速度 (UNORM 中点); RG16F 模式下零速度即 0
        FragVelocity       = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
        FragCameraVelocity = (uVelocityFormat == 1) ? vec2(0.5) : vec2(0.0);
    }
}
)";
#endif

// ==================== Phase E.1.2 — VS_LIT2D + FS_LIT2D shader ====================
//
// 2D Lit forward 渲染 (与 3D PBR 区分: 简化 Lambertian + 多达 16 灯 + 可选 normal map).
// 顶点输入静态 layout 由 RenderVertex2DLit 决定 (location 0..4, 见 render_backend.h).
//
// Uniform 设计:
//   uMVP / uModel       — 标准变换 (model 用于把 normal/tangent 映射到世界空间)
//   uTexture            — baseColor (texture unit 0)
//   uNormalMap          — 法线贴图 (texture unit 1, uHasNormalMap=0 时不采样)
//   uHasNormalMap       — 0 / 1 (E.1.5 在 normalMapTex==0 时设 0)
//   uAmbient            — 环境光 (vec3)
//   uLightCount         — 实际激活灯数 (0..16)
//   uLightType[i]       — 1=Point, 2=Spot
//   uLightPos[i].xy     — 世界坐标 (z 字段未使用, 仅占位 vec3)
//   uLightDir[i].xy     — Spot 方向 (Point 时未使用, 字段需归一化)
//   uLightColor[i]      — RGB
//   uLightRange[i]      — 距离衰减半径 (d > range 时跳过)
//   uLightIntensity[i]  — 强度乘子
//   uLightInnerCos[i]   — cos(innerAngle), Spot only (smoothstep 内边)
//   uLightOuterCos[i]   — cos(outerAngle), Spot only (smoothstep 外边)
//
// 性能预算:
//   16 灯 × (1 int + 2 vec3 + 5 float) = 16*(1+6+5) = 16*12 = 192 标量
//   + 1 vec3 ambient + 4 sampler/int = 196 标量 + 几个杂项
//   远低于 GLES 3.0 MAX_FRAGMENT_UNIFORM_VECTORS=224 vec4 = 896 标量限制
//
// 数组大小必须用字面 16 而非宏 (C++ 宏不会扩展进 R"(...)" raw string).
// C++ 端同步保持 LIT2D_MAX_LIGHTS=16, 越界时调用方应在 Lighting2D::Add 时拒绝.

#define LIT2D_MAX_LIGHTS 16

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)

// ---- VS_LIT2D (GLES 3.0) ----
static const char* VS_LIT2D_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
layout(location=3) in vec3 aNormal;
layout(location=4) in vec4 aTangent;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec2 vUV;
out vec4 vColor;
out vec3 vWorldPos;
out mat3 vTBN;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    mat3 m3 = mat3(uModel);
    vec3 N = normalize(m3 * aNormal);
    vec3 T = normalize(m3 * aTangent.xyz);
    vec3 B = cross(N, T) * aTangent.w;
    vTBN = mat3(T, B, N);
}
)";

// ---- FS_LIT2D (GLES 3.0) ----
static const char* FS_LIT2D_SOURCE = R"(#version 300 es
precision highp float;
in vec2 vUV;
in vec4 vColor;
in vec3 vWorldPos;
in mat3 vTBN;
uniform sampler2D uTexture;
uniform sampler2D uNormalMap;
uniform int       uHasNormalMap;
uniform vec3      uAmbient;
uniform int       uLightCount;
uniform int   uLightType[16];
uniform vec3  uLightPos[16];
uniform vec3  uLightDir[16];
uniform vec3  uLightColor[16];
uniform float uLightRange[16];
uniform float uLightIntensity[16];
uniform float uLightInnerCos[16];
uniform float uLightOuterCos[16];
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: 2D 默认 朝相机
void main() {
    vec4 base = texture(uTexture, vUV) * vColor;
    vec3 N;
    if (uHasNormalMap == 1) {
        vec3 nTS = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
        N = normalize(vTBN * nTS);
    } else {
        N = vTBN[2];
    }
    vec3 lightSum = uAmbient;
    for (int i = 0; i < 16; i++) {
        if (i >= uLightCount) break;
        vec2 toLight = uLightPos[i].xy - vWorldPos.xy;
        float d = length(toLight);
        if (d > uLightRange[i]) continue;
        vec3 L = vec3(toLight / max(d, 0.0001), 0.0);
        float NdotL = max(dot(N, L), 0.0);
        float atten = pow(max(1.0 - d / uLightRange[i], 0.0), 2.0);
        if (uLightType[i] == 2) {
            float cosA = dot(-L.xy, uLightDir[i].xy);
            float spot_f = smoothstep(uLightOuterCos[i], uLightInnerCos[i], cosA);
            atten *= spot_f;
        }
        lightSum += uLightColor[i] * uLightIntensity[i] * NdotL * atten;
    }
    FragColor = vec4(base.rgb * lightSum, base.a);
    FragNormal = vec2(0.5, 0.5);   // view-space (0,0,1)
}
)";

#else  // 桌面 GL 3.3 Core

// ---- VS_LIT2D (GL 3.3) ----
static const char* VS_LIT2D_SOURCE = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
layout(location=3) in vec3 aNormal;
layout(location=4) in vec4 aTangent;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec2 vUV;
out vec4 vColor;
out vec3 vWorldPos;
out mat3 vTBN;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    mat3 m3 = mat3(uModel);
    vec3 N = normalize(m3 * aNormal);
    vec3 T = normalize(m3 * aTangent.xyz);
    vec3 B = cross(N, T) * aTangent.w;
    vTBN = mat3(T, B, N);
}
)";

// ---- FS_LIT2D (GL 3.3) ----
static const char* FS_LIT2D_SOURCE = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
in vec3 vWorldPos;
in mat3 vTBN;
uniform sampler2D uTexture;
uniform sampler2D uNormalMap;
uniform int       uHasNormalMap;
uniform vec3      uAmbient;
uniform int       uLightCount;
uniform int   uLightType[16];
uniform vec3  uLightPos[16];
uniform vec3  uLightDir[16];
uniform vec3  uLightColor[16];
uniform float uLightRange[16];
uniform float uLightIntensity[16];
uniform float uLightInnerCos[16];
uniform float uLightOuterCos[16];
layout(location=0) out vec4 FragColor;
layout(location=1) out vec2 FragNormal;   // Phase E.8.x: 2D 默认 朝相机
void main() {
    vec4 base = texture(uTexture, vUV) * vColor;
    vec3 N;
    if (uHasNormalMap == 1) {
        vec3 nTS = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
        N = normalize(vTBN * nTS);
    } else {
        N = vTBN[2];
    }
    vec3 lightSum = uAmbient;
    for (int i = 0; i < 16; i++) {
        if (i >= uLightCount) break;
        vec2 toLight = uLightPos[i].xy - vWorldPos.xy;
        float d = length(toLight);
        if (d > uLightRange[i]) continue;
        vec3 L = vec3(toLight / max(d, 0.0001), 0.0);
        float NdotL = max(dot(N, L), 0.0);
        float atten = pow(max(1.0 - d / uLightRange[i], 0.0), 2.0);
        if (uLightType[i] == 2) {
            float cosA = dot(-L.xy, uLightDir[i].xy);
            float spot_f = smoothstep(uLightOuterCos[i], uLightInnerCos[i], cosA);
            atten *= spot_f;
        }
        lightSum += uLightColor[i] * uLightIntensity[i] * NdotL * atten;
    }
    FragColor = vec4(base.rgb * lightSum, base.a);
    FragNormal = vec2(0.5, 0.5);   // view-space (0,0,1)
}
)";

#endif

// ==================== Phase E.3.1 — ACES Tonemap shader ====================
//
// 全屏 quad 后处理 pass: HDR RT (RGBA16F) → ACES tonemap + sRGB encode → default fb
//
// 顶点格式 (与 fullscreen quad VBO 一致):
//   layout(0) vec2 aPos    [-1..1] (NDC, 跳过 MVP)
//   layout(1) vec2 aUV     [0..1]  (采样 HDR RT)
//
// uniform:
//   sampler2D uHDRTex     HDR 颜色纹理 (texture unit 0)
//   float     uExposure   线性曝光预乘 (默认 1.0)
//   float     uGamma      sRGB encode gamma (默认 2.2)
//
// ACES fitted by Krzysztof Narkowicz (2016):
//   https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)

// ---- VS_TONEMAP (GLES 3.0) ----
static const char* VS_TONEMAP_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// ---- FS_TONEMAP (GLES 3.0) Phase E.3.4 + F.0.10.8 — 多 tonemap operator + 3D LUT color grading ----
static const char* FS_TONEMAP_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler3D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDRTex;
uniform sampler3D uLUT;          // Phase F.0.10.8 — 3D color grading LUT (未用时默认 unit 1)
uniform float uExposure;
uniform float uGamma;
uniform int   uTonemapMode;      // 0=ACES 1=Reinhard 2=Uncharted2 3=Linear
uniform float uLUTStrength;      // Phase F.0.10.8 — LUT 混合强度 [0,1]
uniform int   uLUTEnabled;       // Phase F.0.10.8 — 0=跳过 LUT, 1=采样 LUT

// ACES filmic (Krzysztof Narkowicz 2016 fit)
vec3 TonemapACES(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a*x + b)) / (x * (c*x + d) + e), 0.0, 1.0);
}

// Reinhard (简单基线, x/(1+x))
vec3 TonemapReinhard(vec3 x) {
    return x / (vec3(1.0) + x);
}

// Uncharted 2 (Hable filmic, 含 white scale)
vec3 U2Func(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A*x + C*B) + D*E) / (x * (A*x + B) + D*F)) - E/F;
}
vec3 TonemapUncharted2(vec3 x) {
    const float W = 11.2;
    vec3 whiteScale = vec3(1.0) / U2Func(vec3(W));
    return clamp(U2Func(x * 2.0) * whiteScale, 0.0, 1.0);
}

// Linear (纯 clamp, 调试用)
vec3 TonemapLinear(vec3 x) {
    return clamp(x, 0.0, 1.0);
}

void main() {
    vec3 hdr = max(texture(uHDRTex, vUV).rgb, vec3(0.0)) * uExposure;
    vec3 ldr;
    if      (uTonemapMode == 1) ldr = TonemapReinhard(hdr);
    else if (uTonemapMode == 2) ldr = TonemapUncharted2(hdr);
    else if (uTonemapMode == 3) ldr = TonemapLinear(hdr);
    else                        ldr = TonemapACES(hdr);   // 0 或其他值 → ACES
    // Phase F.0.10.8: LUT 混合 (uniform branch, GPU 零成本)
    if (uLUTEnabled != 0) {
        vec3 graded = texture(uLUT, clamp(ldr, 0.0, 1.0)).rgb;
        ldr = mix(ldr, graded, uLUTStrength);
    }
    vec3 srgb = pow(ldr, vec3(1.0 / max(uGamma, 0.0001)));
    FragColor = vec4(srgb, 1.0);
}
)";

// ==================== Phase E.4 — Bloom shader (GLES 3.0) ====================

// ---- FS_BLOOM_BRIGHT (GLES 3.0): 亮度阈值提取 + soft knee ----
static const char* FS_BLOOM_BRIGHT_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform float uThreshold;
void main() {
    vec3 c = max(texture(uSrc, vUV).rgb, vec3(0.0));
    float L = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float knee = max(uThreshold * 0.5, 1e-5);
    float soft = clamp(L - uThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(L - uThreshold, soft) / max(L, 1e-5);
    FragColor = vec4(c * contribution, 1.0);
}
)";

// ---- FS_BLOOM_DOWN (GLES 3.0): 13-tap COD AW downsample ----
static const char* FS_BLOOM_DOWN_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform vec4 uUvBounds;   // Phase F.0.10.5: (uMin.xy, uMax.xy) clamp 13-tap; 默认 (0,0,1,1) = 无 clamp
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }   // Phase F.0.10.5
void main() {
    // COD AW 13-tap: 4 个 ±2 偏移 + 8 个 ±1/±2 偏移 + 1 个中心
    // Phase F.0.10.5: ClampUV 防跨 region 边界采样泄漏 (Bloom mip 链上限明显)
    vec3 A = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0,  2.0))).rgb;
    vec3 B = texture(uSrc, ClampUV(vUV + uTexel * vec2( 0.0,  2.0))).rgb;
    vec3 C = texture(uSrc, ClampUV(vUV + uTexel * vec2( 2.0,  2.0))).rgb;
    vec3 D = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0,  0.0))).rgb;
    vec3 E = texture(uSrc, ClampUV(vUV                              )).rgb;
    vec3 F = texture(uSrc, ClampUV(vUV + uTexel * vec2( 2.0,  0.0))).rgb;
    vec3 G = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0, -2.0))).rgb;
    vec3 H = texture(uSrc, ClampUV(vUV + uTexel * vec2( 0.0, -2.0))).rgb;
    vec3 I = texture(uSrc, ClampUV(vUV + uTexel * vec2( 2.0, -2.0))).rgb;
    vec3 J = texture(uSrc, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb;
    vec3 K = texture(uSrc, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb;
    vec3 L = texture(uSrc, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb;
    vec3 M = texture(uSrc, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb;
    // COD AW 权重: 中心 4 重 (J K L M) * 0.5, 8 个外围 * 0.125
    vec3 o = E * 0.125;
    o += (A + C + G + I) * 0.03125;
    o += (B + D + F + H) * 0.0625;
    o += (J + K + L + M) * 0.125;
    FragColor = vec4(o, 1.0);
}
)";

// ---- FS_BLOOM_UP (GLES 3.0): tent 3x3 upsample + intensity ----
// 用于 Upsample 和 Composite; radius=0 + intensity 相当于 composite
static const char* FS_BLOOM_UP_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2  uTexel;
uniform float uRadius;       // upsample 扩散半径 (0..1+)
uniform float uIntensity;    // composite 缩放 (upsample 时为 1.0)
uniform vec4  uUvBounds;     // Phase F.0.10.5: (uMin.xy, uMax.xy) clamp 9-tap tent; 默认 (0,0,1,1) = 无 clamp
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }   // Phase F.0.10.5
void main() {
    vec2 d = uTexel * uRadius;
    // tent 9-tap; Phase F.0.10.5: ClampUV 防跨 region 边界
    vec3 c = texture(uSrc, ClampUV(vUV)).rgb * 4.0;
    c += texture(uSrc, ClampUV(vUV + vec2(-d.x,  0.0))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2( d.x,  0.0))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2( 0.0, -d.y))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2( 0.0,  d.y))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2(-d.x, -d.y))).rgb;
    c += texture(uSrc, ClampUV(vUV + vec2( d.x, -d.y))).rgb;
    c += texture(uSrc, ClampUV(vUV + vec2(-d.x,  d.y))).rgb;
    c += texture(uSrc, ClampUV(vUV + vec2( d.x,  d.y))).rgb;
    FragColor = vec4((c / 16.0) * uIntensity, 1.0);
}
)";

// ==================== Phase E.5 — Auto Exposure shader (GLES 3.0) ====================

// ---- FS_LUMINANCE_EXTRACT (GLES 3.0): hdrTex -> R16F log-luminance ----
// 输出仅 R 通道用 (R16F target); 数值范围 clamp [-12, 12] 防 R16F 半精度 underflow
static const char* FS_LUMINANCE_EXTRACT_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHDRTex;
void main() {
    vec3 rgb = texture(uHDRTex, vUV).rgb;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));   // Rec.709
    float logLuma = log(max(luma, 0.0001));
    FragColor = vec4(clamp(logLuma, -12.0, 12.0), 0.0, 0.0, 0.0);
}
)";

// ==================== Phase E.6 — Lens Dirt + Streak shader (GLES 3.0) ====================

// ---- FS_LENS_DIRT_COMPOSITE (GLES 3.0): bloom x dirt x intensity (additive blend 由调用方控) ----
static const char* FS_LENS_DIRT_COMPOSITE_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uBloomTex;
uniform sampler2D uDirtTex;
uniform float uIntensity;
void main() {
    vec3 bloom = texture(uBloomTex, vUV).rgb;
    vec3 dirt  = texture(uDirtTex,  vUV).rgb;
    FragColor = vec4(bloom * dirt * uIntensity, 1.0);
}
)";

// ---- FS_STREAK_BLUR (GLES 3.0): 7-tap 方向高斯 ----
static const char* FS_STREAK_BLUR_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2  uTexel;
uniform float uLength;
uniform vec2  uDirection;
void main() {
    vec2 d  = normalize(uDirection) * uLength;
    vec3 c  = texture(uSrc, vUV - 3.0 * d).rgb * 0.05
            + texture(uSrc, vUV - 2.0 * d).rgb * 0.10
            + texture(uSrc, vUV - 1.0 * d).rgb * 0.20
            + texture(uSrc, vUV).rgb           * 0.30
            + texture(uSrc, vUV + 1.0 * d).rgb * 0.20
            + texture(uSrc, vUV + 2.0 * d).rgb * 0.10
            + texture(uSrc, vUV + 3.0 * d).rgb * 0.05;
    FragColor = vec4(c, 1.0);
}
)";

// ---- FS_STREAK_COMPOSITE (GLES 3.0): streak x intensity (additive blend 由调用方控) ----
static const char* FS_STREAK_COMPOSITE_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform float uIntensity;
void main() {
    vec3 c = texture(uSrc, vUV).rgb;
    FragColor = vec4(c * uIntensity, 1.0);
}
)";

// ---- FS_LENS_FLARE_GHOST (GLES 3.0): ghost + halo + chromatic aberration ----
// 单 pass 输出: ghost 朝中心反投采样 + halo 环形偏移采样 + RGB 径向色差
// GLES 兼容: 静态 for 上限 8, 内部 `if (i >= count) break;`
static const char* FS_LENS_FLARE_GHOST_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uBrightTex;
uniform sampler2D uFlareTex;        // Phase E.7.4: 用户贴图 (1x1 白 fallback)
uniform int   uGhostCount;          // [0, 8]
uniform float uGhostDispersal;      // [0, 2]
uniform float uHaloWidth;           // [0, 1]
uniform float uChromaticAberration; // [0, 0.02]
uniform int   uDistortionEnabled;   // 0/1

vec3 sampleChroma(sampler2D tex, vec2 uv, vec2 caOffset) {
    if (uDistortionEnabled == 0) {
        return texture(tex, clamp(uv, vec2(0.001), vec2(0.999))).rgb;
    }
    float r = texture(tex, clamp(uv + caOffset, vec2(0.001), vec2(0.999))).r;
    float g = texture(tex, clamp(uv,            vec2(0.001), vec2(0.999))).g;
    float b = texture(tex, clamp(uv - caOffset, vec2(0.001), vec2(0.999))).b;
    return vec3(r, g, b);
}

void main() {
    // 反向采样模拟相机镜组内反射
    vec2 flippedUV = vec2(1.0) - vUV;
    vec2 centerVec = vec2(0.5) - vUV;
    vec3 result = vec3(0.0);

    // Ghost
    if (uGhostCount > 0) {
        vec2 ghostVec = (vec2(0.5) - flippedUV) * uGhostDispersal;
        vec2 caDir = normalize(ghostVec + vec2(1e-6)) * uChromaticAberration;
        for (int i = 0; i < 8; ++i) {
            if (i >= uGhostCount) break;
            vec2 sampleUV = flippedUV + ghostVec * float(i);
            float distFromCenter = length(vec2(0.5) - sampleUV);
            float weight = pow(max(0.0, 1.0 - distFromCenter * 2.0), 4.0);
            result += sampleChroma(uBrightTex, sampleUV, caDir) * weight;
        }
    }

    // Halo
    if (uHaloWidth > 0.0) {
        vec2 haloVec = normalize(centerVec + vec2(1e-6)) * uHaloWidth;
        vec2 haloUV  = vUV + haloVec;
        vec2 caDir   = normalize(haloVec + vec2(1e-6)) * uChromaticAberration;
        float distFromRing = abs(length(centerVec) - uHaloWidth);
        float haloWeight   = smoothstep(0.5, 0.0, distFromRing);
        result += sampleChroma(uBrightTex, haloUV, caDir) * haloWeight;
    }

    // Phase E.7.4: 用户贴图整体调制 (1x1 白 fallback = 不变)
    vec3 flareModulation = texture(uFlareTex, vUV).rgb;
    result *= flareModulation;

    FragColor = vec4(result, 1.0);
}
)";

#else  // 桌面 GL 3.3 Core

// ---- VS_TONEMAP (GL 3.3) ----
static const char* VS_TONEMAP_SOURCE = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// ---- FS_TONEMAP (GL 3.3) Phase E.3.4 + F.0.10.8 — 多 tonemap operator + 3D LUT color grading ----
static const char* FS_TONEMAP_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDRTex;
uniform sampler3D uLUT;          // Phase F.0.10.8 — 3D color grading LUT
uniform float uExposure;
uniform float uGamma;
uniform int   uTonemapMode;      // 0=ACES 1=Reinhard 2=Uncharted2 3=Linear
uniform float uLUTStrength;      // Phase F.0.10.8 — LUT 混合强度 [0,1]
uniform int   uLUTEnabled;       // Phase F.0.10.8 — 0=跳过 LUT, 1=采样 LUT

// ACES filmic (Krzysztof Narkowicz 2016 fit)
vec3 TonemapACES(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a*x + b)) / (x * (c*x + d) + e), 0.0, 1.0);
}

// Reinhard (简单基线, x/(1+x))
vec3 TonemapReinhard(vec3 x) {
    return x / (vec3(1.0) + x);
}

// Uncharted 2 (Hable filmic, 含 white scale)
vec3 U2Func(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A*x + C*B) + D*E) / (x * (A*x + B) + D*F)) - E/F;
}
vec3 TonemapUncharted2(vec3 x) {
    const float W = 11.2;
    vec3 whiteScale = vec3(1.0) / U2Func(vec3(W));
    return clamp(U2Func(x * 2.0) * whiteScale, 0.0, 1.0);
}

// Linear (纯 clamp, 调试用)
vec3 TonemapLinear(vec3 x) {
    return clamp(x, 0.0, 1.0);
}

void main() {
    vec3 hdr = max(texture(uHDRTex, vUV).rgb, vec3(0.0)) * uExposure;
    vec3 ldr;
    if      (uTonemapMode == 1) ldr = TonemapReinhard(hdr);
    else if (uTonemapMode == 2) ldr = TonemapUncharted2(hdr);
    else if (uTonemapMode == 3) ldr = TonemapLinear(hdr);
    else                        ldr = TonemapACES(hdr);   // 0 或其他值 → ACES
    // Phase F.0.10.8: LUT 混合 (uniform branch, GPU 零成本)
    if (uLUTEnabled != 0) {
        vec3 graded = texture(uLUT, clamp(ldr, 0.0, 1.0)).rgb;
        ldr = mix(ldr, graded, uLUTStrength);
    }
    vec3 srgb = pow(ldr, vec3(1.0 / max(uGamma, 0.0001)));
    FragColor = vec4(srgb, 1.0);
}
)";

// ==================== Phase E.4 — Bloom shader (GL 3.3 Core) ====================

// ---- FS_BLOOM_BRIGHT (GL 3.3): 亮度阈值提取 + soft knee ----
static const char* FS_BLOOM_BRIGHT_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform float uThreshold;
void main() {
    vec3 c = max(texture(uSrc, vUV).rgb, vec3(0.0));
    float L = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float knee = max(uThreshold * 0.5, 1e-5);
    float soft = clamp(L - uThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contribution = max(L - uThreshold, soft) / max(L, 1e-5);
    FragColor = vec4(c * contribution, 1.0);
}
)";

// ---- FS_BLOOM_DOWN (GL 3.3): 13-tap COD AW downsample ----
static const char* FS_BLOOM_DOWN_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform vec4 uUvBounds;   // Phase F.0.10.5: (uMin.xy, uMax.xy) clamp 13-tap; 默认 (0,0,1,1) = 无 clamp
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }   // Phase F.0.10.5
void main() {
    // Phase F.0.10.5: ClampUV 防跨 region 边界采样泄漏
    vec3 A = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0,  2.0))).rgb;
    vec3 B = texture(uSrc, ClampUV(vUV + uTexel * vec2( 0.0,  2.0))).rgb;
    vec3 C = texture(uSrc, ClampUV(vUV + uTexel * vec2( 2.0,  2.0))).rgb;
    vec3 D = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0,  0.0))).rgb;
    vec3 E = texture(uSrc, ClampUV(vUV                              )).rgb;
    vec3 F = texture(uSrc, ClampUV(vUV + uTexel * vec2( 2.0,  0.0))).rgb;
    vec3 G = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0, -2.0))).rgb;
    vec3 H = texture(uSrc, ClampUV(vUV + uTexel * vec2( 0.0, -2.0))).rgb;
    vec3 I = texture(uSrc, ClampUV(vUV + uTexel * vec2( 2.0, -2.0))).rgb;
    vec3 J = texture(uSrc, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb;
    vec3 K = texture(uSrc, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb;
    vec3 L = texture(uSrc, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb;
    vec3 M = texture(uSrc, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb;
    vec3 o = E * 0.125;
    o += (A + C + G + I) * 0.03125;
    o += (B + D + F + H) * 0.0625;
    o += (J + K + L + M) * 0.125;
    FragColor = vec4(o, 1.0);
}
)";

// ---- FS_BLOOM_UP (GL 3.3): tent 3x3 upsample + intensity ----
static const char* FS_BLOOM_UP_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2  uTexel;
uniform float uRadius;
uniform float uIntensity;
uniform vec4  uUvBounds;     // Phase F.0.10.5: (uMin.xy, uMax.xy) clamp 9-tap tent
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }   // Phase F.0.10.5
void main() {
    vec2 d = uTexel * uRadius;
    // Phase F.0.10.5: ClampUV 防跨 region 边界
    vec3 c = texture(uSrc, ClampUV(vUV)).rgb * 4.0;
    c += texture(uSrc, ClampUV(vUV + vec2(-d.x,  0.0))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2( d.x,  0.0))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2( 0.0, -d.y))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2( 0.0,  d.y))).rgb * 2.0;
    c += texture(uSrc, ClampUV(vUV + vec2(-d.x, -d.y))).rgb;
    c += texture(uSrc, ClampUV(vUV + vec2( d.x, -d.y))).rgb;
    c += texture(uSrc, ClampUV(vUV + vec2(-d.x,  d.y))).rgb;
    c += texture(uSrc, ClampUV(vUV + vec2( d.x,  d.y))).rgb;
    FragColor = vec4((c / 16.0) * uIntensity, 1.0);
}
)";

// ==================== Phase E.5 — Auto Exposure shader (GL 3.3 Core) ====================

// ---- FS_LUMINANCE_EXTRACT (GL 3.3): hdrTex -> R16F log-luminance ----
// 输出仅 R 通道用 (R16F target); 数值范围 clamp [-12, 12] 防 R16F 半精度 underflow
static const char* FS_LUMINANCE_EXTRACT_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uHDRTex;
void main() {
    vec3 rgb = texture(uHDRTex, vUV).rgb;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));   // Rec.709
    float logLuma = log(max(luma, 0.0001));
    FragColor = vec4(clamp(logLuma, -12.0, 12.0), 0.0, 0.0, 0.0);
}
)";

// ==================== Phase E.6 — Lens Dirt + Streak shader (GL 3.3 Core) ====================

// ---- FS_LENS_DIRT_COMPOSITE (GL 3.3): bloom x dirt x intensity ----
static const char* FS_LENS_DIRT_COMPOSITE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uBloomTex;
uniform sampler2D uDirtTex;
uniform float uIntensity;
void main() {
    vec3 bloom = texture(uBloomTex, vUV).rgb;
    vec3 dirt  = texture(uDirtTex,  vUV).rgb;
    FragColor = vec4(bloom * dirt * uIntensity, 1.0);
}
)";

// ---- FS_STREAK_BLUR (GL 3.3): 7-tap 方向高斯 ----
static const char* FS_STREAK_BLUR_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2  uTexel;
uniform float uLength;
uniform vec2  uDirection;
void main() {
    vec2 d  = normalize(uDirection) * uLength;
    vec3 c  = texture(uSrc, vUV - 3.0 * d).rgb * 0.05
            + texture(uSrc, vUV - 2.0 * d).rgb * 0.10
            + texture(uSrc, vUV - 1.0 * d).rgb * 0.20
            + texture(uSrc, vUV).rgb           * 0.30
            + texture(uSrc, vUV + 1.0 * d).rgb * 0.20
            + texture(uSrc, vUV + 2.0 * d).rgb * 0.10
            + texture(uSrc, vUV + 3.0 * d).rgb * 0.05;
    FragColor = vec4(c, 1.0);
}
)";

// ---- FS_STREAK_COMPOSITE (GL 3.3): streak x intensity ----
static const char* FS_STREAK_COMPOSITE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform float uIntensity;
void main() {
    vec3 c = texture(uSrc, vUV).rgb;
    FragColor = vec4(c * uIntensity, 1.0);
}
)";

// ---- FS_LENS_FLARE_GHOST (GL 3.3): ghost + halo + chromatic aberration ----
// 同 GLES3 版本算法; 详见 GLES3 块的注释.
static const char* FS_LENS_FLARE_GHOST_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uBrightTex;
uniform sampler2D uFlareTex;        // Phase E.7.4: 用户贴图 (1x1 白 fallback)
uniform int   uGhostCount;          // [0, 8]
uniform float uGhostDispersal;      // [0, 2]
uniform float uHaloWidth;           // [0, 1]
uniform float uChromaticAberration; // [0, 0.02]
uniform int   uDistortionEnabled;   // 0/1

vec3 sampleChroma(sampler2D tex, vec2 uv, vec2 caOffset) {
    if (uDistortionEnabled == 0) {
        return texture(tex, clamp(uv, vec2(0.001), vec2(0.999))).rgb;
    }
    float r = texture(tex, clamp(uv + caOffset, vec2(0.001), vec2(0.999))).r;
    float g = texture(tex, clamp(uv,            vec2(0.001), vec2(0.999))).g;
    float b = texture(tex, clamp(uv - caOffset, vec2(0.001), vec2(0.999))).b;
    return vec3(r, g, b);
}

void main() {
    vec2 flippedUV = vec2(1.0) - vUV;
    vec2 centerVec = vec2(0.5) - vUV;
    vec3 result = vec3(0.0);

    if (uGhostCount > 0) {
        vec2 ghostVec = (vec2(0.5) - flippedUV) * uGhostDispersal;
        vec2 caDir = normalize(ghostVec + vec2(1e-6)) * uChromaticAberration;
        for (int i = 0; i < 8; ++i) {
            if (i >= uGhostCount) break;
            vec2 sampleUV = flippedUV + ghostVec * float(i);
            float distFromCenter = length(vec2(0.5) - sampleUV);
            float weight = pow(max(0.0, 1.0 - distFromCenter * 2.0), 4.0);
            result += sampleChroma(uBrightTex, sampleUV, caDir) * weight;
        }
    }

    if (uHaloWidth > 0.0) {
        vec2 haloVec = normalize(centerVec + vec2(1e-6)) * uHaloWidth;
        vec2 haloUV  = vUV + haloVec;
        vec2 caDir   = normalize(haloVec + vec2(1e-6)) * uChromaticAberration;
        float distFromRing = abs(length(centerVec) - uHaloWidth);
        float haloWeight   = smoothstep(0.5, 0.0, distFromRing);
        result += sampleChroma(uBrightTex, haloUV, caDir) * haloWeight;
    }

    // Phase E.7.4: 用户贴图整体调制 (1x1 白 fallback = 不变)
    vec3 flareModulation = texture(uFlareTex, vUV).rgb;
    result *= flareModulation;

    FragColor = vec4(result, 1.0);
}
)";

#endif

// ==================== Phase E.8 — SSAO shaders (双 profile: GLES3 + GL33) ====================
//
// 3 shader:
//   FS_SSAO           — raw AO 计算 (depth 重建 view pos + ddx/ddy 重建 normal + 16 采样 kernel)
//   FS_SSAO_BLUR      — 双边分离滤波 (水平/垂直 depth-aware blur)
//   FS_SSAO_COMPOSITE — HDR *= mix(1.0, ao, intensity) 乘法调制
//
// 算法要点:
//   * 半球采样 (kernel 在 tangent space, z >= 0)
//   * noise 4x4 tile 每 pixel 旋转 kernel (避免 banding)
//   * 深度重建 view pos: invProj * clip -> view (w divide)
//   * 法线重建: cross(ddy(viewPos), ddx(viewPos)) — 每像素屏幕空间面法
//   * range check: smoothstep(0, 1, radius / |P.z - sampP.z|) — 远处采样衰减
//
// 性能:
//   * kernelSize = 16 @ 半分辨率: ~1 ms @ 1920x1080 NV GTX 1060
//   * kernelSize = 8: ~0.5 ms (质量可接受)
//   * blur 2-pass: ~0.3 ms (full-res 采样 5-tap bilateral)

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)

// ---- FS_SSAO (GLES 3.0): raw AO 计算 ----
static const char* FS_SSAO_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepthTex;     // full-res depth texture (NEAREST)
uniform sampler2D uNoiseTex;     // 4x4 RGBA8 noise (REPEAT)
uniform sampler2D uNormalTex;    // Phase E.8.x: G-buffer view-space normal (RG16F)
uniform mat4  uProj;             // 当前 projection (view -> clip)
uniform mat4  uInvProj;          // inverse(uProj)
uniform vec3  uKernel[16];       // 半球采样方向 (tangent space)
uniform int   uKernelSize;
uniform float uRadius;
uniform float uBias;
uniform float uPower;
uniform vec2  uNoiseScale;       // (screenW / 4, screenH / 4)

// 从 UV + hardware depth [0,1] 重建 view-space 位置
vec3 ReconstructViewPos(vec2 uv, float d) {
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v    = uInvProj * clip;
    return v.xyz / v.w;
}

// Phase E.8.x: 从 RG16F G-buffer 解码 view-space normal
// xy 存 [0,1] (代表 [-1,1]); z 由 sqrt(1 - x² - y²) 重建 (z >= 0 背向相机侧)
vec3 DecodeViewNormal(vec2 enc) {
    vec2 nxy = enc * 2.0 - 1.0;
    float zsq = max(0.0, 1.0 - dot(nxy, nxy));
    return vec3(nxy, sqrt(zsq));
}

void main() {
    float d = texture(uDepthTex, vUV).r;
    // 天空 / 无几何: AO = 1 (无遮蔽)
    if (d >= 0.9999) { FragColor = vec4(1.0); return; }

    vec3 P = ReconstructViewPos(vUV, d);
    // Phase E.8.x: G-buffer normal 取代 ddx/ddy 重建 (边缘无条纹噪声)
    vec3 N = DecodeViewNormal(texture(uNormalTex, vUV).rg);
    // noise: (rx, ry, 0), 每像素旋转 kernel 避免 banding
    vec3 R = texture(uNoiseTex, vUV * uNoiseScale).xyz * 2.0 - 1.0;
    R = normalize(vec3(R.xy, 0.0));
    // Gram-Schmidt 构造 tangent space
    vec3 T = normalize(R - N * dot(R, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < 16; i++) {
        if (i >= uKernelSize) break;
        // kernel 变换到 view space
        vec3 samp = TBN * uKernel[i];
        samp = P + samp * uRadius;

        // 投影到屏幕
        vec4 proj = uProj * vec4(samp, 1.0);
        proj.xyz /= proj.w;
        vec2 sampUV = proj.xy * 0.5 + 0.5;

        // 取采样点的实际深度
        float sampDepth = texture(uDepthTex, sampUV).r;
        vec3  sampP     = ReconstructViewPos(sampUV, sampDepth);

        // 距离 falloff: 远处采样贡献衰减
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sampP.z), 1e-4));
        // 若采样点更近 camera (sampP.z > samp.z + bias) 则被遮蔽
        occlusion += (sampP.z >= samp.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(uKernelSize);
    ao = pow(ao, uPower);
    FragColor = vec4(ao, ao, ao, 1.0);
}
)";

// ---- FS_SSAO_BLUR (GLES 3.0): 双边分离滤波 ----
static const char* FS_SSAO_BLUR_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSSAOTex;
uniform sampler2D uDepthTex;
uniform vec2      uTexel;       // (1/w, 1/h)
uniform int       uAxis;        // 0=水平; 1=垂直

void main() {
    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    float cDepth = texture(uDepthTex, vUV).r;
    float sum = 0.0, wsum = 0.0;
    // 5-tap bilateral
    for (int i = -2; i <= 2; i++) {
        vec2 uv = vUV + dir * float(i);
        float ao = texture(uSSAOTex, uv).r;
        float d  = texture(uDepthTex, uv).r;
        // depth-aware 权重: 跨 depth 边界时权重骤降, 保留物体边缘
        float w  = exp(-abs(cDepth - d) * 200.0);
        sum  += ao * w;
        wsum += w;
    }
    float o = sum / max(wsum, 1e-4);
    FragColor = vec4(o, o, o, 1.0);
}
)";

// ---- FS_SSAO_COMPOSITE (GLES 3.0): HDR *= mix(1.0, ao, intensity) ----
static const char* FS_SSAO_COMPOSITE_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneTex;     // HDR color (临时拷贝, 避免 feedback loop)
uniform sampler2D uAOTex;         // blurred AO (R16F, full-res 采样插值)
uniform float     uIntensity;

void main() {
    vec3 hdr = texture(uSceneTex, vUV).rgb;
    float ao = texture(uAOTex, vUV).r;
    // intensity=0: AO 不生效 (输出=hdr); intensity=1: 完全乘 AO
    float o = mix(1.0, ao, uIntensity);
    FragColor = vec4(hdr * o, 1.0);
}
)";

// ==================== Phase E.9 — SSR shaders (GLES 3.0) ====================
//
// 2 shader:
//   FS_SSR           — linear ray march in view space
//   FS_SSR_COMPOSITE — HDR += reflect.rgb * reflect.a * intensity (additive)
//
// 复用 Phase E.8.x 的 G-buffer view-space normal RG16F MRT (slot 1).
// 高质量方案: full-res RGBA16F + 64 步默认 (用户拍板 2026-05-12).

// ---- FS_SSR (GLES 3.0): linear ray march 计算反射 ----
static const char* FS_SSR_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepthTex;     // full-res HDR depth (NEAREST, 已 blit)
uniform sampler2D uNormalTex;    // Phase E.8.x: G-buffer view-space normal (RG16F)
uniform sampler2D uHDRTex;       // HDR color (反射采样源)
uniform mat4  uProj;             // view -> clip
uniform mat4  uInvProj;          // inverse(uProj)
uniform int   uMaxSteps;         // [8, 128] ray march 步数
uniform float uStepSize;         // 每步 view-space units
uniform float uThickness;        // 深度命中容差 (view-space units)
uniform float uMaxDistance;      // ray march 距离上限
uniform float uEdgeFade;         // 屏幕边缘 fade 区域宽度 [0, 0.5]
uniform vec2  uJitterOffset;     // Phase E.12: UV 空间 jitter (±0.5/W, ±0.5/H), TAA-style

// 与 SSAO 同算法: NDC -> view space (perspective divide)
vec3 ReconstructViewPos(vec2 uv, float d) {
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v    = uInvProj * clip;
    return v.xyz / v.w;
}

// Phase E.8.x: 从 RG16F G-buffer 解码 view-space normal
vec3 DecodeViewNormal(vec2 enc) {
    vec2 nxy = enc * 2.0 - 1.0;
    float zsq = max(0.0, 1.0 - dot(nxy, nxy));
    return vec3(nxy, sqrt(zsq));
}

void main() {
    // Phase E.12: ray march 起点 jitter (±0.5 pixel), Temporal accumulation 配套
    vec2 uvJ = vUV + uJitterOffset;
    float d = texture(uDepthTex, uvJ).r;
    // 天空盒不反射 (depth >= 1 表示无几何)
    if (d >= 0.9999) { FragColor = vec4(0.0); return; }

    vec3 viewPos = ReconstructViewPos(uvJ, d);
    vec3 viewN   = DecodeViewNormal(texture(uNormalTex, uvJ).rg);
    vec3 viewV   = normalize(-viewPos);   // 视点方向 (camera 在原点)

    // 自反射剔除: 法线背向相机 (本不该可见, 防御)
    if (dot(viewN, viewV) < 0.05) { FragColor = vec4(0.0); return; }

    // 反射方向 = reflect(入射, 法线); 入射 = -viewV
    vec3 viewR = reflect(-viewV, viewN);

    // ray march in view space
    vec3 hitColor   = vec3(0.0);
    float hitAlpha  = 0.0;

    for (int i = 1; i <= 128; ++i) {
        if (i > uMaxSteps) break;

        vec3 samplePosVS = viewPos + viewR * (uStepSize * float(i));
        // 距离上限 (view-space depth 是负值; -z 是真实距离)
        if (-samplePosVS.z > uMaxDistance) break;

        // 投影到屏幕
        vec4 sampleClip = uProj * vec4(samplePosVS, 1.0);
        if (sampleClip.w <= 0.0) break;   // 后向裁剪
        vec2 sampleUV   = sampleClip.xy / sampleClip.w * 0.5 + 0.5;

        // 屏外 -> 立即终止 (无 fallback, 边缘用 fade)
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0
            || sampleUV.y < 0.0 || sampleUV.y > 1.0) break;

        float sampleDepth = texture(uDepthTex, sampleUV).r;
        // 跳过天空盒采样
        if (sampleDepth >= 0.9999) continue;
        vec3 sampleVS = ReconstructViewPos(sampleUV, sampleDepth);

        // depthDiff > 0: ray 已穿过深度面前 (射线 z 小于 surface z, view space z 是负值)
        float depthDiff = sampleVS.z - samplePosVS.z;
        if (depthDiff > 0.0 && depthDiff < uThickness) {
            // 命中: 边缘 fade 平滑过渡
            float fade = smoothstep(0.0, uEdgeFade, sampleUV.x)
                       * smoothstep(0.0, uEdgeFade, 1.0 - sampleUV.x)
                       * smoothstep(0.0, uEdgeFade, sampleUV.y)
                       * smoothstep(0.0, uEdgeFade, 1.0 - sampleUV.y);
            hitColor = texture(uHDRTex, sampleUV).rgb;
            hitAlpha = fade;
            break;
        }
    }

    FragColor = vec4(hitColor, hitAlpha);
}
)";

// ---- FS_SSR_COMPOSITE (GLES 3.0): HDR += reflect.rgb * reflect.a * intensity ----
static const char* FS_SSR_COMPOSITE_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneTex;     // HDR color (临时拷贝, 解 feedback loop)
uniform sampler2D uReflectTex;   // SSR 反射 RT (RGBA16F, .a = fade weight)
uniform float     uIntensity;

void main() {
    vec3 hdr = texture(uSceneTex, vUV).rgb;
    vec4 ref = texture(uReflectTex, vUV);
    // additive: HDR + reflect.rgb * reflect.a (fade) * intensity
    FragColor = vec4(hdr + ref.rgb * ref.a * uIntensity, 1.0);
}
)";

// ---- FS_SSR_BLUR (GLES 3.0): Phase E.11 dual-mode (Gaussian / Bilateral) ----
// Phase E.10 baseline: separable Gaussian 5-tap, half-res ping-pong.
// Phase E.11 添加: depth-aware bilateral 权重门控 (单 shader 双 mode, runtime 切换).
//   axis=0 水平 / axis=1 垂直; uTexel = 1/dstSize (half-res);
//   uRadius 控制扩散半径 [0.5, 4.0] (texel 单位).
//   uBilateral=0 → 纯 Gaussian (Phase E.10 行为, 兼容);
//   uBilateral=1 → Bilateral, w_neighbor *= exp(-|cDepth-d|·uDepthSigma).
static const char* FS_SSR_BLUR_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrcTex;
uniform sampler2D uDepthTex;    // Phase E.11 (full-res, NEAREST)
uniform vec2  uTexel;           // 1.0 / vec2(dstW, dstH)
uniform int   uAxis;            // 0=H, 1=V
uniform float uRadius;          // [0.5, 4.0]
uniform int   uBilateral;       // Phase E.11: 0=Gaussian, 非 0=Bilateral
uniform float uDepthSigma;      // Phase E.11: [50, 500]

void main() {
    // 5-tap Gaussian (sigma ~= 1.6) weights, 中心 + ±1 + ±2
    const float W0 = 0.227027;
    const float W1 = 0.194594;
    const float W2 = 0.121622;

    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    vec2 off1 = dir * uRadius;
    vec2 off2 = dir * uRadius * 2.0;

    if (uBilateral == 0) {
        // Phase E.10 Gaussian (向后兼容)
        vec4 c = texture(uSrcTex, vUV) * W0;
        c += texture(uSrcTex, vUV + off1) * W1;
        c += texture(uSrcTex, vUV - off1) * W1;
        c += texture(uSrcTex, vUV + off2) * W2;
        c += texture(uSrcTex, vUV - off2) * W2;
        FragColor = c;
        return;
    }

    // Phase E.11 Bilateral 路径
    float cDepth = texture(uDepthTex, vUV).r;
    vec4  sum   = texture(uSrcTex, vUV) * W0;
    float wsum  = W0;

    vec2 uv;  float d, w;

    uv = vUV + off1;
    d  = texture(uDepthTex, uv).r;
    w  = W1 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    uv = vUV - off1;
    d  = texture(uDepthTex, uv).r;
    w  = W1 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    uv = vUV + off2;
    d  = texture(uDepthTex, uv).r;
    w  = W2 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    uv = vUV - off2;
    d  = texture(uDepthTex, uv).r;
    w  = W2 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    FragColor = sum / max(wsum, 1e-4);
}
)";

// ---- FS_SSR_TEMPORAL (GLES 3.0): Phase E.12 reverse-reprojection + neighborhood clip ----
//   uHasHistory=0 首帧 -> 输出 cur (不混合)
//   uRejectionMode=0 -> current-depth threshold rejection; 1 -> 9-tap neighborhood AABB clip
//   uReprojectMat = prevViewProj * invCurViewProj (CPU 预乘)
static const char* FS_SSR_TEMPORAL_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uCurReflectTex;  // 当前帧 SSR raw (slot 0)
uniform sampler2D uHistoryTex;     // 上一帧 temporal 输出 (slot 1)
uniform sampler2D uDepthTex;       // SSR depth (slot 2)
uniform sampler2D uVelocityTex;    // HDR velocity buffer (slot 3), currentUV - previousUV
uniform mat4  uReprojectMat;       // prevViewProj * invCurViewProj
uniform mat4  uInvProj;            // 备用 (当前 shader 内可不用)
uniform vec2  uTexel;              // 1.0 / RT 尺寸
uniform float uBlendAlpha;         // history 权重 [0.5, 0.99]
uniform int   uRejectionMode;      // 0 = current-depth threshold, 1 = neighborhood clip
uniform int   uHasHistory;         // 0 = 首帧, 1 = 累积
uniform int   uHasVelocityTex;     // 1 = 用 velocityTex, 0 = E.12 matrix fallback
// Phase E.14 — dilation + dual-format decode
uniform int   uVelocityDilation;   // 0 = 单点采样; 1 = 3x3 max-length 邻域 (几何边缘抗锯齿)
uniform int   uVelocityFormat;     // 0 = RG16F (raw); 1 = RG8 (encoded with uVelocityScale)
uniform float uVelocityScale;      // RG8 模式下的编码尺度; 默认 0.25

// Phase E.14 — 解码 velocity (RG16F 直返, RG8 还原回 [-scale, +scale])
vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

// Phase E.14 — 3x3 max-length dilation: 几何边缘取邻域最大速度, 抑制 1-px 错配伪影
vec2 SampleVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uVelocityTex, uv + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

void main() {
    vec4 cur = texture(uCurReflectTex, vUV);

    if (uHasHistory == 0) { FragColor = cur; return; }

    // ① reproject: velocity buffer 优先 (E.14: dilation 进场); 无 velocity 时沿用 E.12 matrix fallback
    float depth = texture(uDepthTex, vUV).r;
    vec2 prevUV;
    if (uHasVelocityTex == 1) {
        prevUV = vUV - SampleVelocityDilated(vUV);
    } else {
        vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec4 prevClip = uReprojectMat * ndc;
        float w = max(prevClip.w, 1e-6);
        prevUV = (prevClip.xy / w) * 0.5 + 0.5;
    }

    // 越界 reject
    if (prevUV.x < 0.0 || prevUV.x > 1.0 ||
        prevUV.y < 0.0 || prevUV.y > 1.0) {
        FragColor = cur;
        return;
    }

    if (uRejectionMode == 0) {
        float prevDepth = texture(uDepthTex, prevUV).r;
        if (abs(prevDepth - depth) > 0.002) {
            FragColor = cur;
            return;
        }
    }

    vec4 hist = texture(uHistoryTex, prevUV);

    // ② neighborhood AABB clip (mode=1)
    if (uRejectionMode == 1) {
        vec3 mn = cur.rgb;
        vec3 mx = cur.rgb;
        vec3 s;
        s = texture(uCurReflectTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 0.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 1.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2(-1.0,  0.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 1.0,  0.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2(-1.0,  1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 0.0,  1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 1.0,  1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        hist.rgb = clamp(hist.rgb, mn, mx);
    }

    // ③ blend
    float alpha = clamp(uBlendAlpha, 0.0, 1.0);
    FragColor = mix(cur, hist, alpha);
}
)";

// ---- FS_TAA (GLES 3.0): Phase F.0 — TAA 主管线 reproject + neighborhood AABB clip + alpha blend ----
//   与 SSR Temporal shader 同模式, 差异:
//     1. 输入是 cur HDR scene tex 而非 SSR reflect
//     2. 不依赖 depth (仅用 velocity 做 reproject)
//     3. 不走 matrix fallback (进入 TAA 路径要求 velocity buffer 必须存在)
static const char* FS_TAA_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uCurHdrTex;       // slot 0: 本帧 HDR scene (jittered raster 输出)
uniform sampler2D uHistoryTex;      // slot 1: 上帧 TAA 输出
uniform sampler2D uVelocityTex;     // slot 2: dilated 优先 / fallback raw velocity
uniform vec2  uTexel;               // 1.0 / (W, H)
uniform float uBlendAlpha;          // history 权重 [0.5, 0.99]
uniform int   uNeighborhoodClip;    // 0=纯 reproject+blend, 1=启用 9-tap AABB clip
uniform int   uHasHistory;          // 0=首帧 (输出 cur 不混合), 1=累积
uniform int   uVelocityDilation;    // 0=单点采 (E.18 dilated path), 1=inline 9-tap (fallback)
uniform int   uVelocityFormat;      // 0=RG16F, 1=RG8
uniform float uVelocityScale;       // RG8 decode
uniform int   uAntiFlicker;         // Phase F.0.4: 0=纯 alpha blend, 1=Karis luma-weighted blend
uniform int   uClipMode;            // Phase F.0.2/F.0.3: 0=RGB AABB, 1=YCoCg AABB, 2=YCoCg variance
uniform float uVarianceGamma;       // Phase F.0.3: variance clip 收紧系数 γ (Salvi 2016 / UE5 推荐 1.0, [0, 4])
uniform float uMotionGamma;         // Phase F.0.8: motion-adaptive 高速区域 γ (UE5 高级形式, 默认 1.5, [0, 4])
uniform int   uMotionAdaptiveGamma; // Phase F.0.8: 0=仅用 uVarianceGamma (F.0.3 行为), 1=按 velocity 长度 lerp 两 γ
uniform vec4  uUvBounds;            // Phase F.0.10.5: (uMin.xy, uMax.xy) clamp 邻域采样; 默认 (0,0,1,1) = 无 clamp

// Phase F.0.10.5 — 邻域采样 UV clamp helper (默认 0,0,1,1 时是恒等, 老路径零回归)
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }

// Phase F.0.2 — RGB ↔ YCoCg lift 形式转换 (整数可逆, 与 FXAA / Inside / UE5 标准一致)
//   Y  = 亮度通道 (0.25R + 0.5G + 0.25B); Co = R-B 色度; Cg = G - 0.5(R+B) 色度
//   AABB clip 在 YCoCg 空间执行: 亮度+色度独立约束, 对色彩边缘比 RGB 更鲁棒
vec3 RGBToYCoCg(vec3 c) {
    return vec3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r              - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 YCoCgToRGB(vec3 c) {
    return vec3(
        c.x + c.y - c.z,
        c.x       + c.z,
        c.x - c.y - c.z);
}

vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

vec2 SampleVelocity(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uVelocityTex, ClampUV(uv + vec2(float(dx), float(dy)) * uTexel)).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

void main() {
    vec4 cur = texture(uCurHdrTex, vUV);
    if (uHasHistory == 0) { FragColor = cur; return; }

    vec2 velocity = SampleVelocity(vUV);
    vec2 prevUV   = vUV - velocity;

    // Phase F.0.10.5: history reproject 出 region 边界 reject (默认 (0,0,1,1) = 全屏老行为)
    if (prevUV.x < uUvBounds.x || prevUV.x > uUvBounds.z ||
        prevUV.y < uUvBounds.y || prevUV.y > uUvBounds.w) {
        FragColor = cur;
        return;
    }

    vec4 hist = texture(uHistoryTex, ClampUV(prevUV));  // Phase F.0.10.5: 防御性 clamp

    if (uNeighborhoodClip == 1) {
        if (uClipMode == 2) {
            // Phase F.0.3 — Variance clipping in YCoCg space (Salvi 2016 / UE5 default)
            // m1 = mean(YCoCg(N9)); m2 = mean(YCoCg(N9)^2); sigma = sqrt(max(0, m2-m1^2)); clip = [m1-γσ, m1+γσ]
            // Phase F.0.10.5: 邻域采样 ClampUV 防跨 region 边界泄漏
            vec3 sum = vec3(0.0), sumSq = vec3(0.0);
            vec3 s;
            s = RGBToYCoCg(cur.rgb);                                                                sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0, -1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  0.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  0.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0,  1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb);     sum += s; sumSq += s*s;
            vec3 m1    = sum   * (1.0 / 9.0);
            vec3 m2    = sumSq * (1.0 / 9.0);
            vec3 sigma = sqrt(max(m2 - m1 * m1, vec3(0.0)));    // König-Huygens 公式, max(0) 防浮点负数
            // Phase F.0.8: motion-adaptive γ — 静止区域用 static γ (严防 ghost), 高速运动用 motion γ (宽容防 trail)
            // motionFactor = clamp(|vel| / (4 px UV), 0, 1); 0=静 → mix 返 static, 1=高速 → mix 返 motion
            float dynGamma = uVarianceGamma;
            if (uMotionAdaptiveGamma == 1) {
                float velLen = length(velocity);
                float motionFactor = clamp(velLen / (uTexel.x * 4.0), 0.0, 1.0);
                dynGamma = mix(uVarianceGamma, uMotionGamma, motionFactor);
            }
            vec3 mn    = m1 - dynGamma * sigma;
            vec3 mx    = m1 + dynGamma * sigma;
            vec3 histY = clamp(RGBToYCoCg(hist.rgb), mn, mx);
            hist.rgb   = YCoCgToRGB(histY);
        } else if (uClipMode == 1) {
            // Phase F.0.2 — YCoCg AABB clip 路径 (色彩边缘更鲁棒)
            // Phase F.0.10.5: 邻域采样 ClampUV 防跨 region 边界泄漏
            vec3 curY = RGBToYCoCg(cur.rgb);
            vec3 mn = curY;
            vec3 mx = curY;
            vec3 s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0, -1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  0.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  0.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0,  1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            vec3 histY = RGBToYCoCg(hist.rgb);
            histY = clamp(histY, mn, mx);
            hist.rgb = YCoCgToRGB(histY);
        } else {
            // F.0 RGB AABB clip 路径 (uClipMode==0 时严格复现, 零 ALU 回退)
            // Phase F.0.10.5: 邻域采样 ClampUV 防跨 region 边界泄漏
            vec3 mn = cur.rgb;
            vec3 mx = cur.rgb;
            vec3 s;
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0, -1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  0.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  0.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0,  1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            hist.rgb = clamp(hist.rgb, mn, mx);
        }
    }

    float alpha = clamp(uBlendAlpha, 0.0, 1.0);
    if (uAntiFlicker == 1) {
        // Phase F.0.4: Karis luma weighting (Brian Karis, UE4 2014 SIGGRAPH "High Quality Temporal Supersampling")
        // 高 luma 像素赋予较低权重, 压制 firefly 在时序累积中被放大的闪烁伪影.
        // Rec.709 luma 与 ACES tonemap 同基准.
        float lumaCur  = dot(cur.rgb,  vec3(0.2126, 0.7152, 0.0722));
        float lumaHist = dot(hist.rgb, vec3(0.2126, 0.7152, 0.0722));
        float wCur  = 1.0 / (1.0 + lumaCur);
        float wHist = 1.0 / (1.0 + lumaHist);
        float wc = wCur  * (1.0 - alpha);
        float wh = wHist * alpha;
        FragColor = vec4((cur.rgb * wc + hist.rgb * wh) / (wc + wh), 1.0);
    } else {
        // Phase F.0 原始 blend (低 luma 区域与 Karis 路径几乎同结果)
        FragColor = vec4(mix(cur.rgb, hist.rgb, alpha), 1.0);
    }
}
)";

// ---- FS_SHARPEN (GLES 3.0): Phase F.0.1 — TAA 后 4-tap unsharp mask 锐化补偿 ----
//   算法: sharpened = c + (c - avg4) × sharpness ; avg4 = (N+S+E+W) / 4
//   防 ringing: max(0); HDR 不 clamp 上限 (保留超亮)
//   性能: 5 fetch + 几 ALU/px ≈ 0.03 ms @ 1080p
static const char* FS_SHARPEN_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出 (history[writeIdx])
uniform vec2  uTexelSize;          // 1.0 / vec2(W, H)
uniform float uSharpness;          // [0, 2]
uniform vec4  uUvBounds;           // Phase F.0.10.5: (uMin.xy, uMax.xy); 默认 (0,0,1,1) = 无 clamp

vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }   // Phase F.0.10.5

void main() {
    vec3 c = texture(uInputTex, vUV).rgb;
    // 4-tap 上下左右邻域采样 (对角线舍弃, 业界主流 4-tap unsharp mask)
    // Phase F.0.10.5: ClampUV 防跨 region 边界泄漏
    vec3 n = texture(uInputTex, ClampUV(vUV + vec2(0.0,         uTexelSize.y))).rgb;
    vec3 s = texture(uInputTex, ClampUV(vUV - vec2(0.0,         uTexelSize.y))).rgb;
    vec3 e = texture(uInputTex, ClampUV(vUV + vec2(uTexelSize.x, 0.0       ))).rgb;
    vec3 w = texture(uInputTex, ClampUV(vUV - vec2(uTexelSize.x, 0.0       ))).rgb;
    vec3 avg4 = (n + s + e + w) * 0.25;
    // unsharp mask: c + (c - avg) × sharpness
    vec3 sharpened = c + (c - avg4) * uSharpness;
    // 防 ringing 下界: 黑色像素被锐化后不能为负 (HDR 上界不 clamp)
    FragColor = vec4(max(sharpened, vec3(0.0)), 1.0);
}
)";

// ---- FS_BICUBIC_UPSCALE (GLES 3.0): Phase F.0.9 — Catmull-Rom 5-tap bicubic 上采样 ----
//   适用场景: halfRes=true && sharpness=0 时, 用 bicubic 替代 bilinear stretch 上采样 history
//   算法: Sigggraph 2018 "Filmic SMAA Slidedeck" 5-tap (3x3 hardware bilinear) Catmull-Rom 优化
//   等效 16-tap Catmull-Rom bicubic, 仅用 9 sample (-44% bandwidth)
//   性能: ~0.03 ms @ 1080p (vs bilinear ~0.005ms, +0.025ms)
//   视觉: -50% blur vs bilinear, 适合高画质 4K 移动场景
static const char* FS_BICUBIC_UPSCALE_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: history half-res
uniform vec2  uTexel;              // 1.0 / (srcW, srcH) — src 分辨率纹素

// Catmull-Rom 5-tap (Sigggraph 2018 Filmic SMAA): 9 sample 等效 16-tap bicubic
vec4 SampleCatmullRom9(sampler2D tex, vec2 uv, vec2 texSize) {
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    // Catmull-Rom 卷积核权重 (per axis)
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    // 合并 w1+w2 = 一次 hardware bilinear 替代 (5-tap 优化关键)
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0  = texPos1 - 1.0;
    vec2 texPos3  = texPos1 + 2.0;
    vec2 texPos12 = texPos1 + offset12;

    // 归一化到 UV [0, 1]
    texPos0  = texPos0  / texSize;
    texPos3  = texPos3  / texSize;
    texPos12 = texPos12 / texSize;

    vec4 result = vec4(0.0);
    result += texture(tex, vec2(texPos0.x,  texPos0.y))  * (w0.x  * w0.y);
    result += texture(tex, vec2(texPos12.x, texPos0.y))  * (w12.x * w0.y);
    result += texture(tex, vec2(texPos3.x,  texPos0.y))  * (w3.x  * w0.y);

    result += texture(tex, vec2(texPos0.x,  texPos12.y)) * (w0.x  * w12.y);
    result += texture(tex, vec2(texPos12.x, texPos12.y)) * (w12.x * w12.y);
    result += texture(tex, vec2(texPos3.x,  texPos12.y)) * (w3.x  * w12.y);

    result += texture(tex, vec2(texPos0.x,  texPos3.y))  * (w0.x  * w3.y);
    result += texture(tex, vec2(texPos12.x, texPos3.y))  * (w12.x * w3.y);
    result += texture(tex, vec2(texPos3.x,  texPos3.y))  * (w3.x  * w3.y);

    return result;
}

void main() {
    // texSize = 1 / uTexel (uTexel = 1 / texSize)
    vec2 texSize = 1.0 / uTexel;
    vec4 col = SampleCatmullRom9(uInputTex, vUV, texSize);
    // HDR safe: 防 Catmull-Rom 负权重导致的 ringing 黑斑 (HDR 不截上限保留高光)
    FragColor = vec4(max(col.rgb, vec3(0.0)), 1.0);
}
)";

// ---- FS_LANCZOS_UPSCALE (GLES 3.0): Phase F.0.14 — Lanczos-2 25-tap 5x5 上采样 ----
//   适用场景: halfRes=true && sharpness=0 && upscaleMode==2 时, 用 Lanczos-2 替代 Catmull-Rom (F.0.9)
//   算法: Lanczos kernel L(x) = sinc(x) * sinc(x/2) for |x|<2 else 0; 25 tap (5x5) unrolled
//   性能: ~0.07 ms @ 1080p (vs Catmull-Rom ~0.03 ms, +0.04 ms; vs bilinear ~0.005 ms)
//   视觉: -10% blur vs Catmull-Rom (-55% vs bilinear), 适合 4K/桌面 GPU 超高画质
//   归一化: wsum 除法处理 numerical drift (Lanczos sum_w 理论=1, 数值 ~0.99-1.01)
static const char* FS_LANCZOS_UPSCALE_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: history half-res
uniform vec2  uTexel;              // 1.0 / (srcW, srcH) — src 分辨率纹素

// Lanczos-2: L(x) = sinc(x) * sinc(x/2) for |x| < 2 else 0; sinc(0) = 1
float lanczos(float x) {
    if (abs(x) < 1.0e-5) return 1.0;
    if (abs(x) >= 2.0) return 0.0;
    float pix = 3.141592653589793 * x;
    return (sin(pix) * sin(pix * 0.5)) / (pix * pix * 0.5);
}

void main() {
    // 在 src 像素空间的连续坐标 (vUV / uTexel = vUV * srcSize)
    vec2 srcCoord = vUV / uTexel;
    vec2 srcInt   = floor(srcCoord - 0.5) + 0.5;   // 中心 src 像素的整数中心
    vec2 frac     = srcCoord - srcInt;             // (-0.5, +0.5) sub-pixel offset

    vec3 sum  = vec3(0.0);
    float wsum = 0.0;

    // 5x5 Lanczos-2 kernel (i, j ∈ [-2, 2]); GLSL const-bound for 必展开
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec2 sp = (srcInt + vec2(float(i), float(j))) * uTexel;
            vec2 d  = vec2(float(i), float(j)) - frac;
            float w = lanczos(d.x) * lanczos(d.y);
            sum  += texture(uInputTex, sp).rgb * w;
            wsum += w;
        }
    }
    // wsum 归一化 (Lanczos 不严格 = 1, 数值 drift 修正); HDR safe: max(0) 防负 ringing
    FragColor = vec4(max(sum / max(wsum, 1.0e-4), vec3(0.0)), 1.0);
}
)";

// ---- FS_CAS (GLES 3.0): Phase F.0.6 — TAA 后 5-tap CAS 锐化 (AMD FidelityFX FSR1) ----
//   算法: contrast-adaptive sharpening, 邻域 min/max 计算 dynamic range 自动减弱低对比
//   优势 vs F.0.1 unsharp: 平滑区域不锁牰 (无噪点放大) + HDR firefly 友好 + perceptual gamma
//   性能: 5 fetch + ~12 ALU/px ≈ 0.05 ms @ 1080p (+0.02 ms vs F.0.1)
//   sharpness ∈ [0, 1] (FSR1 标准): 0 → peak=-1/8 弱锐化, 1 → peak=-1/5 强锐化
static const char* FS_CAS_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出
uniform vec2  uTexelSize;          // 1.0 / vec2(W, H)
uniform float uSharpness;          // CAS [0, 1]

void main() {
    vec3 c = texture(uInputTex, vUV).rgb;
    vec3 n = texture(uInputTex, vUV + vec2(0.0,         uTexelSize.y)).rgb;
    vec3 s = texture(uInputTex, vUV - vec2(0.0,         uTexelSize.y)).rgb;
    vec3 e = texture(uInputTex, vUV + vec2(uTexelSize.x, 0.0       )).rgb;
    vec3 w = texture(uInputTex, vUV - vec2(uTexelSize.x, 0.0       )).rgb;

    // 邻域 min/max (per channel, 5-tap)
    vec3 mnRGB = min(c, min(n, min(s, min(e, w))));
    vec3 mxRGB = max(c, max(n, max(s, max(e, w))));

    // 对暗部和亮部对称的 dynamic range (FSR1 trick)
    vec3 mnRGB2 = min(mnRGB, 1.0 - mxRGB);
    vec3 ampRGB = clamp(min(mnRGB, mnRGB2) / max(mxRGB, vec3(1e-4)), 0.0, 1.0);
    ampRGB = sqrt(ampRGB);                       // perceptual gamma

    // peak 系数: sharpness=0 → peak=-1/8 (弱), sharpness=1 → peak=-1/5 (强)
    float peak = -1.0 / mix(8.0, 5.0, uSharpness);
    vec3 wRGB  = ampRGB * peak;
    vec3 rcpW  = 1.0 / (4.0 * wRGB + 1.0);

    vec3 sum = c + (n + s + e + w) * wRGB;
    vec3 sharpened = sum * rcpW;

    // HDR safe: 防黑斑负值 (HDR 不截上限保留高光; ChocoLight 是 HDR pipeline)
    FragColor = vec4(max(sharpened, vec3(0.0)), 1.0);
}
)";

// ---- FS_RCAS (GLES 3.0): Phase F.0.12 — TAA 后 5-tap RCAS 锐化 (AMD FidelityFX FSR2) ----
//   Robust CAS: 在 F.0.6 CAS 基础上加 noise detection + edge protection
//   - noise detection: range < 1/64 时跳过 (避免放大 sensor noise)
//   - edge protection: lobe = sqrt(min(eL-mn, mx-eL) / range), edges 处 lobe 小不 over-sharpen
//   性能: 5 fetch + ~22 ALU/px (vs CAS ~12), +0.03 ms @ 1080p
//   sharpness ∈ [0, 2] (FSR2 标准): 0 → peak=-1/16 (弱), 2 → peak=-1/4 (强)
static const char* FS_RCAS_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出
uniform vec2  uTexelSize;          // 1.0 / vec2(W, H)
uniform float uSharpness;          // RCAS [0, 2]

// FSR2 推荐 noise threshold: 平衡 noise rejection 与小细节保留
const float kNoiseThreshold = 1.0 / 64.0;

void main() {
    vec3 e = texture(uInputTex, vUV).rgb;                                    // 中心
    vec3 b = texture(uInputTex, vUV + vec2(0.0,         uTexelSize.y)).rgb;  // 上
    vec3 h = texture(uInputTex, vUV - vec2(0.0,         uTexelSize.y)).rgb;  // 下
    vec3 d = texture(uInputTex, vUV + vec2(uTexelSize.x, 0.0       )).rgb;  // 右
    vec3 f = texture(uInputTex, vUV - vec2(uTexelSize.x, 0.0       )).rgb;  // 左

    // luma 提取: G channel as proxy (FSR2 优化, 比 0.299R+0.587G+0.114B 快 ~3 ALU)
    float bL = b.g, dL = d.g, eL = e.g, fL = f.g, hL = h.g;

    // 4-tap 邻域 min/max (排除中心, 用于 edge detection)
    float mn4 = min(min(bL, dL), min(fL, hL));
    float mx4 = max(max(bL, dL), max(fL, hL));
    float range = mx4 - mn4;

    // Noise detection: range 太小 → smooth 区域, 跳过 sharpen 避免放大 sensor noise
    if (range < kNoiseThreshold) {
        FragColor = vec4(e, 1.0);
        return;
    }

    // Edge protection: lobe ∈ [0, 0.5], edges 处 (eL≈mn4 或 eL≈mx4) lobe→0 不 over-sharpen
    float lobe = max(min(eL - mn4, mx4 - eL), 0.0);
    lobe = sqrt(lobe / max(range, 1e-4));

    // peak: sharpness=0 → peak=-1/16 (弱), sharpness=2 → peak=-1/4 (强)
    float peak = -1.0 / mix(16.0, 4.0, uSharpness * 0.5);
    float wgt  = peak * lobe;

    // Final composite (与 CAS 同公式): result = (e + 4 邻域 × wgt) / (1 + 4 wgt)
    vec3 sum    = e + (b + d + f + h) * wgt;
    vec3 result = sum / (1.0 + 4.0 * wgt);

    // HDR safe (FSR2 标准): clamp ≥ 0 防黑斑, 不截上限保留高光
    FragColor = vec4(max(result, vec3(0.0)), 1.0);
}
)";

// ---- FS_MOTION_BLUR (GLES 3.0): Phase E.15 per-pixel velocity blur ----
//   1. SampleVelocityDilated 与 SSRTemporal 同算法 (3x3 max-length 邻域可选)
//   2. E3 软限: |vel| <= screenDiagUV × 0.3 ≈ 0.4243 防极端拖尾糊死
//   3. 沿 -vel × strength 均匀 N 采样, N ∈ [1, 32]
static const char* FS_MOTION_BLUR_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneTex;       // HDR + 所有后处理 (Bloom/SSR/LensFlare 累积)
uniform sampler2D uVelocityTex;        // combined velocity (slot 1)
uniform sampler2D uCameraVelocityTex;  // Phase E.16: camera-only velocity (slot 2)
uniform vec2  uTexel;              // 1.0 / vec2(W, H)
uniform float uStrength;           // 用户调 [0, 4]
uniform int   uSampleCount;        // [1, 32]
uniform int   uVelocityDilation;   // 0=单点 / 1=3x3 max-length
uniform int   uVelocityFormat;     // 0=RG16F / 1=RG8
uniform float uVelocityScale;      // RG8 解码 scale (默认 0.25)
uniform int   uMode;               // Phase E.16: 0=combined / 1=camera_only / 2=object_only

vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

vec2 SampleVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uVelocityTex, uv + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

// Phase E.16: 与 SampleVelocityDilated 同算法，切换采 cameraVelocityTex
// GLES3 不支持 sampler 作函数参数，故复制一份
//   mode=1: 单独采此量，mode=2: combined - camera = object_only
vec2 SampleCameraVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uCameraVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uCameraVelocityTex, uv + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

void main() {
    // Phase E.16 — 按 mode 选 velocity 源:
    //   0=combined (camera+object 合一)
    //   1=camera_only (仅相机运动，物体独立于序列)
    //   2=object_only (v_combined - v_camera 近似物体独立运动)
    vec2 vel;
    if (uMode == 1) {
        vel = SampleCameraVelocityDilated(vUV);
    } else if (uMode == 2) {
        vel = SampleVelocityDilated(vUV) - SampleCameraVelocityDilated(vUV);
    } else {
        vel = SampleVelocityDilated(vUV);
    }
    vel *= uStrength;

    // E3 软限: max blur UV = 屏幕对角线 × 0.3 (sqrt(2) * 0.3 ≈ 0.4243)
    const float kMaxBlurUV = 0.4243;
    float velLen = length(vel);
    if (velLen > kMaxBlurUV) vel *= (kMaxBlurUV / velLen);

    int   count    = clamp(uSampleCount, 1, 32);
    float countInv = 1.0 / float(max(count - 1, 1));
    vec3  sum      = vec3(0.0);
    const int kMaxSamples = 32;
    for (int i = 0; i < kMaxSamples; ++i) {
        if (i >= count) break;
        float t = float(i) * countInv;            // [0, 1]
        vec2  uv = vUV - vel * t;                  // 朝过去帧方向采样
        sum += texture(uSceneTex, uv).rgb;
    }
    FragColor = vec4(sum / float(count), 1.0);
}
)";

// ---- FS_VELOCITY_DILATE (GLES 3.0): Phase E.18 独立 velocity dilation pass ----
//   1. 输入 raw velocityTex (RG16F 或 RG8 encoded, 由 uVelocityFormat 控制)
//   2. 9-tap max-length 邻域: 取 (-1,-1)..(1,1) 9 个像素中 length² 最大的 velocity
//   3. 输出 dilatedTex (RG16F, 始终 decode 后的 float, 后续 consumer 单点采可直读)
//   4. 算法与 FS_SSR_TEMPORAL / FS_MOTION_BLUR 内的 SampleVelocityDilated 完全等价
//      (将 inline 9-tap 抽出为独立 pass, 多消费者场景下避免重复计算)
static const char* FS_VELOCITY_DILATE_SOURCE = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrcVelocityTex;   // raw velocity (RG16F 或 RG8 encoded), slot 0
uniform vec2  uTexel;                // 1.0 / vec2(W, H) — full-res, 决定 9-tap 物理覆盖范围
uniform int   uVelocityFormat;       // 0 = RG16F (raw直存); 1 = RG8 (encoded)
uniform float uVelocityScale;        // RG8 解码 scale (默认 0.25)

// 与 FS_SSR_TEMPORAL / FS_MOTION_BLUR 完全一致的 decode 函数
vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

void main() {
    // 9-tap max-length dilation: 几何边缘取邻域最大速度, 抑制 1-px 错配伪影
    // 注意: 输出永远为 decode 后的 float (RG16F), shader 后端 consumer 单点读不再 decode
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(
                texture(uSrcVelocityTex, vUV + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    // dilatedTex 始终 RG16F: .rg = 已 decode 的 float velocity, .b/.a 无意义置 0/1
    FragColor = vec4(bestV, 0.0, 1.0);
}
)";

#else  // 桌面 GL 3.3 Core

// ---- FS_SSAO (GL 3.3): 同 GLES3 算法 ----
static const char* FS_SSAO_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepthTex;
uniform sampler2D uNoiseTex;
uniform sampler2D uNormalTex;    // Phase E.8.x: G-buffer view-space normal (RG16F)
uniform mat4  uProj;
uniform mat4  uInvProj;
uniform vec3  uKernel[16];
uniform int   uKernelSize;
uniform float uRadius;
uniform float uBias;
uniform float uPower;
uniform vec2  uNoiseScale;

vec3 ReconstructViewPos(vec2 uv, float d) {
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v    = uInvProj * clip;
    return v.xyz / v.w;
}

// Phase E.8.x: 从 RG16F G-buffer 解码 view-space normal
vec3 DecodeViewNormal(vec2 enc) {
    vec2 nxy = enc * 2.0 - 1.0;
    float zsq = max(0.0, 1.0 - dot(nxy, nxy));
    return vec3(nxy, sqrt(zsq));
}

void main() {
    float d = texture(uDepthTex, vUV).r;
    if (d >= 0.9999) { FragColor = vec4(1.0); return; }

    vec3 P = ReconstructViewPos(vUV, d);
    vec3 N = DecodeViewNormal(texture(uNormalTex, vUV).rg);
    vec3 R = texture(uNoiseTex, vUV * uNoiseScale).xyz * 2.0 - 1.0;
    R = normalize(vec3(R.xy, 0.0));
    vec3 T = normalize(R - N * dot(R, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < 16; i++) {
        if (i >= uKernelSize) break;
        vec3 samp = TBN * uKernel[i];
        samp = P + samp * uRadius;

        vec4 proj = uProj * vec4(samp, 1.0);
        proj.xyz /= proj.w;
        vec2 sampUV = proj.xy * 0.5 + 0.5;

        float sampDepth = texture(uDepthTex, sampUV).r;
        vec3  sampP     = ReconstructViewPos(sampUV, sampDepth);

        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sampP.z), 1e-4));
        occlusion += (sampP.z >= samp.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(uKernelSize);
    ao = pow(ao, uPower);
    FragColor = vec4(ao, ao, ao, 1.0);
}
)";

// ---- FS_SSAO_BLUR (GL 3.3) ----
static const char* FS_SSAO_BLUR_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSSAOTex;
uniform sampler2D uDepthTex;
uniform vec2      uTexel;
uniform int       uAxis;

void main() {
    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    float cDepth = texture(uDepthTex, vUV).r;
    float sum = 0.0, wsum = 0.0;
    for (int i = -2; i <= 2; i++) {
        vec2 uv = vUV + dir * float(i);
        float ao = texture(uSSAOTex, uv).r;
        float d  = texture(uDepthTex, uv).r;
        float w  = exp(-abs(cDepth - d) * 200.0);
        sum  += ao * w;
        wsum += w;
    }
    float o = sum / max(wsum, 1e-4);
    FragColor = vec4(o, o, o, 1.0);
}
)";

// ---- FS_SSAO_COMPOSITE (GL 3.3) ----
static const char* FS_SSAO_COMPOSITE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneTex;
uniform sampler2D uAOTex;
uniform float     uIntensity;

void main() {
    vec3 hdr = texture(uSceneTex, vUV).rgb;
    float ao = texture(uAOTex, vUV).r;
    float o = mix(1.0, ao, uIntensity);
    FragColor = vec4(hdr * o, 1.0);
}
)";

// ==================== Phase E.9 — SSR shaders (GL 3.3) ====================

// ---- FS_SSR (GL 3.3): linear ray march in view space ----
static const char* FS_SSR_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepthTex;
uniform sampler2D uNormalTex;    // Phase E.8.x: G-buffer view-space normal (RG16F)
uniform sampler2D uHDRTex;
uniform mat4  uProj;
uniform mat4  uInvProj;
uniform int   uMaxSteps;
uniform float uStepSize;
uniform float uThickness;
uniform float uMaxDistance;
uniform float uEdgeFade;
uniform vec2  uJitterOffset;     // Phase E.12: UV 空间 jitter (±0.5/W, ±0.5/H), TAA-style

vec3 ReconstructViewPos(vec2 uv, float d) {
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v    = uInvProj * clip;
    return v.xyz / v.w;
}

vec3 DecodeViewNormal(vec2 enc) {
    vec2 nxy = enc * 2.0 - 1.0;
    float zsq = max(0.0, 1.0 - dot(nxy, nxy));
    return vec3(nxy, sqrt(zsq));
}

void main() {
    // Phase E.12: ray march 起点 jitter (±0.5 pixel), Temporal accumulation 配套
    vec2 uvJ = vUV + uJitterOffset;
    float d = texture(uDepthTex, uvJ).r;
    if (d >= 0.9999) { FragColor = vec4(0.0); return; }

    vec3 viewPos = ReconstructViewPos(uvJ, d);
    vec3 viewN   = DecodeViewNormal(texture(uNormalTex, uvJ).rg);
    vec3 viewV   = normalize(-viewPos);

    if (dot(viewN, viewV) < 0.05) { FragColor = vec4(0.0); return; }

    vec3 viewR = reflect(-viewV, viewN);

    vec3 hitColor   = vec3(0.0);
    float hitAlpha  = 0.0;

    for (int i = 1; i <= 128; ++i) {
        if (i > uMaxSteps) break;

        vec3 samplePosVS = viewPos + viewR * (uStepSize * float(i));
        if (-samplePosVS.z > uMaxDistance) break;

        vec4 sampleClip = uProj * vec4(samplePosVS, 1.0);
        if (sampleClip.w <= 0.0) break;
        vec2 sampleUV   = sampleClip.xy / sampleClip.w * 0.5 + 0.5;

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0
            || sampleUV.y < 0.0 || sampleUV.y > 1.0) break;

        float sampleDepth = texture(uDepthTex, sampleUV).r;
        if (sampleDepth >= 0.9999) continue;
        vec3 sampleVS = ReconstructViewPos(sampleUV, sampleDepth);

        float depthDiff = sampleVS.z - samplePosVS.z;
        if (depthDiff > 0.0 && depthDiff < uThickness) {
            float fade = smoothstep(0.0, uEdgeFade, sampleUV.x)
                       * smoothstep(0.0, uEdgeFade, 1.0 - sampleUV.x)
                       * smoothstep(0.0, uEdgeFade, sampleUV.y)
                       * smoothstep(0.0, uEdgeFade, 1.0 - sampleUV.y);
            hitColor = texture(uHDRTex, sampleUV).rgb;
            hitAlpha = fade;
            break;
        }
    }

    FragColor = vec4(hitColor, hitAlpha);
}
)";

// ---- FS_SSR_COMPOSITE (GL 3.3): HDR += reflect.rgb * reflect.a * intensity ----
static const char* FS_SSR_COMPOSITE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneTex;
uniform sampler2D uReflectTex;
uniform float     uIntensity;

void main() {
    vec3 hdr = texture(uSceneTex, vUV).rgb;
    vec4 ref = texture(uReflectTex, vUV);
    FragColor = vec4(hdr + ref.rgb * ref.a * uIntensity, 1.0);
}
)";

// ---- FS_SSR_BLUR (GL 3.3): Phase E.11 dual-mode (Gaussian / Bilateral) ----
//   uBilateral = 0 → Phase E.10 纯 Gaussian (向后兼容)
//   uBilateral = 1 → Phase E.11 depth-aware Bilateral
//                     w_neighbor = W_i × exp(-|cDepth - d| × uDepthSigma)
static const char* FS_SSR_BLUR_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrcTex;
uniform sampler2D uDepthTex;    // Phase E.11 (full-res, NEAREST)
uniform vec2  uTexel;
uniform int   uAxis;
uniform float uRadius;
uniform int   uBilateral;       // Phase E.11: 0=Gaussian, 非 0=Bilateral
uniform float uDepthSigma;      // Phase E.11: [50, 500]

void main() {
    const float W0 = 0.227027;
    const float W1 = 0.194594;
    const float W2 = 0.121622;

    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    vec2 off1 = dir * uRadius;
    vec2 off2 = dir * uRadius * 2.0;

    if (uBilateral == 0) {
        // Phase E.10 Gaussian (向后兼容)
        vec4 c = texture(uSrcTex, vUV) * W0;
        c += texture(uSrcTex, vUV + off1) * W1;
        c += texture(uSrcTex, vUV - off1) * W1;
        c += texture(uSrcTex, vUV + off2) * W2;
        c += texture(uSrcTex, vUV - off2) * W2;
        FragColor = c;
        return;
    }

    // Phase E.11 Bilateral 路径
    float cDepth = texture(uDepthTex, vUV).r;
    vec4  sum   = texture(uSrcTex, vUV) * W0;
    float wsum  = W0;

    vec2 uv;  float d, w;

    uv = vUV + off1;
    d  = texture(uDepthTex, uv).r;
    w  = W1 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    uv = vUV - off1;
    d  = texture(uDepthTex, uv).r;
    w  = W1 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    uv = vUV + off2;
    d  = texture(uDepthTex, uv).r;
    w  = W2 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    uv = vUV - off2;
    d  = texture(uDepthTex, uv).r;
    w  = W2 * exp(-abs(cDepth - d) * uDepthSigma);
    sum += texture(uSrcTex, uv) * w; wsum += w;

    FragColor = sum / max(wsum, 1e-4);
}
)";

// ---- FS_SSR_TEMPORAL (GL 3.3): Phase E.12 reverse-reprojection + neighborhood clip ----
//   uHasHistory=0 首帧 -> 输出 cur (不混合)
//   uRejectionMode=0 -> current-depth threshold rejection; 1 -> 9-tap neighborhood AABB clip
//   uReprojectMat = prevViewProj * invCurViewProj (CPU 预乘)
static const char* FS_SSR_TEMPORAL_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uCurReflectTex;
uniform sampler2D uHistoryTex;
uniform sampler2D uDepthTex;
uniform sampler2D uVelocityTex;
uniform mat4  uReprojectMat;
uniform mat4  uInvProj;
uniform vec2  uTexel;
uniform float uBlendAlpha;
uniform int   uRejectionMode;
uniform int   uHasHistory;
uniform int   uHasVelocityTex;
// Phase E.14 — dilation + dual-format decode
uniform int   uVelocityDilation;   // 0 = 单点采样; 1 = 3x3 max-length 邻域
uniform int   uVelocityFormat;     // 0 = RG16F (raw); 1 = RG8 (encoded with uVelocityScale)
uniform float uVelocityScale;      // RG8 模式编码尺度; 默认 0.25

vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

vec2 SampleVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uVelocityTex, uv + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

void main() {
    vec4 cur = texture(uCurReflectTex, vUV);

    if (uHasHistory == 0) { FragColor = cur; return; }

    float depth = texture(uDepthTex, vUV).r;
    vec2 prevUV;
    if (uHasVelocityTex == 1) {
        prevUV = vUV - SampleVelocityDilated(vUV);
    } else {
        vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec4 prevClip = uReprojectMat * ndc;
        float w = max(prevClip.w, 1e-6);
        prevUV = (prevClip.xy / w) * 0.5 + 0.5;
    }

    if (prevUV.x < 0.0 || prevUV.x > 1.0 ||
        prevUV.y < 0.0 || prevUV.y > 1.0) {
        FragColor = cur;
        return;
    }

    if (uRejectionMode == 0) {
        float prevDepth = texture(uDepthTex, prevUV).r;
        if (abs(prevDepth - depth) > 0.002) {
            FragColor = cur;
            return;
        }
    }

    vec4 hist = texture(uHistoryTex, prevUV);

    if (uRejectionMode == 1) {
        vec3 mn = cur.rgb;
        vec3 mx = cur.rgb;
        vec3 s;
        s = texture(uCurReflectTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 0.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 1.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2(-1.0,  0.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 1.0,  0.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2(-1.0,  1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 0.0,  1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        s = texture(uCurReflectTex, vUV + uTexel * vec2( 1.0,  1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
        hist.rgb = clamp(hist.rgb, mn, mx);
    }

    float alpha = clamp(uBlendAlpha, 0.0, 1.0);
    FragColor = mix(cur, hist, alpha);
}
)";

// ---- FS_TAA (GL 3.3): Phase F.0 — 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier ----
static const char* FS_TAA_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uCurHdrTex;       // slot 0: 本帧 HDR scene (jittered raster 输出)
uniform sampler2D uHistoryTex;      // slot 1: 上帧 TAA 输出
uniform sampler2D uVelocityTex;     // slot 2: dilated 优先 / fallback raw velocity
uniform vec2  uTexel;
uniform float uBlendAlpha;
uniform int   uNeighborhoodClip;
uniform int   uHasHistory;
uniform int   uVelocityDilation;
uniform int   uVelocityFormat;
uniform float uVelocityScale;
uniform int   uAntiFlicker;         // Phase F.0.4: 0=纯 alpha blend, 1=Karis luma-weighted blend
uniform int   uClipMode;            // Phase F.0.2/F.0.3: 0=RGB AABB, 1=YCoCg AABB, 2=YCoCg variance
uniform float uVarianceGamma;       // Phase F.0.3: variance clip 收紧系数 γ (Salvi 2016 / UE5 推荐 1.0, [0, 4])
uniform vec4  uUvBounds;            // Phase F.0.10.5: (uMin.xy, uMax.xy) clamp 邻域采样; 默认 (0,0,1,1) = 无 clamp

// Phase F.0.10.5 — 邻域采样 UV clamp helper (默认 0,0,1,1 时是恒等, 老路径零回归)
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }

// Phase F.0.2 — RGB ↔ YCoCg lift 形式转换 (与 GLES3 版完全等价)
vec3 RGBToYCoCg(vec3 c) {
    return vec3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r              - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 YCoCgToRGB(vec3 c) {
    return vec3(
        c.x + c.y - c.z,
        c.x       + c.z,
        c.x - c.y - c.z);
}

vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

vec2 SampleVelocity(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uVelocityTex, ClampUV(uv + vec2(float(dx), float(dy)) * uTexel)).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

void main() {
    vec4 cur = texture(uCurHdrTex, vUV);
    if (uHasHistory == 0) { FragColor = cur; return; }

    vec2 velocity = SampleVelocity(vUV);
    vec2 prevUV   = vUV - velocity;

    // Phase F.0.10.5: history reproject 出 region 边界 reject (默认 (0,0,1,1) = 全屏老行为)
    if (prevUV.x < uUvBounds.x || prevUV.x > uUvBounds.z ||
        prevUV.y < uUvBounds.y || prevUV.y > uUvBounds.w) {
        FragColor = cur;
        return;
    }

    vec4 hist = texture(uHistoryTex, ClampUV(prevUV));  // Phase F.0.10.5: 防御性 clamp

    if (uNeighborhoodClip == 1) {
        if (uClipMode == 2) {
            // Phase F.0.3 — Variance clipping in YCoCg space (Salvi 2016 / UE5 default)
            // 与 GLES3 版完全等价; Phase F.0.10.5: 邻域采样 ClampUV 防跨 region 边界泄漏
            vec3 sum = vec3(0.0), sumSq = vec3(0.0);
            vec3 s;
            s = RGBToYCoCg(cur.rgb);                                                                sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0, -1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  0.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  0.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0,  1.0))).rgb);     sum += s; sumSq += s*s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb);     sum += s; sumSq += s*s;
            vec3 m1    = sum   * (1.0 / 9.0);
            vec3 m2    = sumSq * (1.0 / 9.0);
            vec3 sigma = sqrt(max(m2 - m1 * m1, vec3(0.0)));
            vec3 mn    = m1 - uVarianceGamma * sigma;
            vec3 mx    = m1 + uVarianceGamma * sigma;
            vec3 histY = clamp(RGBToYCoCg(hist.rgb), mn, mx);
            hist.rgb   = YCoCgToRGB(histY);
        } else if (uClipMode == 1) {
            // Phase F.0.2 — YCoCg AABB clip 路径 (色彩边缘更鲁棒)
            // Phase F.0.10.5: 邻域采样 ClampUV 防跨 region 边界泄漏
            vec3 curY = RGBToYCoCg(cur.rgb);
            vec3 mn = curY;
            vec3 mx = curY;
            vec3 s;
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0, -1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  0.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  0.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0,  1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            s = RGBToYCoCg(texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb); mn = min(mn, s); mx = max(mx, s);
            vec3 histY = RGBToYCoCg(hist.rgb);
            histY = clamp(histY, mn, mx);
            hist.rgb = YCoCgToRGB(histY);
        } else {
            // F.0 RGB AABB clip 路径 (uClipMode==0 时严格复现, 零 ALU 回退)
            // Phase F.0.10.5: 邻域采样 ClampUV 防跨 region 边界泄漏
            vec3 mn = cur.rgb;
            vec3 mx = cur.rgb;
            vec3 s;
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0, -1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0, -1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  0.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  0.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0,  1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 0.0,  1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2( 1.0,  1.0))).rgb; mn = min(mn, s); mx = max(mx, s);
            hist.rgb = clamp(hist.rgb, mn, mx);
        }
    }

    float alpha = clamp(uBlendAlpha, 0.0, 1.0);
    if (uAntiFlicker == 1) {
        // Phase F.0.4: Karis luma weighting — 高 luma 像素降权重压制 firefly 闪烁
        float lumaCur  = dot(cur.rgb,  vec3(0.2126, 0.7152, 0.0722));
        float lumaHist = dot(hist.rgb, vec3(0.2126, 0.7152, 0.0722));
        float wCur  = 1.0 / (1.0 + lumaCur);
        float wHist = 1.0 / (1.0 + lumaHist);
        float wc = wCur  * (1.0 - alpha);
        float wh = wHist * alpha;
        FragColor = vec4((cur.rgb * wc + hist.rgb * wh) / (wc + wh), 1.0);
    } else {
        FragColor = vec4(mix(cur.rgb, hist.rgb, alpha), 1.0);
    }
}
)";

// ---- FS_SHARPEN (GL 3.3): Phase F.0.1 — TAA 后 4-tap unsharp mask 锐化补偿 ----
// 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier
static const char* FS_SHARPEN_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出 (history[writeIdx])
uniform vec2  uTexelSize;          // 1.0 / vec2(W, H)
uniform float uSharpness;          // [0, 2]
uniform vec4  uUvBounds;           // Phase F.0.10.5: (uMin.xy, uMax.xy); 默认 (0,0,1,1) = 无 clamp

vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }   // Phase F.0.10.5

void main() {
    vec3 c = texture(uInputTex, vUV).rgb;
    // 4-tap 上下左右邻域采样 (对角线舍弃, 业界主流 4-tap unsharp mask)
    // Phase F.0.10.5: ClampUV 防跨 region 边界泄漏
    vec3 n = texture(uInputTex, ClampUV(vUV + vec2(0.0,         uTexelSize.y))).rgb;
    vec3 s = texture(uInputTex, ClampUV(vUV - vec2(0.0,         uTexelSize.y))).rgb;
    vec3 e = texture(uInputTex, ClampUV(vUV + vec2(uTexelSize.x, 0.0       ))).rgb;
    vec3 w = texture(uInputTex, ClampUV(vUV - vec2(uTexelSize.x, 0.0       ))).rgb;
    vec3 avg4 = (n + s + e + w) * 0.25;
    // unsharp mask: c + (c - avg) × sharpness
    vec3 sharpened = c + (c - avg4) * uSharpness;
    // 防 ringing 下界: 黑色像素被锐化后不能为负 (HDR 上界不 clamp)
    FragColor = vec4(max(sharpened, vec3(0.0)), 1.0);
}
)";

// ---- FS_BICUBIC_UPSCALE (GL 3.3): Phase F.0.9 — Catmull-Rom 5-tap bicubic 上采样 ----
// 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier
static const char* FS_BICUBIC_UPSCALE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;
uniform vec2  uTexel;

vec4 SampleCatmullRom9(sampler2D tex, vec2 uv, vec2 texSize) {
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0  = texPos1 - 1.0;
    vec2 texPos3  = texPos1 + 2.0;
    vec2 texPos12 = texPos1 + offset12;

    texPos0  = texPos0  / texSize;
    texPos3  = texPos3  / texSize;
    texPos12 = texPos12 / texSize;

    vec4 result = vec4(0.0);
    result += texture(tex, vec2(texPos0.x,  texPos0.y))  * (w0.x  * w0.y);
    result += texture(tex, vec2(texPos12.x, texPos0.y))  * (w12.x * w0.y);
    result += texture(tex, vec2(texPos3.x,  texPos0.y))  * (w3.x  * w0.y);

    result += texture(tex, vec2(texPos0.x,  texPos12.y)) * (w0.x  * w12.y);
    result += texture(tex, vec2(texPos12.x, texPos12.y)) * (w12.x * w12.y);
    result += texture(tex, vec2(texPos3.x,  texPos12.y)) * (w3.x  * w12.y);

    result += texture(tex, vec2(texPos0.x,  texPos3.y))  * (w0.x  * w3.y);
    result += texture(tex, vec2(texPos12.x, texPos3.y))  * (w12.x * w3.y);
    result += texture(tex, vec2(texPos3.x,  texPos3.y))  * (w3.x  * w3.y);

    return result;
}

void main() {
    vec2 texSize = 1.0 / uTexel;
    vec4 col = SampleCatmullRom9(uInputTex, vUV, texSize);
    FragColor = vec4(max(col.rgb, vec3(0.0)), 1.0);
}
)";

// ---- FS_LANCZOS_UPSCALE (GL 3.3): Phase F.0.14 — Lanczos-2 25-tap 5x5 上采样 ----
// 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier
static const char* FS_LANCZOS_UPSCALE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;
uniform vec2  uTexel;

float lanczos(float x) {
    if (abs(x) < 1.0e-5) return 1.0;
    if (abs(x) >= 2.0) return 0.0;
    float pix = 3.141592653589793 * x;
    return (sin(pix) * sin(pix * 0.5)) / (pix * pix * 0.5);
}

void main() {
    vec2 srcCoord = vUV / uTexel;
    vec2 srcInt   = floor(srcCoord - 0.5) + 0.5;
    vec2 frac     = srcCoord - srcInt;

    vec3 sum  = vec3(0.0);
    float wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec2 sp = (srcInt + vec2(float(i), float(j))) * uTexel;
            vec2 d  = vec2(float(i), float(j)) - frac;
            float w = lanczos(d.x) * lanczos(d.y);
            sum  += texture(uInputTex, sp).rgb * w;
            wsum += w;
        }
    }
    FragColor = vec4(max(sum / max(wsum, 1.0e-4), vec3(0.0)), 1.0);
}
)";

// ---- FS_CAS (GL 3.3): Phase F.0.6 — TAA 后 5-tap CAS 锐化 (AMD FidelityFX FSR1) ----
// 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier
static const char* FS_CAS_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出
uniform vec2  uTexelSize;          // 1.0 / vec2(W, H)
uniform float uSharpness;          // CAS [0, 1]

void main() {
    vec3 c = texture(uInputTex, vUV).rgb;
    vec3 n = texture(uInputTex, vUV + vec2(0.0,         uTexelSize.y)).rgb;
    vec3 s = texture(uInputTex, vUV - vec2(0.0,         uTexelSize.y)).rgb;
    vec3 e = texture(uInputTex, vUV + vec2(uTexelSize.x, 0.0       )).rgb;
    vec3 w = texture(uInputTex, vUV - vec2(uTexelSize.x, 0.0       )).rgb;

    // 邻域 min/max (per channel, 5-tap)
    vec3 mnRGB = min(c, min(n, min(s, min(e, w))));
    vec3 mxRGB = max(c, max(n, max(s, max(e, w))));

    // 对暗部和亮部对称的 dynamic range (FSR1 trick)
    vec3 mnRGB2 = min(mnRGB, 1.0 - mxRGB);
    vec3 ampRGB = clamp(min(mnRGB, mnRGB2) / max(mxRGB, vec3(1e-4)), 0.0, 1.0);
    ampRGB = sqrt(ampRGB);

    // peak: sharpness=0 → peak=-1/8 (弱), sharpness=1 → peak=-1/5 (强)
    float peak = -1.0 / mix(8.0, 5.0, uSharpness);
    vec3 wRGB  = ampRGB * peak;
    vec3 rcpW  = 1.0 / (4.0 * wRGB + 1.0);

    vec3 sum = c + (n + s + e + w) * wRGB;
    vec3 sharpened = sum * rcpW;

    // HDR safe: 防黑斑负值 (HDR 不截上限保留高光)
    FragColor = vec4(max(sharpened, vec3(0.0)), 1.0);
}
)";

// ---- FS_RCAS (GL 3.3): Phase F.0.12 — TAA 后 5-tap RCAS 锐化 (AMD FidelityFX FSR2) ----
// 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier
static const char* FS_RCAS_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出
uniform vec2  uTexelSize;          // 1.0 / vec2(W, H)
uniform float uSharpness;          // RCAS [0, 2]

// FSR2 推荐 noise threshold: 平衡 noise rejection 与小细节保留
const float kNoiseThreshold = 1.0 / 64.0;

void main() {
    vec3 e = texture(uInputTex, vUV).rgb;                                    // 中心
    vec3 b = texture(uInputTex, vUV + vec2(0.0,         uTexelSize.y)).rgb;  // 上
    vec3 h = texture(uInputTex, vUV - vec2(0.0,         uTexelSize.y)).rgb;  // 下
    vec3 d = texture(uInputTex, vUV + vec2(uTexelSize.x, 0.0       )).rgb;  // 右
    vec3 f = texture(uInputTex, vUV - vec2(uTexelSize.x, 0.0       )).rgb;  // 左

    // luma 提取: G channel as proxy (FSR2 优化)
    float bL = b.g, dL = d.g, eL = e.g, fL = f.g, hL = h.g;

    // 4-tap 邻域 min/max (排除中心)
    float mn4 = min(min(bL, dL), min(fL, hL));
    float mx4 = max(max(bL, dL), max(fL, hL));
    float range = mx4 - mn4;

    // Noise detection: 跳过 smooth 区域
    if (range < kNoiseThreshold) {
        FragColor = vec4(e, 1.0);
        return;
    }

    // Edge protection: lobe = sqrt(min(eL-mn, mx-eL) / range)
    float lobe = max(min(eL - mn4, mx4 - eL), 0.0);
    lobe = sqrt(lobe / max(range, 1e-4));

    // peak: sharpness=0 → peak=-1/16, sharpness=2 → peak=-1/4
    float peak = -1.0 / mix(16.0, 4.0, uSharpness * 0.5);
    float wgt  = peak * lobe;

    // Final composite
    vec3 sum    = e + (b + d + f + h) * wgt;
    vec3 result = sum / (1.0 + 4.0 * wgt);

    // HDR safe (FSR2 标准): clamp ≥ 0
    FragColor = vec4(max(result, vec3(0.0)), 1.0);
}
)";

// ---- FS_MOTION_BLUR (GL 3.3): Phase E.15 per-pixel velocity blur ----
//   1. SampleVelocityDilated 与 SSRTemporal 同算法 (3x3 max-length 邻域可选)
//   2. E3 软限: |vel| <= screenDiagUV × 0.3 ≈ 0.4243 防极端拖尾糊死
//   3. 沿 -vel × strength 均匀 N 采样, N ∈ [1, 32]
static const char* FS_MOTION_BLUR_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneTex;       // HDR + 所有后处理 (Bloom/SSR/LensFlare 累积)
uniform sampler2D uVelocityTex;        // combined velocity (slot 1)
uniform sampler2D uCameraVelocityTex;  // Phase E.16: camera-only velocity (slot 2)
uniform vec2  uTexel;              // 1.0 / vec2(W, H)
uniform float uStrength;           // 用户调 [0, 4]
uniform int   uSampleCount;        // [1, 32]
uniform int   uVelocityDilation;   // 0=单点 / 1=3x3 max-length
uniform int   uVelocityFormat;     // 0=RG16F / 1=RG8
uniform float uVelocityScale;      // RG8 解码 scale (默认 0.25)
uniform int   uMode;               // Phase E.16: 0=combined / 1=camera_only / 2=object_only

vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

vec2 SampleVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uVelocityTex, uv + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

// Phase E.16: 与 SampleVelocityDilated 同算法，切换采 cameraVelocityTex
// GLES3 不支持 sampler 作函数参数，故复制一份
//   mode=1: 单独采此量，mode=2: combined - camera = object_only
vec2 SampleCameraVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) return DecodeVelocity(texture(uCameraVelocityTex, uv).rg);
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(texture(uCameraVelocityTex, uv + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    return bestV;
}

void main() {
    // Phase E.16 — 按 mode 选 velocity 源:
    //   0=combined (camera+object 合一)
    //   1=camera_only (仅相机运动，物体独立于序列)
    //   2=object_only (v_combined - v_camera 近似物体独立运动)
    vec2 vel;
    if (uMode == 1) {
        vel = SampleCameraVelocityDilated(vUV);
    } else if (uMode == 2) {
        vel = SampleVelocityDilated(vUV) - SampleCameraVelocityDilated(vUV);
    } else {
        vel = SampleVelocityDilated(vUV);
    }
    vel *= uStrength;

    // E3 软限: max blur UV = 屏幕对角线 × 0.3 (sqrt(2) * 0.3 ≈ 0.4243)
    const float kMaxBlurUV = 0.4243;
    float velLen = length(vel);
    if (velLen > kMaxBlurUV) vel *= (kMaxBlurUV / velLen);

    int   count    = clamp(uSampleCount, 1, 32);
    float countInv = 1.0 / float(max(count - 1, 1));
    vec3  sum      = vec3(0.0);
    const int kMaxSamples = 32;
    for (int i = 0; i < kMaxSamples; ++i) {
        if (i >= count) break;
        float t = float(i) * countInv;            // [0, 1]
        vec2  uv = vUV - vel * t;                  // 朝过去帧方向采样
        sum += texture(uSceneTex, uv).rgb;
    }
    FragColor = vec4(sum / float(count), 1.0);
}
)";

// ---- FS_VELOCITY_DILATE (GL 3.3): Phase E.18 独立 velocity dilation pass ----
// 与 GLES3 版完全等价, 仅 #version + 无 precision qualifier
static const char* FS_VELOCITY_DILATE_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrcVelocityTex;   // raw velocity (RG16F 或 RG8 encoded), slot 0
uniform vec2  uTexel;                // 1.0 / vec2(W, H) — full-res, 决定 9-tap 物理覆盖
uniform int   uVelocityFormat;       // 0 = RG16F (raw直存); 1 = RG8 (encoded)
uniform float uVelocityScale;        // RG8 解码 scale (默认 0.25)

vec2 DecodeVelocity(vec2 raw) {
    return (uVelocityFormat == 1) ? ((raw - 0.5) * (2.0 * uVelocityScale)) : raw;
}

void main() {
    // 9-tap max-length dilation: 邻域 (-1,-1)..(1,1), 按 length² 取最大 velocity
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 v = DecodeVelocity(
                texture(uSrcVelocityTex, vUV + vec2(float(dx), float(dy)) * uTexel).rg);
            float l = dot(v, v);
            if (l > bestLen) { bestLen = l; bestV = v; }
        }
    }
    FragColor = vec4(bestV, 0.0, 1.0);   // dilatedTex.rg = decode 后的 float
}
)";

#endif

// Phase AS.2 — Mesh GPU 资源 (Phase AX 扩展加 morph delta texture)
struct MeshGPU {
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    int    indexCount;
    // Phase AX — morph target 资源 (仅 skinnedMorphMeshes 用; 其他 mesh 默认 0)
    GLuint morphPosTex     = 0;     // RGB32F 2D texture: width=vCount, height=morphCount
    GLuint morphNrmTex     = 0;     // 同上 (NORMAL delta), 0 表示未提供
    int    morphCount      = 0;     // 实际 target 数 (1..MORPH_TARGET_MAX)
    bool   hasMorphNormal  = false;
};

// ==================== GL33Backend ====================

class GL33Backend : public RenderBackend {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;             // Phase A5: 静态索引缓冲, 与 BatchRenderer 配合
    GLuint program = 0;
    GLint  locMVP = -1;
    GLint  locUseTexture = -1;

    // Phase A5: EBO 容量 (索引数), 由 BatchRenderer 一次性上传后永久不变
    int eboCapacity = 0;

    // 当前颜色
    float curColor[4] = {1, 1, 1, 1};

    // 自管理矩阵栈
    std::vector<Mat4> matStack;
    Mat4 projection;          // Phase F.0: 原始 unjittered projection (SSR/SSAO/velocity 用)
    Mat4 jitteredProjection;  // Phase F.0: TAA 启用时 sub-pixel jittered (raster 用)
    bool jitterActive = false; // Phase F.0: true 时 ComputeMVP3D 用 jitteredProjection
    Mat4 modelview;

    // 当前绑定的纹理 (0 = 无)
    GLuint boundTex = 0;

    // ============ Phase F.0.11.2 — PBO 异步 readback (ping-pong, 双 PBO) ============
    // 行为: ReadbackDefaultFBAsync 每次调用启动 PBO[idx] readback + 取 PBO[1-idx] 数据
    //       lazy 分配 (首次调用时建); 尺寸变 → 释放旧 PBO 重建
    GLuint m_pbo[2]      = {0, 0};
    int    m_pbo_w       = 0;
    int    m_pbo_h       = 0;
    int    m_pbo_idx     = 0;            // 下一次 readback 用 m_pbo[m_pbo_idx]
    bool   m_pbo_pending[2] = {false, false};  // 数据是否已 issue (待读)

    // 动态 VBO 容量 (顶点数)
    int vboCapacity = 0;
    static constexpr int INITIAL_VBO_CAPACITY = 1024;

    // ---- Phase AS.4 — 3D mesh + 材质资源 ----
    GLuint programUnlit = 0;
    GLuint programPBR   = 0;
    bool   userShaderActive = false;  // 用户 Shader:Use 时为 true; UseDefaultShader 时复位
    bool   depthTestEnabled = false;  // 当前深度测试状态 (默认关, 与 2D 兼容)
    Mat4   viewMatrix;                // Phase AS.2 — 视图矩阵 (LookAt 结果)
    bool   hasView          = false;  // 是否已 LoadView (false 表示用 modelview 直接)
    Mat4   prevViewProj;
    bool   hasPrevViewProjForVelocity = false;
    bool   hasNextPrevModel = false;
    Mat4   nextPrevModel;

    // Phase AS.4 — Lighting 状态 (CPU 镜像; DrawMeshMaterial 时上传 uniform)
    float dirLightDir[3]   = { 0.408f, 0.816f, 0.408f };
    float dirLightColor[3] = { 0.9f, 0.9f, 0.85f };
    float dirLightIntensity = 1.0f;
    bool  dirLightEnabled  = true;
    float ambientLight[3]  = { 0.2f, 0.2f, 0.25f };
    float cameraPos[3]     = { 0.0f, 0.0f, 0.0f };

    static constexpr int MAX_PT_LIGHTS = 4;
    PointLight pointLights[MAX_PT_LIGHTS] = {};
    bool       pointLightUsed[MAX_PT_LIGHTS] = { false, false, false, false };

    // Mesh 资源池
    std::unordered_map<uint32_t, MeshGPU> meshes;
    uint32_t                              nextMeshId = 1;

    // ---- Phase AW — GPU Skinning 资源 ----
    GLuint  programUnlitSkin = 0;
    GLuint  programPBRSkin   = 0;
    GLuint  uboJointMatrices = 0;
    GLuint  uboPrevJointMatrices = 0;
    bool    gpuSkinningSupported = false;
    static constexpr GLuint UBO_BINDING_POINT = 0;     // 固定 binding point
    static constexpr GLuint PREV_UBO_BINDING_POINT = 1;
    static constexpr int    SKIN_MAX_JOINTS   = 64;     // shader 内 uJointMats[64]
    // SkinnedMesh 资源池: ID 高位 0x80000000 区分普通 mesh
    std::unordered_map<uint32_t, MeshGPU> skinnedMeshes;
    uint32_t                              nextSkinnedMeshId = 0x80000001u;

    // ---- Phase AX — GPU Morph Target 资源 ----
    GLuint  programUnlitSkinMorph = 0;
    GLuint  programPBRSkinMorph   = 0;
    bool    morphTargetsSupported = false;
    // SkinnedMorphMesh 资源池: ID 高位 0xC0000000 区分纯蒙皮 mesh
    std::unordered_map<uint32_t, MeshGPU> skinnedMorphMeshes;
    uint32_t                              nextSkinnedMorphMeshId = 0xC0000001u;

    // ---- Phase E.1 — 2D Lit (forward 多光 + Normal Map) ----
    // E.1.1: VAO/VBO/EBO + 顶点属性 layout
    // E.1.2: VS_LIT2D + FS_LIT2D shader 编译 + uniform location 缓存
    GLuint vaoLit2D     = 0;
    GLuint vboLit2D     = 0;
    GLuint eboLit2D     = 0;
    GLuint programLit2D = 0;        // E.1.2 编译 + link 成功后非 0
    bool   lit2DSupported = false;  // VAO/VBO/EBO + program 全部就绪后置 true
    static constexpr int LIT2D_VBO_INITIAL_VERTS = 4;  // 单 quad; E.1.5 可按需扩容

    // ---- Phase E.3.1 — HDR + ACES Tonemap ----
    // 全屏 quad VBO/VAO (6 顶点 = 2 三角形; pos.xy + uv.xy, 无 EBO)
    GLuint vaoTonemap     = 0;
    GLuint vboTonemap     = 0;
    GLuint programTonemap = 0;        // 编译 + link 成功后非 0
    bool   tonemapSupported = false;  // VBO/VAO + program 全部就绪后置 true
    GLint  locTonemap_HDRTex      = -1;
    GLint  locTonemap_Exposure    = -1;
    GLint  locTonemap_Gamma       = -1;
    GLint  locTonemap_Mode        = -1;   // Phase E.3.4 — uTonemapMode (int)
    GLint  locTonemap_LUT         = -1;   // Phase F.0.10.8 — sampler3D uLUT
    GLint  locTonemap_LUTStrength = -1;   // Phase F.0.10.8 — float uLUTStrength
    GLint  locTonemap_LUTEnabled  = -1;   // Phase F.0.10.8 — int uLUTEnabled
    // HDR FBO → depth RBO 关系映射 (CreateHDRFBO 写入, DeleteHDRFBO 查询并释放)
    std::unordered_map<uint32_t, uint32_t> hdrFboDepthRB;
    // Phase E.8.x — HDR FBO → normal RT 关系映射 (MRT G-buffer view-space normal)
    // CreateHDRFBO 写入 (仅 outNormalTex != null 时), DeleteHDRFBO 查询并释放.
    // GetHDRNormalTex(fbo) 仅读查找.
    std::unordered_map<uint32_t, uint32_t> hdrFboNormalTex;
    std::unordered_map<uint32_t, uint32_t> hdrFboVelocityTex;
    // Phase E.16 — camera-only velocity tex (slot 3 of HDR FBO MRT)
    // CreateHDRFBO 在 outCameraVelocityTex != null 时创建, 与 velocityTex 同格式.
    // 3D shader FS 同时写 FragVelocity (combined) + FragCameraVelocity (camera-only).
    std::unordered_map<uint32_t, uint32_t> hdrFboCameraVelocityTex;
    // Phase E.14 — 记录每个 fbo 创建时使用的 velocity 存储格式，
    // 供 SSRTemporal/3D shader 将来选择对应 program (RG16F vs RG8 双路径)。
    std::unordered_map<uint32_t, VelocityFormat> hdrFboVelocityFormat;
    // Phase E.14 — velocity dilation 全局开关 (默认 ON)。
    bool                                    velocityDilation_ = true;

    // Phase F.1.1 — Mipmap LOD bias (3D mesh shader texture(..) 第 3 个参数).
    //   默认 0.0 (零回归); TAARenderer 在 TAAU 启用时自动调成 log2(scale)-0.7.
    //   shader 内 `texture(s, uv, uMipBias)` 当 uMipBias=0 等同 `texture(s, uv)`.
    float                                   mipBias_ = 0.0f;
    // Phase E.14 — RG8 模式下 shader 编/解码 velocity 用的尺度因子。
    // 本 phase 定为常量 0.25，后续 phase 可推动态调节。
    static constexpr float kVelocityScaleDefault = 0.25f;
    // Phase E.14 — 当前活跃 velocity format (供 3D shader 绘制时上传编码 uniform 用)。
    // 随最近一次 CreateHDRFBO 设定; HDRRenderer 同时只会有 1 张 HDR FBO，足以安全共享。
    VelocityFormat                          activeVelocityFormat_ = VelocityFormat::RG16F;

    // ---- Phase E.4 — Bloom 后处理 ----
    // 3 个 shader program (共用 vaoTonemap/vboTonemap 全屏 quad, 无独立 VAO)
    GLuint programBloomBright = 0;
    GLuint programBloomDown   = 0;
    GLuint programBloomUp     = 0;
    bool   bloomSupported     = false;

    // Bright pass uniform: uSrc(sampler) / uThreshold(float)
    GLint  locBloomBright_Src       = -1;
    GLint  locBloomBright_Threshold = -1;

    // Downsample uniform: uSrc(sampler) / uTexel(vec2)
    GLint  locBloomDown_Src      = -1;
    GLint  locBloomDown_Texel    = -1;
    GLint  locBloomDown_UvBounds = -1;   // Phase F.0.10.5

    // Upsample uniform: uSrc(sampler) / uTexel(vec2) / uRadius(float) / uIntensity(float)
    GLint  locBloomUp_Src       = -1;
    GLint  locBloomUp_Texel     = -1;
    GLint  locBloomUp_Radius    = -1;
    GLint  locBloomUp_Intensity = -1;
    GLint  locBloomUp_UvBounds  = -1;   // Phase F.0.10.5

    // ---- Phase E.5 — Auto Exposure (Eye Adaptation) ----
    // 1 个 shader program (luma extract; 共用 vaoTonemap 全屏 quad)
    GLuint programLumaExtract        = 0;
    bool   autoExposureSupported     = false;
    GLint  locLumaExtract_HDRTex     = -1;

    // ---- Phase E.6 — Lens Dirt + Streak ----
    // Lens Dirt: 1 composite shader + 1x1 white fallback tex
    GLuint programLensDirt           = 0;
    GLuint whiteTex1x1               = 0;       // LensDirt fallback (dirtTex=0 时用)
    bool   lensDirtSupported         = false;
    GLint  locLensDirt_BloomTex      = -1;
    GLint  locLensDirt_DirtTex       = -1;
    GLint  locLensDirt_Intensity     = -1;
    // Streak: 2 shader (blur + composite) + 共用 programBloomBright 作 bright pass
    GLuint programStreakBlur         = 0;
    GLuint programStreakComposite    = 0;
    bool   streakSupported           = false;
    GLint  locStreakBlur_Src         = -1;
    GLint  locStreakBlur_Texel       = -1;
    GLint  locStreakBlur_Length      = -1;
    GLint  locStreakBlur_Direction   = -1;
    GLint  locStreakComposite_Src    = -1;
    GLint  locStreakComposite_Intensity = -1;

    // Phase E.7 — Lens Flare: 1 shader (ghost) + 复用 Bloom bright/composite
    GLuint programLensFlareGhost       = 0;
    bool   lensFlareSupported          = false;
    GLint  locLensFlare_BrightTex      = -1;
    GLint  locLensFlare_FlareTex       = -1;   // Phase E.7.4 — 用户贴图 sampler
    GLint  locLensFlare_GhostCount     = -1;
    GLint  locLensFlare_GhostDispersal = -1;
    GLint  locLensFlare_HaloWidth      = -1;
    GLint  locLensFlare_Aberration     = -1;
    GLint  locLensFlare_DistortionEn   = -1;

    // Phase E.8 — SSAO (3 shader: raw + blur + composite)
    GLuint programSSAO           = 0;
    GLuint programSSAOBlur       = 0;
    GLuint programSSAOComposite  = 0;
    bool   ssaoSupported         = false;
    // SSAO uniform locations cache
    GLint  locSSAO_DepthTex      = -1;
    GLint  locSSAO_NoiseTex      = -1;
    GLint  locSSAO_NormalTex     = -1;   // Phase E.8.x — G-buffer view-space normal sampler
    GLint  locSSAO_Proj          = -1;
    GLint  locSSAO_InvProj       = -1;
    GLint  locSSAO_Kernel        = -1;   // vec3[16]
    GLint  locSSAO_KernelSize    = -1;
    GLint  locSSAO_Radius        = -1;
    GLint  locSSAO_Bias          = -1;
    GLint  locSSAO_Power         = -1;
    GLint  locSSAO_NoiseScale    = -1;
    GLint  locSSAOBlur_SSAOTex   = -1;
    GLint  locSSAOBlur_DepthTex  = -1;
    GLint  locSSAOBlur_Texel     = -1;
    GLint  locSSAOBlur_Axis      = -1;
    GLint  locSSAOComp_SceneTex  = -1;
    GLint  locSSAOComp_AOTex     = -1;
    GLint  locSSAOComp_Intensity = -1;
    // SSAO composite 临时 full-res RGBA16F RT (解 feedback loop)
    GLuint ssaoCompTempFbo       = 0;
    GLuint ssaoCompTempTex       = 0;
    int    ssaoCompTempW         = 0;
    int    ssaoCompTempH         = 0;

    // Phase E.9 — SSR (2 shader: raw + composite)
    // 高质量方案: full-res RGBA16F + 64 步默认 (用户拍板 2026-05-12)
    GLuint programSSR              = 0;
    GLuint programSSRComposite     = 0;
    bool   ssrSupported            = false;
    // FS_SSR uniform locations
    GLint  locSSR_DepthTex         = -1;
    GLint  locSSR_NormalTex        = -1;
    GLint  locSSR_HDRTex           = -1;
    GLint  locSSR_Proj             = -1;
    GLint  locSSR_InvProj          = -1;
    GLint  locSSR_MaxSteps         = -1;
    GLint  locSSR_StepSize         = -1;
    GLint  locSSR_Thickness        = -1;
    GLint  locSSR_MaxDistance      = -1;
    GLint  locSSR_EdgeFade         = -1;
    // FS_SSR_COMPOSITE uniform locations
    GLint  locSSRComp_SceneTex     = -1;
    GLint  locSSRComp_ReflectTex   = -1;
    GLint  locSSRComp_Intensity    = -1;
    // SSR composite 临时 full-res RGBA16F RT (解 feedback loop, 与 SSAO 同模式)
    GLuint ssrCompTempFbo          = 0;
    GLuint ssrCompTempTex          = 0;
    int    ssrCompTempW            = 0;
    int    ssrCompTempH            = 0;

    // Phase E.10 — SSR Blur (反射模糊, half-res ping-pong, 用户拍板 2026-05-12)
    // Phase E.11 — 添加 depth-aware bilateral 双模式统一 shader
    GLuint programSSRBlur          = 0;
    bool   ssrBlurSupported        = false;
    GLint  locSSRBlur_SrcTex       = -1;
    GLint  locSSRBlur_DepthTex     = -1;   // Phase E.11
    GLint  locSSRBlur_Texel        = -1;
    GLint  locSSRBlur_Axis         = -1;
    GLint  locSSRBlur_Radius       = -1;
    GLint  locSSRBlur_Bilateral    = -1;   // Phase E.11
    GLint  locSSRBlur_DepthSigma   = -1;   // Phase E.11

    // Phase E.12 — Temporal SSR (时序累积降噪, 用户拍板 2026-05-14)
    GLuint programSSRTemporal           = 0;
    bool   ssrTemporalSupported         = false;
    // FS_SSR 的 jitter uniform (在 Phase E.9 program 内, 新增缓存)
    GLint  locSSR_JitterOffset          = -1;
    // FS_SSR_TEMPORAL uniform locations
    GLint  locSSRTemporal_CurReflectTex = -1;
    GLint  locSSRTemporal_HistoryTex    = -1;
    GLint  locSSRTemporal_DepthTex      = -1;
    GLint  locSSRTemporal_VelocityTex   = -1;
    GLint  locSSRTemporal_ReprojectMat  = -1;
    GLint  locSSRTemporal_InvProj       = -1;
    GLint  locSSRTemporal_Texel         = -1;
    GLint  locSSRTemporal_BlendAlpha    = -1;
    GLint  locSSRTemporal_RejectionMode = -1;
    GLint  locSSRTemporal_HasHistory    = -1;
    GLint  locSSRTemporal_HasVelocityTex = -1;
    // Phase E.14 — dilation + dual-format decode uniforms
    GLint  locSSRTemporal_VelocityDilation = -1;
    GLint  locSSRTemporal_VelocityFormat   = -1;
    GLint  locSSRTemporal_VelocityScale    = -1;

    // Phase E.15 — Motion Blur (per-pixel velocity blur, 单 program + ping-pong RT)
    // Phase E.16 扩展: + uCameraVelocityTex (slot 2) + uMode
    GLuint programMotionBlur            = 0;
    bool   motionBlurSupported          = false;
    GLint  locMB_SceneTex               = -1;   // sampler2D, slot 0
    GLint  locMB_VelocityTex            = -1;   // sampler2D, slot 1 (combined)
    GLint  locMB_CameraVelocityTex      = -1;   // sampler2D, slot 2 (camera-only)  ★ E.16
    GLint  locMB_Texel                  = -1;
    GLint  locMB_Strength               = -1;
    GLint  locMB_SampleCount            = -1;
    GLint  locMB_VelocityDilation       = -1;
    GLint  locMB_VelocityFormat         = -1;
    GLint  locMB_VelocityScale          = -1;
    GLint  locMB_Mode                   = -1;   // ★ E.16: 0=combined / 1=camera / 2=object

    // Phase E.18 — 独立 Velocity Dilation Pass (9-tap max-length 抽出为单 pass, 多 consumer 共享)
    // 与 motion blur 同模式: 单 program, RT 由 HDRRenderer 持有 (CreateVelocityDilateRT 返 fbo+tex)
    GLuint programVelocityDilate            = 0;
    bool   velocityDilateSupported          = false;
    GLint  locVDilate_SrcVelocityTex        = -1;   // sampler2D, slot 0
    GLint  locVDilate_Texel                 = -1;   // 1.0 / (w, h)
    GLint  locVDilate_VelocityFormat        = -1;   // 0=RG16F / 1=RG8
    GLint  locVDilate_VelocityScale         = -1;   // RG8 decode scale
    // Phase E.18 — dilation pass active 状态 (每帧 EndScene 由 HDRRenderer 设置)
    //   true  → DrawSSRTemporal/DrawMotionBlur 内强制 uVelocityDilation=0 (consumer 单点采)
    //   false → 沿用 velocityDilation_ 旧逻辑 (consumer shader 内 inline 9-tap)
    bool   dilationPassActive_              = false;

    // Phase F.0 — TAA Master Pipeline (single program, history ping-pong RT 由 TAARenderer 持有)
    //   shader: FS_TAA_SOURCE (reproject + neighborhood AABB clip + alpha blend)
    GLuint programTAA                       = 0;
    bool   taaSupported                     = false;
    GLint  locTAA_CurHdrTex                 = -1;   // sampler2D, slot 0
    GLint  locTAA_HistoryTex                = -1;   // sampler2D, slot 1
    GLint  locTAA_VelocityTex               = -1;   // sampler2D, slot 2
    GLint  locTAA_Texel                     = -1;
    GLint  locTAA_BlendAlpha                = -1;
    GLint  locTAA_NeighborhoodClip          = -1;
    GLint  locTAA_HasHistory                = -1;
    GLint  locTAA_VelocityDilation          = -1;
    GLint  locTAA_VelocityFormat            = -1;
    GLint  locTAA_VelocityScale             = -1;
    GLint  locTAA_AntiFlicker               = -1;   // Phase F.0.4: 0/1 上传 Karis weighting 开关
    GLint  locTAA_ClipMode                  = -1;   // Phase F.0.2/F.0.3: 0=RGB AABB, 1=YCoCg AABB, 2=YCoCg variance
    GLint  locTAA_VarianceGamma             = -1;   // Phase F.0.3: variance clip 收紧系数 γ (仅 clipMode==2 生效)
    GLint  locTAA_MotionGamma               = -1;   // Phase F.0.8: motion-adaptive 高速区域 γ (仅 motionAdaptive==1 生效)
    GLint  locTAA_MotionAdaptiveGamma       = -1;   // Phase F.0.8: 0=仅用 varianceGamma (F.0.3 行为), 1=按 vel 长度 lerp 两 γ
    GLint  locTAA_UvBounds                  = -1;   // Phase F.0.10.5: vec4 邻域采样 UV clamp (默认 0,0,1,1 = 无 clamp)
    // Phase F.1 TAAU 不新增 shader uniform: uTexel 始终上传为 1/(renderW, renderH).
    //   F.0 模式 renderW/H == w/h, 行为等价; TAAU 模式 uTexel = 1/render-res 让 cur 邻域 + velocity dilation
    //   的 9-tap step 在 render 像素空间, 与 curHdrTex/velocityTex (render-res) 像素对齐 (UE4/Unity 标准做法).
    //   glViewport 改用 outputW/H (而非 w/h) 让 raster 覆盖 output FBO.

    // Phase F.0.1 — TAA Sharpening (4-tap unsharp mask, 复用 SupportsTAA 能力位)
    //   shader: FS_SHARPEN_SOURCE (uInputTex + uTexelSize + uSharpness)
    GLuint programSharpen                   = 0;
    GLint  locSharpen_InputTex              = -1;   // sampler2D, slot 0
    GLint  locSharpen_TexelSize             = -1;
    GLint  locSharpen_Sharpness             = -1;
    GLint  locSharpen_UvBounds              = -1;   // Phase F.0.10.5: vec4 4-tap NSEW UV clamp

    // Phase F.0.6 — TAA CAS Sharpening (5-tap AMD FidelityFX FSR1, 与 F.0.1 unsharp 共存)
    //   shader: FS_CAS_SOURCE (uInputTex + uTexelSize + uSharpness ∈ [0, 1])
    //   contrast-adaptive: 邻域 min/max 自动减弱低对比区域锐化, HDR safe (clamp ≥ 0)
    GLuint programCAS                       = 0;
    GLint  locCAS_InputTex                  = -1;   // sampler2D, slot 0
    GLint  locCAS_TexelSize                 = -1;
    GLint  locCAS_Sharpness                 = -1;

    // Phase F.0.9 — TAA Custom Upsampler (Catmull-Rom 9-tap bicubic, 仅 sharpness=0+halfRes 路径)
    //   shader: FS_BICUBIC_UPSCALE_SOURCE (uInputTex + uTexel) Sigggraph 2018 Filmic SMAA 优化版
    GLuint programBicubicUpscale            = 0;
    GLint  locBicubic_InputTex              = -1;   // sampler2D, slot 0
    GLint  locBicubic_Texel                 = -1;   // 1.0 / (srcW, srcH) — src 分辨率

    // Phase F.0.14 — TAA Lanczos-2 25-tap 5x5 上采样 (高画质替代 F.0.9 Catmull-Rom)
    //   shader: FS_LANCZOS_UPSCALE_SOURCE (uInputTex + uTexel) Lanczos kernel 25-tap unrolled
    GLuint programLanczosUpscale            = 0;
    GLint  locLanczos_InputTex              = -1;   // sampler2D, slot 0
    GLint  locLanczos_Texel                 = -1;   // 1.0 / (srcW, srcH) — src 分辨率

    // Phase F.0.12 — TAA RCAS Sharpening (5-tap AMD FidelityFX FSR2, 与 F.0.1 unsharp / F.0.6 cas 共存)
    //   shader: FS_RCAS_SOURCE (uInputTex + uTexelSize + uSharpness ∈ [0, 2])
    //   FSR2 robust: noise detection (range < 1/64 跳过) + edge protection (lobe sqrt 限制)
    GLuint programRCAS                      = 0;
    GLint  locRCAS_InputTex                 = -1;   // sampler2D, slot 0
    GLint  locRCAS_TexelSize                = -1;
    GLint  locRCAS_Sharpness                = -1;

    // Phase E.2.1 — Lighting2D dirty bit cache
    // 当 state->version 与此值相等时, UploadLighting2D 跳过所有 glUniform*v 调用
    // 初值 0 + State 初值 1 保证首次调用一定 mismatch (触发首次上传)
    uint32_t lastUploadedLighting2DVersion = 0;

    // Phase E.2.3 — 动态 EBO (专供 DrawLit2DBatch 的动态索引上传)
    // 与静态 eboLit2D 分开: 静态 eboLit2D 存 [0,1,2,0,2,3] 给 DrawLit2DQuad 用
    //                       动态 eboLit2DBatch 存 N quad 的索引 (4 顶点 / quad ×6 索引 / quad)
    // DrawLit2DBatch 结束后必须 glBindBuffer 恢复静态 eboLit2D, 保证下次 DrawLit2DQuad 正确
    GLuint      eboLit2DBatch         = 0;
    GLsizeiptr  eboLit2DBatchCapacity = 0;  // 已分配的索引数 (×sizeof(uint32_t) = 字节数)

    // Phase E.1.2 — Lit2D shader uniform locations
    // 标量 uniform
    GLint locLit2D_MVP            = -1;
    GLint locLit2D_Model          = -1;
    GLint locLit2D_Texture        = -1;  // sampler2D, slot 0
    GLint locLit2D_NormalMap      = -1;  // sampler2D, slot 1
    GLint locLit2D_HasNormalMap   = -1;
    GLint locLit2D_Ambient        = -1;
    GLint locLit2D_LightCount     = -1;
    // uniform array base location (各 16 元素, glUniform*v(loc, count, ...) 一次性上传)
    GLint locLit2D_LightType      = -1;  // int[16]
    GLint locLit2D_LightPos       = -1;  // vec3[16]
    GLint locLit2D_LightDir       = -1;  // vec3[16]
    GLint locLit2D_LightColor     = -1;  // vec3[16]
    GLint locLit2D_LightRange     = -1;  // float[16]
    GLint locLit2D_LightIntensity = -1;  // float[16]
    GLint locLit2D_LightInnerCos  = -1;  // float[16]
    GLint locLit2D_LightOuterCos  = -1;  // float[16]

    // Phase E.1.5 — 动态 vboLit2D 容量跟踪 (DrawLit2DTriangles 任意三角形流需要扩容)
    int    vboLit2DCapacity = 0;  // InitLit2D 设为 LIT2D_VBO_INITIAL_VERTS

    // Phase E.1.5 — Lit2D 永久 mesh 资源池 (与 meshes / skinnedMeshes 平行, 高位 0xA0000000)
    std::unordered_map<uint32_t, MeshGPU> litMeshes;
    uint32_t                              nextLitMeshId = 0xA0000001u;

    // 编译 shader, 返回 0 表示失败
    static GLuint CompileShader(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            CC::Log(CC::LOG_ERROR, "GL33: shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    // 链接 program
    static GLuint LinkProgram(GLuint vs, GLuint fs) {
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(p, 512, nullptr, log);
            CC::Log(CC::LOG_ERROR, "GL33: program link error: %s", log);
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    // 上传 MVP uniform (2D shader, 简单的 projection * modelview)
    void FlushMVP() {
        Mat4 mvp = projection * modelview;
        glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp.m);
    }

    // Phase F.0: 取栅格化用的 projection (jitter active 时用 jitteredProjection, 否则原始)
    // 用于 raster 路径 (gl_Position 计算): ComputeMVP3D / FlushMVP / BeginLit2DDraw / 用户 BindShader 自动 uMVP
    const Mat4& ActiveProjection() const {
        return jitterActive ? jitteredProjection : projection;
    }

    // 计算 3D MVP: ActiveProjection * view * modelview (raster 路径, jitter active 时含 sub-pixel 偏移)
    Mat4 ComputeMVP3D() const {
        Mat4 vm = hasView ? (viewMatrix * modelview) : modelview;
        return ActiveProjection() * vm;
    }

    // Phase F.0: 始终用 unjittered projection (velocity buffer 用, 保持 reproject 准确)
    // vertex shader 内 vCurClip = uCurViewProj * (uModel * pos) 而不用 gl_Position (已含 jitter)
    Mat4 ComputeViewProj3D() const {
        return projection * (hasView ? viewMatrix : Mat4::Identity());
    }

    void UploadVelocityUniforms(GLuint program3D, const Mat4& curModel, const Mat4* prevModelOverride) {
        if (!program3D) return;
        Mat4 curViewProj = ComputeViewProj3D();
        Mat4 prevModel = prevModelOverride ? *prevModelOverride : curModel;
        GLint locCurVP = glGetUniformLocation(program3D, "uCurViewProj");
        GLint locPrevVP = glGetUniformLocation(program3D, "uPrevViewProj");
        GLint locPrevM = glGetUniformLocation(program3D, "uPrevModel");
        GLint locHas = glGetUniformLocation(program3D, "uHasVelocityHistory");
        if (locCurVP >= 0) glUniformMatrix4fv(locCurVP, 1, GL_FALSE, curViewProj.m);
        if (locPrevVP >= 0) glUniformMatrix4fv(locPrevVP, 1, GL_FALSE, prevViewProj.m);
        if (locPrevM >= 0) glUniformMatrix4fv(locPrevM, 1, GL_FALSE, prevModel.m);
        if (locHas >= 0) glUniform1i(locHas, hasPrevViewProjForVelocity ? 1 : 0);
        // Phase E.14 — 上传 velocity 编码双格式 uniform。
        // 这两个 uniform 是 Phase E.14 新增; 旧 program (Phase E.13 编译于未重启后端的
        // 后端实例) 拿不到 location 会返 -1，这里源码统一重新编译不会出现。
        GLint locVelFmt   = glGetUniformLocation(program3D, "uVelocityFormat");
        GLint locVelScale = glGetUniformLocation(program3D, "uVelocityScale");
        if (locVelFmt   >= 0) glUniform1i(locVelFmt,   (activeVelocityFormat_ == VelocityFormat::RG8) ? 1 : 0);
        if (locVelScale >= 0) glUniform1f(locVelScale, kVelocityScaleDefault);
        // Phase F.1.1 — Mipmap LOD bias (FS_UNLIT / FS_PBR shader 内 texture(s, uv, uMipBias))
        //   旧 program (Phase E~F.1.0 编译) 拿不到 location 会返 -1, 静默跳过 (零回归)
        GLint locMipBias = glGetUniformLocation(program3D, "uMipBias");
        if (locMipBias >= 0) glUniform1f(locMipBias, mipBias_);
    }

    // 确保 VBO 容量足够
    void EnsureVBOCapacity(int vertexCount) {
        if (vertexCount <= vboCapacity) return;
        int newCap = vboCapacity ? vboCapacity : INITIAL_VBO_CAPACITY;
        while (newCap < vertexCount) newCap *= 2;
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(RenderVertex), nullptr, GL_DYNAMIC_DRAW);
        vboCapacity = newCap;
    }

public:
    bool Init() override {
        // 编译链接 shader
        GLuint vs = CompileShader(GL_VERTEX_SHADER, VS_SOURCE);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FS_SOURCE);
        if (!vs || !fs) return false;
        program = LinkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!program) return false;

        locMVP = glGetUniformLocation(program, "uMVP");
        locUseTexture = glGetUniformLocation(program, "uUseTexture");

        // 创建 VAO + VBO + EBO (Phase A5)
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, INITIAL_VBO_CAPACITY * sizeof(RenderVertex),
                     nullptr, GL_DYNAMIC_DRAW);
        vboCapacity = INITIAL_VBO_CAPACITY;
        // EBO 绑定到 VAO, BatchRenderer 首次 DrawIndexed 时上传索引数据
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

        // 顶点属性: aPos(0), aTexCoord(1), aColor(2)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                              (void*)offsetof(RenderVertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                              (void*)offsetof(RenderVertex, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                              (void*)offsetof(RenderVertex, r));

        glBindVertexArray(0);

        // 初始矩阵
        projection = Mat4::Identity();
        jitteredProjection = Mat4::Identity();   // Phase F.0
        jitterActive = false;                      // Phase F.0
        modelview = Mat4::Identity();
        matStack.reserve(32);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // ---- Phase AS.4 — 编译 Unlit + PBR 双 shader ----
        GLuint vs3D     = CompileShader(GL_VERTEX_SHADER, VS3D_SOURCE);
        GLuint fsUnlit  = CompileShader(GL_FRAGMENT_SHADER, FS_UNLIT_SOURCE);
        GLuint fsPBR    = CompileShader(GL_FRAGMENT_SHADER, FS_PBR_SOURCE);
        if (vs3D && fsUnlit) {
            programUnlit = LinkProgram(vs3D, fsUnlit);
            if (!programUnlit) CC::Log(CC::LOG_WARN, "GL33: Unlit shader link failed");
        }
        if (vs3D && fsPBR) {
            programPBR = LinkProgram(vs3D, fsPBR);
            if (!programPBR) CC::Log(CC::LOG_WARN, "GL33: PBR shader link failed");
        }
        if (vs3D)    glDeleteShader(vs3D);
        if (fsUnlit) glDeleteShader(fsUnlit);
        if (fsPBR)   glDeleteShader(fsPBR);

        // ---- Phase AW — GPU Skinning: 检测 UBO 上限 + 编译 Skin shader + 创建 UBO ----
        InitGPUSkinning();

        // ---- Phase E.1.1 + E.1.2 — 2D Lit 渲染资源 + shader ----
        InitLit2D();

        // ---- Phase E.3.1 — HDR + ACES Tonemap 渲染资源 + shader ----
        InitTonemap();

        // ---- Phase E.4 — Bloom 后处理 shader (依赖 tonemap 的 vaoTonemap) ----
        InitBloom();

        // ---- Phase E.5 — Auto Exposure shader (依赖 tonemap 的 vaoTonemap) ----
        InitAutoExposure();

        // ---- Phase E.6 — Lens Dirt + Streak shader (依赖 tonemap 的 vaoTonemap + Bloom bright pass) ----
        InitLensFx();

        CC::Log(CC::LOG_INFO, "RenderBackend: GL33 Core initialized (GL %s)%s%s%s",
                (const char*)glGetString(GL_VERSION),
                (programUnlit && programPBR) ? ", 3D Unlit+PBR enabled" :
                (programUnlit || programPBR) ? ", partial 3D shader" : "",
                gpuSkinningSupported ? ", GPU skinning enabled" : "",
                lit2DSupported       ? ", Lit2D enabled" : "");
        return true;
    }

    // Phase AW — GPU Skinning 初始化 (检测 + 编译 skin shader + 创建 UBO)
    // 任何步骤失败 => gpuSkinningSupported = false, 不影响其他渲染功能
    void InitGPUSkinning() {
        // 1. 检测 UBO 上限
        GLint maxUniformBlockSize    = 0;
        GLint maxVertexUniformBlocks = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE,    &maxUniformBlockSize);
        glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &maxVertexUniformBlocks);
        constexpr int kRequiredUboBytes = SKIN_MAX_JOINTS * 16 * (int)sizeof(float);  // 4096
        if (maxUniformBlockSize < kRequiredUboBytes || maxVertexUniformBlocks < 1) {
            CC::Log(CC::LOG_WARN, "GL33: GPU skinning unsupported (UBO size=%d, vert blocks=%d)",
                    maxUniformBlockSize, maxVertexUniformBlocks);
            return;
        }

        // 2. 编译 SKIN VS, link 与 FS_UNLIT / FS_PBR 共用
        GLuint vsSkin   = CompileShader(GL_VERTEX_SHADER,   VS3D_SKIN_SOURCE);
        GLuint fsUnlit2 = CompileShader(GL_FRAGMENT_SHADER, FS_UNLIT_SOURCE);
        GLuint fsPBR2   = CompileShader(GL_FRAGMENT_SHADER, FS_PBR_SOURCE);
        if (vsSkin && fsUnlit2) {
            programUnlitSkin = LinkProgram(vsSkin, fsUnlit2);
            if (!programUnlitSkin) CC::Log(CC::LOG_WARN, "GL33: Unlit Skin shader link failed");
        }
        if (vsSkin && fsPBR2) {
            programPBRSkin = LinkProgram(vsSkin, fsPBR2);
            if (!programPBRSkin) CC::Log(CC::LOG_WARN, "GL33: PBR Skin shader link failed");
        }
        if (vsSkin)   glDeleteShader(vsSkin);
        if (fsUnlit2) glDeleteShader(fsUnlit2);
        if (fsPBR2)   glDeleteShader(fsPBR2);
        if (!programUnlitSkin && !programPBRSkin) {
            CC::Log(CC::LOG_WARN, "GL33: GPU skinning shaders failed to link");
            return;
        }

        // 3. 创建 UBO 并预分配 4096 bytes
        glGenBuffers(1, &uboJointMatrices);
        glBindBuffer(GL_UNIFORM_BUFFER, uboJointMatrices);
        glBufferData(GL_UNIFORM_BUFFER, kRequiredUboBytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glGenBuffers(1, &uboPrevJointMatrices);
        glBindBuffer(GL_UNIFORM_BUFFER, uboPrevJointMatrices);
        glBufferData(GL_UNIFORM_BUFFER, kRequiredUboBytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        if (!uboJointMatrices || !uboPrevJointMatrices) {
            CC::Log(CC::LOG_WARN, "GL33: UBO creation failed");
            if (programUnlitSkin) { glDeleteProgram(programUnlitSkin); programUnlitSkin = 0; }
            if (programPBRSkin)   { glDeleteProgram(programPBRSkin);   programPBRSkin   = 0; }
            if (uboJointMatrices) { glDeleteBuffers(1, &uboJointMatrices); uboJointMatrices = 0; }
            if (uboPrevJointMatrices) { glDeleteBuffers(1, &uboPrevJointMatrices); uboPrevJointMatrices = 0; }
            return;
        }
        // 绑定 UBO 到固定 binding point 0
        glBindBufferBase(GL_UNIFORM_BUFFER, UBO_BINDING_POINT, uboJointMatrices);
        glBindBufferBase(GL_UNIFORM_BUFFER, PREV_UBO_BINDING_POINT, uboPrevJointMatrices);

        // 4. 把 program 中的 "JointBlock" uniform block 关联到 binding point 0
        auto bindBlock = [&](GLuint prog) {
            if (!prog) return;
            GLuint blockIdx = glGetUniformBlockIndex(prog, "JointBlock");
            if (blockIdx != GL_INVALID_INDEX) {
                glUniformBlockBinding(prog, blockIdx, UBO_BINDING_POINT);
            }
        };
        auto bindPrevBlock = [&](GLuint prog) {
            if (!prog) return;
            GLuint blockIdx = glGetUniformBlockIndex(prog, "PrevJointBlock");
            if (blockIdx != GL_INVALID_INDEX) {
                glUniformBlockBinding(prog, blockIdx, PREV_UBO_BINDING_POINT);
            }
        };
        bindBlock(programUnlitSkin);
        bindBlock(programPBRSkin);
        bindPrevBlock(programUnlitSkin);
        bindPrevBlock(programPBRSkin);

        gpuSkinningSupported = true;

        // ---- Phase AX: 编译 + 链接 SKIN_MORPH program ----
        // 失败不影响 GPU skinning 本身, 仅 morphTargetsSupported = false (回退 CPU)
        GLuint vsSkinMorph = CompileShader(GL_VERTEX_SHADER, VS3D_SKIN_MORPH_SOURCE);
        GLuint fsUnlit3    = CompileShader(GL_FRAGMENT_SHADER, FS_UNLIT_SOURCE);
        GLuint fsPBR3      = CompileShader(GL_FRAGMENT_SHADER, FS_PBR_SOURCE);
        if (vsSkinMorph && fsUnlit3) {
            programUnlitSkinMorph = LinkProgram(vsSkinMorph, fsUnlit3);
            if (!programUnlitSkinMorph) CC::Log(CC::LOG_WARN, "GL33: Unlit Skin+Morph link failed");
        }
        if (vsSkinMorph && fsPBR3) {
            programPBRSkinMorph = LinkProgram(vsSkinMorph, fsPBR3);
            if (!programPBRSkinMorph) CC::Log(CC::LOG_WARN, "GL33: PBR Skin+Morph link failed");
        }
        if (vsSkinMorph) glDeleteShader(vsSkinMorph);
        if (fsUnlit3)    glDeleteShader(fsUnlit3);
        if (fsPBR3)      glDeleteShader(fsPBR3);
        if (programUnlitSkinMorph || programPBRSkinMorph) {
            // morph program 也要绑定 JointBlock (与 skin 共享 UBO)
            bindBlock(programUnlitSkinMorph);
            bindBlock(programPBRSkinMorph);
            bindPrevBlock(programUnlitSkinMorph);
            bindPrevBlock(programPBRSkinMorph);
            morphTargetsSupported = true;
            CC::Log(CC::LOG_INFO, "GL33: Phase AX Morph Target shader compiled successfully");
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase AX Morph Target unavailable (CPU fallback)");
        }
    }

    // Phase E.1.1 + E.1.2 — 2D Lit 渲染管线初始化
    //
    // 阶段:
    //   1. glGenVertexArrays / glGenBuffers 创建 VAO + 动态 VBO + 静态 EBO  (E.1.1)
    //   2. 配置顶点属性 layout (location 0..4, 与 VS_LIT2D 静态 layout 一致) (E.1.1)
    //   3. 上传单 quad 静态索引 [0,1,2, 0,2,3]                              (E.1.1)
    //   4. 编译 VS_LIT2D + FS_LIT2D, link programLit2D                     (E.1.2)
    //   5. glGetUniformLocation 缓存所有 uniform location                  (E.1.2)
    //   6. 绑定 sampler uniform 到 texture unit 0/1                        (E.1.2)
    //
    // 失败影响: lit2DSupported = false, DrawLit2DQuad 等接口仍是默认 no-op,
    //          调用方应通过 SupportsLit2D() 检查后回退到普通 Draw 路径.
    //
    // GL 对象 (VAO/VBO/EBO) 与 program 任一失败都退化到 false.
    void InitLit2D() {
        glGenVertexArrays(1, &vaoLit2D);
        glGenBuffers(1, &vboLit2D);
        glGenBuffers(1, &eboLit2D);
        if (!vaoLit2D || !vboLit2D || !eboLit2D) {
            CC::Log(CC::LOG_WARN, "GL33: Phase E.1 Lit2D resource allocation failed");
            // 部分失败时清理已创建的对象, 避免泄漏
            if (eboLit2D) { glDeleteBuffers(1, &eboLit2D); eboLit2D = 0; }
            if (vboLit2D) { glDeleteBuffers(1, &vboLit2D); vboLit2D = 0; }
            if (vaoLit2D) { glDeleteVertexArrays(1, &vaoLit2D); vaoLit2D = 0; }
            return;
        }

        glBindVertexArray(vaoLit2D);
        glBindBuffer(GL_ARRAY_BUFFER, vboLit2D);
        // 预分配单 quad 容量 (E.1.5 时可按 vertexCount 动态扩容; 当前足够)
        glBufferData(GL_ARRAY_BUFFER,
                     LIT2D_VBO_INITIAL_VERTS * (GLsizeiptr)sizeof(RenderVertex2DLit),
                     nullptr, GL_DYNAMIC_DRAW);
        vboLit2DCapacity = LIT2D_VBO_INITIAL_VERTS;  // E.1.5 跟踪初始容量

        // 顶点属性 layout (与 VS_LIT2D in 声明保持一致, 静态 location):
        //   location 0: aPos      vec3 (x,y,z)
        //   location 1: aUV       vec2 (u,v)
        //   location 2: aColor    vec4 (r,g,b,a)
        //   location 3: aNormal   vec3 (nx,ny,nz)
        //   location 4: aTangent  vec4 (tx,ty,tz,tw)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, r));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, nx));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, tx));

        // 静态单 quad 索引: 顶点 0,1,2,3 对应左下,右下,右上,左上 → 两个三角形
        const uint32_t kQuadIndices[6] = { 0u, 1u, 2u, 0u, 2u, 3u };
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2D);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kQuadIndices),
                     kQuadIndices, GL_STATIC_DRAW);

        glBindVertexArray(0);

        // Phase E.2.3 — 动态 EBO 在 VAO 解绑后再生成, 避免被 vaoLit2D 记住绑定;
        // 这样 DrawLit2DBatch 可临时切到 eboLit2DBatch, 不污染 DrawLit2DQuad 的 EBO 状态.
        glGenBuffers(1, &eboLit2DBatch);
        if (eboLit2DBatch) {
            // 预分配单 batch 容量 (256 quad ×6 索引 = 1536 idx ≈ 6KB)
            const int kInitIdxCap = 256 * 6;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2DBatch);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         kInitIdxCap * (GLsizeiptr)sizeof(uint32_t),
                         nullptr, GL_DYNAMIC_DRAW);
            eboLit2DBatchCapacity = kInitIdxCap;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }

        // ---- Phase E.1.2 — 编译 + link Lit2D shader ----
        GLuint vsLit = CompileShader(GL_VERTEX_SHADER,   VS_LIT2D_SOURCE);
        GLuint fsLit = CompileShader(GL_FRAGMENT_SHADER, FS_LIT2D_SOURCE);
        if (vsLit && fsLit) {
            programLit2D = LinkProgram(vsLit, fsLit);
        }
        if (vsLit) glDeleteShader(vsLit);
        if (fsLit) glDeleteShader(fsLit);

        if (!programLit2D) {
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.1.2 Lit2D shader compile/link failed, SupportsLit2D=false");
            // GL 对象保留, 由 Shutdown 释放; program=0 即代表 shader 不可用
            lit2DSupported = false;
            return;
        }

        // 缓存所有 uniform location (link 成功后位置稳定)
        locLit2D_MVP            = glGetUniformLocation(programLit2D, "uMVP");
        locLit2D_Model          = glGetUniformLocation(programLit2D, "uModel");
        locLit2D_Texture        = glGetUniformLocation(programLit2D, "uTexture");
        locLit2D_NormalMap      = glGetUniformLocation(programLit2D, "uNormalMap");
        locLit2D_HasNormalMap   = glGetUniformLocation(programLit2D, "uHasNormalMap");
        locLit2D_Ambient        = glGetUniformLocation(programLit2D, "uAmbient");
        locLit2D_LightCount     = glGetUniformLocation(programLit2D, "uLightCount");
        // 数组 uniform: 通过 "name[0]" 取 base location, 之后 glUniform*v(loc, count, data) 一次性上传
        locLit2D_LightType      = glGetUniformLocation(programLit2D, "uLightType[0]");
        locLit2D_LightPos       = glGetUniformLocation(programLit2D, "uLightPos[0]");
        locLit2D_LightDir       = glGetUniformLocation(programLit2D, "uLightDir[0]");
        locLit2D_LightColor     = glGetUniformLocation(programLit2D, "uLightColor[0]");
        locLit2D_LightRange     = glGetUniformLocation(programLit2D, "uLightRange[0]");
        locLit2D_LightIntensity = glGetUniformLocation(programLit2D, "uLightIntensity[0]");
        locLit2D_LightInnerCos  = glGetUniformLocation(programLit2D, "uLightInnerCos[0]");
        locLit2D_LightOuterCos  = glGetUniformLocation(programLit2D, "uLightOuterCos[0]");

        // 一次性把 sampler uniform 绑到 texture unit 0/1 (link 后位置稳定, 后续 Draw 不需重设)
        glUseProgram(programLit2D);
        if (locLit2D_Texture   >= 0) glUniform1i(locLit2D_Texture,   0);
        if (locLit2D_NormalMap >= 0) glUniform1i(locLit2D_NormalMap, 1);
        glUseProgram(0);

        lit2DSupported = true;
        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.1.2 Lit2D ready (program=%u, MAX_LIGHTS=%d)",
                programLit2D, LIT2D_MAX_LIGHTS);
    }

    // ==================== Phase E.3.1 — Tonemap 初始化 ====================
    //
    // 步骤:
    //   1. 创建全屏 quad VAO + VBO (6 顶点 = 2 三角形; pos.xy + uv.xy, 无 EBO)
    //   2. 上传静态顶点数据 ([-1..1] NDC + [0..1] UV)
    //   3. 配置顶点属性 layout (location 0 = pos, location 1 = uv)
    //   4. 编译 VS_TONEMAP + FS_TONEMAP, link programTonemap
    //   5. 缓存 uniform location (uHDRTex / uExposure / uGamma)
    //   6. 绑定 sampler uniform 到 texture unit 0
    //
    // 失败影响: tonemapSupported = false, SupportsHDR() 返回 false,
    //          HDR 管线整体不可用, 调用方 (HDRRenderer) 应回退 LDR 路径.
    void InitTonemap() {
        glGenVertexArrays(1, &vaoTonemap);
        glGenBuffers(1, &vboTonemap);

        if (!vaoTonemap || !vboTonemap) {
            CC::Log(CC::LOG_WARN, "GL33: Phase E.3.1 Tonemap VAO/VBO allocation failed");
            if (vboTonemap) { glDeleteBuffers(1, &vboTonemap); vboTonemap = 0; }
            if (vaoTonemap) { glDeleteVertexArrays(1, &vaoTonemap); vaoTonemap = 0; }
            return;
        }

        // 全屏 quad: 2 三角形, 6 顶点. 每顶点 4 float (pos.xy + uv.xy).
        // UV.y 翻转: OpenGL 纹理坐标原点在左下, 我们的 HDR RT 内容也是 bottom-up,
        // 所以 UV 直接用 [0..1] 不翻转 (与 Canvas 采样一致).
        static const float kFullscreenQuad[] = {
            // pos.xy       uv.xy
            -1.0f, -1.0f,   0.0f, 0.0f,
             1.0f, -1.0f,   1.0f, 0.0f,
            -1.0f,  1.0f,   0.0f, 1.0f,
            -1.0f,  1.0f,   0.0f, 1.0f,
             1.0f, -1.0f,   1.0f, 0.0f,
             1.0f,  1.0f,   1.0f, 1.0f,
        };

        glBindVertexArray(vaoTonemap);
        glBindBuffer(GL_ARRAY_BUFFER, vboTonemap);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuad), kFullscreenQuad, GL_STATIC_DRAW);

        // layout(location=0) vec2 aPos
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (const void*)0);
        // layout(location=1) vec2 aUV
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (const void*)(2 * sizeof(float)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // 编译 + link shader
        GLuint vsTm = CompileShader(GL_VERTEX_SHADER,   VS_TONEMAP_SOURCE);
        GLuint fsTm = CompileShader(GL_FRAGMENT_SHADER, FS_TONEMAP_SOURCE);
        if (vsTm && fsTm) {
            programTonemap = LinkProgram(vsTm, fsTm);
        }
        if (vsTm) glDeleteShader(vsTm);
        if (fsTm) glDeleteShader(fsTm);

        if (!programTonemap) {
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.3.1 Tonemap shader compile/link failed, SupportsHDR=false");
            // VAO/VBO 保留, 由 Shutdown 释放
            tonemapSupported = false;
            return;
        }

        // 缓存 uniform location
        locTonemap_HDRTex      = glGetUniformLocation(programTonemap, "uHDRTex");
        locTonemap_Exposure    = glGetUniformLocation(programTonemap, "uExposure");
        locTonemap_Gamma       = glGetUniformLocation(programTonemap, "uGamma");
        locTonemap_Mode        = glGetUniformLocation(programTonemap, "uTonemapMode");
        // Phase F.0.10.8: 3 个 LUT uniform (用 sampler3D 占 unit 1)
        locTonemap_LUT         = glGetUniformLocation(programTonemap, "uLUT");
        locTonemap_LUTStrength = glGetUniformLocation(programTonemap, "uLUTStrength");
        locTonemap_LUTEnabled  = glGetUniformLocation(programTonemap, "uLUTEnabled");

        // 一次性绑 sampler 到 texture unit (HDR=unit0, LUT=unit1)
        glUseProgram(programTonemap);
        if (locTonemap_HDRTex >= 0) glUniform1i(locTonemap_HDRTex, 0);
        if (locTonemap_LUT    >= 0) glUniform1i(locTonemap_LUT,    1);
        // 默认 LUT 不启用 (零回归: 无 LUT 用户调用走 fullscreen 路径行为不变)
        if (locTonemap_LUTEnabled  >= 0) glUniform1i(locTonemap_LUTEnabled,  0);
        if (locTonemap_LUTStrength >= 0) glUniform1f(locTonemap_LUTStrength, 0.0f);
        glUseProgram(0);

        tonemapSupported = true;
        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.3.1 Tonemap ready (program=%u, ACES fitted)",
                programTonemap);
    }

    // ==================== Phase E.4 — Bloom 后处理 shader 初始化 ====================
    //
    // 流程 (与 InitTonemap 类似, 但共用 vaoTonemap/vboTonemap, 无需再建全屏 quad):
    //   1. 依赖 tonemapSupported = true (bloom 需要 HDR RT 和全屏 quad)
    //   2. 编译 3 个 FS shader (VS 复用 VS_TONEMAP_SOURCE)
    //   3. link programBloomBright / programBloomDown / programBloomUp
    //   4. 缓存 uniform location
    //   5. 绑 sampler slot 到 texture unit 0
    //
    // 失败影响: bloomSupported = false; SupportsBloom() 返回 false;
    //          BloomRenderer 应静默 fallback (HDR 管线仍可用, 无 bloom 效果).
    void InitBloom() {
        if (!tonemapSupported) {
            // tonemap 初始化失败 → bloom 必然无法工作 (依赖 HDR RT + 全屏 quad)
            bloomSupported = false;
            return;
        }

        // --- 编译 + link 3 个 program (失败时独立记录, 不影响其它) ---
        auto buildProgram = [this](const char* fsSrc, const char* name) -> GLuint {
            GLuint vs = CompileShader(GL_VERTEX_SHADER,   VS_TONEMAP_SOURCE);
            GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
            GLuint prog = 0;
            if (vs && fs) prog = LinkProgram(vs, fs);
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            if (!prog) {
                CC::Log(CC::LOG_WARN,
                        "GL33: Phase E.4 Bloom shader '%s' compile/link failed", name);
            }
            return prog;
        };

        programBloomBright = buildProgram(FS_BLOOM_BRIGHT_SOURCE, "BrightPass");
        programBloomDown   = buildProgram(FS_BLOOM_DOWN_SOURCE,   "Downsample");
        programBloomUp     = buildProgram(FS_BLOOM_UP_SOURCE,     "Upsample");

        if (!programBloomBright || !programBloomDown || !programBloomUp) {
            // 部分失败: 释放已成功的, 全部禁用
            if (programBloomBright) { glDeleteProgram(programBloomBright); programBloomBright = 0; }
            if (programBloomDown)   { glDeleteProgram(programBloomDown);   programBloomDown   = 0; }
            if (programBloomUp)     { glDeleteProgram(programBloomUp);     programBloomUp     = 0; }
            bloomSupported = false;
            CC::Log(CC::LOG_WARN, "GL33: Phase E.4 Bloom disabled (partial shader failure)");
            return;
        }

        // --- 缓存 uniform location + 绑定 sampler slot ---
        locBloomBright_Src       = glGetUniformLocation(programBloomBright, "uSrc");
        locBloomBright_Threshold = glGetUniformLocation(programBloomBright, "uThreshold");
        glUseProgram(programBloomBright);
        if (locBloomBright_Src >= 0) glUniform1i(locBloomBright_Src, 0);

        locBloomDown_Src      = glGetUniformLocation(programBloomDown, "uSrc");
        locBloomDown_Texel    = glGetUniformLocation(programBloomDown, "uTexel");
        locBloomDown_UvBounds = glGetUniformLocation(programBloomDown, "uUvBounds");   // Phase F.0.10.5
        glUseProgram(programBloomDown);
        if (locBloomDown_Src >= 0) glUniform1i(locBloomDown_Src, 0);

        locBloomUp_Src       = glGetUniformLocation(programBloomUp, "uSrc");
        locBloomUp_Texel     = glGetUniformLocation(programBloomUp, "uTexel");
        locBloomUp_Radius    = glGetUniformLocation(programBloomUp, "uRadius");
        locBloomUp_Intensity = glGetUniformLocation(programBloomUp, "uIntensity");
        locBloomUp_UvBounds  = glGetUniformLocation(programBloomUp, "uUvBounds");      // Phase F.0.10.5
        glUseProgram(programBloomUp);
        if (locBloomUp_Src >= 0) glUniform1i(locBloomUp_Src, 0);

        glUseProgram(0);

        bloomSupported = true;
        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.4 Bloom ready (bright=%u, down=%u, up=%u)",
                programBloomBright, programBloomDown, programBloomUp);
    }

    // ==================== Phase E.5 — Auto Exposure shader 初始化 ====================
    //
    // 流程 (与 InitBloom 类似, 共用 vaoTonemap 全屏 quad):
    //   1. 依赖 tonemapSupported = true (luma extract 需要 HDR RT 输入)
    //   2. 编译 1 个 FS shader (VS 复用 VS_TONEMAP_SOURCE)
    //   3. link programLumaExtract + 缓存 uniform location + 绑 sampler slot
    //
    // 失败影响: autoExposureSupported = false, SupportsAutoExposure() 返回 false,
    //          AutoExposureRenderer 应静默 fallback (HDR 管线仍可用, 无 AE 效果).
    void InitAutoExposure() {
        if (!tonemapSupported) {
            // tonemap 初始化失败 → AE 必然无法工作 (依赖 HDR RT + 全屏 quad)
            autoExposureSupported = false;
            return;
        }

        // --- 编译 + link 1 个 program ---
        GLuint vs = CompileShader(GL_VERTEX_SHADER,   VS_TONEMAP_SOURCE);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FS_LUMINANCE_EXTRACT_SOURCE);
        if (vs && fs) programLumaExtract = LinkProgram(vs, fs);
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);

        if (!programLumaExtract) {
            autoExposureSupported = false;
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.5 Auto Exposure disabled (luma extract shader failed)");
            return;
        }

        // --- 缓存 uniform location + 绑定 sampler slot ---
        locLumaExtract_HDRTex = glGetUniformLocation(programLumaExtract, "uHDRTex");
        glUseProgram(programLumaExtract);
        if (locLumaExtract_HDRTex >= 0) glUniform1i(locLumaExtract_HDRTex, 0);
        glUseProgram(0);

        autoExposureSupported = true;
        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.5 Auto Exposure ready (lumaExtract=%u)",
                programLumaExtract);
    }

    // ==================== Phase E.6 — Lens Dirt + Streak shader 初始化 ====================
    //
    // 流程:
    //   1. 依赖 tonemapSupported = true (共用 vaoTonemap 全屏 quad)
    //   2. 编译 3 个 FS shader (VS 复用 VS_TONEMAP_SOURCE):
    //      - programLensDirt       (bloom x dirt x intensity)
    //      - programStreakBlur     (7-tap 方向高斯)
    //      - programStreakComposite (streak x intensity)
    //   3. 缓存 uniform location + 绑定 sampler slot
    //   4. 创建 1x1 白纹理 fallback (LensDirt.SetDirtTexture 未设时用)
    //
    // 失败影响:
    //   - LensDirt shader fail → lensDirtSupported=false (LensDirt API 降级)
    //   - Streak shader (blur 或 composite) fail → streakSupported=false
    //   - 两者独立, Bloom bright pass 作 streak bright 复用, 不需额外 shader
    void InitLensFx() {
        if (!tonemapSupported) {
            lensDirtSupported = false;
            streakSupported = false;
            return;
        }

        auto buildProgram = [this](const char* fsSrc, const char* name) -> GLuint {
            GLuint vs = CompileShader(GL_VERTEX_SHADER,   VS_TONEMAP_SOURCE);
            GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
            GLuint prog = 0;
            if (vs && fs) prog = LinkProgram(vs, fs);
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            if (!prog) {
                CC::Log(CC::LOG_WARN,
                        "GL33: Phase E.6 LensFx shader '%s' compile/link failed", name);
            }
            return prog;
        };

        // --- 1x1 白纹理 fallback ---
        glGenTextures(1, &whiteTex1x1);
        glBindTexture(GL_TEXTURE_2D, whiteTex1x1);
        uint8_t whitePixel[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, whitePixel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // --- Lens Dirt ---
        programLensDirt = buildProgram(FS_LENS_DIRT_COMPOSITE_SOURCE, "LensDirtComposite");
        if (programLensDirt) {
            locLensDirt_BloomTex  = glGetUniformLocation(programLensDirt, "uBloomTex");
            locLensDirt_DirtTex   = glGetUniformLocation(programLensDirt, "uDirtTex");
            locLensDirt_Intensity = glGetUniformLocation(programLensDirt, "uIntensity");
            glUseProgram(programLensDirt);
            if (locLensDirt_BloomTex >= 0) glUniform1i(locLensDirt_BloomTex, 0);
            if (locLensDirt_DirtTex  >= 0) glUniform1i(locLensDirt_DirtTex, 1);
            glUseProgram(0);
            lensDirtSupported = true;
        } else {
            lensDirtSupported = false;
        }

        // --- Streak Blur + Composite (两者需同时成功, 否则 streak 整体不可用) ---
        programStreakBlur      = buildProgram(FS_STREAK_BLUR_SOURCE,      "StreakBlur");
        programStreakComposite = buildProgram(FS_STREAK_COMPOSITE_SOURCE, "StreakComposite");

        if (programStreakBlur && programStreakComposite) {
            locStreakBlur_Src        = glGetUniformLocation(programStreakBlur, "uSrc");
            locStreakBlur_Texel      = glGetUniformLocation(programStreakBlur, "uTexel");
            locStreakBlur_Length     = glGetUniformLocation(programStreakBlur, "uLength");
            locStreakBlur_Direction  = glGetUniformLocation(programStreakBlur, "uDirection");
            glUseProgram(programStreakBlur);
            if (locStreakBlur_Src >= 0) glUniform1i(locStreakBlur_Src, 0);

            locStreakComposite_Src       = glGetUniformLocation(programStreakComposite, "uSrc");
            locStreakComposite_Intensity = glGetUniformLocation(programStreakComposite, "uIntensity");
            glUseProgram(programStreakComposite);
            if (locStreakComposite_Src >= 0) glUniform1i(locStreakComposite_Src, 0);
            glUseProgram(0);

            streakSupported = true;
        } else {
            if (programStreakBlur)      { glDeleteProgram(programStreakBlur);      programStreakBlur = 0; }
            if (programStreakComposite) { glDeleteProgram(programStreakComposite); programStreakComposite = 0; }
            streakSupported = false;
        }

        // --- Phase E.7 — Lens Flare Ghost shader (bright/composite 复用 Bloom) ---
        programLensFlareGhost = buildProgram(FS_LENS_FLARE_GHOST_SOURCE, "LensFlareGhost");
        if (programLensFlareGhost) {
            locLensFlare_BrightTex      = glGetUniformLocation(programLensFlareGhost, "uBrightTex");
            locLensFlare_FlareTex       = glGetUniformLocation(programLensFlareGhost, "uFlareTex");
            locLensFlare_GhostCount     = glGetUniformLocation(programLensFlareGhost, "uGhostCount");
            locLensFlare_GhostDispersal = glGetUniformLocation(programLensFlareGhost, "uGhostDispersal");
            locLensFlare_HaloWidth      = glGetUniformLocation(programLensFlareGhost, "uHaloWidth");
            locLensFlare_Aberration     = glGetUniformLocation(programLensFlareGhost, "uChromaticAberration");
            locLensFlare_DistortionEn   = glGetUniformLocation(programLensFlareGhost, "uDistortionEnabled");
            glUseProgram(programLensFlareGhost);
            if (locLensFlare_BrightTex >= 0) glUniform1i(locLensFlare_BrightTex, 0);   // slot 0
            if (locLensFlare_FlareTex  >= 0) glUniform1i(locLensFlare_FlareTex,  1);   // slot 1
            glUseProgram(0);
            // Lens Flare 需要 Bloom bright/composite 都可用 (复用)
            lensFlareSupported = bloomSupported;
        } else {
            lensFlareSupported = false;
        }

        // --- Phase E.8 — SSAO (3 shader: raw + blur + composite) ---
        programSSAO          = buildProgram(FS_SSAO_SOURCE,           "SSAO");
        programSSAOBlur      = buildProgram(FS_SSAO_BLUR_SOURCE,      "SSAOBlur");
        programSSAOComposite = buildProgram(FS_SSAO_COMPOSITE_SOURCE, "SSAOComposite");
        if (programSSAO && programSSAOBlur && programSSAOComposite) {
            // Cache uniform locations (avoid 每帧 glGetUniformLocation 开销)
            locSSAO_DepthTex   = glGetUniformLocation(programSSAO, "uDepthTex");
            locSSAO_NoiseTex   = glGetUniformLocation(programSSAO, "uNoiseTex");
            locSSAO_Proj       = glGetUniformLocation(programSSAO, "uProj");
            locSSAO_InvProj    = glGetUniformLocation(programSSAO, "uInvProj");
            locSSAO_Kernel     = glGetUniformLocation(programSSAO, "uKernel[0]");
            locSSAO_KernelSize = glGetUniformLocation(programSSAO, "uKernelSize");
            locSSAO_Radius     = glGetUniformLocation(programSSAO, "uRadius");
            locSSAO_Bias       = glGetUniformLocation(programSSAO, "uBias");
            locSSAO_Power      = glGetUniformLocation(programSSAO, "uPower");
            locSSAO_NoiseScale = glGetUniformLocation(programSSAO, "uNoiseScale");
            locSSAO_NormalTex  = glGetUniformLocation(programSSAO, "uNormalTex");  // Phase E.8.x
            glUseProgram(programSSAO);
            if (locSSAO_DepthTex  >= 0) glUniform1i(locSSAO_DepthTex,  0);  // slot 0
            if (locSSAO_NoiseTex  >= 0) glUniform1i(locSSAO_NoiseTex,  1);  // slot 1
            if (locSSAO_NormalTex >= 0) glUniform1i(locSSAO_NormalTex, 2);  // slot 2 (Phase E.8.x)

            locSSAOBlur_SSAOTex  = glGetUniformLocation(programSSAOBlur, "uSSAOTex");
            locSSAOBlur_DepthTex = glGetUniformLocation(programSSAOBlur, "uDepthTex");
            locSSAOBlur_Texel    = glGetUniformLocation(programSSAOBlur, "uTexel");
            locSSAOBlur_Axis     = glGetUniformLocation(programSSAOBlur, "uAxis");
            glUseProgram(programSSAOBlur);
            if (locSSAOBlur_SSAOTex  >= 0) glUniform1i(locSSAOBlur_SSAOTex,  0);
            if (locSSAOBlur_DepthTex >= 0) glUniform1i(locSSAOBlur_DepthTex, 1);

            locSSAOComp_SceneTex  = glGetUniformLocation(programSSAOComposite, "uSceneTex");
            locSSAOComp_AOTex     = glGetUniformLocation(programSSAOComposite, "uAOTex");
            locSSAOComp_Intensity = glGetUniformLocation(programSSAOComposite, "uIntensity");
            glUseProgram(programSSAOComposite);
            if (locSSAOComp_SceneTex >= 0) glUniform1i(locSSAOComp_SceneTex, 0);
            if (locSSAOComp_AOTex    >= 0) glUniform1i(locSSAOComp_AOTex,    1);
            glUseProgram(0);

            // SSAO 需要 HDR (tonemapSupported) + Bloom (bloomSupported) 都可用
            // (HDR 提供 depth RT 源; Bloom 不是直接依赖但能保证 shader 基础设施就位)
            ssaoSupported = tonemapSupported;
        } else {
            ssaoSupported = false;
        }

        // --- Phase E.9 — SSR (2 shader: raw + composite) ---
        programSSR          = buildProgram(FS_SSR_SOURCE,           "SSR");
        programSSRComposite = buildProgram(FS_SSR_COMPOSITE_SOURCE, "SSRComposite");
        if (programSSR && programSSRComposite) {
            // Cache FS_SSR uniforms
            locSSR_DepthTex    = glGetUniformLocation(programSSR, "uDepthTex");
            locSSR_NormalTex   = glGetUniformLocation(programSSR, "uNormalTex");
            locSSR_HDRTex      = glGetUniformLocation(programSSR, "uHDRTex");
            locSSR_Proj        = glGetUniformLocation(programSSR, "uProj");
            locSSR_InvProj     = glGetUniformLocation(programSSR, "uInvProj");
            locSSR_MaxSteps    = glGetUniformLocation(programSSR, "uMaxSteps");
            locSSR_StepSize    = glGetUniformLocation(programSSR, "uStepSize");
            locSSR_Thickness   = glGetUniformLocation(programSSR, "uThickness");
            locSSR_MaxDistance = glGetUniformLocation(programSSR, "uMaxDistance");
            locSSR_EdgeFade    = glGetUniformLocation(programSSR, "uEdgeFade");
            locSSR_JitterOffset = glGetUniformLocation(programSSR, "uJitterOffset");  // Phase E.12
            glUseProgram(programSSR);
            if (locSSR_DepthTex  >= 0) glUniform1i(locSSR_DepthTex,  0);  // slot 0
            if (locSSR_NormalTex >= 0) glUniform1i(locSSR_NormalTex, 1);  // slot 1
            if (locSSR_HDRTex    >= 0) glUniform1i(locSSR_HDRTex,    2);  // slot 2
            // Phase E.12 默认 jitter=0 (= Phase E.11 行为, 调用方未传时可预期)
            if (locSSR_JitterOffset >= 0) glUniform2f(locSSR_JitterOffset, 0.0f, 0.0f);

            // Cache FS_SSR_COMPOSITE uniforms
            locSSRComp_SceneTex   = glGetUniformLocation(programSSRComposite, "uSceneTex");
            locSSRComp_ReflectTex = glGetUniformLocation(programSSRComposite, "uReflectTex");
            locSSRComp_Intensity  = glGetUniformLocation(programSSRComposite, "uIntensity");
            glUseProgram(programSSRComposite);
            if (locSSRComp_SceneTex   >= 0) glUniform1i(locSSRComp_SceneTex,   0);
            if (locSSRComp_ReflectTex >= 0) glUniform1i(locSSRComp_ReflectTex, 1);
            glUseProgram(0);

            // SSR 需要 HDR + G-buffer normal MRT (Phase E.8.x). tonemapSupported 已包含 RGBA16F 探测
            ssrSupported = tonemapSupported;
        } else {
            ssrSupported = false;
        }

        // --- Phase E.10 — SSR Blur (反射模糊, half-res ping-pong; 用户拍板 2026-05-12) ---
        // Phase E.11 — 同一 program 双模式 (uBilateral runtime 切换)
        programSSRBlur = buildProgram(FS_SSR_BLUR_SOURCE, "SSRBlur");
        if (programSSRBlur && ssrSupported) {
            locSSRBlur_SrcTex     = glGetUniformLocation(programSSRBlur, "uSrcTex");
            locSSRBlur_DepthTex   = glGetUniformLocation(programSSRBlur, "uDepthTex");    // Phase E.11
            locSSRBlur_Texel      = glGetUniformLocation(programSSRBlur, "uTexel");
            locSSRBlur_Axis       = glGetUniformLocation(programSSRBlur, "uAxis");
            locSSRBlur_Radius     = glGetUniformLocation(programSSRBlur, "uRadius");
            locSSRBlur_Bilateral  = glGetUniformLocation(programSSRBlur, "uBilateral");   // Phase E.11
            locSSRBlur_DepthSigma = glGetUniformLocation(programSSRBlur, "uDepthSigma");  // Phase E.11
            glUseProgram(programSSRBlur);
            if (locSSRBlur_SrcTex   >= 0) glUniform1i(locSSRBlur_SrcTex,   0);  // slot 0
            if (locSSRBlur_DepthTex >= 0) glUniform1i(locSSRBlur_DepthTex, 1);  // slot 1 (Phase E.11)
            glUseProgram(0);
            ssrBlurSupported = true;
        } else {
            ssrBlurSupported = false;
        }

        // --- Phase E.12 — Temporal SSR (reverse-reprojection + neighborhood clip) ---
        // 用户拍板 2026-05-14: full-res RGBA16F history × 2 + Halton-2,3 8-sample + neighborhood clip
        programSSRTemporal = buildProgram(FS_SSR_TEMPORAL_SOURCE, "SSRTemporal");
        if (programSSRTemporal && ssrSupported) {
            locSSRTemporal_CurReflectTex = glGetUniformLocation(programSSRTemporal, "uCurReflectTex");
            locSSRTemporal_HistoryTex    = glGetUniformLocation(programSSRTemporal, "uHistoryTex");
            locSSRTemporal_DepthTex      = glGetUniformLocation(programSSRTemporal, "uDepthTex");
            locSSRTemporal_VelocityTex   = glGetUniformLocation(programSSRTemporal, "uVelocityTex");
            locSSRTemporal_ReprojectMat  = glGetUniformLocation(programSSRTemporal, "uReprojectMat");
            locSSRTemporal_InvProj       = glGetUniformLocation(programSSRTemporal, "uInvProj");
            locSSRTemporal_Texel         = glGetUniformLocation(programSSRTemporal, "uTexel");
            locSSRTemporal_BlendAlpha    = glGetUniformLocation(programSSRTemporal, "uBlendAlpha");
            locSSRTemporal_RejectionMode = glGetUniformLocation(programSSRTemporal, "uRejectionMode");
            locSSRTemporal_HasHistory    = glGetUniformLocation(programSSRTemporal, "uHasHistory");
            locSSRTemporal_HasVelocityTex = glGetUniformLocation(programSSRTemporal, "uHasVelocityTex");
            // Phase E.14 — dilation/format/scale uniform location
            locSSRTemporal_VelocityDilation = glGetUniformLocation(programSSRTemporal, "uVelocityDilation");
            locSSRTemporal_VelocityFormat   = glGetUniformLocation(programSSRTemporal, "uVelocityFormat");
            locSSRTemporal_VelocityScale    = glGetUniformLocation(programSSRTemporal, "uVelocityScale");
            glUseProgram(programSSRTemporal);
            if (locSSRTemporal_CurReflectTex >= 0) glUniform1i(locSSRTemporal_CurReflectTex, 0);  // slot 0
            if (locSSRTemporal_HistoryTex    >= 0) glUniform1i(locSSRTemporal_HistoryTex,    1);  // slot 1
            if (locSSRTemporal_DepthTex      >= 0) glUniform1i(locSSRTemporal_DepthTex,      2);  // slot 2
            if (locSSRTemporal_VelocityTex   >= 0) glUniform1i(locSSRTemporal_VelocityTex,   3);
            glUseProgram(0);
            ssrTemporalSupported = true;
        } else {
            ssrTemporalSupported = false;
        }

        // ---- Phase E.15 — Motion Blur shader ----
        // 编译失败仅 motionBlurSupported=false; 不影响其他后处理.
        // 复用 VS_TONEMAP_SOURCE 全屏 quad VS, FS = FS_MOTION_BLUR_SOURCE
        programMotionBlur = buildProgram(FS_MOTION_BLUR_SOURCE, "MotionBlur");
        if (programMotionBlur) {
            locMB_SceneTex            = glGetUniformLocation(programMotionBlur, "uSceneTex");
            locMB_VelocityTex         = glGetUniformLocation(programMotionBlur, "uVelocityTex");
            locMB_CameraVelocityTex   = glGetUniformLocation(programMotionBlur, "uCameraVelocityTex");  // ★ E.16
            locMB_Texel               = glGetUniformLocation(programMotionBlur, "uTexel");
            locMB_Strength            = glGetUniformLocation(programMotionBlur, "uStrength");
            locMB_SampleCount         = glGetUniformLocation(programMotionBlur, "uSampleCount");
            locMB_VelocityDilation    = glGetUniformLocation(programMotionBlur, "uVelocityDilation");
            locMB_VelocityFormat      = glGetUniformLocation(programMotionBlur, "uVelocityFormat");
            locMB_VelocityScale       = glGetUniformLocation(programMotionBlur, "uVelocityScale");
            locMB_Mode                = glGetUniformLocation(programMotionBlur, "uMode");                // ★ E.16
            // 一次性绑 sampler 到 texture unit (slot 0=scene, slot 1=velocity, slot 2=cameraVelocity)
            glUseProgram(programMotionBlur);
            if (locMB_SceneTex          >= 0) glUniform1i(locMB_SceneTex,          0);
            if (locMB_VelocityTex       >= 0) glUniform1i(locMB_VelocityTex,       1);
            if (locMB_CameraVelocityTex >= 0) glUniform1i(locMB_CameraVelocityTex, 2);   // ★ E.16
            glUseProgram(0);
            motionBlurSupported = true;
        } else {
            motionBlurSupported = false;
        }

        // ---- Phase E.18 — Independent Velocity Dilation Pass shader ----
        // 编译失败仅 velocityDilateSupported=false; HDRRenderer 走 inline 9-tap fallback.
        // 复用 VS_TONEMAP_SOURCE 全屏 quad VS, FS = FS_VELOCITY_DILATE_SOURCE
        programVelocityDilate = buildProgram(FS_VELOCITY_DILATE_SOURCE, "VelocityDilate");
        if (programVelocityDilate) {
            locVDilate_SrcVelocityTex  = glGetUniformLocation(programVelocityDilate, "uSrcVelocityTex");
            locVDilate_Texel           = glGetUniformLocation(programVelocityDilate, "uTexel");
            locVDilate_VelocityFormat  = glGetUniformLocation(programVelocityDilate, "uVelocityFormat");
            locVDilate_VelocityScale   = glGetUniformLocation(programVelocityDilate, "uVelocityScale");
            // 一次性绑 sampler 到 slot 0
            glUseProgram(programVelocityDilate);
            if (locVDilate_SrcVelocityTex >= 0) glUniform1i(locVDilate_SrcVelocityTex, 0);
            glUseProgram(0);
            velocityDilateSupported = true;
            CC::Log(CC::LOG_INFO, "GL33: Phase E.18 velocity dilation pass shader compiled (program=%u)",
                    programVelocityDilate);
        } else {
            velocityDilateSupported = false;
            CC::Log(CC::LOG_WARN, "GL33: Phase E.18 velocity dilation pass shader compile failed; fallback to inline 9-tap");
        }

        // ---- Phase F.0 — TAA Master Pipeline shader ----
        // 编译失败仅 taaSupported=false; TAARenderer Enable 时检测会拒绝启用 (silent fallback).
        // 复用 VS_TONEMAP_SOURCE 全屏 quad VS, FS = FS_TAA_SOURCE
        programTAA = buildProgram(FS_TAA_SOURCE, "TAA");
        if (programTAA) {
            locTAA_CurHdrTex        = glGetUniformLocation(programTAA, "uCurHdrTex");
            locTAA_HistoryTex       = glGetUniformLocation(programTAA, "uHistoryTex");
            locTAA_VelocityTex      = glGetUniformLocation(programTAA, "uVelocityTex");
            locTAA_Texel            = glGetUniformLocation(programTAA, "uTexel");
            locTAA_BlendAlpha       = glGetUniformLocation(programTAA, "uBlendAlpha");
            locTAA_NeighborhoodClip = glGetUniformLocation(programTAA, "uNeighborhoodClip");
            locTAA_HasHistory       = glGetUniformLocation(programTAA, "uHasHistory");
            locTAA_VelocityDilation = glGetUniformLocation(programTAA, "uVelocityDilation");
            locTAA_VelocityFormat   = glGetUniformLocation(programTAA, "uVelocityFormat");
            locTAA_VelocityScale    = glGetUniformLocation(programTAA, "uVelocityScale");
            locTAA_AntiFlicker      = glGetUniformLocation(programTAA, "uAntiFlicker");     // Phase F.0.4
            locTAA_ClipMode         = glGetUniformLocation(programTAA, "uClipMode");        // Phase F.0.2/F.0.3
            locTAA_VarianceGamma    = glGetUniformLocation(programTAA, "uVarianceGamma");   // Phase F.0.3
            locTAA_MotionGamma         = glGetUniformLocation(programTAA, "uMotionGamma");          // Phase F.0.8
            locTAA_MotionAdaptiveGamma = glGetUniformLocation(programTAA, "uMotionAdaptiveGamma");  // Phase F.0.8
            locTAA_UvBounds            = glGetUniformLocation(programTAA, "uUvBounds");             // Phase F.0.10.5
            // 一次性绑 sampler 到 texture unit (slot 0=cur HDR, slot 1=history, slot 2=velocity)
            glUseProgram(programTAA);
            if (locTAA_CurHdrTex   >= 0) glUniform1i(locTAA_CurHdrTex,   0);
            if (locTAA_HistoryTex  >= 0) glUniform1i(locTAA_HistoryTex,  1);
            if (locTAA_VelocityTex >= 0) glUniform1i(locTAA_VelocityTex, 2);
            glUseProgram(0);
            taaSupported = true;
            CC::Log(CC::LOG_INFO, "GL33: Phase F.0 TAA shader compiled (program=%u)", programTAA);
        } else {
            taaSupported = false;
            CC::Log(CC::LOG_WARN, "GL33: Phase F.0 TAA shader compile failed; TAA Enable 将返 false");
        }

        // ---- Phase F.0.1 — TAA Sharpening shader ----
        //   复用 SupportsTAA() 能力位; 编译失败时 DrawTAASharpenPass 内部 fallback 走 BlitTAAToHDR
        //   shader: FS_SHARPEN_SOURCE (4-tap unsharp mask)
        programSharpen = buildProgram(FS_SHARPEN_SOURCE, "TAA_Sharpen");
        if (programSharpen) {
            locSharpen_InputTex  = glGetUniformLocation(programSharpen, "uInputTex");
            locSharpen_TexelSize = glGetUniformLocation(programSharpen, "uTexelSize");
            locSharpen_Sharpness = glGetUniformLocation(programSharpen, "uSharpness");
            locSharpen_UvBounds  = glGetUniformLocation(programSharpen, "uUvBounds");   // Phase F.0.10.5
            // 一次性绑 sampler slot 0
            glUseProgram(programSharpen);
            if (locSharpen_InputTex >= 0) glUniform1i(locSharpen_InputTex, 0);
            glUseProgram(0);
            CC::Log(CC::LOG_INFO, "GL33: Phase F.0.1 TAA Sharpen shader compiled (program=%u)", programSharpen);
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase F.0.1 TAA Sharpen shader compile failed; DrawTAASharpenPass 将 fallback 走 Blit");
        }

        // ---- Phase F.0.6 — TAA CAS Sharpening shader (AMD FSR1 5-tap) ----
        //   与 F.0.1 unsharp 共存, 用户通过 SetSharpenMode("cas"/"unsharp") 切换
        //   shader: FS_CAS_SOURCE (5-tap CAS, contrast-adaptive)
        programCAS = buildProgram(FS_CAS_SOURCE, "TAA_CAS");
        if (programCAS) {
            locCAS_InputTex  = glGetUniformLocation(programCAS, "uInputTex");
            locCAS_TexelSize = glGetUniformLocation(programCAS, "uTexelSize");
            locCAS_Sharpness = glGetUniformLocation(programCAS, "uSharpness");
            glUseProgram(programCAS);
            if (locCAS_InputTex >= 0) glUniform1i(locCAS_InputTex, 0);
            glUseProgram(0);
            CC::Log(CC::LOG_INFO, "GL33: Phase F.0.6 TAA CAS shader compiled (program=%u)", programCAS);
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase F.0.6 TAA CAS shader compile failed; DrawTAACASPass 将 fallback 走 Blit");
        }

        // ---- Phase F.0.9 — TAA Custom Upsampler (Catmull-Rom bicubic) ----
        //   仅 sharpness=0 && halfRes=true && upscaleMode==1 路径使用
        //   编译失败时 DrawTAAUpscalePass fallback 走 BlitTAAToHDR (bilinear stretch)
        programBicubicUpscale = buildProgram(FS_BICUBIC_UPSCALE_SOURCE, "TAA_BicubicUpscale");
        if (programBicubicUpscale) {
            locBicubic_InputTex = glGetUniformLocation(programBicubicUpscale, "uInputTex");
            locBicubic_Texel    = glGetUniformLocation(programBicubicUpscale, "uTexel");
            // 默认 sampler 绑 slot 0
            glUseProgram(programBicubicUpscale);
            if (locBicubic_InputTex >= 0) glUniform1i(locBicubic_InputTex, 0);
            glUseProgram(0);
            CC::Log(CC::LOG_INFO, "GL33: Phase F.0.9 TAA Bicubic Upscale shader compiled (program=%u)", programBicubicUpscale);
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase F.0.9 TAA Bicubic Upscale shader compile failed; DrawTAAUpscalePass 将 fallback 走 Blit");
        }

        // ---- Phase F.0.14 — TAA Lanczos-2 25-tap 5x5 上采样 ----
        //   仅 sharpness=0 && halfRes=true && upscaleMode==2 (lanczos) 路径使用
        //   编译失败时 DrawTAALanczosPass fallback 走 BlitTAAToHDR (bilinear stretch)
        programLanczosUpscale = buildProgram(FS_LANCZOS_UPSCALE_SOURCE, "TAA_LanczosUpscale");
        if (programLanczosUpscale) {
            locLanczos_InputTex = glGetUniformLocation(programLanczosUpscale, "uInputTex");
            locLanczos_Texel    = glGetUniformLocation(programLanczosUpscale, "uTexel");
            // 默认 sampler 绑 slot 0
            glUseProgram(programLanczosUpscale);
            if (locLanczos_InputTex >= 0) glUniform1i(locLanczos_InputTex, 0);
            glUseProgram(0);
            CC::Log(CC::LOG_INFO, "GL33: Phase F.0.14 TAA Lanczos-2 Upscale shader compiled (program=%u)", programLanczosUpscale);
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase F.0.14 TAA Lanczos-2 Upscale shader compile failed; DrawTAALanczosPass 将 fallback 走 Blit");
        }

        // ---- Phase F.0.12 — TAA RCAS Sharpening (FSR2 5-tap noise+edge aware) ----
        //   与 F.0.1 unsharp / F.0.6 cas 共存; 用户通过 SetSharpenMode("rcas") 切换
        //   shader: FS_RCAS_SOURCE (5-tap RCAS, robust contrast adaptive)
        programRCAS = buildProgram(FS_RCAS_SOURCE, "TAA_RCAS");
        if (programRCAS) {
            locRCAS_InputTex  = glGetUniformLocation(programRCAS, "uInputTex");
            locRCAS_TexelSize = glGetUniformLocation(programRCAS, "uTexelSize");
            locRCAS_Sharpness = glGetUniformLocation(programRCAS, "uSharpness");
            glUseProgram(programRCAS);
            if (locRCAS_InputTex >= 0) glUniform1i(locRCAS_InputTex, 0);
            glUseProgram(0);
            CC::Log(CC::LOG_INFO, "GL33: Phase F.0.12 TAA RCAS shader compiled (program=%u)", programRCAS);
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase F.0.12 TAA RCAS shader compile failed; DrawTAARCASPass 将 fallback 走 Blit");
        }

        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.6+E.7+E.8+E.9+E.10+E.11+E.12 LensFx/SSAO/SSR ready (lensDirt=%s, streak=%s, lensFlare=%s, ssao=%s, ssr=%s, ssrBlur=%s, ssrTemporal=%s; programs=[LD=%u, SB=%u, SC=%u, LFG=%u, S=%u, SB=%u, SC=%u, SSR=%u, SSRC=%u, SSRB=%u, SSRT=%u])",
                lensDirtSupported ? "yes" : "no",
                streakSupported   ? "yes" : "no",
                lensFlareSupported? "yes" : "no",
                ssaoSupported     ? "yes" : "no",
                ssrSupported      ? "yes" : "no",
                ssrBlurSupported  ? "yes" : "no",
                ssrTemporalSupported ? "yes" : "no",
                programLensDirt, programStreakBlur, programStreakComposite,
                programLensFlareGhost,
                programSSAO, programSSAOBlur, programSSAOComposite,
                programSSR, programSSRComposite, programSSRBlur, programSSRTemporal);
    }

    void Shutdown() override {
        if (program) glDeleteProgram(program);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        if (vao) glDeleteVertexArrays(1, &vao);
        program = vao = vbo = ebo = 0;
        eboCapacity = 0;

        // Phase F.0.11.2 — 释放异步 readback PBO (若有)
        ReadbackAsyncShutdown();

        // Phase AS.4 — 释放 3D 资源
        if (programUnlit) { glDeleteProgram(programUnlit); programUnlit = 0; }
        if (programPBR)   { glDeleteProgram(programPBR);   programPBR   = 0; }
        for (auto& kv : meshes) {
            const MeshGPU& m = kv.second;
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
        }
        meshes.clear();

        // Phase AW — 释放 GPU Skinning 资源
        if (programUnlitSkin) { glDeleteProgram(programUnlitSkin); programUnlitSkin = 0; }
        if (programPBRSkin)   { glDeleteProgram(programPBRSkin);   programPBRSkin   = 0; }
        if (uboJointMatrices) { glDeleteBuffers(1, &uboJointMatrices); uboJointMatrices = 0; }
        for (auto& kv : skinnedMeshes) {
            const MeshGPU& m = kv.second;
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
        }
        skinnedMeshes.clear();
        gpuSkinningSupported = false;

        // Phase AX — 释放 Morph Target 资源
        if (programUnlitSkinMorph) { glDeleteProgram(programUnlitSkinMorph); programUnlitSkinMorph = 0; }
        if (programPBRSkinMorph)   { glDeleteProgram(programPBRSkinMorph);   programPBRSkinMorph   = 0; }
        for (auto& kv : skinnedMorphMeshes) {
            const MeshGPU& m = kv.second;
            if (m.ebo)        glDeleteBuffers(1, &m.ebo);
            if (m.vbo)        glDeleteBuffers(1, &m.vbo);
            if (m.vao)        glDeleteVertexArrays(1, &m.vao);
            if (m.morphPosTex) glDeleteTextures(1, &m.morphPosTex);
            if (m.morphNrmTex) glDeleteTextures(1, &m.morphNrmTex);
        }
        skinnedMorphMeshes.clear();
        morphTargetsSupported = false;

        // Phase E.1.1 — 释放 Lit2D 资源 (VAO/VBO/EBO + program)
        if (programLit2D)   { glDeleteProgram(programLit2D); programLit2D = 0; }
        if (eboLit2D)       { glDeleteBuffers(1, &eboLit2D); eboLit2D = 0; }
        if (vboLit2D)       { glDeleteBuffers(1, &vboLit2D); vboLit2D = 0; }
        if (vaoLit2D)       { glDeleteVertexArrays(1, &vaoLit2D); vaoLit2D = 0; }
        // Phase E.2.3 — 释放动态 EBO
        if (eboLit2DBatch)  { glDeleteBuffers(1, &eboLit2DBatch); eboLit2DBatch = 0; }
        eboLit2DBatchCapacity = 0;
        vboLit2DCapacity = 0;

        // Phase E.1.5 — 释放 Lit2D mesh 池
        for (auto& kv : litMeshes) {
            const MeshGPU& m = kv.second;
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
        }
        litMeshes.clear();
        lit2DSupported = false;

        // Phase E.3.1 — 释放 HDR + Tonemap 资源 (VAO/VBO + program + FBO depth RBO 残留)
        if (programTonemap) { glDeleteProgram(programTonemap); programTonemap = 0; }
        if (vboTonemap)     { glDeleteBuffers(1, &vboTonemap); vboTonemap = 0; }
        if (vaoTonemap)     { glDeleteVertexArrays(1, &vaoTonemap); vaoTonemap = 0; }
        locTonemap_HDRTex = locTonemap_Exposure = locTonemap_Gamma = locTonemap_Mode = -1;
        // Phase F.0.10.8: 复位 LUT uniform locs
        locTonemap_LUT = locTonemap_LUTStrength = locTonemap_LUTEnabled = -1;
        // 清理 CreateHDRFBO 遗留的 depth RBO (HDRRenderer::Shutdown 未配对 DeleteHDRFBO 时的兜底)
        for (auto& kv : hdrFboDepthRB) {
            if (kv.second) { GLuint rb = kv.second; glDeleteRenderbuffers(1, &rb); }
        }
        hdrFboDepthRB.clear();
        // Phase E.8.x — 清理 normal tex 兜底
        for (auto& kv : hdrFboNormalTex) {
            if (kv.second) { GLuint t = kv.second; glDeleteTextures(1, &t); }
        }
        hdrFboNormalTex.clear();
        // Phase E.13 — 清理 velocity tex 兜底 (HDRRenderer::Shutdown 未配对 DeleteHDRFBO 时)
        for (auto& kv : hdrFboVelocityTex) {
            if (kv.second) { GLuint t = kv.second; glDeleteTextures(1, &t); }
        }
        hdrFboVelocityTex.clear();
        // Phase E.14 — 同步清理 velocity format 跟踪
        hdrFboVelocityFormat.clear();
        tonemapSupported = false;

        // Phase E.4 — 释放 Bloom shader (pyramid 资源由 BloomRenderer::Shutdown 配对释放)
        if (programBloomBright) { glDeleteProgram(programBloomBright); programBloomBright = 0; }
        if (programBloomDown)   { glDeleteProgram(programBloomDown);   programBloomDown   = 0; }
        if (programBloomUp)     { glDeleteProgram(programBloomUp);     programBloomUp     = 0; }
        locBloomBright_Src = locBloomBright_Threshold = -1;
        locBloomDown_Src = locBloomDown_Texel = locBloomDown_UvBounds = -1;
        locBloomUp_Src = locBloomUp_Texel = locBloomUp_Radius = locBloomUp_Intensity = locBloomUp_UvBounds = -1;
        bloomSupported = false;

        // Phase E.5 — 释放 Auto Exposure shader (luma RT 由 AutoExposureRenderer::Shutdown 配对释放)
        if (programLumaExtract) { glDeleteProgram(programLumaExtract); programLumaExtract = 0; }
        locLumaExtract_HDRTex = -1;
        autoExposureSupported = false;

        // Phase E.6 — 释放 LensFx shader + 1x1 白纹理 (streak RT 由 StreakRenderer::Shutdown 配对释放)
        if (programLensDirt)         { glDeleteProgram(programLensDirt);         programLensDirt = 0; }
        if (programStreakBlur)       { glDeleteProgram(programStreakBlur);       programStreakBlur = 0; }
        if (programStreakComposite)  { glDeleteProgram(programStreakComposite);  programStreakComposite = 0; }
        if (whiteTex1x1)             { glDeleteTextures(1, &whiteTex1x1);        whiteTex1x1 = 0; }
        locLensDirt_BloomTex = locLensDirt_DirtTex = locLensDirt_Intensity = -1;
        locStreakBlur_Src = locStreakBlur_Texel = locStreakBlur_Length = locStreakBlur_Direction = -1;
        locStreakComposite_Src = locStreakComposite_Intensity = -1;
        lensDirtSupported = false;
        streakSupported = false;

        // Phase E.7 — 释放 Lens Flare ghost shader (RT 由 LensFlareRenderer::Shutdown 配对释放)
        if (programLensFlareGhost) { glDeleteProgram(programLensFlareGhost); programLensFlareGhost = 0; }
        locLensFlare_BrightTex = locLensFlare_FlareTex = locLensFlare_GhostCount = locLensFlare_GhostDispersal = -1;
        locLensFlare_HaloWidth = locLensFlare_Aberration = locLensFlare_DistortionEn = -1;
        lensFlareSupported = false;

        // Phase E.8 — 清理 SSAO (3 program + composite temp RT; depth/AO RT 由 SSAORenderer 管)
        if (programSSAO)          { glDeleteProgram(programSSAO);          programSSAO = 0; }
        if (programSSAOBlur)      { glDeleteProgram(programSSAOBlur);      programSSAOBlur = 0; }
        if (programSSAOComposite) { glDeleteProgram(programSSAOComposite); programSSAOComposite = 0; }
        if (ssaoCompTempFbo)      { glDeleteFramebuffers(1, &ssaoCompTempFbo); ssaoCompTempFbo = 0; }
        if (ssaoCompTempTex)      { glDeleteTextures(1, &ssaoCompTempTex);     ssaoCompTempTex = 0; }
        ssaoCompTempW = ssaoCompTempH = 0;
        locSSAO_DepthTex = locSSAO_NoiseTex = locSSAO_NormalTex = locSSAO_Proj = locSSAO_InvProj = -1;
        locSSAO_Kernel = locSSAO_KernelSize = locSSAO_Radius = locSSAO_Bias = -1;
        locSSAO_Power = locSSAO_NoiseScale = -1;
        locSSAOBlur_SSAOTex = locSSAOBlur_DepthTex = locSSAOBlur_Texel = locSSAOBlur_Axis = -1;
        locSSAOComp_SceneTex = locSSAOComp_AOTex = locSSAOComp_Intensity = -1;
        ssaoSupported = false;

        // Phase E.9 — 清理 SSR (2 program + composite temp RT; depth/reflect RT 由 SSRRenderer 管)
        if (programSSR)            { glDeleteProgram(programSSR);            programSSR = 0; }
        if (programSSRComposite)   { glDeleteProgram(programSSRComposite);   programSSRComposite = 0; }
        if (ssrCompTempFbo)        { glDeleteFramebuffers(1, &ssrCompTempFbo); ssrCompTempFbo = 0; }
        if (ssrCompTempTex)        { glDeleteTextures(1, &ssrCompTempTex);     ssrCompTempTex = 0; }
        ssrCompTempW = ssrCompTempH = 0;
        ssrSupported = false;

        // Phase E.10 — 清理 SSR Blur (1 program; ping-pong RT 由 SSRRenderer 管 DeleteSSRBlurRT)
        if (programSSRBlur) { glDeleteProgram(programSSRBlur); programSSRBlur = 0; }
        ssrBlurSupported = false;
        locSSRBlur_SrcTex     = locSSRBlur_DepthTex = -1;
        locSSRBlur_Texel      = locSSRBlur_Axis = locSSRBlur_Radius = -1;
        locSSRBlur_Bilateral  = locSSRBlur_DepthSigma = -1;

        // Phase E.12 — 清理 SSR Temporal (1 program; history RT 由 SSRRenderer 管 DeleteSSRHistoryRT)
        if (programSSRTemporal) { glDeleteProgram(programSSRTemporal); programSSRTemporal = 0; }
        ssrTemporalSupported = false;
        locSSR_JitterOffset          = -1;
        locSSRTemporal_CurReflectTex = locSSRTemporal_HistoryTex    = -1;
        locSSRTemporal_DepthTex      = locSSRTemporal_VelocityTex   = -1;
        locSSRTemporal_ReprojectMat  = -1;
        locSSRTemporal_InvProj       = locSSRTemporal_Texel         = -1;
        locSSRTemporal_BlendAlpha    = locSSRTemporal_RejectionMode = -1;
        locSSRTemporal_HasHistory    = locSSRTemporal_HasVelocityTex = -1;
        // Phase E.14 — dilation/format/scale location 重置
        locSSRTemporal_VelocityDilation = -1;
        locSSRTemporal_VelocityFormat   = -1;
        locSSRTemporal_VelocityScale    = -1;

        // Phase E.15 — Motion Blur 清理 (1 program; ping-pong RT 由 MotionBlurRenderer 管)
        // Phase E.16 — 多 2 个 loc: locMB_CameraVelocityTex / locMB_Mode
        if (programMotionBlur) { glDeleteProgram(programMotionBlur); programMotionBlur = 0; }
        motionBlurSupported = false;
        locMB_SceneTex = locMB_VelocityTex = locMB_CameraVelocityTex = locMB_Texel = -1;
        locMB_Strength = locMB_SampleCount = -1;
        locMB_VelocityDilation = locMB_VelocityFormat = locMB_VelocityScale = locMB_Mode = -1;

        // Phase E.18 — Velocity Dilation Pass 清理 (1 program; ping-pong RT 由 HDRRenderer 管)
        if (programVelocityDilate) { glDeleteProgram(programVelocityDilate); programVelocityDilate = 0; }
        velocityDilateSupported = false;
        dilationPassActive_     = false;
        locVDilate_SrcVelocityTex = locVDilate_Texel = -1;
        locVDilate_VelocityFormat = locVDilate_VelocityScale = -1;

        // Phase F.0 — TAA Master Pipeline 清理 (1 program; history ping-pong RT 由 TAARenderer 管)
        if (programTAA) { glDeleteProgram(programTAA); programTAA = 0; }
        taaSupported           = false;
        jitterActive           = false;   // 同时复位 jitter state
        locTAA_CurHdrTex       = locTAA_HistoryTex       = locTAA_VelocityTex      = -1;
        locTAA_Texel           = locTAA_BlendAlpha       = locTAA_NeighborhoodClip = -1;
        locTAA_HasHistory      = locTAA_VelocityDilation = -1;
        locTAA_VelocityFormat  = locTAA_VelocityScale    = -1;
        locTAA_AntiFlicker     = -1;   // Phase F.0.4 reset
        locTAA_ClipMode        = -1;   // Phase F.0.2 reset
        locTAA_VarianceGamma   = -1;   // Phase F.0.3 reset
        locTAA_MotionGamma         = -1;   // Phase F.0.8 reset
        locTAA_MotionAdaptiveGamma = -1;   // Phase F.0.8 reset
        locTAA_UvBounds            = -1;   // Phase F.0.10.5 reset

        // Phase F.0.1 — TAA Sharpening 清理
        if (programSharpen) { glDeleteProgram(programSharpen); programSharpen = 0; }
        locSharpen_InputTex = locSharpen_TexelSize = locSharpen_Sharpness = locSharpen_UvBounds = -1;

        // Phase F.0.6 — TAA CAS Sharpening 清理
        if (programCAS) { glDeleteProgram(programCAS); programCAS = 0; }
        locCAS_InputTex = locCAS_TexelSize = locCAS_Sharpness = -1;

        // Phase F.0.9 — TAA Custom Upsampler 清理
        if (programBicubicUpscale) { glDeleteProgram(programBicubicUpscale); programBicubicUpscale = 0; }
        locBicubic_InputTex = locBicubic_Texel = -1;

        // Phase F.0.14 — TAA Lanczos-2 Upsampler 清理
        if (programLanczosUpscale) { glDeleteProgram(programLanczosUpscale); programLanczosUpscale = 0; }
        locLanczos_InputTex = locLanczos_Texel = -1;

        // Phase F.0.12 — TAA RCAS Sharpening 清理
        if (programRCAS) { glDeleteProgram(programRCAS); programRCAS = 0; }
        locRCAS_InputTex = locRCAS_TexelSize = locRCAS_Sharpness = -1;
    }

    bool SupportsLit2D() const override { return lit2DSupported; }

    // ==================== Phase E.3.1 — HDR + Tonemap 虚接口实现 ====================

    bool SupportsHDR() const override { return tonemapSupported; }

    /// 创建 HDR FBO: RGBA16F 颜色附件 + (可选) RG16F view-normal/velocity + Depth24 RBO
    /// Phase E.14: velocityFormat 控制可选 velocity 附件使用 RG16F (默认) 或 RG8 (低精度节 VRAM)
    /// Phase E.16: outCameraVelocityTex != null 时额外创建 slot 3 camera-only velocity tex
    uint32_t CreateHDRFBO(int w, int h, uint32_t* outTex,
                          uint32_t* outNormalTex,
                          uint32_t* outVelocityTex,
                          VelocityFormat velocityFormat,
                          uint32_t* outCameraVelocityTex) override {
        if (!tonemapSupported || w <= 0 || h <= 0 || !outTex) return 0;

        // 1. 创建 RGBA16F 颜色纹理 (GL_LINEAR + GL_CLAMP_TO_EDGE)
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return 0;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 1.5. Phase E.8.x — (可选) 创建 RG16F view-space normal 纹理
        GLuint normalTex = 0;
        if (outNormalTex) {
            glGenTextures(1, &normalTex);
            if (!normalTex) { glDeleteTextures(1, &tex); return 0; }
            glBindTexture(GL_TEXTURE_2D, normalTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        GLuint velocityTex = 0;
        if (outVelocityTex) {
            glGenTextures(1, &velocityTex);
            if (!velocityTex) {
                glDeleteTextures(1, &tex);
                if (normalTex) glDeleteTextures(1, &normalTex);
                return 0;
            }
            glBindTexture(GL_TEXTURE_2D, velocityTex);
            // Phase E.14: 按 velocityFormat 选 internalFormat
            //   RG16F: 高精度浮点，直接存 currentUV - prevUV (与 Phase E.13 一致)
            //   RG8:   8-bit UNORM，shader 侧用 uVelocityScale 编码为 [0, 1]，节省 VRAM 4x
            if (velocityFormat == VelocityFormat::RG8) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // Phase E.16 — camera-only velocity tex (slot 3): 与 velocityTex 同格式、同 sampler 参数
        // 仅在 outCameraVelocityTex != null 且 outVelocityTex != null 时创建
        // (设计: 第二张 velocity 依赖第一张；MotionBlur shader mode=2 需要同时采两张)
        GLuint cameraVelocityTex = 0;
        if (outCameraVelocityTex && outVelocityTex && velocityTex) {
            glGenTextures(1, &cameraVelocityTex);
            if (!cameraVelocityTex) {
                glDeleteTextures(1, &tex);
                if (normalTex) glDeleteTextures(1, &normalTex);
                glDeleteTextures(1, &velocityTex);
                return 0;
            }
            glBindTexture(GL_TEXTURE_2D, cameraVelocityTex);
            if (velocityFormat == VelocityFormat::RG8) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_FLOAT, nullptr);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // 2. 创建 Depth24 RBO
        GLuint depthRB = 0;
        glGenRenderbuffers(1, &depthRB);
        if (!depthRB) {
            glDeleteTextures(1, &tex);
            if (normalTex) glDeleteTextures(1, &normalTex);
            if (velocityTex) glDeleteTextures(1, &velocityTex);
            if (cameraVelocityTex) glDeleteTextures(1, &cameraVelocityTex);
            return 0;
        }
        glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        // 3. 创建 FBO 并附加 color + (normal) + (velocity) + (cameraVelocity) + depth
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) {
            glDeleteTextures(1, &tex);
            if (normalTex) glDeleteTextures(1, &normalTex);
            if (velocityTex) glDeleteTextures(1, &velocityTex);
            if (cameraVelocityTex) glDeleteTextures(1, &cameraVelocityTex);
            glDeleteRenderbuffers(1, &depthRB);
            return 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (normalTex) {
            // Phase E.8.x — 附加 normal RT 为 COLOR_ATTACHMENT1
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normalTex, 0);
        }
        if (velocityTex) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, velocityTex, 0);
        }
        if (cameraVelocityTex) {
            // Phase E.16 — camera-only velocity 为 COLOR_ATTACHMENT3
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, cameraVelocityTex, 0);
        }
        // drawBuffers 选择矩阵：Phase E.16 增 3 个带 cameraVelocity 的分支
        if (cameraVelocityTex) {
            // 多 1 张 = 必与 velocity 同时存在; normal 可选
            if (normalTex) {
                const GLenum drawBufs[4] = {
                    GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                    GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
                };
                glDrawBuffers(4, drawBufs);
            } else {
                const GLenum drawBufs[4] = {
                    GL_COLOR_ATTACHMENT0, GL_NONE,
                    GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
                };
                glDrawBuffers(4, drawBufs);
            }
        } else if (normalTex && velocityTex) {
            const GLenum drawBufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
            glDrawBuffers(3, drawBufs);
        } else if (normalTex) {
            const GLenum drawBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, drawBufs);
        } else if (velocityTex) {
            const GLenum drawBufs[3] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2 };
            glDrawBuffers(3, drawBufs);
        } else {
            const GLenum drawBufs[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, drawBufs);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_ERROR, "GL33: HDR%s FBO incomplete (status=0x%X, %dx%d)",
                    normalTex ? " MRT" : "", status, w, h);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &depthRB);
            glDeleteTextures(1, &tex);
            if (normalTex) glDeleteTextures(1, &normalTex);
            if (velocityTex) glDeleteTextures(1, &velocityTex);
            if (cameraVelocityTex) glDeleteTextures(1, &cameraVelocityTex);
            return 0;
        }

        hdrFboDepthRB[fbo] = depthRB;
        if (normalTex) {
            hdrFboNormalTex[fbo] = normalTex;
            *outNormalTex = normalTex;
        }
        if (velocityTex) {
            hdrFboVelocityTex[fbo] = velocityTex;
            hdrFboVelocityFormat[fbo] = velocityFormat;     // Phase E.14: 跟踪格式
            *outVelocityTex = velocityTex;
        }
        if (cameraVelocityTex) {
            // Phase E.16: 跟踪 camera-only velocity tex
            hdrFboCameraVelocityTex[fbo] = cameraVelocityTex;
            *outCameraVelocityTex = cameraVelocityTex;
        }
        // Phase E.14: 同步活跃 format，供 3D shader UploadVelocityUniforms 上传使用
        activeVelocityFormat_ = velocityFormat;
        *outTex = tex;
        return fbo;
    }

    /// Phase E.8.x — 查找 HDR FBO 关联的 view-space normal RT id
    uint32_t GetHDRNormalTex(uint32_t fbo) const override {
        auto it = hdrFboNormalTex.find(fbo);
        return (it != hdrFboNormalTex.end()) ? it->second : 0;
    }

    uint32_t GetHDRVelocityTex(uint32_t fbo) const override {
        auto it = hdrFboVelocityTex.find(fbo);
        return (it != hdrFboVelocityTex.end()) ? it->second : 0;
    }

    /// Phase E.16 — 查找 HDR FBO 关联的 camera-only velocity tex (slot 3)
    /// 返 0 表示未创建 / 后端不支持 / fbo 已释放
    uint32_t GetHDRCameraVelocityTex(uint32_t fbo) const override {
        auto it = hdrFboCameraVelocityTex.find(fbo);
        return (it != hdrFboCameraVelocityTex.end()) ? it->second : 0;
    }

    /// 释放 HDR FBO 资源 (与 CreateHDRFBO 配对; 自动从 map 查 depthRB + normalTex)
    void DeleteHDRFBO(uint32_t fbo, uint32_t tex) override {
        if (fbo) {
            auto itD = hdrFboDepthRB.find(fbo);
            if (itD != hdrFboDepthRB.end()) {
                GLuint rb = itD->second;
                if (rb) glDeleteRenderbuffers(1, &rb);
                hdrFboDepthRB.erase(itD);
            }
            // Phase E.8.x — 释放关联 normal tex (如果存在)
            auto itN = hdrFboNormalTex.find(fbo);
            if (itN != hdrFboNormalTex.end()) {
                GLuint t = itN->second;
                if (t) glDeleteTextures(1, &t);
                hdrFboNormalTex.erase(itN);
            }
            auto itV = hdrFboVelocityTex.find(fbo);
            if (itV != hdrFboVelocityTex.end()) {
                GLuint t = itV->second;
                if (t) glDeleteTextures(1, &t);
                hdrFboVelocityTex.erase(itV);
            }
            // Phase E.16 — 释放关联 camera-only velocity tex
            auto itCV = hdrFboCameraVelocityTex.find(fbo);
            if (itCV != hdrFboCameraVelocityTex.end()) {
                GLuint t = itCV->second;
                if (t) glDeleteTextures(1, &t);
                hdrFboCameraVelocityTex.erase(itCV);
            }
            // Phase E.14: 同步清理 velocity format 记录
            hdrFboVelocityFormat.erase(fbo);
            GLuint f = fbo;
            glDeleteFramebuffers(1, &f);
        }
        if (tex) {
            GLuint t = tex;
            glDeleteTextures(1, &t);
        }
    }

    // Phase F.0.10.8 helper: 上传 LUT uniforms + 绑/解 unit 1 sampler3D
    // useLUT = lutTex != 0 && lutStrength > 0
    void uploadTonemapLUTUniforms_(uint32_t lutTex, float lutStrength) {
        const bool useLUT = (lutTex != 0u && lutStrength > 0.0f);
        if (locTonemap_LUTEnabled  >= 0) glUniform1i(locTonemap_LUTEnabled,  useLUT ? 1 : 0);
        if (locTonemap_LUTStrength >= 0) glUniform1f(locTonemap_LUTStrength, useLUT ? lutStrength : 0.0f);
        if (useLUT) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_3D, (GLuint)lutTex);
            glActiveTexture(GL_TEXTURE0);  // 复位回 unit 0 (与 HDR tex 一致)
        }
    }
    void unbindTonemapLUT_(uint32_t lutTex, float lutStrength) {
        const bool useLUT = (lutTex != 0u && lutStrength > 0.0f);
        if (!useLUT) return;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture(GL_TEXTURE0);
    }

    /// ACES tonemap 全屏 pass: HDR 纹理 → 当前绑定 framebuffer
    /// Phase E.3.4: 支持 4 个 operator (ACES / Reinhard / Uncharted2 / Linear)
    /// Phase F.0.10.8: 加 lutTex/lutStrength (默认 0/0 = 不应用 LUT)
    /// 调用方负责: 先 UnbindFBO() 切到 default fb, 调用后不需要恢复 depth/blend state.
    void DrawTonemapFullscreen(uint32_t hdrTex, float exposure, float gamma,
                                int tonemapMode = 0,
                                uint32_t lutTex = 0,
                                float lutStrength = 0.0f) override {
        if (!tonemapSupported || !hdrTex) return;

        // 1. 关 depth / blend (tonemap 是 destructive full-screen write)
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        // 2. 绑 program + uniform
        glUseProgram(programTonemap);
        if (locTonemap_Exposure >= 0) glUniform1f(locTonemap_Exposure, exposure);
        if (locTonemap_Gamma    >= 0) glUniform1f(locTonemap_Gamma,    gamma);
        if (locTonemap_Mode     >= 0) glUniform1i(locTonemap_Mode,     tonemapMode);
        // Phase F.0.10.8: LUT uniform + 可选 unit 1 绑定
        uploadTonemapLUTUniforms_(lutTex, lutStrength);

        // 3. 绑 HDR 纹理到 texture unit 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);

        // 4. 绑 VAO + draw 6 顶点
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // 5. 解绑 (不恢复 depth / blend: 下次 BeginFrame 会重置)
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        unbindTonemapLUT_(lutTex, lutStrength);   // Phase F.0.10.8
        glUseProgram(0);
    }

    /// Phase F.0.10.6 — Region 限定 tonemap (split-screen multi-instance 必备)
    /// rgnW<=0 || rgnH<=0 退化为 fullscreen (零回归 fallback)
    /// 与 fullscreen 共享 program / VAO / uniform location, 只多 scissor 1 步
    /// Phase F.0.10.8: 加 lutTex/lutStrength (默认 0/0 = 不应用 LUT)
    void DrawTonemapRegion(uint32_t hdrTex, float exposure, float gamma,
                            int tonemapMode, int rgnX, int rgnY,
                            int rgnW, int rgnH,
                            uint32_t lutTex = 0,
                            float lutStrength = 0.0f) override {
        if (!tonemapSupported || !hdrTex) return;
        // 退化路径: rgn 无效时走老 fullscreen (保护性, 简化 caller)
        if (rgnW <= 0 || rgnH <= 0) {
            DrawTonemapFullscreen(hdrTex, exposure, gamma, tonemapMode, lutTex, lutStrength);
            return;
        }

        // 1. 关 depth/blend, 启 scissor (与 F.0.10.3 region 模式一致)
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glEnable(GL_SCISSOR_TEST);
        glScissor(rgnX, rgnY, rgnW, rgnH);

        // 2. 绑 program + uniform (与 fullscreen 一致)
        glUseProgram(programTonemap);
        if (locTonemap_Exposure >= 0) glUniform1f(locTonemap_Exposure, exposure);
        if (locTonemap_Gamma    >= 0) glUniform1f(locTonemap_Gamma,    gamma);
        if (locTonemap_Mode     >= 0) glUniform1i(locTonemap_Mode,     tonemapMode);
        // Phase F.0.10.8: LUT uniform + 可选 unit 1 绑定
        uploadTonemapLUTUniforms_(lutTex, lutStrength);

        // 3. 绑 HDR tex
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);

        // 4. 全屏 quad (scissor 限制实际写区)
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // 5. 解绑 + 复位 scissor (重要: 防影响后续 pass)
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        unbindTonemapLUT_(lutTex, lutStrength);   // Phase F.0.10.8
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.6 — 复位
    }

    /// Phase F.0.10.8 — 创建 3D LUT 纹理 (RGB8 + LINEAR + CLAMP_TO_EDGE)
    /// 不依赖 tonemapSupported (LUT 是用户资源, 与 tonemap shader 解耦)
    uint32_t CreateLUT3D(int size, const uint8_t* data) override {
        if (size < 1 || !data) return 0u;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return 0u;
        glBindTexture(GL_TEXTURE_3D, tex);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8,
                     size, size, size,
                     0, GL_RGB, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        const GLenum err = glGetError();
        glBindTexture(GL_TEXTURE_3D, 0);
        if (err != GL_NO_ERROR) {
            glDeleteTextures(1, &tex);
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase F.0.10.8 CreateLUT3D failed (size=%d, gl err=0x%x)", size, err);
            return 0u;
        }
        return (uint32_t)tex;
    }

    /// Phase F.0.10.8 — 删除 3D LUT 纹理 (与 CreateLUT3D / CreateLUT3DFloat 配对)
    bool DeleteLUT3D(uint32_t lutTex) override {
        if (!lutTex) return false;
        GLuint t = (GLuint)lutTex;
        glDeleteTextures(1, &t);
        return true;
    }

    /// Phase F.0.10.8.6 — 探测是否支持 HDR LUT
    /// GL3.3 desktop / GLES3 都支持 GL_RGB16F + GL_TEXTURE_3D + GL_FLOAT 上传, 返 true
    bool SupportsLUT3DFloat() const override { return true; }

    /// Phase F.0.10.8.5 — 创建 HDR float 3D LUT (RGB16F + LINEAR + CLAMP_TO_EDGE)
    /// 用于 .cube DOMAIN_MAX > 1.0 / 16-bit HALD PNG / ACES workflow
    uint32_t CreateLUT3DFloat(int size, const float* data) override {
        if (size < 1 || !data) return 0u;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return 0u;
        glBindTexture(GL_TEXTURE_3D, tex);
        // 关键差异: 内部格式 GL_RGB16F (vs RGB8) + 上传类型 GL_FLOAT (vs UNSIGNED_BYTE)
        // 移动端要求 EXT_color_buffer_half_float (大多数 GLES3 设备已支持)
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F,
                     size, size, size,
                     0, GL_RGB, GL_FLOAT, data);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        const GLenum err = glGetError();
        glBindTexture(GL_TEXTURE_3D, 0);
        if (err != GL_NO_ERROR) {
            glDeleteTextures(1, &tex);
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase F.0.10.8.5 CreateLUT3DFloat failed (size=%d, gl err=0x%x)",
                    size, err);
            return 0u;
        }
        return (uint32_t)tex;
    }

    // ==================== Phase E.4 — Bloom 虚接口实现 ====================

    bool SupportsBloom() const override { return bloomSupported; }

    /// 创建 Bloom pyramid: 每级 RGBA16F 纹理 + FBO, 无 depth (bloom 不需要深度)
    int CreateBloomPyramid(int w, int h, int levels,
                            uint32_t* outFbos, uint32_t* outTexs) override {
        if (!bloomSupported || w <= 0 || h <= 0 || levels <= 0 || !outFbos || !outTexs) {
            return 0;
        }

        int created = 0;
        int curW = w, curH = h;
        for (int i = 0; i < levels; ++i) {
            // 1. 创建 RGBA16F 颜色纹理 (GL_LINEAR + GL_CLAMP_TO_EDGE, 与 HDR RT 一致)
            GLuint tex = 0;
            glGenTextures(1, &tex);
            if (!tex) break;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, curW, curH, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            // 2. 创建 FBO 并附加 color (无 depth)
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            if (!fbo) { glDeleteTextures(1, &tex); break; }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE) {
                CC::Log(CC::LOG_ERROR,
                        "GL33: Bloom pyramid[%d] FBO incomplete (status=0x%X, %dx%d)",
                        i, status, curW, curH);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &tex);
                break;
            }

            outFbos[i] = fbo;
            outTexs[i] = tex;
            ++created;

            // 下一级尺寸减半 (最小 1x1)
            curW = (curW > 1) ? curW / 2 : 1;
            curH = (curH > 1) ? curH / 2 : 1;
            // 若已到 1x1, 继续 level 无意义; 但 created 仍按请求返回
            if (curW == 1 && curH == 1 && i < levels - 1) {
                // 允许继续创建剩余 1x1 级 (不做硬截断, 保证 levels 严格)
            }
        }

        return created;
    }

    void DeleteBloomPyramid(uint32_t* fbos, uint32_t* texs, int levels) override {
        if (!fbos || !texs || levels <= 0) return;
        for (int i = 0; i < levels; ++i) {
            if (fbos[i]) { GLuint f = fbos[i]; glDeleteFramebuffers(1, &f); fbos[i] = 0; }
            if (texs[i]) { GLuint t = texs[i]; glDeleteTextures(1, &t);     texs[i] = 0; }
        }
    }

    /// Bright Pass: HDR sceneTex → outFbo (pyramid[0]), 亮度阈值提取 + soft knee
    void DrawBloomBrightPass(uint32_t sceneTex, uint32_t outFbo,
                              int w, int h, float threshold,
                              int rgnX, int rgnY, int rgnW, int rgnH) override {
        if (!bloomSupported || !sceneTex || !outFbo) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)outFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.3 — scissor 限定 region 写区域 (split-screen 必备)
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programBloomBright);
        if (locBloomBright_Threshold >= 0) glUniform1f(locBloomBright_Threshold, threshold);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)sceneTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Downsample: srcTex → dstFbo (13-tap COD AW)
    void DrawBloomDownsample(uint32_t srcTex, uint32_t dstFbo,
                              int dstW, int dstH,
                              int rgnX, int rgnY, int rgnW, int rgnH,
                              const float* uvBounds) override {   // Phase F.0.10.5
        if (!bloomSupported || !srcTex || !dstFbo || dstW <= 0 || dstH <= 0) return;

        // uTexel = 1.0 / srcSize. 注: downsample 的 src 是上一级, 大小 = dst * 2
        float texelX = 1.0f / (float)(dstW * 2);
        float texelY = 1.0f / (float)(dstH * 2);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.3 — scissor 限定 region 写区域 (mip 缩半逻辑由 caller 处理)
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programBloomDown);
        if (locBloomDown_Texel >= 0) glUniform2f(locBloomDown_Texel, texelX, texelY);
        // Phase F.0.10.5: uvBounds 上传 (nullptr 上传 0,0,1,1 = 无 clamp, 零回归)
        if (locBloomDown_UvBounds >= 0) {
            const float defaultBounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glUniform4fv(locBloomDown_UvBounds, 1, uvBounds ? uvBounds : defaultBounds);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Upsample + additive blend: srcTex → dstFbo (tent 3x3, hardware blend)
    void DrawBloomUpsample(uint32_t srcTex, uint32_t dstFbo,
                            int dstW, int dstH, float radius,
                            int rgnX, int rgnY, int rgnW, int rgnH,
                            const float* uvBounds) override {   // Phase F.0.10.5
        if (!bloomSupported || !srcTex || !dstFbo || dstW <= 0 || dstH <= 0) return;

        // uTexel = 1.0 / dstSize (tent 采样在 dst 空间进行, 扩散半径相对 dst 像素)
        float texelX = 1.0f / (float)dstW;
        float texelY = 1.0f / (float)dstH;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        // Phase F.0.10.3 — scissor 限定 region (additive blend 也受 scissor 影响)
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        // additive blend: dst = dst + src (hardware blend 加速)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        glUseProgram(programBloomUp);
        if (locBloomUp_Texel     >= 0) glUniform2f(locBloomUp_Texel, texelX, texelY);
        if (locBloomUp_Radius    >= 0) glUniform1f(locBloomUp_Radius, radius);
        if (locBloomUp_Intensity >= 0) glUniform1f(locBloomUp_Intensity, 1.0f);  // upsample 不缩放
        // Phase F.0.10.5: uvBounds 上传
        if (locBloomUp_UvBounds >= 0) {
            const float defaultBounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glUniform4fv(locBloomUp_UvBounds, 1, uvBounds ? uvBounds : defaultBounds);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Final composite: bloomTex additive blend → hdrFbo (intensity 缩放)
    /// 复用 upsample shader, radius=0 相当于零偏移采样 (tent 中心权重 16/16 = 全权)
    void DrawBloomComposite(uint32_t bloomTex, uint32_t hdrFbo,
                             int w, int h, float intensity,
                             int rgnX, int rgnY, int rgnW, int rgnH,
                             const float* uvBounds) override {   // Phase F.0.10.5
        if (!bloomSupported || !bloomTex || !hdrFbo || w <= 0 || h <= 0) return;

        float texelX = 1.0f / (float)w;
        float texelY = 1.0f / (float)h;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hdrFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        // Phase F.0.10.3 — scissor 限定 composite additive 写区域 (split-screen 必备)
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        glUseProgram(programBloomUp);
        if (locBloomUp_Texel     >= 0) glUniform2f(locBloomUp_Texel, texelX, texelY);
        if (locBloomUp_Radius    >= 0) glUniform1f(locBloomUp_Radius, 0.0f);       // 0 偏移 = 零扩散
        if (locBloomUp_Intensity >= 0) glUniform1f(locBloomUp_Intensity, intensity);
        // Phase F.0.10.5: composite 复用 BloomUp shader, uvBounds 同口上传 (radius=0 时 ClampUV 是 no-op)
        if (locBloomUp_UvBounds >= 0) {
            const float defaultBounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glUniform4fv(locBloomUp_UvBounds, 1, uvBounds ? uvBounds : defaultBounds);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)bloomTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.5 — Auto Exposure 虚接口实现 ====================

    bool SupportsAutoExposure() const override { return autoExposureSupported; }

    /// 创建 luminance RT: 单色 R16F mipmap-able tex + FBO, 无 depth (luma 不需要)
    /// 内部尺寸 = max(srcW/4, 8) × max(srcH/4, 8); mipmap 链由 GenerateLuminanceMipmap 生成
    bool CreateLuminanceTarget(int srcW, int srcH,
                                uint32_t* outFbo, uint32_t* outTex,
                                int* outW, int* outH) override {
        if (!autoExposureSupported || srcW <= 0 || srcH <= 0
            || !outFbo || !outTex || !outW || !outH) return false;

        // 下采到 1/4 (~480x270 for 1080p), 但下限 8x8 防极小窗口
        int w = srcW / 4; if (w < 8) w = 8;
        int h = srcH / 4; if (h < 8) h = 8;

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        // 计算 mipmap 等级数 = floor(log2(max(w, h))) + 1
        int maxDim = (w > h) ? w : h;
        int mipLevels = 1;
        while ((1 << mipLevels) <= maxDim) ++mipLevels;
        // GL 3.3+ / GLES 3.0 都支持 glTexStorage2D + R16F
        glTexStorage2D(GL_TEXTURE_2D, mipLevels, GL_R16F, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.5 CreateLuminanceTarget failed (FBO incomplete: 0x%X)",
                    (unsigned)status);
            return false;
        }

        *outFbo = (uint32_t)fbo;
        *outTex = (uint32_t)tex;
        *outW   = w;
        *outH   = h;
        return true;
    }

    void DeleteLuminanceTarget(uint32_t fbo, uint32_t tex) override {
        if (fbo) { GLuint f = (GLuint)fbo; glDeleteFramebuffers(1, &f); }
        if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t); }
    }

    /// Pass 1: hdrTex 全屏 quad → lumFbo (log luminance 写入 R16F)
    void DrawLuminanceExtract(uint32_t hdrTex, uint32_t lumFbo,
                               int w, int h) override {
        if (!autoExposureSupported || !hdrTex || !lumFbo || w <= 0 || h <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)lumFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programLumaExtract);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Pass 2: GPU 自动算 mipmap 链 (最后一层 1×1 = 全图平均 log luminance)
    void GenerateLuminanceMipmap(uint32_t lumTex) override {
        if (!autoExposureSupported || !lumTex) return;
        glBindTexture(GL_TEXTURE_2D, (GLuint)lumTex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /// Pass 3: 同步读 1×1 R16F 到 CPU (v1 stall 可接受, 仅 2 字节)
    /// 部分驱动不支持 GL_HALF_FLOAT 直接读出, fallback GL_FLOAT (4 字节但兼容性更好)
    float ReadbackLuminance1x1(uint32_t lumFbo, int lastMipLevel) override {
        if (!autoExposureSupported || !lumFbo || lastMipLevel < 0) return 0.0f;

        // 把目标 FBO 重新 attach 到最后一层 mip (保留原 attachment 不破坏)
        // 注: GL/GLES3 都支持 glFramebufferTexture2D 重 attach; 但简单做法是
        //     直接 glBindFramebuffer + glReadPixels 读 attachment 0 的 mip0
        //     若需读 mipN, 必须先 attach mipN 或用 PBO.
        // 简化方案: 不读 mip; 读取最后调用 GenerateLuminanceMipmap 后再绑 attach 0 mip = 0,
        //           然后用 glGetTexImage / 替代方案。但 GLES3 不支持 glGetTexImage,
        //           故必须 re-attach mipN 才能 readPixels(0,0,1,1).
        GLint  prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)lumFbo);

        // 取 attachment 0 的 tex id (从 glGetFramebufferAttachmentParameteriv 查询)
        GLint texId = 0;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texId);

        // 重新 attach 到最后一层 mip
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, (GLuint)texId, lastMipLevel);

        float pixel = 0.0f;
        // 用 GL_FLOAT 读 (兼容性最好); 1×1 = 4 字节
        glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &pixel);
        // 检测 GL error (某些驱动若不支持 GL_FLOAT/GL_RED 组合会报错)
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            // 静默 fallback: 重置错误流, 返回 0 (调用方将 luma 视为 1.0)
            while (glGetError() != GL_NO_ERROR) {}
            pixel = 0.0f;
        }

        // 复位 attachment 到 mip 0 (保持后续帧可重新 mipmap reduce)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, (GLuint)texId, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
        return pixel;
    }

    // ==================== Phase E.6 — Lens Dirt 虚接口实现 ====================

    bool SupportsLensDirt() const override { return lensDirtSupported; }

    /// Lens dirt composite: hdrFbo += bloomTex x dirtTex x intensity
    void DrawLensDirtComposite(uint32_t bloomTex, uint32_t dirtTex,
                                uint32_t hdrFbo,
                                int w, int h, float intensity) override {
        if (!lensDirtSupported || !bloomTex || !hdrFbo || w <= 0 || h <= 0) return;

        // dirtTex == 0 时 fallback 到 1x1 白纹理 (乘白色 = bloom x intensity 原样)
        GLuint dirt = dirtTex ? (GLuint)dirtTex : whiteTex1x1;
        if (!dirt) return;   // 白纹理未创建 (极少发生)

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hdrFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        glUseProgram(programLensDirt);
        if (locLensDirt_Intensity >= 0) glUniform1f(locLensDirt_Intensity, intensity);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)bloomTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, dirt);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.6 — Streak 虚接口实现 ====================

    bool SupportsStreak() const override { return streakSupported; }

    /// 创建 streak ping-pong RT 对 (2 个 RGBA16F + FBO)
    bool CreateStreakTargets(int srcW, int srcH,
                              uint32_t* outFbos, uint32_t* outTexs,
                              int* outW, int* outH) override {
        if (!streakSupported || srcW <= 0 || srcH <= 0
            || !outFbos || !outTexs || !outW || !outH) return false;

        int w = srcW / 2; if (w < 32) w = 32;
        int h = srcH / 2; if (h < 32) h = 32;

        // 创建 2 个 tex + 2 个 FBO; 任何一步失败则清理全部
        GLuint texs[2] = {0, 0};
        GLuint fbos[2] = {0, 0};
        bool ok = true;

        for (int i = 0; i < 2 && ok; ++i) {
            glGenTextures(1, &texs[i]);
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                         GL_RGBA, GL_HALF_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &fbos[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, texs[i], 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE) {
                CC::Log(CC::LOG_WARN,
                        "GL33: Phase E.6 CreateStreakTargets slot %d incomplete (0x%X)",
                        i, (unsigned)status);
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (int i = 0; i < 2; ++i) {
                if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
                if (texs[i]) glDeleteTextures(1, &texs[i]);
            }
            return false;
        }

        outFbos[0] = (uint32_t)fbos[0];
        outFbos[1] = (uint32_t)fbos[1];
        outTexs[0] = (uint32_t)texs[0];
        outTexs[1] = (uint32_t)texs[1];
        *outW = w;
        *outH = h;
        return true;
    }

    void DeleteStreakTargets(uint32_t* fbos, uint32_t* texs) override {
        if (!fbos || !texs) return;
        for (int i = 0; i < 2; ++i) {
            if (fbos[i]) { GLuint f = (GLuint)fbos[i]; glDeleteFramebuffers(1, &f); fbos[i] = 0; }
            if (texs[i]) { GLuint t = (GLuint)texs[i]; glDeleteTextures(1, &t);     texs[i] = 0; }
        }
    }

    /// Streak bright pass: v1 复用 Bloom programBloomBright (相同 threshold + soft knee)
    void DrawStreakBright(uint32_t hdrTex, uint32_t outFbo,
                           int w, int h, float threshold) override {
        if (!streakSupported || !bloomSupported || !hdrTex || !outFbo) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)outFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programBloomBright);
        if (locBloomBright_Threshold >= 0) glUniform1f(locBloomBright_Threshold, threshold);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// 1D 方向模糊: srcTex → dstFbo (7-tap 方向高斯)
    void DrawStreakBlur(uint32_t srcTex, uint32_t dstFbo,
                         int w, int h,
                         float length,
                         float dirX, float dirY) override {
        if (!streakSupported || !srcTex || !dstFbo || w <= 0 || h <= 0) return;

        float texelX = 1.0f / (float)w;
        float texelY = 1.0f / (float)h;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programStreakBlur);
        if (locStreakBlur_Texel     >= 0) glUniform2f(locStreakBlur_Texel, texelX, texelY);
        if (locStreakBlur_Length    >= 0) glUniform1f(locStreakBlur_Length, length);
        if (locStreakBlur_Direction >= 0) glUniform2f(locStreakBlur_Direction, dirX, dirY);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// 加性合成: streakTex × intensity → hdrFbo
    void DrawStreakComposite(uint32_t streakTex, uint32_t hdrFbo,
                              int w, int h, float intensity) override {
        if (!streakSupported || !streakTex || !hdrFbo || w <= 0 || h <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hdrFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        glUseProgram(programStreakComposite);
        if (locStreakComposite_Intensity >= 0) glUniform1f(locStreakComposite_Intensity, intensity);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)streakTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.7 — Lens Flare 虚接口实现 ====================

    bool SupportsLensFlare() const override { return lensFlareSupported; }

    /// 创建 lens flare ping-pong RT 对 (2 个 RGBA16F + FBO, 半分辨率) — 同 Streak 实现
    bool CreateLensFlareTargets(int srcW, int srcH,
                                 uint32_t* outFbos, uint32_t* outTexs,
                                 int* outW, int* outH) override {
        if (!lensFlareSupported || srcW <= 0 || srcH <= 0
            || !outFbos || !outTexs || !outW || !outH) return false;

        int w = srcW / 2; if (w < 32) w = 32;
        int h = srcH / 2; if (h < 32) h = 32;

        GLuint texs[2] = {0, 0};
        GLuint fbos[2] = {0, 0};
        bool ok = true;

        for (int i = 0; i < 2 && ok; ++i) {
            glGenTextures(1, &texs[i]);
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                         GL_RGBA, GL_HALF_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &fbos[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, texs[i], 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE) {
                CC::Log(CC::LOG_WARN,
                        "GL33: Phase E.7 CreateLensFlareTargets slot %d incomplete (0x%X)",
                        i, (unsigned)status);
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (int i = 0; i < 2; ++i) {
                if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
                if (texs[i]) glDeleteTextures(1, &texs[i]);
            }
            return false;
        }

        outFbos[0] = (uint32_t)fbos[0];
        outFbos[1] = (uint32_t)fbos[1];
        outTexs[0] = (uint32_t)texs[0];
        outTexs[1] = (uint32_t)texs[1];
        *outW = w;
        *outH = h;
        return true;
    }

    void DeleteLensFlareTargets(uint32_t* fbos, uint32_t* texs) override {
        if (!fbos || !texs) return;
        for (int i = 0; i < 2; ++i) {
            if (fbos[i]) { GLuint f = (GLuint)fbos[i]; glDeleteFramebuffers(1, &f); fbos[i] = 0; }
            if (texs[i]) { GLuint t = (GLuint)texs[i]; glDeleteTextures(1, &t);     texs[i] = 0; }
        }
    }

    /// Ghost + Halo + Chromatic Aberration: brightTex → dstFbo (覆盖写, 不开 blend)
    /// Phase E.7.4: flareTex==0 时 fallback 到 1x1 白 (不影响 procedural 行为)
    void DrawLensFlareGhost(uint32_t brightTex, uint32_t flareTex, uint32_t dstFbo,
                             int w, int h,
                             int ghostCount, float ghostDispersal,
                             float haloWidth, float chromaticAberration,
                             bool distortionEnabled) override {
        if (!lensFlareSupported || !brightTex || !dstFbo || w <= 0 || h <= 0) return;

        // Phase E.7.4: flareTex == 0 fallback 到 LensDirt 复用的 1x1 白纹理
        GLuint flare = flareTex ? (GLuint)flareTex : whiteTex1x1;
        if (!flare) return;   // 白纹理未创建 (极少发生)

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programLensFlareGhost);
        if (locLensFlare_GhostCount     >= 0) glUniform1i(locLensFlare_GhostCount, ghostCount);
        if (locLensFlare_GhostDispersal >= 0) glUniform1f(locLensFlare_GhostDispersal, ghostDispersal);
        if (locLensFlare_HaloWidth      >= 0) glUniform1f(locLensFlare_HaloWidth, haloWidth);
        if (locLensFlare_Aberration     >= 0) glUniform1f(locLensFlare_Aberration, chromaticAberration);
        if (locLensFlare_DistortionEn   >= 0) glUniform1i(locLensFlare_DistortionEn, distortionEnabled ? 1 : 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)brightTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, flare);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.8 — SSAO 虚接口实现 ====================
    //
    // 双 RT 旁路方案: HDR RT 零侵入; SSAO 自管 depth tex + FBO + AO ping-pong RT.
    // Composite 解 feedback loop: 用内部 ssaoCompTempTex 临时存 HDR color, 再做 modulate.

    bool SupportsSSAO() const override { return ssaoSupported; }

    /// 创建 SSAO 专用 depth tex + 小 FBO (无 color attachment, 仅作 blit 目标)
    bool CreateSSAODepthRT(int w, int h, uint32_t* outFbo, uint32_t* outTex) override {
        if (!ssaoSupported || w <= 0 || h <= 0 || !outFbo || !outTex) return false;

        // 1. 创建 depth texture (NEAREST + CLAMP_TO_EDGE, 与 HDR depth RB 同精度 24)
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return false;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 2. 小 FBO 仅 depth attachment (no color)
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) { glDeleteTextures(1, &tex); return false; }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
        // 无 color attachment: 用 glDrawBuffers (GL3.3 + GLES3 都支持; glDrawBuffer 是桌面专用)
        static const GLenum noneBufs[1] = { GL_NONE };
        glDrawBuffers(1, noneBufs);
        glReadBuffer(GL_NONE);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.8 CreateSSAODepthRT incomplete (0x%X, %dx%d)",
                    (unsigned)status, w, h);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return false;
        }

        *outFbo = (uint32_t)fbo;
        *outTex = (uint32_t)tex;
        return true;
    }

    void DeleteSSAODepthRT(uint32_t fbo, uint32_t tex) override {
        if (fbo) { GLuint f = (GLuint)fbo; glDeleteFramebuffers(1, &f); }
        if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t); }
    }

    /// 旁路核心: glBlitFramebuffer 从 HDR FBO 复制 depth 到 SSAO FBO
    /// Phase F.0.10.3: rgnW/H > 0 时仅 blit sub-rect (split-screen 必备); 0 = 全屏老路径
    void BlitHDRDepthToSSAO(uint32_t hdrFbo, uint32_t ssaoDepthFbo, int w, int h,
                            int rgnX, int rgnY, int rgnW, int rgnH) override {
        if (!ssaoSupported || !hdrFbo || !ssaoDepthFbo || w <= 0 || h <= 0) return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)hdrFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)ssaoDepthFbo);
        // Phase F.0.10.3 — sub-rect blit (与 MotionBlur Pass2 同模式)
        // 注: glBlitFramebuffer 不受 GL_SCISSOR_TEST 影响, 必须用 src/dst rect 显式控制
        if (rgnW > 0 && rgnH > 0) {
            glBlitFramebuffer(rgnX, rgnY, rgnX + rgnW, rgnY + rgnH,
                              rgnX, rgnY, rgnX + rgnW, rgnY + rgnH,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else {
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

    /// 创建 SSAO AO ping-pong RT (2 个 R16F, 半分辨率)
    bool CreateSSAOTargets(int srcW, int srcH,
                            uint32_t* outFbos, uint32_t* outTexs,
                            int* outW, int* outH) override {
        if (!ssaoSupported || srcW <= 0 || srcH <= 0
            || !outFbos || !outTexs || !outW || !outH) return false;

        int w = srcW / 2; if (w < 32) w = 32;
        int h = srcH / 2; if (h < 32) h = 32;

        GLuint texs[2] = {0, 0};
        GLuint fbos[2] = {0, 0};
        bool ok = true;

        for (int i = 0; i < 2 && ok; ++i) {
            glGenTextures(1, &texs[i]);
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            // R16F 单通道 (AO 灰度, blur pass 仍只读 .r)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0,
                         GL_RED, GL_HALF_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &fbos[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, texs[i], 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE) {
                CC::Log(CC::LOG_WARN,
                        "GL33: Phase E.8 CreateSSAOTargets slot %d incomplete (0x%X)",
                        i, (unsigned)status);
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (int i = 0; i < 2; ++i) {
                if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
                if (texs[i]) glDeleteTextures(1, &texs[i]);
            }
            return false;
        }

        outFbos[0] = (uint32_t)fbos[0];
        outFbos[1] = (uint32_t)fbos[1];
        outTexs[0] = (uint32_t)texs[0];
        outTexs[1] = (uint32_t)texs[1];
        *outW = w;
        *outH = h;
        return true;
    }

    void DeleteSSAOTargets(uint32_t* fbos, uint32_t* texs) override {
        if (!fbos || !texs) return;
        for (int i = 0; i < 2; ++i) {
            if (fbos[i]) { GLuint f = (GLuint)fbos[i]; glDeleteFramebuffers(1, &f); fbos[i] = 0; }
            if (texs[i]) { GLuint t = (GLuint)texs[i]; glDeleteTextures(1, &t);     texs[i] = 0; }
        }
    }

    /// 创建 4x4 RGBA8 noise (REPEAT + NEAREST), RGB = 单位 (x,y,0) 半球向量
    /// deterministic 算法 (避免 srand 影响外部状态)
    uint32_t CreateSSAONoiseTex() override {
        if (!ssaoSupported) return 0;
        // 16 个像素 × 4 通道, 用 LCG (linear congruential generator) 生成 deterministic 序列
        unsigned char data[16 * 4];
        unsigned int s = 1u;   // seed
        for (int i = 0; i < 16; ++i) {
            // LCG: X_{n+1} = (a*X_n + c) mod m, params from Numerical Recipes
            s = s * 1664525u + 1013904223u;
            float x = ((s & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;   // [-1, 1]
            s = s * 1664525u + 1013904223u;
            float y = ((s & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
            // 归一化 (避免零向量, 但概率极低)
            float len = sqrtf(x * x + y * y);
            if (len < 1e-4f) { x = 1.0f; y = 0.0f; len = 1.0f; }
            x /= len; y /= len;
            // pack: [-1,1] -> [0,255]; shader 内部 *2-1 还原
            data[i * 4 + 0] = (unsigned char)((x * 0.5f + 0.5f) * 255.0f);
            data[i * 4 + 1] = (unsigned char)((y * 0.5f + 0.5f) * 255.0f);
            data[i * 4 + 2] = 128;  // z = 0
            data[i * 4 + 3] = 255;
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return 0;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
        return (uint32_t)tex;
    }

    void DeleteSSAONoiseTex(uint32_t tex) override {
        if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t); }
    }

    /// SSAO raw pass: depthTex + noiseTex + normalTex (Phase E.8.x) -> dstFbo (R16F)
    void DrawSSAO(uint32_t depthTex, uint32_t noiseTex, uint32_t normalTex,
                  uint32_t dstFbo,
                  int w, int h,
                  const float* projMat4, const float* invProjMat4,
                  const float* kernel, int kernelSize,
                  float radius, float bias, float power) override {
        if (!ssaoSupported || !depthTex || !noiseTex || !normalTex || !dstFbo
            || w <= 0 || h <= 0 || !projMat4 || !invProjMat4 || !kernel
            || kernelSize <= 0 || kernelSize > 16) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programSSAO);
        if (locSSAO_Proj       >= 0) glUniformMatrix4fv(locSSAO_Proj,    1, GL_FALSE, projMat4);
        if (locSSAO_InvProj    >= 0) glUniformMatrix4fv(locSSAO_InvProj, 1, GL_FALSE, invProjMat4);
        if (locSSAO_Kernel     >= 0) glUniform3fv(locSSAO_Kernel, kernelSize, kernel);
        if (locSSAO_KernelSize >= 0) glUniform1i(locSSAO_KernelSize, kernelSize);
        if (locSSAO_Radius     >= 0) glUniform1f(locSSAO_Radius, radius);
        if (locSSAO_Bias       >= 0) glUniform1f(locSSAO_Bias, bias);
        if (locSSAO_Power      >= 0) glUniform1f(locSSAO_Power, power);
        // noise tile 缩放: AO RT 是半分辨率, noise 4x4, 故 (w / 4) 即可
        if (locSSAO_NoiseScale >= 0) glUniform2f(locSSAO_NoiseScale, (float)w / 4.0f, (float)h / 4.0f);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)depthTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)noiseTex);
        glActiveTexture(GL_TEXTURE2);                               // Phase E.8.x — G-buffer normal
        glBindTexture(GL_TEXTURE_2D, (GLuint)normalTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        // 反向解绑 (slot 2 → 0)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// 双边分离滤波: srcAOTex + depthTex -> dstFbo (axis: 0=水平, 1=垂直)
    void DrawSSAOBlur(uint32_t srcAOTex, uint32_t depthTex, uint32_t dstFbo,
                      int w, int h, int axis) override {
        if (!ssaoSupported || !srcAOTex || !depthTex || !dstFbo || w <= 0 || h <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programSSAOBlur);
        if (locSSAOBlur_Texel >= 0) glUniform2f(locSSAOBlur_Texel, 1.0f / (float)w, 1.0f / (float)h);
        if (locSSAOBlur_Axis  >= 0) glUniform1i(locSSAOBlur_Axis, axis ? 1 : 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcAOTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)depthTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// HDR composite: 解 feedback loop
    /// 流程: ① glBlitFramebuffer 从 dstFbo 复制 color 到 ssaoCompTempTex (full-res RGBA16F)
    ///       ② 绑 dstFbo, 用 ssaoCompTempTex + aoTex 输入, shader 写到 dstFbo
    void DrawSSAOComposite(uint32_t aoTex, uint32_t dstFbo,
                            int w, int h, float intensity) override {
        if (!ssaoSupported || !aoTex || !dstFbo || w <= 0 || h <= 0) return;

        // 懒重建 composite temp RT (尺寸变化时)
        if (ssaoCompTempW != w || ssaoCompTempH != h || !ssaoCompTempTex) {
            if (ssaoCompTempFbo) { glDeleteFramebuffers(1, &ssaoCompTempFbo); ssaoCompTempFbo = 0; }
            if (ssaoCompTempTex) { glDeleteTextures(1, &ssaoCompTempTex);     ssaoCompTempTex = 0; }

            glGenTextures(1, &ssaoCompTempTex);
            if (!ssaoCompTempTex) return;
            glBindTexture(GL_TEXTURE_2D, ssaoCompTempTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &ssaoCompTempFbo);
            if (!ssaoCompTempFbo) {
                glDeleteTextures(1, &ssaoCompTempTex); ssaoCompTempTex = 0;
                return;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, ssaoCompTempFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, ssaoCompTempTex, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                glDeleteFramebuffers(1, &ssaoCompTempFbo); ssaoCompTempFbo = 0;
                glDeleteTextures(1, &ssaoCompTempTex);     ssaoCompTempTex = 0;
                return;
            }
            ssaoCompTempW = w;
            ssaoCompTempH = h;
        }

        // ① blit dstFbo (HDR color) -> ssaoCompTempFbo (临时 copy)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)dstFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ssaoCompTempFbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // ② 绑 dstFbo, shader 读 ssaoCompTempTex + aoTex, 写到 dstFbo
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glUseProgram(programSSAOComposite);
        if (locSSAOComp_Intensity >= 0) glUniform1f(locSSAOComp_Intensity, intensity);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ssaoCompTempTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)aoTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.9 — SSR 虚接口实现 ====================
    //
    // 设计与 SSAO 平行: 独立 depth tex 旁路 (复用 BlitHDRDepthToSSAO 接口) +
    // full-res RGBA16F 反射 RT + composite 用临时 RT 解 feedback loop.

    bool SupportsSSR() const override { return ssrSupported; }

    /// 创建 SSR 专用 depth tex + 小 FBO (无 color attachment, full-res, 与 SSAO 同模式)
    bool CreateSSRDepthRT(int w, int h, uint32_t* outFbo, uint32_t* outTex) override {
        if (!ssrSupported || w <= 0 || h <= 0 || !outFbo || !outTex) return false;

        // depth texture (NEAREST + CLAMP_TO_EDGE, depth24)
        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return false;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) { glDeleteTextures(1, &tex); return false; }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
        // 无 color attachment: 显式禁用 draw/read buffer
        GLenum drawBufs[1] = { GL_NONE };
        glDrawBuffers(1, drawBufs);
        glReadBuffer(GL_NONE);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.9 CreateSSRDepthRT incomplete (0x%X, %dx%d)",
                    (unsigned)status, w, h);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return false;
        }
        *outFbo = (uint32_t)fbo;
        *outTex = (uint32_t)tex;
        return true;
    }

    void DeleteSSRDepthRT(uint32_t fbo, uint32_t tex) override {
        if (fbo) { GLuint f = (GLuint)fbo; glDeleteFramebuffers(1, &f); }
        if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t); }
    }

    /// 创建 SSR 反射 RT: full-res RGBA16F + GL_LINEAR + GL_CLAMP_TO_EDGE, 无 depth
    bool CreateSSRTargets(int w, int h, uint32_t* outFbo, uint32_t* outTex) override {
        if (!ssrSupported || w <= 0 || h <= 0 || !outFbo || !outTex) return false;

        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return false;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) { glDeleteTextures(1, &tex); return false; }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.9 CreateSSRTargets incomplete (0x%X, %dx%d)",
                    (unsigned)status, w, h);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return false;
        }
        *outFbo = (uint32_t)fbo;
        *outTex = (uint32_t)tex;
        return true;
    }

    void DeleteSSRTargets(uint32_t* fbo, uint32_t* tex) override {
        if (fbo && *fbo) { GLuint f = (GLuint)*fbo; glDeleteFramebuffers(1, &f); *fbo = 0; }
        if (tex && *tex) { GLuint t = (GLuint)*tex; glDeleteTextures(1, &t);     *tex = 0; }
    }

    /// SSR raw pass: depthTex + normalTex + hdrTex -> dstFbo (RGBA16F, full-res)
    /// Phase E.12: jitterX/jitterY 是像素单位偏移 (±0.5 范围), backend 内除尺寸转 UV 空间
    /// Phase F.0.10.3: rgnW/H > 0 时 scissor 限定 raster 写区域 (ray march shader 内仍全屏)
    void DrawSSR(uint32_t depthTex, uint32_t normalTex, uint32_t hdrTex,
                 uint32_t dstFbo,
                 int w, int h,
                 const float* projMat4, const float* invProjMat4,
                 int maxSteps, float stepSize, float thickness,
                 float maxDist, float edgeFade,
                 float jitterX, float jitterY,
                 int rgnX, int rgnY, int rgnW, int rgnH) override {
        if (!ssrSupported || !depthTex || !normalTex || !hdrTex || !dstFbo
            || w <= 0 || h <= 0 || !projMat4 || !invProjMat4 || maxSteps <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        // Phase F.0.10.3 — scissor 限定 region 写区域 (ray march 仍跨边界采样, 反射借邻区合理)
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_BLEND);

        glUseProgram(programSSR);
        if (locSSR_Proj        >= 0) glUniformMatrix4fv(locSSR_Proj,    1, GL_FALSE, projMat4);
        if (locSSR_InvProj     >= 0) glUniformMatrix4fv(locSSR_InvProj, 1, GL_FALSE, invProjMat4);
        if (locSSR_MaxSteps    >= 0) glUniform1i(locSSR_MaxSteps,    maxSteps);
        if (locSSR_StepSize    >= 0) glUniform1f(locSSR_StepSize,    stepSize);
        if (locSSR_Thickness   >= 0) glUniform1f(locSSR_Thickness,   thickness);
        if (locSSR_MaxDistance >= 0) glUniform1f(locSSR_MaxDistance, maxDist);
        if (locSSR_EdgeFade    >= 0) glUniform1f(locSSR_EdgeFade,    edgeFade);
        // Phase E.12 — jitter (pixel 转 UV): jitter / size
        if (locSSR_JitterOffset >= 0) {
            glUniform2f(locSSR_JitterOffset,
                        jitterX / (float)w, jitterY / (float)h);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)depthTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)normalTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        // 反向解绑 (slot 2 → 0)
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// SSR composite: 解 feedback loop (与 SSAO 同模式)
    /// 流程: ① glBlitFramebuffer 复制 hdrFbo color -> ssrCompTempTex
    ///       ② 绑 hdrFbo, shader 读 ssrCompTempTex + reflectTex, 加性写到 hdrFbo
    /// Phase F.0.10.3: rgnW/H > 0 时 ① blit 子矩形 + ② shader 加 scissor (additive write)
    void DrawSSRComposite(uint32_t reflectTex, uint32_t hdrFbo,
                          int w, int h, float intensity,
                          int rgnX, int rgnY, int rgnW, int rgnH) override {
        if (!ssrSupported || !reflectTex || !hdrFbo || w <= 0 || h <= 0) return;

        // 懒重建 composite temp RT (尺寸变化时)
        if (ssrCompTempW != w || ssrCompTempH != h || !ssrCompTempTex) {
            if (ssrCompTempFbo) { glDeleteFramebuffers(1, &ssrCompTempFbo); ssrCompTempFbo = 0; }
            if (ssrCompTempTex) { glDeleteTextures(1, &ssrCompTempTex);     ssrCompTempTex = 0; }

            glGenTextures(1, &ssrCompTempTex);
            if (!ssrCompTempTex) return;
            glBindTexture(GL_TEXTURE_2D, ssrCompTempTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &ssrCompTempFbo);
            if (!ssrCompTempFbo) {
                glDeleteTextures(1, &ssrCompTempTex); ssrCompTempTex = 0;
                return;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, ssrCompTempFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, ssrCompTempTex, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                glDeleteFramebuffers(1, &ssrCompTempFbo); ssrCompTempFbo = 0;
                glDeleteTextures(1, &ssrCompTempTex);     ssrCompTempTex = 0;
                return;
            }
            ssrCompTempW = w;
            ssrCompTempH = h;
        }

        // ① blit hdrFbo (HDR color) -> ssrCompTempFbo (临时 copy)
        // Phase F.0.10.3 — sub-rect blit (节省 IO + 防越界, 仅 copy 本 region)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)hdrFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ssrCompTempFbo);
        if (rgnW > 0 && rgnH > 0) {
            glBlitFramebuffer(rgnX, rgnY, rgnX + rgnW, rgnY + rgnH,
                              rgnX, rgnY, rgnX + rgnW, rgnY + rgnH,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        // ② 绑 hdrFbo, shader 读 ssrCompTempTex + reflectTex, 加性写到 hdrFbo
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hdrFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        // Phase F.0.10.3 — scissor 限定 composite 写区域
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_BLEND);   // shader 内部 hdr + reflect, 不用硬件 blend

        glUseProgram(programSSRComposite);
        if (locSSRComp_Intensity >= 0) glUniform1f(locSSRComp_Intensity, intensity);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ssrCompTempTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)reflectTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.10 — SSR Blur 实现 ====================
    //
    // 设计: half-res ping-pong (用户拍板 2026-05-12), 内存代价 1080p ~4MB,
    //       性能 < 0.3 ms (vs Phase E.9 SSR 主 pass ~3 ms).
    //       反射 upscale 由 composite 阶段硬件 bilinear filter 自动完成.

    /// 创建 half-res blur ping-pong RT (RGBA16F × 2)
    bool CreateSSRBlurRT(int wFull, int hFull,
                          uint32_t* outFbos, uint32_t* outTexs,
                          int* outW, int* outH) override {
        // 防御性检查
        if (!ssrBlurSupported || !outFbos || !outTexs || !outW || !outH ||
            wFull <= 0 || hFull <= 0) {
            if (outW) *outW = 0;
            if (outH) *outH = 0;
            return false;
        }
        // half-res: 至少 1×1 像素
        int hw = (wFull / 2 > 0) ? (wFull / 2) : 1;
        int hh = (hFull / 2 > 0) ? (hFull / 2) : 1;

        // 先全部清零, 后续失败时可统一调 DeleteSSRBlurRT 清理
        outFbos[0] = outFbos[1] = 0;
        outTexs[0] = outTexs[1] = 0;

        for (int i = 0; i < 2; ++i) {
            glGenTextures(1, &outTexs[i]);
            if (!outTexs[i]) { DeleteSSRBlurRT(outFbos, outTexs); *outW = *outH = 0; return false; }
            glBindTexture(GL_TEXTURE_2D, outTexs[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, hw, hh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            // bilinear filter: composite 阶段读 half-res 自动 upscale
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &outFbos[i]);
            if (!outFbos[i]) { DeleteSSRBlurRT(outFbos, outTexs); *outW = *outH = 0; return false; }
            glBindFramebuffer(GL_FRAMEBUFFER, outFbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, outTexs[i], 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                DeleteSSRBlurRT(outFbos, outTexs);
                *outW = *outH = 0;
                return false;
            }
        }
        *outW = hw;
        *outH = hh;
        return true;
    }

    void DeleteSSRBlurRT(uint32_t* fbos, uint32_t* texs) override {
        if (fbos) {
            for (int i = 0; i < 2; ++i) {
                if (fbos[i]) { glDeleteFramebuffers(1, &fbos[i]); fbos[i] = 0; }
            }
        }
        if (texs) {
            for (int i = 0; i < 2; ++i) {
                if (texs[i]) { glDeleteTextures(1, &texs[i]); texs[i] = 0; }
            }
        }
    }

    /// separable Gaussian blur pass: srcTex -> dstFbo
    /// axis 0=horizontal, 1=vertical; uTexel = 1/dstSize.
    /// Phase F.0.10.3: rgnW/H > 0 时 scissor 限定 half-res blur 写区域
    ///                  (caller 负责将 full-res region 缩半传入)
    void DrawSSRBlur(uint32_t srcTex, uint32_t depthTex,
                     uint32_t dstFbo, int dstW, int dstH,
                     int axis, float radius,
                     bool bilateralEnabled, float depthSigma,
                     int rgnX, int rgnY, int rgnW, int rgnH) override {
        // Phase E.11: depthTex 必须非零 (bilateral=true 时依赖; bilateral=false 时 shader
        // 不采样 depthTex, 但本函数仍会 bind slot 1 避免 driver 状态不一致 — 所以仍要严检)
        if (!ssrBlurSupported || !programSSRBlur || !srcTex || !depthTex || !dstFbo ||
            dstW <= 0 || dstH <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        // Phase F.0.10.3 — scissor 限定 half-res blur 写区域 (region 已是 half-res 空间)
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        glUseProgram(programSSRBlur);
        if (locSSRBlur_Texel      >= 0) glUniform2f(locSSRBlur_Texel,  1.0f / (float)dstW, 1.0f / (float)dstH);
        if (locSSRBlur_Axis       >= 0) glUniform1i(locSSRBlur_Axis,   axis ? 1 : 0);
        if (locSSRBlur_Radius     >= 0) glUniform1f(locSSRBlur_Radius, radius);
        // Phase E.11 — dual-mode uniform
        if (locSSRBlur_Bilateral  >= 0) glUniform1i(locSSRBlur_Bilateral,  bilateralEnabled ? 1 : 0);
        if (locSSRBlur_DepthSigma >= 0) glUniform1f(locSSRBlur_DepthSigma, depthSigma);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);
        glActiveTexture(GL_TEXTURE1);                              // Phase E.11 — depth slot 1
        glBindTexture(GL_TEXTURE_2D, (GLuint)depthTex);

        // 复用 tonemap fullscreen VAO (6 vert triangles, NDC 直绘)
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        // 反向解绑 (slot 1 → 0), 与 DrawSSAO / DrawSSAOBlur 同模式
        glActiveTexture(GL_TEXTURE1);                              // Phase E.11
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.12 — Temporal SSR 虚接口实现 ====================
    //
    // 设计与 SSR Blur 平行: history ping-pong RGBA16F (与 SSR raw 同尺寸, full-res) +
    // temporal pass (reproject + neighborhood clip + history blend).
    // 用户拍板 2026-05-14: 全 A 组合 (TAA-style 业界标准).

    /// Phase E.12 — 创建 SSR temporal history ping-pong RT (full-res RGBA16F × 2)
    /// 与 CreateSSRTargets 同模式, 但一次创建两个 (ping-pong).
    bool CreateSSRHistoryRT(int w, int h,
                             uint32_t* outFbos, uint32_t* outTexs) override {
        if (outFbos) outFbos[0] = outFbos[1] = 0;
        if (outTexs) outTexs[0] = outTexs[1] = 0;
        if (!ssrTemporalSupported || w <= 0 || h <= 0 || !outFbos || !outTexs) return false;

        for (int i = 0; i < 2; ++i) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            if (!tex) { DeleteSSRHistoryRT(outFbos, outTexs); return false; }
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            // LINEAR: temporal reproject UV 取样 (可能非对齐) 需双线性
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            if (!fbo) {
                glDeleteTextures(1, &tex);
                DeleteSSRHistoryRT(outFbos, outTexs);
                return false;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                CC::Log(CC::LOG_WARN,
                        "GL33: Phase E.12 CreateSSRHistoryRT incomplete (0x%X, %dx%d, idx=%d)",
                        (unsigned)status, w, h, i);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &tex);
                DeleteSSRHistoryRT(outFbos, outTexs);
                return false;
            }
            outFbos[i] = (uint32_t)fbo;
            outTexs[i] = (uint32_t)tex;
        }
        return true;
    }

    void DeleteSSRHistoryRT(uint32_t* fbos, uint32_t* texs) override {
        if (fbos) {
            for (int i = 0; i < 2; ++i) {
                if (fbos[i]) { GLuint f = (GLuint)fbos[i]; glDeleteFramebuffers(1, &f); fbos[i] = 0; }
            }
        }
        if (texs) {
            for (int i = 0; i < 2; ++i) {
                if (texs[i]) { GLuint t = (GLuint)texs[i]; glDeleteTextures(1, &t); texs[i] = 0; }
            }
        }
    }

    /// Phase E.12 — Temporal pass: reproject + neighborhood clip + history blend
    /// Phase E.14 — 末尾增 3 个 trailing 参数：dilation、velocityScale、velocityFormat
    /// shader 内部的 hasHistory=0 (首帧) 路径会强制输出 cur, 避免 1-frame 黑帧.
    /// Phase F.0.10.3: rgnW/H > 0 时 scissor 限定 history write 区域 (防写脏邻 region; reproject 跨 region 读 history 不受影响)
    void DrawSSRTemporal(uint32_t curReflectTex,
                          uint32_t historyTex,
                          uint32_t depthTex,
                          uint32_t velocityTex,
                          uint32_t dstFbo,
                          int w, int h,
                          const float* reprojectMat4,
                          const float* invProjMat4,
                          float blendAlpha,
                          int   rejectionMode,
                          int   hasHistory,
                          bool           velocityDilation,
                          float          velocityScale,
                          VelocityFormat velocityFormat,
                          int rgnX, int rgnY, int rgnW, int rgnH) override {
        if (!ssrTemporalSupported || !programSSRTemporal
            || !curReflectTex || !depthTex || !dstFbo
            || w <= 0 || h <= 0 || !reprojectMat4 || !invProjMat4) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        // Phase F.0.10.3 — scissor 限定 history write 区域
        if (rgnW > 0 && rgnH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        glUseProgram(programSSRTemporal);
        if (locSSRTemporal_ReprojectMat  >= 0) glUniformMatrix4fv(locSSRTemporal_ReprojectMat,  1, GL_FALSE, reprojectMat4);
        if (locSSRTemporal_InvProj       >= 0) glUniformMatrix4fv(locSSRTemporal_InvProj,       1, GL_FALSE, invProjMat4);
        if (locSSRTemporal_Texel         >= 0) glUniform2f(locSSRTemporal_Texel, 1.0f / (float)w, 1.0f / (float)h);
        if (locSSRTemporal_BlendAlpha    >= 0) glUniform1f(locSSRTemporal_BlendAlpha,    blendAlpha);
        if (locSSRTemporal_RejectionMode >= 0) glUniform1i(locSSRTemporal_RejectionMode, rejectionMode);
        if (locSSRTemporal_HasHistory    >= 0) glUniform1i(locSSRTemporal_HasHistory,    hasHistory);
        if (locSSRTemporal_HasVelocityTex >= 0) glUniform1i(locSSRTemporal_HasVelocityTex, velocityTex ? 1 : 0);
        // Phase E.14 — dilation / format / scale uniform 上传
        // Phase E.18: 若 dilation pass 已执行 (dilationPassActive_=true), shader 单点采 dilatedTex,
        //             强制 uVelocityDilation=0 (跳过 shader 内 inline 9-tap);
        //             否则沿用 velocityDilation 旧逻辑 (shader 内 inline 9-tap)
        const int ssrUVDValue = dilationPassActive_ ? 0 : (velocityDilation ? 1 : 0);
        if (locSSRTemporal_VelocityDilation >= 0) glUniform1i(locSSRTemporal_VelocityDilation, ssrUVDValue);
        if (locSSRTemporal_VelocityFormat   >= 0) glUniform1i(locSSRTemporal_VelocityFormat,   (velocityFormat == VelocityFormat::RG8) ? 1 : 0);
        if (locSSRTemporal_VelocityScale    >= 0) glUniform1f(locSSRTemporal_VelocityScale,    velocityScale);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)curReflectTex);
        glActiveTexture(GL_TEXTURE1);
        // historyTex 可能为 0 (首帧); shader 内 hasHistory==0 不采样, 但 driver 需有有效 binding.
        // 首帧场景下传 0 可能报 GL_INVALID_OPERATION, 回退用 curReflectTex 作占位
        glBindTexture(GL_TEXTURE_2D, historyTex ? (GLuint)historyTex : (GLuint)curReflectTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, (GLuint)depthTex);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, velocityTex ? (GLuint)velocityTex : (GLuint)curReflectTex);

        // 复用 tonemap fullscreen VAO
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        // 反向解绑 (slot 3 → 0)
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.3 — 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// 获取当前 projection / view 矩阵 (SSAO 重建 view pos 需要)
    void GetProjection(float* out16) const override {
        if (!out16) return;
        memcpy(out16, projection.m, sizeof(float) * 16);
    }

    void GetView(float* out16) const override {
        if (!out16) return;
        if (hasView) memcpy(out16, viewMatrix.m, sizeof(float) * 16);
        else {
            // 未 SetCamera: 返回 identity (Lit2D 场景的默认行为)
            for (int i = 0; i < 16; ++i) out16[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
    }

    /// Phase F.0.13 — camera 帧间运动幅度标量 (Frobenius distance of viewProj matrix)
    /// 用于 TAARenderer motion-adaptive sharpness 在高速时自动降低 sharpness, 减 reprojection trail
    /// 算法: ssd = sum( (curVP[i] - prevVP[i])^2 ), return sqrt(ssd)
    /// 同时反映平移 + 旋转 + FoV 变化, 简单稳定; 经验上 0=静止, ~1 中等, >2 高速
    /// 首帧 hasPrevViewProjForVelocity=false 时返 0 (避免误判为高速)
    float ComputeCameraMotionScalar() const override {
        if (!hasPrevViewProjForVelocity) return 0.0f;
        // ComputeViewProj3D 已是 const, 直接调用 (仅计算 projection * view)
        const Mat4 cur = ComputeViewProj3D();
        float ssd = 0.0f;
        for (int i = 0; i < 16; ++i) {
            const float d = cur.m[i] - prevViewProj.m[i];
            ssd += d * d;
        }
        return std::sqrt(ssd);
    }

    // ==================== Phase E.1.5 — Lit2D 绘制实现 ====================
    //
    // 结构:
    //   - EnsureLit2DVBOCapacity: 动态扩容 vboLit2D (为 DrawLit2DTriangles 任意 count 准备)
    //   - BeginLit2DDraw / EndLit2DDraw: prologue + epilogue (被 Quad / Triangles / Mesh draw 复用)
    //   - UploadLighting2D: 按 active state SOA 上传 8 个 uniform 数组 + ambient + count
    //   - DrawLit2DQuad: 4 verts 动态 VBO + 静态 EBO + glDrawElements
    //   - DrawLit2DTriangles: 任意 count verts + glDrawArrays
    //   - CreateLit2DMesh / DeleteLit2DMesh: 独立 VAO+VBO+EBO 永久 mesh, 与 CreateMesh 平行

    /// 保证 vboLit2D 能容纳 vertexCount 个顶点; 不够则 ×2 增长
    /// VBO 重分配不影响 vaoLit2D 内存储的 vertex attribute 指针 (VAO 绑定 vbo handle 不变)
    void EnsureLit2DVBOCapacity(int vertexCount) {
        if (vertexCount <= vboLit2DCapacity) return;
        int newCap = vboLit2DCapacity ? vboLit2DCapacity : LIT2D_VBO_INITIAL_VERTS;
        while (newCap < vertexCount) newCap *= 2;
        glBindBuffer(GL_ARRAY_BUFFER, vboLit2D);
        glBufferData(GL_ARRAY_BUFFER,
                     newCap * (GLsizeiptr)sizeof(RenderVertex2DLit),
                     nullptr, GL_DYNAMIC_DRAW);
        vboLit2DCapacity = newCap;
    }

    /// Lit2D draw prologue: 切 program + MVP/Model + 绑 baseColor/normalMap + 上传 lighting state
    /// 不绑 VAO (由 caller 选用 vaoLit2D / mesh.vao)
    void BeginLit2DDraw(uint32_t baseColorTex, uint32_t normalMapTex) {
        glUseProgram(programLit2D);

        // MVP 与默认 2D 一致 (projection * modelview); Model = modelview 供 shader 变换法线到世界
        Mat4 mvp = projection * modelview;
        if (locLit2D_MVP   >= 0) glUniformMatrix4fv(locLit2D_MVP,   1, GL_FALSE, mvp.m);
        if (locLit2D_Model >= 0) glUniformMatrix4fv(locLit2D_Model, 1, GL_FALSE, modelview.m);

        // baseColor 绑 slot 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, baseColorTex);
        // normalMap 绑 slot 1 (为 0 时 GL 会绑到"默认纹理", uHasNormalMap=0 后 shader 不采样)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, normalMapTex);
        if (locLit2D_HasNormalMap >= 0) {
            glUniform1i(locLit2D_HasNormalMap, normalMapTex ? 1 : 0);
        }

        // 上传 Lighting2D 状态 (转发到 backend->UploadLighting2D)
        Lighting2D::UploadToShader(this, programLit2D);
    }

    /// Lit2D draw epilogue: 解绑 VAO + 切回默认 2D shader + active texture 复位到 slot 0
    void EndLit2DDraw() {
        glBindVertexArray(0);
        glUseProgram(program);
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);
    }

    void UploadLighting2D(const Lighting2D::State* state) override {
        if (!state || !lit2DSupported) return;

        // 独立调用时 caller 可能未 glUseProgram(programLit2D), 主动切过去保证 glUniform 生效
        glUseProgram(programLit2D);

        // Phase E.2.1 — dirty bit: state 自上次上传以来未变, 跳过所有 glUniform*v
        // 注: 仍要 glUseProgram(programLit2D) 因为 caller 可能切了其它 program
        if (state->version == lastUploadedLighting2DVersion) {
            return;
        }
        lastUploadedLighting2DVersion = state->version;

        // build SOA 临时数组 (栈分配, 16 light 总共 ~600 bytes, 远低于线程栈默认 1MB)
        constexpr int M = Lighting2D::MAX_LIGHTS;  // 16
        int   types[M];
        float poss[M * 3];   // shader 侧 vec3, z 强制 0 (2D 场景)
        float dirs[M * 3];
        float cols[M * 3];
        float rngs[M];
        float intens[M];
        float ics[M];
        float ocs[M];
        int cnt = 0;
        for (int i = 0; i < M; ++i) {
            const auto& l = state->lights[i];
            if (l.type == Lighting2D::TYPE_INACTIVE) continue;
            types[cnt]      = l.type;
            poss[cnt*3+0]   = l.pos[0];   poss[cnt*3+1] = l.pos[1];   poss[cnt*3+2] = 0.0f;
            dirs[cnt*3+0]   = l.dir[0];   dirs[cnt*3+1] = l.dir[1];   dirs[cnt*3+2] = 0.0f;
            cols[cnt*3+0]   = l.color[0]; cols[cnt*3+1] = l.color[1]; cols[cnt*3+2] = l.color[2];
            rngs[cnt]       = l.range;
            intens[cnt]     = l.intensity;
            ics[cnt]        = l.innerCos;
            ocs[cnt]        = l.outerCos;
            ++cnt;
        }

        if (locLit2D_Ambient    >= 0) glUniform3fv(locLit2D_Ambient, 1, state->ambient);
        if (locLit2D_LightCount >= 0) glUniform1i (locLit2D_LightCount, cnt);
        if (cnt > 0) {
            if (locLit2D_LightType      >= 0) glUniform1iv(locLit2D_LightType,      cnt, types);
            if (locLit2D_LightPos       >= 0) glUniform3fv(locLit2D_LightPos,       cnt, poss);
            if (locLit2D_LightDir       >= 0) glUniform3fv(locLit2D_LightDir,       cnt, dirs);
            if (locLit2D_LightColor     >= 0) glUniform3fv(locLit2D_LightColor,     cnt, cols);
            if (locLit2D_LightRange     >= 0) glUniform1fv(locLit2D_LightRange,     cnt, rngs);
            if (locLit2D_LightIntensity >= 0) glUniform1fv(locLit2D_LightIntensity, cnt, intens);
            if (locLit2D_LightInnerCos  >= 0) glUniform1fv(locLit2D_LightInnerCos,  cnt, ics);
            if (locLit2D_LightOuterCos  >= 0) glUniform1fv(locLit2D_LightOuterCos,  cnt, ocs);
        }
    }

    void DrawLit2DQuad(const RenderVertex2DLit verts[4],
                       uint32_t baseColorTex,
                       uint32_t normalMapTex) override {
        if (!lit2DSupported || !verts) return;
        BeginLit2DDraw(baseColorTex, normalMapTex);

        glBindVertexArray(vaoLit2D);
        glBindBuffer(GL_ARRAY_BUFFER, vboLit2D);
        EnsureLit2DVBOCapacity(4);  // 初始容量已 = 4, 默认 no-op
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        4 * (GLsizeiptr)sizeof(RenderVertex2DLit), verts);
        // 静态 EBO [0,1,2, 0,2,3] 在 InitLit2D 中上传
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

        EndLit2DDraw();
    }

    void DrawLit2DTriangles(const RenderVertex2DLit* verts, int count,
                            uint32_t baseColorTex,
                            uint32_t normalMapTex) override {
        if (!lit2DSupported || !verts || count <= 0 || (count % 3) != 0) return;
        BeginLit2DDraw(baseColorTex, normalMapTex);

        glBindVertexArray(vaoLit2D);
        glBindBuffer(GL_ARRAY_BUFFER, vboLit2D);
        EnsureLit2DVBOCapacity(count);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        count * (GLsizeiptr)sizeof(RenderVertex2DLit), verts);
        // 任意三角形流 → 不用 EBO (顺序读顶点)
        glDrawArrays(GL_TRIANGLES, 0, count);

        EndLit2DDraw();
    }

    /// Phase E.2.3 — 保证 eboLit2DBatch 能容纳 idxCount 个 uint32; 不够则 ×2 增长
    void EnsureLit2DEBOBatchCapacity(int idxCount) {
        if ((GLsizeiptr)idxCount <= eboLit2DBatchCapacity) return;
        GLsizeiptr newCap = eboLit2DBatchCapacity ? eboLit2DBatchCapacity : 256 * 6;
        while (newCap < (GLsizeiptr)idxCount) newCap *= 2;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2DBatch);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     newCap * (GLsizeiptr)sizeof(uint32_t),
                     nullptr, GL_DYNAMIC_DRAW);
        eboLit2DBatchCapacity = newCap;
    }

    /// Phase E.2.3 — 批量 Lit2D 绘制 (动态 VBO + 动态 EBO + 单次 lighting upload)
    /// 关键: 临时切到 eboLit2DBatch 后, 必须 glBindBuffer 恢复到 eboLit2D, 否则
    /// vaoLit2D 内的 GL_ELEMENT_ARRAY_BUFFER 绑定会被改变, 下次 DrawLit2DQuad 会读错索引.
    void DrawLit2DBatch(const RenderVertex2DLit* verts, int vertCount,
                        const uint32_t* indices, int idxCount,
                        uint32_t baseColorTex,
                        uint32_t normalMapTex) override {
        if (!lit2DSupported || !verts || vertCount <= 0 ||
            !indices || idxCount <= 0 || (idxCount % 3) != 0) return;
        if (!eboLit2DBatch) return;  // Init 时分配失败的 fallback

        BeginLit2DDraw(baseColorTex, normalMapTex);

        glBindVertexArray(vaoLit2D);

        // 上传顶点 (动态 VBO 已绑到 vaoLit2D)
        glBindBuffer(GL_ARRAY_BUFFER, vboLit2D);
        EnsureLit2DVBOCapacity(vertCount);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        vertCount * (GLsizeiptr)sizeof(RenderVertex2DLit), verts);

        // 临时切到动态 EBO 上传索引
        // (注: vaoLit2D 当前绑定的是静态 eboLit2D, 我们 *改* GL_ELEMENT_ARRAY_BUFFER 绑定后,
        //  glDrawElements 用的是 当前绑定 的 EBO, 不是 VAO 创建时绑定的; 但 VAO 会"记忆"
        //  GL_ELEMENT_ARRAY_BUFFER 的最后一次绑定 — 所以画完后必须切回 eboLit2D.)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2DBatch);
        EnsureLit2DEBOBatchCapacity(idxCount);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                        idxCount * (GLsizeiptr)sizeof(uint32_t), indices);

        glDrawElements(GL_TRIANGLES, idxCount, GL_UNSIGNED_INT, nullptr);

        // 恢复静态 EBO 绑定 (vaoLit2D 必须始终指向 eboLit2D, 否则 DrawLit2DQuad 会用错索引)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboLit2D);

        EndLit2DDraw();
    }

    uint32_t CreateLit2DMesh(const RenderVertex2DLit* verts, int vCount,
                              const uint32_t* indices, int iCount) override {
        if (!lit2DSupported || !verts || vCount <= 0 || !indices || iCount <= 0 || (iCount % 3) != 0) {
            return 0;
        }

        MeshGPU m;
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ebo);
        if (!m.vao || !m.vbo || !m.ebo) {
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            return 0;
        }
        glBindVertexArray(m.vao);

        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vCount * (GLsizeiptr)sizeof(RenderVertex2DLit),
                     verts, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, iCount * (GLsizeiptr)sizeof(uint32_t),
                     indices, GL_STATIC_DRAW);

        // 顶点属性 (与 InitLit2D / VS_LIT2D 严格一致)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, r));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, nx));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex2DLit),
                              (void*)offsetof(RenderVertex2DLit, tx));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        m.indexCount = iCount;
        uint32_t id = nextLitMeshId++;
        litMeshes[id] = m;
        return id;
    }

    void DeleteLit2DMesh(uint32_t meshId) override {
        auto it = litMeshes.find(meshId);
        if (it == litMeshes.end()) return;
        const MeshGPU& m = it->second;
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        litMeshes.erase(it);
    }

    const char* GetName() const override { return "GL33Core"; }

    // ---- 帧控制 ----
    void BeginFrame(float cr, float cg, float cb, float ca) override {
        glClearColor(cr, cg, cb, ca);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program);
        glBindVertexArray(vao);
    }
    void EndFrame() override {
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // Phase F.0.11 — 同步读 default fb 到 RGBA8 (用于 Screenshot / RecordPNGSequence)
    //   注意: PACK_ALIGNMENT 默认 4, 对 RGBA 来说每像素 4 字节天然对齐, 但
    //   region 起点不是 4 对齐时仍需 GL_PACK_ALIGNMENT=1 保险.
    //
    //   ⚠ 关键: 必须先 fully drain 之前帧累积的 GL error (HDR / TAA / SSR 等渲染期可能
    //   产生不影响功能的 driver-specific error, 不 drain 会误报本次 readback 失败).
    bool ReadbackDefaultFB(int x, int y, int w, int h, unsigned char* out_rgba) override {
        if (w <= 0 || h <= 0 || !out_rgba) return false;
        // Step 1: drain pre-existing errors (不属于本次 readback)
        while (glGetError() != GL_NO_ERROR) {}
        // Step 2: setup state + readback (取消 PBO 绑定, 走标准 client memory 路径)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
        // Step 3: 仅检查 readback 自身 error
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            while (glGetError() != GL_NO_ERROR) {}
            return false;
        }
        return true;
    }

    // ============================================================================
    // Phase F.0.11.2 — PBO 异步 readback (ping-pong)
    //
    // 关键 GL 习语:
    //   1) 绑 GL_PIXEL_PACK_BUFFER + glReadPixels (ptr=0): GPU→PBO 异步 DMA 启动, 不 stall
    //   2) glMapBufferRange(PBO[N-1], MAP_READ_BIT): 取上一帧数据 + memcpy + Unmap;
    //      GLES3.0+ 与 GL3.0+ 都支持 (glGetBufferSubData 不在 GLES 中, 不能用)
    //   3) PBO 大小固定后复用; 仅尺寸变更时重建
    //
    // 第一次调用: PBO[0] 启动, PBO[1] 无数据 → 返 false (out_rgba 不写)
    // 第二次调用: PBO[1] 启动, 取 PBO[0] → 返 true, 数据是上 1 帧的
    // 第三次调用: PBO[0] 启动, 取 PBO[1] → 返 true, 数据是上 1 帧的
    // ============================================================================
    bool ReadbackDefaultFBAsync(int x, int y, int w, int h, unsigned char* out_rgba) override {
        if (w <= 0 || h <= 0 || !out_rgba) return false;
        // Step 0: drain stale errors (同 sync 版)
        while (glGetError() != GL_NO_ERROR) {}

        // Step 1: 尺寸变更 → 全部重建 (lazy 分配 + 自适应窗口 resize)
        if (w != m_pbo_w || h != m_pbo_h) {
            ReadbackAsyncShutdown();   // 释放旧 PBO + 重置 pending
            for (int i = 0; i < 2; ++i) {
                glGenBuffers(1, &m_pbo[i]);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
                // GL_STREAM_READ: GPU 写一次, CPU 读一次, hint driver 用 mappable 内存
                glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, nullptr, GL_STREAM_READ);
            }
            m_pbo_w   = w;
            m_pbo_h   = h;
            m_pbo_idx = 0;
        }

        // Step 2: 启动新 readback 到 m_pbo[m_pbo_idx]
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_pbo_idx]);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // PBO 绑定时, 第 7 参数 (data) 是 PBO 内偏移, 这里 0 = 起始
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)0);
        m_pbo_pending[m_pbo_idx] = true;

        // Step 3: 取上一帧 PBO 数据 (若有)
        int prev_idx = 1 - m_pbo_idx;
        bool got = false;
        if (m_pbo_pending[prev_idx]) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[prev_idx]);
            // glMapBufferRange + memcpy + glUnmapBuffer (跳 GLES 不支持的 glGetBufferSubData)
            const GLsizeiptr bytes = (GLsizeiptr)w * h * 4;
            void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
            if (mapped) {
                std::memcpy(out_rgba, mapped, (size_t)bytes);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                m_pbo_pending[prev_idx] = false;
                got = true;
            }
        }

        // Step 4: cleanup + flip ping-pong index
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pbo_idx = prev_idx;   // 下次用现在的 prev (即下一帧再启动 readback)

        // Step 5: 检查 GL error (失败仍返已取到的数据 / false)
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            while (glGetError() != GL_NO_ERROR) {}
            // 不 fail-fast: 已取到 out_rgba 数据时仍返 true (best-effort)
        }
        return got;
    }

    void ReadbackAsyncShutdown() override {
        for (int i = 0; i < 2; ++i) {
            if (m_pbo[i]) {
                glDeleteBuffers(1, &m_pbo[i]);
                m_pbo[i] = 0;
            }
            m_pbo_pending[i] = false;
        }
        m_pbo_w   = 0;
        m_pbo_h   = 0;
        m_pbo_idx = 0;
    }

    // Phase F.0.11.4 — 读 RGBA16F/RGBA32F 纹理到 RGBA32F CPU buf (HDR 截图)
    //   策略: 创建临时 FBO 绑 tex 为 COLOR_ATTACHMENT0, glReadPixels(GL_RGBA, GL_FLOAT)
    //   兼容: GL3.3 + GLES3 (glGetTexImage 不可用), 临时 FBO 是 safe path
    bool ReadbackTextureRGBAFloat(uint32_t tex, int w, int h, float* out) override {
        if (!tex || !out || w <= 0 || h <= 0) return false;
        while (glGetError() != GL_NO_ERROR) {}

        // 保存当前 FBO 绑定 (避免破坏调用方状态)
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

        GLuint tmpFbo = 0;
        glGenFramebuffers(1, &tmpFbo);
        if (!tmpFbo) return false;
        glBindFramebuffer(GL_FRAMEBUFFER, tmpFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, (GLuint)tex, 0);

        // 检查 FBO 完整性 (RGBA16F texture 应该可作 color attachment)
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
            glDeleteFramebuffers(1, &tmpFbo);
            return false;
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);    // 关 PBO (走 client memory 路径)
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, out);
        GLenum err = glGetError();

        // 清理: detach + delete + restore prev FBO
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
        glDeleteFramebuffers(1, &tmpFbo);

        if (err != GL_NO_ERROR) {
            while (glGetError() != GL_NO_ERROR) {}
            return false;
        }
        return true;
    }

    // Phase F.0.11.3 — flush 最后一帧 pending PBO (StopRecord 时调一次, 防丢帧)
    //   pending PBO 在 m_pbo_idx (即下次将启动 readback 的位置, 对应上次启动还没取的 PBO).
    //   注: glMapBufferRange 会 sync block 等 GPU 完成 (因为是同一帧的数据),
    //       但仅 1 次, 不影响异步主路径.
    bool ReadbackAsyncFlushLast(unsigned char* out_rgba) override {
        if (!out_rgba || m_pbo_w <= 0 || m_pbo_h <= 0) return false;
        // 找 pending=true 的 PBO (理论上最多 1 个, 因为 ping-pong 总是取上一次留下一次)
        int waiting = -1;
        for (int i = 0; i < 2; ++i) if (m_pbo_pending[i]) { waiting = i; break; }
        if (waiting < 0) return false;

        while (glGetError() != GL_NO_ERROR) {}
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[waiting]);
        // glMapBufferRange + memcpy + glUnmapBuffer (跳 GLES 不支持的 glGetBufferSubData)
        const GLsizeiptr bytes = (GLsizeiptr)m_pbo_w * m_pbo_h * 4;
        bool got = false;
        void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (mapped) {
            std::memcpy(out_rgba, mapped, (size_t)bytes);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            got = true;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pbo_pending[waiting] = false;

        // 容错: glGetError 失败仍返 got (out_rgba 已填, best-effort)
        if (glGetError() != GL_NO_ERROR) {
            while (glGetError() != GL_NO_ERROR) {}
        }
        return got;
    }

    // ---- 状态 ----
    void SetColor(float r, float g, float b, float a) override {
        curColor[0] = r; curColor[1] = g; curColor[2] = b; curColor[3] = a;
    }
    void GetColor(float* r, float* g, float* b, float* a) override {
        *r = curColor[0]; *g = curColor[1]; *b = curColor[2]; *a = curColor[3];
    }
    void SetViewport(int x, int y, int w, int h) override {
        glViewport(x, y, w, h);
    }
    // Phase F.0.10.2 — 查询 OpenGL 当前 viewport (直接 glGetIntegerv)
    void GetViewport(int* x, int* y, int* w, int* h) override {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        if (x) *x = vp[0]; if (y) *y = vp[1]; if (w) *w = vp[2]; if (h) *h = vp[3];
    }

    // ---- 变换栈 ----
    void PushMatrix() override {
        matStack.push_back(modelview);
    }
    void PopMatrix() override {
        if (!matStack.empty()) {
            modelview = matStack.back();
            matStack.pop_back();
        }
    }
    void Translate(float x, float y, float z) override {
        modelview = modelview * Mat4::Translate(x, y, z);
    }
    void Rotate(float angle, float ax, float ay, float az) override {
        modelview = modelview * Mat4::Rotate(angle, ax, ay, az);
    }
    void Scale(float sx, float sy, float sz) override {
        modelview = modelview * Mat4::Scale(sx, sy, sz);
    }
    void LoadOrtho(float l, float r, float b, float t, float n, float f) override {
        projection = Mat4::Ortho(l, r, b, t, n, f);
        modelview = Mat4::Identity();
    }

    // ---- Phase A5: 索引绘制 (BatchRenderer 走此路径) ----
    void DrawIndexed(const RenderVertex* verts, int vertexCount,
                     const uint16_t* indices, int indexCount,
                     uint32_t textureId) override {
        if (!verts || !indices || vertexCount <= 0 || indexCount <= 0) return;

        // 更新 MVP + 纹理 uniform
        FlushMVP();
        if (textureId) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, (GLuint)textureId);
            glUniform1i(locUseTexture, 1);
            boundTex = (GLuint)textureId;
        } else {
            glUniform1i(locUseTexture, 0);
        }

        // 上传顶点数据
        EnsureVBOCapacity(vertexCount);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(RenderVertex), verts);

        // 上传索引数据 (EBO 已在 VAO 中绑定)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        if (indexCount > eboCapacity) {
            // 扩容 (按 2 倍增长)
            int newCap = eboCapacity ? eboCapacity : 1024;
            while (newCap < indexCount) newCap *= 2;
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, newCap * sizeof(uint16_t),
                         nullptr, GL_DYNAMIC_DRAW);
            eboCapacity = newCap;
        }
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexCount * sizeof(uint16_t), indices);

        // 一次性 indexed draw
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void*)0);

        if (textureId) {
            glBindTexture(GL_TEXTURE_2D, 0);
            boundTex = 0;
        }
    }

    // ---- 绘制 ----
    void DrawArrays(DrawMode mode, const RenderVertex* verts, int count) override {
        if (count <= 0) return;

        // 更新 MVP
        FlushMVP();

        // 纹理状态
        glUniform1i(locUseTexture, boundTex ? 1 : 0);

        // 上传顶点数据
        EnsureVBOCapacity(count);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(RenderVertex), verts);

        // GL 3.3 Core 没有 GL_QUADS, 需要拆分
        if (mode == DrawMode::Quads) {
            // 每 4 个顶点拆为 2 个三角形 (0,1,2 + 0,2,3)
            int quadCount = count / 4;
            int triVertCount = quadCount * 6;

            // 构建索引或直接生成三角形顶点
            // 使用临时缓冲拆分 (性能足够, 2D 场景顶点少)
            std::vector<RenderVertex> triVerts;
            triVerts.reserve(triVertCount);
            for (int i = 0; i < quadCount; ++i) {
                int base = i * 4;
                triVerts.push_back(verts[base + 0]);
                triVerts.push_back(verts[base + 1]);
                triVerts.push_back(verts[base + 2]);
                triVerts.push_back(verts[base + 0]);
                triVerts.push_back(verts[base + 2]);
                triVerts.push_back(verts[base + 3]);
            }
            EnsureVBOCapacity(triVertCount);
            glBufferSubData(GL_ARRAY_BUFFER, 0, triVertCount * sizeof(RenderVertex), triVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, triVertCount);
        } else {
            GLenum glMode;
            switch (mode) {
                case DrawMode::Lines:       glMode = GL_LINES; break;
                case DrawMode::LineLoop:    glMode = GL_LINE_LOOP; break;
                case DrawMode::LineStrip:   glMode = GL_LINE_STRIP; break;
                case DrawMode::Triangles:   glMode = GL_TRIANGLES; break;
                case DrawMode::TriangleFan: glMode = GL_TRIANGLE_FAN; break;
                default:                    glMode = GL_TRIANGLES; break;
            }
            glDrawArrays(glMode, 0, count);
        }
    }

    // ---- 纹理 ----
    uint32_t CreateTexture(int w, int h, int channels, const void* pixels) override {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // GL 3.3 Core: 内部格式必须使用 sized format
        GLenum intFmt, fmt;
        if (channels == 4) { intFmt = GL_RGBA8; fmt = GL_RGBA; }
        else if (channels == 3) { intFmt = GL_RGB8; fmt = GL_RGB; }
        else { intFmt = GL_R8; fmt = GL_RED; }
        glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        // 单通道纹理: 映射 RED → ALPHA, RGB=1 (兼容旧 GL_ALPHA 行为, 用于字体图集)
        if (channels == 1) {
#if defined(__EMSCRIPTEN__)
            // WebGL2 不支持 texture swizzle, 展开 R8 → RGBA (白底 + alpha)
            if (pixels) {
                std::vector<uint8_t> rgba(w * h * 4);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int i = 0; i < w * h; ++i) {
                    rgba[i*4+0] = 255; rgba[i*4+1] = 255;
                    rgba[i*4+2] = 255; rgba[i*4+3] = src[i];
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            return tex;
#elif defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
            // GLES3: 单独设置 swizzle (GL_TEXTURE_SWIZZLE_RGBA 不在 GLES 3.0 规范中)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
#else
            GLint swizzle[] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
#endif
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }
    void DeleteTexture(uint32_t texId) override {
        GLuint t = texId;
        glDeleteTextures(1, &t);
    }
    // Phase G.1.5 收尾 T2 — 为已上传的 2D 纹理生成 mipmap 链.
    // 调 glGenerateMipmap 后, MIN_FILTER 切到 LINEAR_MIPMAP_LINEAR (三线性过滤);
    // MAG_FILTER 保持 LINEAR (mipmap 不影响放大采样).
    void GenerateMipmap2D(uint32_t texId) override {
        if (!texId) return;
        glBindTexture(GL_TEXTURE_2D, (GLuint)texId);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    // Phase G.1.5 T3 — 设置 2D 纹理采样器参数 (透传 cgltf sampler).
    // 任一参数为 0 表示跳过 (保持现有值), 否则用 raw GL enum 直接 glTexParameteri.
    void SetTexture2DSampler(uint32_t texId, int magFilter, int minFilter,
                             int wrapS, int wrapT) override {
        if (!texId) return;
        glBindTexture(GL_TEXTURE_2D, (GLuint)texId);
        if (magFilter) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        if (minFilter) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        if (wrapS)     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     wrapS);
        if (wrapT)     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     wrapT);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void BindTexture(uint32_t texId) override {
        boundTex = texId;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);
    }
    void UnbindTexture() override {
        boundTex = 0;
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void UpdateTexture(uint32_t texId, int x, int y, int w, int h,
                       int channels, const void* pixels) override {
        glBindTexture(GL_TEXTURE_2D, texId);
#if defined(__EMSCRIPTEN__)
        if (channels == 1 && pixels) {
            // 内部纹理已为 RGBA8, 展开后上传
            std::vector<uint8_t> rgba(w * h * 4);
            const uint8_t* src = (const uint8_t*)pixels;
            for (int i = 0; i < w * h; ++i) {
                rgba[i*4+0] = 255; rgba[i*4+1] = 255;
                rgba[i*4+2] = 255; rgba[i*4+3] = src[i];
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else {
            GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
        }
#else
        GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
#endif
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void ReplaceTexture(uint32_t texId, int w, int h, int channels, const void* pixels) override {
        glBindTexture(GL_TEXTURE_2D, texId);
#if defined(__EMSCRIPTEN__)
        if (channels == 1) {
            if (pixels) {
                std::vector<uint8_t> rgba(w * h * 4);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int i = 0; i < w * h; ++i) {
                    rgba[i*4+0] = 255; rgba[i*4+1] = 255;
                    rgba[i*4+2] = 255; rgba[i*4+3] = src[i];
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
        } else {
            GLenum intFmt = (channels == 4) ? GL_RGBA8 : GL_RGB8;
            GLenum fmt    = (channels == 4) ? GL_RGBA  : GL_RGB;
            glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        }
#else
        GLenum intFmt, fmt;
        if (channels == 4) { intFmt = GL_RGBA8; fmt = GL_RGBA; }
        else if (channels == 3) { intFmt = GL_RGB8; fmt = GL_RGB; }
        else { intFmt = GL_R8; fmt = GL_RED; }
        glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
#endif
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ---- FBO ----
    uint32_t CreateFBO(int w, int h, uint32_t* outTex, uint32_t* outDepthRB) override {
        GLuint tex = CreateTexture(w, h, 4, nullptr);
        GLuint depthRB = 0;
        glGenRenderbuffers(1, &depthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_ERROR, "GL33: FBO incomplete (status=0x%X)", status);
            glDeleteFramebuffers(1, &fbo);
            GLuint t = tex; glDeleteTextures(1, &t);
            glDeleteRenderbuffers(1, &depthRB);
            return 0;
        }
        *outTex = tex;
        *outDepthRB = depthRB;
        return fbo;
    }
    void DeleteFBO(uint32_t fbo, uint32_t tex, uint32_t depthRB) override {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) { GLuint t = tex; glDeleteTextures(1, &t); }
        if (depthRB) glDeleteRenderbuffers(1, &depthRB);
    }
    void BindFBO(uint32_t fbo) override {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    void UnbindFBO() override {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ---- 裁剪 ----
    void SetScissor(bool enable, int x, int y, int w, int h) override {
        if (enable) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, w, h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }

    // ---- 用户 Shader 支持 ----
    bool SupportsShaders() const override { return true; }

    uint32_t CreateShader(const char* vertexSrc, const char* fragmentSrc,
                          char* errLog, int errLogSize) override {
        if (!vertexSrc || !fragmentSrc) return 0;

        auto tryCompile = [&](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                if (errLog && errLogSize > 0) {
                    glGetShaderInfoLog(s, errLogSize - 1, nullptr, errLog);
                    errLog[errLogSize - 1] = '\0';
                }
                glDeleteShader(s);
                return 0;
            }
            return s;
        };

        GLuint vs = tryCompile(GL_VERTEX_SHADER, vertexSrc);
        if (!vs) return 0;
        GLuint fs = tryCompile(GL_FRAGMENT_SHADER, fragmentSrc);
        if (!fs) { glDeleteShader(vs); return 0; }

        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        // 绑定默认属性位置, 让用户 shader 复用引擎的 VAO
        glBindAttribLocation(p, 0, "aPos");
        glBindAttribLocation(p, 1, "aTexCoord");
        glBindAttribLocation(p, 2, "aColor");
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!ok) {
            if (errLog && errLogSize > 0) {
                glGetProgramInfoLog(p, errLogSize - 1, nullptr, errLog);
                errLog[errLogSize - 1] = '\0';
            }
            glDeleteProgram(p);
            return 0;
        }
        return (uint32_t)p;
    }

    void DeleteShader(uint32_t shaderId) override {
        if (shaderId) glDeleteProgram((GLuint)shaderId);
    }

    bool UseShader(uint32_t shaderId) override {
        if (!shaderId) return false;
        glUseProgram((GLuint)shaderId);
        userShaderActive = true;  // Phase AS.2 — 标记用户 shader 激活
        // 自动上传 MVP 到约定名 uMVP (若存在)
        GLint locMVPUser = glGetUniformLocation((GLuint)shaderId, "uMVP");
        if (locMVPUser >= 0) {
            Mat4 mvp = projection * modelview;
            glUniformMatrix4fv(locMVPUser, 1, GL_FALSE, mvp.m);
        }
        return true;
    }

    void UseDefaultShader() override {
        glUseProgram(program);
        userShaderActive = false;  // Phase AS.2 — 复位标志
        FlushMVP();
    }

    int GetUniformLocation(uint32_t shaderId, const char* name) override {
        if (!shaderId || !name) return -1;
        return glGetUniformLocation((GLuint)shaderId, name);
    }

    void SetUniform1f(int loc, float v) override {
        if (loc >= 0) glUniform1f(loc, v);
    }
    void SetUniform2f(int loc, float x, float y) override {
        if (loc >= 0) glUniform2f(loc, x, y);
    }
    void SetUniform3f(int loc, float x, float y, float z) override {
        if (loc >= 0) glUniform3f(loc, x, y, z);
    }
    void SetUniform4f(int loc, float x, float y, float z, float w) override {
        if (loc >= 0) glUniform4f(loc, x, y, z, w);
    }
    void SetUniform1i(int loc, int v) override {
        if (loc >= 0) glUniform1i(loc, v);
    }
    void SetUniformMat4(int loc, const float* m) override {
        if (loc >= 0 && m) glUniformMatrix4fv(loc, 1, GL_FALSE, m);
    }

    // ---- Phase AS.1 新增 uniform setter ----
    void SetUniformMat3(int loc, const float* m) override {
        if (loc >= 0 && m) glUniformMatrix3fv(loc, 1, GL_FALSE, m);
    }
    void SetUniform2i(int loc, int x, int y) override {
        if (loc >= 0) glUniform2i(loc, x, y);
    }
    void SetUniform3i(int loc, int x, int y, int z) override {
        if (loc >= 0) glUniform3i(loc, x, y, z);
    }
    void SetUniform4i(int loc, int x, int y, int z, int w) override {
        if (loc >= 0) glUniform4i(loc, x, y, z, w);
    }
    void SetUniform1fv(int loc, int count, const float* v) override {
        if (loc >= 0 && v && count > 0) glUniform1fv(loc, count, v);
    }
    void SetUniform2fv(int loc, int count, const float* v) override {
        if (loc >= 0 && v && count > 0) glUniform2fv(loc, count, v);
    }
    void SetUniformSampler(int loc, int slot, uint32_t texId) override {
        if (loc < 0 || !texId) return;
        // 限制 slot 在合理范围 (大多数 GPU 至少支持 16 个 texture unit)
        if (slot < 0) slot = 0;
        if (slot > 15) slot = 15;
        glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texId);
        glUniform1i(loc, slot);
        // 恢复活动 texture unit 到 slot 0, 与引擎默认绘制一致
        glActiveTexture(GL_TEXTURE0);
    }
    void GenerateMipmap(uint32_t texId) override {
        if (!texId) return;
        glBindTexture(GL_TEXTURE_2D, (GLuint)texId);
        glGenerateMipmap(GL_TEXTURE_2D);
        // mipmap 生成后, 需要让 min filter 支持 mipmap 才有效
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void ClearCurrent(float r, float g, float b, float a) override {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void ResetVelocityHistory() override {
        prevViewProj = ComputeViewProj3D();
        hasPrevViewProjForVelocity = false;
        hasNextPrevModel = false;
    }

    void CommitVelocityHistory() override {
        prevViewProj = ComputeViewProj3D();
        hasPrevViewProjForVelocity = true;
    }

    // ---- Phase E.14 — velocity dilation 开关 + 编码 scale + 活跃 format ----
    void  SetVelocityDilation(bool enabled) override { velocityDilation_ = enabled; }
    bool  GetVelocityDilation() const override { return velocityDilation_; }
    float GetVelocityScale() const override { return kVelocityScaleDefault; }
    VelocityFormat GetActiveVelocityFormat() const override { return activeVelocityFormat_; }

    // ---- Phase F.1.1 — Mipmap LOD bias ----
    //   clamp [-4, +4] (业界经验; 默认正常工作范围 [-1.7, 0])
    //   3D mesh shader (FS_SOURCE / FS_UNLIT / FS_PBR / FS_LIT2D) 内 `texture(s, uv, uMipBias)` 用此值
    void SetMipBias(float bias) override {
        if (bias < -4.0f) bias = -4.0f;
        else if (bias > 4.0f) bias = 4.0f;
        mipBias_ = bias;
    }
    float GetMipBias() const override { return mipBias_; }

    // ==================== Phase E.15 — Motion Blur 虚接口实现 ====================

    bool SupportsMotionBlur() const override { return motionBlurSupported; }

    /// 创建 motion blur ping-pong RT (RGBA16F, color-only, 无 depth)
    /// @param  outTex             返回 GL tex id (失败为 0)
    /// @param  storageW, storageH Phase E.17 — RT 实际分配尺寸 (0 = 沿用 w/h, full-res)
    /// @return GL fbo id (失败为 0)
    uint32_t CreateMotionBlurRT(int w, int h, uint32_t* outTex,
                                 int storageW, int storageH) override {
        if (outTex) *outTex = 0;
        if (!motionBlurSupported || w <= 0 || h <= 0 || !outTex) return 0;

        // ★ Phase E.17: storageW/H==0 → fallback w/h (向后兼容全分辨率)
        const int sw = (storageW > 0) ? storageW : w;
        const int sh = (storageH > 0) ? storageH : h;

        GLuint fbo = 0, tex = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &tex);
        if (!fbo || !tex) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (tex) glDeleteTextures(1, &tex);
            return 0;
        }

        glBindTexture(GL_TEXTURE_2D, tex);
        // ★ Phase E.17: 使用 sw/sh 实际分配 (half-res 下 = (w+1)/2, (h+1)/2)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, sw, sh, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            // Phase E.17: 记录 逻辑尺寸 与 实际存储尺寸（half-res 调试友好）
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.15 Motion Blur FBO incomplete (status=0x%X), logical=%dx%d storage=%dx%d",
                    status, w, h, sw, sh);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return 0;
        }

        *outTex = (uint32_t)tex;
        return (uint32_t)fbo;
    }

    void DeleteMotionBlurRT(uint32_t fbo, uint32_t tex) override {
        if (fbo) { GLuint f = (GLuint)fbo; glDeleteFramebuffers(1, &f); }
        if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t);     }
    }

    /// Motion Blur 完整 2-pass:
    ///   Pass1: bind motionBlurFbo → 沿 velocity 多采样 sceneTex → 写 motionBlurTex
    ///   Pass2: glBlitFramebuffer(motionBlurFbo → dstFbo, COLOR_BUFFER) 覆盖 sceneTex
    /// Phase E.17: rtW/rtH 为 motionBlurTex 实际尺寸，half-res 下 < w/h → 自动 GL_LINEAR 上采样
    void DrawMotionBlur(uint32_t sceneTex, uint32_t velocityTex,
                        uint32_t cameraVelocityTex,             // ★ Phase E.16
                        uint32_t motionBlurFbo, uint32_t motionBlurTex,
                        uint32_t dstFbo,
                        int w, int h,
                        float strength, int sampleCount,
                        int mode,                               // ★ Phase E.16
                        int rtW, int rtH,                       // ★ Phase E.17
                        int rgnX, int rgnY,                     // ★ Phase F.0.10.3
                        int rgnW, int rgnH) override {          // ★ Phase F.0.10.3
        if (!motionBlurSupported || !programMotionBlur) return;
        if (!sceneTex || !velocityTex || !motionBlurFbo || !motionBlurTex || !dstFbo) return;
        if (w <= 0 || h <= 0) return;

        // ★ Phase E.17: rtW/H==0 → fallback w/h (full-res, 与 Phase E.16 等价)
        const int passW = (rtW > 0) ? rtW : w;
        const int passH = (rtH > 0) ? rtH : h;

        // ★ Phase F.0.10.3 — region 化判定: rgnW>0 && rgnH>0 时启 scissor (Pass1) + sub-rect blit (Pass2)
        // half-res storage (passW < w) 时, Pass1 写 storage 空间, region 也需缩半;
        // Pass2 是 storage→full 的 blit, dst 用 full-res region 即可 (storageRgn 是 src rect)
        const bool useRegion = (rgnW > 0 && rgnH > 0);
        // Pass1 storage 空间 region: half-res 时缩半
        int sRgnX = rgnX, sRgnY = rgnY, sRgnW = rgnW, sRgnH = rgnH;
        if (useRegion && (passW < w || passH < h)) {
            // half-res: storage region 是 dst region 的 1/2 (与 mip 缩半同模式)
            sRgnX = rgnX / 2;
            sRgnY = rgnY / 2;
            sRgnW = (rgnW > 1) ? (rgnW / 2) : 1;
            sRgnH = (rgnH > 1) ? (rgnH / 2) : 1;
        }

        // Phase E.16 — mode=1/2 但 cameraVelocityTex 缺失 → silent fallback combined (mode=0)
        // 不打 warning log 避免每帧刷屏
        int safeMode = mode;
        if ((mode == 1 || mode == 2) && !cameraVelocityTex) {
            safeMode = 0;
        }
        // slot 2 占位策略：mode=0 也需绑有效 sampler，避免 driver invalid binding
        uint32_t boundCameraTex = cameraVelocityTex ? cameraVelocityTex : velocityTex;

        // ===== Pass1: 全屏 shader, 写 motionBlurFbo =====
        // ★ Phase E.17: viewport 用实际 RT 尺寸 (passW, passH)。
        //               关键：uTexel 仍按全分辨率 1/vec2(w, h) 上传 (line 下方)，
        //               原因：velocityTex/cameraVelocityTex 始终是全分辨率，
        //                    9-tap dilation 邻域物理覆盖必须一致；
        //                    vUV (0..1) 与 viewport 无关，bilinear filter 自动下采样。
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)motionBlurFbo);
        glViewport(0, 0, passW, passH);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // ★ Phase F.0.10.3 — scissor 限定 Pass1 写 storage 空间 region (split-screen 必备)
        if (useRegion) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(sRgnX, sRgnY, sRgnW, sRgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programMotionBlur);
        // 上传 uniform (clamp 调用方已做, 此处再补 sanity)
        if (locMB_Texel       >= 0) glUniform2f(locMB_Texel, 1.0f / (float)w, 1.0f / (float)h);
        if (locMB_Strength    >= 0) glUniform1f(locMB_Strength, strength);
        if (locMB_SampleCount >= 0) glUniform1i(locMB_SampleCount, sampleCount);
        // Phase E.14 联动: 实时取 backend 的 dilation/format/scale
        // Phase E.18: 若 dilation pass 已执行 (dilationPassActive_=true), motion blur shader 单点采 dilatedTex,
        //             强制 uVelocityDilation=0 (跳过 shader 内 inline 9-tap);
        //             否则沿用 velocityDilation_ 旧逻辑
        const int mbUVDValue = dilationPassActive_ ? 0 : (velocityDilation_ ? 1 : 0);
        if (locMB_VelocityDilation >= 0) glUniform1i(locMB_VelocityDilation, mbUVDValue);
        if (locMB_VelocityFormat   >= 0) glUniform1i(locMB_VelocityFormat,
                                                    (activeVelocityFormat_ == VelocityFormat::RG8) ? 1 : 0);
        if (locMB_VelocityScale    >= 0) glUniform1f(locMB_VelocityScale, kVelocityScaleDefault);
        if (locMB_Mode             >= 0) glUniform1i(locMB_Mode, safeMode);  // ★ Phase E.16

        // 绑 sampler unit (slot 0 = scene, slot 1 = velocity, slot 2 = cameraVelocity Phase E.16)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)sceneTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)velocityTex);
        glActiveTexture(GL_TEXTURE2);                            // ★ Phase E.16
        glBindTexture(GL_TEXTURE_2D, (GLuint)boundCameraTex);    // 占位: mode=0 也绑有效 tex

        // 全屏三角 (用 vaoTonemap, 6 顶点 = 2 三角形)
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // 状态复位
        glActiveTexture(GL_TEXTURE2);                            // ★ Phase E.16
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        // ★ Phase F.0.10.3 — Pass1 完成, 复位 scissor (Pass2 blit 不依赖 scissor)
        // 注: glBlitFramebuffer 不受 GL_SCISSOR_TEST 影响 (glScissor 仅作用于 raster);
        //     因此 Pass2 用 src/dst rect 显式控制 sub-rect, 无需 enable scissor
        glDisable(GL_SCISSOR_TEST);

        // ===== Pass2: blit motionBlurFbo → dstFbo (覆盖 sceneTex) =====
        // ★ Phase E.17: src=(passW, passH) → dst=(w, h)。
        //               passW/H < w/h → 自动选 GL_LINEAR 硬件 bilinear 上采样;
        //               同尺寸 → GL_NEAREST (与 Phase E.16 等价，零回归)
        // ★ Phase F.0.10.3: useRegion 时 src=(sRgn) → dst=(rgn) 仅 blit 子矩形
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)motionBlurFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dstFbo);
        const GLenum blitFilter = (passW == w && passH == h) ? GL_NEAREST : GL_LINEAR;
        if (useRegion) {
            // src rect (storage 空间) → dst rect (full 空间)
            glBlitFramebuffer(sRgnX, sRgnY, sRgnX + sRgnW, sRgnY + sRgnH,
                              rgnX,  rgnY,  rgnX  + rgnW,  rgnY  + rgnH,
                              GL_COLOR_BUFFER_BIT, blitFilter);
        } else {
            glBlitFramebuffer(0, 0, passW, passH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, blitFilter);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

    // ==================== Phase E.18 — Velocity Dilation Pass 虚接口实现 ====================

    bool SupportsVelocityDilation() const override { return velocityDilateSupported; }

    /// 创建 dilation pass ping-pong RT (RG16F, color-only, 无 depth)
    /// 与 motion blur RT 同模式; dilatedTex.rg = shader 内已 decode 的 float velocity
    /// Phase E.18.1: w/h = logical (full-res, 当前未使用), sw/sh = storage (实际 RT 尺寸)
    ///               halfRes=false 时 sw=w, sh=h; halfRes=true 时 sw=((w+1)/2), sh=((h+1)/2)
    uint32_t CreateVelocityDilateRT(int w, int h, int sw, int sh, uint32_t* outTex) override {
        (void)w; (void)h;   // logical 当前未用 (保留供未来 sanity check; 实际尺寸取自 sw/sh)
        if (outTex) *outTex = 0;
        if (!velocityDilateSupported || sw <= 0 || sh <= 0 || !outTex) return 0;

        GLuint fbo = 0, tex = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &tex);
        if (!fbo || !tex) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (tex) glDeleteTextures(1, &tex);
            return 0;
        }

        glBindTexture(GL_TEXTURE_2D, tex);
        // dilatedTex 永远 RG16F (无视 raw velocity format), shader 内 decode 后直存 float
        // Phase E.18.1: 实际存储尺寸 = sw × sh (half-res 时 1/4 像素 / 1/4 VRAM)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, sw, sh, 0, GL_RG, GL_FLOAT, nullptr);
        // bilinear: consumer 单点采 sub-pixel 时硬件 filter 平滑 (half-res 时承担上采角色);
        // CLAMP_TO_EDGE: 边界采样不引入 wrap artifact
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_WARN,
                    "GL33: Phase E.18 Velocity Dilate FBO incomplete (status=0x%X), %dx%d (logical %dx%d)",
                    status, sw, sh, w, h);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return 0;
        }

        *outTex = (uint32_t)tex;
        return (uint32_t)fbo;
    }

    void DeleteVelocityDilateRT(uint32_t fbo, uint32_t tex) override {
        if (fbo) { GLuint f = (GLuint)fbo; glDeleteFramebuffers(1, &f); }
        if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t);     }
    }

    /// 执行 dilation pass — 全屏 9-tap max-length 单 pass
    ///   shader 内据 backend velocityFormat 状态 decode raw velocity,
    ///   输出写入 dilatedTex (RG16F, 已 decode 的 float)
    /// Phase E.18.1: sw/sh = dilatedTex storage 尺寸 (= viewport, uTexel = 1/(sw,sh))
    ///               half-res 时邻域物理覆盖 = 6 raw 像素 (vs full-res 的 3 raw 像素), max-filter 更鲁棒
    void DrawVelocityDilate(uint32_t srcVelocityTex, uint32_t dstFbo, int sw, int sh) override {
        if (!velocityDilateSupported || !programVelocityDilate) return;
        if (!srcVelocityTex || !dstFbo || sw <= 0 || sh <= 0) return;

        // viewport 跟随 dilatedTex 实际尺寸 (full-res 或 half-res)
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, sw, sh);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glUseProgram(programVelocityDilate);
        // uTexel = 1.0 / vec2(sw, sh) — 半分辨率纹素间距; 9-tap 在 raw velocity space 物理覆盖 = 2(sw 步长)*3 = 6 raw 像素
        if (locVDilate_Texel          >= 0) glUniform2f(locVDilate_Texel, 1.0f / (float)sw, 1.0f / (float)sh);
        // 据 backend 当前 velocityFormat 状态告知 shader 如何 decode raw (RG16F 直存 / RG8 反编码)
        if (locVDilate_VelocityFormat >= 0) glUniform1i(locVDilate_VelocityFormat,
                                                       (activeVelocityFormat_ == VelocityFormat::RG8) ? 1 : 0);
        if (locVDilate_VelocityScale  >= 0) glUniform1f(locVDilate_VelocityScale, kVelocityScaleDefault);

        // 绑 src raw velocity 到 slot 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcVelocityTex);

        // 全屏三角 (复用 vaoTonemap, 6 顶点 = 2 三角形)
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // 状态复位
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// dilation pass 当前帧激活态 — HDRRenderer EndScene 每帧调用
    /// true  → DrawSSRTemporal / DrawMotionBlur 内强制 uVelocityDilation=0 (consumer 单点采 dilatedTex)
    /// false → 沿用 velocityDilation_ 旧逻辑 (consumer shader 内 inline 9-tap)
    void SetDilationPassActive(bool active) override { dilationPassActive_ = active; }
    bool GetDilationPassActive() const override     { return dilationPassActive_; }

    // ==================== Phase F.0 — TAA Master Pipeline 虚接口实现 ====================

    bool SupportsTAA() const override { return taaSupported; }

    /// 创建 TAA history ping-pong RT (RGBA16F × 2, color-only, 无 depth)
    /// 与 sceneTex 同尺寸; 失败时全清零返 false (TAARenderer Enable 检测会失败)
    bool CreateTAAHistoryRT(int w, int h, uint32_t* outFbos, uint32_t* outTexs) override {
        if (outFbos) outFbos[0] = outFbos[1] = 0;
        if (outTexs) outTexs[0] = outTexs[1] = 0;
        if (!taaSupported || w <= 0 || h <= 0 || !outFbos || !outTexs) return false;

        for (int i = 0; i < 2; ++i) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            if (!tex) { DeleteTAAHistoryRT(outFbos, outTexs); return false; }
            glBindTexture(GL_TEXTURE_2D, tex);
            // RGBA16F (与 HDR sceneTex 同格式), BILINEAR 让 reproject sub-pixel 采更平滑
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            if (!fbo) { glDeleteTextures(1, &tex); DeleteTAAHistoryRT(outFbos, outTexs); return false; }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &tex);
                DeleteTAAHistoryRT(outFbos, outTexs);
                return false;
            }
            outFbos[i] = fbo;
            outTexs[i] = tex;
        }
        return true;
    }

    /// 释放 TAA history RT (与 CreateTAAHistoryRT 配对)
    void DeleteTAAHistoryRT(uint32_t* fbos, uint32_t* texs) override {
        if (!fbos || !texs) return;
        for (int i = 0; i < 2; ++i) {
            if (fbos[i]) { GLuint id = fbos[i]; glDeleteFramebuffers(1, &id); fbos[i] = 0; }
            if (texs[i]) { GLuint id = texs[i]; glDeleteTextures(1, &id);     texs[i] = 0; }
        }
    }

    // Phase F.1 TAAU — output-res sceneTex (RGBA16F, linear filter, clamp-to-edge)
    //   用途: TAAU 启用时 sharpen/tonemap 的输入 (sceneTex 在 render-res 不可直接用)
    //   linear filter 是因 sharpen 4-tap unsharp 与 tonemap 都按 sub-pixel UV 采样
    bool CreateOutputSceneTex(int w, int h, uint32_t* outFbo, uint32_t* outTex) override {
        if (outFbo) *outFbo = 0;
        if (outTex) *outTex = 0;
        if (!taaSupported || w <= 0 || h <= 0 || !outFbo || !outTex) return false;

        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return false;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) { glDeleteTextures(1, &tex); return false; }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return false;
        }
        *outFbo = fbo;
        *outTex = tex;
        return true;
    }

    void DeleteOutputSceneTex(uint32_t fbo, uint32_t tex) override {
        if (fbo) { GLuint id = fbo; glDeleteFramebuffers(1, &id); }
        if (tex) { GLuint id = tex; glDeleteTextures(1, &id); }
    }

    /// 执行 TAA pass: reproject + neighborhood AABB clip + alpha blend
    /// Phase E.18 联动: dilationPassActive=true 时 shader 单点采 dilatedTex (绑给 velocityTex);
    ///                  否则 consumer 走 inline 9-tap (velocityTex=raw RG16F/RG8 encoded)
    void DrawTAAPass(uint32_t curHdrTex, uint32_t historyTex, uint32_t velocityTex, uint32_t dstFbo,
                     int w, int h,
                     float blendAlpha, int neighborhoodClip, int hasHistory,
                     bool velocityDilation, float velocityScale,
                     VelocityFormat velocityFormat,
                     int   antiFlicker,
                     int   clipMode,
                     float varianceGamma,
                     float motionGamma,                   // Phase F.0.8
                     int   motionAdaptiveGamma,           // Phase F.0.8
                     int rgnX, int rgnY,                  // Phase F.0.10.2: split-screen region
                     int rgnW, int rgnH,                  // Phase F.0.10.2: rgnW<=0 时全屏 (零回归)
                     const float* uvBounds,               // Phase F.0.10.5: vec4 邻域采样 clamp; nullptr = 全屏 no-op
                     int renderW, int renderH,            // Phase F.1 TAAU: render-res 尺寸 (0 = 同 w/h, F.0 行为)
                     int outputW, int outputH,            // Phase F.1 TAAU: output-res 尺寸 (0 = 同 w/h, F.0 行为)
                     int taauEnabled) override            // Phase F.1 TAAU: 0=F.0 路径, 1=TAAU 路径
    {
        if (!taaSupported || !programTAA) return;
        if (!curHdrTex || !dstFbo || w <= 0 || h <= 0) return;

        // Phase F.1 TAAU — 解析双尺寸 (默认 0 退化为 w/h, 与 F.0 行为完全一致)
        const int rW = (taauEnabled && renderW > 0) ? renderW : w;
        const int rH = (taauEnabled && renderH > 0) ? renderH : h;
        const int oW = (taauEnabled && outputW > 0) ? outputW : w;
        const int oH = (taauEnabled && outputH > 0) ? outputH : h;

        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, oW, oH);   // Phase F.1: viewport 用 output-res (F.0 模式 oW==w)
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.2: region 启用时 scissor 限制写入到子矩形, 让该 instance 只更新它的 region
        // 全屏路径 (rgnW/rgnH<=0) 仍走 glDisable, 与 F.0 ~ F.0.14 行为一致
        const bool useScissor = (rgnW > 0 && rgnH > 0);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programTAA);
        // Phase F.1 TAAU: uTexel 始终上传 1/(render-res), 让 cur 邻域 + velocity dilation 9-tap step
        //   在 render 像素空间 (与 curHdrTex/velocityTex 像素对齐). F.0 模式 rW/rH == w/h, 行为不变.
        if (locTAA_Texel            >= 0) glUniform2f(locTAA_Texel, 1.0f / (float)rW, 1.0f / (float)rH);
        if (locTAA_BlendAlpha       >= 0) glUniform1f(locTAA_BlendAlpha,       blendAlpha);
        if (locTAA_NeighborhoodClip >= 0) glUniform1i(locTAA_NeighborhoodClip, neighborhoodClip);
        if (locTAA_HasHistory       >= 0) glUniform1i(locTAA_HasHistory,       hasHistory);
        // Phase E.18 联动: dilation pass active 时强制单点 (consumer 单点读已 decode 的 dilatedTex)
        const int taaUVDValue = dilationPassActive_ ? 0 : (velocityDilation ? 1 : 0);
        if (locTAA_VelocityDilation >= 0) glUniform1i(locTAA_VelocityDilation, taaUVDValue);
        if (locTAA_VelocityFormat   >= 0) glUniform1i(locTAA_VelocityFormat,
                                                     (velocityFormat == VelocityFormat::RG8) ? 1 : 0);
        if (locTAA_VelocityScale    >= 0) glUniform1f(locTAA_VelocityScale,    velocityScale);
        // Phase F.0.4: Karis luma weighting blend 开关 (0=纯 alpha blend, 1=Karis)
        if (locTAA_AntiFlicker      >= 0) glUniform1i(locTAA_AntiFlicker,      antiFlicker);
        // Phase F.0.2: clip 色彩空间 (0=RGB AABB 与 F.0 一致, 1=YCoCg AABB 默认, 2=YCoCg variance F.0.3)
        if (locTAA_ClipMode         >= 0) glUniform1i(locTAA_ClipMode,         clipMode);
        // Phase F.0.3: variance clip 收紧系数 γ (仅 clipMode==2 生效，调用方已 clamp [0, 4])
        if (locTAA_VarianceGamma    >= 0) glUniform1f(locTAA_VarianceGamma,    varianceGamma);
        // Phase F.0.8: motion-adaptive γ — 高速区域 γ + 开关 (仅 motionAdaptive==1 && clipMode==2 生效)
        if (locTAA_MotionGamma         >= 0) glUniform1f(locTAA_MotionGamma,         motionGamma);
        if (locTAA_MotionAdaptiveGamma >= 0) glUniform1i(locTAA_MotionAdaptiveGamma, motionAdaptiveGamma);
        // Phase F.0.10.5: uvBounds 上传 (nullptr 时上传 0,0,1,1 即恒等 clamp, 零回归)
        if (locTAA_UvBounds >= 0) {
            const float defaultBounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glUniform4fv(locTAA_UvBounds, 1, uvBounds ? uvBounds : defaultBounds);
        }

        // 绑 sampler: slot 0=cur HDR, slot 1=history (空时给 cur 占位避免黑帧), slot 2=velocity
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)curHdrTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)(historyTex ? historyTex : curHdrTex));
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, (GLuint)velocityTex);

        // 全屏 quad: 复用 tonemap 的 VAO (vaoTonemap), VS 已编译为 VS_TONEMAP_SOURCE
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // 解绑 sampler (slot 2 → 0 复位至 slot 0, 避免后续 pass 误读)
        for (int s = 2; s >= 0; --s) {
            glActiveTexture(GL_TEXTURE0 + s);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glUseProgram(0);
        // Phase F.0.10.2: 退出前关 scissor, 避免影响后续 pass
        if (useScissor) glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Phase F.0.1 — TAA Sharpen pass: 4-tap unsharp mask, 替代 BlitTAAToHDR (in-place 写回 sceneTex)
    /// shader 编译失败时调用方应回落到 BlitTAAToHDR; sharpness <= 0 由调用方在 CPU 端跳过.
    void DrawTAASharpenPass(uint32_t srcTex, uint32_t dstFbo,
                            int w, int h, float sharpness,
                            int rgnX, int rgnY, int rgnW, int rgnH,
                            const float* uvBounds) override {   // Phase F.0.10.5
        if (!programSharpen || !srcTex || !dstFbo || w <= 0 || h <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.2: region 启用时 scissor 限制 sharpen 写回到子矩形 (history 仍按全屏采)
        const bool useScissor = (rgnW > 0 && rgnH > 0);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programSharpen);
        if (locSharpen_TexelSize >= 0) glUniform2f(locSharpen_TexelSize, 1.0f / (float)w, 1.0f / (float)h);
        if (locSharpen_Sharpness >= 0) glUniform1f(locSharpen_Sharpness, sharpness);
        // Phase F.0.10.5: uvBounds 上传 (nullptr 时上传 0,0,1,1, 与 F.0.1 老路径等价)
        if (locSharpen_UvBounds >= 0) {
            const float defaultBounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glUniform4fv(locSharpen_UvBounds, 1, uvBounds ? uvBounds : defaultBounds);
        }

        // 绑 src TAA 输出到 slot 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        // 全屏 quad: 复用 tonemap VAO
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // 状态复位
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        if (useScissor) glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.2: 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Phase F.0.9 — TAA Custom Upscale pass: Catmull-Rom 9-tap bicubic (Sigggraph 2018 Filmic SMAA)
    /// 仅 halfRes=true && sharpness=0 && upscaleMode==1 路径使用, 替代 BlitTAAToHDR 的 GL_LINEAR stretch.
    /// shader 编译失败时静默 no-op (TAARenderer 层 fallback 走 BlitTAAToHDR).
    /// @param srcTex     history half-res tex
    /// @param dstFbo     HDR FBO
    /// @param srcW, srcH src 分辨率 (history half-res, 如 W/2, H/2)
    /// @param dstW, dstH dst 分辨率 (sceneTex full-res)
    void DrawTAAUpscalePass(uint32_t srcTex, uint32_t dstFbo,
                            int srcW, int srcH,
                            int dstW, int dstH,
                            int rgnX, int rgnY, int rgnW, int rgnH) override {   // Phase F.0.10.2
        if (!programBicubicUpscale || !srcTex || !dstFbo || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.2: region 启用时 scissor 限制 upscale 写回到子矩形
        const bool useScissor = (rgnW > 0 && rgnH > 0);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programBicubicUpscale);
        // uTexel = 1 / src 分辨率 (Catmull-Rom 在 src 纹素空间采样)
        if (locBicubic_Texel >= 0) glUniform2f(locBicubic_Texel, 1.0f / (float)srcW, 1.0f / (float)srcH);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        if (useScissor) glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.2: 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Phase F.0.14 — TAA Lanczos-2 25-tap 5x5 上采样 pass (高画质替代 F.0.9 Catmull-Rom)
    /// 与 F.0.9 DrawTAAUpscalePass 同接口签名, 仅 program/uniform 不同; 仅 upscaleMode==2 路径调用
    /// shader 编译失败时静默 no-op (TAARenderer 层 fallback 走 BlitTAAToHDR / DrawTAAUpscalePass)
    /// @param srcTex     history half-res tex
    /// @param dstFbo     HDR FBO
    /// @param srcW, srcH src 分辨率
    /// @param dstW, dstH dst 分辨率
    void DrawTAALanczosPass(uint32_t srcTex, uint32_t dstFbo,
                            int srcW, int srcH,
                            int dstW, int dstH,
                            int rgnX, int rgnY, int rgnW, int rgnH) override {   // Phase F.0.10.2
        if (!programLanczosUpscale || !srcTex || !dstFbo || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.2: region 启用时 scissor 限制 lanczos 写回到子矩形
        const bool useScissor = (rgnW > 0 && rgnH > 0);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programLanczosUpscale);
        // uTexel = 1 / src 分辨率 (Lanczos 在 src 纹素空间 5x5 采样)
        if (locLanczos_Texel >= 0) glUniform2f(locLanczos_Texel, 1.0f / (float)srcW, 1.0f / (float)srcH);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        if (useScissor) glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.2: 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Phase F.0.12 — TAA RCAS pass: 5-tap robust contrast-adaptive sharpening (AMD FidelityFX FSR2)
    /// 与 F.0.1 unsharp / F.0.6 cas 同接口; sharpness ∈ [0, 2] (FSR2 标准范围, 调用方负责 clamp).
    /// FSR2 robust 增强: noise detection (range<1/64 跳过) + edge protection (lobe sqrt 限制).
    /// shader 编译失败时静默 no-op (TAARenderer 层 fallback 走 BlitTAAToHDR).
    void DrawTAARCASPass(uint32_t srcTex, uint32_t dstFbo,
                         int w, int h, float sharpness,
                         int rgnX, int rgnY, int rgnW, int rgnH) override {   // Phase F.0.10.2
        if (!programRCAS || !srcTex || !dstFbo || w <= 0 || h <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.2: region 启用时 scissor 限制 RCAS 写回到子矩形
        const bool useScissor = (rgnW > 0 && rgnH > 0);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programRCAS);
        if (locRCAS_TexelSize >= 0) glUniform2f(locRCAS_TexelSize, 1.0f / (float)w, 1.0f / (float)h);
        if (locRCAS_Sharpness >= 0) glUniform1f(locRCAS_Sharpness, sharpness);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        // 状态复位
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        if (useScissor) glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.2: 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Phase F.0.6 — TAA CAS pass: 5-tap contrast-adaptive sharpening (AMD FidelityFX FSR1)
    /// 与 F.0.1 unsharp 同接口; sharpness ∈ [0, 1] (FSR1 标准范围, 调用方负责 clamp).
    /// shader 编译失败时静默 no-op (TAARenderer 层 fallback 走 BlitTAAToHDR).
    void DrawTAACASPass(uint32_t srcTex, uint32_t dstFbo,
                        int w, int h, float sharpness,
                        int rgnX, int rgnY, int rgnW, int rgnH) override {   // Phase F.0.10.2
        if (!programCAS || !srcTex || !dstFbo || w <= 0 || h <= 0) return;

        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        // Phase F.0.10.2: region 启用时 scissor 限制 CAS 写回到子矩形
        const bool useScissor = (rgnW > 0 && rgnH > 0);
        if (useScissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rgnX, rgnY, rgnW, rgnH);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glUseProgram(programCAS);
        if (locCAS_TexelSize >= 0) glUniform2f(locCAS_TexelSize, 1.0f / (float)w, 1.0f / (float)h);
        if (locCAS_Sharpness >= 0) glUniform1f(locCAS_Sharpness, sharpness);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        if (useScissor) glDisable(GL_SCISSOR_TEST);   // Phase F.0.10.2: 复位
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// 把 TAA 输出 blit 回 HDR sceneTex (覆盖, 让 Tonemap 用 TAA 后内容)
    /// 直接用 glBlitFramebuffer: src=TAA history FBO, dst=HDR FBO (sceneTex 是 COLOR_ATTACHMENT0)
    /// 注意 dstFbo 是 HDR FBO 而非裸 sceneTex; 由调用方 (TAARenderer) 传入并确保 sceneTex 是 attachment0.
    /// Phase F.0.5: src/dst 尺寸不同时走 GL_LINEAR stretch (history half-res → sceneTex full-res 上采样)
    void BlitTAAToHDR(uint32_t srcTex, uint32_t dstFbo,
                      int srcW, int srcH,
                      int dstW = 0, int dstH = 0,
                      int rgnX = 0, int rgnY = 0,                 // Phase F.0.10.2
                      int rgnW = 0, int rgnH = 0) override {
        if (!srcTex || !dstFbo || srcW <= 0 || srcH <= 0) return;
        // dstW/dstH=0 → 退化为 src 同 dst (Phase F.0 老行为)
        if (dstW <= 0) dstW = srcW;
        if (dstH <= 0) dstH = srcH;

        // Phase F.0.10.2: region 启用时, 用 dst rect 直接定位子矩形, src 仍是全 size
        // src 矩形 → dst 矩形: GL_LINEAR stretch 处理尺寸不匹配
        const bool useRegion = (rgnW > 0 && rgnH > 0);
        int srcX0 = 0, srcY0 = 0, srcX1 = srcW, srcY1 = srcH;
        int dstX0 = 0, dstY0 = 0, dstX1 = dstW, dstY1 = dstH;
        if (useRegion) {
            dstX0 = rgnX;        dstY0 = rgnY;
            dstX1 = rgnX + rgnW; dstY1 = rgnY + rgnH;
            // src 仍是全 size (instance 自己的 history 是 region-size, 全采样)
        }

        // 用临时 FBO 包 srcTex (避免要求 TAARenderer 传 FBO; 接口更对称)
        GLuint tempFbo = 0;
        glGenFramebuffers(1, &tempFbo);
        if (!tempFbo) return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, tempFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, (GLuint)srcTex, 0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dstFbo);
            // Phase F.0.5: 尺寸相同走 GL_NEAREST (1:1 拷贝, 零回归);
            //              尺寸不同走 GL_LINEAR (bilinear stretch, half→full 上采样)
            // Phase F.0.10.2: region 用 dst rect 偏移; src 全 size → dst region 1:1 / stretch
            const int srcRW = srcX1 - srcX0;
            const int srcRH = srcY1 - srcY0;
            const int dstRW = dstX1 - dstX0;
            const int dstRH = dstY1 - dstY0;
            const GLenum filter = (srcRW == dstRW && srcRH == dstRH) ? GL_NEAREST : GL_LINEAR;
            glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1,
                              dstX0, dstY0, dstX1, dstY1,
                              GL_COLOR_BUFFER_BIT, filter);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &tempFbo);
    }

    void SetNextPreviousModelMatrix(const float* prevModelMat4) override {
        if (!prevModelMat4) {
            hasNextPrevModel = false;
            return;
        }
        memcpy(nextPrevModel.m, prevModelMat4, sizeof(float) * 16);
        hasNextPrevModel = true;
    }

    // ==================== Phase AS.2 — 3D mesh + 深度测试 + camera ====================

    bool Supports3D() const override { return programPBR != 0 || programUnlit != 0; }

    uint32_t CreateMesh(const RenderVertex3D* verts, int vCount,
                        const uint32_t* indices, int iCount) override {
        if (!verts || vCount <= 0 || !indices || iCount <= 0) return 0;
        if (!programPBR && !programUnlit) return 0;  // 无 3D shader 时不能创建

        MeshGPU m;
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ebo);
        glBindVertexArray(m.vao);

        // VBO: 上传顶点
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(RenderVertex3D), verts, GL_STATIC_DRAW);

        // EBO: 上传索引 (uint32, 兼容 mesh 顶点数 > 65536)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, iCount * sizeof(uint32_t), indices, GL_STATIC_DRAW);

        // 顶点属性 layout: pos(0), normal(1), uv(2), color(3)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, nx));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, u));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, r));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        m.indexCount = iCount;
        uint32_t id = nextMeshId++;
        meshes[id] = m;
        return id;
    }

    /// Phase G.1.2 — 注册 worker 已上传完毕的 mesh GL handles
    /// 输入: 已经 glBufferData 完成的 vao/vbo/ebo + index count;
    /// 行为: 仅 C++ 共享状态写入 (O(1)), 不发任何 GL 命令;
    /// 失败: 不接管 handles, 返 0; 调用方负责清理.
    uint32_t RegisterUploadedMesh(uint32_t vao, uint32_t vbo, uint32_t ebo, int idxCount) override {
        if (!vao || !vbo || !ebo || idxCount <= 0) return 0;
        if (!programPBR && !programUnlit) return 0;  // 与 CreateMesh 等价的 3D 支持检查
        MeshGPU m;
        m.vao = (GLuint)vao;
        m.vbo = (GLuint)vbo;
        m.ebo = (GLuint)ebo;
        m.indexCount = idxCount;
        uint32_t id = nextMeshId++;
        meshes[id] = m;
        return id;
    }

    void DeleteMesh(uint32_t meshId) override {
        // Phase AW: 高位 0x80000000 表示 skinned mesh
        if (meshId & 0x80000000u) {
            auto it = skinnedMeshes.find(meshId);
            if (it == skinnedMeshes.end()) return;
            const MeshGPU& m = it->second;
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            skinnedMeshes.erase(it);
            return;
        }
        auto it = meshes.find(meshId);
        if (it == meshes.end()) return;
        const MeshGPU& m = it->second;
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        meshes.erase(it);
    }

    /// 旧 API: 用 textureId 简单绘制. 内部走 default PBR + uColor=(1,1,1,1) + texBaseColor 模式.
    void DrawMesh(uint32_t meshId, uint32_t textureId) override {
        // 构造默认 PBR MaterialDesc, 走 DrawMeshMaterial
        MaterialDesc desc = {};
        desc.mode = 1;  // PBR
        desc.color[0] = desc.color[1] = desc.color[2] = desc.color[3] = 1.0f;
        desc.metallic = 0.0f;
        desc.roughness = 1.0f;
        desc.normalScale = 1.0f;
        desc.occlusionStrength = 1.0f;
        desc.alphaMode = 0;
        desc.alphaCutoff = 0.5f;
        desc.texBaseColor = textureId;
        DrawMeshMaterial(meshId, &desc);
    }

    void SetDepthTest(bool enable) override {
        if (enable == depthTestEnabled) return;
        depthTestEnabled = enable;
        if (enable) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    void SetDepthFunc(int func) override {
        // 0=Less, 1=LEqual, 2=Greater, 3=GEqual, 4=Equal, 5=NotEqual, 6=Always, 7=Never
        GLenum gl_func;
        switch (func) {
            case 0: gl_func = GL_LESS; break;
            case 1: gl_func = GL_LEQUAL; break;
            case 2: gl_func = GL_GREATER; break;
            case 3: gl_func = GL_GEQUAL; break;
            case 4: gl_func = GL_EQUAL; break;
            case 5: gl_func = GL_NOTEQUAL; break;
            case 6: gl_func = GL_ALWAYS; break;
            case 7: gl_func = GL_NEVER; break;
            default: gl_func = GL_LEQUAL; break;
        }
        glDepthFunc(gl_func);
    }

    void LoadView(const float* viewMat4) override {
        if (!viewMat4) {
            hasView = false;
            return;
        }
        memcpy(viewMatrix.m, viewMat4, sizeof(viewMatrix.m));
        hasView = true;
    }

    void LoadProjection(const float* projMat4) override {
        if (!projMat4) return;
        memcpy(projection.m, projMat4, sizeof(projection.m));
        // Phase F.0: 用户重设 projection 时强制清 jitter (TAA 下帧 BeginScene 重新设)
        jitterActive = false;
    }

    // ==================== Phase F.0 — TAA Master Pipeline 双 projection 接口 ====================

    /// 设置 jittered projection 矩阵 — TAA Enable 时每帧 BeginScene 前调.
    /// 后续 raster 路径 (ComputeMVP3D / FlushMVP / BeginLit2DDraw) 用 jitteredProjection.
    /// GetProjection() / ComputeViewProj3D() 仍返 unjittered (SSR/SSAO/velocity 零改动).
    void LoadJitteredProjection(const float* jitteredProj) override {
        if (!jitteredProj) return;
        memcpy(jitteredProjection.m, jitteredProj, sizeof(jitteredProjection.m));
        jitterActive = true;
    }

    /// 清除 jitter 模式 — TAA Process 末尾调, 下帧 BeginScene 重新设.
    void ClearJitteredProjection() override {
        jitterActive = false;
    }

    /// 查询当前是否启用了 jittered projection (debug HUD 用)
    bool IsJitteredProjectionActive() const override {
        return jitterActive;
    }

    // ==================== Phase AS.4 — 材质系统 + 多光源实现 ====================

    /// 上传 sampler 绑定 + uHasXxxTex 标志 (PBR 和 Unlit 共用辅助)
    void BindMaterialTexture(GLuint program, const char* samplerName, const char* hasFlagName,
                              int slot, uint32_t texId) {
        GLint locSampler = glGetUniformLocation(program, samplerName);
        GLint locHas     = glGetUniformLocation(program, hasFlagName);
        if (texId) {
            glActiveTexture(GL_TEXTURE0 + slot);
            glBindTexture(GL_TEXTURE_2D, texId);
            if (locSampler >= 0) glUniform1i(locSampler, slot);
            if (locHas >= 0)     glUniform1i(locHas, 1);
        } else {
            if (locHas >= 0)     glUniform1i(locHas, 0);
        }
    }

    /// 上传 PBR / Unlit 共用的 baseColor / emissive / alpha uniforms
    void UploadCommonMatUniforms(GLuint program, const MaterialDesc* d) {
        GLint locColor    = glGetUniformLocation(program, "uColor");
        GLint locEmissive = glGetUniformLocation(program, "uEmissive");
        GLint locAMode    = glGetUniformLocation(program, "uAlphaMode");
        GLint locACutoff  = glGetUniformLocation(program, "uAlphaCutoff");
        if (locColor >= 0)    glUniform4f(locColor, d->color[0], d->color[1], d->color[2], d->color[3]);
        if (locEmissive >= 0) glUniform3f(locEmissive, d->emissive[0], d->emissive[1], d->emissive[2]);
        if (locAMode >= 0)    glUniform1i(locAMode, d->alphaMode);
        if (locACutoff >= 0)  glUniform1f(locACutoff, d->alphaCutoff);
        // Phase E.8.x: 上传 mat3(view) 供 FS 计算 view-space normal MRT.
        // Mat4 是 column-major; mat3 取前 3 column 的前 3 行.
        // 未 SetCamera 时 view=identity (符合 Lit2D 等无 view 场景默认行为).
        GLint locViewM3 = glGetUniformLocation(program, "uViewMat3");
        if (locViewM3 >= 0) {
            float v3[9];
            if (hasView) {
                v3[0] = viewMatrix.m[0]; v3[1] = viewMatrix.m[1]; v3[2] = viewMatrix.m[2];
                v3[3] = viewMatrix.m[4]; v3[4] = viewMatrix.m[5]; v3[5] = viewMatrix.m[6];
                v3[6] = viewMatrix.m[8]; v3[7] = viewMatrix.m[9]; v3[8] = viewMatrix.m[10];
            } else {
                v3[0]=1; v3[1]=0; v3[2]=0;
                v3[3]=0; v3[4]=1; v3[5]=0;
                v3[6]=0; v3[7]=0; v3[8]=1;
            }
            glUniformMatrix3fv(locViewM3, 1, GL_FALSE, v3);
        }
    }

    /// 上传 PBR 专用 lighting + scalar uniforms (摄像机 + 光源 + metallic/roughness/AO)
    void UploadPBRLightingUniforms(GLuint program, const MaterialDesc* d) {
        // 标量
        GLint locM   = glGetUniformLocation(program, "uMetallic");
        GLint locR   = glGetUniformLocation(program, "uRoughness");
        GLint locNS  = glGetUniformLocation(program, "uNormalScale");
        GLint locOS  = glGetUniformLocation(program, "uOcclusionStrength");
        if (locM  >= 0) glUniform1f(locM,  d->metallic);
        if (locR  >= 0) glUniform1f(locR,  d->roughness);
        if (locNS >= 0) glUniform1f(locNS, d->normalScale);
        if (locOS >= 0) glUniform1f(locOS, d->occlusionStrength);
        // 摄像机 + 环境光
        GLint locCam  = glGetUniformLocation(program, "uCameraPos");
        GLint locAmb  = glGetUniformLocation(program, "uAmbient");
        if (locCam >= 0) glUniform3f(locCam, cameraPos[0], cameraPos[1], cameraPos[2]);
        if (locAmb >= 0) glUniform3f(locAmb, ambientLight[0], ambientLight[1], ambientLight[2]);
        // 方向光
        GLint locDirEn = glGetUniformLocation(program, "uDirLightEnabled");
        GLint locDirD  = glGetUniformLocation(program, "uDirLightDir");
        GLint locDirC  = glGetUniformLocation(program, "uDirLightColor");
        if (locDirEn >= 0) glUniform1i(locDirEn, dirLightEnabled ? 1 : 0);
        if (locDirD  >= 0) glUniform3f(locDirD, dirLightDir[0], dirLightDir[1], dirLightDir[2]);
        if (locDirC  >= 0) {
            // intensity 直接乘进 color (shader 简化, 不单独传 intensity)
            float c[3] = {
                dirLightColor[0] * dirLightIntensity,
                dirLightColor[1] * dirLightIntensity,
                dirLightColor[2] * dirLightIntensity
            };
            glUniform3f(locDirC, c[0], c[1], c[2]);
        }
        // 点光数组 (跳过 used=false 的槽)
        float pos[MAX_PT_LIGHTS * 3]   = {};
        float col[MAX_PT_LIGHTS * 3]   = {};
        float range[MAX_PT_LIGHTS]     = {};
        int   activeCount = 0;
        for (int i = 0; i < MAX_PT_LIGHTS; i++) {
            if (!pointLightUsed[i]) continue;
            const PointLight& pl = pointLights[i];
            pos[activeCount * 3 + 0] = pl.pos[0];
            pos[activeCount * 3 + 1] = pl.pos[1];
            pos[activeCount * 3 + 2] = pl.pos[2];
            col[activeCount * 3 + 0] = pl.color[0] * pl.intensity;
            col[activeCount * 3 + 1] = pl.color[1] * pl.intensity;
            col[activeCount * 3 + 2] = pl.color[2] * pl.intensity;
            range[activeCount]       = pl.range;
            activeCount++;
        }
        GLint locPLCount = glGetUniformLocation(program, "uPointLightCount");
        GLint locPLPos   = glGetUniformLocation(program, "uPointLightPos");
        GLint locPLCol   = glGetUniformLocation(program, "uPointLightColor");
        GLint locPLRng   = glGetUniformLocation(program, "uPointLightRange");
        if (locPLCount >= 0) glUniform1i(locPLCount, activeCount);
        if (locPLPos   >= 0) glUniform3fv(locPLPos,   activeCount, pos);
        if (locPLCol   >= 0) glUniform3fv(locPLCol,   activeCount, col);
        if (locPLRng   >= 0) glUniform1fv(locPLRng,   activeCount, range);
    }

    /// 用 material 描述符绘制 mesh
    void DrawMeshMaterial(uint32_t meshId, const MaterialDesc* desc) override {
        if (!desc) return;
        auto it = meshes.find(meshId);
        if (it == meshes.end()) return;
        const MeshGPU& m = it->second;
        if (!m.vao || m.indexCount <= 0) return;

        GLuint program3D = (desc->mode == 0) ? programUnlit : programPBR;

        // 选择 shader: 用户 shader 激活时不切, 否则用引擎 3D 默认
        if (!userShaderActive && program3D) {
            glUseProgram(program3D);

            // MVP / Model
            Mat4 mvp = ComputeMVP3D();
            GLint locMVP   = glGetUniformLocation(program3D, "uMVP");
            GLint locModel = glGetUniformLocation(program3D, "uModel");
            if (locMVP   >= 0) glUniformMatrix4fv(locMVP,   1, GL_FALSE, mvp.m);
            if (locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, modelview.m);
            Mat4 prevModel;
            const Mat4* prevModelPtr = nullptr;
            if (hasNextPrevModel) {
                prevModel = nextPrevModel;
                prevModelPtr = &prevModel;
            }
            UploadVelocityUniforms(program3D, modelview, prevModelPtr);
            hasNextPrevModel = false;

            // 共用 uniforms (color/emissive/alpha)
            UploadCommonMatUniforms(program3D, desc);

            // 纹理 (PBR 5 个, Unlit 2 个)
            BindMaterialTexture(program3D, "uTexBaseColor", "uHasBaseColorTex", 0, desc->texBaseColor);
            BindMaterialTexture(program3D, "uTexEmissive",  "uHasEmissiveTex",  3, desc->texEmissive);
            if (desc->mode == 1) {
                BindMaterialTexture(program3D, "uTexMetallicRoughness", "uHasMetallicRoughnessTex", 1, desc->texMetallicRoughness);
                BindMaterialTexture(program3D, "uTexNormal",            "uHasNormalTex",            2, desc->texNormal);
                BindMaterialTexture(program3D, "uTexOcclusion",         "uHasOcclusionTex",         4, desc->texOcclusion);
                UploadPBRLightingUniforms(program3D, desc);
            }
            // 恢复 active texture 到 0 (后续 2D 期望)
            glActiveTexture(GL_TEXTURE0);
        } else if (userShaderActive) {
            // 用户 shader: 仅绑 baseColor 到 slot 0, 用户负责 sampler uniform
            if (desc->texBaseColor) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, desc->texBaseColor);
            }
        }
        if (userShaderActive || !program3D) hasNextPrevModel = false;

        // 临时启用深度测试 (如果用户没启用)
        bool tempDepth = !depthTestEnabled;
        if (tempDepth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }

        // doubleSided: 关掉面剔除
        bool restoreCull = false;
        if (desc->doubleSided) {
            GLboolean cullEnabled = GL_FALSE;
            glGetBooleanv(GL_CULL_FACE, &cullEnabled);
            if (cullEnabled) {
                glDisable(GL_CULL_FACE);
                restoreCull = true;
            }
        }

        // alphaMode "blend": 确保启用 blend (引擎默认已 enable, 但保险起见)
        // alphaMode "opaque" / "mask": 不影响 blend (blend 默认 enable, 不动)

        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, (void*)0);
        glBindVertexArray(0);

        if (restoreCull) glEnable(GL_CULL_FACE);
        if (tempDepth)   glDisable(GL_DEPTH_TEST);

        // 切回默认 2D shader
        if (!userShaderActive && program3D) {
            glUseProgram(program);
            glBindVertexArray(vao);
        }
    }

    void SetDirectionalLight(const float* dir, const float* color, float intensity, bool enabled) override {
        if (dir) {
            dirLightDir[0] = dir[0];
            dirLightDir[1] = dir[1];
            dirLightDir[2] = dir[2];
        }
        if (color) {
            dirLightColor[0] = color[0];
            dirLightColor[1] = color[1];
            dirLightColor[2] = color[2];
        }
        dirLightIntensity = intensity;
        dirLightEnabled   = enabled;
    }

    void SetAmbientLight(const float* rgb) override {
        if (!rgb) return;
        ambientLight[0] = rgb[0];
        ambientLight[1] = rgb[1];
        ambientLight[2] = rgb[2];
    }

    void SetCameraPos(const float* pos) override {
        if (!pos) return;
        cameraPos[0] = pos[0];
        cameraPos[1] = pos[1];
        cameraPos[2] = pos[2];
    }

    int AddPointLight(const PointLight* light) override {
        if (!light) return 0;
        for (int i = 0; i < MAX_PT_LIGHTS; i++) {
            if (!pointLightUsed[i]) {
                pointLights[i] = *light;
                pointLightUsed[i] = true;
                return i + 1;  // 1-indexed id
            }
        }
        return 0;  // 已满
    }

    void RemovePointLight(int id) override {
        if (id < 1 || id > MAX_PT_LIGHTS) return;
        pointLightUsed[id - 1] = false;
    }

    void ClearPointLights() override {
        for (int i = 0; i < MAX_PT_LIGHTS; i++) {
            pointLightUsed[i] = false;
        }
    }

    int GetPointLightCount() const override {
        int n = 0;
        for (int i = 0; i < MAX_PT_LIGHTS; i++) if (pointLightUsed[i]) n++;
        return n;
    }

    int GetMaxPointLights() const override { return MAX_PT_LIGHTS; }

    // ==================== Phase AW — GPU Skinning 实现 ====================

    bool SupportsGPUSkinning() const override { return gpuSkinningSupported; }

    uint32_t CreateSkinnedMesh(const RenderVertex3DSkin* verts, int vCount,
                                const uint32_t* indices, int iCount) override {
        if (!verts || vCount <= 0 || !indices || iCount <= 0) return 0;
        if (!gpuSkinningSupported) return 0;
        if (!programUnlitSkin && !programPBRSkin) return 0;

        MeshGPU m;
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ebo);
        glBindVertexArray(m.vao);

        // VBO: 上传 skin 顶点
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(RenderVertex3DSkin), verts, GL_STATIC_DRAW);

        // EBO: 上传索引
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, iCount * sizeof(uint32_t), indices, GL_STATIC_DRAW);

        // 顶点属性 layout: pos(0), normal(1), uv(2), color(3), joints(4 IPointer), weights(5)
        const GLsizei stride = sizeof(RenderVertex3DSkin);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(RenderVertex3DSkin, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(RenderVertex3DSkin, nx));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(RenderVertex3DSkin, u));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(RenderVertex3DSkin, r));
        // location=4: joints (uint8 × 4) — 用 IPointer 保持整数语义
        glEnableVertexAttribArray(4);
        glVertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, stride,
                                (void*)offsetof(RenderVertex3DSkin, joints_packed));
        // location=5: weights (vec4)
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(RenderVertex3DSkin, weights));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        m.indexCount = iCount;
        uint32_t id = nextSkinnedMeshId++;
        skinnedMeshes[id] = m;
        return id;
    }

    void DrawSkinnedMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                  const float* jointMatrices, int jointCount,
                                  const float* prevJointMatrices,
                                  int prevJointCount) override {
        if (!desc || !jointMatrices || jointCount <= 0) return;
        if (!gpuSkinningSupported) return;
        auto it = skinnedMeshes.find(meshId);
        if (it == skinnedMeshes.end()) return;
        const MeshGPU& m = it->second;
        if (!m.vao || m.indexCount <= 0) return;

        GLuint program3D = (desc->mode == 0) ? programUnlitSkin : programPBRSkin;
        if (!program3D) return;

        glUseProgram(program3D);

        // ---- 上传 jointMatrices 到 UBO (截断到 SKIN_MAX_JOINTS) ----
        int n = (jointCount > SKIN_MAX_JOINTS) ? SKIN_MAX_JOINTS : jointCount;
        glBindBuffer(GL_UNIFORM_BUFFER, uboJointMatrices);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, n * 16 * (GLsizeiptr)sizeof(float), jointMatrices);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        const float* prevJointsUpload = (prevJointMatrices && prevJointCount >= n)
                                            ? prevJointMatrices
                                            : jointMatrices;
        glBindBuffer(GL_UNIFORM_BUFFER, uboPrevJointMatrices);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, n * 16 * (GLsizeiptr)sizeof(float), prevJointsUpload);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        // ---- MVP / Model uniforms (与 DrawMeshMaterial 一致) ----
        Mat4 mvp = ComputeMVP3D();
        GLint locMVP   = glGetUniformLocation(program3D, "uMVP");
        GLint locModel = glGetUniformLocation(program3D, "uModel");
        if (locMVP   >= 0) glUniformMatrix4fv(locMVP,   1, GL_FALSE, mvp.m);
        if (locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, modelview.m);
        Mat4 prevModel;
        const Mat4* prevModelPtr = nullptr;
        if (hasNextPrevModel) {
            prevModel = nextPrevModel;
            prevModelPtr = &prevModel;
        }
        UploadVelocityUniforms(program3D, modelview, prevModelPtr);
        hasNextPrevModel = false;

        // ---- 共用 uniforms ----
        UploadCommonMatUniforms(program3D, desc);

        // ---- 纹理 + lighting ----
        BindMaterialTexture(program3D, "uTexBaseColor", "uHasBaseColorTex", 0, desc->texBaseColor);
        BindMaterialTexture(program3D, "uTexEmissive",  "uHasEmissiveTex",  3, desc->texEmissive);
        if (desc->mode == 1) {
            BindMaterialTexture(program3D, "uTexMetallicRoughness", "uHasMetallicRoughnessTex", 1, desc->texMetallicRoughness);
            BindMaterialTexture(program3D, "uTexNormal",            "uHasNormalTex",            2, desc->texNormal);
            BindMaterialTexture(program3D, "uTexOcclusion",         "uHasOcclusionTex",         4, desc->texOcclusion);
            UploadPBRLightingUniforms(program3D, desc);
        }
        glActiveTexture(GL_TEXTURE0);

        // ---- 临时启用深度测试 ----
        bool tempDepth = !depthTestEnabled;
        if (tempDepth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }

        // ---- doubleSided ----
        bool restoreCull = false;
        if (desc->doubleSided) {
            GLboolean cullEnabled = GL_FALSE;
            glGetBooleanv(GL_CULL_FACE, &cullEnabled);
            if (cullEnabled) {
                glDisable(GL_CULL_FACE);
                restoreCull = true;
            }
        }

        // ---- 绘制 ----
        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, (void*)0);
        glBindVertexArray(0);

        if (restoreCull) glEnable(GL_CULL_FACE);
        if (tempDepth)   glDisable(GL_DEPTH_TEST);

        // 切回默认 2D shader (与 DrawMeshMaterial 一致)
        glUseProgram(program);
        glBindVertexArray(vao);
    }

    // ==================== Phase AX — GPU Morph Target 实现 ====================

    bool SupportsMorphTargets() const override { return morphTargetsSupported; }

    // 创建 morph delta 2D texture (RGB32F): width=vCount, height=morphCount
    // data 布局: [target_0_per_vertex_xyz][target_1...]...
    static GLuint UploadMorphDeltaTexture(const float* deltas, int vCount, int morphCount) {
        if (!deltas || vCount <= 0 || morphCount <= 0) return 0;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        // RGB32F (与 GLES 3.0 + GL 3.3 都支持; alpha 通道留空避免 RGBA 浪费)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, vCount, morphCount,
                      0, GL_RGB, GL_FLOAT, deltas);
        // 不需要 mipmap; nearest filter 保证 texelFetch 精确取值
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }

    uint32_t CreateSkinnedMorphMesh(const RenderVertex3DSkin* verts, int vCount,
                                      const uint32_t* indices, int iCount,
                                      const float* posDeltas,
                                      const float* nrmDeltas,
                                      int morphTargetCount) override {
        if (!verts || vCount <= 0 || !indices || iCount <= 0) return 0;
        if (!morphTargetsSupported) return 0;
        if (morphTargetCount <= 0 || morphTargetCount > 8) return 0;
        if (!posDeltas) return 0;     // POSITION delta 必须存在
        if (!programUnlitSkinMorph && !programPBRSkinMorph) return 0;

        MeshGPU m{};
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ebo);
        glBindVertexArray(m.vao);

        // VBO + EBO (与 CreateSkinnedMesh 一致)
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(RenderVertex3DSkin), verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, iCount * sizeof(uint32_t), indices, GL_STATIC_DRAW);

        const GLsizei stride = sizeof(RenderVertex3DSkin);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(RenderVertex3DSkin, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(RenderVertex3DSkin, nx));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(RenderVertex3DSkin, u));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(RenderVertex3DSkin, r));
        glEnableVertexAttribArray(4);
        glVertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, stride, (void*)offsetof(RenderVertex3DSkin, joints_packed));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(RenderVertex3DSkin, weights));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // morph delta textures
        m.morphPosTex = UploadMorphDeltaTexture(posDeltas, vCount, morphTargetCount);
        if (!m.morphPosTex) {
            // 创建失败 → 清理资源, 返回 0
            glDeleteVertexArrays(1, &m.vao);
            glDeleteBuffers(1, &m.vbo);
            glDeleteBuffers(1, &m.ebo);
            CC::Log(CC::LOG_WARN, "GL33: morph posDelta texture upload failed");
            return 0;
        }
        if (nrmDeltas) {
            m.morphNrmTex = UploadMorphDeltaTexture(nrmDeltas, vCount, morphTargetCount);
            m.hasMorphNormal = (m.morphNrmTex != 0);
        }

        m.indexCount     = iCount;
        m.morphCount     = morphTargetCount;
        uint32_t id      = nextSkinnedMorphMeshId++;
        skinnedMorphMeshes[id] = m;
        return id;
    }

    void DrawSkinnedMorphMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                         const float* jointMatrices, int jointCount,
                                         const float* morphWeights, int morphTargetCount,
                                         const float* prevJointMatrices,
                                         int prevJointCount,
                                         const float* prevMorphWeights,
                                         int prevMorphTargetCount) override {
        if (!desc || !jointMatrices || jointCount <= 0) return;
        if (!morphWeights || morphTargetCount <= 0) return;
        if (!morphTargetsSupported) return;
        auto it = skinnedMorphMeshes.find(meshId);
        if (it == skinnedMorphMeshes.end()) return;
        const MeshGPU& m = it->second;
        if (!m.vao || m.indexCount <= 0) return;

        GLuint program3D = (desc->mode == 0) ? programUnlitSkinMorph : programPBRSkinMorph;
        if (!program3D) return;

        glUseProgram(program3D);

        // 上传 jointMatrices 到 UBO (与 DrawSkinnedMeshMaterial 一致)
        int n = (jointCount > SKIN_MAX_JOINTS) ? SKIN_MAX_JOINTS : jointCount;
        glBindBuffer(GL_UNIFORM_BUFFER, uboJointMatrices);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, n * 16 * (GLsizeiptr)sizeof(float), jointMatrices);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        const float* prevJointsUpload = (prevJointMatrices && prevJointCount >= n)
                                            ? prevJointMatrices
                                            : jointMatrices;
        glBindBuffer(GL_UNIFORM_BUFFER, uboPrevJointMatrices);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, n * 16 * (GLsizeiptr)sizeof(float), prevJointsUpload);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        // MVP / Model uniforms
        Mat4 mvp = ComputeMVP3D();
        GLint locMVP   = glGetUniformLocation(program3D, "uMVP");
        GLint locModel = glGetUniformLocation(program3D, "uModel");
        if (locMVP   >= 0) glUniformMatrix4fv(locMVP,   1, GL_FALSE, mvp.m);
        if (locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, modelview.m);
        Mat4 prevModel;
        const Mat4* prevModelPtr = nullptr;
        if (hasNextPrevModel) {
            prevModel = nextPrevModel;
            prevModelPtr = &prevModel;
        }
        UploadVelocityUniforms(program3D, modelview, prevModelPtr);
        hasNextPrevModel = false;

        // ---- Phase AX: morph weights uniform array + delta textures ----
        int mn = (morphTargetCount > 8) ? 8 : morphTargetCount;
        if (mn > m.morphCount) mn = m.morphCount;
        GLint locMW    = glGetUniformLocation(program3D, "uMorphWeights");
        GLint locPMW   = glGetUniformLocation(program3D, "uPrevMorphWeights");
        GLint locMC    = glGetUniformLocation(program3D, "uMorphCount");
        GLint locHasN  = glGetUniformLocation(program3D, "uHasMorphNormal");
        if (locMW   >= 0) glUniform1fv(locMW, mn, morphWeights);
        if (locPMW  >= 0) {
            const float* prevMorphUpload = (prevMorphWeights && prevMorphTargetCount >= mn)
                                               ? prevMorphWeights
                                               : morphWeights;
            glUniform1fv(locPMW, mn, prevMorphUpload);
        }
        if (locMC   >= 0) glUniform1i(locMC, mn);
        if (locHasN >= 0) glUniform1i(locHasN, m.hasMorphNormal ? 1 : 0);

        // 绑定 morph delta textures (slot 5/6, 与 material textures 0-4 错开)
        GLint locPosTex = glGetUniformLocation(program3D, "uMorphPosDelta");
        if (locPosTex >= 0 && m.morphPosTex) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, m.morphPosTex);
            glUniform1i(locPosTex, 5);
        }
        GLint locNrmTex = glGetUniformLocation(program3D, "uMorphNrmDelta");
        if (locNrmTex >= 0 && m.morphNrmTex) {
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, m.morphNrmTex);
            glUniform1i(locNrmTex, 6);
        } else if (locNrmTex >= 0) {
            // 即使没有 normal delta 也要绑定 sampler 到一个有效 texture (避免 GL 警告)
            // 复用 posTex 作为 placeholder, shader 由 uHasMorphNormal=0 控制不读
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, m.morphPosTex);
            glUniform1i(locNrmTex, 6);
        }

        // 共用 uniforms (material / lighting)
        UploadCommonMatUniforms(program3D, desc);
        BindMaterialTexture(program3D, "uTexBaseColor", "uHasBaseColorTex", 0, desc->texBaseColor);
        BindMaterialTexture(program3D, "uTexEmissive",  "uHasEmissiveTex",  3, desc->texEmissive);
        if (desc->mode == 1) {
            BindMaterialTexture(program3D, "uTexMetallicRoughness", "uHasMetallicRoughnessTex", 1, desc->texMetallicRoughness);
            BindMaterialTexture(program3D, "uTexNormal",            "uHasNormalTex",            2, desc->texNormal);
            BindMaterialTexture(program3D, "uTexOcclusion",         "uHasOcclusionTex",         4, desc->texOcclusion);
            UploadPBRLightingUniforms(program3D, desc);
        }
        glActiveTexture(GL_TEXTURE0);

        // 深度测试 + cull (与 DrawSkinnedMeshMaterial 一致)
        bool tempDepth = !depthTestEnabled;
        if (tempDepth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }
        bool restoreCull = false;
        if (desc->doubleSided) {
            GLboolean cullEnabled = GL_FALSE;
            glGetBooleanv(GL_CULL_FACE, &cullEnabled);
            if (cullEnabled) {
                glDisable(GL_CULL_FACE);
                restoreCull = true;
            }
        }

        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, (void*)0);
        glBindVertexArray(0);

        if (restoreCull) glEnable(GL_CULL_FACE);
        if (tempDepth)   glDisable(GL_DEPTH_TEST);

        // 切回默认 2D shader
        glUseProgram(program);
        glBindVertexArray(vao);
    }
};

// ==================== GL33Backend 工厂 ====================

RenderBackend* CreateGL33Backend() {
    auto* b = new GL33Backend();
    if (b->Init()) return b;
    delete b;
    return nullptr;
}

// ==================== 运行时自动选择工厂 ====================

RenderBackend* CreateRenderBackend() {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
    // GLES3: 直接创建, 无需 glad 加载
    RenderBackend* gl33 = CreateGL33Backend();
    if (gl33) return gl33;
    CC::Log(CC::LOG_ERROR, "GLES3 backend init failed!");
    return nullptr;
#else
    // 桌面: glad 加载 + Legacy 回退
    extern RenderBackend* CreateLegacyBackend();
    if (gladLoadGL((GLADloadfunc)PlatformWindow::GetGLProcAddress)) {
        if (GLAD_GL_VERSION_3_3) {
            RenderBackend* gl33 = CreateGL33Backend();
            if (gl33) return gl33;
            CC::Log(CC::LOG_WARN, "GL33 backend init failed, falling back to Legacy");
        } else {
            CC::Log(CC::LOG_INFO, "GL version < 3.3, using Legacy backend");
        }
    } else {
        CC::Log(CC::LOG_WARN, "glad failed to load GL, using Legacy backend");
    }
    RenderBackend* legacy = CreateLegacyBackend();
    if (legacy) return legacy;
    CC::Log(CC::LOG_ERROR, "No render backend available!");
    return nullptr;
#endif
}
