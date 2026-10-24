// I3D_target: a frame others follow (FRAME_TARGET; vtable 0x1009beb0, 0x140 bytes; 0x10056250 -
// 0x10056e80). Its frames turn toward it (TARGETF_LOOK_AT) or copy its rotation, position and
// scale in their parents' space (TARGETF_ROTATION / POSITION / SCALE) whenever it moves.
#pragma once

#include "core/c_vector.h"
#include "i3d/frame.h"

// I3D_target::mode.
enum : uint32_t {
    TARGETF_LOOK_AT = 0x1,                  // the frames point their z axis at the target
    TARGETF_ROTATION = 0x2,
    TARGETF_POSITION = 0x4,
    TARGETF_SCALE = 0x8,
};

class I3D_target : public I3D_frame {
public:
    // 0x4C 0x100563a0: also the mode of another target.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10056250 / 0x100562e0: in / out of the targets' list.
    void Init() override;
    void Uninit() override;
    // 0x68 0x10056640: a moved target waits in g_targetUpdates for the end of Update.
    void MatrixChanged() override;
    // 0x70 0x10056c70: the frame, the mode and the indices of the frames (resolved by the loader).
    void Load() override;
    // 0x74 0x10056410: a frame follows this target (leaving the one it followed).
    virtual void __stdcall AddFrame(I3D_frame *frame);
    // 0x78 0x10056510: I3DERR_OBJECTNOTFOUND when the frame does not follow this target.
    virtual I3D_RESULT __stdcall RemoveFrame(I3D_frame *frame);
    // 0x7C 0x100563f0: TARGETF_*; the frames follow at once.
    virtual void __stdcall SetMode(uint32_t newMode);

    // The frames take the target's transform (0x100566d0).
    void UpdateFrames();

    ls3d::C_vector<I3D_frame *> frames;     // +0x120 (frame indices until the loader resolves them)
    uint32_t mode;                          // +0x130 TARGETF_*
    uint32_t unknown134[3];                 // +0x134
};
static_assert(sizeof(I3D_target) == 0x140, "I3D_target layout");
static_assert(offsetof(I3D_target, mode) == 0x130, "I3D_target layout");

namespace ls3d::i3d {

// Every target (0x100c0570).
extern C_vector<I3D_target *> g_targets;
// The target `frame` follows, null if none (0x100565a0).
I3D_target *FindTargetOf(const I3D_frame *frame);

}  // namespace ls3d::i3d
