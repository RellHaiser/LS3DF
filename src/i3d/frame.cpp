// I3D_frame (0x1001b8f0 - 0x1001ef00).
#include "i3d/frame.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "core/c_vector.h"
#include "core/debug.h"
#include "external/rw_data.h"
#include "i3d/area.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/loader.h"
#include "i3d/occluder.h"
#include "i3d/sector.h"
#include "i3d/visual.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace ls3d::i3d {

I3D_frame *g_releasedFrame;                         // 0x100be440
// Targets moved by Update; processed when the outermost Update ends (0x100be464, 0x100be478).
C_vector<I3D_frame *> g_targetUpdates;
bool g_targetUpdateBusy;

}  // namespace ls3d::i3d

namespace {

constexpr float kHalfPi = 1.57079637f;              // 0x3fc90fdb
constexpr float kPi = 3.14159274f;                  // 0x40490fdb

// Insertions into the name tree; every 0x800th (from the 0x400th) rebalances it (0x100be474).
int g_nameInsertions;

// The flag bits Duplicate takes from the source / keeps of its own.
constexpr uint32_t kDuplicatedFlags = 0x0f673edf;
constexpr uint32_t kOwnFlags = FRMFLAGS_SCENE_SOUND | FRMFLAGS_SHADOW_CASTER | FRMFLAGS_TARGETED;

// The type bit of EnumFrames' and FindChildFrame's masks (0 for types they never match:
// occluders, scenes; FindChildFrame does not know joints either).
uint32_t EnumTypeBit(int type)
{
    switch (type) {
    case FRAME_VISUAL: return 0x1;
    case FRAME_LIGHT: return 0x2;
    case FRAME_CAMERA: return 0x4;
    case FRAME_SOUND: return 0x8;
    case FRAME_SECTOR: return 0x10;
    case FRAME_DUMMY: return 0x20;
    case FRAME_TARGET: return 0x40;
    case FRAME_USER: return 0x100;
    case FRAME_MODEL: return 0x80;
    case FRAME_JOINT: return 0x400;
    case FRAME_VOLUME: return 0x200;
    case FRAME_AREA: return 0x1000;
    case FRAME_SHADOW: return 0x2000;
    default: return 0;
    }
}
uint32_t FindTypeBit(int type) { return type == FRAME_JOINT ? 0 : EnumTypeBit(type); }

bool TypeMatches(uint32_t flags, uint32_t typeBit)
{
    return (flags & ENUMF_ALL_TYPES) == ENUMF_ALL_TYPES || (flags & typeBit);
}

int AsciiUpper(char c)
{
    const int i = c;
    return i >= 'a' && i <= 'z' ? i - ('a' - 'A') : i;
}

// The name masks of ENUMF_WILDMASK: '?' matches any character, '*' the rest; the mask and the
// name are compared up to the end of the shorter one, its terminator included.
bool MatchWildcard(const char *mask, const char *name, bool caseSensitive)
{
    size_t n = std::strlen(mask);
    const size_t nameLength = std::strlen(name);
    if (nameLength < n)
        n = nameLength;
    for (size_t i = 0; i <= n; ++i) {
        const char c = mask[i];
        if (c == '*')
            return true;
        if (c == '?')
            continue;
        if (caseSensitive ? c != name[i] : AsciiUpper(c) != AsciiUpper(name[i]))
            return false;
    }
    return true;
}

bool MatchName(const char *mask, const char *name, uint32_t flags)
{
    const bool caseSensitive = (flags & ENUMF_CASESENSITIVE) != 0;
    if (flags & ENUMF_WILDMASK)
        return MatchWildcard(mask, name, caseSensitive);
    return (caseSensitive ? std::strcmp(mask, name) : _stricmp(mask, name)) == 0;
}

// Removes `child` from the child list of `parent`; false if it is not in it.
bool RemoveChild(I3D_frame *parent, I3D_frame *child)
{
    I3D_frame **link = &parent->firstChild;
    while (*link != child) {
        if (!*link)
            return false;
        link = &(*link)->nextSibling;
    }
    *link = child->nextSibling;
    return true;
}

// The user property blocks: an int16 reference count, then the text.
int16_t &PropsRefCount(char *props) { return reinterpret_cast<int16_t *>(props)[-1]; }
char *NewProps(size_t textSize)
{
    auto *block = static_cast<int16_t *>(::operator new(textSize + sizeof(int16_t), std::nothrow));
    if (!block)
        return nullptr;
    *block = 1;
    return reinterpret_cast<char *>(block + 1);
}

// The LinkTo enumeration callbacks (0x1001daf0, 0x1001db10, 0x1001db30).
uint32_t __stdcall SectorSceneProc(I3D_frame *frame, void *scene)
{
    static_cast<I3D_sector *>(frame)->SetScene(static_cast<I3D_scene *>(scene));
    return 0;
}
uint32_t __stdcall OccluderSceneProc(I3D_frame *frame, void *scene)
{
    static_cast<I3D_occluder *>(frame)->SetScene(static_cast<I3D_scene *>(scene));
    return 0;
}
uint32_t __stdcall AreaSceneProc(I3D_frame *frame, void *scene)
{
    static_cast<I3D_area *>(frame)->SetScene(static_cast<I3D_scene *>(scene));
    return 0;
}

// FindChildFrame's callback (0x1001deb0): keeps the first frame and stops.
uint32_t __stdcall FindFirstProc(I3D_frame *frame, void *result)
{
    *static_cast<I3D_frame **>(result) = frame;
    return 2;
}

// The scene of the sector `frame` is in (the first sector up from it), null if none.
I3D_scene *SceneOf(I3D_frame *frame)
{
    for (; frame; frame = frame->parent) {
        if (frame->type == FRAME_SECTOR)
            return static_cast<I3D_sector *>(frame)->scene;
    }
    return nullptr;
}

}  // namespace

void *I3D_frame::operator new(size_t size) { return AlignedAlloc(size, 16); }
void I3D_frame::operator delete(void *p) { AlignedFree(p); }

// --- Matrices ----------------------------------------------------------------------------------

void I3D_frame::UpdateLMatrix()
{
    if (!(frameFlags & FRMFLAGS_LMATRIX_VALID)) {
        frameFlags |= FRMFLAGS_LMATRIX_VALID | FRMFLAGS_ROT_VALID;
        localMatrix.SetRot3(rot);
    } else {
        if (frameFlags & FRMFLAGS_ROT_VALID)
            return;
        if (!(frameFlags & FRMFLAGS_ROT_QUAT_VALID))
            rot.Make(localMatrix);
        frameFlags |= FRMFLAGS_ROT_QUAT_VALID | FRMFLAGS_ROT_VALID;
    }
    for (int c = 0; c < 3; ++c) {
        localMatrix.m[0][c] *= scale.x;
        localMatrix.m[1][c] *= scale.y;
        localMatrix.m[2][c] *= scale.z;
    }
}

void I3D_frame::UpdateWMatrixProc()
{
    UpdateLMatrix();
    float scaleSum;
    if (!parent) {
        worldMatrix = localMatrix;
        scaleSum = (scale.z + scale.y) + scale.x;
    } else {
        worldMatrix = localMatrix * parent->GetMatrix();
        scaleSum = ((scale.z + scale.y) + scale.x) * parent->worldScale;
    }
    worldScale = scaleSum * 0.333333343f;
    frameFlags |= FRMFLAGS_WMATRIX_VALID;
}

const S_matrix &I3D_frame::ParentInverse(S_matrix &temp) const
{
    if (parent->type == FRAME_SECTOR)
        return static_cast<I3D_sector *>(parent)->GetInvMatrix();
    temp.Inverse(parent->GetMatrix());
    return temp;
}

void I3D_frame::SetFromParentSpace(const S_matrix &m, bool position)
{
    localMatrix.SetDir3(m(2), m(1));
    frameFlags = (frameFlags & ~0x12Cu) | FRMFLAGS_CHANGED | FRMFLAGS_LMATRIX_VALID;
    if (position)
        SetLocalPos(m(3));
}

// --- Life cycle --------------------------------------------------------------------------------

void I3D_frame::Init()
{
    parent = nullptr;
    firstChild = nullptr;
    nextSibling = nullptr;
    worldMatrix.Identity();
    localMatrix.Identity();
    changeCount = 0;
    scale = S_vector(1.0f, 1.0f, 1.0f);
    rot.Identity();
    worldScale = 1.0f;
    ownerModel = nullptr;
    callback = nullptr;
    data = nullptr;
    name = nullptr;
    nameOffset = 0;
    userProps = nullptr;
    bound.bbox.Invalidate();
    bound.center.Zero();
    bound.radius = 0.0f;
    nameLeft = nullptr;
    nameRight = nullptr;
    frameFlags = FRMFLAGS_CHANGED | FRMFLAGS_WMATRIX_VALID | FRMFLAGS_LMATRIX_VALID |
                 FRMFLAGS_ROT_QUAT_VALID | FRMFLAGS_ROT_VALID | FRMFLAGS_ON;
    renderStamp = -1;
}

void I3D_frame::NotifyOwnerRemoved()
{
    if (!ownerModel)
        return;
    if (ownerModel->type == FRAME_MODEL)
        ModelFrameReleased(ownerModel, this);
    else if (ownerModel->type == FRAME_SCENE)
        SceneFrameReleased(ownerModel, this);
}

void I3D_frame::Uninit()
{
    if (frameFlags & FRMFLAGS_NAMED)
        RemoveName();
    NotifyOwnerRemoved();
    if (frameFlags & FRMFLAGS_TARGETED)
        RemoveFromTargets(this);
    if (frameFlags & FRMFLAGS_SHADOW_CASTER)
        RemoveShadowCaster(this);
    if (frameFlags & FRMFLAGS_SCENE_SOUND)
        g_driver->FrameReleased(this);
    if (parent) {
        parent->frameFlags &= ~FRMFLAGS_BOUND_VALID;
        if (RemoveChild(parent, this)) {
            nextSibling = nullptr;
            parent = nullptr;
        }
    }
    while (firstChild) {
        if (firstChild->type == FRAME_SHADOW)
            ShadowSetSource(firstChild, nullptr);
        I3D_frame *child = firstChild;
        if (child->parent == this) {
            frameFlags &= ~FRMFLAGS_BOUND_VALID;
            firstChild = child->nextSibling;
            child->nextSibling = nullptr;
            child->parent = nullptr;
        }
    }
    SetUserProps(nullptr);
    ::operator delete(name);        // (the pointer stays until Init)
    g_releasedFrame = this;
}

uint32_t __stdcall I3D_frame::Release()
{
    if (--refCount > 0)
        return refCount;
    if (refCount != 0) {
        dbgPrintf("!! m_refCount:%d  this:0x%x I3D_frame::Release()", refCount, this);
        ReportError(I3DERR_NOTINITIALIZED, kSevError, "m_refCount<0", reinterpret_cast<uintptr_t>(this));
        return 0;
    }
    FreeFrameObject(this);
    return 0;
}

uint32_t __stdcall I3D_frame::GetVisualType()
{
    return visualIndex == 1 ? VisualFourCC(1) : 0;
}

// --- Transform setters -------------------------------------------------------------------------

void __stdcall I3D_frame::SetWorldPos(const S_vector &pos)
{
    if (!parent) {
        SetLocalPos(pos);
        return;
    }
    S_matrix temp;
    const S_vector local = pos * ParentInverse(temp);
    SetLocalPos(local);
}

void __stdcall I3D_frame::SetWorldDir(const S_vector &dir, float roll)
{
    if (!parent) {
        SetDir(dir, roll);
        return;
    }
    S_matrix m{};
    m.SetDir(dir, roll);
    S_matrix temp;
    m = m * ParentInverse(temp);
    SetFromParentSpace(m, false);
}

void __stdcall I3D_frame::SetWorldPosDir3(const S_vector &pos, const S_vector &dir, const S_vector &up)
{
    if (!parent) {
        localMatrix.SetDir3(dir, up);
        frameFlags = (frameFlags & ~0x12Cu) | FRMFLAGS_CHANGED | FRMFLAGS_LMATRIX_VALID;
        SetLocalPos(pos);
        return;
    }
    S_matrix m{};
    m.SetDir3(dir, up);
    m(3) = pos;
    S_matrix temp;
    m = m * ParentInverse(temp);
    SetFromParentSpace(m, true);
}

void __stdcall I3D_frame::SetWorldPosDir(const S_vector &pos, const S_vector &dir, float roll)
{
    if (!parent) {
        SetDir(dir, roll);
        SetLocalPos(pos);
        return;
    }
    S_matrix m{};
    m.SetDir3(dir, roll);
    m(3) = pos;
    S_matrix temp;
    m = m * ParentInverse(temp);
    SetFromParentSpace(m, true);
}

void __stdcall I3D_frame::SetWorldPosRot(const S_vector &pos, const S_quat &q)
{
    if (!parent) {
        rot = q;
        rot.Normalize();
        frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
        SetLocalPos(pos);
        return;
    }
    // (Without the sector's cached inverse, unlike the other setters.)
    S_matrix inv{};
    inv.Inverse(parent->GetMatrix());
    S_matrix m{};
    m.SetRot3(q);
    m(3) = pos;
    m = m * inv;
    SetFromParentSpace(m, true);
}

// The rotation as pitch (about x), yaw (about y) and roll (about z) angles of the direction.
void __stdcall I3D_frame::SetDir(const S_vector &dir, float roll)
{
    float pitch = 0.0f;
    float yaw = 0.0f;
    const auto pitchOf = [&dir]() {
        const float t = dir.y / MagnitudeOfSquare((dir.z * dir.z + dir.y * dir.y) + dir.x * dir.x);
        if (std::fabs(t) < 1.0f)
            return Asin(t);
        return t > 0.0f ? kHalfPi : -kHalfPi;
    };
    const bool hasY = std::fabs(dir.y) > 1e-5f;
    if (std::fabs(dir.x) > 1e-5f) {
        if (hasY)
            pitch = pitchOf();
        if (std::fabs(dir.z) < 1e-5f) {
            if (dir.x > 0.0f)
                yaw = -kHalfPi;
            else if (dir.x < 0.0f)
                yaw = kHalfPi;
        } else {
            yaw = static_cast<float>(std::atan2(static_cast<double>((-1.0f / dir.z) * dir.x), 1.0));
            if (dir.z < 0.0f)
                yaw = yaw + kPi;
        }
    } else {
        if (hasY)
            pitch = pitchOf();
        if (dir.z < 0.0f)
            yaw = kPi;
    }
    S_quat qPitch, qYaw, qRoll;
    qPitch.Make(S_vector(1.0f, 0.0f, 0.0f), pitch);
    qYaw.Make(S_vector(0.0f, 1.0f, 0.0f), yaw);
    qRoll.Make(S_vector(0.0f, 0.0f, 1.0f), roll);
    rot = (qRoll * qPitch) * qYaw;
    frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
}

// --- Simple setters ----------------------------------------------------------------------------

void __stdcall I3D_frame::SetCallback(I3D_CALLBACK *cb, uint32_t messages)
{
    const uint32_t bits = messages & FRMFLAGS_CALLBACK_MASK;
    if (!cb) {
        frameFlags &= ~bits;        // (the callback itself stays)
        return;
    }
    callback = cb;
    frameFlags |= bits;
}

void I3D_frame::ClearParentBounds()
{
    if (type == FRAME_SECTOR)
        return;
    for (I3D_frame *f = parent; f && f->type != FRAME_SECTOR; f = f->parent)
        f->frameFlags &= ~FRMFLAGS_BOUND_VALID;
}

void __stdcall I3D_frame::SetOn(bool on)
{
    if (on)
        frameFlags |= FRMFLAGS_ON;
    else
        frameFlags &= ~FRMFLAGS_ON;
    ClearParentBounds();
}

I3D_RESULT __stdcall I3D_frame::SetUserProps(const char *props)
{
    if (userProps && --PropsRefCount(userProps) < 1)
        ::operator delete(&PropsRefCount(userProps));
    if (!props || !*props) {
        userProps = nullptr;
        return I3D_OK;
    }
    const size_t size = std::strlen(props) + 1;
    userProps = NewProps(size);
    if (!userProps) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    std::memcpy(userProps, props, size);
    return I3D_OK;
}

// --- Names -------------------------------------------------------------------------------------

void I3D_frame::InsertName()
{
    if (!name)
        return;
    if ((++g_nameInsertions & 0x7ff) == 0x400)
        g_driver->RebalanceNameTree();
    if (!g_nameRoot) {
        g_nameRoot = this;
        frameFlags |= FRMFLAGS_NAMED;
        return;
    }
    I3D_frame *node = g_nameRoot;
    for (;;) {
        const int c = std::strcmp(name, node->name);
        if (c < 0) {
            if (!node->nameLeft) {
                node->nameLeft = this;
                break;
            }
            node = node->nameLeft;
        } else if (c > 0) {
            if (!node->nameRight) {
                node->nameRight = this;
                break;
            }
            node = node->nameRight;
        } else {
            // Equal names chain to the right: after the last of them.
            while (node->nameRight && !std::strcmp(name, node->nameRight->name))
                node = node->nameRight;
            nameRight = node->nameRight;
            node->nameRight = this;
            break;
        }
    }
    frameFlags |= FRMFLAGS_NAMED;
}

void I3D_frame::RemoveName()
{
    frameFlags &= ~FRMFLAGS_NAMED;
    if (!name)
        return;
    if (FindNameNode(name, this) == this) {
        I3D_frame **link;
        if (this == g_nameRoot)
            link = &g_nameRoot;
        else if (g_nameParent->nameLeft == this)
            link = &g_nameParent->nameLeft;
        else
            link = &g_nameParent->nameRight;
        if (!nameRight) {
            *link = nameLeft;
        } else {
            *link = nameRight;
            if (nameLeft) {
                I3D_frame *n = nameRight;
                while (n->nameLeft)
                    n = n->nameLeft;
                n->nameLeft = nameLeft;
            }
        }
    }
    nameLeft = nullptr;
    nameRight = nullptr;
}

I3D_RESULT __stdcall I3D_frame::SetName(const char *newName)
{
    if (name == newName)
        return I3D_OK;
    if (name && newName && !std::strcmp(name, newName))
        return I3D_OK;
    if (frameFlags & FRMFLAGS_NAMED)
        RemoveName();
    if (!newName || !*newName) {
        ::operator delete(name);
        name = nullptr;
        nameOffset = 0;
        return I3D_OK;
    }
    const size_t length = std::strlen(newName);
    if (!name || std::strlen(name) < length) {
        ::operator delete(name);
        name = static_cast<char *>(::operator new(length + 1, std::nothrow));
        if (!name) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
    }
    std::strcpy(name, newName);
    nameOffset = 0;
    if (NamesEnabled())
        InsertName();
    return I3D_OK;
}

// The model prefix: "prefix.shortname" (an unnamed frame becomes "prefix.").
I3D_RESULT I3D_frame::SetNamePrefix(const char *prefix)
{
    if (frameFlags & FRMFLAGS_NAMED)
        RemoveName();
    size_t needed = std::strlen(prefix) + 2;
    bool inPlace = false;
    if (name) {
        needed += std::strlen(name) - nameOffset;
        inPlace = needed <= std::strlen(name);
    }
    if (inPlace) {
        // (The original copies at most 299 characters of the short name.)
        char shortName[300];
        std::strncpy(shortName, name + nameOffset, 299);
        shortName[299] = 0;
        std::sprintf(name, "%s.%s", prefix, shortName);
    } else {
        char *buffer = static_cast<char *>(::operator new(needed, std::nothrow));
        if (!buffer) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        if (!name)
            std::sprintf(buffer, "%s.", prefix);
        else
            std::sprintf(buffer, "%s.%s", prefix, name + nameOffset);
        ::operator delete(name);
        name = buffer;
    }
    nameOffset = static_cast<int16_t>(std::strlen(prefix) + 1);
    if (NamesEnabled())
        InsertName();
    return I3D_OK;
}

I3D_RESULT I3D_frame::RemoveNamePrefix()
{
    if (!nameOffset || !name)
        return I3D_OK;
    if (frameFlags & FRMFLAGS_NAMED)
        RemoveName();
    const int rest = static_cast<int>(std::strlen(name) + 1) - nameOffset;
    if (rest < 2) {
        ::operator delete(name);
        name = nullptr;
        nameOffset = 0;
        return I3D_OK;
    }
    std::memmove(name, name + nameOffset, rest);
    nameOffset = 0;
    if (NamesEnabled())
        InsertName();
    return I3D_OK;
}

// --- Hierarchy ---------------------------------------------------------------------------------

void I3D_frame::AddChildFront(I3D_frame *child)
{
    frameFlags &= ~FRMFLAGS_BOUND_VALID;
    child->nextSibling = firstChild;
    firstChild = child;
    child->parent = this;
}

int __stdcall I3D_frame::NumChildren()
{
    int n = 0;
    for (I3D_frame *c = firstChild; c; c = c->nextSibling)
        ++n;
    return n;
}

I3D_frame *__stdcall I3D_frame::GetChild(int index)
{
    for (I3D_frame *c = firstChild; c; c = c->nextSibling) {
        if (!index--)
            return c;
    }
    return nullptr;
}

I3D_RESULT __stdcall I3D_frame::LinkTo(I3D_frame *newParent, uint32_t flags)
{
    I3D_frame *const oldParent = parent;
    if (newParent == oldParent)
        return I3D_OK;
    if ((flags & I3DLINK_UPDMATRIX) && !(frameFlags & FRMFLAGS_WMATRIX_VALID))
        UpdateWMatrixProc();
    if (oldParent) {
        oldParent->frameFlags &= ~FRMFLAGS_BOUND_VALID;
        if (RemoveChild(oldParent, this)) {
            nextSibling = nullptr;
            parent = nullptr;
        }
        if (flags & I3DLINK_NOBOUNDS)
            oldParent->ClearParentBounds();
    }
    if (newParent) {
        if (flags & I3DLINK_NOBOUNDS)
            newParent->ClearParentBounds();
        newParent->AddChildFront(this);
    }

    if (!(flags & I3DLINK_UPDMATRIX)) {
        frameFlags = (frameFlags & ~0x120u) | FRMFLAGS_CHANGED;
    } else {
        // The old world transform in the new parent's space.
        S_matrix m;
        if (newParent) {
            S_matrix temp;
            const S_matrix &inv = ParentInverse(temp);
            m = GetMatrix() * inv;
        } else {
            m = GetMatrix();
        }
        SetLocalPos(m(3));
        scale = m.GetScale();       // (the original copies a 4th, unset value into scaleW)
        if (!(frameFlags & FRMFLAGS_ROT_QUAT_VALID))
            rot.Make(localMatrix);
        frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
        localMatrix.SetDir3(m(2), m(1));
        frameFlags = (frameFlags & ~0x12Cu) | FRMFLAGS_CHANGED | FRMFLAGS_LMATRIX_VALID;
    }
    if (flags & I3DLINK_UPDATE)
        Update();

    const uint32_t sectorBits = frameFlags & FRMFLAGS_SECTOR_MASK;
    if (!sectorBits)
        return I3D_OK;
    for (I3D_frame *f = newParent; f; f = f->parent)
        f->frameFlags |= sectorBits;
    // The old ancestors lose the bits of their kind (the original then walks each one's children
    // without doing anything).
    for (I3D_frame *f = oldParent; f; f = f->parent) {
        switch (f->type) {
        case FRAME_SECTOR: f->frameFlags &= ~0x00140000u; break;
        case FRAME_OCCLUDER: f->frameFlags &= ~0x000c0000u; break;
        case FRAME_AREA: f->frameFlags &= ~0x00180000u; break;
        default: f->frameFlags &= ~FRMFLAGS_SECTOR_MASK; break;
        }
    }
    I3D_scene *const oldScene = SceneOf(oldParent);
    I3D_scene *const newScene = SceneOf(newParent);
    if (oldScene == newScene)
        return I3D_OK;
    if (type == FRAME_SECTOR)
        static_cast<I3D_sector *>(this)->SetScene(newScene);
    EnumFrames(SectorSceneProc, newScene, EnumTypeBit(FRAME_SECTOR), nullptr);
    if (type == FRAME_OCCLUDER)
        static_cast<I3D_occluder *>(this)->SetScene(newScene);
    // (0x800 is no type bit EnumFrames knows: the original's occluder pass finds nothing.)
    EnumFrames(OccluderSceneProc, newScene, 0x800, nullptr);
    if (type == FRAME_AREA)
        static_cast<I3D_area *>(this)->SetScene(newScene);
    EnumFrames(AreaSceneProc, newScene, EnumTypeBit(FRAME_AREA), nullptr);
    return I3D_OK;
}

// --- Enumeration -------------------------------------------------------------------------------

I3D_RESULT I3D_frame::EnumFramesProc(I3D_ENUMPROC *proc, void *context, uint32_t flags, const char *mask)
{
    bool match = TypeMatches(flags, EnumTypeBit(type));
    if (match && mask)
        match = name && MatchName(mask, name + nameOffset, flags);
    if (match) {
        const uint32_t r = proc(this, context);
        if (r == 2)
            return I3DERR_CANCELED;
        if (r == 1)
            return I3D_OK;
    }
    if (g_releasedFrame == this)
        return I3D_OK;
    for (I3D_frame *c = firstChild; c;) {
        I3D_frame *next = c->nextSibling;
        if (c->EnumFramesProc(proc, context, flags, mask) == I3DERR_CANCELED)
            return I3DERR_CANCELED;
        c = next;
    }
    return I3D_OK;
}

I3D_RESULT __stdcall I3D_frame::EnumFrames(I3D_ENUMPROC *proc, void *context, uint32_t flags, const char *mask)
{
    if (!firstChild)
        return I3D_OK;
    g_releasedFrame = nullptr;
    for (I3D_frame *c = firstChild; c;) {
        I3D_frame *next = c->nextSibling;
        if (c->EnumFramesProc(proc, context, flags, mask) == I3DERR_CANCELED)
            return I3DERR_CANCELED;
        c = next;
    }
    return I3D_OK;
}

I3D_frame *__stdcall I3D_frame::FindChildFrame(const char *frameName, uint32_t flags)
{
    I3D_frame *found = nullptr;
    EnumFrames(FindFirstProc, &found, flags, frameName);
    return found;
}

// The frame itself or a descendant, matching the full name.
I3D_frame *I3D_frame::FindChild(const char *mask, uint32_t flags)
{
    if (TypeMatches(flags, FindTypeBit(type))) {
        if (!mask)
            return this;
        if (name && MatchName(mask, name, flags))
            return this;
    }
    for (I3D_frame *c = firstChild; c; c = c->nextSibling) {
        if (I3D_frame *f = c->FindChild(mask, flags))
            return f;
    }
    return nullptr;
}

// --- Copies and loading ------------------------------------------------------------------------

I3D_RESULT __stdcall I3D_frame::Duplicate(I3D_frame *src)
{
    if (!src)
        return I3DERR_INVALIDPARAMS;
    if (frameFlags & FRMFLAGS_NAMED)
        RemoveName();
    // Fix: the original frees the name before copying the source's, which reads freed memory when
    // a frame duplicates itself; the copy is made first.
    char *const oldName = name;
    const char *const srcName = src->name;
    name = nullptr;
    if (srcName) {
        name = static_cast<char *>(::operator new(std::strlen(srcName) + 1, std::nothrow));
        if (name)
            std::strcpy(name, srcName);
    }
    ::operator delete(oldName);
    localMatrix = src->localMatrix;
    rot = src->rot;
    scale = src->scale;
    scaleW = src->scaleW;
    frameFlags = (src->frameFlags & kDuplicatedFlags) | (frameFlags & kOwnFlags) | FRMFLAGS_CHANGED;
    callback = src->callback;
    ++changeCount;
    if (!name) {
        nameOffset = 0;
        if (srcName) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
    } else {
        if (NamesEnabled())
            InsertName();
        nameOffset = src->nameOffset;
    }
    SetUserProps(nullptr);
    if (src->userProps) {
        userProps = src->userProps;
        ++PropsRefCount(userProps);
    }
    return I3D_OK;
}

void I3D_frame::Load()
{
    dtaRead(g_loadFile, &localMatrix.m[3][0], sizeof(S_vector));
    // The rotation, the scale (with scaleW) and the low byte of the flags.
    dtaRead(g_loadFile, reinterpret_cast<uint8_t *>(this) + offsetof(I3D_frame, rot), 0x21);
    uint32_t length = 0;
    dtaRead(g_loadFile, &length, 1);
    if (length) {
        name = static_cast<char *>(::operator new(length + 1, std::nothrow));
        if (!name) {
            g_loadResult = I3DERR_OUTOFMEM;
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return;
        }
        nameOffset = 0;
        dtaRead(g_loadFile, name, length);
        name[length] = 0;
        if (NamesEnabled())
            InsertName();
    }
    dtaRead(g_loadFile, &length, 1);
    if (length) {
        userProps = NewProps(length + 1);
        if (!userProps) {
            g_loadResult = I3DERR_OUTOFMEM;
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return;
        }
        dtaRead(g_loadFile, userProps, length);
        userProps[length] = 0;
    }
    frameFlags = (frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
}

// --- Update ------------------------------------------------------------------------------------

void I3D_frame::UpdateChildren()
{
    const uint32_t changed = frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED);
    if (changed) {
        ++changeCount;
        if (type == FRAME_VISUAL) {
            auto *visual = static_cast<I3D_visual *>(this);
            if (!(visual->visualFlags & VISF_WORLD_SPACE))
                visual->visualFlags &= ~VISF_WBOUND_VALID;
        }
    }
    for (I3D_frame *c = firstChild; c; c = c->nextSibling) {
        if (changed)
            c->frameFlags = (c->frameFlags & ~0x120u) | FRMFLAGS_CHILD_CHANGED;
        c->UpdateChildren();
        c->MatrixChanged();
        if (c->frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED)) {
            c->frameFlags &= ~(FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED);
            frameFlags = (frameFlags & ~FRMFLAGS_BOUND_VALID) | FRMFLAGS_CHILD_CHANGED;
        }
    }
}

void __stdcall I3D_frame::Update()
{
    UpdateChildren();
    MatrixChanged();
    ClearParentBounds();
    if ((frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED)) && type == FRAME_VISUAL) {
        auto *visual = static_cast<I3D_visual *>(this);
        if (!(visual->visualFlags & VISF_WORLD_SPACE))
            visual->visualFlags &= ~VISF_WBOUND_VALID;
    }
    frameFlags &= ~(FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED);
    if (g_targetUpdates.Size() && !g_targetUpdateBusy) {
        g_targetUpdateBusy = true;
        // (By index: the original keeps a pointer into the list, which a target that adds to it
        // would leave dangling.)
        const int count = g_targetUpdates.Size();
        for (int i = 0; i < count; ++i)
            UpdateTarget(g_targetUpdates[i]);
        g_targetUpdates.Clear();
        g_targetUpdateBusy = false;
    }
}

void __stdcall I3D_frame::UpdateBound()
{
    if (type == FRAME_SECTOR) {
        // The sector's box in world space.
        const I3D_bbox &box = static_cast<I3D_sector *>(this)->box;
        if (!box.IsValid()) {
            bound.bbox.Invalidate();
            bound.radius = 0.0f;
        } else {
            const S_matrix &m = GetMatrix();
            bound.bbox.min = bound.bbox.max = box.Corner(0) * m;
            for (int i = 1; i < 8; ++i)
                bound.bbox.Expand(box.Corner(i) * m);
            bound.MakeSphere();
        }
        frameFlags |= FRMFLAGS_BOUND_VALID;
        return;
    }
    if (type == FRAME_VISUAL)
        bound.bbox = static_cast<I3D_visual *>(this)->GetWorldBound();
    else
        bound.bbox.Invalidate();
    I3D_bbox &b = bound.bbox;
    for (I3D_frame *c = firstChild; c; c = c->nextSibling) {
        if (!(c->frameFlags & FRMFLAGS_ON))
            continue;
        const I3D_bbox &cb = c->GetBound().bbox;
        if (!cb.IsValid())
            continue;
        if (cb.min.x < b.min.x)
            b.min.x = cb.min.x;
        if (b.max.x < cb.max.x)
            b.max.x = cb.max.x;
        if (cb.min.y < b.min.y)
            b.min.y = cb.min.y;
        if (b.max.y < cb.max.y)
            b.max.y = cb.max.y;
        if (cb.min.z < b.min.z)
            b.min.z = cb.min.z;
        if (b.max.z < cb.max.z)
            b.max.z = cb.max.z;
    }
    bound.MakeSphere();
    frameFlags |= FRMFLAGS_BOUND_VALID;
}

void __stdcall I3D_frame::SetHierarchyLightLevel(float level)
{
    if (type == FRAME_VISUAL && (static_cast<I3D_visual *>(this)->visualFlags & VISF_OBJECT_PARAMS))
        ObjectSetLightLevel(this, level);
    for (I3D_frame *c = firstChild; c; c = c->nextSibling)
        c->SetHierarchyLightLevel(level);
}

void __stdcall I3D_frame::SetHierarchyAlpha(float alpha)
{
    if (type == FRAME_VISUAL && (static_cast<I3D_visual *>(this)->visualFlags & VISF_OBJECT_PARAMS))
        ObjectSetAlpha(this, alpha);
    for (I3D_frame *c = firstChild; c; c = c->nextSibling)
        c->SetHierarchyAlpha(alpha);
}
