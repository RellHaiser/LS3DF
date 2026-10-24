// I3D_dummy: an invisible box (FRAME_DUMMY; vtable 0x1009bba8, 0x150 bytes; 0x1001bb70 -
// 0x1001c080). Scenes keep a list of the dummies they draw as boxes (FRMFLAGS_SCENE_DUMMY).
#pragma once

#include "i3d/frame.h"

// I3D_dummy::boxFlags.
enum : uint32_t {
    DUMMYF_UNIT_BOX = 0x1,                  // the box is [0, 1]^3 (BakeBox 0 / 1)
    DUMMYF_CENTERED_BOX = 0x2,              // the box is [-1, 1]^3 (BakeBox 2)
};

class I3D_dummy : public I3D_frame {
public:
    // 0x4C 0x1001bc10: also the box of another dummy.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x1001bb70 / 0x1001bbc0
    void Init() override;
    void Uninit() override;
    // 0x70 0x1001c060: the frame, then the box.
    void Load() override;
    // 0x74 0x1001bc60: moves the box's position and size into the frame's transform, leaving the
    // unit box [-1, 1] (mode 2) or [0, 1] (other modes).
    virtual void __stdcall BakeBox(int mode);

    const I3D_bbox &GetBBox() const { return box; }

    I3D_bbox box;                           // +0x120
    uint32_t boxFlags;                      // +0x140 DUMMYF_*
    uint32_t unknown144[3];                 // +0x144
};
static_assert(sizeof(I3D_dummy) == 0x150, "I3D_dummy layout");
