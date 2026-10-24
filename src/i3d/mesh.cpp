// I3D_face_group, I3D_mesh_level, I3D_mesh (0x10030990 - 0x10032230).
#include "i3d/mesh.h"

#include <cmath>
#include <cstring>

#include "core/debug.h"
#include "i3d/driver.h"
#include "i3d/material.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

// An empty box: its minimum above its maximum.
void ResetBox(S_vector4 &lo, S_vector4 &hi)
{
    lo.x = lo.y = lo.z = 1e16f;
    hi.x = hi.y = hi.z = -1e16f;
}

// D3DUSAGE_SOFTWAREPROCESSING: the meshes' vertex and index buffers live in the managed pool.
constexpr uint32_t kMeshUsage = D3DUSAGE_SOFTWAREPROCESSING;

}  // namespace

// ---------------------------------------------------------------- I3D_face_group

I3D_face_group::I3D_face_group() : material(nullptr), numFaces(0), next(nullptr), faces{} {}

I3D_face_group::~I3D_face_group()
{
    if (material)
        material->Release();
    FreeIB(&faces);
    delete next;
}

void I3D_face_group::SetMaterial(I3D_material *newMaterial)
{
    if (newMaterial == material)
        return;
    if (material)
        material->Release();
    material = newMaterial;
    if (newMaterial)
        ++newMaterial->refCount;
}

I3D_triface *I3D_face_group::SetFaces(const I3D_triface *src, int count)
{
    if (!src) {
        FreeIB(&faces);
        faces = S_IBAlloc{};
        numFaces = 0;
        return nullptr;
    }
    I3D_triface *dst = AllocFaces(count);
    if (dst)                                // (the original copies into null: AllocFaces' failure is fatal)
        std::memcpy(dst, src, sizeof(I3D_triface) * static_cast<size_t>(count));
    numFaces = count;
    return dst;
}

I3D_triface *I3D_face_group::AllocFaces(int count)
{
    FreeIB(&faces);
    faces = S_IBAlloc{};
    if (!g_ibManager.Alloc(kMeshUsage, static_cast<uint32_t>(count * 3), &faces)) {
        numFaces = 0;
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return nullptr;
    }
    numFaces = count;
    return Lock(0);
}

I3D_triface *I3D_face_group::Lock(uint32_t flags)
{
    if (!numFaces)
        return nullptr;
    uint16_t *data = faces.buffer->Lock(flags & 1 ? D3DLOCK_READONLY : 0);
    return reinterpret_cast<I3D_triface *>(data + faces.offset);
}

void I3D_face_group::Unlock()
{
    if (faces.buffer)                       // (the original reads through a null buffer)
        faces.buffer->Unlock();
}

I3D_RESULT I3D_face_group::Duplicate(I3D_face_group *src)
{
    I3D_triface *dst = AllocFaces(src->numFaces);
    const I3D_triface *from = src->Lock(1);
    std::memcpy(dst, from, sizeof(I3D_triface) * static_cast<size_t>(src->numFaces));
    numFaces = src->numFaces;
    SetMaterial(src->material);
    return I3D_OK;
}

I3D_RESULT I3D_face_group::DuplicateMaterial()
{
    if (!material || material->refCount < 2)
        return I3D_OK;
    I3D_material *copy = material->matFlags & MATF_ANIMATED ? g_driver->CreateAnimMaterial() : g_driver->CreateMaterial();
    if (!copy) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    const I3D_RESULT r = copy->CopyFrom(material);
    if (r < 0) {
        copy->Release();
        return r;
    }
    material->Release();
    material = copy;
    return I3D_OK;
}

// ---------------------------------------------------------------- I3D_mesh_level

void *I3D_mesh_level::operator new(size_t size) noexcept { return AlignedAlloc(size, 16); }
void I3D_mesh_level::operator delete(void *p) { AlignedFree(p); }

I3D_mesh_level::I3D_mesh_level()
    : refCount(1), vertices{}, renderVertices{}, extraData(nullptr), extraFormat(0), numVertices(0),
      numFaceGroups(0), faceGroups(nullptr), lodDistance(0.0f), flags(MLF_RENDER_STALE),
      envScale(2.0f * std::sqrt(2.0f))
{
    ResetBox(bboxMin, bboxMax);
}

I3D_mesh_level::~I3D_mesh_level()
{
    FreeVB(&renderVertices);
    renderVertices = S_VBAlloc{};
    delete faceGroups;
    ::operator delete(extraData);
    if (!numVertices && vertices.buffer) {
        // (A level without vertices that still has a buffer pointer: the original deletes the
        // pointer as a plain block.)
        ::operator delete(vertices.buffer);
    } else {
        FreeVB(&vertices);
    }
}

uint32_t I3D_mesh_level::Release()
{
    if (--refCount)
        return static_cast<uint32_t>(refCount);
    delete this;
    return 0;
}

void *I3D_mesh_level::LockVertices(uint32_t lockFlags)
{
    if (!vertices.vb)
        return nullptr;
    C_VertexBuffer *b = vertices.buffer;
    return b->Lock(lockFlags & 1 ? D3DLOCK_READONLY : 0) + b->stride * vertices.offset;
}

void I3D_mesh_level::UnlockVertices()
{
    if (vertices.vb && vertices.buffer->lockedData)
        vertices.buffer->Unlock();
}

I3D_RESULT I3D_mesh_level::Duplicate(I3D_mesh_level *src)
{
    if (src == this)
        return I3D_OK;
    FreeRenderVB();
    delete faceGroups;
    // (The original leaves the deleted list here when the source has no face groups or a copy
    // fails.)
    faceGroups = nullptr;
    numFaceGroups = 0;
    FreeVB(&vertices);
    vertices = S_VBAlloc{};
    if (!src) {
        numVertices = 0;
        ResetBox(bboxMin, bboxMax);
        lodDistance = 0.0f;
        return I3D_OK;
    }
    flags = src->flags;
    envScale = src->envScale;
    std::memcpy(&lodDistance, &src->lodDistance, 4);
    I3D_RESULT r = SetVertices(src->LockVertices(1), src->numVertices);
    if (r < 0)
        return r;
    SetExtraFormat(src->extraFormat);
    if (extraFormat)
        std::memcpy(extraData, src->extraData, static_cast<size_t>(ExtraStride() * src->numVertices));
    if (src->faceGroups) {
        I3D_face_group *last = nullptr;
        for (I3D_face_group *s = src->faceGroups; s; s = s->next) {
            auto *g = new I3D_face_group;
            r = g->Duplicate(s);
            if (r < 0) {
                delete g;
                return r;
            }
            if (last) {
                last->next = g;
                ++numFaceGroups;
            } else {
                faceGroups = g;
                numFaceGroups = 1;
            }
            last = g;
        }
    }
    bboxMin = src->bboxMin;
    bboxMax = src->bboxMax;
    return I3D_OK;
}

int I3D_mesh_level::NumFaces()
{
    int n = 0;
    for (I3D_face_group *g = faceGroups; g; g = g->next)
        n += g->numFaces;
    return n;
}

I3D_face_group *I3D_mesh_level::GetFaceGroup(int index)
{
    if (index >= numFaceGroups)
        return nullptr;
    I3D_face_group *g = faceGroups;
    for (; index > 0; --index)
        g = g->next;
    return g;
}

I3D_RESULT I3D_mesh_level::GetStats(S_meshStats *out)
{
    out->size = 0x70;
    out->numVertices = numVertices;
    out->vertexBytes = numVertices << 5;
    const int n = NumFaces();
    out->numFaces = n;
    out->faceBytes = n * 6;
    return I3D_OK;
}

void I3D_mesh_level::AddFaceGroup(I3D_face_group *group)
{
    if (!group)
        return;
    if (group->material && (group->material->matFlags & MATF_ENV_TEXTURE)) {
        flags |= MLF_ENV;
        const uint32_t source = group->material->matFlags >> 12;
        if ((source & 7) > 1)
            flags = (flags & ~MLF_ENV_SOURCE_MASK) | (source & MLF_ENV_SOURCE_MASK);
        envScale = 2.0f * std::sqrt(2.0f) / static_cast<float>(static_cast<int>(group->material->matFlags & 0x7f));
    }
    group->next = faceGroups;
    numFaceGroups = faceGroups ? numFaceGroups + 1 : 1;
    faceGroups = group;
}

I3D_RESULT I3D_mesh_level::RemoveFaceGroup(I3D_face_group *group)
{
    if (!group || !faceGroups)
        return I3DERR_OBJECTNOTFOUND;
    if (faceGroups == group) {
        faceGroups = group->next;
        group->next = nullptr;
        if (!--numFaceGroups)
            flags &= ~MLF_ENV;
        return I3D_OK;
    }
    for (I3D_face_group *g = faceGroups; g->next; g = g->next) {
        if (g->next == group) {
            // (The original clears only the group's own link: the group stays in the list and the
            // ones after it are lost.)
            g->next = group->next;
            group->next = nullptr;
            --numFaceGroups;
            return I3D_OK;
        }
    }
    return I3DERR_OBJECTNOTFOUND;
}

I3D_RESULT I3D_mesh_level::SetVertices(const void *src, int count)
{
    if (count != numVertices && renderVertices.vb)
        FreeRenderVB();
    FillRenderVB(true, nullptr, nullptr);
    FreeVB(&vertices);
    vertices = S_VBAlloc{};
    if (!src) {
        numVertices = 0;
        ResetBox(bboxMin, bboxMax);
        return I3D_OK;
    }
    if (!g_vbManager.Alloc(kMeshUsage, 0x20, static_cast<uint32_t>(count), &vertices, 8, false)) {
        numVertices = 0;
        ResetBox(bboxMin, bboxMax);
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    C_VertexBuffer *b = vertices.buffer;
    const uint32_t offset = vertices.offset;
    std::memcpy(b->Lock(0) + b->stride * offset, src, static_cast<size_t>(count) * 0x20);
    numVertices = count;
    ComputeBounds();
    return I3D_OK;
}

void *I3D_mesh_level::AllocVertices(int count)
{
    if (count != numVertices && renderVertices.vb)
        FreeRenderVB();
    FillRenderVB(true, nullptr, nullptr);
    FreeVB(&vertices);
    vertices = S_VBAlloc{};
    if (!count) {
        numVertices = 0;
        ResetBox(bboxMin, bboxMax);
        return nullptr;
    }
    if (!g_vbManager.Alloc(kMeshUsage, 0x20, static_cast<uint32_t>(count), &vertices, 8, false)) {
        numVertices = 0;
        ResetBox(bboxMin, bboxMax);
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return nullptr;
    }
    numVertices = count;
    C_VertexBuffer *b = vertices.buffer;
    return b->Lock(0) + b->stride * vertices.offset;
}

void I3D_mesh_level::ComputeBounds()
{
    const auto *v = static_cast<const uint8_t *>(LockVertices(1));
    if (!v) {
        ResetBox(bboxMin, bboxMax);
        return;
    }
    std::memcpy(&bboxMax, v, sizeof(S_vector4));
    std::memcpy(&bboxMin, v, sizeof(S_vector4));
    // (The original runs 2^32 - 1 times for a buffer with no vertices.)
    for (int i = 1; i < numVertices; ++i) {
        const auto *p = reinterpret_cast<const S_vector *>(v + 0x20 * i);
        if (bboxMin.x > p->x)
            bboxMin.x = p->x;
        if (bboxMax.x < p->x)
            bboxMax.x = p->x;
        if (bboxMin.y > p->y)
            bboxMin.y = p->y;
        if (bboxMax.y < p->y)
            bboxMax.y = p->y;
        if (bboxMin.z > p->z)
            bboxMin.z = p->z;
        if (bboxMax.z < p->z)
            bboxMax.z = p->z;
    }
}

bool I3D_mesh_level::CreateRenderVB(uint32_t vbFlags, const S_matrix *envMatrix, S_VBAlloc *dest)
{
    uint32_t usage = vbFlags & 1 ? D3DUSAGE_WRITEONLY : D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC;
    if (vbFlags & 2)
        usage |= D3DUSAGE_SOFTWAREPROCESSING;
    // With environment coordinates (8 more bytes); positions only: 8 bytes less (the original's
    // test for source 5 adds nothing to "source above 1").
    const bool env = (flags & MLF_ENV_SOURCE_MASK) > 1 && !(vbFlags & 0x10);
    uint32_t stride, format;
    if (!(vbFlags & 0x20) && !(extraFormat & 1)) {
        stride = env ? 0x28 : 0x20;
        format = env ? 1 : 0;
    } else {
        stride = env ? 0x20 : 0x18;
        format = env ? 3 : 2;
    }
    if (!dest)
        dest = &renderVertices;
    if (!g_vbManager.Alloc(usage, stride, static_cast<uint32_t>(numVertices), dest, format, false)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return false;
    }
    if (!(vbFlags & 8))
        FillRenderVB(false, envMatrix, dest);
    return true;
}

void I3D_mesh_level::FreeRenderVB()
{
    FreeVB(&renderVertices);
    renderVertices = S_VBAlloc{};
}

void I3D_mesh_level::FillRenderVB(bool deferred, const S_matrix *envMatrix, S_VBAlloc *dest)
{
    if (!dest)
        dest = &renderVertices;
    if (deferred || !dest->vb) {
        flags |= MLF_RENDER_STALE;
        return;
    }
    C_VertexBuffer *sb = vertices.buffer;
    const uint8_t *src = sb->Lock(D3DLOCK_READONLY) + sb->stride * vertices.offset;
    C_VertexBuffer *db = dest->buffer;
    uint8_t *dst = db->Lock(0) + db->stride * dest->offset;
    const uint32_t source = flags & MLF_ENV_SOURCE_MASK;
    if ((flags & MLF_ENV) && source >= 2) {
        if (source <= 4) {
            MakeEnvCoordinates(dst, 0x28, src, 0x20, numVertices, envMatrix, envScale, static_cast<int>(source));
        } else if (source == 5) {
            const uint8_t *extra = extraData;
            const int es = ExtraStride();
            if (!(extraFormat & 1)) {
                for (int i = numVertices; i != 0; --i) {
                    std::memcpy(dst, src, 0x20);
                    std::memcpy(dst + 0x20, extra, static_cast<size_t>(es));
                    extra += es;
                    src += 0x20;
                    dst += 0x20 + es;
                }
            }
        }
    } else if (extraFormat & 1) {
        // The position and the four extra bytes (16 bytes a vertex, whatever the buffer's stride).
        const uint8_t *extra = extraData;
        for (int i = numVertices; i != 0; --i) {
            std::memcpy(dst, src, 12);
            std::memcpy(dst + 12, extra, 4);
            dst += 16;
            src += 0x20;
            extra += 4;
        }
    } else {
        std::memcpy(dst, src, static_cast<size_t>(numVertices) << 5);
    }
    flags &= ~MLF_RENDER_STALE;
}

void *I3D_mesh_level::SetExtraFormat(uint32_t format)
{
    extraFormat = format;
    if (extraData) {
        ::operator delete(extraData);
        extraData = nullptr;
    }
    if (format && numVertices) {
        extraData = static_cast<uint8_t *>(::operator new(static_cast<size_t>(numVertices * ExtraStride())));
        return extraData;
    }
    return nullptr;
}

int I3D_mesh_level::ExtraStride()
{
    int n = extraFormat & 1 ? 4 : 0;
    if (extraFormat & 2)
        return n + 8;
    if (extraFormat & 4)
        return n + 0x10;
    if (extraFormat & 8)
        n += 0x18;
    return n;
}

// ---------------------------------------------------------------- I3D_mesh

void *I3D_mesh::operator new(size_t size) noexcept { return AlignedAlloc(size, 16); }
void I3D_mesh::operator delete(void *p) { AlignedFree(p); }

I3D_mesh::I3D_mesh() : center{}, radius(0.0f), refCount(1), numLevels(0), levels{}
{
    ResetBox(bboxMin, bboxMax);
}

I3D_mesh::~I3D_mesh()
{
    // (The original releases the counted levels only: those SetLevel(null) cut off leak.)
    for (I3D_mesh_level *l : levels) {
        if (l)
            l->Release();
    }
}

uint32_t I3D_mesh::Release()
{
    if (--refCount)
        return static_cast<uint32_t>(refCount);
    delete this;
    return 0;
}

I3D_mesh_level *I3D_mesh::GetLevel(int index) { return levels[index]; }

I3D_RESULT I3D_mesh::SetLevel(I3D_mesh_level *level, int index)
{
    if (index >= kMaxLevels)
        return I3DERR_INVALIDPARAMS;
    I3D_mesh_level *&slot = levels[index];
    if (slot == level) {
        if (level)
            ComputeBounds();
        return I3D_OK;
    }
    if (slot)
        slot->Release();
    slot = level;
    const uint32_t i = static_cast<uint32_t>(index);
    if (level) {
        ++level->refCount;
        if (numLevels <= i)
            numLevels = i + 1;
    } else if (numLevels > i) {
        numLevels = i;
    }
    ComputeBounds();
    return I3D_OK;
}

I3D_RESULT I3D_mesh::Duplicate(I3D_mesh *src)
{
    if (src == this)
        return I3D_OK;
    for (I3D_mesh_level *&l : levels) {
        if (l)
            l->Release();
        l = nullptr;
    }
    numLevels = 0;
    if (!src) {
        ResetBox(bboxMin, bboxMax);
        center = S_vector{};
        radius = 0.0f;
        return I3D_OK;
    }
    for (int i = 0; i < kMaxLevels; ++i) {
        if (!src->levels[i])
            continue;
        I3D_mesh_level *l = g_driver->CreateMeshLevel();
        if (!l) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        const I3D_RESULT r = l->Duplicate(src->levels[i]);
        if (r < 0) {
            l->Release();                   // (leaked in the original)
            return r;
        }
        SetLevel(l, i);
        l->Release();
    }
    return I3D_OK;
}

I3D_RESULT I3D_mesh::GetStats(S_meshStats *out)
{
    out->size = 0x70;
    int verts = 0, faces = 0;
    for (uint32_t i = 0; i < numLevels; ++i) {
        if (!levels[i])                     // (a gap below the last level: the original reads through null)
            continue;
        verts += levels[i]->numVertices;
        faces += levels[i]->NumFaces();
    }
    out->numVertices = verts;
    out->vertexBytes = verts << 5;
    out->numFaces = faces;
    out->faceBytes = faces * 6;
    return I3D_OK;
}

void I3D_mesh::ComputeBounds()
{
    ResetBox(bboxMin, bboxMax);
    for (int i = static_cast<int>(numLevels) - 1; i >= 0; --i) {
        const I3D_mesh_level *l = levels[i];
        if (!l || !(l->bboxMin.x <= l->bboxMax.x))
            continue;
        if (bboxMin.x > l->bboxMin.x)
            bboxMin.x = l->bboxMin.x;
        if (bboxMax.x < l->bboxMax.x)
            bboxMax.x = l->bboxMax.x;
        if (bboxMin.y > l->bboxMin.y)
            bboxMin.y = l->bboxMin.y;
        if (bboxMax.y < l->bboxMax.y)
            bboxMax.y = l->bboxMax.y;
        if (bboxMin.z > l->bboxMin.z)
            bboxMin.z = l->bboxMin.z;
        if (bboxMax.z < l->bboxMax.z)
            bboxMax.z = l->bboxMax.z;
    }
    if (!(bboxMin.x <= bboxMax.x)) {
        radius = 0.0f;
        return;
    }
    const float hx = (bboxMax.x - bboxMin.x) * 0.5f;
    const float hy = (bboxMax.y - bboxMin.y) * 0.5f;
    const float hz = (bboxMax.z - bboxMin.z) * 0.5f;
    center = S_vector(hx + bboxMin.x, hy + bboxMin.y, hz + bboxMin.z);
    radius = std::sqrt((hz * hz + hy * hy) + hx * hx);
}

I3D_RESULT I3D_mesh::DuplicateMaterials()
{
    for (I3D_mesh_level *l : levels) {
        if (!l)
            continue;
        for (I3D_face_group *g = l->faceGroups; g; g = g->next) {
            const I3D_RESULT r = g->DuplicateMaterial();
            if (r < 0)
                return r;
        }
    }
    return I3D_OK;
}

bool I3D_mesh::SameLayout(const I3D_mesh *other)
{
    if (numLevels != other->numLevels)
        return false;
    for (int i = 0; i < kMaxLevels; ++i) {
        const I3D_mesh_level *a = levels[i], *b = other->levels[i];
        if (!a ? b != nullptr : !b || a->numVertices != b->numVertices)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- environment coordinates

namespace ls3d::i3d {

void MakeEnvCoordinates(uint8_t *dst, int dstStride, const uint8_t *src, int srcStride, int count,
                        const S_matrix *matrix, float scale, int source)
{
    if (source < 2 || source > 4)
        return;
    const float k = DriverStateFloat(DRVSTATE_ENV_UV_SCALE) * scale * 0.3535534f;
    for (int n = count; n != 0; --n) {
        const S_vector r = *reinterpret_cast<const S_vector *>(src) * *matrix;
        std::memcpy(dst, src, 0x20);
        auto *uv = reinterpret_cast<float *>(dst + 0x20);
        switch (source) {
        case 2:
            uv[0] = r.x * k;
            uv[1] = r.z * k;
            break;
        case 3:
            uv[0] = r.y * k;
            uv[1] = r.z * k;
            break;
        default:
            uv[0] = r.x * k;
            uv[1] = r.y * k;
            break;
        }
        dst += dstStride;
        src += srcStride;
    }
}

}  // namespace ls3d::i3d
