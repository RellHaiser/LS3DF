// I3D_model: a loaded model file (FRAME_MODEL; vtable 0x1009ba50, 0x140 bytes; 0x10033fe0 -
// 0x10034ef0). The model owns the frames it loaded (each named "<model name>.<frame name>") and
// plays animations on them through its animation stack.
#pragma once

#include "core/c_vector.h"
#include "i3d/frame.h"

class I3D_anim_stack;
class I3D_animation_set;
class I3D_pose;
class I3D_pose_set;

class I3D_model : public I3D_frame {
public:
    // 0x20 0x10034ba0: the animations advance (with tracks to play) and the model updates.
    void __stdcall Tick(int time) override;
    // 0x34 0x100346a0: also the prefix of the model's frames.
    I3D_RESULT __stdcall SetName(const char *newName) override;
    // 0x4C 0x10034b60: also another model's frames (DuplicateModel).
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10033fe0 / 0x10034000
    void Init() override;
    void Uninit() override;
    // 0x74 0x100340b0: loads a model file (frame names stay out of the driver's name tree while
    // a named model loads); the file name is kept without directory and extension.
    virtual I3D_RESULT __stdcall Open(const char *fileName, uint32_t flags, void *loadCallback, void *context);
    // 0x78 0x10034020: the animations, frames and file name go.
    virtual void __stdcall Close();
    // 0x7C / 0x80 0x100341c0 / 0x10034260: frames owned by the model (a reference each).
    virtual void __stdcall AddFrame(I3D_frame *frame);
    virtual I3D_RESULT __stdcall RemoveFrame(I3D_frame *frame);
    // 0x84 - 0xAC 0x100342f0 - 0x10034600: the stages (0 - 7) of the animation stack.
    virtual I3D_RESULT __stdcall SetAnimation(I3D_animation_set *set, int stage, float weight, int mode);
    virtual I3D_animation_set *__stdcall GetAnimation(int stage);
    virtual void __stdcall StopAnimation(int stage);
    virtual void __stdcall StartAnimation(int stage);
    virtual I3D_RESULT __stdcall SetAnimWeight(int stage, float weight);   // clamped to 0 - 1
    virtual float __stdcall GetAnimWeight(int stage);
    virtual I3D_RESULT __stdcall SetAnimTime(int stage, int time);
    virtual int __stdcall GetAnimTime(int stage);
    virtual I3D_RESULT __stdcall SetAnimMode(int stage, int mode);
    virtual int __stdcall GetAnimMode(int stage);
    virtual bool __stdcall IsAnimActive(int stage);
    // 0xB0 0x10034bd0: each stage advances by its own delta; the model updates.
    virtual void __stdcall TickStages(const int (&deltas)[8]);
    // 0xB4 - 0xD4 0x10034c60 - 0x10034c40: poses.
    virtual I3D_RESULT __stdcall SetPoseSet(I3D_pose_set *set);
    // (Distinct names: MSVC orders overloaded virtual functions in reverse in the vtable.)
    virtual I3D_RESULT __stdcall SetPoseByName(const char *frameName, I3D_pose *pose);
    virtual I3D_RESULT __stdcall SetPose(I3D_frame *frame, I3D_pose *pose);
    virtual I3D_RESULT __stdcall SetPoseSetWeight(float weight, I3D_pose_set *set);
    virtual I3D_RESULT __stdcall SetPoseWeight(float weight);
    virtual float __stdcall GetPoseWeight();
    virtual void __stdcall ClearPoses();
    virtual int __stdcall NumTracks();
    virtual void *__stdcall GetTracks();
    // 0xD8 0x10034760: a copy of another model's frame hierarchy (new frames, the targets and
    // shadows linked up again), playing its stage 0 animation.
    virtual I3D_RESULT __stdcall DuplicateModel(I3D_model *source);

    // 0x10034630: the model's name as the prefix of its frames' names (none without a name).
    I3D_RESULT ApplyNamePrefix();

    ls3d::C_vector<I3D_frame *> frames;     // +0x120
    I3D_anim_stack *animStack;              // +0x130 made on demand
    // +0x134 the file's base name (shared by duplicates: an int16 reference count precedes it).
    char *fileName;
    uint32_t unknown138[2];                 // +0x138

private:
    I3D_anim_stack *Stack();                // the stack, made when missing (null when out of memory)
};
static_assert(sizeof(I3D_model) == 0x140, "I3D_model layout");
static_assert(offsetof(I3D_model, frames) == 0x120, "I3D_model layout");
static_assert(offsetof(I3D_model, animStack) == 0x130, "I3D_model layout");
