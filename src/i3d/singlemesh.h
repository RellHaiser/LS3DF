// I3D_singlemesh: the skinned mesh visual (SNGM; vtable 0x1009c5f8, 0x250 bytes; 0x10051dc0 -
// 0x10053b90) and the single morph I3D_singlemorph (SMMR; vtable 0x1009c510, 0x270 bytes;
// 0x10053c00 - 0x10054240), a skinned mesh with morph targets.
//
// An object visual whose mesh follows a skeleton: the I3D_joint frames below it (up to the next
// skinned visual), found by their jointIndex. The skin data (shared by copies) gives each joint
// the inverse of its bind pose and the box of the vertices it moves, and each mesh level's first
// `numWeighted` vertices their joint (1-based) and the weight of that joint's parent (x / 256).
// Each level is skinned into a vertex buffer of its own (S_skinLevel) when a joint moved since.
#pragma once

#include "i3d/mesh.h"
#include "i3d/morph.h"
#include "i3d/object.h"

class I3D_joint;

constexpr int kMaxSkinJoints = 64;

struct S_skinJoint {
    S_matrix bindInverse;                   // +0x00 the mesh's space into the joint's
    I3D_bbox box;                           // +0x40 its vertices in its space (invalid: none)
};
static_assert(sizeof(S_skinJoint) == 0x60, "S_skinJoint layout");

// The skin data (0xC0 bytes, 16-byte aligned, reference counted).
struct S_skinData {
    int refCount;                           // +0x00
    int numJoints;                          // +0x04
    S_skinJoint *joints;                    // +0x08 16-byte aligned
    uint32_t unknown0C;                     // +0x0C
    I3D_bbox bounds;                        // +0x10 of the mesh in its bind pose
    int numWeighted[I3D_mesh::kMaxLevels];  // +0x30 per level: the vertices skinned (the first)
    // +0x58 per level and vertex: the joint (1-based; 0 none) and its parent's weight (x / 256).
    uint8_t *vertexJoints[I3D_mesh::kMaxLevels];
    uint8_t parents[kMaxSkinJoints];        // +0x80 a joint's parent joint (1-based; 0 none)
};
static_assert(sizeof(S_skinData) == 0xC0, "S_skinData layout");

// S_skinLevel::flags.
enum : uint32_t {
    SKINLEVEL_STALE = 0x20,                 // the buffer waits for FillRenderVB (RefreshLevel)
};

// A mesh level skinned (0x14 bytes).
struct S_skinLevel {
    int stamp = -2000;                      // +0x00 I3D_singlemesh::jointChanges when skinned
    uint32_t flags = 0;                     // +0x04 SKINLEVEL_*
    ls3d::S_VBAlloc vb = {};                // +0x08 the level's render layout
};
static_assert(sizeof(S_skinLevel) == 0x14, "S_skinLevel layout");

class I3D_singlemesh : public I3D_object {
public:
    // 0x4C 0x10052070: as I3D_object's; a skinned mesh's skin data (shared) too, unless this
    // is a mirror's copy.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x10051dc0
    // 0x64 0x10051e10: the joints let go, the skinned levels and the skin data released.
    void Uninit() override;
    // 0x70 0x10053900: the object, then the skin data.
    void Load() override;
    // 0x7C 0x100570a0: nothing (the joints move the vertices; the visual base's).
    void __stdcall SetWorldSpace(bool on) override {}
    // 0x90 0x10052300: ComputeBound when a joint moved since the last.
    void __stdcall UpdateLocalBound() override;
    // 0x94 0x100533b0: as I3D_object's with the level skinned into its own buffer.
    void Prepare() override;
    // 0xAC / 0xB0 0x10051f20 / 0x10051ee0: a level's / every level's skinned buffer refilled
    // when next drawn.
    void __stdcall RefreshLevel(int level) override;
    void __stdcall RefreshLevels() override;

    // 0xB4 0x10051f50: a joint's box in its space (< 0: the bind pose's), null when none.
    virtual const I3D_bbox *__stdcall GetJointBBox(int joint);
    // 0xB8 0x100529c0: the joint moving a vertex of a level (null: none, unchecked indices).
    virtual I3D_joint *__stdcall GetVertexJoint(int vertex, int level);
    // 0xBC 0x10052a10: a level's vertex positions skinned (the unskinned ones copied with their
    // normal's x as w) into a buffer shared by all, valid until the next call; null without
    // level or skin data.
    virtual const S_vector4 *__stdcall SkinnedVertices(int level);
    // 0xC0 / 0xC4 0x10051fa0 / 0x10051ff0: a level's / every level's skinned buffer freed.
    virtual void __stdcall ReleaseLevel(int level);
    virtual void __stdcall ReleaseLevels();
    // 0xC8 0x10052fa0 (__thiscall): the level skinned into its buffer (made when missing).
    virtual void SkinLevel(int level);

    // 0x100527d0: the joints found again among the descendants (up to 64; false when none).
    bool FixJoints();

    int numJoints;                          // +0x210
    I3D_joint **joints;                     // +0x214 by I3D_joint::jointIndex
    int jointChanges;                       // +0x218 counts the joints' moves (-300)
    int boundStamp;                         // +0x21C jointChanges when localBound was made (-400)
    S_skinData *skin;                       // +0x220
    S_skinLevel *levels[I3D_mesh::kMaxLevels];  // +0x224 per mesh level
    uint32_t unknown24C;                    // +0x24C

protected:
    // The joints let go (their mesh cleared) and the array freed.
    void ClearJoints();
    // The skinned levels freed.
    void FreeLevels();
    // 0x10052050: another skinned mesh's skin data (a reference).
    void CopySkin(const I3D_singlemesh &src);
    // 0x10052330: the bound of the skinned mesh: the bind pose's box grown by each joint's box
    // where the joint is now (without skin data: the mesh's).
    void ComputeBound();
    // The joints' skinning matrices into g_jointMatrices (inlined).
    void ComputeJointMatrices();
};
static_assert(sizeof(I3D_singlemesh) == 0x250, "I3D_singlemesh layout");
static_assert(offsetof(I3D_singlemesh, joints) == 0x214, "I3D_singlemesh layout");
static_assert(offsetof(I3D_singlemesh, levels) == 0x224, "I3D_singlemesh layout");

class I3D_singlemorph : public I3D_singlemesh, public C_morph {
public:
    // 0x4C 0x10053d70: as I3D_singlemesh's; of a morph visual also its morph data (and a copy of
    // its mesh: the vertices are morphed in place).
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x10053c00
    void Uninit() override;                                                    // 0x64 0x10053c20
    // 0x70 0x10054240: the skinned mesh, then the morph data; the bound is the data's.
    void Load() override;
    // 0x9C 0x10053d00: the mesh; the bound stays the morph data's.
    I3D_RESULT __stdcall SetMesh(I3D_mesh *newMesh, uint32_t flags) override;
    // 0xBC 0x10054210 / 0xC8 0x100541e0: the level morphed first.
    const S_vector4 *__stdcall SkinnedVertices(int level) override;
    void SkinLevel(int level) override;

    // I3D_visual_morph (0x10053c40 - 0x10053cc0).
    int __stdcall NumMorphTargets() override { return numTargets; }
    int __stdcall NumMorphChannels() override { return numChannels; }
    I3D_RESULT __stdcall SetMorphWeight(int channel, int target, float weight) override;
    float __stdcall GetMorphWeight(int channel, int target) override;
};
static_assert(sizeof(I3D_singlemorph) == 0x270, "I3D_singlemorph layout");

namespace ls3d::i3d {

// The skinning matrices of the visual being skinned (0x100bf3e0): a joint's bind pose inverse
// times where the joint is now.
extern S_matrix g_jointMatrices[kMaxSkinJoints];

}  // namespace ls3d::i3d
