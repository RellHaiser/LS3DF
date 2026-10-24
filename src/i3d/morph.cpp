// Morphing (0x10034f40 - 0x10035f70).
#include "i3d/morph.h"

#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "external/rw_data.h"
#include "i3d/driver.h"
#include "i3d/loader.h"
#include "i3d/mesh.h"
#include "i3d/vb_manager.h"

using namespace ls3d;
using namespace ls3d::i3d;

// S_morphDef ---------------------------------------------------------------------------------------

// 0x10035d40
S_morphDef::S_morphDef(int levels, int perLevel)
{
    refCount = 1;
    if (levels < 1 || perLevel < 1) {
        dbgPrintf("!! Invalid parameter  MorphDef constructor");
        numLevels = 0;
        numChannels = 0;
        channels = nullptr;
        return;
    }
    const int n = levels * perLevel;
    channels = new (std::nothrow) S_morphChannel[static_cast<size_t>(n)]();
    if (!channels) {
        dbgPrintf("!! Out of memory  MorphDef constructor");
        numLevels = 0;
        numChannels = 0;
        return;
    }
    numChannels = perLevel;
    numLevels = levels;
    bounds.bbox.Invalidate();
    bounds.center.Zero();
    bounds.radius = 0.0f;
}

namespace ls3d::i3d {

void ReleaseMorphDef(S_morphDef *def)
{
    if (--def->refCount < 0) {
        dbgPrintf("!! RefCount:0x%x    (MorphDef)", def);
        return;
    }
    if (def->refCount)
        return;
    if (S_morphChannel *c = def->channels) {
        // (0x10035ed0 for each.)
        const int n = def->numLevels * def->numChannels;
        for (int i = 0; i < n; ++i) {
            ::operator delete(c[i].indices);
            if (c[i].targets)
                AlignedFree(c[i].targets);
        }
        delete[] c;
    }
    AlignedFree(def);
}

}  // namespace ls3d::i3d

// C_morph --------------------------------------------------------------------------------------------

// 0x10034f40
void C_morph::Init()
{
    morphFlags = MORPHF_NEW;
    numTargets = 0;
    numChannels = 0;
    weights = nullptr;
    normalized = nullptr;
    def = nullptr;
}

// 0x10034f60
void C_morph::Uninit()
{
    morphFlags &= ~MORPHF_DATA;
    numTargets = 0;
    numChannels = 0;
    ::operator delete(weights);
    weights = nullptr;
    if (def) {
        ReleaseMorphDef(def);
        def = nullptr;
    }
}

// 0x10034ff0
void C_morph::SetWeight(int channel, int target, float weight)
{
    weights[numTargets * channel + target] = weight;
    morphFlags |= MORPHF_LEVELS_DIRTY;
}

// 0x10035020
float C_morph::GetWeight(int channel, int target) const { return weights[numTargets * channel + target]; }

// 0x10035040
void C_morph::Normalize()
{
    for (int c = 0; c < numChannels; ++c) {
        const float *w = weights + c * numTargets;
        float *n = normalized + c * numTargets;
        float sum = 0.0f;
        for (int t = 0; t < numTargets; ++t)
            sum = sum + std::fabs(w[t]);
        if (sum < kEpsilon) {
            n[0] = 1.0f;
            for (int t = 1; t < numTargets; ++t)
                n[t] = 0.0f;
            continue;
        }
        const float inv = 1.0f / sum;
        for (int t = 0; t < numTargets; ++t)
            n[t] = inv * w[t];
    }
}

// 0x10035100
void C_morph::Apply(int level, I3D_mesh *mesh)
{
    const uint32_t mask = 1u << level;
    if (!(morphFlags & mask) || !(morphFlags & MORPHF_DATA))
        return;
    Normalize();
    if (!def || level >= def->numLevels)
        return;
    const S_morphChannel *ch = def->channels + def->numChannels * level;
    if (!ch || !mesh)
        return;
    I3D_mesh_level *ml = mesh->GetLevel(level);
    if (!ml)
        return;
    C_VertexBuffer *vb = ml->vertices.buffer;
    uint8_t *vertices = vb->Lock(0) + vb->stride * ml->vertices.offset;
    const float *w = normalized;
    for (int c = numChannels; c; --c, ++ch, w += numTargets) {
        const float *src = ch->targets;
        for (int k = 0; k < ch->numVertices; ++k) {
            float p[6];
            for (int i = 0; i < 6; ++i)
                p[i] = src[i] * w[0];
            src += 6;
            for (int t = 1; t < numTargets; ++t, src += 6) {
                if (!(w[t] > kEpsilon))
                    continue;
                for (int i = 0; i < 6; ++i)
                    p[i] = p[i] + src[i] * w[t];
            }
            // (Without indices the original read through a null pointer: the vertices in order.)
            const int index = ch->indices ? ch->indices[k] : k;
            std::memcpy(vertices + index * 0x20, p, sizeof p);
        }
    }
    morphFlags &= ~mask;
    ml->FillRenderVB(true, nullptr, nullptr);
}

// 0x100353a0
I3D_RESULT C_morph::CopyFrom(const C_morph &src)
{
    if (!src.def)
        return I3DERR_GENERIC;
    if (def) {
        // (The pointer is replaced below whatever the count said.)
        ReleaseMorphDef(def);
    }
    def = src.def;
    ++def->refCount;
    numTargets = src.numTargets;
    numChannels = src.numChannels;
    ::operator delete(weights);
    const int n = numTargets * numChannels;
    weights = static_cast<float *>(::operator new(static_cast<size_t>(n) * 8, std::nothrow));
    if (!weights) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    normalized = weights + n;
    std::memcpy(weights, src.weights, static_cast<size_t>(n) * sizeof(float));
    return I3D_OK;
}

// 0x10035490
void C_morph::Load()
{
    auto outOfMemory = [this] {
        numTargets = 0;
        numChannels = 0;
        g_loadResult = I3DERR_OUTOFMEM;
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
    };
    dtaRead(g_loadFile, &numTargets, 1);
    if (!numTargets)
        return;
    dtaRead(g_loadFile, &numChannels, 1);
    int levels = 0;
    dtaRead(g_loadFile, &levels, 1);
    void *mem = AlignedAlloc(sizeof(S_morphDef), 16);
    def = mem ? new (mem) S_morphDef(levels, numChannels) : nullptr;
    if (!def) {
        outOfMemory();
        return;
    }
    S_morphChannel *ch = def->numLevels > 0 ? def->channels : nullptr;
    for (int i = numChannels * levels; i; --i, ++ch) {
        ch->numVertices = 0;
        dtaRead(g_loadFile, &ch->numVertices, 2);
        if (!ch->numVertices)
            continue;
        const int bytes = numTargets * ch->numVertices * 0x18;
        ch->targets = static_cast<float *>(AlignedAlloc(static_cast<size_t>(bytes), 16));
        if (!ch->targets) {
            outOfMemory();
            return;
        }
        dtaRead(g_loadFile, ch->targets, ch->numVertices * numTargets * 0x18);
        char hasIndices;
        dtaRead(g_loadFile, &hasIndices, 1);
        if (!hasIndices) {
            ch->indices = nullptr;
            continue;
        }
        ch->indices = static_cast<uint16_t *>(::operator new(static_cast<size_t>(ch->numVertices) * 2, std::nothrow));
        if (!ch->indices) {
            outOfMemory();
            return;
        }
        dtaRead(g_loadFile, ch->indices, ch->numVertices * 2);
    }
    weights = static_cast<float *>(::operator new(static_cast<size_t>(numChannels * numTargets) * 8, std::nothrow));
    if (!weights) {
        outOfMemory();
        return;
    }
    normalized = weights + numTargets * numChannels;
    float *w = weights;
    for (int c = 0; c < numChannels; ++c) {
        *w++ = 1.0f;
        for (int t = 1; t < numTargets; ++t)
            *w++ = 0.0f;
    }
    morphFlags |= MORPHF_DATA;
    dtaRead(g_loadFile, &def->bounds, 0x30);
}

// I3D_morph --------------------------------------------------------------------------------------

// 0x100356f0
void I3D_morph::Init()
{
    I3D_object::Init();
    C_morph::Init();
}

// 0x10035730
void I3D_morph::Uninit()
{
    C_morph::Uninit();
    I3D_object::Uninit();
}

// 0x10035800
I3D_RESULT I3D_morph::SetMorphWeight(int channel, int target, float weight)
{
    if (channel < 0 || channel >= numChannels || target < 0 || target >= numTargets)
        return I3DERR_INVALIDPARAMS;
    ++changeCount;
    weights[numTargets * channel + target] = weight;
    morphFlags |= MORPHF_LEVELS_DIRTY;
    visualFlags &= ~VISF_WBOUND_VALID;
    return I3D_OK;
}

// 0x10035880
float I3D_morph::GetMorphWeight(int channel, int target)
{
    // (Out of range the original returns what the FPU held.)
    if (channel < 0 || channel >= numChannels || target < 0 || target >= numTargets)
        return 0.0f;
    return weights[numTargets * channel + target];
}

// 0x100358d0
I3D_RESULT I3D_morph::SetMesh(I3D_mesh *newMesh, uint32_t flags)
{
    I3D_object::SetMesh(newMesh, flags);
    if (def)
        localBound = def->bounds;
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
    return I3D_OK;
}

// 0x10035940
I3D_RESULT I3D_morph::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    visualFlags &= ~VISF_WBOUND_VALID;
    I3D_frame::Duplicate(source);
    morphFlags &= ~(MORPHF_DATA | MORPHF_NEW);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    morphFlags &= ~MORPHF_DATA;
    numTargets = 0;
    numChannels = 0;
    ::operator delete(weights);
    weights = nullptr;
    if (def) {
        ReleaseMorphDef(def);
        def = nullptr;
    }
    auto *src = static_cast<I3D_object *>(source);
    bool generic = true;
    switch (src->GetVisualType()) {
    case FourCC("LMAP"):
    case FourCC("BBRD"):
    case FourCC("SNGM"):
    case FourCC("OBJ_"):
        break;
    case FourCC("MRPH"):
    case FourCC("SMMR"):
        // (This visual's own mirrored flag decides.)
        if (!(visualFlags & VISF_MIRRORED)) {
            // (A single morph's weights are read at the morph visual's offset: the original's.)
            const auto &srcMorph = *reinterpret_cast<const C_morph *>(reinterpret_cast<const uint8_t *>(src) + 0x210);
            I3D_RESULT r = CopyFrom(srcMorph);
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
            lightLevel = src->lightLevel;
            alpha = src->alpha;
            generic = false;
        }
        break;
    case FourCC("LENS"):
        SetMesh(nullptr, 0);
        generic = false;
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
    if (src->visualFlags & VISF_WORLD_SPACE) {
        src->GetWorldBound();
        worldBound = src->worldBound;
        return I3D_OK;
    }
    visualFlags &= ~VISF_WBOUND_VALID;
    return I3D_OK;
}

// 0x10035c10
void I3D_morph::Prepare()
{
    if (!mesh)
        return;
    const int level = SelectLevel(LodDistance2());
    if (!mesh->levels[level])
        return;
    Apply(level, mesh);
    I3D_object::Prepare();
}

// 0x10035f00
void I3D_morph::Load()
{
    morphFlags &= ~MORPHF_NEW;
    I3D_object::Load();
    C_morph::Load();
    if (def)
        localBound = def->bounds;
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
}
