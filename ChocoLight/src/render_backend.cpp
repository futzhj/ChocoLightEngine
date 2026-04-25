/**
 * @file render_backend.cpp
 * @brief Mat4 矩阵运算实现 + 工厂函数
 */

#include "render_backend.h"
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ==================== Mat4 实现 ====================

Mat4 Mat4::Identity() {
    Mat4 r;
    memset(r.m, 0, sizeof(r.m));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4::Ortho(float left, float right, float bottom, float top, float near, float far) {
    Mat4 r = Identity();
    r.m[0]  =  2.0f / (right - left);
    r.m[5]  =  2.0f / (top - bottom);
    r.m[10] = -2.0f / (far - near);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(far + near) / (far - near);
    return r;
}

Mat4 Mat4::Translate(float x, float y, float z) {
    Mat4 r = Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 Mat4::Rotate(float angleDeg, float ax, float ay, float az) {
    Mat4 r = Identity();
    float rad = angleDeg * (float)M_PI / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    // 归一化轴
    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) return r;
    ax /= len; ay /= len; az /= len;

    float t = 1.0f - c;
    r.m[0]  = t * ax * ax + c;
    r.m[1]  = t * ax * ay + s * az;
    r.m[2]  = t * ax * az - s * ay;
    r.m[4]  = t * ax * ay - s * az;
    r.m[5]  = t * ay * ay + c;
    r.m[6]  = t * ay * az + s * ax;
    r.m[8]  = t * ax * az + s * ay;
    r.m[9]  = t * ay * az - s * ax;
    r.m[10] = t * az * az + c;
    return r;
}

Mat4 Mat4::Scale(float sx, float sy, float sz) {
    Mat4 r = Identity();
    r.m[0] = sx;
    r.m[5] = sy;
    r.m[10] = sz;
    return r;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += m[k * 4 + row] * o.m[col * 4 + k];
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

// ==================== 全局渲染后端 ====================

RenderBackend* g_render = nullptr;
