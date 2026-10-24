// I3D_user (0x10056ed0 - 0x10056f30).
#include "i3d/user.h"

void I3D_user::Init()
{
    I3D_frame::Init();
    userValue = 0;
}

I3D_RESULT __stdcall I3D_user::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    if (source->type == FRAME_USER)
        userValue = static_cast<I3D_user *>(source)->userValue;
    return I3D_frame::Duplicate(source);
}
