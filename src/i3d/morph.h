// Morphing: the morph data of a mesh (S_morphDef, shared), the per-visual weights (C_morph,
// 0x10034f40 - 0x10035490) and the morph visual I3D_morph (MRPH; vtable 0x1009c378, 0x230 bytes;
// 0x100356f0 - 0x10035f70). The single-morph visual (SMMR) has a C_morph too.
//
// Per mesh level the data holds channels (groups of vertices); each channel has for each of its
// vertices a position and normal per morph target. A vertex becomes the blend of its targets by
// the channel's weights (normalized to sum 1).
#pragma once

#include "i3d/object.h"

// A channel: its vertices' indices and their targets (numTargets x (position, normal) each).
struct S_morphChannel {
    int numVertices;                        // +0x00
    uint16_t *indices;                      // +0x04 (null when the file has none)
    float *targets;                         // +0x08 16-byte aligned
};

// The morph data (0x40 bytes, 16-byte aligned, reference counted).
struct S_morphDef {
    // 0x10035d40: room for `levels` x `channels` channels.
    S_morphDef(int levels, int channels);

    int numLevels;                          // +0x00
    int numChannels;                        // +0x04
    S_morphChannel *channels;               // +0x08 [level][channel] (new[] with a count before it)
    int refCount;                           // +0x0C
    I3D_bound_volume bounds;                // +0x10 the morphed mesh's (from the file)
};
static_assert(sizeof(S_morphDef) == 0x40, "S_morphDef layout");

// MORPHF_* (C_morph::morphFlags).
enum : uint32_t {
    MORPHF_LEVELS_DIRTY = 0x0000ffff,       // the mesh levels whose vertices wait for Apply
    MORPHF_DATA = 0x00010000,               // morph data loaded or copied
    MORPHF_NEW = 0x00020000,                // (set by Init, cleared by the visuals' Load)
};

// The weights of a visual's morph (0x20 bytes; base vtable 0x1009c5dc).
class C_morph {
public:
    C_morph() = default;
    C_morph(const C_morph &) = delete;
    C_morph &operator=(const C_morph &) = delete;

    virtual void Init();                    // 0x00 0x10034f40
    virtual void Uninit();                  // 0x04 0x10034f60 the weights freed, the data released

    // 0x10034ff0 / 0x10035020 (unchecked)
    void SetWeight(int channel, int target, float weight);
    float GetWeight(int channel, int target) const;
    // 0x10035040: the weights of each channel scaled to sum 1 (by absolute value; none: target 0).
    void Normalize();
    // 0x10035100: the level's vertices blended from the data when marked dirty.
    void Apply(int level, I3D_mesh *mesh);
    // 0x100353a0: the data (shared) and the weights of another visual's morph.
    I3D_RESULT CopyFrom(const C_morph &src);
    // 0x10035490: from the model file being loaded.
    void Load();

    uint32_t morphFlags;                    // +0x04 MORPHF_*
    int numTargets;                         // +0x08
    int numChannels;                        // +0x0C
    float *weights;                         // +0x10 [channel][target]
    float *normalized;                      // +0x14 (the same block, after the weights)
    S_morphDef *def;                        // +0x18
    uint32_t unknown1C;                     // +0x1C
};
static_assert(sizeof(C_morph) == 0x20, "C_morph layout");

class I3D_morph : public I3D_object, public C_morph {
public:
    // 0x4C 0x10035940: as I3D_object's; another morph visual's data (and a copy of its mesh: the
    // vertices are morphed in place).
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x100356f0
    void Uninit() override;                                                    // 0x64 0x10035730
    // 0x70 0x10035f00: the object, then the morph data; the bound is the data's.
    void Load() override;
    // 0x94 0x10035c10: the level for the distance morphed, then drawn as an object.
    void Prepare() override;
    // 0x9C 0x100358d0: the mesh; the bound stays the morph data's.
    I3D_RESULT __stdcall SetMesh(I3D_mesh *newMesh, uint32_t flags) override;

    // I3D_visual_morph (0x100357e0 - 0x10035880).
    int __stdcall NumMorphTargets() override { return numTargets; }
    int __stdcall NumMorphChannels() override { return numChannels; }
    I3D_RESULT __stdcall SetMorphWeight(int channel, int target, float weight) override;
    float __stdcall GetMorphWeight(int channel, int target) override;
};
static_assert(sizeof(I3D_morph) == 0x230, "I3D_morph layout");

namespace ls3d::i3d {

// A morph data reference dropped (the last frees it).
void ReleaseMorphDef(S_morphDef *def);

}  // namespace ls3d::i3d
