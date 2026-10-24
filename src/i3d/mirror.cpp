// I3D_mirror (0x10032280 - 0x10033fb0) and the scenes' mirror lists (I3D_scene 0x10048120 /
// 0x100481c0). (0x10033f90 is the static initialiser of this file's copy of a header's zero
// vector; 0x10033f70 the vertex buffer manager's locked-list removal, inlined.)
#include "i3d/mirror.h"

#include <cmath>
#include <new>

#include "core/debug.h"
#include "core/fpu.h"
#include "external/rw_data.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/loader.h"
#include "i3d/object.h"
#include "i3d/render_helpers.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ig::g_pDevice;
using ig::g_rs;

namespace {

// The visuals the mirror being prepared reflects (0x100be580, grow 0x20; 0x10033170 / 0x100331a0
// make and free it) and that mirror (0x100be5a0).
C_vector<I3D_visual *> s_reflected(0x20);
I3D_mirror *s_mirror;

// The scene of the sector above `f` (none: null).
I3D_scene *SectorScene(I3D_frame *f)
{
    for (; f; f = f->parent) {
        if (f->type == FRAME_SECTOR)
            return static_cast<I3D_sector *>(f)->scene;
    }
    return nullptr;
}

// 0x100331c0: the visuals below `f` inside the mirror's plane box (their bound's sphere touching
// the plane's, a corner of their box inside it on every side).
void CollectReflected(I3D_frame *f)
{
    if (!(f->frameFlags & 0x400001))
        return;
    const I3D_bound_volume &b = f->GetBound();
    const S_vector d(b.center.x - s_mirror->planeCenter.x, b.center.y - s_mirror->planeCenter.y,
                     b.center.z - s_mirror->planeCenter.z);
    const float r = s_mirror->planeRadius + b.radius;
    if (r * r < SquareZXY(d))
        return;
    if (f->type == FRAME_VISUAL) {
        auto *vis = static_cast<I3D_visual *>(f);
        if (!(vis->visualFlags & (VISF_MIRRORED | 0x20)) &&
            ((vis->visualFlags & VISF_OBJECT_PARAMS) || vis->GetVisualType() == FourCC("EMTR"))) {
            const S_vector4 *corners = BoxCorners(vis->GetLocalBound().bbox);
            const S_matrix m = vis->GetMatrix() * s_mirror->planeInverse;
            uint32_t sides = 0;
            for (int i = 0; i < 8; ++i) {
                const S_vector p = *reinterpret_cast<const S_vector *>(&corners[i]) * m;
                if (p.x < 1.0f)
                    sides |= 1;
                if (p.x > -1.0f)
                    sides |= 2;
                if (p.y < 1.0f)
                    sides |= 4;
                if (p.y > -1.0f)
                    sides |= 8;
                if (p.z < 1.0f)
                    sides |= 0x10;
                if (p.z > -1.0f)
                    sides |= 0x20;
            }
            if (sides == 0x3f)
                s_reflected.PushBack(vis);
        }
    }
    for (I3D_frame *c = f->firstChild; c; c = c->nextSibling)
        CollectReflected(c);
}

// The copies released (their originals' references too).
void ReleaseCopies(C_vector<I3D_visual *> &copies)
{
    for (I3D_visual *c : copies) {
        static_cast<I3D_frame *>(c->data)->Release();
        c->Release();
    }
}

}  // namespace

// I3D_scene's mirror list ------------------------------------------------------------------------------

// 0x10048120
void I3D_scene::AddMirror(I3D_mirror *mirror)
{
    if (mirror->scene == this)
        return;
    mirrors.PushBack(mirror);
    mirror->scene = this;
}

// 0x100481c0
I3D_RESULT I3D_scene::RemoveMirror(I3D_mirror *mirror)
{
    if (!mirrors.RemoveSwap(mirror))
        return I3DERR_OBJECTNOTFOUND;
    mirror->scene = nullptr;
    return I3D_OK;
}

// I3D_mirror ----------------------------------------------------------------------------------------------

// 0x10032280
void I3D_mirror::Init()
{
    I3D_visual::Init();
    color.x = color.y = color.z = 0.0f;
    planeMatrix.Identity();
    numVertices = 0;
    numFaces = 0;
    vertices = nullptr;
    mirrorFlags = 0;
    stamp = static_cast<uint16_t>(g_driver->mirrorStamp++);
    scene = nullptr;
    planeCenter = S_vector(0.0f, 0.0f, 0.0f);
    planeRadius = 0.0f;
    range = 100.0f;
    colorVertices = S_VBAlloc{};
    faces = S_IBAlloc{};
}

// 0x10032330
void I3D_mirror::Uninit()
{
    if (scene)
        scene->RemoveMirror(this);
    if (colorVertices.vb)
        FreeVB(&colorVertices);
    ReleaseCopies(copies);
    copies.Clear();
    AlignedFree(vertices);
    // (Fix: the original leaves the pointer to the freed vertices.)
    vertices = nullptr;
    FreeIB(&faces);
    I3D_frame::Uninit();
}

// 0x100323c0
bool I3D_mirror::SetColor(const S_vector4 &c)
{
    if (IsTinyBits(std::fabs(color.x - c.x)) && IsTinyBits(std::fabs(color.y - c.y)) &&
        IsTinyBits(std::fabs(color.z - c.z)))
        return true;
    color = c;
    auto clamp = [](float &v) {
        if (v < 0.0f)
            v = 0.0f;
        else if (v > 1.0f)
            v = 1.0f;
    };
    clamp(color.x);
    clamp(color.y);
    clamp(color.z);
    if (colorVertices.vb)
        FillColorVertices();
    return false;
}

// 0x10032520
I3D_triface *I3D_mirror::LockFaces(uint32_t flags)
{
    if (!numFaces)
        return nullptr;
    uint16_t *p = faces.buffer->Lock((flags & 1) ? D3DLOCK_READONLY : 0);
    return reinterpret_cast<I3D_triface *>(p + faces.offset);
}

// 0x10032630
I3D_RESULT I3D_mirror::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    ReleaseCopies(copies);
    // (Fix: the original keeps the released copies listed.)
    copies.Clear();
    AlignedFree(vertices);
    vertices = nullptr;
    numVertices = 0;
    FreeIB(&faces);
    faces = S_IBAlloc{};
    if (colorVertices.vb) {
        FreeVB(&colorVertices);
        colorVertices = S_VBAlloc{};
    }
    numFaces = 0;
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    if (static_cast<I3D_visual *>(source)->GetVisualType() != FourCC("MIRR"))
        return I3D_OK;
    auto *src = static_cast<I3D_mirror *>(source);
    planeMatrix = src->planeMatrix;
    mirrorFlags = (mirrorFlags & ~MIRF_REFLECTION_VALID) | MIRF_PLANE;
    vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * src->numVertices, 16));
    // (Fix: the original copies into a failed allocation.)
    if (!vertices) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    numVertices = src->numVertices;
    for (int i = 0; i < numVertices; ++i)
        vertices[i] = src->vertices[i];
    if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, src->numFaces * 3, &faces)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    numFaces = src->numFaces;
    const I3D_triface *from = src->LockFaces(1);
    I3D_triface *to = LockFaces(1);
    std::memcpy(to, from, static_cast<size_t>(numFaces) * sizeof(I3D_triface));
    localBound = src->localBound;
    visualFlags = (visualFlags & ~VISF_WBOUND_VALID) | VISF_BOUND_VALID;
    range = src->range;
    color = src->color;
    return I3D_OK;
}

// 0x10032880
void I3D_mirror::FillColorVertices()
{
    if (!colorVertices.vb && !g_vbManager.Alloc(D3DUSAGE_WRITEONLY, 0x18, numVertices, &colorVertices, 2, false)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    const uint32_t r = static_cast<uint32_t>(Fistp(color.x * 255.0f));
    const uint32_t g = static_cast<uint32_t>(Fistp(color.y * 255.0f));
    const uint32_t b = static_cast<uint32_t>(Fistp(color.z * 255.0f));
    const uint32_t argb = (((0xffffff00u | r) << 8) | g) << 8 | b;
    C_VertexBuffer *vb = colorVertices.buffer;
    auto *dst = reinterpret_cast<uint32_t *>(vb->Lock(0) + vb->stride * colorVertices.offset);
    for (int i = 0; i < numVertices; ++i, dst += 6) {
        std::memcpy(dst, &vertices[i], 12);
        dst[3] = argb;
    }
}

// 0x10032c90
I3D_RESULT I3D_mirror::LinkTo(I3D_frame *newParent, uint32_t flags)
{
    if (scene)
        scene->RemoveMirror(this);
    if (I3D_scene *s = SectorScene(newParent))
        s->AddMirror(this);
    const I3D_RESULT r = I3D_frame::LinkTo(newParent, flags);
    MatrixChanged();
    return r;
}

// 0x10032cf0
void I3D_mirror::MatrixChanged()
{
    if (frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED)) {
        visualFlags &= ~(VISF_WBOUND_VALID | VISF_INVMATRIX_VALID);
        mirrorFlags &= ~MIRF_REFLECTION_VALID;
        // The copies are placed again when next drawn.
        for (I3D_visual *c : copies)
            c->changeCount = -1;
    }
    if (!scene) {
        if (I3D_scene *s = SectorScene(parent))
            s->AddMirror(this);
    }
}

// 0x10032d80
void I3D_mirror::Tick(int)
{
    I3D_visual **c = copies.Data();
    for (int n = copies.Size(); n; --n) {
        auto *original = static_cast<I3D_frame *>((*c)->data);
        if (original->refCount >= 2 && original->parent) {
            ++c;
            continue;
        }
        original->Release();
        I3D_visual *copy = *c;
        copy->Release();
        // (The slot gets the last copy, looked at next.)
        copies.RemoveSwap(copy);
    }
}

// 0x10033d50
void I3D_mirror::SetPlane(const I3D_bbox &box, const S_matrix &matrix)
{
    planeMatrix = matrix;
    for (int r = 0; r < 3; ++r) {
        const float h = ((&box.max.x)[r] - (&box.min.x)[r]) * 0.5f;
        planeMatrix.m[r][0] = h * planeMatrix.m[r][0];
        planeMatrix.m[r][1] = h * planeMatrix.m[r][1];
        planeMatrix.m[r][2] = h * planeMatrix.m[r][2];
    }
    const S_vector center((box.max.x + box.min.x) * 0.5f, (box.max.y + box.min.y) * 0.5f,
                          (box.max.z + box.min.z) * 0.5f);
    // (By the scaled matrix: the original's.)
    planeMatrix(3) = center * planeMatrix;
    mirrorFlags = (mirrorFlags & ~MIRF_REFLECTION_VALID) | MIRF_PLANE;
}

// 0x10033e80
void I3D_mirror::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &localBound, 0x30);
    visualFlags = (visualFlags & ~VISF_WBOUND_VALID) | VISF_BOUND_VALID;
    // The plane matrix, color, range and the vertex and face counts.
    dtaRead(g_loadFile, &planeMatrix, 0x5c);
    mirrorFlags = (mirrorFlags & ~MIRF_REFLECTION_VALID) | MIRF_PLANE;
    vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVertices, 16));
    // (Fix: the original reads into a failed allocation.)
    if (!vertices) {
        g_loadResult = I3DERR_OUTOFMEM;
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    dtaRead(g_loadFile, vertices, numVertices * 16);
    if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, numFaces * 3, &faces)) {
        g_loadResult = I3DERR_OUTOFMEM;
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    dtaRead(g_loadFile, LockFaces(0), numFaces * 6);
}

namespace ls3d::i3d {

// 0x10032e50
void MirrorPrepare(I3D_mirror *mirror)
{
    if (mirror->renderStamp != g_renderStamp) {
        mirror->mirrorFlags &= ~MIRF_VISIBLE;
        return;
    }
    // A mirror facing away (its normal within the view cone pointing from the camera) is not seen.
    S_vector n = mirror->GetMatrix()(2);
    NormalizeWithSquare(n, SquareYZX(n));
    I3D_camera *cam = g_renderCamera;
    const S_vector v = n.RotateByMatrix(cam->viewMatrix);
    if (v.z > 0.0f) {
        const float len = MagnitudeOfSquare((v.y * v.y + v.x * v.x) + v.z * v.z);
        const float c = v.z / len;
        if (std::cos(static_cast<double>(cam->fov * 0.5f)) < c) {
            mirror->mirrorFlags &= ~(MIRF_VISIBLE | MIRF_REFLECTS);
            return;
        }
    }
    mirror->mirrorFlags |= MIRF_VISIBLE;
    const float r = g_fovScale * mirror->range;
    const S_vector &cp = cam->GetMatrix()(3);
    const S_vector &mp = mirror->GetMatrix()(3);
    const S_vector d(mp.x - cp.x, mp.y - cp.y, mp.z - cp.z);
    if (r * r < SquareZXY(d) || !g_driverStates[DRVSTATE_MIRRORS]) {
        mirror->mirrorFlags &= ~MIRF_REFLECTS;
        return;
    }
    mirror->mirrorFlags |= MIRF_REFLECTS;
}

// 0x10033410
bool MirrorBeginReflection(I3D_mirror *mirror)
{
    if (!mirror->colorVertices.vb) {
        mirror->FillColorVertices();
        mirror->colorVertices.buffer->Unlock();
    }
    g_renderMirror = mirror;
    if (!(mirror->mirrorFlags & MIRF_REFLECTION_VALID)) {
        if (!(mirror->mirrorFlags & MIRF_PLANE))
            return false;
        mirror->mirrorFlags |= MIRF_REFLECTION_VALID;
        // The plane in world space: its center, the sphere around it, its inverse.
        const S_matrix plane = mirror->planeMatrix * mirror->GetMatrix();
        mirror->planeCenter = plane(3);
        const S_vector axes = S_vector(1.0f, 1.0f, 1.0f).RotateByMatrix(plane);
        mirror->planeRadius2 = SquareZYX(axes);
        mirror->planeRadius = std::sqrt(mirror->planeRadius2);
        mirror->planeInverse.Inverse(plane);
        // The reflection: into the mirror frame's space (looking along its z), z flipped, back.
        S_vector z = mirror->GetMatrix()(2);
        NormalizeWithSquare(z, SquareXYZ(z));
        S_matrix frame;
        frame.SetDir(z);
        const S_matrix &w = mirror->GetMatrix();
        frame(3) = w(3);
        frame.m[3][3] = w.m[3][3];
        mirror->reflection.Inverse(frame);
        S_matrix flip = frame;
        flip.m[2][0] = frame.m[2][0] * -1.0f;
        flip.m[2][1] = frame.m[2][1] * -1.0f;
        flip.m[2][2] = frame.m[2][2] * -1.0f;
        mirror->reflection = mirror->reflection * flip;
    }
    s_reflected.Clear();
    s_mirror = mirror;
    CollectReflected(mirror->scene->primarySector);
    CollectReflected(mirror->scene->omniSector);
    if (!s_reflected.Size()) {
        mirror->mirrorFlags &= ~MIRF_REFLECTS;
        return false;
    }

    // z cleared to 0, then the mirror's faces written with z always: only there can the
    // reflection be drawn.
    g_pDevice->Clear(0, nullptr, (ig::g_caps & 0x10 | 8) >> 2, 0, 0.0f, 0);
    if (g_rs.zFunc != 0) {
        g_rs.zFunc = 0;
        g_pDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    }
    if (g_rs.fog != 0) {
        g_rs.fog = 0;
        g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    }
    if (g_rs.colorOp[0] != D3DTOP_MODULATE) {
        g_rs.colorOp[0] = D3DTOP_MODULATE;
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    }
    if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
        g_rs.colorOp[1] = D3DTOP_DISABLE;
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (g_rs.alpha != 0) {
        g_rs.alpha = 0;
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
    g_rs.cullDefault = D3DCULL_CCW;
    if (g_rs.cull != 1) {
        g_rs.cull = 1;
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (g_rs.cull != 3) {
        g_rs.cull = 3;
        g_pDevice->SetRenderState(D3DRS_CULLMODE, g_rs.cullDefault);
    }
    // The world x view x projection matrix, transposed, for the vertex shader.
    float wvp[4][4];
    MultiplyAffine4x4(wvp, mirror->GetMatrix().m, g_viewProjection->m);
    float constants[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            constants[i][j] = wvp[j][i];
    g_pDevice->SetVertexShaderConstant(0, constants, 4);
    SetVertexFormat(9);
    g_pDevice->SetIndices(mirror->faces.ib, mirror->colorVertices.offset);
    g_pDevice->SetStreamSource(0, mirror->colorVertices.vb, mirror->colorVertices.buffer->stride);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, mirror->numVertices, mirror->faces.offset,
                                    mirror->numFaces);
    if (g_rs.zFunc != 1) {
        g_rs.zFunc = 1;
        g_pDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    }

    // Each reflected visual through its copy, placed at the reflected position.
    for (I3D_visual *original : s_reflected) {
        I3D_visual *copy = nullptr;
        for (I3D_visual *c : mirror->copies) {
            if (c->data == original) {
                copy = c;
                break;
            }
        }
        if (!copy) {
            copy = static_cast<I3D_visual *>(g_driver->CreateVisual(original->GetVisualType()));
            mirror->copies.PushBack(copy);
            copy->visualFlags = VISF_MIRRORED;
            copy->Duplicate(original);
            copy->data = original;
            ++original->refCount;
            copy->visualFlags = (copy->visualFlags & ~VISF_WORLD_SPACE) | VISF_MIRRORED;
            copy->frameFlags |= 0x200000;
            copy->SetCallback(nullptr, 0x1a00);
        }
        if (copy->changeCount != original->changeCount) {
            const S_matrix &w = (original->visualFlags & VISF_WORLD_SPACE) ? kIdentityMatrix : original->GetMatrix();
            copy->localMatrix = w * mirror->reflection;
            const S_vector s = copy->localMatrix.GetScale();
            copy->scale = s;
            copy->frameFlags = (copy->frameFlags & ~0x28u) | 0x40000014u;
            copy->Update();
            copy->SetBoundVolume(original->GetLocalBound());
            copy->changeCount = original->changeCount;
        }
        if (copy->visualFlags & VISF_OBJECT_PARAMS)
            static_cast<I3D_object *>(copy)->alpha = static_cast<I3D_object *>(original)->alpha;
        copy->Prepare();
        copy->renderStamp = g_renderStamp;
    }
    if (!mirror->copies.Size())
        mirror->mirrorFlags &= ~MIRF_REFLECTS;
    return mirror->copies.Size() != 0;
}

// 0x10032a60
void MirrorDrawSurface(I3D_mirror *mirror)
{
    if (!mirror->vertices || !mirror->faces.ib)
        return;
    if (!mirror->colorVertices.vb) {
        mirror->FillColorVertices();
        mirror->colorVertices.buffer->Unlock();
    }
    if (g_rs.colorOp[0] != D3DTOP_MODULATE) {
        g_rs.colorOp[0] = D3DTOP_MODULATE;
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    }
    if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
        g_rs.colorOp[1] = D3DTOP_DISABLE;
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    g_pIGraph->SetWorldMatrix(&mirror->GetMatrix());
    g_pDevice->SetTexture(0, nullptr);
    SetVertexFormat(static_cast<int>(mirror->colorVertices.buffer->format));
    g_pDevice->SetIndices(mirror->faces.ib, mirror->colorVertices.offset);
    g_pDevice->SetStreamSource(0, mirror->colorVertices.vb, mirror->colorVertices.buffer->stride);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, mirror->numVertices, mirror->faces.offset,
                                    mirror->numFaces);
}

}  // namespace ls3d::i3d
