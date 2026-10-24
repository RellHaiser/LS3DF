// I3D_camera (0x10009840 - 0x1000c6a0).
#include "i3d/camera.h"

#include <cmath>

#include "igraph/igraph.h"
#include "i3d/i3d_hooks.h"
#include "i3d/scene.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

constexpr float kPi = 3.14159274f;

// The two orders the camera's code sums squared lengths and plane distances in.
enum E_sum { SUM_XZY, SUM_ZYX };

float SumProducts(const S_vector &p, const S_vector &n, E_sum order)
{
    return order == SUM_XZY ? (p.x * n.x + p.z * n.z) + p.y * n.y : (p.z * n.z + p.y * n.y) + p.x * n.x;
}

// The plane through `base`, `a` and `b` (normal (b - base) x (a - base), 0x1000c6a0, normalized)
// whose distance is measured at `p`; also into `d`.
void MakePlane(S_plane &plane, float &d, const S_vector &a, const S_vector &base, const S_vector &b,
               E_sum squares, const S_vector &p, E_sum products)
{
    S_vector n = (b - base).Cross(a - base);
    NormalizeWithSquare(n, squares == SUM_XZY ? SquareXZY(n) : SquareZYX(n));
    plane.normal = n;
    plane.d = -SumProducts(p, n, products);
    d = plane.d;
}

// cos(h) / sin(h) of the half angle, with the original's widening: the full angle (at most 1.5)
// for aspect ratios over 2.5.
float Cotangent(float angle, float aspect)
{
    float h = angle * 0.5f;
    if (aspect > 2.5f) {
        h = h + h;
        if (h > 1.5f)
            h = 1.5f;
    }
    // (FCOS / FSIN keep extended precision; the division rounds to the FPU's 24 bits.)
    return static_cast<float>(std::cos(static_cast<double>(h)) / std::sin(static_cast<double>(h)));
}

void Multiply4(S_matrix &r, const S_matrix &a, const S_matrix &b) { ::Multiply4x4(r.m, a.m, b.m); }

void ZeroMatrix(S_matrix &m)
{
    for (auto &row : m.m)
        for (float &f : row)
            f = 0.0f;
}

}  // namespace

void I3D_camera::Init()
{
    I3D_frame::Init();
    nearSectorFov = 0.785398185f;
    fov = 0.785398185f;
    nearRange = 1.0f;
    farRange = 1000.0f;
    nearSectorNear = 0.01f;
    nearSectorFar = 20.0f;
    const float width = static_cast<float>(g_pIGraph->ScrnX());
    const int height = g_pIGraph->ScrnY();
    scene = nullptr;
    sector = nullptr;
    ortho = false;
    seenChangeCount = seenSectorChangeCount = 0;
    nearHalfWidth = nearRange;
    nearSectorHalfHeight = nearSectorNear;
    nearHalfHeight = nearRange;
    orthoScale = 1.0f;
    nearSectorHalfWidth = nearSectorNear;
    frameFlags |= FRMFLAGS_CAMERA_PROJECTION;
    aspect = width / static_cast<float>(height);
    BuildProjection();
    viewMatrix.Identity();
    viewProjection = projection;
    invViewMatrix.Identity();
    viewRotation.Identity();
    backdropViewProjection = backdropProjection;
    invViewRotation.Identity();
    nearSectorViewProjection = nearSectorProjection;
}

void I3D_camera::Uninit()
{
    if (scene)
        scene->activeCamera = nullptr;
    I3D_frame::Uninit();
}

I3D_RESULT __stdcall I3D_camera::Duplicate(I3D_frame *source)
{
    const I3D_RESULT r = I3D_frame::Duplicate(source);
    if (r < 0)
        return r;
    if (source->type == FRAME_CAMERA) {
        auto *src = static_cast<I3D_camera *>(source);
        fov = src->fov;
        nearRange = src->nearRange;
        farRange = src->farRange;
        nearSectorFov = src->nearSectorFov;
        nearSectorNear = src->nearSectorNear;
        nearSectorFar = src->nearSectorFar;
        aspect = src->aspect;
        scene = nullptr;
        sector = nullptr;
        ortho = src->ortho;
        orthoScale = src->orthoScale;
        seenChangeCount = seenSectorChangeCount = 0;
        nearHalfWidth = src->nearHalfWidth;
        nearHalfHeight = src->nearHalfHeight;
        // (The near sector's half sizes stay as they were.)
        frameFlags |= FRMFLAGS_CAMERA_PROJECTION;
        viewMatrix.Identity();
        invViewMatrix.Identity();
        projection = src->projection;
        viewProjection = src->projection;
        viewRotation.Identity();
        invViewRotation.Identity();
        backdropProjection = src->backdropProjection;
        backdropViewProjection = src->backdropProjection;
        nearSectorProjection = src->nearSectorProjection;
    }
    return I3D_OK;
}

void I3D_camera::BuildProjection()
{
    ZeroMatrix(projection);
    if (ortho) {
        projection.m[0][0] = orthoScale;
        const float m22 = 1.0f / (farRange - nearRange);
        projection.m[1][1] = aspect * orthoScale;
        projection.m[2][2] = m22;
        projection.m[3][2] = -(m22 * nearRange);
        projection.m[3][3] = 1.0f;
        backdropProjection = projection;
        float b22, b32;
        if (!scene) {
            b22 = 1.0f / (farRange * 1000.0f - nearRange * 10.0f);
            backdropProjection.m[2][2] = b22;
            b32 = (b22 * nearRange) * -10.0f;
        } else {
            b22 = 1.0f / (scene->backdropFar - scene->backdropNear);
            backdropProjection.m[2][2] = b22;
            b32 = -(b22 * scene->backdropNear);
        }
        backdropProjection.m[3][2] = b32;
        nearSectorProjection = projection;
        // (The near sector's offset from the backdrop's scale.)
        nearSectorProjection.m[3][2] = -(b22 * nearSectorNear);
        ++changeCount;
        return;
    }
    const float cot = Cotangent(fov, aspect);
    const float m11 = cot * aspect;
    projection.m[0][0] = cot;
    projection.m[1][1] = m11;
    const float m22 = farRange / (farRange - nearRange);
    projection.m[2][2] = m22;
    projection.m[2][3] = 1.0f;
    projection.m[3][2] = -(m22 * nearRange);
    backdropProjection = projection;
    nearHalfWidth = nearRange / cot;
    nearHalfHeight = nearRange / m11;
    if (!scene) {
        const float far1000 = farRange * 1000.0f;
        const float b22 = far1000 / (far1000 - nearRange * 10.0f);
        backdropProjection.m[2][2] = b22;
        backdropProjection.m[3][2] = (b22 * nearRange) * -10.0f;
    } else {
        const float b22 = scene->backdropFar / (scene->backdropFar - scene->backdropNear);
        backdropProjection.m[2][2] = b22;
        backdropProjection.m[3][2] = -(b22 * scene->backdropNear);
    }
    ZeroMatrix(nearSectorProjection);
    const float nearCot = Cotangent(nearSectorFov, aspect);
    const float n11 = nearCot * aspect;
    nearSectorProjection.m[0][0] = nearCot;
    nearSectorProjection.m[1][1] = n11;
    const float n22 = nearSectorFar / (nearSectorFar - nearSectorNear);
    nearSectorProjection.m[2][2] = n22;
    nearSectorProjection.m[2][3] = 1.0f;
    nearSectorProjection.m[3][2] = -(n22 * nearSectorNear);
    ++changeCount;
    nearSectorHalfWidth = nearSectorNear / nearCot;
    nearSectorHalfHeight = nearSectorNear / n11;
}

void I3D_camera::UpdateSector()
{
    if (!scene) {
        sector = nullptr;
        GetMatrix();
        return;
    }
    GetMatrix();
    sector = SceneLocateSector(scene, worldMatrix(3), sector);
}

void I3D_camera::MatrixChanged()
{
    UpdateSector();
    const S_matrix &w = worldMatrix;
    viewMatrix.Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            viewMatrix.m[r][c] = w.m[c][r];
    viewMatrix(3) = S_vector(-w.m[3][0], -w.m[3][1], -w.m[3][2]) * viewMatrix;
    viewMatrix.m[3][3] = 1.0f;
    invViewMatrix.Inverse(viewMatrix);
    Multiply4(viewProjection, viewMatrix, projection);
    viewRotation = viewMatrix;
    viewRotation(3) = S_vector(0.0f, 0.0f, 0.0f);
    Multiply4(backdropViewProjection, viewRotation, backdropProjection);
    invViewRotation = invViewMatrix;
    invViewRotation(3) = S_vector(0.0f, 0.0f, 0.0f);
    Multiply4(nearSectorViewProjection, viewMatrix, nearSectorProjection);

    const S_vector &pos = w(3);
    const S_vector &dir = w(2);
    frustum[0].normal = dir;
    frustum[0].d = w.m[2][3];
    frustumD[0] = -((pos.z * dir.z + pos.y * dir.y) + dir.x * pos.x);

    if (ortho) {
        const float a = 1.0f / orthoScale;
        const float b = a / aspect;
        const S_vector p0 = S_vector(-a, b, 0.0f) * w;
        const S_vector p1 = S_vector(-a, -b, 0.0f) * w;
        const S_vector p2 = S_vector(a, b, 0.0f) * w;
        const S_vector p3 = S_vector(a, -b, 0.0f) * w;
        const S_vector q0 = S_vector(-a, -b, 1.0f) * w;
        const S_vector q1 = S_vector(a, b, 1.0f) * w;
        MakePlane(frustum[1], frustumD[1], q0, p0, p1, SUM_XZY, q0, SUM_XZY);
        MakePlane(frustum[2], frustumD[2], q1, p3, p2, SUM_ZYX, q1, SUM_XZY);
        MakePlane(frustum[3], frustumD[3], q1, p2, p0, SUM_XZY, q1, SUM_XZY);
        MakePlane(frustum[4], frustumD[4], q0, p1, p3, SUM_XZY, q0, SUM_XZY);
        backdropFrustum[0].normal = dir;
        backdropFrustum[0].d = w.m[2][3];
        backdropFrustumD[0] = 0.0f;
        MakePlane(backdropFrustum[1], backdropFrustumD[1], q0, p0, p1, SUM_XZY, q0, SUM_XZY);
        MakePlane(backdropFrustum[2], backdropFrustumD[2], q1, p3, p2, SUM_XZY, q1, SUM_XZY);
        MakePlane(backdropFrustum[3], backdropFrustumD[3], q1, p2, p0, SUM_XZY, q1, SUM_ZYX);
        MakePlane(backdropFrustum[4], backdropFrustumD[4], q0, p1, p3, SUM_XZY, q0, SUM_ZYX);
        for (int i = 0; i < 5; ++i) {
            nearSectorFrustum[i] = frustum[i];
            nearSectorFrustumD[i] = frustumD[i];
        }
        return;
    }

    const S_vector p0 = S_vector(nearHalfWidth * -100.0f, nearHalfHeight * 100.0f, nearRange * 100.0f) * w;
    const S_vector p1 = S_vector(nearHalfWidth * -100.0f, nearHalfHeight * -100.0f, nearRange * 100.0f) * w;
    const S_vector p2 = S_vector(nearHalfWidth * 100.0f, nearHalfHeight * 100.0f, nearRange * 100.0f) * w;
    const S_vector p3 = S_vector(nearHalfWidth * 100.0f, nearHalfHeight * -100.0f, nearRange * 100.0f) * w;
    MakePlane(frustum[1], frustumD[1], pos, p0, p1, SUM_ZYX, pos, SUM_ZYX);
    MakePlane(frustum[2], frustumD[2], pos, p3, p2, SUM_ZYX, pos, SUM_ZYX);
    MakePlane(frustum[3], frustumD[3], pos, p2, p0, SUM_ZYX, pos, SUM_ZYX);
    MakePlane(frustum[4], frustumD[4], pos, p1, p3, SUM_ZYX, pos, SUM_ZYX);
    backdropFrustum[0].normal = dir;
    backdropFrustum[0].d = w.m[2][3];
    backdropFrustumD[0] = 0.0f;
    MakePlane(backdropFrustum[1], backdropFrustumD[1], pos, p0, p1, SUM_ZYX, pos, SUM_ZYX);
    MakePlane(backdropFrustum[2], backdropFrustumD[2], pos, p3, p2, SUM_ZYX, pos, SUM_ZYX);
    MakePlane(backdropFrustum[3], backdropFrustumD[3], pos, p2, p0, SUM_ZYX, pos, SUM_ZYX);
    MakePlane(backdropFrustum[4], backdropFrustumD[4], pos, p1, p3, SUM_XZY, pos, SUM_ZYX);
    nearSectorFrustum[0] = frustum[0];
    nearSectorFrustumD[0] = frustumD[0];
    const float hw = nearSectorHalfWidth, hh = nearSectorHalfHeight, nz = nearSectorNear * 100.0f;
    const S_vector r0 = S_vector(hw * -100.0f, hh * 100.0f, nz) * w;
    const S_vector r1 = S_vector(hw * -100.0f, hh * -100.0f, nz) * w;
    const S_vector r2 = S_vector(hw * 100.0f, hh * 100.0f, nz) * w;
    const S_vector r3 = S_vector(hw * 100.0f, hh * -100.0f, nz) * w;
    MakePlane(nearSectorFrustum[1], nearSectorFrustumD[1], pos, r0, r1, SUM_XZY, pos, SUM_ZYX);
    MakePlane(nearSectorFrustum[2], nearSectorFrustumD[2], pos, r3, r2, SUM_XZY, pos, SUM_ZYX);
    MakePlane(nearSectorFrustum[3], nearSectorFrustumD[3], pos, r2, r0, SUM_XZY, pos, SUM_ZYX);
    MakePlane(nearSectorFrustum[4], nearSectorFrustumD[4], pos, r1, r3, SUM_ZYX, pos, SUM_ZYX);
}

void __stdcall I3D_camera::SetFOV(float angle)
{
    if (angle < 0.01f)
        angle = 0.01f;
    else if (angle >= kPi)
        angle = kPi;
    fov = angle;
    if (!ortho)
        BuildProjection();
}

void __stdcall I3D_camera::SetNearSectorFOV(float angle)
{
    if (angle < 0.01f)
        angle = 0.01f;
    else if (angle >= kPi)
        angle = kPi;
    nearSectorFov = angle;
    if (!ortho)
        BuildProjection();
}

void __stdcall I3D_camera::SetRange(float nearDist, float farDist)
{
    if (std::fabs(farDist - nearDist) < 0.01f || nearDist < 0.001f || farDist < 0.001f)
        return;
    nearRange = nearDist;
    farRange = farDist;
    BuildProjection();
}

void __stdcall I3D_camera::SetNearSectorRange(float nearDist, float farDist)
{
    if (std::fabs(farDist - nearDist) < 0.01f || nearDist < 0.001f || farDist < 0.001f)
        return;
    nearSectorNear = nearDist;
    nearSectorFar = farDist;
    BuildProjection();
}

void __stdcall I3D_camera::SetOrthoScale(float s)
{
    if (s < 1e-6f)
        return;
    orthoScale = s;
    if (ortho)
        BuildProjection();
}

void __stdcall I3D_camera::SetOrthogonal(bool on)
{
    ortho = on;
    BuildProjection();
}

void __stdcall I3D_camera::SetAspectRatio(float a)
{
    if (a < 0.01f || a > 100.0f)
        return;
    aspect = a;
    if (!ortho)
        BuildProjection();
}

bool __stdcall I3D_camera::IsSphereVisible(const S_vector4 &sphere)
{
    const auto dist = [&sphere](const S_plane &p, float d) {
        return ((p.normal.z * sphere.z + p.normal.x * sphere.x) + p.normal.y * sphere.y) + d;
    };
    const float z = dist(frustum[0], frustumD[0]);
    if (z + sphere.w < nearRange)
        return false;
    if (z - sphere.w > farRange)
        return false;
    const float minDist = -sphere.w;
    for (int i = 1; i < 5; ++i) {
        if (dist(frustum[i], frustumD[i]) < minDist)
            return false;
    }
    return true;
}
