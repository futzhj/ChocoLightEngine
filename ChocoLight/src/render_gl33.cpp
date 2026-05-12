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
out vec4 FragColor;
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

// ---- FS_TONEMAP (GLES 3.0) Phase E.3.4 — 多 tonemap operator ----
static const char* FS_TONEMAP_SOURCE = R"(#version 300 es
precision highp float;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDRTex;
uniform float uExposure;
uniform float uGamma;
uniform int   uTonemapMode;   // 0=ACES 1=Reinhard 2=Uncharted2 3=Linear

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
void main() {
    // COD AW 13-tap: 4 个 ±2 偏移 + 8 个 ±1/±2 偏移 + 1 个中心
    vec3 A = texture(uSrc, vUV + uTexel * vec2(-2.0,  2.0)).rgb;
    vec3 B = texture(uSrc, vUV + uTexel * vec2( 0.0,  2.0)).rgb;
    vec3 C = texture(uSrc, vUV + uTexel * vec2( 2.0,  2.0)).rgb;
    vec3 D = texture(uSrc, vUV + uTexel * vec2(-2.0,  0.0)).rgb;
    vec3 E = texture(uSrc, vUV                              ).rgb;
    vec3 F = texture(uSrc, vUV + uTexel * vec2( 2.0,  0.0)).rgb;
    vec3 G = texture(uSrc, vUV + uTexel * vec2(-2.0, -2.0)).rgb;
    vec3 H = texture(uSrc, vUV + uTexel * vec2( 0.0, -2.0)).rgb;
    vec3 I = texture(uSrc, vUV + uTexel * vec2( 2.0, -2.0)).rgb;
    vec3 J = texture(uSrc, vUV + uTexel * vec2(-1.0,  1.0)).rgb;
    vec3 K = texture(uSrc, vUV + uTexel * vec2( 1.0,  1.0)).rgb;
    vec3 L = texture(uSrc, vUV + uTexel * vec2(-1.0, -1.0)).rgb;
    vec3 M = texture(uSrc, vUV + uTexel * vec2( 1.0, -1.0)).rgb;
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
void main() {
    vec2 d = uTexel * uRadius;
    // tent 9-tap
    vec3 c = texture(uSrc, vUV).rgb * 4.0;
    c += texture(uSrc, vUV + vec2(-d.x,  0.0)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2( d.x,  0.0)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2( 0.0, -d.y)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2( 0.0,  d.y)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2(-d.x, -d.y)).rgb;
    c += texture(uSrc, vUV + vec2( d.x, -d.y)).rgb;
    c += texture(uSrc, vUV + vec2(-d.x,  d.y)).rgb;
    c += texture(uSrc, vUV + vec2( d.x,  d.y)).rgb;
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

// ---- FS_TONEMAP (GL 3.3) Phase E.3.4 — 多 tonemap operator ----
static const char* FS_TONEMAP_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDRTex;
uniform float uExposure;
uniform float uGamma;
uniform int   uTonemapMode;   // 0=ACES 1=Reinhard 2=Uncharted2 3=Linear

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
void main() {
    vec3 A = texture(uSrc, vUV + uTexel * vec2(-2.0,  2.0)).rgb;
    vec3 B = texture(uSrc, vUV + uTexel * vec2( 0.0,  2.0)).rgb;
    vec3 C = texture(uSrc, vUV + uTexel * vec2( 2.0,  2.0)).rgb;
    vec3 D = texture(uSrc, vUV + uTexel * vec2(-2.0,  0.0)).rgb;
    vec3 E = texture(uSrc, vUV                              ).rgb;
    vec3 F = texture(uSrc, vUV + uTexel * vec2( 2.0,  0.0)).rgb;
    vec3 G = texture(uSrc, vUV + uTexel * vec2(-2.0, -2.0)).rgb;
    vec3 H = texture(uSrc, vUV + uTexel * vec2( 0.0, -2.0)).rgb;
    vec3 I = texture(uSrc, vUV + uTexel * vec2( 2.0, -2.0)).rgb;
    vec3 J = texture(uSrc, vUV + uTexel * vec2(-1.0,  1.0)).rgb;
    vec3 K = texture(uSrc, vUV + uTexel * vec2( 1.0,  1.0)).rgb;
    vec3 L = texture(uSrc, vUV + uTexel * vec2(-1.0, -1.0)).rgb;
    vec3 M = texture(uSrc, vUV + uTexel * vec2( 1.0, -1.0)).rgb;
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
void main() {
    vec2 d = uTexel * uRadius;
    vec3 c = texture(uSrc, vUV).rgb * 4.0;
    c += texture(uSrc, vUV + vec2(-d.x,  0.0)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2( d.x,  0.0)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2( 0.0, -d.y)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2( 0.0,  d.y)).rgb * 2.0;
    c += texture(uSrc, vUV + vec2(-d.x, -d.y)).rgb;
    c += texture(uSrc, vUV + vec2( d.x, -d.y)).rgb;
    c += texture(uSrc, vUV + vec2(-d.x,  d.y)).rgb;
    c += texture(uSrc, vUV + vec2( d.x,  d.y)).rgb;
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

void main() {
    float d = texture(uDepthTex, vUV).r;
    // 天空 / 无几何: AO = 1 (无遮蔽)
    if (d >= 0.9999) { FragColor = vec4(1.0); return; }

    vec3 P = ReconstructViewPos(vUV, d);
    // 屏幕空间 ddx/ddy 重建法线
    vec3 N = normalize(cross(dFdy(P), dFdx(P)));
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

#else  // 桌面 GL 3.3 Core

// ---- FS_SSAO (GL 3.3): 同 GLES3 算法 ----
static const char* FS_SSAO_SOURCE = R"(
#version 330 core
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepthTex;
uniform sampler2D uNoiseTex;
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

void main() {
    float d = texture(uDepthTex, vUV).r;
    if (d >= 0.9999) { FragColor = vec4(1.0); return; }

    vec3 P = ReconstructViewPos(vUV, d);
    vec3 N = normalize(cross(dFdy(P), dFdx(P)));
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
    GLint  locTonemap_HDRTex   = -1;
    GLint  locTonemap_Exposure = -1;
    GLint  locTonemap_Gamma    = -1;
    GLint  locTonemap_Mode     = -1;   // Phase E.3.4 — uTonemapMode (int)
    // HDR FBO → depth RBO 关系映射 (CreateHDRFBO 写入, DeleteHDRFBO 查询并释放)
    std::unordered_map<uint32_t, uint32_t> hdrFboDepthRB;

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
    GLint  locBloomDown_Src   = -1;
    GLint  locBloomDown_Texel = -1;

    // Upsample uniform: uSrc(sampler) / uTexel(vec2) / uRadius(float) / uIntensity(float)
    GLint  locBloomUp_Src       = -1;
    GLint  locBloomUp_Texel     = -1;
    GLint  locBloomUp_Radius    = -1;
    GLint  locBloomUp_Intensity = -1;

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
        locTonemap_HDRTex   = glGetUniformLocation(programTonemap, "uHDRTex");
        locTonemap_Exposure = glGetUniformLocation(programTonemap, "uExposure");
        locTonemap_Gamma    = glGetUniformLocation(programTonemap, "uGamma");
        locTonemap_Mode     = glGetUniformLocation(programTonemap, "uTonemapMode");

        // 一次性绑 sampler 到 texture unit 0
        glUseProgram(programTonemap);
        if (locTonemap_HDRTex >= 0) glUniform1i(locTonemap_HDRTex, 0);
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

        locBloomDown_Src   = glGetUniformLocation(programBloomDown, "uSrc");
        locBloomDown_Texel = glGetUniformLocation(programBloomDown, "uTexel");
        glUseProgram(programBloomDown);
        if (locBloomDown_Src >= 0) glUniform1i(locBloomDown_Src, 0);

        locBloomUp_Src       = glGetUniformLocation(programBloomUp, "uSrc");
        locBloomUp_Texel     = glGetUniformLocation(programBloomUp, "uTexel");
        locBloomUp_Radius    = glGetUniformLocation(programBloomUp, "uRadius");
        locBloomUp_Intensity = glGetUniformLocation(programBloomUp, "uIntensity");
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
            glUseProgram(programSSAO);
            if (locSSAO_DepthTex >= 0) glUniform1i(locSSAO_DepthTex, 0);  // slot 0
            if (locSSAO_NoiseTex >= 0) glUniform1i(locSSAO_NoiseTex, 1);  // slot 1

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

        CC::Log(CC::LOG_INFO,
                "GL33: Phase E.6+E.7+E.8 LensFx/SSAO ready (lensDirt=%s, streak=%s, lensFlare=%s, ssao=%s; programs=[LD=%u, SB=%u, SC=%u, LFG=%u, S=%u, SB=%u, SC=%u])",
                lensDirtSupported ? "yes" : "no",
                streakSupported   ? "yes" : "no",
                lensFlareSupported? "yes" : "no",
                ssaoSupported     ? "yes" : "no",
                programLensDirt, programStreakBlur, programStreakComposite,
                programLensFlareGhost,
                programSSAO, programSSAOBlur, programSSAOComposite);
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
        // 清理 CreateHDRFBO 遗留的 depth RBO (HDRRenderer::Shutdown 未配对 DeleteHDRFBO 时的兜底)
        for (auto& kv : hdrFboDepthRB) {
            if (kv.second) { GLuint rb = kv.second; glDeleteRenderbuffers(1, &rb); }
        }
        hdrFboDepthRB.clear();
        tonemapSupported = false;

        // Phase E.4 — 释放 Bloom shader (pyramid 资源由 BloomRenderer::Shutdown 配对释放)
        if (programBloomBright) { glDeleteProgram(programBloomBright); programBloomBright = 0; }
        if (programBloomDown)   { glDeleteProgram(programBloomDown);   programBloomDown   = 0; }
        if (programBloomUp)     { glDeleteProgram(programBloomUp);     programBloomUp     = 0; }
        locBloomBright_Src = locBloomBright_Threshold = -1;
        locBloomDown_Src = locBloomDown_Texel = -1;
        locBloomUp_Src = locBloomUp_Texel = locBloomUp_Radius = locBloomUp_Intensity = -1;
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
        locSSAO_DepthTex = locSSAO_NoiseTex = locSSAO_Proj = locSSAO_InvProj = -1;
        locSSAO_Kernel = locSSAO_KernelSize = locSSAO_Radius = locSSAO_Bias = -1;
        locSSAO_Power = locSSAO_NoiseScale = -1;
        locSSAOBlur_SSAOTex = locSSAOBlur_DepthTex = locSSAOBlur_Texel = locSSAOBlur_Axis = -1;
        locSSAOComp_SceneTex = locSSAOComp_AOTex = locSSAOComp_Intensity = -1;
        ssaoSupported = false;
    }

    bool SupportsLit2D() const override { return lit2DSupported; }

    // ==================== Phase E.3.1 — HDR + Tonemap 虚接口实现 ====================

    bool SupportsHDR() const override { return tonemapSupported; }

    /// 创建 HDR FBO: RGBA16F 颜色附件 + Depth24 RBO (hdrFboDepthRB map 关联管理)
    uint32_t CreateHDRFBO(int w, int h, uint32_t* outTex) override {
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

        // 2. 创建 Depth24 RBO
        GLuint depthRB = 0;
        glGenRenderbuffers(1, &depthRB);
        if (!depthRB) { glDeleteTextures(1, &tex); return 0; }
        glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        // 3. 创建 FBO 并附加 color + depth
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        if (!fbo) {
            glDeleteTextures(1, &tex);
            glDeleteRenderbuffers(1, &depthRB);
            return 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_ERROR, "GL33: HDR FBO incomplete (status=0x%X, %dx%d)",
                    status, w, h);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &depthRB);
            glDeleteTextures(1, &tex);
            return 0;
        }

        hdrFboDepthRB[fbo] = depthRB;
        *outTex = tex;
        return fbo;
    }

    /// 释放 HDR FBO 资源 (与 CreateHDRFBO 配对; 自动从 map 查 depthRB)
    void DeleteHDRFBO(uint32_t fbo, uint32_t tex) override {
        if (fbo) {
            auto it = hdrFboDepthRB.find(fbo);
            if (it != hdrFboDepthRB.end()) {
                GLuint rb = it->second;
                if (rb) glDeleteRenderbuffers(1, &rb);
                hdrFboDepthRB.erase(it);
            }
            GLuint f = fbo;
            glDeleteFramebuffers(1, &f);
        }
        if (tex) {
            GLuint t = tex;
            glDeleteTextures(1, &t);
        }
    }

    /// ACES tonemap 全屏 pass: HDR 纹理 → 当前绑定 framebuffer
    /// Phase E.3.4: 支持 4 个 operator (ACES / Reinhard / Uncharted2 / Linear)
    /// 调用方负责: 先 UnbindFBO() 切到 default fb, 调用后不需要恢复 depth/blend state.
    void DrawTonemapFullscreen(uint32_t hdrTex, float exposure, float gamma,
                                int tonemapMode = 0) override {
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

        // 3. 绑 HDR 纹理到 texture unit 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdrTex);

        // 4. 绑 VAO + draw 6 顶点
        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // 5. 解绑 (不恢复 depth / blend: 下次 BeginFrame 会重置)
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
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
                              int w, int h, float threshold) override {
        if (!bloomSupported || !sceneTex || !outFbo) return;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)outFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        glUseProgram(programBloomBright);
        if (locBloomBright_Threshold >= 0) glUniform1f(locBloomBright_Threshold, threshold);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)sceneTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Downsample: srcTex → dstFbo (13-tap COD AW)
    void DrawBloomDownsample(uint32_t srcTex, uint32_t dstFbo,
                              int dstW, int dstH) override {
        if (!bloomSupported || !srcTex || !dstFbo || dstW <= 0 || dstH <= 0) return;

        // uTexel = 1.0 / srcSize. 注: downsample 的 src 是上一级, 大小 = dst * 2
        float texelX = 1.0f / (float)(dstW * 2);
        float texelY = 1.0f / (float)(dstH * 2);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        glUseProgram(programBloomDown);
        if (locBloomDown_Texel >= 0) glUniform2f(locBloomDown_Texel, texelX, texelY);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Upsample + additive blend: srcTex → dstFbo (tent 3x3, hardware blend)
    void DrawBloomUpsample(uint32_t srcTex, uint32_t dstFbo,
                            int dstW, int dstH, float radius) override {
        if (!bloomSupported || !srcTex || !dstFbo || dstW <= 0 || dstH <= 0) return;

        // uTexel = 1.0 / dstSize (tent 采样在 dst 空间进行, 扩散半径相对 dst 像素)
        float texelX = 1.0f / (float)dstW;
        float texelY = 1.0f / (float)dstH;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dstFbo);
        glViewport(0, 0, dstW, dstH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        // additive blend: dst = dst + src (hardware blend 加速)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        glUseProgram(programBloomUp);
        if (locBloomUp_Texel     >= 0) glUniform2f(locBloomUp_Texel, texelX, texelY);
        if (locBloomUp_Radius    >= 0) glUniform1f(locBloomUp_Radius, radius);
        if (locBloomUp_Intensity >= 0) glUniform1f(locBloomUp_Intensity, 1.0f);  // upsample 不缩放

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)srcTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// Final composite: bloomTex additive blend → hdrFbo (intensity 缩放)
    /// 复用 upsample shader, radius=0 相当于零偏移采样 (tent 中心权重 16/16 = 全权)
    void DrawBloomComposite(uint32_t bloomTex, uint32_t hdrFbo,
                             int w, int h, float intensity) override {
        if (!bloomSupported || !bloomTex || !hdrFbo || w <= 0 || h <= 0) return;

        float texelX = 1.0f / (float)w;
        float texelY = 1.0f / (float)h;

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hdrFbo);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);

        glUseProgram(programBloomUp);
        if (locBloomUp_Texel     >= 0) glUniform2f(locBloomUp_Texel, texelX, texelY);
        if (locBloomUp_Radius    >= 0) glUniform1f(locBloomUp_Radius, 0.0f);       // 0 偏移 = 零扩散
        if (locBloomUp_Intensity >= 0) glUniform1f(locBloomUp_Intensity, intensity);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)bloomTex);

        glBindVertexArray(vaoTonemap);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glDisable(GL_BLEND);
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
    void BlitHDRDepthToSSAO(uint32_t hdrFbo, uint32_t ssaoDepthFbo, int w, int h) override {
        if (!ssaoSupported || !hdrFbo || !ssaoDepthFbo || w <= 0 || h <= 0) return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)hdrFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)ssaoDepthFbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
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

    /// SSAO raw pass: depthTex + noiseTex + uniforms -> dstFbo (R16F)
    void DrawSSAO(uint32_t depthTex, uint32_t noiseTex, uint32_t dstFbo,
                  int w, int h,
                  const float* projMat4, const float* invProjMat4,
                  const float* kernel, int kernelSize,
                  float radius, float bias, float power) override {
        if (!ssaoSupported || !depthTex || !noiseTex || !dstFbo
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
