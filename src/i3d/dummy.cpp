// I3D_dummy (0x1001bb70 - 0x1001c080).
#include "i3d/dummy.h"

#include "external/rw_data.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/loader.h"

using namespace ls3d::i3d;

void I3D_dummy::Init()
{
    I3D_frame::Init();
    box.Invalidate();
    boxFlags = 0;
}

void I3D_dummy::Uninit()
{
    if (frameFlags & FRMFLAGS_SCENE_DUMMY) {
        for (int i = g_driver->scenes.Size(); i > 0; --i)
            SceneRemoveDummy(g_driver->scenes[i - 1], this);
    }
    I3D_frame::Uninit();
}

I3D_RESULT __stdcall I3D_dummy::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    if (source->type == FRAME_DUMMY)
        box = static_cast<I3D_dummy *>(source)->box;
    return I3D_frame::Duplicate(source);
}

void I3D_dummy::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &box, sizeof box);
}

// (scaleW gets an unset value in the original.)
void __stdcall I3D_dummy::BakeBox(int mode)
{
    S_vector origin, size;
    if (mode == 2) {
        const S_vector sum(box.min.x + box.max.x, box.min.y + box.max.y, box.min.z + box.max.z);
        origin = S_vector(sum.x * 0.5f, sum.y * 0.5f, sum.z * 0.5f);
        const S_vector d(box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z);
        size = S_vector(d.x * 0.5f, d.y * 0.5f, d.z * 0.5f);
    } else {
        origin = box.min;
        size = S_vector(box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z);
    }
    UpdateLMatrix();
    SetLocalPos(origin * localMatrix);
    scale = S_vector(size.x * scale.x, size.y * scale.y, size.z * scale.z);
    if (!(frameFlags & FRMFLAGS_ROT_QUAT_VALID))
        rot.Make(localMatrix);
    frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
    if (mode == 2) {
        box.min = S_vector(-1.0f, -1.0f, -1.0f);
        box.max = S_vector(1.0f, 1.0f, 1.0f);
        boxFlags = (boxFlags & ~DUMMYF_UNIT_BOX) | DUMMYF_CENTERED_BOX;
    } else {
        box.min = S_vector(0.0f, 0.0f, 0.0f);
        box.max = S_vector(1.0f, 1.0f, 1.0f);
        boxFlags = (boxFlags & ~DUMMYF_CENTERED_BOX) | DUMMYF_UNIT_BOX;
    }
}
