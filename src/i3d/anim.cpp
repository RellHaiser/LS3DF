// Keyframe animation (0x10004c60 - 0x100054b0, 0x1001ee00 - 0x10020a20, 0x10043210 - 0x10043700).
#include "i3d/anim.h"

#include <cstring>
#include <new>

#include "core/debug.h"
#include "i3d/frame.h"
#include "i3d/i3d_hooks.h"
#include "i3d/model.h"
#include "i3d/scene.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

uint16_t U16(const uint8_t *p)
{
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint32_t U32(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// The blob's sections: rotations (count, times, 16-byte aligned quaternions), positions and
// scales (count, times, a pad to 4 bytes when the count is even, vectors), notes (count, times,
// values: 16-bit right after the times, 32-bit after the same pad).
const uint8_t *AlignUp16(const uint8_t *p)
{
    return p + (-reinterpret_cast<intptr_t>(p) & 0xF);
}
const uint8_t *RotationKeys(const uint8_t *section)
{
    return AlignUp16(section + 2 + 2 * U16(section));
}
const uint8_t *SkipRotations(const uint8_t *section)
{
    return RotationKeys(section) + 16 * U16(section);
}
const uint8_t *VectorKeys(const uint8_t *section)
{
    const int n = U16(section);
    return section + 2 + 2 * n + (n & 1 ? 0 : 2);
}
const uint8_t *SkipVectors(const uint8_t *section)
{
    return VectorKeys(section) + 12 * U16(section);
}

// The sections of a track, by its flags.
const uint8_t *PositionSection(const uint8_t *track)
{
    const uint8_t *p = track + 4;
    if (U32(track) & ANIMTRACK_ROTATION)
        p = SkipRotations(p);
    return p;
}
const uint8_t *ScaleSection(const uint8_t *track)
{
    const uint8_t *p = PositionSection(track);
    if (U32(track) & ANIMTRACK_POSITION)
        p = SkipVectors(p);
    return p;
}
const uint8_t *NoteSection(const uint8_t *track)
{
    const uint8_t *p = ScaleSection(track);
    if (U32(track) & ANIMTRACK_SCALE)
        p = SkipVectors(p);
    return p;
}

// The part of `path` between the last backslash and the last dot (searched in the whole path: a
// dot before the last backslash leaves nothing); `length` <= 0 for none.
void BaseName(const char *path, const char *&start, int &length)
{
    const char *dot = nullptr;
    start = path;
    const char *p = path;
    for (; *p; ++p) {
        if (*p == '.')
            dot = p;
        else if (*p == '\\')
            start = p + 1;
    }
    if (!dot)
        dot = p;
    length = static_cast<int>(dot - start);
}

// The name of a frame without its model prefix ("" without a name).
const char *ShortName(const I3D_frame *f) { return f->name ? f->name + f->nameOffset : ""; }

// The frames a stack's owner owns (a model's or a scene's list).
const C_vector<I3D_frame *> &OwnedFrames(I3D_frame *owner)
{
    if (owner->GetType() == FRAME_MODEL)
        return static_cast<I3D_model *>(owner)->frames;
    return static_cast<I3D_scene *>(owner)->frames;
}

// The frame index (40 ms) of a time, as the original's 16-bit compares see it.
uint16_t KeyFrame(int time) { return static_cast<uint16_t>(time / kAnimFrameTime); }

// Where `frame` is among `times` (n of them): the key before and after it and the fraction (the
// original's float steps: the time in frames, minus the key's, over the keys' distance), or
// `*single` when the frame is before the first or at / after the last key.
struct KeySpan {
    int k0, k1;
    float f;
    bool single;
};
KeySpan FindKeys(const uint16_t *times, int n, int time)
{
    const uint16_t frame = KeyFrame(time);
    if (frame < times[0])
        return {0, 0, 0.0f, true};
    const int last = n - 1;
    if (!(frame < times[last]))
        return {last, last, 0.0f, true};
    int i = 1;
    if (last != 1) {
        while (i < last && !(frame < times[i]))
            ++i;
    }
    const int t0 = times[i - 1], t1 = times[i];
    const float f = (static_cast<float>(time) * 0.025f - static_cast<float>(t0)) / static_cast<float>(t1 - t0);
    return {i - 1, i, f, false};
}

// A blend weight of a stage counts when above 0 (or NaN, as the original's test lets through).
bool Counts(float w) { return !(w < 0.0f) && !(w == 0.0f); }

}  // namespace

// ---------------------------------------------------------------- I3D_animation_set

I3D_animation_set::I3D_animation_set() : refCount(1), defaultMode(1), name(nullptr), data(nullptr) {}

I3D_animation_set::~I3D_animation_set()
{
    AlignedFree(data);
    ::operator delete(name);
}

uint32_t I3D_animation_set::Release()
{
    if (--refCount < 0)
        return 0;
    if (refCount)
        return refCount;
    delete this;
    return 0;
}

I3D_RESULT I3D_animation_set::Open(const char *fileName, uint32_t)
{
    AlignedFree(data);
    data = nullptr;
    if (name) {
        ::operator delete(name);
        name = nullptr;
    }
    const I3D_RESULT result = OpenAnimationFile(fileName, this);
    if (result < 0)
        return result;
    // The blob's offsets become pointers: the entries', then the 32-bit notes' (offsets from the
    // end of their value array).
    const int n = U16(data);
    auto *entries = reinterpret_cast<uint32_t *>(data + 4);
    for (int i = 0; i < 2 * n; ++i)
        entries[i] += static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
    for (int t = 0; t < n; ++t) {
        uint8_t *track = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(entries[2 * t + 1]));
        if (!(U32(track) & ANIMTRACK_NOTES32))
            continue;
        const uint8_t *section = NoteSection(track);
        const int count = U16(section);
        uint8_t *values = const_cast<uint8_t *>(VectorKeys(section));
        for (int k = 0; k < count; ++k) {
            const uint32_t offset = U32(values + 4 * k);
            const uint32_t pointer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(values)) + offset + 4 * count;
            std::memcpy(values + 4 * k, &pointer, sizeof pointer);
        }
    }
    const char *start;
    int length;
    BaseName(fileName, start, length);
    if (length > 0) {
        name = static_cast<char *>(::operator new(length + 1, std::nothrow));
        if (!name) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        std::memcpy(name, start, length);
        name[length] = 0;
    }
    return I3D_OK;
}

int I3D_animation_set::GetLength() { return data ? U16(data + 2) * kAnimFrameTime : 0; }

int I3D_animation_set::NumTracks() { return data ? U16(data) : 0; }

const char *I3D_animation_set::GetTrackName(int track)
{
    // (The original takes a negative index: a read before the table. Refused.)
    if (!data || track < 0 || U16(data) <= track)
        return nullptr;
    return Tracks()[track].name;
}

int I3D_animation_set::FindTrack(const char *trackName)
{
    if (!data)
        return -1;
    for (int i = 0; i < U16(data); ++i) {
        if (!std::strcmp(Tracks()[i].name, trackName))
            return i;
    }
    return -1;
}

int I3D_animation_set::NumPositionKeys(int track)
{
    if (!data || static_cast<uint32_t>(track) >= U16(data))
        return 0;
    const uint8_t *t = Tracks()[track].track;
    return U32(t) & ANIMTRACK_POSITION ? U16(PositionSection(t)) : 0;
}

I3D_RESULT I3D_animation_set::GetPositionKey(int track, int key, int *time, S_vector *pos)
{
    if (!data)
        return I3DERR_NOTLOADED;
    if (static_cast<uint32_t>(track) >= U16(data))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *t = Tracks()[track].track;
    if (!(U32(t) & ANIMTRACK_POSITION))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *s = PositionSection(t);
    if (static_cast<uint32_t>(key) >= U16(s))
        return I3DERR_INVALIDPARAMS;
    *time = U16(s + 2 + 2 * key) * kAnimFrameTime;
    std::memcpy(pos, VectorKeys(s) + 12 * key, 12);
    return I3D_OK;
}

int I3D_animation_set::NumRotationKeys(int track)
{
    if (!data || static_cast<uint32_t>(track) >= U16(data))
        return 0;
    const uint8_t *t = Tracks()[track].track;
    return U32(t) & ANIMTRACK_ROTATION ? U16(t + 4) : 0;
}

I3D_RESULT I3D_animation_set::GetRotationKey(int track, int key, int *time, S_quat *rot)
{
    if (!data)
        return I3DERR_NOTLOADED;
    if (static_cast<uint32_t>(track) >= U16(data))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *t = Tracks()[track].track;
    if (!(U32(t) & ANIMTRACK_ROTATION))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *s = t + 4;
    if (static_cast<uint32_t>(key) >= U16(s))
        return I3DERR_INVALIDPARAMS;
    *time = U16(s + 2 + 2 * key) * kAnimFrameTime;
    std::memcpy(rot, RotationKeys(s) + 16 * key, 16);
    return I3D_OK;
}

int I3D_animation_set::NumScaleKeys(int track)
{
    if (!data || static_cast<uint32_t>(track) >= U16(data))
        return 0;
    const uint8_t *t = Tracks()[track].track;
    return U32(t) & ANIMTRACK_SCALE ? U16(ScaleSection(t)) : 0;
}

I3D_RESULT I3D_animation_set::GetScaleKey(int track, int key, int *time, S_vector *scaleOut)
{
    if (!data)
        return I3DERR_NOTLOADED;
    if (static_cast<uint32_t>(track) >= U16(data))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *t = Tracks()[track].track;
    if (!(U32(t) & ANIMTRACK_SCALE))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *s = ScaleSection(t);
    if (static_cast<uint32_t>(key) >= U16(s))
        return I3DERR_INVALIDPARAMS;
    *time = U16(s + 2 + 2 * key) * kAnimFrameTime;
    std::memcpy(scaleOut, VectorKeys(s) + 12 * key, 12);
    return I3D_OK;
}

int I3D_animation_set::NumNoteKeys(int track, int *is16)
{
    if (!data || static_cast<uint32_t>(track) >= U16(data))
        return 0;
    const uint8_t *t = Tracks()[track].track;
    const uint32_t flags = U32(t);
    if (!(flags & (ANIMTRACK_NOTES16 | ANIMTRACK_NOTES32)))
        return 0;
    if (is16)
        *is16 = flags >> 4 & 1;
    return U16(NoteSection(t));
}

I3D_RESULT I3D_animation_set::GetNoteKey16(int track, int key, int *time, uint16_t *value)
{
    if (!data)
        return I3DERR_NOTLOADED;
    if (static_cast<uint32_t>(track) >= U16(data))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *t = Tracks()[track].track;
    if (!(U32(t) & ANIMTRACK_NOTES16))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *s = NoteSection(t);
    const int n = U16(s);
    if (static_cast<uint32_t>(key) >= static_cast<uint32_t>(n))
        return I3DERR_INVALIDPARAMS;
    *time = U16(s + 2 + 2 * key) * kAnimFrameTime;
    *value = U16(s + 2 + 2 * (n + key));
    return I3D_OK;
}

I3D_RESULT I3D_animation_set::GetNoteKey32(int track, int key, int *time, uint32_t *value)
{
    if (!data)
        return I3DERR_NOTLOADED;
    if (static_cast<uint32_t>(track) >= U16(data))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *t = Tracks()[track].track;
    if (!(U32(t) & ANIMTRACK_NOTES32))
        return I3DERR_INVALIDPARAMS;
    const uint8_t *s = NoteSection(t);
    if (static_cast<uint32_t>(key) >= U16(s))
        return I3DERR_INVALIDPARAMS;
    *time = U16(s + 2 + 2 * key) * kAnimFrameTime;
    *value = U32(VectorKeys(s) + 4 * key);
    return I3D_OK;
}

// ---------------------------------------------------------------- poses

void *I3D_pose::operator new(size_t size) noexcept { return AlignedAlloc(size, 16); }
void I3D_pose::operator delete(void *p) { AlignedFree(p); }

I3D_pose::I3D_pose() : refCount(1), name(nullptr), poseFlags(0)
{
    // (position.w stays unset)
    position.x = position.y = position.z = 0.0f;
    scale = {1.0f, 1.0f, 1.0f, 0.0f};       // (the original's global (1, 1, 1) at 0x100d1570)
    rotation.v = S_vector(0.0f, 0.0f, 0.0f);
    rotation.s = 1.0f;
}

uint32_t I3D_pose::Release()
{
    if (--refCount < 0) {
        dbgPrintf("!! RefCount:0x%x   I3D_pose", this);
        return 0;
    }
    if (refCount)
        return refCount;
    ::operator delete(name);
    delete this;
    return 0;
}

void I3D_pose::SetName(const char *newName)
{
    ::operator delete(name);
    // (The original leaves the freed name for a null one: freed twice by Release. Cleared.)
    name = nullptr;
    if (!newName)
        return;
    name = static_cast<char *>(::operator new(std::strlen(newName) + 1, std::nothrow));
    if (!name) {
        dbgPrintf("!! Out of memory  I3D_pose::SetName()");
        return;
    }
    std::strcpy(name, newName);
}

void I3D_pose::Clear() { poseFlags = 0; }

void I3D_pose::SetPosition(const S_vector4 *pos)
{
    if (pos) {
        position = *pos;
        poseFlags |= POSEF_POSITION;
    } else {
        poseFlags &= ~POSEF_POSITION;
    }
}

void I3D_pose::SetScale(const S_vector4 *s)
{
    if (s) {
        scale = *s;
        poseFlags |= POSEF_SCALE;
    } else {
        poseFlags &= ~POSEF_SCALE;
    }
}

void I3D_pose::SetRotationAdd(const S_quat *rot)
{
    if (rot) {
        rotation = *rot;
        poseFlags = (poseFlags & ~POSEF_ROTATION_SET) | POSEF_ROTATION_ADD;
    } else {
        poseFlags &= ~POSEF_ROTATION_ADD;
    }
}

void I3D_pose::SetRotationSet(const S_quat *rot)
{
    if (rot) {
        rotation = *rot;
        poseFlags = (poseFlags & ~POSEF_ROTATION_ADD) | POSEF_ROTATION_SET;
    } else {
        poseFlags &= ~(POSEF_ROTATION_ADD | POSEF_ROTATION_SET);
    }
}

void I3D_pose::SetDirAdd(const S_vector &dir, float roll)
{
    rotation.SetDir(dir, roll);
    poseFlags |= POSEF_ROTATION_ADD;
}

void I3D_pose::SetDirAddOnly(const S_vector *dir, float roll)
{
    if (dir) {
        rotation.SetDir(*dir, roll);
        poseFlags = (poseFlags & ~POSEF_ROTATION_SET) | POSEF_ROTATION_ADD;
    } else {
        poseFlags &= ~(POSEF_ROTATION_ADD | POSEF_ROTATION_SET);
    }
}

void I3D_pose::SetDirSet(const S_vector &dir, float roll)
{
    rotation.SetDir(dir, roll);
    poseFlags = (poseFlags & ~POSEF_ROTATION_ADD) | POSEF_ROTATION_SET;
}

I3D_pose_set::I3D_pose_set() : refCount(1) {}

I3D_pose_set::~I3D_pose_set()
{
    for (I3D_pose *pose : poses)
        pose->Release();
    poses.Free();
}

uint32_t I3D_pose_set::Release()
{
    if (--refCount < 0) {
        dbgPrintf("!! RefCount:0x%x    I3D_pose_set", this);
        return 0;
    }
    if (refCount)
        return refCount;
    delete this;
    return 0;
}

void I3D_pose_set::AddPose(I3D_pose *pose, const char *poseName)
{
    pose->SetName(poseName);
    poses.PushBack(pose);
    ++pose->refCount;
}

I3D_RESULT I3D_pose_set::RemovePose(I3D_pose *pose)
{
    if (!poses.RemoveSwap(pose))
        return I3DERR_OBJECTNOTFOUND;
    pose->Release();
    return I3D_OK;
}

const char *I3D_pose_set::GetPoseName(int index)
{
    if (index >= poses.Size() || !poses[index]->name)
        return "";
    return poses[index]->name;
}

// ---------------------------------------------------------------- I3D_anim_track

I3D_anim_track::I3D_anim_track(I3D_anim_stack *owner, I3D_frame *animated)
    : pose(nullptr), poseWeight(1.0f), stages(), frame(animated), stack(owner)
{
}

I3D_anim_track::~I3D_anim_track()
{
    if (pose)
        pose->Release();
}

void I3D_anim_track::ClearStage(int stage) { stages[stage].flags = 0; }

void I3D_anim_track::SetPose(I3D_pose *newPose)
{
    if (newPose == pose)
        return;
    stack->dirty = true;
    if (!newPose) {
        pose->Release();
        pose = nullptr;
        return;
    }
    if (pose)
        pose->Release();
    pose = newPose;
    ++newPose->refCount;
}

void I3D_anim_track::SetPoseWeight(float weight) { poseWeight = weight; }

void I3D_anim_track::Evaluate()
{
    if (!frame)
        return;
    // (The fourth dwords of the position and scale: what the blends leave, as in the original
    // whose stack holds earlier values there.)
    S_vector4 pos{}, scl{};
    S_quat rot{};
    int numPos = 0, numRot = 0, numScale = 0;

    // Positions and scales: interpolated keys, blended by the stage weights.
    const auto vectors = [&](const uint16_t *times, int n, int time, S_vector4 &out, int &count, float w) {
        const KeySpan span = FindKeys(times, n, time);
        const auto *keys = reinterpret_cast<const S_vector *>(VectorKeys(reinterpret_cast<const uint8_t *>(times) - 2));
        S_vector v;
        if (span.single) {
            v = keys[span.k0];
        } else {
            const S_vector &a = keys[span.k0], &b = keys[span.k1];
            v = S_vector((b.x - a.x) * span.f + a.x, (b.y - a.y) * span.f + a.y, (b.z - a.z) * span.f + a.z);
        }
        // (A NaN weight blends: the original tests the compare's C0 flag only.)
        if (count == 0 || w >= 1.0f) {
            out.x = v.x;
            out.y = v.y;
            out.z = v.z;
        } else {
            const float inv = 1.0f - w;
            out.x = out.x * inv + w * v.x;
            out.y = w * v.y + inv * out.y;
            out.z = inv * out.z + w * v.z;
        }
        ++count;
    };
    for (int s = 0; s < 8; ++s) {
        const I3D_anim_keys &k = stages[s];
        const I3D_anim_stack::Stage &st = stack->stages[s];
        if ((k.flags & ANIMTRACK_POSITION) && Counts(st.weight) && st.active)
            vectors(k.positions, k.numPositions, st.time, pos, numPos, st.weight);
    }
    // Rotations: slerped keys, slerped by the stage weights (a full weight starts over).
    for (int s = 0; s < 8; ++s) {
        const I3D_anim_keys &k = stages[s];
        const I3D_anim_stack::Stage &st = stack->stages[s];
        if (!st.active || !(k.flags & ANIMTRACK_ROTATION) || !Counts(st.weight))
            continue;
        const KeySpan span = FindKeys(k.rotations, k.numRotations, st.time);
        const auto *keys = reinterpret_cast<const S_quat *>(RotationKeys(reinterpret_cast<const uint8_t *>(k.rotations) - 2));
        const S_quat q = span.single ? keys[span.k0] : keys[span.k0].Slerp(keys[span.k1], span.f, false);
        if (numRot != 0 && !(st.weight >= 1.0f)) {
            rot = rot.Slerp(q, st.weight, false);
            ++numRot;
        } else {
            rot = q;
            numRot = 1;
        }
    }
    for (int s = 0; s < 8; ++s) {
        const I3D_anim_keys &k = stages[s];
        const I3D_anim_stack::Stage &st = stack->stages[s];
        if ((k.flags & ANIMTRACK_SCALE) && Counts(st.weight) && st.active)
            vectors(k.scales, k.numScales, st.time, scl, numScale, st.weight);
    }

    // The pose on top.
    if (pose && poseWeight > 0.0f) {
        const uint32_t flags = pose->poseFlags;
        const float pw = poseWeight;
        if (pw < 1.0f) {
            const auto blend = [&](S_vector4 &out, const S_vector4 &to, int &count) {
                if (count == 0) {
                    out = to;
                } else {
                    const float inv = 1.0f - pw;
                    out.x = pw * to.x + out.x * inv;
                    out.y = to.y * pw + inv * out.y;
                    out.z = to.z * pw + inv * out.z;
                }
                ++count;
            };
            if (flags & POSEF_POSITION)
                blend(pos, pose->position, numPos);
            if (flags & POSEF_SCALE)
                blend(scl, pose->scale, numScale);
            if (flags & POSEF_ROTATION_ADD) {
                if (numRot == 0) {
                    rot = pose->rotation;
                    numRot = 1;
                } else {
                    // The pose's rotation with its scalar part weighted, then the animation's.
                    S_quat q = pose->rotation;
                    q.s = q.s * pw;
                    rot = q * rot;
                    ++numRot;
                }
            } else if (flags & POSEF_ROTATION_SET) {
                rot = numRot == 0 ? pose->rotation : rot.Slerp(pose->rotation, pw, false);
                ++numRot;
            }
        } else {
            // (A full weight sets the position and scale without counting them: they apply only
            // when a stage animates them too.)
            if (flags & POSEF_POSITION)
                pos = pose->position;
            if (flags & POSEF_SCALE)
                scl = pose->scale;
            if (flags & POSEF_ROTATION_ADD) {
                if (numRot == 0) {
                    rot = pose->rotation;
                    numRot = 1;
                } else {
                    rot = pose->rotation * rot;
                    ++numRot;
                }
            } else if (flags & POSEF_ROTATION_SET) {
                rot = pose->rotation;
                numRot = 1;
            }
        }
    }

    // Into the frame.
    if (numPos && !(frame->frameFlags & FRMFLAGS_POS_LOCKED)) {
        frame->localMatrix(3) = S_vector(pos.x, pos.y, pos.z);
        frame->frameFlags = (frame->frameFlags & ~0x120u) | FRMFLAGS_CHANGED;
    }
    if (numRot) {
        if (numRot > 1)
            rot.Normalize();
        frame->rot = rot;
        frame->rot.Normalize();
        frame->frameFlags = (frame->frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
    }
    if (numScale) {
        frame->scale = S_vector(scl.x, scl.y, scl.z);
        frame->scaleW = scl.w;
        if (!(frame->frameFlags & FRMFLAGS_ROT_QUAT_VALID))
            frame->rot.Make(frame->localMatrix);
        frame->frameFlags = (frame->frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
    }

    // The note keys passed since the last evaluation (forwards (prev, cur], backwards [cur, prev),
    // either wrapping around the end).
    if (!(frame->frameFlags & (I3DCALLBACK_NOTE16 | I3DCALLBACK_NOTE32)) || !frame->callback)
        return;
    for (int s = 0; s < 8; ++s) {
        const I3D_anim_keys &k = stages[s];
        const I3D_anim_stack::Stage &st = stack->stages[s];
        if (!(k.flags & (ANIMTRACK_NOTES16 | ANIMTRACK_NOTES32)) || !st.active)
            continue;
        const uint16_t cur = KeyFrame(st.time), prev = KeyFrame(st.prevTime);
        if (cur == prev)
            continue;
        const bool wide = (k.flags & ANIMTRACK_NOTES32) != 0;
        const uint32_t message = wide ? I3DCALLBACK_NOTE32 : I3DCALLBACK_NOTE16;
        if (!(frame->frameFlags & message))
            continue;
        const int n = k.numNotes;
        const uint8_t *values = wide ? VectorKeys(reinterpret_cast<const uint8_t *>(k.notes) - 2)
                                     : reinterpret_cast<const uint8_t *>(k.notes + n);
        const bool backwards = st.lastDelta < 0;
        for (int i = 0; i < n; ++i) {
            const uint16_t t = k.notes[i];
            bool passed;
            if (!backwards)
                passed = cur < prev ? (t > prev || t <= cur) : (t > prev && t <= cur);
            else
                passed = cur > prev ? (t >= cur || t < prev) : (t >= cur && t < prev);
            if (passed) {
                const uint32_t value = wide ? U32(values + 4 * i) : U16(values + 2 * i);
                frame->callback(frame, message, value, static_cast<uint32_t>(t) * kAnimFrameTime);
            }
        }
    }
}

// ---------------------------------------------------------------- I3D_anim_stack

// (The original leaves `dirty` unset: a new stack's first Tick may evaluate nothing for nothing.)
I3D_anim_stack::I3D_anim_stack(I3D_frame *ownerFrame) : owner(ownerFrame), dirty(false), stages(), tracks(0x20) {}

I3D_anim_stack::~I3D_anim_stack()
{
    Clear();
    tracks.Free();
}

void I3D_anim_stack::Clear()
{
    for (I3D_anim_track *track : tracks)
        delete track;
    tracks.Clear();
    for (Stage &stage : stages) {
        if (stage.set)
            stage.set->Release();
    }
    std::memset(stages, 0, sizeof stages);
}

I3D_anim_track *I3D_anim_stack::GetTrack(I3D_frame *frame)
{
    for (I3D_anim_track *track : tracks) {
        if (track->frame == frame)
            return track;
    }
    auto *track = new (std::nothrow) I3D_anim_track(this, frame);
    if (!track) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return nullptr;
    }
    tracks.PushBack(track);
    frame->frameFlags |= FRMFLAGS_ANIMATED;
    return track;
}

I3D_anim_track *I3D_anim_stack::GetTrack(const char *frameName)
{
    for (I3D_anim_track *track : tracks) {
        if (!std::strcmp(ShortName(track->frame), frameName))
            return track;
    }
    I3D_frame *const frame = owner->FindChildFrame(frameName, 0x2ffff);
    if (!frame)
        return nullptr;
    auto *track = new (std::nothrow) I3D_anim_track(this, frame);
    if (!track) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return nullptr;
    }
    tracks.PushBack(track);
    frame->frameFlags |= FRMFLAGS_ANIMATED;
    return track;
}

void I3D_anim_stack::PurgeTracks()
{
    for (int i = tracks.Size(); i > 0;) {
        I3D_anim_track *const track = tracks[--i];
        if (track->pose)
            continue;
        bool used = false;
        for (int s = 0; s < 8 && !used; ++s)
            used = stages[s].active || track->stages[s].flags;
        if (used)
            continue;
        delete track;
        tracks.RemoveAtSwap(i);
    }
}

I3D_RESULT I3D_anim_stack::SetAnimation(I3D_animation_set *set, int stage, float weight, int mode)
{
    Stage &st = stages[stage];
    if (st.set == set) {
        if (!set)
            return I3D_OK;
    } else {
        if (st.set) {
            for (I3D_anim_track *track : tracks)
                track->stages[stage].flags = 0;
            st.set->Release();
        }
        st.set = set;
        if (!set) {
            st.active = false;
            PurgeTracks();
            return I3D_OK;
        }
        ++set->refCount;
    }
    st.active = true;
    st.time = 0;
    st.prevTime = -kAnimFrameTime;
    if (mode == 0)
        mode = set->defaultMode;
    st.weight = weight;
    st.mode = mode;
    // The owner's first frame of each track's name gets the track's keys.
    const I3D_animation_set::TrackEntry *entry = set->Tracks();
    for (int n = set->NumTracks(); n; --n, ++entry) {
        for (I3D_frame *frame : OwnedFrames(owner)) {
            if (std::strcmp(ShortName(frame), entry->name))
                continue;
            I3D_anim_track *const track = GetTrack(frame);
            if (!track)
                return I3DERR_OUTOFMEM;
            I3D_anim_keys &keys = track->stages[stage];
            const uint8_t *p = entry->track;
            const uint16_t flags = U16(p);
            keys.flags = flags;
            p += 4;
            if (flags & ANIMTRACK_ROTATION) {
                keys.numRotations = U16(p);
                keys.rotations = reinterpret_cast<const uint16_t *>(p + 2);
                p = SkipRotations(p);
            } else {
                keys.numRotations = 0;
            }
            if (flags & ANIMTRACK_POSITION) {
                keys.positions = reinterpret_cast<const uint16_t *>(p + 2);
                keys.numPositions = U16(p);
                p = SkipVectors(p);
            } else {
                keys.numPositions = 0;
            }
            if (flags & ANIMTRACK_SCALE) {
                keys.scales = reinterpret_cast<const uint16_t *>(p + 2);
                keys.numScales = U16(p);
                p = SkipVectors(p);
            } else {
                keys.numScales = 0;
            }
            if (flags & (ANIMTRACK_NOTES16 | ANIMTRACK_NOTES32)) {
                keys.numNotes = U16(p);
                keys.notes = reinterpret_cast<const uint16_t *>(p + 2);
            } else {
                keys.numNotes = 0;
            }
            break;
        }
    }
    PurgeTracks();
    dirty = true;
    return I3D_OK;
}

void I3D_anim_stack::EndStages()
{
    if (!dirty)
        return;
    dirty = false;
    for (I3D_anim_track *track : tracks)
        track->Evaluate();
    for (int s = 0; s < 8; ++s) {
        Stage &st = stages[s];
        if (!st.active)
            continue;
        st.prevTime = st.time;
        // (A stage started without a set (I3D_model::StartAnimation) made the original read the
        // set through null: such a stage never ends.)
        if (!st.set)
            continue;
        if (!(st.time < 0 || st.set->GetLength() <= st.time))
            continue;
        // The end: the first animated frame that wants to know is told.
        for (I3D_anim_track *track : tracks) {
            I3D_frame *const f = track->frame;
            if (f && (f->frameFlags & I3DCALLBACK_ANIM_END)) {
                f->callback(f, I3DCALLBACK_ANIM_END, s, 0);
                break;
            }
        }
        if (st.mode == ANIMMODE_LOOP)
            st.time = st.time < 0 ? st.time + st.set->GetLength() : st.time - st.set->GetLength();
        else
            st.active = false;
    }
}

void I3D_anim_stack::Tick(int delta)
{
    if (delta) {
        dirty = true;
        for (Stage &st : stages) {
            if (st.active) {
                st.time += delta;
                st.lastDelta = delta;
            }
        }
    }
    EndStages();
}

void I3D_anim_stack::Tick(const int (&deltas)[8])
{
    for (int s = 0; s < 8; ++s) {
        if (!deltas[s])
            continue;
        dirty = true;
        if (stages[s].active) {
            stages[s].time += deltas[s];
            stages[s].lastDelta = deltas[s];
        }
    }
    EndStages();
}

I3D_RESULT I3D_anim_stack::SetPose(I3D_frame *frame, I3D_pose *pose)
{
    I3D_anim_track *const track = GetTrack(frame);
    if (!track)
        return I3DERR_OBJECTNOTFOUND;
    track->SetPose(pose);
    dirty = true;
    return I3D_OK;
}

I3D_RESULT I3D_anim_stack::SetPose(const char *frameName, I3D_pose *pose)
{
    I3D_anim_track *const track = GetTrack(frameName);
    if (!track)
        return I3DERR_OBJECTNOTFOUND;
    track->SetPose(pose);
    dirty = true;
    return I3D_OK;
}

I3D_RESULT I3D_anim_stack::SetPoseSet(I3D_pose_set *set)
{
    for (I3D_pose *pose : set->poses) {
        I3D_anim_track *const track = GetTrack(pose->name ? pose->name : "");
        if (!track)
            return I3DERR_OBJECTNOTFOUND;
        track->SetPose(pose);
    }
    dirty = true;
    return I3D_OK;
}

void I3D_anim_stack::SetPoseWeight(float weight)
{
    for (I3D_anim_track *track : tracks)
        track->SetPoseWeight(weight);
    dirty = true;
}

I3D_RESULT I3D_anim_stack::SetPoseWeight(float weight, I3D_pose_set *set)
{
    dirty = true;
    for (I3D_pose *pose : set->poses) {
        I3D_anim_track *const track = GetTrack(pose->name ? pose->name : "");
        if (!track)
            return I3DERR_OBJECTNOTFOUND;
        track->SetPoseWeight(weight);
    }
    return I3D_OK;
}

float I3D_anim_stack::GetPoseWeight()
{
    for (I3D_anim_track *track : tracks) {
        if (track->pose)
            return track->poseWeight;
    }
    return 0.0f;
}

void I3D_anim_stack::ClearPoses()
{
    for (I3D_anim_track *track : tracks) {
        if (track->pose)
            track->SetPose(nullptr);
    }
    dirty = true;
}
