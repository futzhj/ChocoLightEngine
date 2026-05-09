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

// Phase AS.2 — 透视投影矩阵 (列主序, 与 Ortho 一致)
// fovY: 垂直视野角度 (度); aspect: w/h; n/f: 近远裁剪面 (>0)
Mat4 Mat4::Perspective(float fovYDeg, float aspect, float n, float f) {
    Mat4 r;
    memset(r.m, 0, sizeof(r.m));
    float fovYRad = fovYDeg * (float)M_PI / 180.0f;
    float t = 1.0f / tanf(fovYRad * 0.5f);  // cot(fovY/2)
    if (aspect <= 0.0f) aspect = 1.0f;
    if (n <= 0.0f) n = 0.1f;
    if (f <= n) f = n + 1.0f;

    r.m[0]  = t / aspect;
    r.m[5]  = t;
    r.m[10] = -(f + n) / (f - n);
    r.m[11] = -1.0f;
    r.m[14] = -(2.0f * f * n) / (f - n);
    // r.m[15] = 0 (透视除法用)
    return r;
}

// Phase AS.2 — LookAt 视图矩阵 (右手坐标系, OpenGL 约定)
// eye: 摄像机世界坐标; target: 注视点; up: 上方向 (通常 (0,1,0))
Mat4 Mat4::LookAt(float ex, float ey, float ez,
                  float tx, float ty, float tz,
                  float ux, float uy, float uz) {
    // forward = normalize(target - eye)
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) return Identity();
    fx /= fl; fy /= fl; fz /= fl;

    // right = normalize(forward × up)
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-6f) return Identity();
    rx /= rl; ry /= rl; rz /= rl;

    // up' = right × forward (重新正交化)
    float upx = ry * fz - rz * fy;
    float upy = rz * fx - rx * fz;
    float upz = rx * fy - ry * fx;

    // 列主序填充 view = R^T * T(-eye)
    Mat4 m;
    m.m[0]  = rx;     m.m[4]  = ry;     m.m[8]  = rz;     m.m[12] = -(rx * ex + ry * ey + rz * ez);
    m.m[1]  = upx;    m.m[5]  = upy;    m.m[9]  = upz;    m.m[13] = -(upx * ex + upy * ey + upz * ez);
    m.m[2]  = -fx;    m.m[6]  = -fy;    m.m[10] = -fz;    m.m[14] = (fx * ex + fy * ey + fz * ez);
    m.m[3]  = 0;      m.m[7]  = 0;      m.m[11] = 0;      m.m[15] = 1.0f;
    return m;
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
