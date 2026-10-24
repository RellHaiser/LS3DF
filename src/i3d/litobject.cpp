// I3D_lit_object and its lightmap levels (0x100232d0 - 0x1002b030, but Construct and its helpers:
// lightmap_build.cpp). 0x10023390 is the static initialiser of this file's copy of a header's
// (0, 0, 0, 1); 0x1002af30 / 0x1002af60 are vector constructors (grow 0x20 / 2) of the visual
// factory.
#include "i3d/litobject.h"

#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "core/fpu.h"
#include "external/rw_data.h"
#include "i3d/driver.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/render_helpers.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ig::g_pDevice;
using ig::g_rs;

namespace {

// The cached D3DRS_SPECULARENABLE (IGraph's g_rs.specular; inlined).
void SetSpecular(int on)
{
    if (g_rs.specular == on)
        return;
    g_rs.specular = on;
    g_pDevice->SetRenderState(D3DRS_SPECULARENABLE, on);
}

}  // namespace

// ---------------------------------------------------------------- S_lightmapLevel

// (The original leaves the pointers freed here as they were, and the part count at -1: its
// callers delete the level.)
void S_lightmapLevel::Free()
{
    ::operator delete(uvs);
    uvs = nullptr;
    ::operator delete(unknown08);
    unknown08 = nullptr;
    delete lightmap;
    lightmap = nullptr;
    if (faceParts) {
        for (int i = 0; i < numFaceGroups; ++i)
            ::operator delete(faceParts[i]);
        ::operator delete(faceParts);
    }
    faceParts = nullptr;
    for (int i = 0; i < numParts; ++i) {
        S_lightmapPart &p = parts[i];
        // (Fix: the original frees only patches wider than 3, so narrower ones' pixels leak.)
        if (!p.IsUniform()) {
            ::operator delete(p.pixels);
            ::operator delete(p.texels);
        }
    }
    numParts = 0;
    ::operator delete(parts);
    parts = nullptr;
    FreeIB(&faces);
    faces = {};
    FreeVB(&vertices);
    vertices = {};
    ::operator delete(vertexMap);
    vertexMap = nullptr;
    ::operator delete(colors);
    colors = nullptr;
    textureBytes = 0;
}

void S_lightmapLevel::SetFaceParts(uint16_t *p, int faceGroup)
{
    if (faceGroup < numFaceGroups)
        faceParts[faceGroup] = p;
}

bool S_lightmapLevel::Build(I3D_mesh_level *level, bool keepFaces, const I3D_frame *owner)
{
    lightmap = new (std::nothrow) C_lightmap;
    if (!lightmap) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return false;
    }
    auto *indices = static_cast<uint16_t *>(::operator new(sizeof(uint16_t) * static_cast<size_t>(numParts)));
    for (int i = 0; i < numParts; ++i)
        indices[i] = static_cast<uint16_t>(i);
    textureBytes = 0;
    if (!lightmap->Create(indices, parts, numParts, &textureBytes, owner)) {
        ::operator delete(indices);
        return false;
    }
    ::operator delete(indices);
    if (!uvs)
        return false;
    // Each vertex's uv in its patch to the texture: the patch's texel centers, turned for a
    // turned patch (u + 2).
    const ITexture *t = lightmap->texture;
    const float invWidth = 1.0f / static_cast<float>(t->width);
    const float invHeight = 1.0f / static_cast<float>(t->height);
    S_lightmapUV *uv = uvs;
    for (int i = 0; i < numVertices; ++i, ++uv) {
        level->LockVertices(0);             // (every vertex; the result is not used)
        const uint16_t partIndex = vertexMap[i].part;
        if (partIndex >= numParts)
            return false;
        const S_lightmapPart &p = parts[partIndex];
        const float u0 = uv->u;
        const float v0 = uv->v;
        if (!(p.u >= 2.0f)) {
            uv->u = (static_cast<float>(p.width - 1) * u0 + 0.5f) * invWidth + p.u;
            uv->v = (static_cast<float>(p.height - 1) * v0 + 0.5f) * invHeight + p.v;
        } else {
            uv->u = ((static_cast<float>(p.height - 1) * v0 * invWidth + p.u) - 2.0f) + invWidth * 0.5f;
            uv->v = (static_cast<float>(p.width - 1) * u0 + 0.5f) * invHeight + p.v;
        }
    }
    if (keepFaces)
        return true;
    // The faces: each corner the lightmap vertex of its vertex in the face's patch (0 when none).
    if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, static_cast<uint32_t>(level->NumFaces() * 3), &faces)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return false;
    }
    uint16_t *dst = faces.buffer->Lock(0) + faces.offset;
    int groupIndex = 0;
    for (I3D_face_group *fg = level->faceGroups; fg; fg = fg->next, ++groupIndex) {
        const uint16_t *facePart = faceParts[groupIndex];
        const int n = fg->numFaces;
        const uint16_t *tri = reinterpret_cast<const uint16_t *>(fg->Lock(1));
        for (int f = 0; f < n; ++f, tri += 3, ++facePart) {
            const uint16_t part = *facePart;
            for (int c = 0; c < 3; ++c) {
                uint16_t found = 0;
                for (uint16_t k = 0; k < numVertices; ++k) {
                    if (vertexMap[k].vertex == tri[c] && vertexMap[k].part == part) {
                        found = k;
                        break;
                    }
                }
                *dst++ = found;
            }
        }
    }
    return true;
}

bool S_lightmapLevel::FillVertices(I3D_mesh_level *level, I3D_mesh *, uint32_t flags)
{
    if (dirty) {
        dirty = false;
        if ((flags & LMSET_TEXTURES) && vertices.vb) {
            FreeVB(&vertices);
            vertices = {};
        }
    }
    C_VertexBuffer *srcBuffer = level->vertices.buffer;
    if (flags & LMSET_VERTEX_COLORS) {
        // The level's vertices (position, colour, uv) with the baked colours.
        if (!vertices.vb && !level->CreateRenderVB(0x29, nullptr, &vertices))
            return false;
        const uint8_t *src = srcBuffer->Lock(D3DLOCK_READONLY) + srcBuffer->stride * level->vertices.offset;
        C_VertexBuffer *dstBuffer = vertices.buffer;
        uint8_t *dst = dstBuffer->Lock(0) + dstBuffer->stride * vertices.offset;
        const uint32_t *color = colors;
        for (int i = numVertices; i != 0; --i, src += 0x20, dst += 0x18) {
            std::memcpy(dst, src, 12);
            std::memcpy(dst + 12, color++, 4);
            std::memcpy(dst + 16, src + 0x18, 8);
        }
        return true;
    }
    // The lightmap vertices: position, normal, lightmap uv, uv.
    // (Fix: a set of neither kind made dirty allocates again over its buffer; the original leaks it.)
    if (vertices.vb) {
        FreeVB(&vertices);
        vertices = {};
    }
    if (!g_vbManager.Alloc(D3DUSAGE_WRITEONLY, 0x28, static_cast<uint32_t>(numVertices), &vertices, 1, false)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return false;
    }
    const uint8_t *src = srcBuffer->Lock(D3DLOCK_READONLY) + srcBuffer->stride * level->vertices.offset;
    C_VertexBuffer *dstBuffer = vertices.buffer;
    uint8_t *dst = dstBuffer->Lock(0) + dstBuffer->stride * vertices.offset;
    for (int i = 0; i < numVertices; ++i, dst += 0x28) {
        const uint8_t *s = src + static_cast<size_t>(vertexMap[i].vertex) * 0x20;
        std::memcpy(dst, s, 0x18);
        std::memcpy(dst + 0x18, &uvs[i], 8);
        std::memcpy(dst + 0x20, s + 0x18, 8);
    }
    return true;
}

// ---------------------------------------------------------------- I3D_lit_object

void I3D_lit_object::Init()
{
    I3D_object::Init();
    currentSet = 0;
    for (S_lightmapSet &s : sets) {
        s.loaded = false;
        s.flags = LMSET_TEXTURES;
        s.resolution = 5.0f;
        s.smoothAngle = 45.0f;
        for (S_lightmapLevel *&l : s.levels)
            l = nullptr;
    }
}

void I3D_lit_object::Uninit()
{
    for (S_lightmapSet &s : sets) {
        for (S_lightmapLevel *&l : s.levels) {
            delete l;
            l = nullptr;
        }
    }
    I3D_object::Uninit();
}

I3D_RESULT I3D_lit_object::FreeLightmaps(int)
{
    S_lightmapSet &s = CurrentSet();
    s.loaded = false;
    for (S_lightmapLevel *&l : s.levels) {
        delete l;
        l = nullptr;
    }
    return I3D_OK;
}

I3D_RESULT I3D_lit_object::SaveLightmaps(int)
{
    dbgPrintf("NEMUZETE UKLADAT LM, KDYZ JE #define LS3D_FINAL !!!!!!!");
    return I3DERR_GENERIC;
}

I3D_RESULT I3D_lit_object::SetSetFlags(uint32_t flags)
{
    FreeLightmaps(0);
    CurrentSet().flags = flags;
    return I3D_OK;
}

void I3D_lit_object::SetCurrentSet(int set)
{
    // (Fix: the original takes any number, and later indexes past the sets with it.)
    if (set < 0 || set >= 6)
        return;
    for (S_lightmapLevel *l : CurrentSet().levels) {
        if (l)
            l->dirty = true;
    }
    currentSet = set;
}

int I3D_lit_object::GetTextureBytes()
{
    int bytes = 0;
    if (!mesh)
        return 0;
    for (const S_lightmapSet &s : sets) {
        for (uint32_t i = 0; i < mesh->numLevels; ++i) {
            if (s.levels[i])
                bytes += s.levels[i]->textureBytes;
        }
    }
    return bytes;
}

S_lightmapLevel *I3D_lit_object::LevelAt(int level, int set)
{
    // (Fix: the original indexes the sets and levels with any numbers.)
    if (set < 0 || set >= 6 || level < 0 || level >= I3D_mesh::kMaxLevels)
        return nullptr;
    return sets[set].levels[level];
}

int I3D_lit_object::GetNumParts(int level, int set)
{
    const S_lightmapLevel *l = LevelAt(level, set);
    return l ? l->numParts : 0;
}

S_lightmapPart *I3D_lit_object::GetParts(int level, int set)
{
    S_lightmapLevel *l = LevelAt(level, set);
    return l ? l->parts : nullptr;
}

void I3D_lit_object::RewriteParts(int level, int set)
{
    if (!g_driverStates[DRVSTATE_LM_TRUECOLOR] || !ig::g_fmtTrueColor)
        return;
    S_lightmapLevel *l = LevelAt(level, set);
    if (!l || !l->lightmap)                 // (fix: a level without patches has no lightmap)
        return;
    ITexture *texture = l->lightmap->texture;
    int pitch = 0;
    auto *bits = static_cast<uint8_t *>(texture->Lock(nullptr, &pitch, 0));
    if (!bits)
        return;
    for (int i = 0; i < l->numParts; ++i) {
        const S_lightmapPart &p = l->parts[i];
        const bool turned = !(p.u < 2.0f);
        const int x = turned ? Ftol(static_cast<float>(texture->width) * (p.u - 2.0f))
                             : Ftol(static_cast<float>(texture->width) * p.u);
        const int y = Ftol(static_cast<float>(texture->height) * p.v);
        uint8_t *d = bits + y * pitch + x * 4;
        if (p.IsUniform()) {
            for (int r = 0; r < 3; ++r, d += pitch - p.width * 4) {
                for (int c = 0; c < 3; ++c, d += 4) {
                    d[0] = static_cast<uint8_t>(p.color);
                    d[1] = static_cast<uint8_t>(p.color >> 8);
                    d[2] = static_cast<uint8_t>(p.color >> 16);
                }
            }
            continue;
        }
        // (Fix: the atlas frees the pixels of placed patches, which the original reads here.)
        if (!p.pixels)
            continue;
        // The pixels at the patch's size in the atlas (resized when it shrank).
        uint8_t *resized = nullptr;
        const uint8_t *src = p.pixels;
        if (p.sourceWidth != p.width || p.sourceHeight != p.height) {
            g_pIGraph->ResizeImage(p.sourceWidth, p.sourceHeight, p.sourceWidth * 3, p.pixels, p.width, p.height,
                                   p.width * 3, &resized, 3, true);
            src = resized;
        }
        if (src) {
            if (turned) {
                for (int r = 0; r < p.height; ++r) {
                    uint8_t *q = d + r * 4;
                    for (int c = 0; c < p.width; ++c, q += pitch, src += 3) {
                        q[0] = src[0];
                        q[1] = src[1];
                        q[2] = src[2];
                    }
                }
            } else {
                for (int r = 0; r < p.height; ++r, d += pitch - p.width * 4) {
                    for (int c = 0; c < p.width; ++c, d += 4, src += 3) {
                        d[0] = src[0];
                        d[1] = src[1];
                        d[2] = src[2];
                    }
                }
            }
        }
        ::operator delete(resized);
    }
    texture->Unlock();
}

I3D_RESULT I3D_lit_object::LockFaces(int level, int set, uint16_t **)
{
    S_lightmapLevel *l = LevelAt(level, set);
    if (!l)
        return -1;
    if (l->faces.buffer)                    // (fix: vertex colour levels have no faces)
        l->faces.buffer->Lock(0);
    return I3D_OK;
}

void I3D_lit_object::RefreshLevel(int level)
{
    if (level < 0 || level >= I3D_mesh::kMaxLevels)     // (fix: the original reads past the levels)
        return;
    for (S_lightmapSet &s : sets) {
        if (s.levels[level])
            s.levels[level]->dirty = true;
    }
}

void I3D_lit_object::RefreshLevels()
{
    if (!mesh)                              // (fix: the original reads the missing mesh)
        return;
    for (S_lightmapSet &s : sets) {
        for (uint32_t i = 0; i < mesh->numLevels; ++i) {
            if (s.levels[i])
                s.levels[i]->dirty = true;
        }
    }
}

I3D_RESULT I3D_lit_object::SetMesh(I3D_mesh *newMesh, uint32_t keep)
{
    auto takeBound = [this](const I3D_mesh *m) {
        std::memcpy(&localBound.bbox, &m->bboxMin, 0x20);
        localBound.center = m->center;
        localBound.radius = m->radius;
    };
    if (newMesh == mesh) {
        if (newMesh && !(visualFlags & VISF_WORLD_SPACE))
            takeBound(newMesh);
        return I3D_OK;
    }
    if (!static_cast<uint8_t>(keep))
        FreeLightmaps(0);
    if (mesh)
        mesh->Release();
    mesh = newMesh;
    if (!newMesh) {
        localBound.bbox.min = S_vector(1e16f, 1e16f, 1e16f);
        localBound.bbox.max = S_vector(-1e16f, -1e16f, -1e16f);
        localBound.center = S_vector(0.0f, 0.0f, 0.0f);
        localBound.radius = 0.0f;
        return I3D_OK;
    }
    ++newMesh->refCount;
    if (!(visualFlags & VISF_WORLD_SPACE))
        takeBound(newMesh);
    textureLevel = 0;
    // (The original returns whatever eax held.)
    return I3D_OK;
}

I3D_RESULT I3D_lit_object::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    FreeLightmaps(0);
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    auto *src = static_cast<I3D_object *>(source);
    switch (src->GetVisualType()) {
    case FourCC("LMAP"): {
        const auto *ls = static_cast<const I3D_lit_object *>(source);
        for (int i = 0; i < 6; ++i) {
            sets[i].flags = ls->sets[i].flags;
            sets[i].resolution = ls->sets[i].resolution;
            sets[i].smoothAngle = ls->sets[i].smoothAngle;
        }
        [[fallthrough]];
    }
    case FourCC("BBRD"):
    case FourCC("MRPH"):
    case FourCC("SNGM"):
    case FourCC("SMMR"):
    case FourCC("OBJ_"):
        SetMesh(src->mesh, 1);
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
    if (src->visualFlags & VISF_WORLD_SPACE) {
        if (!(src->visualFlags & VISF_WBOUND_VALID))
            src->UpdateWBoundProc();
        worldBound = src->worldBound;
    } else {
        visualFlags &= ~VISF_WBOUND_VALID;
    }
    return I3D_OK;
}

namespace {

// One level of a set from the lightmap file (LoadLightmaps): textures (patches, lightmap
// vertices, faces) or vertex colours.
I3D_RESULT ReadLevel(int file, S_lightmapLevel *l, I3D_mesh_level *ml, uint32_t flags, uint8_t version,
                     const I3D_frame *owner)
{
    if (!(flags & LMSET_TEXTURES)) {
        if (flags & LMSET_VERTEX_COUNT) {
            dtaRead(file, &l->numVertices, 4);
            if (l->numVertices != ml->numVertices)
                return I3DERR_FILECORRUPTED;
        } else {
            l->numVertices = ml->numVertices;
        }
        l->colors = static_cast<uint32_t *>(::operator new(sizeof(uint32_t) * l->numVertices, std::nothrow));
        if (!l->colors) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        if (dtaRead(file, l->colors, l->numVertices * 4) != l->numVertices * 4)
            return I3DERR_FILECORRUPTED;
        return I3D_OK;
    }
    uint16_t counts[2];                     // patches, face groups
    if (dtaRead(file, counts, 4) != 4)
        return I3DERR_FILECORRUPTED;
    if (counts[1] != ml->numFaceGroups)
        return I3DERR_INVALIDPARAMS;
    const int numParts = counts[0];
    auto *parts = static_cast<S_lightmapPart *>(::operator new(sizeof(S_lightmapPart) * numParts, std::nothrow));
    if (!parts) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    std::memset(parts, 0, sizeof(S_lightmapPart) * numParts);
    l->parts = parts;
    l->numParts = numParts;
    for (int k = 0; k < numParts; ++k) {
        S_lightmapPart &p = parts[k];
        uint8_t uniform = 0;
        if (dtaRead(file, &uniform, 1) != 1)
            return I3DERR_FILECORRUPTED;
        if (uniform) {
            p.sourceWidth = p.sourceHeight = p.width = p.height = 3;
            p.area = 9;
            if (dtaRead(file, &p.color, 4) != 4)
                return I3DERR_FILECORRUPTED;
        }
        // Mip images: the first one is the patch, the others are skipped.
        uint32_t numImages = 0;
        if (dtaRead(file, &numImages, 4) != 4)
            return I3DERR_FILECORRUPTED;
        for (uint32_t j = 0; j < numImages; ++j) {
            int w = 0, h = 0;
            if (dtaRead(file, &w, 4) != 4 || dtaRead(file, &h, 4) != 4)
                return I3DERR_FILECORRUPTED;
            if (uniform)
                continue;
            const int size = h * w * 3;
            auto *buf = static_cast<uint8_t *>(::operator new(static_cast<size_t>(size), std::nothrow));
            if (!buf)
                return I3DERR_OUTOFMEM;
            if (dtaRead(file, buf, size) != size) {
                ::operator delete(buf);
                return I3DERR_FILECORRUPTED;
            }
            // Version 0x21 stores RGB (the atlas wants BGR).
            if (version == 0x21) {
                for (int n = 0; n < size; n += 3) {
                    const uint8_t t = buf[n];
                    buf[n] = buf[n + 2];
                    buf[n + 2] = t;
                }
            }
            if (j != 0) {
                ::operator delete(buf);
                continue;
            }
            // (A 3 x 3 image would read as a uniform patch.)
            if (w == 3 && h == 3) {
                ::operator delete(buf);     // (fix: the original leaks it)
                return I3DERR_FILECORRUPTED;
            }
            p.sourceHeight = p.height = static_cast<int16_t>(h);
            p.pixels = buf;
            p.sourceWidth = p.width = static_cast<int16_t>(w);
            p.area = h * w;
        }
    }
    // The lightmap vertices: their uv in their patch and their vertex and patch.
    uint32_t numVertices = 0;
    if (!dtaRead(file, &numVertices, 4))
        return I3DERR_FILECORRUPTED;
    l->numVertices = static_cast<int>(numVertices);
    l->uvs = static_cast<S_lightmapUV *>(::operator new(sizeof(S_lightmapUV) * numVertices, std::nothrow));
    if (!l->uvs)
        return I3DERR_OUTOFMEM;
    if (!dtaRead(file, l->uvs, l->numVertices * 8))
        return I3DERR_FILECORRUPTED;
    l->vertexMap = static_cast<S_lightmapVertex *>(::operator new(sizeof(S_lightmapVertex) * numVertices, std::nothrow));
    if (!l->vertexMap)
        return I3DERR_OUTOFMEM;
    if (!dtaRead(file, l->vertexMap, l->numVertices * 4))
        return I3DERR_FILECORRUPTED;
    uint32_t numIndices = 0;
    if (!dtaRead(file, &numIndices, 4))
        return I3DERR_FILECORRUPTED;
    if (static_cast<uint32_t>(ml->NumFaces() * 3) != numIndices)
        return I3DERR_FILECORRUPTED;
    if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, numIndices, &l->faces)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    uint16_t *indices = l->faces.buffer->Lock(0) + l->faces.offset;
    if (!dtaRead(file, indices, static_cast<int>(numIndices) * 2))
        return I3DERR_FILECORRUPTED;
    // Each face group's faces' patches.
    const int numFaceGroups = counts[1];
    l->faceParts = static_cast<uint16_t **>(::operator new(sizeof(uint16_t *) * numFaceGroups, std::nothrow));
    if (!l->faceParts) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    l->numFaceGroups = numFaceGroups;
    std::memset(l->faceParts, 0, sizeof(uint16_t *) * numFaceGroups);
    I3D_face_group *fg = ml->faceGroups;
    for (int b = 0; b < numFaceGroups; ++b, fg = fg->next) {
        int n = 0;
        if (dtaRead(file, &n, 4) != 4 || n != fg->numFaces)
            return I3DERR_FILECORRUPTED;
        auto *fp = static_cast<uint16_t *>(::operator new(sizeof(uint16_t) * static_cast<size_t>(n), std::nothrow));
        if (!fp) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        if (dtaRead(file, fp, n * 2) != n * 2) {
            ::operator delete(fp);          // (fix: the original leaks it)
            ReportError(I3DERR_OUTOFMEM, kSevFatal);    // (sic)
            return I3DERR_FILECORRUPTED;
        }
        l->SetFaceParts(fp, b);
    }
    if (!l->Build(ml, true, owner))
        return I3DERR_GENERIC;
    return I3D_OK;
}

}  // namespace

I3D_RESULT I3D_lit_object::LoadLightmaps(int file)
{
    FreeLightmaps(0);
    if (!mesh)
        return I3DERR_INVALIDPARAMS;
    uint8_t mask = 0;
    dtaRead(file, &mask, 1);
    if (!mask)
        return I3D_OK;
    int numSets = 0;
    for (int b = 0; b < 8; ++b)
        numSets += (mask >> b) & 1;
    for (int s = 0; s < numSets; ++s) {
        // version, flags, levels, resolution, smoothing angle, set number (1 - 6)
        uint8_t header[15];
        if (dtaRead(file, header, 15) != 15)
            return I3DERR_FILECORRUPTED;
        uint8_t number = header[14];
        if (number > 6)
            number = 1;
        // (Fix: number 0 makes the original write its set over the object's fields before them.)
        if (number == 0)
            number = 1;
        S_lightmapSet &set = sets[number - 1];
        set.flags = header[1];
        std::memcpy(&set.resolution, header + 6, 4);
        std::memcpy(&set.smoothAngle, header + 10, 4);
        int numLevels = 0;
        std::memcpy(&numLevels, header + 2, 4);
        const uint8_t version = header[0];
        if ((version != 0x22 && version != 0x21) || static_cast<uint32_t>(numLevels) != mesh->numLevels)
            return I3DERR_INVALIDPARAMS;
        for (int i = 0; i < numLevels; ++i) {
            I3D_mesh_level *ml = mesh->levels[i];
            uint16_t unused = 0;
            if (dtaRead(file, &unused, 2) != 2)
                return I3DERR_FILECORRUPTED;
            S_lightmapLevel *l = set.levels[i];
            if (l) {
                // (Fix: the original overwrites the level's data without freeing it.)
                l->Free();
            } else if (!(l = new (std::nothrow) S_lightmapLevel)) {
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return I3DERR_OUTOFMEM;
            }
            const I3D_RESULT r = ReadLevel(file, l, ml, set.flags, version, this);
            if (r != I3D_OK) {
                // (Fix: the original frees a level still in the set.)
                if (set.levels[i] == l)
                    set.levels[i] = nullptr;
                delete l;
                return r;
            }
            set.levels[i] = l;
        }
        set.loaded = true;
    }
    return I3D_OK;
}

void I3D_lit_object::Prepare()
{
    if (!mesh)
        return;
    if (PrepareMirrored())
        return;
    const float distance2 = LodDistance2();
    const int levelIndex = SelectLevel(distance2);
    if (levelIndex >= I3D_mesh::kMaxLevels)  // (fix: the original reads the level past the last)
        return;
    I3D_mesh_level *level = mesh->levels[levelIndex];
    if (!level || !level->faceGroups || !level->numVertices)
        return;
    S_lightmapSet &set = CurrentSet();
    S_lightmapLevel *l = set.levels[levelIndex];
    if (!l) {
        l = new (std::nothrow) S_lightmapLevel;
        if (!l) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return;
        }
        set.levels[levelIndex] = l;
    }
    if (!set.loaded) {
        I3D_object::Prepare();
        return;
    }
    if ((l->dirty || !l->vertices.vb) && !l->FillVertices(level, mesh, set.flags)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    ITexture *lightmapTexture = nullptr;
    uint32_t lightmapId = 0;
    if (!(set.flags & LMSET_VERTEX_COLORS)) {
        SetLodFade(levelIndex, level, distance2);
        SetLightSector(g_rc.sector);
        SelectLights(this, lights, 0x10);
        if (l->lightmap) {                  // (fix: the original reads a missing lightmap)
            lightmapTexture = l->lightmap->texture;
            lightmapId = lightmapTexture->id;
        }
    }
    uint32_t offset = 0;
    for (I3D_face_group *fg = level->faceGroups; fg; fg = fg->next) {
        S_renderItem &item = g_rc.items.PushBackUninit();
        item.visual = this;
        item.sector = g_rc.sector;
        item.texture = lightmapTexture;
        item.data08 = offset;
        item.data0C = fg;
        offset += static_cast<uint32_t>(fg->numFaces) * 3;
        I3D_material *mat = fg->material;
        if (mat && (mat->matFlags & MATF_TEXTURE)) {
            if (mat->matFlags & MATF_ANIMATED)
                mat->UpdateTexture();
            item.texture2 = mat->texture;
        } else {
            item.texture2 = nullptr;
        }
        if (!mat || (!item.texture2 && (mat->opacity * lodFade) * alpha >= 1.0f)) {
            item.sortKey = 0;
            ++g_rc.numOpaque;
        } else if (!(mat->matFlags & (MATF_ALPHA | MATF_ADDITIVE)) && !((mat->opacity * lodFade) * alpha < 1.0f)) {
            const uint32_t id = item.texture2 ? item.texture2->id : 0;
            item.sortKey = (mat->matFlags & MATF_COLORKEY)
                               ? (((lightmapId & 0xffff) | 0x8000) << 15 | (id & RIKEY_TEXTURE_MASK))
                               : ((lightmapId & RIKEY_TEXTURE_MASK) << 15 | (id & RIKEY_TEXTURE_MASK));
            ++g_rc.numOpaque;
        } else {
            item.sortKey = RIKEY_BLENDED | (static_cast<uint32_t>(Fistp(depth)) & RIKEY_DEPTH_MASK);
            ++g_rc.numBlended;
        }
    }
}

void I3D_lit_object::Draw(const S_renderItem *item)
{
    I3D_lit_object *src = (visualFlags & VISF_MIRRORED) ? static_cast<I3D_lit_object *>(data) : this;
    S_lightmapSet &set = src->CurrentSet();
    S_lightmapLevel *l = set.levels[src->textureLevel];
    const int numVertices = l->numVertices;
    if (!set.loaded) {
        DrawMeshItem(this, item);
        return;
    }
    enum : uint32_t { kTexCoord = 1, kClampU = 2, kClampV = 4 };
    uint32_t restore = 0;
    SetAlphaOp(0, D3DTOP_MODULATE);
    SetAlphaOp(1, D3DTOP_MODULATE);
    auto *fg = static_cast<I3D_face_group *>(item->data0C);
    I3D_material *mat = fg->material;
    if (!mat) {
        g_pDevice->SetMaterial(&ig::g_defaultMaterial);
        SetColorOp(0, D3DTOP_MODULATE2X);
        SetColorOp(1, D3DTOP_DISABLE);
        SetAlphaState(0);
        g_pDevice->SetTexture(0, nullptr);
        SetSpecular(0);
    } else {
        const float opacity = (lodFade * alpha) * mat->opacity;
        if (mat->matFlags & MATF_COLORS) {
            SetMaterial(*reinterpret_cast<D3DMATERIAL8 *>(&mat->diffuse), opacity, lightLevel);
            SetSpecular(mat->power > 1.0f ? 1 : 0);
        } else {
            SetMaterial(ig::g_defaultMaterial, opacity, lightLevel);
            SetSpecular(0);
        }
        // Stage 0 the lightmap, stage 1 the material's texture (white without either).
        ITexture *lightmapTexture = item->texture;
        ITexture *texture = item->texture2;
        if (texture) {
            if (lightmapTexture) {
                SetColorOp(0, D3DTOP_ADD);
                SetColorOp(1, D3DTOP_MODULATE2X);
                g_pDevice->SetTexture(0, lightmapTexture->d3dTexture);
                g_pDevice->SetTexture(1, texture->d3dTexture);
            } else {
                SetColorOp(0, D3DTOP_MODULATE2X);
                SetColorOp(1, D3DTOP_DISABLE);
                g_pDevice->SetTexture(0, texture->d3dTexture);
                if (set.flags & LMSET_TEXTURES) {
                    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1);
                    restore |= kTexCoord;
                }
            }
        } else if (lightmapTexture) {
            SetColorOp(0, D3DTOP_ADD);
            SetColorOp(1, D3DTOP_MODULATE2X);
            g_pDevice->SetTexture(0, lightmapTexture->d3dTexture);
            g_pDevice->SetTexture(1, ig::g_defaultTexture->d3dTexture);
        } else {
            SetColorOp(0, D3DTOP_MODULATE2X);
            SetColorOp(1, D3DTOP_DISABLE);
            g_pDevice->SetTexture(0, ig::g_defaultTexture->d3dTexture);
        }
        const uint32_t flags = mat->matFlags;
        if (flags & (MATF_ADDITIVE | MATF_ADDITIVE_LOW)) {
            SetAlphaState(1);
            SetBlendState(1);
        } else if ((flags & MATF_ALPHA) || opacity < 1.0f) {
            SetAlphaState(1);
            SetBlendState(0);
        } else if (flags & MATF_COLORKEY) {
            SetAlphaState(3);
            SetBlendState(0);
        } else {
            SetAlphaState(0);
        }
        SetCullState(static_cast<int>(((~flags & MATF_TWO_SIDED) | 0x8000000) >> 27));
    }
    const S_IBAlloc *ib;
    if (set.flags & LMSET_TEXTURES) {
        ib = &l->faces;
        EnableLights(item->sector->ambientColor2, lights.count, lights.indices);
        if (mat) {
            if (mat->matFlags & MATF_CLAMP_U) {
                g_pDevice->SetTextureStageState(1, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
                restore |= kClampU;
            }
            if (mat->matFlags & MATF_CLAMP_V) {
                g_pDevice->SetTextureStageState(1, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
                restore |= kClampV;
            }
        }
    } else {
        // Vertex colours: the face group's own faces, unlit.
        ib = &fg->faces;
        const_cast<S_renderItem *>(item)->data08 = 0;   // (the original resets the item's offset)
        g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
        if (mat) {
            if (mat->matFlags & MATF_CLAMP_U) {
                g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
                restore |= kClampU;
            }
            if (mat->matFlags & MATF_CLAMP_V) {
                g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
                restore |= kClampV;
            }
        }
    }
    const S_matrix &m = (visualFlags & VISF_WORLD_SPACE) ? kIdentityMatrix : GetMatrix();
    g_pDevice->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX *>(&m));
    SetVertexFormat(static_cast<int>(l->vertices.buffer->format), false);
    g_pDevice->SetStreamSource(0, l->vertices.vb, l->vertices.buffer->stride);
    g_pDevice->SetIndices(ib->ib, l->vertices.offset);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(numVertices), ib->offset + item->data08,
                                    static_cast<UINT>(fg->numFaces));
    if (set.flags & LMSET_TEXTURES) {
        if (restore & kTexCoord)
            g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        if (restore & kClampU)
            g_pDevice->SetTextureStageState(1, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        if (restore & kClampV)
            g_pDevice->SetTextureStageState(1, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    } else {
        g_pDevice->SetRenderState(D3DRS_LIGHTING, TRUE);
        if (restore & kClampU)
            g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        if (restore & kClampV)
            g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    }
}
