/**
 * @file render_gl33.cpp
 * @brief OpenGL 3.3 Core Profile 渲染后端 (GL33Backend)
 * @note VAO/VBO + shader pipeline, 自管理矩阵栈
 */

#include "render_backend.h"
#include "light.h"

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
void main() {
    if (uUseTexture == 1) {
        FragColor = vColor * texture(uTexture, vTexCoord);
    } else {
        FragColor = vColor;
    }
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

out vec4 FragColor;

void main() {
    if (uUseTexture == 1) {
        FragColor = vColor * texture(uTexture, vTexCoord);
    } else {
        FragColor = vColor;
    }
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
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormalW = mat3(uModel) * aNormal;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vTexCoord = aUV;
    vColor = aColor;
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
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * aNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
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
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
const int MORPH_MAX = 8;
uniform float     uMorphWeights[MORPH_MAX];
uniform int       uMorphCount;
uniform int       uHasMorphNormal;
uniform sampler2D uMorphPosDelta;
uniform sampler2D uMorphNrmDelta;
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    // 1. Morph: base + Σ (weight × delta)
    vec3 morphedPos    = aPos;
    vec3 morphedNormal = aNormal;
    for (int i = 0; i < MORPH_MAX; ++i) {
        if (i >= uMorphCount) break;
        float w = uMorphWeights[i];
        if (w == 0.0) continue;
        morphedPos += w * texelFetch(uMorphPosDelta, ivec2(gl_VertexID, i), 0).xyz;
        if (uHasMorphNormal == 1) {
            morphedNormal += w * texelFetch(uMorphNrmDelta, ivec2(gl_VertexID, i), 0).xyz;
        }
    }
    // 2. Skin: 4 joint blend
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(morphedPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * morphedNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
}
)";

// ---- FS Unlit (GLES 3.0) ----
static const char* FS_UNLIT_SOURCE = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
in vec4 vColor;
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform sampler2D uTexBaseColor;
uniform sampler2D uTexEmissive;
uniform int uHasBaseColorTex;
uniform int uHasEmissiveTex;
uniform int uAlphaMode;       // 0=opaque, 1=blend, 2=mask
uniform float uAlphaCutoff;
layout(location=0) out vec4 FragColor;
void main() {
    vec4 base = uColor * vColor;
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord);
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord).rgb;
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(base.rgb + emissive, outAlpha);
}
)";

// ---- FS PBR (GLES 3.0, 简化 Cook-Torrance) ----
static const char* FS_PBR_SOURCE = R"(#version 300 es
precision highp float;
in vec3 vNormalW;
in vec3 vWorldPos;
in vec2 vTexCoord;
in vec4 vColor;
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
layout(location=0) out vec4 FragColor;
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
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord);
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessTex == 1) {
        vec3 mr = texture(uTexMetallicRoughness, vTexCoord).rgb;
        metallic *= mr.b;
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = normalize(vNormalW);
    if (uHasNormalTex == 1) {
        vec3 mappedN = texture(uTexNormal, vTexCoord).rgb * 2.0 - 1.0;
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
    if (uHasOcclusionTex == 1) ao = mix(1.0, texture(uTexOcclusion, vTexCoord).r, uOcclusionStrength);
    vec3 ambient = uAmbient * base.rgb * ao;
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord).rgb;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(ambient + lightSum + emissive, outAlpha);
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
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormalW = mat3(uModel) * aNormal;
    vWorldPos = (uModel * vec4(aPos, 1.0)).xyz;
    vTexCoord = aUV;
    vColor = aColor;
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
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * aNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
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
layout(std140) uniform JointBlock {
    mat4 uJointMats[64];
};
const int MORPH_MAX = 8;
uniform float     uMorphWeights[MORPH_MAX];
uniform int       uMorphCount;
uniform int       uHasMorphNormal;
uniform sampler2D uMorphPosDelta;
uniform sampler2D uMorphNrmDelta;
out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    vec3 morphedPos    = aPos;
    vec3 morphedNormal = aNormal;
    for (int i = 0; i < MORPH_MAX; ++i) {
        if (i >= uMorphCount) break;
        float w = uMorphWeights[i];
        if (w == 0.0) continue;
        morphedPos += w * texelFetch(uMorphPosDelta, ivec2(gl_VertexID, i), 0).xyz;
        if (uHasMorphNormal == 1) {
            morphedNormal += w * texelFetch(uMorphNrmDelta, ivec2(gl_VertexID, i), 0).xyz;
        }
    }
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(morphedPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * morphedNormal;
    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
}
)";

// ---- FS Unlit (GL 3.3) ----
static const char* FS_UNLIT_SOURCE = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform sampler2D uTexBaseColor;
uniform sampler2D uTexEmissive;
uniform int uHasBaseColorTex;
uniform int uHasEmissiveTex;
uniform int uAlphaMode;
uniform float uAlphaCutoff;
out vec4 FragColor;
void main() {
    vec4 base = uColor * vColor;
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord);
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord).rgb;
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(base.rgb + emissive, outAlpha);
}
)";

// ---- FS PBR (GL 3.3) ----
static const char* FS_PBR_SOURCE = R"(
#version 330 core
in vec3 vNormalW;
in vec3 vWorldPos;
in vec2 vTexCoord;
in vec4 vColor;
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
out vec4 FragColor;
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
    if (uHasBaseColorTex == 1) base *= texture(uTexBaseColor, vTexCoord);
    if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessTex == 1) {
        vec3 mr = texture(uTexMetallicRoughness, vTexCoord).rgb;
        metallic *= mr.b;
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = normalize(vNormalW);
    if (uHasNormalTex == 1) {
        vec3 mappedN = texture(uTexNormal, vTexCoord).rgb * 2.0 - 1.0;
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
    if (uHasOcclusionTex == 1) ao = mix(1.0, texture(uTexOcclusion, vTexCoord).r, uOcclusionStrength);
    vec3 ambient = uAmbient * base.rgb * ao;
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive *= texture(uTexEmissive, vTexCoord).rgb;
    float outAlpha = (uAlphaMode == 1) ? base.a : 1.0;
    FragColor = vec4(ambient + lightSum + emissive, outAlpha);
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
    Mat4 projection;
    Mat4 modelview;

    // 当前绑定的纹理 (0 = 无)
    GLuint boundTex = 0;

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
    bool    gpuSkinningSupported = false;
    static constexpr GLuint UBO_BINDING_POINT = 0;     // 固定 binding point
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
    // E.1.1 (本任务): 仅创建 VAO/VBO/EBO + 配置顶点属性 layout, shader 留给 E.1.2
    GLuint vaoLit2D     = 0;
    GLuint vboLit2D     = 0;
    GLuint eboLit2D     = 0;
    GLuint programLit2D = 0;       // E.1.2 接入, 本任务中始终 0
    bool   lit2DSupported = false; // E.1.1 资源就绪后置 true
    static constexpr int LIT2D_VBO_INITIAL_VERTS = 4;  // 单 quad; E.1.5 可按需扩容

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

    // 计算 3D MVP: projection * view * modelview (含相机 transform)
    Mat4 ComputeMVP3D() const {
        Mat4 vm = hasView ? (viewMatrix * modelview) : modelview;
        return projection * vm;
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

        // ---- Phase E.1.1 — 2D Lit 渲染资源 (VAO/VBO/EBO) ----
        InitLit2D();

        CC::Log(CC::LOG_INFO, "RenderBackend: GL33 Core initialized (GL %s)%s%s%s",
                (const char*)glGetString(GL_VERSION),
                (programUnlit && programPBR) ? ", 3D Unlit+PBR enabled" :
                (programUnlit || programPBR) ? ", partial 3D shader" : "",
                gpuSkinningSupported ? ", GPU skinning enabled" : "",
                lit2DSupported       ? ", Lit2D resources ready" : "");
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
        if (!uboJointMatrices) {
            CC::Log(CC::LOG_WARN, "GL33: UBO creation failed");
            if (programUnlitSkin) { glDeleteProgram(programUnlitSkin); programUnlitSkin = 0; }
            if (programPBRSkin)   { glDeleteProgram(programPBRSkin);   programPBRSkin   = 0; }
            return;
        }
        // 绑定 UBO 到固定 binding point 0
        glBindBufferBase(GL_UNIFORM_BUFFER, UBO_BINDING_POINT, uboJointMatrices);

        // 4. 把 program 中的 "JointBlock" uniform block 关联到 binding point 0
        auto bindBlock = [&](GLuint prog) {
            if (!prog) return;
            GLuint blockIdx = glGetUniformBlockIndex(prog, "JointBlock");
            if (blockIdx != GL_INVALID_INDEX) {
                glUniformBlockBinding(prog, blockIdx, UBO_BINDING_POINT);
            }
        };
        bindBlock(programUnlitSkin);
        bindBlock(programPBRSkin);

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
            morphTargetsSupported = true;
            CC::Log(CC::LOG_INFO, "GL33: Phase AX Morph Target shader compiled successfully");
        } else {
            CC::Log(CC::LOG_WARN, "GL33: Phase AX Morph Target unavailable (CPU fallback)");
        }
    }

    // Phase E.1.1 — 2D Lit 渲染管线初始化 (仅 GL 对象, shader 在 E.1.2 接入)
    //
    // 本任务范围:
    //   1. glGenVertexArrays / glGenBuffers 创建 VAO + 动态 VBO + 静态 EBO
    //   2. 配置顶点属性 layout (location 0..4, 与 VS_LIT2D 静态 layout 一致)
    //   3. 上传单 quad 静态索引 [0,1,2, 0,2,3]
    //   4. 不编译 shader; programLit2D 留空, 由 E.1.2 实现
    //
    // 失败影响: lit2DSupported = false, DrawLit2DQuad 等接口仍是默认 no-op,
    //          调用方应通过 SupportsLit2D() 检查后回退到普通 Draw 路径.
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

        // GL 对象就绪 (shader 留 E.1.2): 后续 SupportsLit2D() 返回 true
        // E.1.2 完成 program link 后, 可在该处加严判定 (lit2DSupported &= programLit2D != 0)
        lit2DSupported = true;
        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.1.1 Lit2D VAO/VBO/EBO ready (shader pending E.1.2)");
    }

    void Shutdown() override {
        if (program) glDeleteProgram(program);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        if (vao) glDeleteVertexArrays(1, &vao);
        program = vao = vbo = ebo = 0;
        eboCapacity = 0;

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
        if (programLit2D) { glDeleteProgram(programLit2D); programLit2D = 0; }
        if (eboLit2D)     { glDeleteBuffers(1, &eboLit2D); eboLit2D = 0; }
        if (vboLit2D)     { glDeleteBuffers(1, &vboLit2D); vboLit2D = 0; }
        if (vaoLit2D)     { glDeleteVertexArrays(1, &vaoLit2D); vaoLit2D = 0; }
        lit2DSupported = false;
    }

    bool SupportsLit2D() const override { return lit2DSupported; }

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
                                  const float* jointMatrices, int jointCount) override {
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

        // ---- MVP / Model uniforms (与 DrawMeshMaterial 一致) ----
        Mat4 mvp = ComputeMVP3D();
        GLint locMVP   = glGetUniformLocation(program3D, "uMVP");
        GLint locModel = glGetUniformLocation(program3D, "uModel");
        if (locMVP   >= 0) glUniformMatrix4fv(locMVP,   1, GL_FALSE, mvp.m);
        if (locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, modelview.m);

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
                                         const float* morphWeights, int morphTargetCount) override {
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

        // MVP / Model uniforms
        Mat4 mvp = ComputeMVP3D();
        GLint locMVP   = glGetUniformLocation(program3D, "uMVP");
        GLint locModel = glGetUniformLocation(program3D, "uModel");
        if (locMVP   >= 0) glUniformMatrix4fv(locMVP,   1, GL_FALSE, mvp.m);
        if (locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, modelview.m);

        // ---- Phase AX: morph weights uniform array + delta textures ----
        int mn = (morphTargetCount > 8) ? 8 : morphTargetCount;
        if (mn > m.morphCount) mn = m.morphCount;
        GLint locMW    = glGetUniformLocation(program3D, "uMorphWeights");
        GLint locMC    = glGetUniformLocation(program3D, "uMorphCount");
        GLint locHasN  = glGetUniformLocation(program3D, "uHasMorphNormal");
        if (locMW   >= 0) glUniform1fv(locMW, mn, morphWeights);
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
