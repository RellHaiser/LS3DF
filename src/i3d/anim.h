// Keyframe animation: animation sets (I3D_animation_set, 0x10004c60 - 0x100054b0), poses
// (I3D_pose / I3D_pose_set, 0x10043210 - 0x10043700) and the per-model animation stack with its
// tracks (I3D_anim_stack / I3D_anim_track, 0x1001ee00 - 0x10020a20).
//
// An animation set is a loaded blob of tracks, one per frame name: key times (in frames of 40 ms)
// with positions, rotations, scales and note keys. A model (or scene) plays up to 8 sets at once in
// the stages of its animation stack; for each animated frame a track holds the keys of every
// stage, and evaluating it blends the stages by weight (positions and scales linearly, rotations
// by slerp), then a pose on top (an override of position / scale / rotation, blended or added),
// and calls the frame's callback for the note keys passed.
#pragma once

#include "core/c_vector.h"
#include "i3d/i3d.h"

class I3D_frame;

// Track flags (the first word of a track in the blob).
enum : uint32_t {
    ANIMTRACK_POSITION = 0x02,
    ANIMTRACK_ROTATION = 0x04,
    ANIMTRACK_SCALE = 0x08,
    ANIMTRACK_NOTES16 = 0x10,               // note keys with 16-bit values (callback message 0x200)
    ANIMTRACK_NOTES32 = 0x20,               // note keys with 32-bit values (message 0x1000)
};

// Frame callback messages of the animation (FRMFLAGS_CALLBACK_MASK bits).
enum : uint32_t {
    I3DCALLBACK_NOTE16 = 0x200,             // (frame, 0x200, value, time ms)
    I3DCALLBACK_ANIM_END = 0x800,           // (frame, 0x800, stage, 0): a stage reached its end
    I3DCALLBACK_NOTE32 = 0x1000,            // (frame, 0x1000, value, time ms)
};

// (A frame with an animation track.)
constexpr uint32_t FRMFLAGS_ANIMATED = 0x10000000;

// The frame time of the keys: 40 ms.
constexpr int kAnimFrameTime = 40;

// ---------------------------------------------------------------- animation set

class I3D_animation_set final {
public:
    I3D_animation_set();                    // 0x10004c60 (I3D_driver::CreateAnimationSet)
    ~I3D_animation_set();                   // (inline in Release; not in the vtable)
    I3D_animation_set(const I3D_animation_set &) = delete;
    I3D_animation_set &operator=(const I3D_animation_set &) = delete;

    // 0x00 0x10004c80: the last reference deletes the set (below 0 nothing happens).
    virtual uint32_t __stdcall Release();
    // 0x04 0x10004ce0: loads the file's tracks; the name becomes the file's base name.
    virtual I3D_RESULT __stdcall Open(const char *fileName, uint32_t flags);
    // 0x08 0x10004e80: the length in ms (frames x 40).
    virtual int __stdcall GetLength();
    // 0x0C / 0x10 / 0x14 0x10004e60 / 0x10004ea0 / 0x10004ee0
    virtual int __stdcall NumTracks();
    virtual const char *__stdcall GetTrackName(int track);
    virtual int __stdcall FindTrack(const char *name);       // -1 if none (case-sensitive)
    // 0x18 - 0x38 0x10005020 - 0x100053d0: the keys of a track (times in ms).
    virtual int __stdcall NumPositionKeys(int track);
    virtual I3D_RESULT __stdcall GetPositionKey(int track, int key, int *time, S_vector *pos);
    virtual int __stdcall NumRotationKeys(int track);
    virtual I3D_RESULT __stdcall GetRotationKey(int track, int key, int *time, S_quat *rot);
    virtual int __stdcall NumScaleKeys(int track);
    virtual I3D_RESULT __stdcall GetScaleKey(int track, int key, int *time, S_vector *scale);
    // 0x30: the note keys of either kind; `*is16` (when given): ANIMTRACK_NOTES16.
    virtual int __stdcall NumNoteKeys(int track, int *is16);
    virtual I3D_RESULT __stdcall GetNoteKey16(int track, int key, int *time, uint16_t *value);   // 0x34
    virtual I3D_RESULT __stdcall GetNoteKey32(int track, int key, int *time, uint32_t *value);   // 0x38

    // The blob: uint16 track count, uint16 frame count, then a (name, track) pointer pair per
    // track (0x10004ed0 returns the pairs).
    struct TrackEntry {
        const char *name;
        uint8_t *track;
    };
    const TrackEntry *Tracks() const { return data ? reinterpret_cast<const TrackEntry *>(data + 4) : nullptr; }

    int refCount;                           // +0x04
    int defaultMode;                        // +0x08 (1) I3D_anim_stack's mode when 0 is asked for
    char *name;                             // +0x0C
    uint8_t *data;                          // +0x10 16-byte aligned (the loader's)
};
static_assert(sizeof(I3D_animation_set) == 0x14, "I3D_animation_set layout");

// ---------------------------------------------------------------- poses

// Pose flags (I3D_pose::poseFlags).
enum : uint32_t {
    POSEF_POSITION = 0x1,
    POSEF_SCALE = 0x2,
    POSEF_ROTATION_ADD = 0x4,               // the rotation is applied on top of the animation's
    POSEF_ROTATION_SET = 0x8,               // the rotation replaces (blends to) the animation's
};

class I3D_pose final {
public:
    I3D_pose();                             // 0x10043210 (I3D_driver::CreatePose)
    I3D_pose(const I3D_pose &) = delete;
    I3D_pose &operator=(const I3D_pose &) = delete;

    // 0x00 0x10043270: the last reference frees the pose.
    virtual uint32_t __stdcall Release();
    virtual void __stdcall SetName(const char *name);                      // 0x04 0x10043460
    virtual void __stdcall Clear();                                        // 0x08 0x100432c0
    // 0x0C / 0x10 0x100433e0 / 0x10043420 (16 bytes each; null: unset).
    virtual void __stdcall SetPosition(const S_vector4 *pos);
    virtual void __stdcall SetScale(const S_vector4 *scale);
    // 0x14 / 0x18 0x100432d0 / 0x10043310: the rotation added / set (null: unset).
    virtual void __stdcall SetRotationAdd(const S_quat *rot);
    virtual void __stdcall SetRotationSet(const S_quat *rot);
    // 0x1C 0x10043390: the added rotation from a direction (keeps POSEF_ROTATION_SET).
    virtual void __stdcall SetDirAdd(const S_vector &dir, float roll);
    // 0x20 / 0x24 0x10043350 / 0x100433b0: the added / set rotation from a direction (0x20: a
    // null direction unsets both).
    virtual void __stdcall SetDirAddOnly(const S_vector *dir, float roll);
    virtual void __stdcall SetDirSet(const S_vector &dir, float roll);

    // Poses live in 16-byte aligned blocks.
    static void *operator new(size_t size) noexcept;
    static void operator delete(void *p);

    uint32_t padding04[3];
    S_vector4 position;                     // +0x10
    S_vector4 scale;                        // +0x20
    S_quat rotation;                        // +0x30
    int refCount;                           // +0x40
    char *name;                             // +0x44 the frame it is for
    uint32_t poseFlags;                     // +0x48 POSEF_*
    uint32_t padding4C;
};
static_assert(sizeof(I3D_pose) == 0x50, "I3D_pose layout");

class I3D_pose_set final {
public:
    I3D_pose_set();                         // 0x100434c0 (I3D_driver::CreatePoseSet)
    ~I3D_pose_set();                        // 0x100434f0: releases its poses
    I3D_pose_set(const I3D_pose_set &) = delete;
    I3D_pose_set &operator=(const I3D_pose_set &) = delete;

    virtual uint32_t __stdcall Release();                                  // 0x00 0x10043560
    // 0x04 0x100435d0: names the pose and keeps a reference.
    virtual void __stdcall AddPose(I3D_pose *pose, const char *name);
    virtual I3D_RESULT __stdcall RemovePose(I3D_pose *pose);               // 0x08 0x10043650
    virtual const char *__stdcall GetPoseName(int index);                  // 0x0C 0x100435a0

    int refCount;                           // +0x04
    ls3d::C_vector<I3D_pose *> poses;       // +0x08
};
static_assert(sizeof(I3D_pose_set) == 0x18, "I3D_pose_set layout");

// ---------------------------------------------------------------- the animation stack

class I3D_anim_stack;

// One stage's keys of a track (pointers into the animation set's blob).
struct I3D_anim_keys {
    const uint16_t *positions;              // +0x00 the times, then (aligned) the keys
    const uint16_t *rotations;              // +0x04
    const uint16_t *scales;                 // +0x08
    const uint16_t *notes;                  // +0x0C
    uint16_t numPositions;                  // +0x10
    uint16_t numRotations;                  // +0x12
    uint16_t numScales;                     // +0x14
    uint16_t numNotes;                      // +0x16
    uint16_t flags;                         // +0x18 ANIMTRACK_* of the stage's set
    uint16_t padding1A;
};
static_assert(sizeof(I3D_anim_keys) == 0x1C, "I3D_anim_keys layout");

// The animation of one frame (vtable 0x1009cac0, 0xF4 bytes).
class I3D_anim_track final {
public:
    I3D_anim_track(I3D_anim_stack *owner, I3D_frame *frame);
    ~I3D_anim_track();                      // (inline in the original: releases the pose)
    I3D_anim_track(const I3D_anim_track &) = delete;
    I3D_anim_track &operator=(const I3D_anim_track &) = delete;

    virtual void __stdcall ClearStage(int stage);       // 0x00 0x1001eeb0: the stage's keys unused
    virtual void __stdcall SetPose(I3D_pose *newPose);  // 0x04 0x1001fed0
    virtual void __stdcall SetPoseWeight(float weight); // 0x08 0x1001eea0

    // 0x1001eed0: the frame's transform from the stages and the pose; the notes passed.
    void Evaluate();

    I3D_pose *pose;                         // +0x04
    float poseWeight;                       // +0x08 (1)
    I3D_anim_keys stages[8];                // +0x0C
    I3D_frame *frame;                       // +0xEC
    I3D_anim_stack *stack;                  // +0xF0
};
static_assert(sizeof(I3D_anim_track) == 0xF4, "I3D_anim_track layout");

// Loop modes (I3D_anim_stack::Stage::mode).
enum : int {
    ANIMMODE_ONCE = 1,                      // stops at the end
    ANIMMODE_LOOP = 2,
};

class I3D_anim_stack {
public:
    // A playing animation (0x1C bytes).
    struct Stage {
        I3D_animation_set *set;             // +0x00
        bool active;                        // +0x04
        uint8_t padding05[3];
        int mode;                           // +0x08 ANIMMODE_*
        int time;                           // +0x0C ms
        int prevTime;                       // +0x10 at the last evaluation (-40 at the start)
        int lastDelta;                      // +0x14 the last advance (negative: backwards)
        float weight;                       // +0x18
    };
    static_assert(sizeof(Stage) == 0x1C, "I3D_anim_stack::Stage layout");

    explicit I3D_anim_stack(I3D_frame *owner);         // 0x1001ff20 (a model or a scene)
    ~I3D_anim_stack();                                 // 0x1001ff60
    I3D_anim_stack(const I3D_anim_stack &) = delete;
    I3D_anim_stack &operator=(const I3D_anim_stack &) = delete;

    void Clear();                                      // 0x1001ffc0: tracks and stages go
    // 0x10020150 / 0x10020040: every stage (the stages each) advance(s); after a change the
    // tracks are evaluated and the stages at their end loop or stop.
    void Tick(int delta);
    void Tick(const int (&deltas)[8]);
    // 0x10020260: `set` plays in stage `stage` (null: the stage stops) at `weight`, `mode` 0 being
    // the set's default; the owner's frames named like its tracks get them.
    I3D_RESULT SetAnimation(I3D_animation_set *set, int stage, float weight, int mode);
    I3D_RESULT SetPose(I3D_frame *frame, I3D_pose *pose);          // 0x100204d0
    I3D_RESULT SetPose(const char *frameName, I3D_pose *pose);     // 0x10020500
    I3D_RESULT SetPoseSet(I3D_pose_set *set);                      // 0x10020530
    void SetPoseWeight(float weight);                              // 0x10020590: every track's
    I3D_RESULT SetPoseWeight(float weight, I3D_pose_set *set);     // 0x100205d0: the set's frames'
    float GetPoseWeight();                                         // 0x10020630: the first posed track's
    void ClearPoses();                                             // 0x10020670

    I3D_frame *owner;                       // +0x00
    bool dirty;                             // +0x04 the tracks need an evaluation
    uint8_t padding05[3];
    Stage stages[8];                        // +0x08
    ls3d::C_vector<I3D_anim_track *> tracks;   // +0xE8

private:
    I3D_anim_track *GetTrack(I3D_frame *frame);        // 0x10020740: found or made
    I3D_anim_track *GetTrack(const char *frameName);   // 0x10020880: (the owner's FindChildFrame)
    void PurgeTracks();                                // 0x100206b0: tracks with nothing to do go
    void EndStages();                                  // the end of the Ticks
};
static_assert(sizeof(I3D_anim_stack) == 0xF8, "I3D_anim_stack layout");
static_assert(offsetof(I3D_anim_stack, tracks) == 0xE8, "I3D_anim_stack layout");
