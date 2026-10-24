// I3D_billboard (0x10005f80 - 0x10007dd0).
#include "i3d/billboard.h"

#include <cmath>

#include "i3d/camera.h"
#include "i3d/loader.h"
#include "i3d/mesh.h"
#include "i3d/render_helpers.h"
#include "i3d/render_state.h"
#include "external/rw_data.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

// The identity matrix world-space visuals turn with (0x1009b480).
const S_matrix &Identity()
{
    static const S_matrix s_identity = [] {
        S_matrix m;
        m.Identity();
        return m;
    }();
    return s_identity;
}

float Sign(float v) { return v < 0.0f ? -1.0f : 1.0f; }

// The SDK's inline 2D normalization of (a, b) with squared length `sq`: unchanged when (nearly)
// unit; a (nearly) zero vector becomes a unit along a (else b) when non-zero (A), or along b when
// a is zero, else along a (B).
bool Scale2(float &a, float &b, float sq)
{
    if (std::fabs(sq - 1.0f) < kEpsilon)
        return true;
    if (sq < kEpsilon)
        return false;
    const float inv = 1.0f / std::sqrt(sq);
    a = a * inv;
    b = inv * b;
    return true;
}
void Norm2A(float &a, float &b, float sq)
{
    if (Scale2(a, b, sq))
        return;
    if (a != 0.0f)
        a = Sign(a);
    else if (b != 0.0f)
        b = Sign(b);
}
void Norm2B(float &a, float &b, float sq)
{
    if (Scale2(a, b, sq))
        return;
    if (a == 0.0f)
        b = Sign(b);
    else
        a = Sign(a);
}

}  // namespace

// 0x10005f80
void I3D_billboard::Init()
{
    I3D_object::Init();
    axis = 2;
    mode = BBRDMODE_AXIS_SPHERE;
}

// 0x10005fa0
void I3D_billboard::UpdateLocalBound()
{
    visualFlags |= VISF_BOUND_VALID;
    if (mesh && (mesh->bboxMin.x < mesh->bboxMax.x || mesh->bboxMin.x == mesh->bboxMax.x)) {
        localBound.center = mesh->center;
        localBound.radius = mesh->radius;
        const S_vector &c = localBound.center;
        const float r = localBound.radius;
        localBound.bbox.min = S_vector(c.x - r, c.y - r, c.z - r);
        localBound.bbox.max = S_vector(c.x + r, c.y + r, c.z + r);
        return;
    }
    localBound.bbox.Invalidate();
    localBound.radius = 0.0f;
}

// 0x100060a0
I3D_RESULT I3D_billboard::SetMesh(I3D_mesh *newMesh, uint32_t flags)
{
    I3D_object::SetMesh(newMesh, flags);
    UpdateLocalBound();
    return I3D_OK;
}

// 0x100060d0
I3D_RESULT I3D_billboard::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    auto *src = static_cast<I3D_visual *>(source);
    visualFlags = src->visualFlags & 0x2ff52;
    localBound = src->localBound;
    if (!(src->visualFlags & VISF_WORLD_SPACE)) {
        visualFlags &= ~VISF_WBOUND_VALID;
    } else {
        src->GetWorldBound();
        worldBound = src->worldBound;
    }
    switch (src->GetVisualType()) {
    case FourCC("BBRD"):
        axis = static_cast<I3D_billboard *>(src)->axis;
        mode = static_cast<I3D_billboard *>(src)->mode;
        break;
    case FourCC("LMAP"):
    case FourCC("MRPH"):
    case FourCC("SNGM"):
    case FourCC("SMMR"):
    case FourCC("OBJ_"):
        break;
    case FourCC("LENS"):
        SetMesh(nullptr, 0);
        return I3D_OK;
    default:
        return I3DERR_GENERIC;
    }
    SetMesh(static_cast<I3D_object *>(src)->mesh, 0);
    return I3D_OK;
}

// 0x10007db0
void I3D_billboard::Load()
{
    I3D_object::Load();
    dtaRead(g_loadFile, &axis, 5);
}

// 0x10006230
void I3D_billboard::ComputeMatrix(const S_vector &camera, S_matrix &out)
{
    // The inverse matrix: the camera in the frame's space.
    auto inverse = [this]() -> const S_matrix & {
        if (visualFlags & VISF_WORLD_SPACE)
            return Identity();
        if (!(visualFlags & VISF_INVMATRIX_VALID)) {
            visualFlags |= VISF_INVMATRIX_VALID;
            invWorldMatrix.Inverse(GetMatrix());
        }
        return invWorldMatrix;
    };
    auto world = [this]() -> const S_matrix & { return (visualFlags & VISF_WORLD_SPACE) ? Identity() : GetMatrix(); };
    float *o = &out.m[0][0];
    switch (mode) {
    case BBRDMODE_AXIS_SPHERE: {
        if (axis < 0 || axis > 2)
            return;
        const S_vector c = camera * inverse();
        float a = c.x, b = c.z;
        float r = std::sqrt(c.z * c.z + c.x * c.x), s = c.y;
        if (axis == 1) {
            Norm2B(r, s, s * s + r * r);
            Norm2A(a, b, c.z * c.z + c.x * c.x);
        } else {
            Norm2A(a, b, c.z * c.z + c.x * c.x);
            Norm2B(r, s, axis == 0 ? s * s + r * r : r * r + s * s);
        }
        const S_matrix &wm = world();
        const float *w = &wm.m[0][0];
        for (int i = 0; i < 4; ++i) {
            const float ab = b * w[8 + i] + a * w[i];
            switch (axis) {
            case 0:
                o[i] = ab * r + s * w[4 + i];
                o[4 + i] = r * w[4 + i] - ab * s;
                o[8 + i] = a * w[8 + i] - b * w[i];
                break;
            case 1:
                o[i] = ab * s - r * w[4 + i];
                o[4 + i] = ab * r + s * w[4 + i];
                o[8 + i] = a * w[8 + i] - b * w[i];
                break;
            default:
                o[i] = a * w[8 + i] - b * w[i];
                o[4 + i] = r * w[4 + i] - ab * s;
                o[8 + i] = -(s * w[4 + i]) - ab * r;
                break;
            }
        }
        out(3) = wm(3);
        out.m[3][3] = wm.m[3][3];
        return;
    }
    case BBRDMODE_AXIS: {
        if (axis < 0 || axis > 2)
            return;
        const S_matrix &im = inverse();
        const float(*m)[4] = im.m;
        const float x = camera.x, y = camera.y, z = camera.z;
        const S_matrix *wp;
        switch (axis) {
        case 0: {
            float u = ((m[0][1] * x + m[1][1] * y) + m[2][1] * z) + m[3][1];
            float v = ((m[0][2] * x + m[1][2] * y) + m[2][2] * z) + m[3][2];
            Norm2B(v, u, v * v + u * u);
            wp = &world();
            const float *w = &wp->m[0][0];
            for (int i = 0; i < 4; ++i) {
                o[i] = w[i];
                o[12 + i] = w[12 + i];
            }
            for (int i = 0; i < 4; ++i) {
                o[4 + i] = u * w[8 + i] - v * w[4 + i];
                o[8 + i] = -(u * w[4 + i]) - v * w[8 + i];
            }
            return;
        }
        case 1: {
            float u = ((m[1][0] * y + m[2][0] * z) + m[0][0] * x) + m[3][0];
            float v = ((m[1][2] * y + m[2][2] * z) + m[0][2] * x) + m[3][2];
            Norm2A(u, v, v * v + u * u);
            wp = &world();
            const float *w = &wp->m[0][0];
            for (int i = 0; i < 4; ++i)
                o[i] = u * w[8 + i] - v * w[i];
            for (int i = 4; i < 8; ++i)
                o[i] = w[i];
            for (int i = 12; i < 16; ++i)
                o[i] = w[i];
            for (int i = 0; i < 4; ++i)
                o[8 + i] = -(u * w[i]) - v * w[8 + i];
            return;
        }
        default: {
            float u = ((m[1][0] * y + m[2][0] * z) + x * m[0][0]) + m[3][0];
            float v = ((m[1][1] * y + m[2][1] * z) + m[0][1] * x) + m[3][1];
            Norm2B(u, v, v * v + u * u);
            wp = &world();
            const float *w = &wp->m[0][0];
            for (int i = 0; i < 4; ++i) {
                o[i] = v * w[i] - u * w[4 + i];
                o[4 + i] = v * w[4 + i] + u * w[i];
            }
            for (int i = 8; i < 16; ++i)
                o[i] = w[i];
            return;
        }
        }
    }
    case BBRDMODE_SCREEN_SCALED:
    case BBRDMODE_SCREEN: {
        out = g_rc.camera->invViewMatrix;
        if (mode == BBRDMODE_SCREEN_SCALED) {
            const S_vector s = parent ? GetMatrix().GetScale() : scale;
            for (int c = 0; c < 3; ++c) {
                out.m[0][c] = s.x * out.m[0][c];
                out.m[1][c] = s.y * out.m[1][c];
                out.m[2][c] = s.z * out.m[2][c];
            }
        }
        const S_matrix &wm = GetMatrix();
        out(3) = wm(3);
        out.m[3][3] = wm.m[3][3];
        out.m[0][3] = 0.0f;
        out.m[1][3] = 0.0f;
        out.m[2][3] = 0.0f;
        out.m[3][3] = 1.0f;
        return;
    }
    case BBRDMODE_SCREEN_LOCAL: {
        UpdateLMatrix();
        out = localMatrix * g_rc.camera->invViewMatrix;
        const S_matrix &wm = GetMatrix();
        out(3) = wm(3);
        out.m[3][3] = 1.0f;
        const S_vector d(camera.x - wm.m[3][0], camera.y - wm.m[3][1], camera.z - wm.m[3][2]);
        // The farther, the bigger: from 20 units, at most 3 times.
        float f = MagnitudeOfSquare(SquareZYX(d)) * 0.05f;
        if (f < 1.0f)
            return;
        if (f > 3.0f)
            f = 3.0f;
        for (int c = 0; c < 3; ++c) {
            out.m[0][c] = f * out.m[0][c];
            out.m[1][c] = f * out.m[1][c];
            out.m[2][c] = f * out.m[2][c];
        }
        return;
    }
    default:
        return;
    }
}

// 0x100078e0
void I3D_billboard::Prepare()
{
    if (!mesh)
        return;
    if (visualFlags & VISF_MIRRORED) {
        auto *original = static_cast<I3D_billboard *>(data);
        axis = original->axis;
        mode = original->mode;
        PrepareMirrored();
        ComputeMatrix(*reinterpret_cast<const S_vector *>(&g_rc.lodPosition), matrix);
        return;
    }
    const float distance2 = LodDistance2();
    const int levelIndex = SelectLevel(distance2);
    I3D_mesh_level *level = mesh->levels[levelIndex];
    if (!level || !level->faceGroups || !level->numVertices)
        return;
    ComputeMatrix(*reinterpret_cast<const S_vector *>(&g_rc.lodPosition), matrix);
    if (!level->renderVertices.vb)
        level->CreateRenderVB(0x11, &matrix, nullptr);
    else if (level->flags & MLF_RENDER_STALE)
        level->FillRenderVB(false, nullptr, nullptr);
    SetLodFade(levelIndex, level, distance2);
    SetLightSector(g_rc.sector);
    SelectLights(this, lights, 1);
    QueueFaceGroups(level, nullptr);
}
