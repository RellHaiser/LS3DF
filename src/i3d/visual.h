// I3D_visual: the base of the drawable frames (FRAME_VISUAL; vtable 0x1009c950, 0x1E0 bytes;
// methods 0x10056f80 - 0x10057660). The driver makes visuals by index (visualIndex, the order of
// the fourCC table: OBJ_, LMAP, SNGM, SMMR, BBRD, MRPH, LENS, PROJ, MIRR, EMTR, LNDP, SHD).
//
// Visuals have a second base at +0x120, the morph weight interface (I3D_visual_morph); only the
// morph visuals implement it.
#pragma once

#include "i3d/frame.h"

namespace ls3d::i3d {
struct S_renderItem;
}

// Visual flags (I3D_visual::visualFlags).
enum : uint32_t {
    VISF_NO_ZWRITE = 0x00000002,            // (renderer) drawn without writing z
    VISF_TEXTURE_LOD = 0x00000010,          // objects: the levels are texture LODs of level 0's geometry (+0x1E4)
    VISF_WORLD_SPACE = 0x00000040,          // geometry in world space: moving the frame keeps the bounds
    VISF_LOD_AT_POSITION = 0x00000080,      // LOD / depth measured at the frame's position (not the bound's center)
    VISF_ZBIAS = 0x00000100,                // (renderer) drawn with z bias 2
    VISF_MODE_A1 = 0x00000200,              // SetModeA (slot 0x80): 0 none, 1 this bit, 2 both
    VISF_MODE_A2 = 0x00000400,
    VISF_OBJECT_PARAMS = 0x00000800,        // an I3D_object: takes the hierarchy's light level / alpha
    VISF_MODE_B1 = 0x00002000,              // SetModeB (slot 0x88)
    VISF_MODE_B2 = 0x00004000,
    VISF_NO_FOG = 0x00008000,               // (renderer) its sector's fog does not apply
    VISF_MIRRORED = 0x00010000,             // a mirror's reflected copy: `data` is the visual it shows
    VISF_BOUND_VALID = 0x00020000,          // localBound up to date
    VISF_WBOUND_VALID = 0x00040000,         // worldBound up to date
    VISF_INVMATRIX_VALID = 0x00100000,      // invWorldMatrix up to date
    VISF_UV_STALE = 0x02000000,             // objects (texture LODs): the render vertices need the level's UVs
};

// The visual types (0x1009d450).
constexpr int kVisualTypeCount = 12;
// The fourCC of visual index `index` ('OBJ_', 'LMAP', ...; 0x100a6edc).
uint32_t VisualFourCC(int index);
// The index of a visual fourCC, -1 if unknown (0x10056f80).
int VisualIndex(uint32_t fourCC);

// Morph weights of a visual: per channel (a group of vertices) the weight of each morph target
// (vtable 0x1009c93c for visuals without any).
class I3D_visual_morph {
public:
    virtual int __stdcall NumMorphTargets() { return 0; }                           // 0x10057630
    virtual int __stdcall NumMorphChannels() { return 0; }                          // 0x10057630
    virtual I3D_RESULT __stdcall SetMorphWeight(int channel, int target, float weight)   // 0x10057640
    {
        return I3DERR_GENERIC;
    }
    virtual float __stdcall GetMorphWeight(int channel, int target) { return 0.0f; }   // 0x10057650
};

class I3D_visual : public I3D_frame, public I3D_visual_morph {
public:
    // 0x04 0x10057050: the fourCC of the visual's type.
    uint32_t __stdcall GetVisualType() override;
    // 0x60 0x10056fa0
    void Init() override;
    // 0x68 0x10057070: a moved visual's world bound and inverse matrix are stale.
    void MatrixChanged() override;
    // 0x74 0x10057490: the local bound (box and sphere) as given.
    virtual void __stdcall SetBoundVolume(const I3D_bound_volume &volume);
    // 0x78 0x100574f0: the local box; the sphere around it.
    virtual void __stdcall SetBBox(const I3D_bbox &box);
    // 0x7C 0x100570a0 (nothing in the base): objects and lightmaps bake their vertices into world
    // space (VISF_WORLD_SPACE; the scene does it to the level's static visuals) or back.
    virtual void __stdcall SetWorldSpace(bool on) {}
    // 0x80 / 0x84 0x100570b0 / 0x10057100: a 3-state mode in VISF_MODE_A1/A2 (0 on the visuals of a
    // carried body, 2 when it is put down).
    virtual void __stdcall SetModeA(int mode);
    virtual int __stdcall GetModeA();
    // 0x88 / 0x8C 0x10057120 / 0x10057170: the same in VISF_MODE_B1/B2.
    virtual void __stdcall SetModeB(int mode);
    virtual int __stdcall GetModeB();
    // 0x90 0x10057190: computes the local bound of the visual's geometry (the base has none).
    virtual void __stdcall UpdateLocalBound();
    // 0x94 0x10073330 (__thiscall, nothing in the base): queues the visual's primitives
    // (S_renderItem) for the pass in g_rc.
    virtual void Prepare() {}
    // 0x98 0x1002b0a0 (__thiscall, nothing in the base): draws one queued item.
    virtual void Draw(const ls3d::i3d::S_renderItem *item) {}

    const I3D_bound_volume &GetLocalBound()
    {
        if (!(visualFlags & VISF_BOUND_VALID))
            UpdateLocalBound();
        return localBound;
    }
    const I3D_bbox &GetWorldBound()
    {
        if (!(visualFlags & VISF_WBOUND_VALID))
            UpdateWBoundProc();
        return worldBound.bbox;
    }

    // 0x100571b0 (exported "?UpdateWBoundProc@I3D_visual@@AAGXXZ", __stdcall): the world box of the
    // local one and the sphere around it.
    void __stdcall UpdateWBoundProc();

    // +0x120: I3D_visual_morph's vtable pointer.
    uint32_t unknown124[3];                 // +0x124
    S_matrix invWorldMatrix;                // +0x130 VISF_INVMATRIX_VALID
    I3D_bound_volume worldBound;            // +0x170 VISF_WBOUND_VALID
    I3D_bound_volume localBound;            // +0x1A0 VISF_BOUND_VALID
    uint32_t visualFlags;                   // +0x1D0
    I3D_visual *nextRenderVisual;           // +0x1D4 (renderer) its sector's render list
    float depth;                            // +0x1D8 (renderer) view depth (the portal pass; blended items sort by it)
    uint32_t unknown1DC;                    // +0x1DC
};
static_assert(sizeof(I3D_visual) == 0x1E0, "I3D_visual layout");
static_assert(offsetof(I3D_visual, invWorldMatrix) == 0x130, "I3D_visual layout");
static_assert(offsetof(I3D_visual, worldBound) == 0x170, "I3D_visual layout");
static_assert(offsetof(I3D_visual, visualFlags) == 0x1D0, "I3D_visual layout");
