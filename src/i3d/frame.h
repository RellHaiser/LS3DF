// I3D_frame: a node of the scene graph (vtable 0x1009bfc8, 0x120 bytes; methods 0x1001b8f0 -
// 0x1001ef00). Every frame type derives from it; FRAME_USER frames are I3D_user (user.h).
//
// Frames are made by I3D_driver::CreateFrame / CreateVisual from per-type free lists: the C++
// object lives on, Init (slot 0x60) gives it its starting state and Uninit (slot 0x64) undoes it
// when the last reference goes (Release), after which it waits on the free list for reuse.
//
// The local transform is kept as rot (quaternion) + scale + the position row of localMatrix; the
// flags say which representation is current (FRMFLAGS_ROT_QUAT_VALID: rot,
// FRMFLAGS_LMATRIX_VALID: the matrix rotation, FRMFLAGS_ROT_VALID: the matrix rotation includes
// the scale). worldMatrix follows on demand (FRMFLAGS_WMATRIX_VALID, UpdateWMatrixProc).
//
// The game's SDK header (i3d.h) names slots 0x08 / 0x0C / 0x1C SetPos / SetDir /
// SetWorldDir; the code says world position, world direction and local direction.
#pragma once

#include "i3d/i3d.h"

class I3D_frame {
public:
    I3D_frame() = default;
    I3D_frame(const I3D_frame &) = delete;
    I3D_frame &operator=(const I3D_frame &) = delete;

    // 0x00 0x1001c370: the last reference uninitializes the frame and puts it on its free list.
    virtual uint32_t __stdcall Release();
    // 0x04 0x1001c350: 'LMAP' for a frame of visual index 1, else 0 (visuals return their fourCC).
    virtual uint32_t __stdcall GetVisualType();
    // 0x08 0x1001c9f0: position in world space (converted into the parent's space).
    virtual void __stdcall SetWorldPos(const S_vector &pos);
    // 0x0C 0x1001c8d0: direction (z axis) and roll in world space.
    virtual void __stdcall SetWorldDir(const S_vector &dir, float roll);
    // 0x10 0x1001cca0: position, direction and up vector in world space.
    virtual void __stdcall SetWorldPosDir3(const S_vector &pos, const S_vector &dir, const S_vector &up);
    // 0x14 0x1001cb10: position, direction and roll in world space.
    virtual void __stdcall SetWorldPosDir(const S_vector &pos, const S_vector &dir, float roll);
    // 0x18 0x1001ce40: position and rotation in world space.
    virtual void __stdcall SetWorldPosRot(const S_vector &pos, const S_quat &rot);
    // 0x1C 0x1001c400: local direction and roll (the rotation's quaternion).
    virtual void __stdcall SetDir(const S_vector &dir, float roll);
    // 0x20 0x1001b8d0: advances the frame's own animation (none).
    virtual void __stdcall Tick(int time) {}
    // 0x24 0x1001e610: takes the changes of the frame and its descendants into account.
    virtual void __stdcall Update();
    // 0x28 0x1001cfa0: callback for the messages in `messages` (FRMFLAGS_CALLBACK_MASK bits).
    virtual void __stdcall SetCallback(I3D_CALLBACK *callback, uint32_t messages);
    // 0x2C 0x1001e290: the user property text (shared, reference counted).
    virtual I3D_RESULT __stdcall SetUserProps(const char *props);
    // 0x30 0x1001cfe0
    virtual void __stdcall SetOn(bool on);
    // 0x34 0x1001d330
    virtual I3D_RESULT __stdcall SetName(const char *name);
    // 0x38 0x1001d680: new parent (flags I3DLINK_*).
    virtual I3D_RESULT __stdcall LinkTo(I3D_frame *parent, uint32_t flags);
    // 0x3C / 0x40 0x1001db80 / 0x1001dba0
    virtual int __stdcall NumChildren();
    virtual I3D_frame *__stdcall GetChild(int index);
    // 0x44 0x1001de40: the descendants matching the type bits and the name mask.
    virtual I3D_RESULT __stdcall EnumFrames(I3D_ENUMPROC *proc, void *context, uint32_t flags, const char *mask);
    // 0x48 0x1001ded0: the first descendant matching (EnumFrames' rules).
    virtual I3D_frame *__stdcall FindChildFrame(const char *name, uint32_t flags);
    // 0x4C 0x1001e150: copies the transform, flags, callback, name and properties of another frame.
    virtual I3D_RESULT __stdcall Duplicate(I3D_frame *source);
    // 0x50 / 0x54 0x1001eba0 / 0x1001ebf0: light level / opacity of the object visuals of the
    // hierarchy (those with VISF_OBJECT_PARAMS).
    virtual void __stdcall SetHierarchyLightLevel(float level);
    virtual void __stdcall SetHierarchyAlpha(float alpha);
    // 0x58 0x1001e7d0: bound of the frame and its children (world space).
    virtual void __stdcall UpdateBound();
    // 0x5C: scalar deleting destructor (only when the driver frees its free lists).
    virtual ~I3D_frame() = default;
    // Internal slots 0x60 - 0x70 are __thiscall.
    // 0x60 0x1001c0e0: the state of a new frame.
    virtual void Init();
    // 0x64 0x1001c1f0: unlinks the frame from everything (the last Release).
    virtual void Uninit();
    // 0x68 / 0x6C 0x100881e0: told after the matrix changed (Update) / (not called by the base).
    virtual void MatrixChanged() {}
    virtual void Slot6C() {}
    // 0x70 0x1001ec40: reads the frame from the model file being loaded (failures go to
    // ls3d::i3d::g_loadResult).
    virtual void Load();

    // The world matrix, brought up to date.
    const S_matrix &GetMatrix()
    {
        if (!(frameFlags & FRMFLAGS_WMATRIX_VALID))
            UpdateWMatrixProc();
        return worldMatrix;
    }
    void UpdateWMatrix() { UpdateWMatrixProc(); }
    // 0x1001b8f0: the local matrix' rotation rows brought up to date (scaled).
    void UpdateLMatrix();
    // The bound, brought up to date.
    const I3D_bound_volume &GetBound()
    {
        if (!(frameFlags & FRMFLAGS_BOUND_VALID))
            UpdateBound();
        return bound;
    }
    // Position setter of the base (keeps FRMFLAGS_POS_LOCKED frames in place).
    void SetLocalPos(const S_vector &pos)
    {
        if (frameFlags & FRMFLAGS_POS_LOCKED)
            return;
        localMatrix(3) = pos;
        frameFlags = (frameFlags & ~0x120u) | FRMFLAGS_CHANGED;
    }
    E_I3D_FRAME_TYPE GetType() const { return static_cast<E_I3D_FRAME_TYPE>(type); }
    const char *GetName() const { return name ? name : ""; }
    void AddRef() { ++refCount; }
    // Clears FRMFLAGS_BOUND_VALID of the ancestors up to the first sector (0x1001e790; nothing for
    // a sector).
    void ClearParentBounds();
    void AddChildFront(I3D_frame *child);                       // 0x1001db50
    I3D_RESULT SetNamePrefix(const char *prefix);               // 0x1001d470 "prefix.name"
    I3D_RESULT RemoveNamePrefix();                              // 0x1001d5d0
    void NotifyOwnerRemoved();                                  // 0x1001c3d0
    // The recursive EnumFrames / FindChildFrame step (0x1001dbd0 / 0x1001df00).
    I3D_RESULT EnumFramesProc(I3D_ENUMPROC *proc, void *context, uint32_t flags, const char *mask);
    I3D_frame *FindChild(const char *name, uint32_t flags);

    // The driver's name tree (a binary tree of named frames; equal names chain to the right).
    void InsertName();                                          // 0x1001d100
    void RemoveName();                                          // 0x1001d040

protected:
    // The inverse of the parent's world matrix (the parent's cached one if it is a sector).
    const S_matrix &ParentInverse(S_matrix &temp) const;
    // Sets the local rotation from a world-space matrix `m` (rows 1 and 2) and the position from
    // its row 3, keeping FRMFLAGS_POS_LOCKED frames in place.
    void SetFromParentSpace(const S_matrix &m, bool position);
    // The recursive Update step (0x1001e550).
    void UpdateChildren();

private:
    // 0x1001e390 (exported "?UpdateWMatrixProc@I3D_frame@@AAEXXZ"): the world matrix from the
    // parent's and the local transform.
    void UpdateWMatrixProc();

public:
    uint32_t padding04[3];                  // +0x04 (the bound is 16-byte aligned; never written)
    I3D_bound_volume bound;                 // +0x10 FRMFLAGS_BOUND_VALID
    S_matrix worldMatrix;                   // +0x40
    S_matrix localMatrix;                   // +0x80 row 3: the position
    S_quat rot;                             // +0xC0
    S_vector scale;                         // +0xD0
    float scaleW;                           // +0xDC (read from model files with the scale)
    uint32_t frameFlags;                    // +0xE0
    int16_t refCount;                       // +0xE4
    int16_t nameOffset;                     // +0xE6 the name without the model prefix starts here
    char *name;                             // +0xE8
    void *data;                             // +0xEC the application's
    char *userProps;                        // +0xF0 (an int16 reference count precedes the text)
    I3D_CALLBACK *callback;                 // +0xF4
    uint16_t type;                          // +0xF8 E_I3D_FRAME_TYPE
    uint16_t visualIndex;                   // +0xFA visuals: index in the driver's visual factory
    float worldScale;                       // +0xFC uniform world scale (average of the axes)
    I3D_frame *ownerModel;                  // +0x100 model or scene that loaded the frame
    int changeCount;                        // +0x104 counts the frame's changes (Update, Duplicate)
    I3D_frame *parent;                      // +0x108
    I3D_frame *firstChild;                  // +0x10C
    I3D_frame *nextSibling;                 // +0x110 also the free-list link
    I3D_frame *nameLeft;                    // +0x114 name tree
    I3D_frame *nameRight;                   // +0x118
    int renderStamp;                        // +0x11C

    // Frames live in 16-byte aligned blocks (AlignedAlloc).
    static void *operator new(size_t size);
    static void operator delete(void *p);
    static void *operator new(size_t, void *where) { return where; }
    static void operator delete(void *, void *) {}
};
static_assert(sizeof(I3D_frame) == 0x120, "I3D_frame layout");
static_assert(offsetof(I3D_frame, bound) == 0x10, "I3D_frame layout");
static_assert(offsetof(I3D_frame, worldMatrix) == 0x40, "I3D_frame layout");
static_assert(offsetof(I3D_frame, rot) == 0xC0, "I3D_frame layout");
static_assert(offsetof(I3D_frame, frameFlags) == 0xE0, "I3D_frame layout");
static_assert(offsetof(I3D_frame, type) == 0xF8, "I3D_frame layout");
static_assert(offsetof(I3D_frame, parent) == 0x108, "I3D_frame layout");
static_assert(offsetof(I3D_frame, renderStamp) == 0x11C, "I3D_frame layout");

// LinkTo flags.
enum : uint32_t {
    I3DLINK_UPDMATRIX = 1,                  // keep the world transform
    I3DLINK_NOBOUNDS = 2,                   // also clear the bounds of both old and new ancestors
    I3DLINK_UPDATE = 4,                     // Update after linking
};

namespace ls3d::i3d {

// The name tree root (0x100be010).
extern I3D_frame *g_nameRoot;
// The frame whose Uninit ran last (0x100be440): EnumFrames does not descend into it.
extern I3D_frame *g_releasedFrame;
// The name tree node of `frame` (named `name`); g_nameParent its parent (0x1001a1f0).
I3D_frame *FindNameNode(const char *name, const I3D_frame *frame);
extern I3D_frame *g_nameParent;


}  // namespace ls3d::i3d
