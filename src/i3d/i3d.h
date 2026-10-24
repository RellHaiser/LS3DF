// I3D: the scene engine of LS3DF (0x10001000 - 0x10069a00). Basic types shared by its classes.
//
// The classes are the original's, in the global namespace (two members are exported with their
// decorated names: I3D_frame::UpdateWMatrixProc, I3D_visual::UpdateWBoundProc). Interface methods
// are __stdcall (COM style, `this` on the stack), internal virtuals too unless marked; member
// offsets are the original's (the game reads several inline, through its SDK headers).
#pragma once

#include <cstddef>
#include <cstdint>
#include <malloc.h>

#include <windows.h>

#include "core/i3d_result.h"
#include "math/ls3d_math.h"

class I3D_frame;
class I3D_visual;
class I3D_scene;
class I3D_sector;
class I3D_driver;
class I3D_material;
class I3D_anim_material;
class I3D_face_group;
class I3D_mesh_level;
class I3D_mesh;
class I3D_animation_set;
class I3D_pose;
class I3D_pose_set;

// A triangle: three vertex indices.
struct I3D_triface {
    uint16_t v[3];
};
static_assert(sizeof(I3D_triface) == 6, "I3D_triface layout");

typedef HRESULT I3D_RESULT;

// A visual type as GetVisualType returns it: the characters in memory order read as a
// little-endian dword ("BBRD" is 0x44524242). (A multi-character literal 'BBRD' is the reverse,
// 0x42425244, and never matches.)
template <size_t N> constexpr uint32_t FourCC(const char (&s)[N])
{
    static_assert(N >= 2 && N <= 5, "one to four characters");
    uint32_t v = 0;
    for (size_t i = 0; i + 1 < N; ++i)
        v |= static_cast<uint32_t>(static_cast<uint8_t>(s[i])) << (8 * i);
    return v;
}
static_assert(FourCC("BBRD") == 0x44524242, "fourCC byte order");

// Frame types (I3D_frame::type).
enum E_I3D_FRAME_TYPE : uint16_t {
    FRAME_NULL = 0,
    FRAME_VISUAL = 1,
    FRAME_LIGHT = 2,
    FRAME_CAMERA = 3,
    FRAME_SOUND = 4,
    FRAME_SECTOR = 5,
    FRAME_DUMMY = 6,
    FRAME_TARGET = 7,
    FRAME_USER = 8,
    FRAME_MODEL = 9,
    FRAME_JOINT = 10,
    FRAME_VOLUME = 11,
    FRAME_OCCLUDER = 12,
    FRAME_SCENE = 13,
    FRAME_AREA = 14,
    FRAME_SHADOW = 15,
    FRAME_LAST
};

// Frame flags (I3D_frame::frameFlags).
enum : uint32_t {
    FRMFLAGS_ON = 0x00000001,
    FRMFLAGS_ROT_VALID = 0x00000004,           // the local matrix' rotation is the scaled quaternion
    FRMFLAGS_ROT_QUAT_VALID = 0x00000008,      // rot (quaternion) up to date
    FRMFLAGS_LMATRIX_VALID = 0x00000010,       // the local matrix' rotation part is up to date
    FRMFLAGS_WMATRIX_VALID = 0x00000020,
    FRMFLAGS_POS_LOCKED = 0x00000080,          // the position setters do nothing
    FRMFLAGS_BOUND_VALID = 0x00000100,
    FRMFLAGS_CALLBACK_MASK = 0x00001a00,       // SetCallback's message bits
    FRMFLAGS_CHILD_CHANGED = 0x00002000,       // a descendant moved since the last Update
    FRMFLAGS_TARGETED = 0x00004000,            // frame of an I3D_target
    FRMFLAGS_SHADOW_CASTER = 0x00008000,       // in an I3D_shadow's caster list
    FRMFLAGS_SECTOR_MASK = 0x001c0000,         // (sector bookkeeping, LinkTo)
    FRMFLAGS_SCENE_SOUND = 0x00800000,         // known to the driver's scenes (I3D_driver slot 0x94)
    FRMFLAGS_NAMED = 0x20000000,               // in the driver's name tree
    FRMFLAGS_CHANGED = 0x40000000,             // moved since the last Update
    FRMFLAGS_SCENE_DUMMY = 0x80000000,         // dummies: in the scenes' list of drawn boxes
};

// Bounding box: min and max corners, each padded to 16 bytes. An "empty" box has min > max.
struct I3D_bbox {
    S_vector min;
    float minW;
    S_vector max;
    float maxW;

    void Invalidate()                      // 1e16 / -1e16 (0x5a0e1bca / 0xda0e1bca)
    {
        min.x = min.y = min.z = 1e16f;
        max.x = max.y = max.z = -1e16f;
    }
    bool IsValid() const { return min.x <= max.x; }
    // Grows the box to hold `p` (0x10015050; NaN coordinates leave it as it is).
    void Expand(const S_vector &p)
    {
        if (p.x < min.x)
            min.x = p.x;
        if (max.x < p.x)
            max.x = p.x;
        if (p.y < min.y)
            min.y = p.y;
        if (max.y < p.y)
            max.y = p.y;
        if (p.z < min.z)
            min.z = p.z;
        if (max.z < p.z)
            max.z = p.z;
    }
    // Corner `i` in the order the engine walks a box: min, then x, y, x, z, x, y, x toggled.
    S_vector Corner(int i) const
    {
        static const uint8_t kBits[8] = {0, 1, 3, 2, 6, 7, 5, 4};      // bit 0 x, 1 y, 2 z: max
        const int b = kBits[i];
        return S_vector(b & 1 ? max.x : min.x, b & 2 ? max.y : min.y, b & 4 ? max.z : min.z);
    }
};
static_assert(sizeof(I3D_bbox) == 0x20, "I3D_bbox layout");

// A sphere (the driver's DrawSphere).
struct I3D_bsphere {
    S_vector center;
    float radius;
};

struct I3D_bound_volume {
    I3D_bbox bbox;
    S_vector center;                       // sphere around the box
    float radius;

    // The sphere around the box (0x1001ee00): its center and half diagonal; radius 0 for an empty
    // box (the center then stays as it was).
    void MakeSphere()
    {
        if (!bbox.IsValid()) {
            radius = 0.0f;
            return;
        }
        const float hx = (bbox.max.x - bbox.min.x) * 0.5f;
        const float hy = (bbox.max.y - bbox.min.y) * 0.5f;
        const float hz = (bbox.max.z - bbox.min.z) * 0.5f;
        center = S_vector(hx + bbox.min.x, hy + bbox.min.y, hz + bbox.min.z);
        radius = std::sqrt((hz * hz + hy * hy) + hx * hx);
    }
};
static_assert(sizeof(I3D_bound_volume) == 0x30, "I3D_bound_volume layout");

// Frame enumeration callback (EnumFrames): 0 continue, 1 skip the children, 2 stop.
typedef uint32_t __stdcall I3D_ENUMPROC(I3D_frame *frame, void *context);
// Frame callback (SetCallback): the frame, a message (FRMFLAGS_CALLBACK_MASK bits), parameters.
typedef int __stdcall I3D_CALLBACK(I3D_frame *frame, uint32_t message, uint32_t param1, uint32_t param2);

// EnumFrames / FindChildFrame flags (above the 16 type bits).
enum : uint32_t {
    ENUMF_ALL_TYPES = 0x0000ffff,
    ENUMF_WILDMASK = 0x00010000,           // '*' and '?' in the name mask
    ENUMF_CASESENSITIVE = 0x00020000,
};

namespace ls3d::i3d {

// The engine's 16-byte aligned blocks: the statically linked CRT's _aligned_malloc (0x1008d42a)
// and _aligned_free (0x1008d472).
inline void *AlignedAlloc(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
inline void AlignedFree(void *p) { _aligned_free(p); }

}  // namespace ls3d::i3d
