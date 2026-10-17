#pragma once
// 4x4 float matrices in the engine's convention: row-major storage, row vectors (p' = p * M), rigid transforms
// store basis rows X, Y, Z and the translation in row 3 (CAMERA_FINDINGS.md, verified to 1e-5 live).
#include <cmath>

namespace gowvr {

struct Mat4 {
    float m[4][4];
};

inline Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Inverse of a rigid transform (orthonormal 3x3 basis rows + translation row).
inline Mat4 RigidInverse(const Mat4& c) {
    Mat4 v{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) v.m[i][j] = c.m[j][i];
    for (int j = 0; j < 3; ++j)
        v.m[3][j] = -(c.m[3][0] * v.m[0][j] + c.m[3][1] * v.m[1][j] + c.m[3][2] * v.m[2][j]);
    v.m[0][3] = v.m[1][3] = v.m[2][3] = 0.f;
    v.m[3][3] = 1.f;
    return v;
}

// D-090: removes shear from the 3x3 basis rows (Gram-Schmidt from row 0) keeping each row's length (the joint's scale)
// and the basis' handedness (mirrored rig joints have a negative determinant). A component-wise blend of two rotations
// is not a rotation (sheared, shortened rows); this makes it one again.
inline void OrthonormalizeKeepScale(Mat4& m) {
    float r[3][3], len[3];
    for (int i = 0; i < 3; ++i) {
        len[i] = sqrtf(m.m[i][0] * m.m[i][0] + m.m[i][1] * m.m[i][1] + m.m[i][2] * m.m[i][2]);
        for (int j = 0; j < 3; ++j) r[i][j] = m.m[i][j];
    }
    if (len[0] < 1e-8f || len[1] < 1e-8f || len[2] < 1e-8f) return;
    const float hand = (r[0][1] * r[1][2] - r[0][2] * r[1][1]) * r[2][0] + (r[0][2] * r[1][0] - r[0][0] * r[1][2]) * r[2][1] +
                       (r[0][0] * r[1][1] - r[0][1] * r[1][0]) * r[2][2];  // det sign
    float x[3] = {r[0][0] / len[0], r[0][1] / len[0], r[0][2] / len[0]};
    const float d = r[1][0] * x[0] + r[1][1] * x[1] + r[1][2] * x[2];
    float y[3] = {r[1][0] - x[0] * d, r[1][1] - x[1] * d, r[1][2] - x[2] * d};
    const float ly = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (ly < 1e-8f) return;
    for (int j = 0; j < 3; ++j) y[j] /= ly;
    float z[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    if (hand < 0.f) for (int j = 0; j < 3; ++j) z[j] = -z[j];
    for (int j = 0; j < 3; ++j) {
        m.m[0][j] = x[j] * len[0];
        m.m[1][j] = y[j] * len[1];
        m.m[2][j] = z[j] * len[2];
    }
}

// D-090: inverse of an affine transform (any invertible 3x3 basis + translation row, row vectors). RigidInverse (the
// transpose) is only the inverse of a pure rotation; for a scaled / sheared basis it leaves a residual turn and shear.
inline Mat4 AffineInverse(const Mat4& c) {
    const float a = c.m[0][0], b = c.m[0][1], e = c.m[0][2], d = c.m[1][0], f = c.m[1][1], g = c.m[1][2], h = c.m[2][0],
                i = c.m[2][1], k = c.m[2][2];
    const float co00 = f * k - g * i, co01 = -(d * k - g * h), co02 = d * i - f * h;
    const float det = a * co00 + b * co01 + e * co02;
    if (fabsf(det) < 1e-12f) return RigidInverse(c);
    const float s = 1.f / det;
    Mat4 v{};
    v.m[0][0] = co00 * s;               v.m[0][1] = -(b * k - e * i) * s;  v.m[0][2] = (b * g - e * f) * s;
    v.m[1][0] = co01 * s;               v.m[1][1] = (a * k - e * h) * s;   v.m[1][2] = -(a * g - e * d) * s;
    v.m[2][0] = co02 * s;               v.m[2][1] = -(a * i - b * h) * s;  v.m[2][2] = (a * f - b * d) * s;
    for (int j = 0; j < 3; ++j)
        v.m[3][j] = -(c.m[3][0] * v.m[0][j] + c.m[3][1] * v.m[1][j] + c.m[3][2] * v.m[2][j]);
    v.m[3][3] = 1.f;
    return v;
}

// Rotation about the local Y (up) axis by `rad`, as a row-vector matrix to pre-multiply a camera->world transform.
inline Mat4 LocalYaw(float rad) {
    float c = cosf(rad), s = sinf(rad);
    Mat4 r{};
    r.m[0][0] = c;  r.m[0][2] = -s;
    r.m[1][1] = 1.f;
    r.m[2][0] = s;  r.m[2][2] = c;
    r.m[3][3] = 1.f;
    return r;
}

// Translation along the local X (right) axis, pre-multiplied onto camera->world.
inline Mat4 LocalOffsetX(float x) {
    Mat4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
    r.m[3][0] = x;
    return r;
}

}  // namespace gowvr
