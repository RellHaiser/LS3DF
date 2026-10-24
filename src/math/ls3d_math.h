// LS3D engine math types (S_vector, S_quat, S_matrix, ...).
//
// The layouts are part of the LS3DF.dll ABI: the game passes these structures to the engine and
// reads them back. The non-inline methods are exported by LS3DF.dll as __stdcall member functions
// (decorated "QAG" / "QBG"); everything else is inline, as in the original SDK headers (the
// original DLL has out-of-line copies of some of them, noted with their addresses).
//
// Float expressions keep the original evaluation order (sums are not associative): the engine runs
// with the FPU in single precision (Direct3D's default), so the x87 code of the original computes the
// same values as this float code.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#ifndef LS3D_API
#define LS3D_API __declspec(dllimport)
#endif

struct S_matrix;
struct S_matrix3;
struct S_quat;

namespace ls3d {

// The SDK's "below 1e-8" test on a non-negative float's bits (CMP dword, 0x322bcc77): an integer
// compare, so negative values (sign bit set) also count as small.
inline bool IsTinyBits(float f)
{
    int32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits < 0x322bcc77;
}

constexpr float kEpsilon = 1e-8f;

// Square root of a squared length as the SDK's Magnitude computes it: exact 0 and 1 for (nearly)
// zero and unit lengths.
inline float MagnitudeOfSquare(float sq)
{
    if (IsTinyBits(sq))
        return 0.0f;
    if (std::fabs(sq - 1.0f) < kEpsilon)
        return 1.0f;
    return std::sqrt(sq);
}

// acos as the original's CRT computes it (_CIacos, 0x1008d4e0): atan2(sqrt((1 + x)(1 - x)), x),
// where the sum, product and square root are rounded to the FPU precision, which the engine runs at
// single precision (Direct3D's default). FPATAN itself is exact; AcosD is that value before the
// caller rounds it (some callers go on with the unrounded result).
inline double AcosD(float x)
{
    const float t = std::sqrt((1.0f + x) * (1.0f - x));
    return std::atan2(static_cast<double>(t), static_cast<double>(x));
}
inline float Acos(float x) { return static_cast<float>(AcosD(x)); }

// asin as the original's CRT computes it (_CIasin, 0x1008dbb0): atan2(x, sqrt((1 + x)(1 - x))), the
// same roundings as AcosD.
inline double AsinD(float x)
{
    const float t = std::sqrt((1.0f + x) * (1.0f - x));
    return std::atan2(static_cast<double>(x), static_cast<double>(t));
}
inline float Asin(float x) { return static_cast<float>(AsinD(x)); }

}  // namespace ls3d

struct S_vector2 {
    float x, y;

    LS3D_API float __stdcall AngleTo(const S_vector2 &v) const;
};

struct S_vector {
    float x, y, z;

    S_vector() = default;
    S_vector(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}       // 0x10005a60

    void Zero() { x = y = z = 0.0f; }
    S_vector operator+(const S_vector &v) const { return S_vector(v.x + x, v.y + y, v.z + z); }    // 0x10010930
    S_vector operator-(const S_vector &v) const { return S_vector(x - v.x, y - v.y, z - v.z); }    // 0x10005a80
    S_vector operator-() const { return S_vector(-x, -y, -z); }
    S_vector operator*(float s) const { return S_vector(s * x, y * s, z * s); }                // 0x10010900
    S_vector &operator+=(const S_vector &v) { x += v.x; y += v.y; z += v.z; return *this; }
    S_vector &operator-=(const S_vector &v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    S_vector &operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    bool operator==(const S_vector &v) const { return x == v.x && y == v.y && z == v.z; }
    bool operator!=(const S_vector &v) const { return !(*this == v); }

    // (z * v.z + y * v.y) + x * v.x, as the out-of-line copy 0x1000c680.
    float Dot(const S_vector &v) const { return v.z * z + v.y * y + v.x * x; }
    S_vector Cross(const S_vector &v) const                                  // 0x1002ec40
    {
        return S_vector(y * v.z - z * v.y, z * v.x - v.z * x, x * v.y - y * v.x);
    }
    float Square() const { return x * x + y * y + z * z; }                   // 0x10011d80
    float Magnitude() const { return ls3d::MagnitudeOfSquare(Square()); }    // 0x1002fe90

    // SDK Normalize: a (nearly) zero vector becomes a unit vector along its first non-zero axis
    // (x, then z, then y).
    void Normalize()
    {
        const float sq = x * x + y * y + z * z;
        if (std::fabs(sq - 1.0f) < ls3d::kEpsilon)
            return;
        if (!(sq < ls3d::kEpsilon)) {
            const float inv = 1.0f / std::sqrt(sq);
            x = inv * x;
            y = inv * y;
            z = inv * z;
        } else if (x != 0.0f) {                     // (NaN counts as non-zero and positive)
            x = x < 0.0f ? -1.0f : 1.0f;
        } else if (z != 0.0f) {
            z = z < 0.0f ? -1.0f : 1.0f;
        } else {
            y = y < 0.0f ? -1.0f : 1.0f;
        }
    }
    // Assigns `v` normalized (SDK SetNormalized, 0x10010960; its squared length sums z first).
    void SetNormalized(const S_vector &v)
    {
        *this = v;
        const float sq = v.z * v.z + v.y * v.y + v.x * v.x;
        if (std::fabs(sq - 1.0f) < ls3d::kEpsilon)
            return;
        if (!(sq < ls3d::kEpsilon)) {
            const float inv = 1.0f / std::sqrt(sq);
            x = inv * v.x;
            y = inv * v.y;
            z = inv * v.z;
        } else if (v.x != 0.0f) {
            x = v.x < 0.0f ? -1.0f : 1.0f;
        } else if (v.z != 0.0f) {
            z = v.z < 0.0f ? -1.0f : 1.0f;
        } else {
            y = v.y < 0.0f ? -1.0f : 1.0f;
        }
    }

    LS3D_API float __stdcall AngleTo(const S_vector &v) const;
    LS3D_API float __stdcall CosAngleTo(const S_vector &v) const;
    LS3D_API S_vector __stdcall RotateByMatrix(const S_matrix &m) const;
    LS3D_API S_vector __stdcall RotateByNormMatrix(const S_matrix &m) const;
    LS3D_API S_vector __stdcall operator*(const S_matrix &m) const;
    LS3D_API S_vector &__stdcall operator*=(const S_matrix &m);
    LS3D_API S_vector __stdcall operator*(const S_matrix3 &m) const;
    LS3D_API S_vector &__stdcall operator*=(const S_matrix3 &m);
};

struct S_vector4 {
    float x, y, z, w;
};

namespace ls3d {

// The squared length in the orders the inlined normalizations of the original sum it in (the
// association matters; each function's order follows its disassembly).
inline float SquareXYZ(const S_vector &v) { return (v.x * v.x + v.y * v.y) + v.z * v.z; }
inline float SquareXZY(const S_vector &v) { return (v.x * v.x + v.z * v.z) + v.y * v.y; }
inline float SquareYZX(const S_vector &v) { return (v.y * v.y + v.z * v.z) + v.x * v.x; }
inline float SquareZYX(const S_vector &v) { return (v.z * v.z + v.y * v.y) + v.x * v.x; }
inline float SquareZXY(const S_vector &v) { return (v.z * v.z + v.x * v.x) + v.y * v.y; }

// The SDK's inline normalization of `v` whose squared length is `sq`: unchanged when (nearly)
// unit, else scaled, a (nearly) zero vector becoming a unit vector along its first non-zero axis
// (x, then z, then y).
inline void NormalizeWithSquare(S_vector &v, float sq)
{
    if (std::fabs(sq - 1.0f) < kEpsilon)
        return;
    if (!(sq < kEpsilon)) {
        const float inv = 1.0f / std::sqrt(sq);
        v.x = v.x * inv;
        v.y = v.y * inv;
        v.z = inv * v.z;
    } else if (v.x != 0.0f) {
        v.x = v.x < 0.0f ? -1.0f : 1.0f;
    } else if (v.z != 0.0f) {
        v.z = v.z < 0.0f ? -1.0f : 1.0f;
    } else {
        v.y = v.y < 0.0f ? -1.0f : 1.0f;
    }
}

}  // namespace ls3d

// Quaternion: the vector part first, then the scalar (identity = {0, 0, 0, 1}).
struct S_quat {
    S_vector v;
    float s;

    S_quat() = default;
    S_quat(float s_, const S_vector &v_) : v(v_), s(s_) {}                   // 0x1001edb0

    void Identity() { v.Zero(); s = 1.0f; }
    // Hamilton product this * b, as the SDK's inline operator computes it.
    S_quat operator*(const S_quat &b) const
    {
        const S_vector vec = (v * b.s + b.v * s) + v.Cross(b.v);
        return S_quat(s * b.s - v.Dot(b.v), vec);
    }
    // Flips the sign of all four components when the scalar is negative (0x10030990).
    void MakePositive()
    {
        if (s < 0.0f) {
            s = -s;
            v.x = -v.x;
            v.y = -v.y;
            v.z = -v.z;
        }
    }

    LS3D_API void __stdcall Make(const S_matrix &m);
    LS3D_API void __stdcall Make(const S_vector &axis, float angle);
    LS3D_API void __stdcall Make(const S_vector &rotationVector);
    LS3D_API void __stdcall Normalize();
    LS3D_API void __stdcall SetDir(const S_vector &dir, float roll);
    LS3D_API S_vector __stdcall GetDir() const;
    LS3D_API void __stdcall Inverse(S_vector &axis, float &angle) const;
    LS3D_API S_matrix __stdcall RotationMatrix() const;
    LS3D_API S_quat __stdcall RotateByMatrix(const S_matrix &m) const;
    LS3D_API S_quat __stdcall Slerp(const S_quat &q, float t, bool shortestPath) const;
};

// Row-major 4x4 matrix; row 3 holds the translation (Direct3D convention, row vectors).
struct S_matrix {
    float m[4][4];

    S_vector &operator()(int row) { return *reinterpret_cast<S_vector *>(m[row]); }
    const S_vector &operator()(int row) const { return *reinterpret_cast<const S_vector *>(m[row]); }

    // Clears column 3 and the translation row (what the non-"3" setters do after the rotation).
    void ClearTranslation()
    {
        m[0][3] = 0.0f;
        m[1][3] = 0.0f;
        m[2][3] = 0.0f;
        m[3][0] = 0.0f;
        m[3][1] = 0.0f;
        m[3][2] = 0.0f;
        m[3][3] = 1.0f;
    }

    LS3D_API void __stdcall Identity();
    LS3D_API void __stdcall SetDir(const S_vector &dir);
    LS3D_API void __stdcall SetDir(const S_vector &dir, float roll);
    LS3D_API void __stdcall SetDir(const S_vector &dir, const S_vector &up);
    LS3D_API void __stdcall SetDir3(const S_vector &dir);
    LS3D_API void __stdcall SetDir3(const S_vector &dir, float roll);
    LS3D_API void __stdcall SetDir3(const S_vector &dir, const S_vector &up);
    LS3D_API void __stdcall SetRot(const S_quat &q);
    LS3D_API void __stdcall SetRot(const S_matrix &m);
    LS3D_API void __stdcall SetRot3(const S_quat &q);
    LS3D_API void __stdcall SetInvRot(const S_quat &q);
    LS3D_API void __stdcall SetInvRot3(const S_quat &q);
    LS3D_API bool __stdcall Inverse(const S_matrix &src);
    LS3D_API S_vector __stdcall GetScale() const;
    LS3D_API S_vector __stdcall GetScale2() const;
    LS3D_API float __stdcall GetUScale() const;
    LS3D_API S_matrix __stdcall RotateByMatrix(const S_matrix &m) const;
    LS3D_API S_matrix __stdcall operator*(const S_matrix &m) const;
    LS3D_API S_matrix &__stdcall operator*=(const S_matrix &m);
};

// 3x4 matrix: the 3x3 rotation in columns 0..2, the translation in column 3 (m[row][3]).
struct S_matrix3 {
    float m[3][4];

    LS3D_API void __stdcall Identity();
    LS3D_API bool __stdcall Inverse(const S_matrix3 &src);
    LS3D_API S_matrix3 __stdcall operator*(const S_matrix3 &m) const;
    LS3D_API S_matrix3 &__stdcall operator*=(const S_matrix3 &m);
};

// General 4x4 matrix (projections).
struct S_matrix4 {
    float m[4][4];

    LS3D_API S_matrix4 __stdcall Mult4X4(const S_matrix4 &b) const;
};

struct S_plane {
    S_vector normal;
    float d;

    float Distance(const S_vector &p) const { return normal.Dot(p) + d; }

    LS3D_API bool __stdcall IntersectionLine(const S_plane &other, S_vector &linePos, S_vector &lineDir) const;
};

LS3D_API S_matrix __stdcall RotateXMatrix(float angle);
LS3D_API S_matrix __stdcall RotateYMatrix(float angle);
LS3D_API S_matrix __stdcall RotateZMatrix(float angle);
// Closest points of two lines (p1 + t1 * d1, p2 + t2 * d2); false when they are parallel.
LS3D_API bool __stdcall LineLineIntersect(const S_vector &p1, const S_vector &d1, const S_vector &p2,
                                          const S_vector &d2, float &t1, float &t2);

// a * b as full 4 x 4 matrices into `r` (0x1002e430; the grouping of S_matrix4::Mult4X4). `r`
// must not be one of the inputs.
inline void Multiply4x4(float (&r)[4][4], const float (&a)[4][4], const float (&b)[4][4])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i][j] = (a[i][2] * b[2][j] + a[i][3] * b[3][j]) + (a[i][1] * b[1][j] + a[i][0] * b[0][j]);
}

// a (affine: its column 3 taken as 0, 0, 0, 1) * b into `r` (0x1002e680, the grouping of the
// original). `r` must not be one of the inputs.
inline void MultiplyAffine4x4(float (&r)[4][4], const float (&a)[4][4], const float (&b)[4][4])
{
    for (int j = 0; j < 4; ++j) {
        r[0][j] = j == 0 ? (a[0][0] * b[0][0] + a[0][1] * b[1][0]) + a[0][2] * b[2][0]
                         : (a[0][1] * b[1][j] + b[0][j] * a[0][0]) + b[2][j] * a[0][2];
        r[1][j] = (a[1][2] * b[2][j] + a[1][1] * b[1][j]) + a[1][0] * b[0][j];
        r[2][j] = (a[2][2] * b[2][j] + a[2][1] * b[1][j]) + b[0][j] * a[2][0];
    }
    r[3][0] = ((a[3][0] * b[0][0] + a[3][2] * b[2][0]) + a[3][1] * b[1][0]) + b[3][0];
    for (int j = 1; j < 4; ++j)
        r[3][j] = ((b[2][j] * a[3][2] + b[1][j] * a[3][1]) + b[0][j] * a[3][0]) + b[3][j];
}

// Uniform random number in [0, 1] (rand() / 32767).
extern "C" LS3D_API float RandFloat();

// The identity matrix constants of the original (0x1009cd20, 0x1009cd60).
extern const S_matrix kIdentityMatrix;
extern const S_matrix3 kIdentityMatrix3;

static_assert(sizeof(S_vector2) == 0x08, "S_vector2 layout");
static_assert(sizeof(S_vector) == 0x0C, "S_vector layout");
static_assert(sizeof(S_vector4) == 0x10, "S_vector4 layout");
static_assert(sizeof(S_quat) == 0x10, "S_quat layout");
static_assert(sizeof(S_matrix) == 0x40, "S_matrix layout");
static_assert(sizeof(S_matrix3) == 0x30, "S_matrix3 layout");
static_assert(sizeof(S_matrix4) == 0x40, "S_matrix4 layout");
static_assert(sizeof(S_plane) == 0x10, "S_plane layout");
