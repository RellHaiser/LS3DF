// I3D_projector (0x10043730 - 0x100462b0; 0x10043f90 / 0x10043fc0 make and free the pending list,
// 0x100461b0 - 0x100462a0 are the static initialisers of this file's vectors).
#include "i3d/projector.h"

#include <cmath>
#include <new>

#include "core/debug.h"
#include "core/fpu.h"
#include "external/rw_data.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/loader.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/object.h"
#include "i3d/proj_geometry.h"
#include "i3d/render_helpers.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/singlemesh.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;
using ig::g_rs;

namespace {

// The projectors waiting for RenderProjectors (0x100be9bc, grow 0x20).
C_vector<I3D_projector *> s_pending(0x20);

// Build's walk (0x10045bd0): the projector's sector, world box, flags, sector mask and the next
// free receiver slot (0x100be9b8, 0x100be9b4, 0x100be990, 0x100be9d0, 0x100be9cc).
I3D_sector *s_buildSector;
const I3D_bbox *s_buildBox;
uint32_t s_buildFlags;
uint32_t s_buildSectorMask;
int s_numReceivers;                         // 0x100be9b0

I3D_sector *FindSector(I3D_frame *f)
{
    for (f = f->parent; f; f = f->parent) {
        if (f->type == FRAME_SECTOR)
            return static_cast<I3D_sector *>(f);
    }
    return nullptr;
}

// The texture the projector draws: its own, else its material's.
ITexture *DrawnTexture(I3D_projector *p)
{
    if (p->texture)
        return p->texture;
    I3D_material *m = p->material;
    if (m->matFlags & MATF_ANIMATED)
        m->UpdateTexture();
    return m->texture;
}

bool Receives(I3D_visual *v)
{
    switch (v->GetVisualType()) {
    case FourCC("LMAP"):
    case FourCC("MRPH"):
    case FourCC("SNGM"):
    case FourCC("SMMR"):
    case FourCC("OBJ_"):
        return (v->visualFlags & VISF_MODE_B1) != 0;
    default:
        return false;
    }
}

// 0x10045bd0
void GatherReceivers(I3D_frame *f)
{
    if (!(f->frameFlags & FRMFLAGS_ON))
        return;
    const I3D_bbox &fb = f->GetBound().bbox;
    const I3D_bbox &pb = *s_buildBox;
    if (pb.max.x < fb.min.x || pb.max.y < fb.min.y || pb.max.z < fb.min.z || fb.max.x < pb.min.x ||
        fb.max.y < pb.min.y || fb.max.z < pb.min.z)
        return;
    switch (f->type) {
    case FRAME_VISUAL: {
        auto *v = static_cast<I3D_visual *>(f);
        if (!Receives(v))
            break;
        const I3D_bbox &wb = v->GetWorldBound();
        if (wb.min.x <= pb.max.x && wb.min.y <= pb.max.y && wb.min.z <= pb.max.z && pb.min.x <= wb.max.x &&
            pb.min.y <= wb.max.y && pb.min.z <= wb.max.z) {
            if (s_numReceivers > 0x1ff)
                return;
            proj::g_receivers[s_numReceivers++] = v;
        }
        break;
    }
    case FRAME_SECTOR:
        if (f != s_buildSector) {
            if (s_buildFlags & 8)
                return;
            if (!(static_cast<uint32_t>(static_cast<I3D_sector *>(f)->environmentId) & s_buildSectorMask))
                return;
        }
        break;
    case FRAME_MODEL:
        if (s_buildFlags & 4)
            return;
        break;
    default:
        break;
    }
    if (!(s_buildFlags & 0x10)) {
        for (I3D_frame *c = f->firstChild; c; c = c->nextSibling)
            GatherReceivers(c);
    }
}

// The vertex color of a fade mode at depth z.
uint32_t PackColor(uint32_t a, float r, float g, float b)
{
    const uint32_t rr = static_cast<uint32_t>(Fistp(r * 255.0f));
    const uint32_t gg = static_cast<uint32_t>(Fistp(g * 255.0f));
    const uint32_t bb = static_cast<uint32_t>(Fistp(b * 255.0f));
    return (((a << 8 | rr) << 8) | gg) << 8 | bb;
}
float Clamp01(float f)
{
    if (f > 1.0f)
        return 1.0f;
    if (f < 0.0f)
        return 0.0f;
    return f;
}

}  // namespace

// 0x10043730
void I3D_projector::Init()
{
    I3D_visual::Init();
    texture = nullptr;
    material = nullptr;
    orthogonal = true;
    range = 100.0f;
    intensity = 1.0f;
    opacity = 0.117f;
    // (The box's w values come from an unset local.)
    localBound.bbox.min = S_vector(-1.0f, -1.0f, 0.0f);
    localBound.bbox.max = S_vector(1.0f, 1.0f, 1.0f);
    localBound.center = S_vector(0.0f, 0.0f, 0.5f);
    localBound.radius = 1.5f;
    buffered = 0;
    projFlags = 0;
    color = S_vector4{1.0f, 1.0f, 1.0f, 0.0f};
    mode = 0;
    receiverSum = 0;
    vertices = nullptr;
    vertexCapacity = 0;
    numVertices = 0;
    faces = nullptr;
    faceCapacity = 0;
    numOpaque = 0;
    numBlended = 0;
    blendedCenter = S_vector4{0.0f, 0.0f, 0.0f, 0.0f};
    numReceivers = 0;
    sector = nullptr;
    buildStamp = -1;
    vb = S_VBAlloc{};
    ib = S_IBAlloc{};
    visualFlags |= VISF_NO_ZWRITE;
}

// 0x100438f0
void I3D_projector::Uninit()
{
    if (texture)
        texture->Release();
    if (material)
        material->Release();
    ::operator delete(vertices);
    ::operator delete(faces);
    // (Fix: the original leaves the freed pointers.)
    vertices = nullptr;
    faces = nullptr;
    vertexCapacity = faceCapacity = 0;
    texture = nullptr;
    material = nullptr;
    FreeVB(&vb);
    FreeIB(&ib);
    vb = S_VBAlloc{};
    ib = S_IBAlloc{};
    I3D_frame::Uninit();
}

// 0x10043960
I3D_RESULT I3D_projector::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    switch (static_cast<I3D_visual *>(source)->GetVisualType()) {
    case FourCC("PROJ"): {
        auto *src = static_cast<I3D_projector *>(source);
        SetTexture(src->texture);
        SetMaterial(src->material);
        SetOrthogonal(src->orthogonal);
        range = src->range;
        SetColor(src->color);
        SetIntensity(src->intensity);
        SetOpacity(src->opacity);
        SetMode(src->mode);
        SetBuffered(src->buffered);
        sector = nullptr;
        return I3D_OK;
    }
    case FourCC("LENS"):
        SetMaterial(nullptr);
        return I3D_OK;
    case FourCC("LMAP"):
    case FourCC("BBRD"):
    case FourCC("MRPH"):
    case FourCC("SNGM"):
    case FourCC("SMMR"):
    case FourCC("OBJ_"): {
        I3D_mesh *m = static_cast<I3D_object *>(source)->mesh;
        if (m) {
            if (I3D_mesh_level *level = m->GetLevel(0)) {
                if (I3D_face_group *fg = level->GetFaceGroup(0))
                    SetMaterial(fg->material);
            }
        }
        return I3D_OK;
    }
    default:
        return I3D_OK;
    }
}

// 0x10043af0
void I3D_projector::SetTexture(ITexture *t)
{
    if (texture == t)
        return;
    if (texture)
        texture->Release();
    texture = t;
    if (t)
        ++t->refCount;
}

// 0x10043b20
void I3D_projector::SetMaterial(I3D_material *m)
{
    if (material == m)
        return;
    if (material)
        material->Release();
    material = m;
    if (m)
        ++m->refCount;
}

// 0x10043b50
void I3D_projector::SetOrthogonal(bool on)
{
    orthogonal = on;
    projFlags = 0;
    receiverSum = 0;
}

// 0x10043c50
void I3D_projector::SetIntensity(float value)
{
    intensity = value < 0.0f ? 0.0f : value;
    projFlags = 0;
    receiverSum = 0;
}

// 0x10043ca0
void I3D_projector::SetOpacity(float value)
{
    opacity = Clamp01(value);
    projFlags = 0;
    receiverSum = 0;
}

// 0x10043b70
void I3D_projector::SetColor(const S_vector4 &c)
{
    color = c;
    color.x = Clamp01(color.x);
    color.y = Clamp01(color.y);
    color.z = Clamp01(color.z);
    projFlags = 0;
    receiverSum = 0;
}

// 0x10043d10
void I3D_projector::SetMode(int newMode)
{
    mode = newMode;
    projFlags = 0;
    receiverSum = 0;
}

// 0x10043d30
void I3D_projector::SetBuffered(uint32_t value)
{
    buffered = value;
    projFlags = 0;
    receiverSum = 0;
}

// 0x10043d50
void I3D_projector::MatrixChanged()
{
    if (!(visualFlags & VISF_WORLD_SPACE) && (frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED))) {
        visualFlags &= ~(VISF_WBOUND_VALID | VISF_INVMATRIX_VALID);
        sector = nullptr;
    }
}

// 0x10046130
void I3D_projector::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &orthogonal, 1);
    dtaRead(g_loadFile, &mode, 1);
    int id = 0;
    dtaRead(g_loadFile, &id, 2);
    if (id > g_loadNumMaterials) {
        g_loadResult = I3DERR_FILECORRUPTED;
        return;
    }
    if (id > 0) {
        material = g_loadMaterials[id - 1];
        ++material->refCount;
    }
}

// 0x10043d80
void I3D_projector::QueueItems()
{
    auto push = [this](uint32_t sortKey) {
        S_renderItem &item = g_rc.items.PushBackUninit();
        item.visual = this;
        item.sector = g_rc.sector;
        item.texture = DrawnTexture(this);
        item.sortKey = sortKey;
        ++g_rc.numBlended;
    };
    if (numOpaque)
        push(0xbfffffffu);
    if (numBlended) {
        const I3D_camera *cam = g_renderCamera;
        const S_vector &n = cam->frustum[0].normal;
        const float z =
            (((blendedCenter.z * n.z + blendedCenter.y * n.y) + blendedCenter.x * n.x) + cam->frustumD[0]) * 100.0f;
        push((static_cast<uint32_t>(Fistp(z)) & RIKEY_DEPTH_MASK) | RIKEY_BLENDED);
    }
}

// 0x10043fe0
void I3D_projector::Prepare()
{
    if (!(frameFlags & FRMFLAGS_ON))
        return;
    if (!texture) {
        if (!material)
            return;
        if (material->matFlags & MATF_ANIMATED)
            material->UpdateTexture();
        if (!material->texture)
            return;
    }
    if (!sector && !(sector = FindSector(this)))
        return;
    const float r = g_fovScale * range;
    const S_vector &cp = g_renderCamera->GetMatrix()(3);
    const S_vector &pp = GetMatrix()(3);
    const S_vector d(pp.x - cp.x, pp.y - cp.y, pp.z - cp.z);
    if (r * r < SquareZXY(d))
        return;
    if (projFlags & PROJF_BUFFERED) {
        if (!numVertices)
            return;
        if (vb.vb) {
            QueueItems();
            return;
        }
        projFlags &= ~PROJF_BUFFERED;
    }
    s_pending.PushBack(this);
}

// 0x10045860
void I3D_projector::Rebuild()
{
    if (frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED))
        ++changeCount;
    if (buffered)
        projFlags |= PROJF_BUFFERED;
    s_numReceivers = 0;
    const I3D_bbox &pb = GetWorldBound();
    // (Sectors sharing an environment bit count as the projector's.)
    const uint32_t mask = static_cast<uint32_t>(sector->environmentId);
    // The receivers among the frame's queued visuals.
    I3D_visual *last = nullptr;
    for (int i = 0; i < g_rc.items.Size(); ++i) {
        const S_renderItem &item = g_rc.items[i];
        I3D_visual *v = item.visual;
        if (v == last || !v)
            continue;
        last = v;
        if (!Receives(v))
            continue;
        if (item.sector != sector && !(static_cast<uint32_t>(item.sector->environmentId) & mask))
            continue;
        const I3D_bbox &wb = v->GetWorldBound();
        if (wb.min.x > pb.max.x || wb.min.y > pb.max.y || wb.min.z > pb.max.z || wb.max.x < pb.min.x ||
            wb.max.y < pb.min.y || wb.max.z < pb.min.z)
            continue;
        proj::g_receivers[s_numReceivers++] = v;
        if (s_numReceivers >= 0x200)
            break;
    }
    if (!s_numReceivers) {
        numVertices = 0;
        numReceivers = 0;
        return;
    }
    int sum = 0;
    for (int i = 0; i < s_numReceivers; ++i) {
        auto *o = static_cast<I3D_object *>(proj::g_receivers[i]);
        sum += o->changeCount + ((o->visualFlags & VISF_TEXTURE_LOD) ? 0 : o->textureLevel);
    }
    if (buildStamp != changeCount || s_numReceivers != numReceivers || sum != receiverSum) {
        buildStamp = changeCount;
        numReceivers = s_numReceivers;
        receiverSum = sum;
        BuildGeometry();
        QueueItems();
        return;
    }
    if (numVertices)
        QueueItems();
}

// 0x10045ab0
void I3D_projector::Build(uint32_t flags, I3D_frame *root)
{
    if (!sector && !(sector = FindSector(this)))
        return;
    s_buildSector = sector;
    buffered = 3;
    projFlags |= PROJF_BUFFERED;
    s_numReceivers = 0;
    s_buildBox = &GetWorldBound();
    s_buildFlags = flags;
    s_buildSectorMask = static_cast<uint32_t>(sector->environmentId);
    if (!root && !(flags & 8)) {
        I3D_scene *scene = sector->scene;
        if (!scene) {
            numVertices = 0;
            return;
        }
        GatherReceivers(scene->primarySector);
        if (flags & 2)
            GatherReceivers(scene->omniSector);
    } else {
        GatherReceivers(root ? root : sector);
    }
    if (s_numReceivers) {
        BuildGeometry();
        return;
    }
    numVertices = 0;
}

// 0x100441c0
void I3D_projector::BuildGeometry()
{
    proj::Begin();
    const float offset = DriverStateFloat(DRVSTATE_PROJECTOR_OFFSET);
    const bool additive = mode == 0 || mode == 4 || mode == 8;
    const S_matrix &world = (visualFlags & VISF_WORLD_SPACE) ? kIdentityMatrix : GetMatrix();
    auto inverse = [this]() -> const S_matrix & {
        if (visualFlags & VISF_WORLD_SPACE)
            return kIdentityMatrix;
        if (!(visualFlags & VISF_INVMATRIX_VALID)) {
            visualFlags |= VISF_INVMATRIX_VALID;
            invWorldMatrix.Inverse(GetMatrix());
        }
        return invWorldMatrix;
    };
    proj::Setup(&inverse(), &world, orthogonal, offset, additive ? 0.0f : offset, opacity);

    for (int r = 0; r < s_numReceivers; ++r) {
        auto *o = static_cast<I3D_object *>(proj::g_receivers[r]);
        // Inside the volume when a corner of the box is inside each of its sides.
        const S_vector4 *corners = BoxCorners(o->GetLocalBound().bbox);
        const S_matrix m = o->GetMatrix() * inverse();
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
            if (p.z > 0.0f)
                sides |= 0x10;
            if (p.z < 1.0f)
                sides |= 0x20;
        }
        if (sides != 0x3f || !o->mesh)
            continue;
        const int levelIndex = (o->visualFlags & VISF_TEXTURE_LOD) ? 0 : o->textureLevel;
        I3D_mesh_level *level = o->mesh->GetLevel(levelIndex);
        if (!level)
            continue;
        const uint32_t withBlended = o->visualFlags & VISF_MODE_B2;
        const bool worldSpace = (o->visualFlags & VISF_WORLD_SPACE) != 0;
        const uint32_t visType = o->GetVisualType();
        if (visType == FourCC("SNGM") || visType == FourCC("SMMR")) {
            const S_vector4 *skinned = static_cast<I3D_singlemesh *>(o)->SkinnedVertices(levelIndex);
            if (!skinned)
                continue;
            if (worldSpace)
                proj::ProjectWorldMesh4(skinned, level->numVertices, level->faceGroups, level->NumFaces(), withBlended);
            else
                proj::ProjectMesh4(skinned, level->numVertices, level->faceGroups, level->NumFaces(), withBlended,
                                   &o->GetMatrix());
        } else if (worldSpace) {
            const int numFaces = level->NumFaces();
            proj::ProjectWorldMesh(level->LockVertices(1), level->numVertices, level->faceGroups, numFaces,
                                   withBlended);
        } else {
            const S_matrix *w = &o->GetMatrix();
            const int numFaces = level->NumFaces();
            proj::ProjectMesh(level->LockVertices(1), level->numVertices, level->faceGroups, numFaces, withBlended, w);
        }
    }

    const int n = proj::NumVertices();
    if (!n)
        numVertices = 0;
    S_projectedVertex *dst = nullptr;
    if (!buffered) {
        if (vb.buffer) {
            FreeVB(&vb);
            vb.buffer = nullptr;
            vertexCapacity = 0;
            FreeIB(&ib);
            ib.buffer = nullptr;
            faceCapacity = 0;
        }
        if (vertexCapacity < n) {
            ::operator delete(vertices);
            vertexCapacity = n + 16 < 100 ? 100 : n + 16;
            vertices = static_cast<S_projectedVertex *>(
                ::operator new(sizeof(S_projectedVertex) * vertexCapacity, std::nothrow));
            if (!vertices) {
                vertexCapacity = 0;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
            }
        }
        dst = vertices;
    } else {
        if (vertices) {
            ::operator delete(vertices);
            ::operator delete(faces);
            vertices = nullptr;
            vertexCapacity = 0;
            faces = nullptr;
            faceCapacity = 0;
        }
        if (vertexCapacity < n || !vb.vb) {
            FreeVB(&vb);
            vertexCapacity = n + 8;
            if (!g_vbManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, sizeof(S_projectedVertex), vertexCapacity, &vb, 2,
                                   false)) {
                vertexCapacity = 0;
                vb.buffer = nullptr;
            }
        }
        // (Fix: the original locks a failed allocation.)
        if (vb.buffer)
            dst = reinterpret_cast<S_projectedVertex *>(vb.buffer->Lock(0) + vb.buffer->stride * vb.offset);
    }
    numVertices = n;
    if (!dst) {
        numVertices = 0;
        return;
    }

    // The vertices: volume position, texture coordinates by the projection, the color faded by
    // the mode over the depth.
    const S_vector4 *src = proj::Vertices();
    for (int i = 0; i < n; ++i, ++dst, ++src) {
        dst->x = src->x;
        dst->y = src->y;
        dst->z = src->z;
        if (orthogonal) {
            dst->u = (src->x + 1.0f) * 0.5f;
            dst->v = 0.5f - src->y * 0.5f;
        } else if (IsTinyBits(src->z)) {
            // (Both written to u, v left as it was: the original's.)
            dst->u = src->x <= 0.0f ? 0.0f : 1.0f;
            dst->u = src->y <= 0.0f ? 1.0f : 0.0f;
        } else {
            const float s = 0.5f / src->z;
            dst->u = s * src->x + 0.5f;
            dst->v = 0.5f - s * src->y;
        }
        switch (mode) {
        case 4:
        case 5: {
            const float f = Clamp01((1.0f - src->z) * intensity);
            dst->color = mode == 4 ? PackColor(0xff, f * color.x, f * color.y, f * color.z)
                                   : PackColor(static_cast<uint32_t>(Fistp(f * 255.0f)), color.x, color.y, color.z);
            break;
        }
        case 8:
        case 9: {
            const float t = (0.5f - std::fabs(src->z - 0.5f)) * intensity;
            const float f = Clamp01(t + t);
            dst->color = mode == 8 ? PackColor(0xff, f * color.x, f * color.y, f * color.z)
                                   : PackColor(static_cast<uint32_t>(Fistp(f * 255.0f)), color.x, color.y, color.z);
            break;
        }
        default: {
            const float a = 1.0f <= intensity ? 1.0f : intensity;
            dst->color = PackColor(static_cast<uint32_t>(Fistp(a * 255.0f)), color.x, color.y, color.z);
            break;
        }
        }
    }

    // The faces: the opaque materials', then the blended ones'.
    const int no = proj::NumOpaqueFaces(), nb = proj::NumBlendedFaces();
    uint16_t *dstFaces = nullptr;
    if (!buffered) {
        if (faceCapacity < nb + no) {
            ::operator delete(faces);
            faceCapacity = nb + 16 + no < 100 ? 100 : nb + 16 + no;
            faces = static_cast<uint16_t *>(::operator new(static_cast<size_t>(faceCapacity) * 6, std::nothrow));
            if (!faces) {
                faceCapacity = 0;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
            }
        }
        dstFaces = faces;
    } else {
        if (faceCapacity < nb + no) {
            FreeIB(&ib);
            faceCapacity = nb + 4 + no;
            if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, faceCapacity * 3, &ib)) {
                ib.buffer = nullptr;
                faceCapacity = 0;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
            }
        }
        // (Fix: the original locks a failed allocation.)
        if (ib.buffer)
            dstFaces = ib.buffer->Lock(0) + ib.offset;
    }
    if (!dstFaces) {
        numVertices = 0;
        return;
    }
    std::memcpy(dstFaces, proj::OpaqueFaces(), static_cast<size_t>(no) * 6);
    numOpaque = no;
    std::memcpy(dstFaces + no * 3, proj::BlendedFaces(), static_cast<size_t>(nb) * 6);
    numBlended = nb;
    if (proj::BlendedSumCount() > 0) {
        const S_vector &s = proj::BlendedSum();
        const float inv = 1.0f / static_cast<float>(proj::BlendedSumCount());
        const S_vector c(s.x * inv, s.y * inv, inv * s.z);
        const S_vector w = c * GetMatrix();
        blendedCenter = S_vector4{w.x, w.y, w.z, blendedCenter.w};
    }
}

// 0x10045dd0
void I3D_projector::Draw(const S_renderItem *item)
{
    if (!numVertices)
        return;
    if (g_rs.colorOp[0] != D3DTOP_MODULATE) {
        g_rs.colorOp[0] = D3DTOP_MODULATE;
        g_pGfx->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    }
    if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
        g_rs.colorOp[1] = D3DTOP_DISABLE;
        g_pGfx->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (g_rs.alpha != 1) {
        g_rs.alpha = 1;
        g_pGfx->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pGfx->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_pGfx->SetRenderState(D3DRS_ALPHAREF, 1);
    }
    if (mode == 0 || mode == 4 || mode == 8) {
        if (g_rs.blend != 2) {
            g_rs.blend = 2;
            g_pGfx->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            g_pGfx->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        }
    } else {
        if (g_rs.blend != 0) {
            g_rs.blend = 0;
            g_pGfx->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_pGfx->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        }
        if (g_rs.alphaOp[0] != D3DTOP_MODULATE) {
            g_rs.alphaOp[0] = D3DTOP_MODULATE;
            g_pGfx->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        }
    }
    if (g_rs.cull != 3) {
        g_rs.cull = 3;
        g_pGfx->SetRenderState(D3DRS_CULLMODE, g_rs.cullDefault);
    }
    g_pGfx->SetTexture(0, DrawnTexture(this)->deviceTexture);
    g_pIGraph->SetWorldMatrix(&GetMatrix());
    SetZBias(g_projectorZBias);
    g_pGfx->SetRenderState(D3DRS_LIGHTING, FALSE);
    // (Both queued items have the texture, so both draw the opaque faces: the original's.)
    const bool opaque = item->texture != nullptr;
    if (!(projFlags & PROJF_BUFFERED)) {
        if (opaque)
            g_pIGraph->DrawIndexedPrimitive(IGP_TRIANGLES, numVertices, faces, numOpaque, vertices, IGV_COLOR_TEX);
        else
            g_pIGraph->DrawIndexedPrimitive(IGP_TRIANGLES, numVertices, faces + numOpaque * 3, numBlended, vertices,
                                            IGV_COLOR_TEX);
    } else {
        SetVertexFormat(2);
        g_pGfx->SetStreamSource(0, vb.vb, sizeof(S_projectedVertex));
        // (The blended faces offset the base vertex, not the first index: the original's.)
        g_pGfx->SetIndices(ib.ib, (opaque ? 0 : numOpaque * 3) + vb.offset);
        g_pGfx->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, numVertices, ib.offset, opaque ? numOpaque : numBlended);
    }
    g_pGfx->SetRenderState(D3DRS_LIGHTING, TRUE);
}

namespace ls3d::i3d {

// 0x10044190
void RenderProjectors()
{
    for (I3D_projector *p : s_pending)
        p->Rebuild();
    s_pending.Clear();
}

}  // namespace ls3d::i3d
