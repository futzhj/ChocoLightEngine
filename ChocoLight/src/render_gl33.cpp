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
    }

    bool SupportsLit2D() const override { return lit2DSupported; }

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
