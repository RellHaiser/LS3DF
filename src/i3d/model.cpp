// I3D_model (0x10033fe0 - 0x10034ef0).
#include "i3d/model.h"

#include <cstring>
#include <new>

#include "core/debug.h"
#include "i3d/anim.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/target.h"
#include "i3d/visual.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

// The name of a frame without its model prefix ("" without a name).
const char *ShortName(const I3D_frame *f) { return f->name ? f->name + f->nameOffset : ""; }

// The shared file name block: an int16 reference count, then the text.
int16_t &NameRefCount(char *text) { return reinterpret_cast<int16_t *>(text)[-1]; }

// DuplicateModel's state (0x100be5e0 - 0x100be624): the new frames that were targets' frames
// with the source frame's target, the new shadows, the model and its source.
struct TargetLink {
    I3D_frame *frame;
    I3D_frame *sourceTarget;
};
C_vector<TargetLink> s_targetLinks(0x10);
C_vector<I3D_frame *> s_shadows(0x20);
I3D_model *s_duplicating;

// 0x10034920: copies of `source`'s children (not models) below `parent`, recursively. (Each new
// child goes to the front of the list: the copies' order is the reverse of the originals'.)
I3D_RESULT CloneChildren(I3D_frame *parent, I3D_frame *source)
{
    for (I3D_frame *c = source->firstChild; c; c = c->nextSibling) {
        I3D_frame *copy;
        if (c->type == FRAME_VISUAL)
            copy = g_driver->CreateVisual(c->GetVisualType());
        else if (c->type != FRAME_MODEL)
            copy = g_driver->CreateFrame(c->type);
        else
            continue;
        if (!copy) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        I3D_RESULT result = copy->Duplicate(c);
        if (result < 0)
            return result;
        if (c->frameFlags & FRMFLAGS_TARGETED)
            s_targetLinks.PushBack({copy, FindTargetOf(c)});
        if (copy->type == FRAME_SHADOW)
            s_shadows.PushBack(copy);
        parent->AddChildFront(copy);
        s_duplicating->AddFrame(copy);
        copy->Release();
        result = CloneChildren(copy, c);
        if (copy->type == FRAME_VISUAL) {
            const uint32_t fourCC = c->GetVisualType();
            if (fourCC == 0x4d474e53 || fourCC == 0x524d4d53)     // 'SNGM' / 'SMMR'
                SkinFixJoints(copy);
        }
        if (result < 0)
            return result;
    }
    return I3D_OK;
}

}  // namespace

// ---------------------------------------------------------------- frame slots

void I3D_model::Init()
{
    I3D_frame::Init();
    fileName = nullptr;
    animStack = nullptr;
}

void I3D_model::Uninit()
{
    Close();
    I3D_frame::Uninit();
}

void I3D_model::Tick(int time)
{
    if (animStack && animStack->tracks.Size()) {
        animStack->Tick(time);
        Update();
    }
}

I3D_RESULT I3D_model::SetName(const char *newName)
{
    I3D_RESULT result = I3D_frame::SetName(newName);
    if (result >= 0)
        result = ApplyNamePrefix();
    return result;
}

I3D_RESULT I3D_model::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    I3D_RESULT result = I3D_frame::Duplicate(source);
    if (result >= 0 && source->GetType() == FRAME_MODEL)
        result = DuplicateModel(static_cast<I3D_model *>(source));
    return result;
}

I3D_RESULT I3D_model::ApplyNamePrefix()
{
    const char *prefix = name ? name : "";
    if (!*prefix) {
        for (I3D_frame *f : frames) {
            const I3D_RESULT result = f->RemoveNamePrefix();
            if (result < 0)
                return result;
        }
    } else {
        for (I3D_frame *f : frames) {
            const I3D_RESULT result = f->SetNamePrefix(prefix);
            if (result < 0)
                return result;
        }
    }
    return I3D_OK;
}

// ---------------------------------------------------------------- the model's own slots

I3D_RESULT I3D_model::Open(const char *file, uint32_t flags, void *loadCallback, void *context)
{
    Close();
    const uint32_t savedNames = g_driverStates[DRVSTATE_FRAME_NAMES];
    if (*(name ? name : ""))
        g_driverStates[DRVSTATE_FRAME_NAMES] = 0;
    const I3D_RESULT result = OpenModelFile(file, flags, loadCallback, context, this);
    g_driverStates[DRVSTATE_FRAME_NAMES] = savedNames;
    if (result < 0)
        return result;
    // The base name: between the last backslash and the last dot (a dot before the last
    // backslash leaves nothing).
    const char *start = file, *dot = nullptr, *p = file;
    for (; *p; ++p) {
        if (*p == '.')
            dot = p;
        else if (*p == '\\')
            start = p + 1;
    }
    if (!dot)
        dot = p;
    const int length = static_cast<int>(dot - start);
    if (length > 0) {
        auto *block = static_cast<int16_t *>(::operator new(length + 3, std::nothrow));
        if (!block) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        *block = 1;
        fileName = reinterpret_cast<char *>(block + 1);
        std::memcpy(fileName, start, length);
        fileName[length] = 0;
    }
    if (!*(name ? name : ""))
        return I3D_OK;
    return ApplyNamePrefix();
}

void I3D_model::Close()
{
    if (animStack) {
        delete animStack;
    }
    animStack = nullptr;
    for (I3D_frame *f : frames) {
        f->ownerModel = nullptr;
        f->Release();
    }
    frames.Clear();
    if (fileName) {
        if (--NameRefCount(fileName) < 1)
            ::operator delete(&NameRefCount(fileName));
        fileName = nullptr;
    }
    ClearParentBounds();
}

void I3D_model::AddFrame(I3D_frame *frame)
{
    ++frame->refCount;
    frame->NotifyOwnerRemoved();
    frames.PushBack(frame);
    frame->ownerModel = this;
}

I3D_RESULT I3D_model::RemoveFrame(I3D_frame *frame)
{
    if (!frames.RemoveSwap(frame))
        return I3DERR_OBJECTNOTFOUND;
    frame->ownerModel = nullptr;
    if (frame->refCount > 0)
        frame->Release();
    return I3D_OK;
}

I3D_anim_stack *I3D_model::Stack()
{
    if (!animStack) {
        animStack = new (std::nothrow) I3D_anim_stack(this);
        if (!animStack)
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
    }
    return animStack;
}

I3D_RESULT I3D_model::SetAnimation(I3D_animation_set *set, int stage, float weight, int mode)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!Stack())
        return I3DERR_OUTOFMEM;
    return animStack->SetAnimation(set, stage, weight, mode);
}

I3D_animation_set *I3D_model::GetAnimation(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return nullptr;
    return animStack->stages[stage].set;
}

void I3D_model::StopAnimation(int stage)
{
    if (static_cast<uint32_t>(stage) < 8 && animStack)
        animStack->stages[stage].active = false;
}

void I3D_model::StartAnimation(int stage)
{
    if (static_cast<uint32_t>(stage) < 8 && animStack)
        animStack->stages[stage].active = true;
}

I3D_RESULT I3D_model::SetAnimWeight(int stage, float weight)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    if (weight < 0.0f) {
        animStack->stages[stage].weight = 0.0f;
    } else {
        if (1.0f < weight)
            weight = 1.0f;
        animStack->stages[stage].weight = weight;
    }
    animStack->dirty = true;
    return I3D_OK;
}

float I3D_model::GetAnimWeight(int stage)
{
    if (static_cast<uint32_t>(stage) >= 8 || !animStack)
        return 0.0f;
    return animStack->stages[stage].weight;
}

I3D_RESULT I3D_model::SetAnimTime(int stage, int time)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    animStack->stages[stage].time = time;
    animStack->stages[stage].prevTime = time;
    animStack->dirty = true;
    return I3D_OK;
}

int I3D_model::GetAnimTime(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return 0;
    return animStack->stages[stage].time;
}

I3D_RESULT I3D_model::SetAnimMode(int stage, int mode)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    animStack->stages[stage].mode = mode;
    return I3D_OK;
}

int I3D_model::GetAnimMode(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return 0;
    return animStack->stages[stage].mode;
}

bool I3D_model::IsAnimActive(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return false;
    return animStack->stages[stage].active;
}

void I3D_model::TickStages(const int (&deltas)[8])
{
    if (animStack) {
        animStack->Tick(deltas);
        Update();
    }
}

I3D_RESULT I3D_model::SetPoseSet(I3D_pose_set *set)
{
    if (!Stack())
        return I3DERR_OUTOFMEM;
    return animStack->SetPoseSet(set);
}

I3D_RESULT I3D_model::SetPoseByName(const char *frameName, I3D_pose *pose)
{
    if (!Stack())
        return I3DERR_OUTOFMEM;
    return animStack->SetPose(frameName, pose);
}

I3D_RESULT I3D_model::SetPose(I3D_frame *frame, I3D_pose *pose)
{
    if (!Stack())
        return I3DERR_OUTOFMEM;
    return animStack->SetPose(frame, pose);
}

I3D_RESULT I3D_model::SetPoseSetWeight(float weight, I3D_pose_set *set)
{
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    return animStack->SetPoseWeight(weight, set);
}

I3D_RESULT I3D_model::SetPoseWeight(float weight)
{
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    animStack->SetPoseWeight(weight);
    return I3D_OK;
}

float I3D_model::GetPoseWeight() { return animStack ? animStack->GetPoseWeight() : 0.0f; }

void I3D_model::ClearPoses()
{
    if (animStack)
        animStack->ClearPoses();
}

int I3D_model::NumTracks() { return animStack ? animStack->tracks.Size() : 0; }

void *I3D_model::GetTracks() { return animStack ? animStack->tracks.Data() : nullptr; }

I3D_RESULT I3D_model::DuplicateModel(I3D_model *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    Close();
    if (source->fileName) {
        fileName = source->fileName;
        ++NameRefCount(fileName);
    }
    const uint32_t savedNames = g_driverStates[DRVSTATE_FRAME_NAMES];
    g_driverStates[DRVSTATE_FRAME_NAMES] = 0;
    s_targetLinks.Clear();
    s_shadows.Clear();
    s_duplicating = this;
    I3D_RESULT result = CloneChildren(this, source);
    g_driverStates[DRVSTATE_FRAME_NAMES] = savedNames;
    if (result < 0)
        return result;
    result = ApplyNamePrefix();
    // The copies of targeted frames join the copies of their targets (found by name).
    for (const TargetLink &link : s_targetLinks) {
        const char *targetName = ShortName(link.sourceTarget);
        for (I3D_frame *f : frames) {
            if (!std::strcmp(ShortName(f), targetName)) {
                if (f)
                    static_cast<I3D_target *>(f)->AddFrame(link.frame);
                break;
            }
        }
    }
    for (I3D_frame *shadow : s_shadows) {
        if (shadow->parent)
            ShadowSetSource(shadow, shadow->parent);
    }
    Update();
    if (source->animStack)
        SetAnimation(source->animStack->stages[0].set, 0, 1.0f, 0);
    return result;
}

// ---------------------------------------------------------------- free functions

namespace ls3d::i3d {

void ModelAddFrame(I3D_frame *model, I3D_frame *frame) { static_cast<I3D_model *>(model)->AddFrame(frame); }
void ModelFrameReleased(I3D_frame *model, I3D_frame *frame) { static_cast<I3D_model *>(model)->RemoveFrame(frame); }

}  // namespace ls3d::i3d
