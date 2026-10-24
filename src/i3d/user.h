// I3D_user: a frame for the application's use (FRAME_USER; vtable 0x1009bb30, 0x130 bytes;
// 0x10056ed0 - 0x10056f30): an I3D_frame with one value of the application's.
#pragma once

#include "i3d/frame.h"

class I3D_user : public I3D_frame {
public:
    // 0x4C 0x10056ef0: also the value of another user frame.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 0x10056ed0
    void Init() override;

    uint32_t userValue;                     // +0x120
    uint32_t unknown124[3];                 // +0x124
};
static_assert(sizeof(I3D_user) == 0x130, "I3D_user layout");
