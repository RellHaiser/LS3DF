// I3D_volume: a collision volume of a model (FRAME_VOLUME; vtable 0x1009b960, 0x160 bytes;
// 0x100576b0 - 0x10057910). Its world matrix is its local one: volumes are tested in the space of
// the frame they belong to.
#pragma once

#include "i3d/frame.h"

class I3D_volume : public I3D_frame {
public:
    // 0x60 0x100576b0
    void Init() override;
    // 0x68 0x10057730: keeps the scale the volume type allows (type 1: none and the world
    // position as the local one, 4: none along y, 5 / 6: uniform), then the matrices.
    void MatrixChanged() override;
    // 0x74 0x10057720
    virtual int __stdcall GetVolumeType() { return volumeType; }

    I3D_bbox box;                           // +0x120
    int volumeType;                         // +0x140
    uint32_t unknown144[4];                 // +0x144
    uint32_t unknown154[2];                 // +0x154
    uint32_t unknown15C;                    // +0x15C
};
static_assert(sizeof(I3D_volume) == 0x160, "I3D_volume layout");
static_assert(offsetof(I3D_volume, volumeType) == 0x140, "I3D_volume layout");
