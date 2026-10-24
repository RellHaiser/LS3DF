// Exported math functions of LS3DF.dll (0x1002c450 - 0x10030990).
//
// Every expression keeps the original's evaluation order, taken from the x87 code (tools/x87expr.py);
// see ls3d_math.h for why that makes the results identical.
//
// Memory-safety fix: the original's S_vector::operator*(const S_matrix &), operator*(const
// S_matrix3 &), RotateByMatrix, RotateByNormMatrix, S_quat::Inverse and S_plane::IntersectionLine
// write their S_vector result through a 16-byte temporary, so they store 4 bytes past the 12-byte
// S_vector (uninitialised stack, or the quaternion's scalar for Inverse). The port writes 12 bytes.
#include <cstdlib>

#include "math/ls3d_math.h"

using ls3d::kEpsilon;
using ls3d::MagnitudeOfSquare;

const S_matrix kIdentityMatrix = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};     // 0x1009cd20
const S_matrix3 kIdentityMatrix3 = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};                 // 0x1009cd60

namespace {

constexpr float kPi = 3.14159274f;          // 0x40490fdb
constexpr float kTwoPi = 6.28318548f;       // 0x40c90fdb

float Sin(float x) { return static_cast<float>(std::sin(static_cast<double>(x))); }
float Cos(float x) { return static_cast<float>(std::cos(static_cast<double>(x))); }

// The SDK's inline S_matrix product a * b (affine: column 3 of the result is 0, 0, 0, 1). The
// translation row adds b's translation right after the first product.
S_matrix Multiply(const S_matrix &a, const S_matrix &b)
{
    S_matrix r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        r.m[i][3] = 0.0f;
    }
    for (int j = 0; j < 3; ++j)
        r.m[3][j] = a.m[3][0] * b.m[0][j] + b.m[3][j] + a.m[3][1] * b.m[1][j] + a.m[3][2] * b.m[2][j];
    r.m[3][3] = 1.0f;
    return r;
}

// Z rotation as the inline code builds it (identity with the 2x2 block set).
S_matrix ZRotation(float angle)
{
    S_matrix r = kIdentityMatrix;
    const float c = Cos(angle), s = Sin(angle);
    r.m[0][0] = c;
    r.m[0][1] = s;
    r.m[1][0] = -s;
    r.m[1][1] = c;
    return r;
}

// The 3x3 rotation of a quaternion, shared by SetRot3, SetInvRot3 and RotationMatrix
// (rot[i][j] goes to m[i][j], or m[j][i] when transposed).
void QuatToRotation(const S_quat &q, float rot[3][3])
{
    const float x = q.v.x, y = q.v.y, z = q.v.z, w = q.s;
    const float z2 = z + z;
    const float xx = (x + x) * x;
    const float xy = (y + y) * x;
    const float xz = z2 * x;
    const float yy = (y + y) * y;
    const float yz = z2 * y;
    const float zz = z2 * z;
    const float wx = (x + x) * w;
    rot[0][0] = 1.0f - (zz + yy);
    rot[0][1] = xy - z2 * w;
    rot[0][2] = (y + y) * w + xz;
    rot[1][0] = z2 * w + xy;
    rot[1][1] = 1.0f - (zz + xx);
    rot[1][2] = yz - wx;
    rot[2][0] = xz - (y + y) * w;
    rot[2][1] = wx + yz;
    rot[2][2] = 1.0f - (yy + xx);
}

// The unit vector the inline code derives from `v` with the SDK Normalize rules (in place of
// S_vector::Normalize where the source vector is kept separately).
S_vector Normalized(const S_vector &v, float sq)
{
    S_vector n = v;
    if (std::fabs(sq - 1.0f) < kEpsilon)
        return n;
    if (!(sq < kEpsilon)) {
        const float inv = 1.0f / std::sqrt(sq);
        return S_vector(inv * v.x, inv * v.y, inv * v.z);
    }
    if (v.x != 0.0f)
        n.x = v.x < 0.0f ? -1.0f : 1.0f;
    else if (v.z != 0.0f)
        n.z = v.z < 0.0f ? -1.0f : 1.0f;
    else
        n.y = v.y < 0.0f ? -1.0f : 1.0f;
    return n;
}

}  // namespace

// 0x1002c450
extern "C" float RandFloat()
{
    return static_cast<float>(std::rand()) * 3.05185094e-05f;
}

// 0x1002c470
S_matrix __stdcall RotateXMatrix(float angle)
{
    S_matrix r = kIdentityMatrix;
    const float c = Cos(angle), s = Sin(angle);
    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
}

// 0x1002c4d0
S_matrix __stdcall RotateYMatrix(float angle)
{
    S_matrix r = kIdentityMatrix;
    const float c = Cos(angle), s = Sin(angle);
    r.m[0][0] = c;
    r.m[0][2] = s;
    r.m[2][0] = -s;
    r.m[2][2] = c;
    return r;
}

// 0x1002c530
S_matrix __stdcall RotateZMatrix(float angle)
{
    return ZRotation(angle);
}

// 0x1002c590
float __stdcall S_vector2::AngleTo(const S_vector2 &v) const
{
    const float len = std::sqrt((v.x * v.x + v.y * v.y) * (x * x + y * y));
    if (len < kEpsilon)
        return 0.0f;
    return ls3d::Acos((v.x * x + v.y * y) / len);
}

// 0x1002c600
void __stdcall S_matrix::Identity()
{
    *this = kIdentityMatrix;
}

// 0x1002c620 (reads `src` after writing parts of the result, as the original: no in-place use).
bool __stdcall S_matrix::Inverse(const S_matrix &src)
{
    const float(&s)[4][4] = src.m;
    m[0][0] = s[1][1] * s[2][2] - s[2][1] * s[1][2];
    m[1][0] = s[2][2] * s[1][0] - s[2][0] * s[1][2];
    m[2][0] = s[2][1] * s[1][0] - s[1][1] * s[2][0];
    const float det = (s[0][0] * m[0][0] - m[1][0] * s[0][1]) + m[2][0] * s[0][2];
    if (std::fabs(det) < kEpsilon) {
        *this = kIdentityMatrix;
        return false;
    }
    const float inv = 1.0f / det;
    m[0][0] = inv * m[0][0];
    m[0][1] = -((s[2][2] * s[0][1] - s[2][1] * s[0][2]) * inv);
    m[0][2] = (s[1][2] * s[0][1] - s[0][2] * s[1][1]) * inv;
    m[0][3] = 0.0f;
    m[1][0] = -(inv * m[1][0]);
    m[1][1] = (s[0][0] * s[2][2] - s[0][2] * s[2][0]) * inv;
    m[1][2] = -((s[0][0] * s[1][2] - s[0][2] * s[1][0]) * inv);
    m[1][3] = 0.0f;
    m[2][0] = inv * m[2][0];
    m[2][1] = -((s[2][1] * s[0][0] - s[2][0] * s[0][1]) * inv);
    m[2][2] = (s[1][1] * s[0][0] - s[1][0] * s[0][1]) * inv;
    m[2][3] = 0.0f;
    m[3][0] = -((s[3][2] * m[2][0] + m[0][0] * s[3][0]) + m[1][0] * s[3][1]);
    m[3][1] = -((m[2][1] * s[3][2] + s[3][0] * m[0][1]) + s[3][1] * m[1][1]);
    m[3][2] = -((s[3][2] * m[2][2] + s[3][1] * m[1][2]) + s[3][0] * m[0][2]);
    m[3][3] = 1.0f;
    return true;
}

// 0x1002c7a0
void __stdcall S_matrix::SetDir(const S_vector &dir)
{
    SetDir3(dir);
    ClearTranslation();
}

// 0x1002c7d0: rotation looking along `dir`, with the up vector as close to world y as possible.
void __stdcall S_matrix::SetDir3(const S_vector &dir)
{
    S_vector &x = (*this)(0), &y = (*this)(1), &z = (*this)(2);
    z = dir;
    const float sq = dir.z * dir.z + dir.y * dir.y + dir.x * dir.x;
    if (std::fabs(sq - 1.0f) < kEpsilon) {
        z = dir;
    } else if (!(sq < kEpsilon)) {
        const float inv = 1.0f / std::sqrt(sq);
        z = S_vector(inv * dir.x, inv * dir.y, inv * dir.z);
    } else if (dir.x != 0.0f) {
        z.x = dir.x < 0.0f ? -1.0f : 1.0f;
    } else if (dir.z != 0.0f) {
        z.z = dir.z < 0.0f ? -1.0f : 1.0f;
    } else {
        z.y = dir.y < 0.0f ? -1.0f : 1.0f;
    }
    // World y without its part along z; world z when z is (nearly) vertical; world x at last.
    S_vector p(z.x * z.y, z.y * z.y, z.z * z.y);
    S_vector up(-p.x, 1.0f - p.y, -p.z);
    if (up.y * up.y + up.z * up.z + up.x * up.x < kEpsilon) {
        p = S_vector(z.x * z.z, z.y * z.z, z.z * z.z);
        up = S_vector(-p.x, -p.y, 1.0f - p.z);
        if (up.x * up.x + up.z * up.z + up.y * up.y < kEpsilon)
            up = S_vector(1.0f, 0.0f, 0.0f);
    }
    y.SetNormalized(up);
    x = S_vector(y.y * z.z - y.z * z.y, y.z * z.x - y.x * z.z, y.x * z.y - y.y * z.x);
}

// 0x1002cad0
void __stdcall S_matrix::SetDir(const S_vector &dir, float roll)
{
    SetDir3(dir);
    if (std::fabs(roll) > kEpsilon)
        *this = Multiply(ZRotation(roll), *this);
    ClearTranslation();
}

// 0x1002cce0
void __stdcall S_matrix::SetDir3(const S_vector &dir, float roll)
{
    SetDir3(dir);
    if (std::fabs(roll) > kEpsilon)
        *this = Multiply(ZRotation(roll), *this);
}

// 0x1002ced0
void __stdcall S_matrix::SetDir(const S_vector &dir, const S_vector &up)
{
    SetDir3(dir, up);
    ClearTranslation();
}

// 0x1002cf10: rotation looking along `dir` with `up` (made orthogonal to it) as the y axis.
void __stdcall S_matrix::SetDir3(const S_vector &dir, const S_vector &up)
{
    S_vector &x = (*this)(0), &y = (*this)(1), &z = (*this)(2);
    z = dir;
    const float sq = dir.z * dir.z + dir.y * dir.y + dir.x * dir.x;
    if (std::fabs(sq - 1.0f) < kEpsilon) {
        z = dir;
    } else if (!(sq < kEpsilon)) {
        const float inv = 1.0f / std::sqrt(sq);
        z = S_vector(inv * dir.x, inv * dir.y, inv * dir.z);
    } else if (dir.x != 0.0f) {
        z.x = dir.x < 0.0f ? -1.0f : 1.0f;
    } else if (dir.z != 0.0f) {
        z.z = dir.z < 0.0f ? -1.0f : 1.0f;
    } else {
        z.y = dir.y < 0.0f ? -1.0f : 1.0f;
    }
    const float d = (z.y * up.y + z.z * up.z) + up.x * z.x;
    S_vector p(d * z.x, d * z.y, d * z.z);
    S_vector u(up.x - p.x, up.y - p.y, up.z - p.z);
    if (u.z * u.z + u.y * u.y + u.x * u.x < kEpsilon) {
        p = S_vector(z.x * z.y, z.y * z.y, z.z * z.y);
        u = S_vector(-p.x, 1.0f - p.y, -p.z);
        if (u.x * u.x + u.y * u.y + u.z * u.z < kEpsilon) {
            p = S_vector(z.z * z.x, z.y * z.z, z.z * z.z);
            u = S_vector(-p.x, -p.y, 1.0f - p.z);
            if (u.Square() < kEpsilon)
                u = S_vector(1.0f, 0.0f, 0.0f);
        }
    }
    y.SetNormalized(u);
    x = y.Cross(z);
}

// 0x1002d260
void __stdcall S_matrix::SetRot(const S_quat &q)
{
    SetRot3(q);
    ClearTranslation();
}

// 0x1002d290
void __stdcall S_matrix::SetInvRot(const S_quat &q)
{
    SetInvRot3(q);
    ClearTranslation();
}

// 0x1002d2c0: the rotation part of `src` with its columns normalized.
void __stdcall S_matrix::SetRot(const S_matrix &src)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            m[i][j] = src.m[i][j];
    for (int c = 0; c < 3; ++c) {
        const float sq = (m[0][c] * m[0][c] + m[1][c] * m[1][c]) + m[2][c] * m[2][c];
        if (std::fabs(sq - 1.0f) > kEpsilon) {
            const float inv = 1.0f / std::sqrt(sq);
            m[0][c] = inv * m[0][c];
            m[1][c] = inv * m[1][c];
            m[2][c] = inv * m[2][c];
        }
    }
    ClearTranslation();
}

// 0x1002d450 (only the 3x3 part is written)
void __stdcall S_matrix::SetRot3(const S_quat &q)
{
    if (std::fabs(q.s) >= 1.0f) {
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f;
        return;
    }
    float rot[3][3];
    QuatToRotation(q, rot);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = rot[i][j];
}

// 0x1002d580: the transposed rotation.
void __stdcall S_matrix::SetInvRot3(const S_quat &q)
{
    if (std::fabs(q.s) >= 1.0f) {
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f;
        return;
    }
    float rot[3][3];
    QuatToRotation(q, rot);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[j][i] = rot[i][j];
}

// 0x1002d6b0
S_matrix &__stdcall S_matrix::operator*=(const S_matrix &b)
{
    *this = Multiply(*this, b);
    return *this;
}

// 0x1002d830
S_matrix __stdcall S_matrix::operator*(const S_matrix &b) const
{
    return Multiply(*this, b);
}

// 0x1002d9b0: like operator* without b's translation (the translation row is rotated only).
S_matrix __stdcall S_matrix::RotateByMatrix(const S_matrix &b) const
{
    S_matrix r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
        r.m[i][3] = i == 3 ? 1.0f : 0.0f;
    }
    return r;
}

// 0x1002db20: lengths of the three axes.
S_vector __stdcall S_matrix::GetScale() const
{
    const float sz = m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2];
    const float sy = m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2];
    const float sx = (m[0][2] * m[0][2] + m[0][0] * m[0][0]) + m[0][1] * m[0][1];
    return S_vector(MagnitudeOfSquare(sx), MagnitudeOfSquare(sy), MagnitudeOfSquare(sz));
}

// 0x1002dc90: squared lengths of the three axes.
S_vector __stdcall S_matrix::GetScale2() const
{
    return S_vector(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2],
                    m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2],
                    m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2]);
}

// 0x1002dd00: mean length of the three axes.
float __stdcall S_matrix::GetUScale() const
{
    const float sx = MagnitudeOfSquare((m[0][2] * m[0][2] + m[0][0] * m[0][0]) + m[0][1] * m[0][1]);
    const float sy = MagnitudeOfSquare(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
    const float sz = MagnitudeOfSquare(m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2]);
    return (sz + sy + sx) * 0.333333343f;
}

// 0x1002de90
void __stdcall S_matrix3::Identity()
{
    *this = kIdentityMatrix3;
}

// 0x1002deb0
bool __stdcall S_matrix3::Inverse(const S_matrix3 &src)
{
    const float(&s)[3][4] = src.m;
    m[0][0] = s[1][1] * s[2][2] - s[2][1] * s[1][2];
    m[1][0] = s[2][2] * s[1][0] - s[2][0] * s[1][2];
    m[2][0] = s[2][1] * s[1][0] - s[1][1] * s[2][0];
    const float det = (s[0][0] * m[0][0] - m[1][0] * s[0][1]) + m[2][0] * s[0][2];
    if (std::fabs(det) < kEpsilon) {
        *this = kIdentityMatrix3;
        return false;
    }
    const float inv = 1.0f / det;
    m[0][0] = inv * m[0][0];
    m[0][1] = -((s[2][2] * s[0][1] - s[2][1] * s[0][2]) * inv);
    m[0][2] = (s[1][2] * s[0][1] - s[0][2] * s[1][1]) * inv;
    m[1][0] = -(inv * m[1][0]);
    m[1][1] = (s[0][0] * s[2][2] - s[0][2] * s[2][0]) * inv;
    m[1][2] = -((s[0][0] * s[1][2] - s[0][2] * s[1][0]) * inv);
    m[2][0] = inv * m[2][0];
    m[2][1] = -((s[2][1] * s[0][0] - s[2][0] * s[0][1]) * inv);
    m[2][2] = (s[1][1] * s[0][0] - s[1][0] * s[0][1]) * inv;
    m[0][3] = -((m[0][0] * s[0][3] + m[2][0] * s[2][3]) + m[1][0] * s[1][3]);
    m[1][3] = -((m[2][1] * s[2][3] + m[1][1] * s[1][3]) + m[0][1] * s[0][3]);
    m[2][3] = -((s[0][3] * m[0][2] + m[2][2] * s[2][3]) + m[1][2] * s[1][3]);
    return true;
}

namespace {

// 0x1002e090: S_matrix3 product (the translation column is a row vector moved by b).
S_matrix3 Multiply3(const S_matrix3 &a, const S_matrix3 &b)
{
    S_matrix3 r;
    r.m[0][0] = (a.m[0][0] * b.m[0][0] + a.m[0][1] * b.m[1][0]) + a.m[0][2] * b.m[2][0];
    r.m[0][1] = (a.m[0][1] * b.m[1][1] + b.m[0][1] * a.m[0][0]) + b.m[2][1] * a.m[0][2];
    r.m[0][2] = (a.m[0][1] * b.m[1][2] + b.m[0][2] * a.m[0][0]) + b.m[2][2] * a.m[0][2];
    r.m[1][0] = (a.m[1][2] * b.m[2][0] + a.m[1][1] * b.m[1][0]) + a.m[1][0] * b.m[0][0];
    r.m[1][1] = (a.m[1][2] * b.m[2][1] + a.m[1][1] * b.m[1][1]) + a.m[1][0] * b.m[0][1];
    r.m[1][2] = (a.m[1][2] * b.m[2][2] + a.m[1][1] * b.m[1][2]) + a.m[1][0] * b.m[0][2];
    r.m[2][0] = (a.m[2][0] * b.m[0][0] + a.m[2][2] * b.m[2][0]) + a.m[2][1] * b.m[1][0];
    r.m[2][1] = (b.m[2][1] * a.m[2][2] + b.m[1][1] * a.m[2][1]) + b.m[0][1] * a.m[2][0];
    r.m[2][2] = (b.m[2][2] * a.m[2][2] + b.m[1][2] * a.m[2][1]) + b.m[0][2] * a.m[2][0];
    for (int j = 0; j < 3; ++j)
        r.m[j][3] = ((a.m[2][3] * b.m[2][j] + a.m[1][3] * b.m[1][j]) + a.m[0][3] * b.m[0][j]) + b.m[j][3];
    return r;
}

}  // namespace

// 0x1002e020
S_matrix3 &__stdcall S_matrix3::operator*=(const S_matrix3 &b)
{
    *this = Multiply3(*this, b);
    return *this;
}

// 0x1002e050
S_matrix3 __stdcall S_matrix3::operator*(const S_matrix3 &b) const
{
    return Multiply3(*this, b);
}

// 0x1002e1d0
S_matrix4 __stdcall S_matrix4::Mult4X4(const S_matrix4 &b) const
{
    S_matrix4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = (m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j]) + (m[i][1] * b.m[1][j] + m[i][0] * b.m[0][j]);
    return r;
}

// 0x1002e830: cosine of HALF the angle between the vectors (sqrt((cos + 1) / 2)).
float __stdcall S_vector::CosAngleTo(const S_vector &v) const
{
    const float dot = (v.z * z + v.y * y) + v.x * x;
    const float len = std::sqrt(((v.x * v.x + v.y * v.y) + v.z * v.z) * ((x * x + y * y) + z * z));
    return std::sqrt((dot / len + 1.0f) * 0.5f);
}

// 0x1002e8c0
float __stdcall S_vector::AngleTo(const S_vector &v) const
{
    const float a = MagnitudeOfSquare(x * x + y * y + z * z);
    const float b = MagnitudeOfSquare(v.x * v.x + v.y * v.y + v.z * v.z);
    const float len = b * a;
    if (len < kEpsilon)
        return 0.0f;
    const float c = ((v.x * x + v.z * z) + v.y * y) / len;
    if (!(c > -1.0f))
        return ls3d::Acos(-1.0f);
    return ls3d::Acos(c < 1.0f ? c : 1.0f);
}

// 0x1002ea40
S_vector &__stdcall S_vector::operator*=(const S_matrix &m)
{
    const float ox = x, oy = y, oz = z;
    z = ((m.m[2][2] * oz + m.m[0][2] * ox) + m.m[1][2] * oy) + m.m[3][2];
    x = ((m.m[2][0] * oz + m.m[1][0] * oy) + m.m[0][0] * ox) + m.m[3][0];
    y = ((m.m[2][1] * oz + m.m[0][1] * ox) + m.m[1][1] * oy) + m.m[3][1];
    return *this;
}

// 0x1002eab0
S_vector __stdcall S_vector::operator*(const S_matrix &m) const
{
    return S_vector(((x * m.m[0][0] + m.m[3][0]) + y * m.m[1][0]) + z * m.m[2][0],
                    ((x * m.m[0][1] + m.m[3][1]) + y * m.m[1][1]) + z * m.m[2][1],
                    ((x * m.m[0][2] + m.m[3][2]) + y * m.m[1][2]) + z * m.m[2][2]);
}

// 0x1002eb40
S_vector &__stdcall S_vector::operator*=(const S_matrix3 &m)
{
    const float ox = x, oy = y, oz = z;
    z = ((m.m[2][2] * oz + m.m[0][2] * ox) + m.m[1][2] * oy) + m.m[2][3];
    x = ((m.m[2][0] * oz + m.m[1][0] * oy) + m.m[0][0] * ox) + m.m[0][3];
    y = ((m.m[2][1] * oz + m.m[0][1] * ox) + m.m[1][1] * oy) + m.m[1][3];
    return *this;
}

// 0x1002ebb0
S_vector __stdcall S_vector::operator*(const S_matrix3 &m) const
{
    return S_vector(((m.m[2][0] * z + m.m[1][0] * y) + m.m[0][0] * x) + m.m[0][3],
                    ((m.m[2][1] * z + m.m[0][1] * x) + m.m[1][1] * y) + m.m[1][3],
                    ((m.m[2][2] * z + m.m[0][2] * x) + m.m[1][2] * y) + m.m[2][3]);
}

// 0x1002ec80: the vector rotated (and scaled) by the 3x3 part.
S_vector __stdcall S_vector::RotateByMatrix(const S_matrix &m) const
{
    return S_vector((x * m.m[0][0] + y * m.m[1][0]) + z * m.m[2][0],
                    (x * m.m[0][1] + y * m.m[1][1]) + z * m.m[2][1],
                    (x * m.m[0][2] + y * m.m[1][2]) + z * m.m[2][2]);
}

// 0x1002ed10: rotated by the 3x3 part with each component divided by its column's length.
S_vector __stdcall S_vector::RotateByNormMatrix(const S_matrix &m) const
{
    S_vector r = RotateByMatrix(m);
    float *out = &r.x;
    for (int c = 0; c < 3; ++c) {
        const float sq = (m.m[0][c] * m.m[0][c] + m.m[1][c] * m.m[1][c]) + m.m[2][c] * m.m[2][c];
        if (std::fabs(sq - 1.0f) > kEpsilon)
            out[c] = out[c] / std::sqrt(sq);
    }
    return r;
}

// 0x1002ee70
S_matrix __stdcall S_quat::RotationMatrix() const
{
    if (std::fabs(s) >= 1.0f)
        return kIdentityMatrix;
    float rot[3][3];
    QuatToRotation(*this, rot);
    S_matrix r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = rot[i][j];
        r.m[i][3] = 0.0f;
    }
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
    r.m[3][3] = 1.0f;
    return r;
}

// 0x1002efd0: the rotated z axis.
S_vector __stdcall S_quat::GetDir() const
{
    if (std::fabs(s) >= 1.0f)
        return S_vector(0.0f, 0.0f, 1.0f);
    const float xx = (v.x + v.x) * v.x;
    const float yy = (v.y + v.y) * v.y;
    const float zy = (v.z + v.z) * v.y;
    const float xw = (v.x + v.x) * s;
    return S_vector((v.z + v.z) * v.x - (v.y + v.y) * s, xw + zy, 1.0f - (yy + xx));
}

// 0x1002f070: rotation by `angle` around `axis` (normalized afterwards).
void __stdcall S_quat::Make(const S_vector &axis, float angle)
{
    // (FSIN's result stays on the FPU stack: the products are rounded once.)
    const float c = Cos(angle * 0.5f);
    const double sn = std::sin(static_cast<double>(angle * 0.5f));
    const float x = static_cast<float>(sn * axis.x), y = static_cast<float>(sn * axis.y),
                z = static_cast<float>(sn * axis.z);
    v = S_vector(x, y, z);
    s = c;
    const float sq = ((c * c + z * z) + x * x) + y * y;
    if (std::fabs(sq - 1.0f) < kEpsilon)
        return;
    if (sq < kEpsilon) {
        Identity();
        return;
    }
    const float inv = 1.0f / std::sqrt(sq);
    v = S_vector(x * inv, y * inv, z * inv);
    s = c * inv;
}

// 0x1002f170: rotation by the rotation vector `rv` (axis times angle).
void __stdcall S_quat::Make(const S_vector &rv)
{
    const float sq = (rv.x * rv.x + rv.y * rv.y) + rv.z * rv.z;
    if (ls3d::IsTinyBits(sq)) {
        Identity();
        return;
    }
    const float len = std::sqrt(sq);
    const float half = 0.5f * len;
    const float k = static_cast<float>(std::sin(static_cast<double>(half)) / len);
    v = S_vector(k * rv.x, k * rv.y, k * rv.z);
    s = Cos(half);
}

// 0x1002f220: the rotation of a matrix (its axes may be scaled). Degenerate axes are rebuilt from
// the others first; then the rotation is composed of one taking world y to the matrix's y axis and
// a roll around it that takes x + y to the matrix's (normalized) x + y.
void __stdcall S_quat::Make(const S_matrix &m)
{
    const S_vector &r0 = m(0), &r1 = m(1), &r2 = m(2);
    const float sz = r2.Magnitude(), sy = r1.Magnitude(), sx = r0.Magnitude();
    if ((sz + sy) + sx < kEpsilon) {
        Identity();
        return;
    }
    if ((sz * sy) * sx < kEpsilon) {
        S_matrix fixed = m;
        if (sx < kEpsilon) {
            const S_vector n(r2.z * r1.y - r1.z * r2.y, r1.z * r2.x - r2.z * r1.x, r2.y * r1.x - r1.y * r2.x);
            fixed(0) = n;
            if (MagnitudeOfSquare((n.y * n.y + n.z * n.z) + n.x * n.x) < kEpsilon)
                fixed(0) = S_vector(1.0f, 0.0f, 0.0f);
        }
        if (sy < kEpsilon) {
            const S_vector n(r0.y * r2.z - r0.z * r2.y, r0.z * r2.x - r2.z * r0.x, r0.x * r2.y - r0.y * r2.x);
            fixed(1) = n;
            if (MagnitudeOfSquare((n.x * n.x + n.y * n.y) + n.z * n.z) < kEpsilon)
                fixed(1) = S_vector(0.0f, 1.0f, 0.0f);
        }
        if (sz < kEpsilon) {
            const S_vector n(r1.z * r0.y - r0.z * r1.y, r0.z * r1.x - r1.z * r0.x, r1.y * r0.x - r0.y * r1.x);
            fixed(2) = n;
            if (MagnitudeOfSquare((n.x * n.x + n.y * n.y) + n.z * n.z) < kEpsilon)
                fixed(2) = S_vector(0.0f, 0.0f, 1.0f);
        }
        const float fz = fixed(2).Magnitude(), fy = fixed(1).Magnitude(), fx = fixed(0).Magnitude();
        if ((fz * fy) * fx < kEpsilon) {
            Identity();
            return;
        }
        S_quat q;
        q.Make(fixed);
        *this = q;
        return;
    }

    const float rx = 1.0f / sx;
    const float ry = 1.0f / sy;
    const S_vector y(ry * r1.x, ry * r1.y, ry * r1.z);
    const S_vector x(rx * r0.x, rx * r0.y, rx * r0.z);
    const S_vector sum(x.x + y.x, y.y + x.y, y.z + x.z);

    // q1: world y -> y.
    S_quat q1;
    if (!(std::fabs(y.y) >= 1.0f)) {
        const S_vector a(y.z, 0.0f, -y.x);
        const S_vector n = Normalized(a, a.x * a.x + a.z * a.z);
        q1.Make(S_vector(-n.x, -n.y, -n.z), ls3d::Acos(y.y));
    } else {
        q1.v = S_vector(0.0f, 0.0f, y.y > 0.0f ? 0.0f : 1.0f);
        q1.s = y.y > 0.0f ? 1.0f : 0.0f;
    }

    // (1, 1, 0) turned by q1.
    S_vector t;
    if (!(std::fabs(q1.s) >= 1.0f)) {
        const float qx = q1.v.x, qy = q1.v.y, qz = q1.v.z, qw = q1.s;
        const float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
        t.x = ((1.0f - (qy * y2 + qz * z2)) + z2 * qw) + y2 * qx;
        t.y = ((y2 * qx - z2 * qw) + 1.0f) - (qx * x2 + qz * z2);
        t.z = ((qy * z2 + z2 * qx) + y2 * qw) - x2 * qw;
    } else {
        t = S_vector(1.0f, 1.0f, 0.0f);
    }

    // q2: the roll around y that takes t to sum.
    const S_vector d1(t.x - y.x, t.y - y.y, t.z - y.z);
    const S_vector d2(sum.x - y.x, sum.y - y.y, sum.z - y.z);
    const float dot = (d2.z * d1.z + d2.y * d1.y) + d2.x * d1.x;
    S_quat q2;
    if (!(std::fabs(dot) >= 0.99999)) {
        const S_vector c(d2.z * d1.y - d2.y * d1.z, d1.z * d2.x - d2.z * d1.x, d2.y * d1.x - d1.y * d2.x);
        const S_vector n = Normalized(c, (c.x * c.x + c.y * c.y) + c.z * c.z);
        q2.Make(S_vector(-n.x, -n.y, -n.z), ls3d::Acos(dot));
    } else {
        const S_vector c(d1.z * y.y - d1.y * y.z, y.z * d1.x - d1.z * y.x, d1.y * y.x - y.y * d1.x);
        const float triple = (sum.z * c.z + sum.y * c.y) + c.x * sum.x;
        const float angle = dot > 0.0f ? 0.0f : kPi;
        q2.Make(triple > 0.0f ? S_vector(-y.x, -y.y, -y.z) : y, angle);
    }

    // q1 * q2
    const S_vector c = q1.v.Cross(q2.v);
    const S_vector a(q1.v.x * q2.s + q2.v.x * q1.s, q1.v.y * q2.s + q2.v.y * q1.s, q1.v.z * q2.s + q2.v.z * q1.s);
    const float w = q2.s * q1.s;
    v = S_vector(c.x + a.x, c.y + a.y, c.z + a.z);
    s = w - q1.v.Dot(q2.v);
    MakePositive();
}

// 0x1002ff50
void __stdcall S_quat::SetDir(const S_vector &dir, float roll)
{
    S_matrix m;
    m.SetDir3(dir);
    if (std::fabs(roll) > kEpsilon)
        m = Multiply(ZRotation(roll), m);
    Make(m);
}

// 0x10030160: axis and angle (0 .. 2 pi).
void __stdcall S_quat::Inverse(S_vector &axis, float &angle) const
{
    axis = v;
    axis.Normalize();
    if (s >= 1.0f)
        angle = 0.0f;
    else if (s <= -1.0f)
        angle = kTwoPi;
    else
        angle = static_cast<float>(ls3d::AcosD(s) * 2.0);
}

// 0x100302e0 (the original ignores `shortestPath`: it always takes the shorter way).
S_quat __stdcall S_quat::Slerp(const S_quat &q, float t, bool shortestPath) const
{
    (void)shortestPath;
    float cosom = ((q.v.z * v.z + q.v.y * v.y) + q.v.x * v.x) + s * q.s;
    S_quat to = q;
    if (cosom < 0.0f) {
        cosom = -cosom;
        to.v = S_vector(-q.v.x, -q.v.y, -q.v.z);
        to.s = -q.s;
    }
    float k0, k1;
    if (1.0f - cosom > 0.0001) {
        // (The sine of the unrounded angle; the products with `inv` are rounded once.)
        const double omegaD = ls3d::AcosD(cosom);
        const float omega = static_cast<float>(omegaD);
        const float inv = static_cast<float>(1.0 / std::sin(omegaD));
        k0 = static_cast<float>(std::sin(static_cast<double>((1.0f - t) * omega)) * inv);
        k1 = static_cast<float>(std::sin(static_cast<double>(omega * t)) * inv);
    } else {
        k0 = 1.0f - t;
        k1 = t;
    }
    S_quat r;
    r.v.x = k0 * v.x + k1 * to.v.x;
    r.v.y = to.v.y * k1 + k0 * v.y;
    r.v.z = to.v.z * k1 + k0 * v.z;
    r.s = k0 * s + k1 * to.s;
    return r;
}

// 0x10030440: this * (rotation of m).
S_quat __stdcall S_quat::RotateByMatrix(const S_matrix &m) const
{
    S_quat a;
    a.Make(m);
    const S_quat &b = *this;
    S_quat r;
    r.s = a.s * b.s - ((a.v.x * b.v.x + a.v.z * b.v.z) + a.v.y * b.v.y);
    r.v.x = (a.v.z * b.v.y - a.v.y * b.v.z) + (a.v.x * b.s + a.s * b.v.x);
    r.v.y = (a.v.y * b.s + a.s * b.v.y) + (a.v.x * b.v.z - a.v.z * b.v.x);
    r.v.z = (a.v.z * b.s + a.s * b.v.z) + (a.v.y * b.v.x - a.v.x * b.v.y);
    return r;
}

// 0x10030540: unit length with a non-negative scalar.
void __stdcall S_quat::Normalize()
{
    const float sq = ((v.x * v.x + v.y * v.y) + v.z * v.z) + s * s;
    if (sq > kEpsilon) {
        if (std::fabs(sq - 1.0f) < kEpsilon && s < 0.0f) {
            s = -s;
            v.x = -v.x;
            v.y = -v.y;
            v.z = -v.z;
        }
        float inv = 1.0f / std::sqrt(sq);
        if (s < 0.0f)
            inv = -inv;
        v.x = inv * v.x;
        v.y = inv * v.y;
        v.z = inv * v.z;
        s = inv * s;
        return;
    }
    s = 0.0f;
    if (v.x != 0.0f)
        v.x = v.x < 0.0f ? -1.0f : 1.0f;
    else if (v.z != 0.0f)
        v.z = v.z < 0.0f ? -1.0f : 1.0f;
    else
        v.y = v.y < 0.0f ? -1.0f : 1.0f;
}

// 0x100306c0: the line where two planes meet (a point with one coordinate 0, and the unit
// direction); false when they are parallel.
bool __stdcall S_plane::IntersectionLine(const S_plane &o, S_vector &pos, S_vector &dir) const
{
    const S_vector &a = normal, &b = o.normal;
    dir = S_vector(a.y * b.z - b.y * a.z, a.z * b.x - a.x * b.z, b.y * a.x - a.y * b.x);
    const float xx = dir.x * dir.x, yy = dir.y * dir.y, zz = dir.z * dir.z;
    float f;
    if (zz > yy && zz > xx && zz > kEpsilon) {
        f = dir.z;
        pos.x = a.y * o.d - b.y * d;
        pos.y = b.x * d - a.x * o.d;
        pos.z = 0.0f;
    } else if (yy > xx && yy > kEpsilon) {
        f = dir.y;
        pos.y = 0.0f;
        pos.x = b.z * d - a.z * o.d;
        pos.z = a.x * o.d - b.x * d;
    } else if (xx > kEpsilon) {
        f = dir.x;
        pos.x = 0.0f;
        pos.y = a.z * o.d - b.z * d;
        pos.z = b.y * d - a.y * o.d;
    } else {
        return false;
    }
    const float inv = 1.0f / f;
    pos.x = inv * pos.x;
    pos.y = inv * pos.y;
    pos.z = inv * pos.z;
    const float n = 1.0f / std::sqrt((xx + yy) + zz);
    dir.x = n * dir.x;
    dir.y = n * dir.y;
    dir.z = n * dir.z;
    return true;
}

// 0x10030870
bool __stdcall LineLineIntersect(const S_vector &p1, const S_vector &d1, const S_vector &p2, const S_vector &d2,
                                 float &t1, float &t2)
{
    const S_vector w(p1.x - p2.x, p1.y - p2.y, p1.z - p2.z);
    const float dd2 = (d2.x * d2.x + d2.y * d2.y) + d2.z * d2.z;
    const float b = (d1.z * d2.z + d1.y * d2.y) + d1.x * d2.x;
    const float den = ((d1.x * d1.x + d1.y * d1.y) + d1.z * d1.z) * dd2 - b * b;
    if (ls3d::IsTinyBits(std::fabs(den)))
        return false;
    const float e = (w.z * d2.z + w.y * d2.y) + w.x * d2.x;
    const float wd1 = (w.z * d1.z + w.y * d1.y) + w.x * d1.x;
    t1 = (b * e - wd1 * dd2) / den;
    t2 = (t1 * b + e) / dd2;
    return true;
}
