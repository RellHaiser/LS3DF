// I3D_joint: a bone of a skinned mesh's skeleton (FRAME_JOINT; vtable 0x1009b9d8, 0x170 bytes;
// 0x10020a70 - 0x10020bc0). The I3D_singlemesh it moves keeps it in its joint array.
#pragma once

#include "i3d/frame.h"

class I3D_singlemesh;

// I3D_joint::jointFlags.
enum : uint32_t {
    JOINTF_8 = 0x08,                        // (cleared with the matrix; the skinning code's)
    JOINTF_MODEL_MATRIX_VALID = 0x10,       // modelMatrix up to date
};

class I3D_joint : public I3D_frame {
public:
    // 0x4C 0x10020b50: also the index of another joint.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10020a70 / 0x10020a90
    void Init() override;
    void Uninit() override;
    // 0x68 0x10020ad0: a moved joint makes its mesh's bounds stale.
    void MatrixChanged() override;
    // 0x70 0x10020ba0: the frame, then the joint index.
    void Load() override;

    // The joint in its skinned mesh's space (modelMatrix; inlined by I3D_singlemesh): its local
    // matrix times its parents' up to the first visual, sector or model; a parent joint with a
    // current model matrix ends the walk with that.
    const S_matrix &GetModelMatrix();

    S_matrix modelMatrix;                   // +0x120 the joint in its mesh's space (skinning)
    int jointIndex;                         // +0x160 in the skeleton
    uint32_t jointFlags;                    // +0x164 JOINTF_*
    I3D_singlemesh *mesh;                   // +0x168 the visual it moves
    uint32_t unknown16C;                    // +0x16C
};
static_assert(sizeof(I3D_joint) == 0x170, "I3D_joint layout");
static_assert(offsetof(I3D_joint, jointIndex) == 0x160, "I3D_joint layout");
