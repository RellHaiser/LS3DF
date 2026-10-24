// I3D_volume (0x100576b0 - 0x10057910).
#include "i3d/volume.h"

void I3D_volume::Init()
{
    I3D_frame::Init();
    volumeType = 0;
    unknown144[0] = unknown144[1] = unknown144[2] = unknown144[3] = 0;
    box.Invalidate();
    unknown154[0] = unknown154[1] = 0;
}

// (scaleW gets an unset value in the original for types 1, 5 and 6.)
void I3D_volume::MatrixChanged()
{
    const uint32_t flags = frameFlags;
    if (flags & FRMFLAGS_CHANGED) {
        bool rescaled = true;
        switch (volumeType) {
        case 1:
            scale = S_vector(1.0f, 1.0f, 1.0f);
            if (!(flags & FRMFLAGS_ROT_QUAT_VALID))
                rot.Make(localMatrix);
            frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
            UpdateWMatrix();
            SetLocalPos(worldMatrix(3));
            rescaled = false;
            break;
        case 4:
            scale.y = 1.0f;
            break;
        case 5:
        case 6:
            scale = S_vector(scale.x, scale.x, scale.x);
            break;
        default:
            rescaled = false;
            break;
        }
        if (rescaled) {
            if (!(flags & FRMFLAGS_ROT_QUAT_VALID))
                rot.Make(localMatrix);
            frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
        }
    }
    UpdateLMatrix();
    worldMatrix = localMatrix;
    frameFlags |= FRMFLAGS_WMATRIX_VALID;
}
