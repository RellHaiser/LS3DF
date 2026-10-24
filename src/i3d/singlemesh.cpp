// I3D_singlemesh and I3D_singlemorph (0x10051dc0 - 0x10054240; the skinning core 0x10065c60).
// (0x10053bb0 / 0x10053bd0 and 0x100542c0 / 0x100542e0 are the static initialisers of this
// file's copies of a header's zero vector and identity quaternion.)
#include "i3d/singlemesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "external/rw_data.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/joint.h"
#include "i3d/loader.h"
#include "i3d/render_helpers.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace ls3d::i3d {

S_matrix g_jointMatrices[kMaxSkinJoints];

}  // namespace ls3d::i3d

namespace {

// SkinnedVertices' buffer (0x100c0500 / 0x100c0504).
S_vector4 *s_skinned;
int s_skinnedCapacity;

// FixJoints' joints found (0x100c03e0 / 0x100bf3d0).
I3D_joint *s_found[kMaxSkinJoints];
int s_numFound;

// The skinning matrix of a 1-based joint number (null: none). (Fix: the original indexes its 64
// matrices unchecked; 0 reads the 64 bytes before them, numbers above 64 past them.)
const S_matrix *JointMatrix(int joint)
{
    return joint >= 1 && joint <= kMaxSkinJoints ? &g_jointMatrices[joint - 1] : nullptr;
}

// The parent of a 1-based joint number (0: none).
int ParentJoint(const S_skinData *skin, int joint)
{
    return joint >= 1 && joint <= kMaxSkinJoints ? skin->parents[joint - 1] : 0;
}

S_skinData *NewSkinData()
{
    auto *s = static_cast<S_skinData *>(AlignedAlloc(sizeof(S_skinData), 16));
    if (!s)
        return nullptr;
    s->refCount = 1;
    s->numJoints = 0;
    s->joints = nullptr;
    for (int &n : s->numWeighted)
        n = 0;
    for (uint8_t *&vj : s->vertexJoints)
        vj = nullptr;
    s->bounds.Invalidate();
    return s;
}

// A skin data reference dropped (the last frees it).
void ReleaseSkin(S_skinData *s)
{
    if (--s->refCount < 0 || s->refCount)
        return;
    AlignedFree(s->joints);
    for (uint8_t *vj : s->vertexJoints)
        ::operator delete(vj);
    AlignedFree(s);
}

// 0x10052960: the joints in `frame`'s subtree, each child list up to its first skinned visual.
void CollectJoints(I3D_frame *frame)
{
    for (I3D_frame *f = frame->firstChild; f; f = f->nextSibling) {
        if (f->type == FRAME_VISUAL) {
            const uint32_t t = static_cast<I3D_visual *>(f)->GetVisualType();
            if (t == FourCC("SNGM") || t == FourCC("SMMR"))
                return;
        } else if (f->type == FRAME_JOINT) {
            if (s_numFound >= kMaxSkinJoints)
                return;
            s_found[s_numFound++] = static_cast<I3D_joint *>(f);
        }
        CollectJoints(f);
    }
}

// 0x10065c60: `count` vertices (0x20 bytes: position, normal, ...) skinned into `dst` (position
// and normal every `dstStride` bytes): by the vertex's joint, blended with the joint's parent by
// the vertex's weight.
void SkinVertices(uint8_t *dst, uint32_t dstStride, const float *src, int count,
                  const uint8_t *vertexJoints, const S_skinData *skin)
{
    for (; count; --count, dst += dstStride, src += 8, vertexJoints += 2) {
        const float x = src[0], y = src[1], z = src[2];
        const float nx = src[3], ny = src[4], nz = src[5];
        float p1x = x, p1y = y, p1z = z, n1x = nx, n1y = ny, n1z = nz;
        if (const S_matrix *m1 = JointMatrix(vertexJoints[0])) {
            const float(*M)[4] = m1->m;
            p1x = ((x * M[0][0] + y * M[1][0]) + z * M[2][0]) + M[3][0];
            p1y = ((M[2][1] * z + M[1][1] * y) + M[0][1] * x) + M[3][1];
            p1z = ((z * M[2][2] + y * M[1][2]) + x * M[0][2]) + M[3][2];
            n1x = (ny * M[1][0] + nx * M[0][0]) + nz * M[2][0];
            n1y = (M[2][1] * nz + M[1][1] * ny) + M[0][1] * nx;
            n1z = (nz * M[2][2] + ny * M[1][2]) + nx * M[0][2];
        }
        float *o = reinterpret_cast<float *>(dst);
        if (!vertexJoints[1]) {
            o[0] = p1x;
            o[1] = p1y;
            o[2] = p1z;
            o[3] = n1x;
            o[4] = n1y;
            o[5] = n1z;
            continue;
        }
        const float f = static_cast<float>(vertexJoints[1]) * 0.00390625f;
        float p2x = x, p2y = y, p2z = z, n2x = nx, n2y = ny, n2z = nz;
        if (const S_matrix *m2 = JointMatrix(ParentJoint(skin, vertexJoints[0]))) {
            const float(*M)[4] = m2->m;
            p2x = ((M[1][0] * y + M[0][0] * x) + z * M[2][0]) + M[3][0];
            p2y = ((M[2][1] * z + M[1][1] * y) + M[0][1] * x) + M[3][1];
            p2z = ((z * M[2][2] + y * M[1][2]) + x * M[0][2]) + M[3][2];
            n2x = (M[0][0] * nx + nz * M[2][0]) + ny * M[1][0];
            n2y = (M[2][1] * nz + M[1][1] * ny) + M[0][1] * nx;
            n2z = (nz * M[2][2] + ny * M[1][2]) + nx * M[0][2];
        }
        const float g = 1.0f - f;
        o[0] = p1x * g + p2x * f;
        o[2] = g * p1z + p2z * f;
        o[1] = g * p1y + p2y * f;
        o[3] = n1x * g + n2x * f;
        o[5] = g * n1z + n2z * f;
        o[4] = n1y * g + n2y * f;
    }
}

// SkinnedVertices' position transform (its own sum order).
S_vector SkinPosition(const S_matrix &m, float x, float y, float z)
{
    const float(*M)[4] = m.m;
    const float px = ((M[1][0] * y + M[2][0] * z) + M[0][0] * x) + M[3][0];
    const float py = ((M[1][1] * y + M[0][1] * x) + M[2][1] * z) + M[3][1];
    const float pz = ((M[1][2] * y + M[0][2] * x) + M[2][2] * z) + M[3][2];
    return S_vector(px, py, pz);
}

}  // namespace

// I3D_singlemesh -----------------------------------------------------------------------------------

// 0x10051dc0
void I3D_singlemesh::Init()
{
    I3D_object::Init();
    numJoints = 0;
    joints = nullptr;
    jointChanges = -300;
    boundStamp = -400;
    skin = nullptr;
    for (S_skinLevel *&l : levels)
        l = nullptr;
}

void I3D_singlemesh::ClearJoints()
{
    for (int i = 0; i < numJoints; ++i) {
        if (joints[i])
            joints[i]->mesh = nullptr;
    }
    ::operator delete(joints);
    joints = nullptr;
    numJoints = 0;
}

void I3D_singlemesh::FreeLevels()
{
    for (S_skinLevel *&l : levels) {
        if (l) {
            FreeVB(&l->vb);
            delete l;
            l = nullptr;
        }
    }
}

// 0x10051e10
void I3D_singlemesh::Uninit()
{
    ClearJoints();
    FreeLevels();
    if (skin) {
        ReleaseSkin(skin);
        skin = nullptr;
    }
    I3D_object::Uninit();
}

// 0x10051f20
void I3D_singlemesh::RefreshLevel(int level)
{
    if (!mesh)
        return;
    if (S_skinLevel *l = levels[level])
        l->flags |= SKINLEVEL_STALE;
    ++jointChanges;
}

// 0x10051ee0
void I3D_singlemesh::RefreshLevels()
{
    if (!mesh)
        return;
    for (int i = static_cast<int>(mesh->numLevels) - 1; i >= 0; --i) {
        if (S_skinLevel *l = levels[i])
            l->flags |= SKINLEVEL_STALE;
    }
    ++jointChanges;
}

// 0x10051f50
const I3D_bbox *I3D_singlemesh::GetJointBBox(int joint)
{
    if (joint >= numJoints || !skin)
        return nullptr;
    if (joint < 0)
        return &skin->bounds;
    if (skin->joints && joint < skin->numJoints)
        return &skin->joints[joint].box;
    return nullptr;
}

// 0x10051fa0
void I3D_singlemesh::ReleaseLevel(int level)
{
    S_skinLevel *l = levels[level];
    if (l && l->vb.vb) {
        FreeVB(&l->vb);
        l->vb = S_VBAlloc{};
    }
    I3D_object::RefreshLevel(level);
    ++jointChanges;
}

// 0x10051ff0
void I3D_singlemesh::ReleaseLevels()
{
    for (S_skinLevel *l : levels) {
        if (l && l->vb.vb) {
            FreeVB(&l->vb);
            l->vb = S_VBAlloc{};
        }
    }
    I3D_object::RefreshLevels();
    ++jointChanges;
}

// 0x10052050
void I3D_singlemesh::CopySkin(const I3D_singlemesh &src)
{
    S_skinData *s = src.skin;
    if (s)
        ++s->refCount;
    // (Fix: the original drops the reference it had without releasing it.)
    if (skin)
        ReleaseSkin(skin);
    skin = s;
}

// 0x10052070
I3D_RESULT I3D_singlemesh::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    ClearParentBounds();
    I3D_frame::Duplicate(source);
    ++jointChanges;
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    ClearJoints();
    FreeLevels();
    auto *src = static_cast<I3D_object *>(source);
    switch (src->GetVisualType()) {
    case FourCC("SNGM"):
    case FourCC("SMMR"):
        // (This visual's own mirrored flag decides; a mirror's copy takes the mesh only.)
        if (!(visualFlags & VISF_MIRRORED)) {
            lightLevel = src->lightLevel;
            alpha = src->alpha;
            SetMesh(src->mesh, 0);
            CopySkin(*static_cast<I3D_singlemesh *>(src));
            break;
        }
        [[fallthrough]];
    case FourCC("LMAP"):
    case FourCC("BBRD"):
    case FourCC("MRPH"):
    case FourCC("OBJ_"):
        SetMesh(src->mesh, 0);
        lightLevel = src->lightLevel;
        alpha = src->alpha;
        break;
    case FourCC("LENS"):
        SetMesh(nullptr, 0);
        break;
    default:
        return I3DERR_GENERIC;
    }
    visualFlags = src->visualFlags & 0x2ff52;
    localBound = src->localBound;
    if (!(src->visualFlags & VISF_WORLD_SPACE)) {
        visualFlags &= ~VISF_WBOUND_VALID;
    } else {
        src->GetWorldBound();
        worldBound = src->worldBound;
    }
    return I3D_OK;
}

// 0x10052300
void I3D_singlemesh::UpdateLocalBound()
{
    if (boundStamp != jointChanges) {
        ComputeBound();
        return;
    }
    visualFlags |= VISF_BOUND_VALID;
}

// 0x10052330
void I3D_singlemesh::ComputeBound()
{
    boundStamp = jointChanges;
    ++changeCount;
    if (!joints && !(visualFlags & VISF_MIRRORED))
        FixJoints();
    if (!mesh) {
        localBound.bbox.Invalidate();
        localBound.radius = 0.0f;
    } else if (!skin) {
        std::memcpy(&localBound.bbox, &mesh->bboxMin, 0x20);
        localBound.center = mesh->center;
        localBound.radius = mesh->radius;
    } else {
        I3D_bbox box = skin->bounds;
        if (joints && skin->joints) {
            // (Fix: the original walks this visual's joint count of the skin's joints.)
            const int n = std::min(numJoints, skin->numJoints);
            for (int i = 0; i < n; ++i) {
                I3D_joint *joint = joints[i];
                const I3D_bbox &jb = skin->joints[i].box;
                if (!joint || !jb.IsValid())
                    continue;
                const S_matrix &m = joint->GetModelMatrix();
                for (int c = 0; c < 8; ++c)
                    box.Expand(jb.Corner(c) * m);
            }
        }
        localBound.bbox = box;
        if (box.IsValid()) {
            const S_vector d(box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z);
            const S_vector h(d.x * 0.5f, d.y * 0.5f, d.z * 0.5f);
            localBound.center = box.min + h;
            localBound.radius = std::sqrt((h.z * h.z + h.y * h.y) + h.x * h.x);
        } else {
            localBound.radius = 0.0f;
        }
        visualFlags = (visualFlags & ~VISF_WBOUND_VALID) | VISF_BOUND_VALID;
        ClearParentBounds();
        return;
    }
    visualFlags = (visualFlags & ~VISF_WBOUND_VALID) | VISF_BOUND_VALID;
}

// 0x100527d0
bool I3D_singlemesh::FixJoints()
{
    ClearJoints();
    if (!mesh)
        return false;
    s_numFound = 0;
    CollectJoints(this);
    if (!s_numFound)
        return false;
    joints = static_cast<I3D_joint **>(::operator new(sizeof(I3D_joint *) * s_numFound, std::nothrow));
    if (!joints) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return false;
    }
    for (int i = 0; i < s_numFound; ++i)
        joints[i] = nullptr;
    numJoints = s_numFound;
    for (int i = 0; i < numJoints; ++i) {
        for (int k = 0; k < numJoints; ++k) {
            if (s_found[k]->jointIndex == i) {
                joints[i] = s_found[k];
                s_found[k]->mesh = this;
                break;
            }
        }
    }
    ComputeBound();
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
    return true;
}

// 0x100529c0
I3D_joint *I3D_singlemesh::GetVertexJoint(int vertex, int level)
{
    if (!skin)
        return nullptr;
    const uint8_t *vj = skin->vertexJoints[level];
    if (!vj)
        return nullptr;
    const int j = vj[vertex * 2];
    if (!j || j > numJoints)
        return nullptr;
    return joints[j - 1];
}

void I3D_singlemesh::ComputeJointMatrices()
{
    const S_skinJoint *sj = skin->joints;
    if (!sj)
        return;
    // (Fix: the original walks this visual's joint count of the skin's joints.)
    const int n = std::min(numJoints, skin->numJoints);
    for (int i = 0; i < n; ++i) {
        if (I3D_joint *joint = joints[i])
            g_jointMatrices[i] = sj[i].bindInverse * joint->GetModelMatrix();
        else
            g_jointMatrices[i].Identity();
    }
}

// 0x10052a10
const S_vector4 *I3D_singlemesh::SkinnedVertices(int levelIndex)
{
    if (!joints && !(visualFlags & VISF_MIRRORED))
        FixJoints();
    // (Fix: the original reads the mesh unchecked.)
    if (!mesh)
        return nullptr;
    I3D_mesh_level *level = mesh->levels[levelIndex];
    if (!level || !skin)
        return nullptr;
    if (s_skinnedCapacity < level->numVertices) {
        AlignedFree(s_skinned);
        s_skinnedCapacity = std::max(level->numVertices + 100, 500);
        s_skinned = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * s_skinnedCapacity, 16));
        if (!s_skinned) {
            s_skinnedCapacity = 0;
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return nullptr;
        }
    }
    ComputeJointMatrices();
    C_VertexBuffer *vb = level->vertices.buffer;
    const float *v = reinterpret_cast<const float *>(vb->Lock(D3DLOCK_READONLY) + vb->stride * level->vertices.offset);
    const int numWeighted = skin->numWeighted[levelIndex];
    const uint8_t *vj = skin->vertexJoints[levelIndex];
    S_vector4 *out = s_skinned;
    for (int i = numWeighted; i; --i, ++out, v += 8, vj += 2) {
        S_vector p1(v[0], v[1], v[2]);
        if (const S_matrix *m1 = JointMatrix(vj[0]))
            p1 = SkinPosition(*m1, v[0], v[1], v[2]);
        // (The original leaves w as it finds it on the stack.)
        out->w = 0.0f;
        if (!vj[1]) {
            out->x = p1.x;
            out->y = p1.y;
            out->z = p1.z;
            continue;
        }
        const float f = static_cast<float>(vj[1]) * 0.00390625f;
        S_vector p2(v[0], v[1], v[2]);
        if (const S_matrix *m2 = JointMatrix(ParentJoint(skin, vj[0])))
            p2 = SkinPosition(*m2, v[0], v[1], v[2]);
        const float g = 1.0f - f;
        out->x = g * p1.x + p2.x * f;
        out->y = g * p1.y + p2.y * f;
        out->z = g * p1.z + p2.z * f;
    }
    for (int i = level->numVertices - numWeighted; i > 0; --i, ++out, v += 8) {
        out->x = v[0];
        out->y = v[1];
        out->z = v[2];
        out->w = v[3];
    }
    return s_skinned;
}

// 0x10052fa0
void I3D_singlemesh::SkinLevel(int levelIndex)
{
    if (!joints && !(visualFlags & VISF_MIRRORED))
        FixJoints();
    S_skinLevel *sl = levels[levelIndex];
    if (!sl)
        return;
    sl->stamp = jointChanges;
    // (Fix: the original reads the mesh unchecked; Prepare has one.)
    if (!mesh)
        return;
    I3D_mesh_level *level = mesh->levels[levelIndex];
    if (!level)
        return;
    if (visualFlags & VISF_MIRRORED) {
        static_cast<I3D_singlemesh *>(data)->SkinLevel(levelIndex);
        return;
    }
    if (!sl->vb.buffer)
        level->CreateRenderVB(1, &GetMatrix(), &sl->vb);
    if (!skin)
        return;
    ComputeJointMatrices();
    const int n = skin->numWeighted[levelIndex];
    if (!n)
        return;
    C_VertexBuffer *db = sl->vb.buffer;
    uint8_t *dst = db->Lock(0) + db->stride * sl->vb.offset;
    C_VertexBuffer *sb = level->vertices.buffer;
    const uint8_t *src = sb->Lock(D3DLOCK_READONLY) + sb->stride * level->vertices.offset;
    SkinVertices(dst, db->stride, reinterpret_cast<const float *>(src), n, skin->vertexJoints[levelIndex], skin);
}

// 0x100533b0
void I3D_singlemesh::Prepare()
{
    if (!mesh)
        return;
    if (PrepareMirrored())
        return;
    const float distance2 = LodDistance2();
    const int levelIndex = SelectLevel(distance2);
    I3D_mesh_level *level = mesh->levels[levelIndex];
    if (!level || !level->faceGroups || !level->numVertices)
        return;
    S_skinLevel *&sl = levels[levelIndex];
    if (!sl) {
        sl = new (std::nothrow) S_skinLevel;
        if (!sl) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return;
        }
    }
    if (sl->flags & SKINLEVEL_STALE) {
        if (sl->vb.vb)
            level->FillRenderVB(false, &GetMatrix(), &sl->vb);
        sl->flags &= ~SKINLEVEL_STALE;
        ++jointChanges;
    }
    // (The original then copies a mirror's copy's joint count from its original: not reached.)
    if (sl->stamp != jointChanges || !sl->vb.vb)
        SkinLevel(levelIndex);
    SetLodFade(levelIndex, level, distance2);
    SetLightSector(g_rc.sector);
    SelectLights(this, lights, 1);
    QueueFaceGroups(level, nullptr);
}

// 0x10053900
void I3D_singlemesh::Load()
{
    I3D_object::Load();
    if (!mesh)
        return;
    if (skin) {
        ReleaseSkin(skin);
        // (Fix: the original keeps the released pointer when the file has no skin data.)
        skin = nullptr;
    }
    int count = 0;
    dtaRead(g_loadFile, &count, 1);
    if (!count)
        return;
    auto fail = [] {
        g_loadResult = I3DERR_OUTOFMEM;
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
    };
    S_skinData *s = NewSkinData();
    skin = s;
    if (!s) {
        fail();
        return;
    }
    dtaRead(g_loadFile, &s->bounds, 0x20);
    s->numJoints = count;
    // (Fix: the original reads all the parents into the 64 bytes there are.)
    uint8_t parents[256];
    dtaRead(g_loadFile, parents, count);
    std::memcpy(s->parents, parents, std::min(count, kMaxSkinJoints));
    auto *sj = static_cast<S_skinJoint *>(AlignedAlloc(sizeof(S_skinJoint) * count, 16));
    if (!sj) {
        fail();
        return;
    }
    s->joints = sj;
    for (int i = count; i; --i)
        dtaRead(g_loadFile, sj++, sizeof(S_skinJoint));
    for (uint32_t i = 0; i < mesh->numLevels; ++i) {
        I3D_mesh_level *level = mesh->GetLevel(i);
        if (!level)
            continue;
        // (The original makes room for the level's vertices before it reads the count. Fix: room
        // for as many as the file gives.)
        dtaRead(g_loadFile, &s->numWeighted[i], 4);
        const int numWeighted = s->numWeighted[i];
        const int room = std::max(level->numVertices, numWeighted);
        auto *vj = static_cast<uint8_t *>(::operator new(static_cast<size_t>(room) * 2, std::nothrow));
        s->vertexJoints[i] = vj;
        if (!vj) {
            fail();
            return;
        }
        if (numWeighted > 0)
            dtaRead(g_loadFile, vj, numWeighted * 2);
        const int rest = level->numVertices - numWeighted;
        if (rest > 0)
            std::memset(vj + numWeighted * 2, 0, static_cast<size_t>(rest) * 2);
    }
}

// I3D_singlemorph ----------------------------------------------------------------------------------

// 0x10053c00
void I3D_singlemorph::Init()
{
    I3D_singlemesh::Init();
    C_morph::Init();
}

// 0x10053c20
void I3D_singlemorph::Uninit()
{
    C_morph::Uninit();
    I3D_singlemesh::Uninit();
}

// 0x10053c60
I3D_RESULT I3D_singlemorph::SetMorphWeight(int channel, int target, float weight)
{
    if (channel < 0 || channel >= numChannels || target < 0 || target >= numTargets)
        return I3DERR_INVALIDPARAMS;
    ++jointChanges;
    visualFlags &= ~VISF_WBOUND_VALID;
    SetWeight(channel, target, weight);
    return I3D_OK;
}

// 0x10053cc0
float I3D_singlemorph::GetMorphWeight(int channel, int target)
{
    // (Out of range the original returns what the FPU held.)
    if (channel < 0 || channel >= numChannels || target < 0 || target >= numTargets)
        return 0.0f;
    return GetWeight(channel, target);
}

// 0x10053d00
I3D_RESULT I3D_singlemorph::SetMesh(I3D_mesh *newMesh, uint32_t flags)
{
    I3D_object::SetMesh(newMesh, flags);
    if (def)
        localBound = def->bounds;
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
    return I3D_OK;
}

// 0x10053d70
I3D_RESULT I3D_singlemorph::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    visualFlags &= ~VISF_WBOUND_VALID;
    I3D_frame::Duplicate(source);
    ++jointChanges;
    morphFlags &= ~(MORPHF_DATA | MORPHF_NEW);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    C_morph::Uninit();
    ClearJoints();
    FreeLevels();
    // (The source, not this visual: the original's.)
    source->MatrixChanged();
    auto *src = static_cast<I3D_object *>(source);
    // Another visual's morph data and a mesh copy of our own (the vertices are morphed in place).
    auto copyMorph = [this, src](const C_morph &m) -> I3D_RESULT {
        if (!(m.morphFlags & MORPHF_DATA)) {
            SetMesh(src->mesh, 0);
            return I3D_OK;
        }
        I3D_RESULT r = CopyFrom(m);
        if (r < 0) {
            SetMesh(src->mesh, 0);
            return r;
        }
        I3D_mesh *copy = g_driver->CreateMesh();
        if (!copy) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        r = copy->Duplicate(src->mesh);
        if (r < 0) {
            copy->Release();
            return r;
        }
        morphFlags |= MORPHF_DATA;
        SetMesh(copy, 0);
        copy->Release();
        return I3D_OK;
    };
    // (This visual's own mirrored flag decides; a mirror's copy takes the mesh only.)
    const bool own = !(visualFlags & VISF_MIRRORED);
    bool generic = false;
    switch (src->GetVisualType()) {
    case FourCC("MRPH"):
        if (own) {
            lightLevel = src->lightLevel;
            alpha = src->alpha;
            const I3D_RESULT r = copyMorph(*static_cast<I3D_morph *>(src));
            if (r < 0)
                return r;
        } else {
            generic = true;
        }
        break;
    case FourCC("SNGM"):
        // (The skin data without the mesh: the original's.)
        if (own) {
            lightLevel = src->lightLevel;
            alpha = src->alpha;
            CopySkin(*static_cast<I3D_singlemesh *>(src));
        } else {
            generic = true;
        }
        break;
    case FourCC("SMMR"):
        if (own) {
            lightLevel = src->lightLevel;
            alpha = src->alpha;
            auto *s = static_cast<I3D_singlemorph *>(src);
            const I3D_RESULT r = copyMorph(*s);
            if (r < 0)
                return r;
            CopySkin(*s);
        } else {
            generic = true;
        }
        break;
    case FourCC("LENS"):
        SetMesh(nullptr, 0);
        break;
    case FourCC("LMAP"):
    case FourCC("BBRD"):
    case FourCC("OBJ_"):
        generic = true;
        break;
    default:
        return I3DERR_GENERIC;
    }
    if (generic) {
        SetMesh(src->mesh, 0);
        lightLevel = src->lightLevel;
        alpha = src->alpha;
    }
    visualFlags = src->visualFlags & 0x2ff52;
    localBound = src->localBound;
    if (!(src->visualFlags & VISF_WORLD_SPACE)) {
        visualFlags &= ~VISF_WBOUND_VALID;
    } else {
        src->GetWorldBound();
        worldBound = src->worldBound;
    }
    return I3D_OK;
}

// 0x100541e0
void I3D_singlemorph::SkinLevel(int level)
{
    Apply(level, mesh);
    I3D_singlemesh::SkinLevel(level);
}

// 0x10054210
const S_vector4 *I3D_singlemorph::SkinnedVertices(int level)
{
    Apply(level, mesh);
    return I3D_singlemesh::SkinnedVertices(level);
}

// 0x10054240
void I3D_singlemorph::Load()
{
    morphFlags &= ~MORPHF_NEW;
    I3D_singlemesh::Load();
    if (g_loadResult != I3D_OK)
        return;
    C_morph::Load();
    if (def)
        localBound = def->bounds;
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
}

namespace ls3d::i3d {

const S_vector4 *SkinnedVertices(I3D_frame *visual, int level)
{
    return static_cast<I3D_singlemesh *>(visual)->SkinnedVertices(level);
}

void SkinFixJoints(I3D_frame *visual) { static_cast<I3D_singlemesh *>(visual)->FixJoints(); }

// 0x10053b90
void FreeSkinBuffers()
{
    AlignedFree(s_skinned);
    s_skinned = nullptr;
    s_skinnedCapacity = 0;
}

}  // namespace ls3d::i3d
