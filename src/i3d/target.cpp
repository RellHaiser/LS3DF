// I3D_target (0x10056250 - 0x10056e80).
#include "i3d/target.h"

#include <cmath>

#include "external/rw_data.h"
#include "i3d/i3d_hooks.h"
#include "i3d/loader.h"

namespace ls3d::i3d {

C_vector<I3D_target *> g_targets;                   // 0x100c0570

extern C_vector<I3D_frame *> g_targetUpdates;       // frame.cpp

I3D_target *FindTargetOf(const I3D_frame *frame)
{
    for (I3D_target *t : g_targets) {
        for (I3D_frame *f : t->frames) {
            if (f == frame)
                return t;
        }
    }
    return nullptr;
}

// 0x100565f0
void RemoveFromTargets(I3D_frame *frame)
{
    if (I3D_target *t = FindTargetOf(frame))
        t->RemoveFrame(frame);
}

// 0x100566d0
void UpdateTarget(I3D_frame *target) { static_cast<I3D_target *>(target)->UpdateFrames(); }

}  // namespace ls3d::i3d

using namespace ls3d::i3d;

void I3D_target::Init()
{
    I3D_frame::Init();
    g_targets.PushBack(this);
    mode = TARGETF_LOOK_AT;
}

void I3D_target::Uninit()
{
    for (I3D_frame *f : frames)
        f->frameFlags &= ~FRMFLAGS_TARGETED;
    frames.Clear();
    g_targets.RemoveSwap(this);
    I3D_frame::Uninit();
}

I3D_RESULT __stdcall I3D_target::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type == FRAME_TARGET)
        mode = static_cast<I3D_target *>(source)->mode;
    return I3D_OK;
}

void I3D_target::MatrixChanged()
{
    if ((frameFlags & FRMFLAGS_ON) && (frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED)))
        g_targetUpdates.PushBack(this);
}

void I3D_target::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &mode, 2);
    int count = 0;
    dtaRead(g_loadFile, &count, 1);
    if (!count)
        return;
    g_loadTargets.PushBack(this);
    frames.Reserve(count > frames.Size() ? count : frames.Size());
    while (count--) {
        uintptr_t index = 0;
        dtaRead(g_loadFile, &index, 2);
        if (index)
            frames.PushBack(reinterpret_cast<I3D_frame *>(index));
    }
}

void __stdcall I3D_target::AddFrame(I3D_frame *frame)
{
    if (frame == this)
        return;
    if (frame->frameFlags & FRMFLAGS_TARGETED)
        RemoveFromTargets(frame);
    frames.PushBack(frame);
    frame->frameFlags |= FRMFLAGS_TARGETED;
}

I3D_RESULT __stdcall I3D_target::RemoveFrame(I3D_frame *frame)
{
    if (!frames.RemoveSwap(frame))
        return I3DERR_OBJECTNOTFOUND;
    if (frame)
        frame->frameFlags &= ~FRMFLAGS_TARGETED;
    return I3D_OK;
}

void __stdcall I3D_target::SetMode(uint32_t newMode)
{
    mode = newMode;
    UpdateFrames();
}

void I3D_target::UpdateFrames()
{
    const int count = frames.Size();
    if (!count)
        return;
    const S_vector targetPos = GetMatrix()(3);
    for (int i = 0; i < count; ++i) {
        I3D_frame *f = frames[i];
        if (mode & TARGETF_LOOK_AT) {
            S_vector dir;
            dir.SetNormalized(targetPos - f->GetMatrix()(3));
            S_vector axis = f->GetMatrix()(2);
            axis.Normalize();
            if (std::fabs(axis.x - dir.x) > ls3d::kEpsilon || std::fabs(axis.y - dir.y) > ls3d::kEpsilon ||
                std::fabs(axis.z - dir.z) > ls3d::kEpsilon) {
                f->SetWorldDir(dir, 0.0f);
                f->Update();
            }
        } else if (mode & (TARGETF_ROTATION | TARGETF_POSITION | TARGETF_SCALE)) {
            if (!f->parent) {
                // The whole transform, whatever the mode.
                f->localMatrix = GetMatrix();
                f->scale = f->localMatrix.GetScale();
                f->frameFlags = (f->frameFlags & ~0x28u) | FRMFLAGS_CHANGED | FRMFLAGS_LMATRIX_VALID |
                                FRMFLAGS_ROT_VALID;
                continue;
            }
            S_matrix inv;
            inv.Inverse(f->parent->GetMatrix());
            const S_matrix m = GetMatrix() * inv;
            if (mode & TARGETF_POSITION)
                f->SetLocalPos(m(3));
            if (mode & TARGETF_SCALE) {
                f->scale = m.GetScale();
                if (!(f->frameFlags & FRMFLAGS_ROT_QUAT_VALID))
                    f->rot.Make(f->localMatrix);
                f->frameFlags = (f->frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
            }
            if (mode & TARGETF_ROTATION) {
                f->localMatrix.SetDir3(m(2), m(1));
                f->frameFlags = (f->frameFlags & ~0x12Cu) | FRMFLAGS_CHANGED | FRMFLAGS_LMATRIX_VALID;
            }
        }
    }
}
