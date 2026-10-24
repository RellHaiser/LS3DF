// The scene's collision queries (I3D_scene slots 0xFC - 0x114; 0x1000c7a0 - 0x10015050): rays and
// moving spheres against the frames' visuals (their boxes, then the faces of their mesh level),
// the skinned meshes against their joints' boxes.
#pragma once

#include "i3d/i3d.h"

class I3D_face_group;
class I3D_frame;
class I3D_material;

// The query flags (the ray queries'; the sphere queries share the frame ones).
enum : uint32_t {
    I3DCOL_BOX_ONLY = 0x00000001,           // the visuals' boxes, no faces
    I3DCOL_NO_JOINTS = 0x00008000,          // skinned meshes are not tested
    I3DCOL_ALL_HITS = 0x00010000,           // no pruning by the nearest hit (filters see every face)
    I3DCOL_FILTER = 0x00020000,             // faces go through a filter (the given one, else alpha)
    I3DCOL_UNLIMITED = 0x00040000,          // the ray goes on past its length
    I3DCOL_DONE_AT_ROOT = 0x00080000,       // IntersectFrame: the frame's children are not walked
    I3DCOL_TWO_SIDED = 0x00100000,          // every face is tested from both sides
    I3DCOL_FIRST_HIT = 0x01000000,          // the first hit ends the query
    I3DCOL_LOD_FRAME = 0x02000000,          // the frame argument is tested at the level given, the rest at 0
    I3DCOL_SKIP_FLAGGED = 0x04000000,       // visuals with VISF_0x1000 are not tested
    I3DCOL_HIDDEN_TOO = 0x08000000,         // frames switched off are walked too
    I3DCOL_SKIP_FRAME = 0x10000000,         // the frame argument's subtree is not walked (else only it)
    I3DCOL_OMNI_SECTOR = 0x20000000,        // also the omni sector (after the primary)
    I3DCOL_NEAR_SECTOR = 0x40000000,        // also the near sector (first)
    I3DCOL_FIRST_FACE = 0x80000000,         // a visual's first hit face is its hit (no nearest search)
};

// What a face filter sees: the texture coordinates of the hit (when the material has a
// texture) and the face group's material.
struct S_collisionTexel {
    float u, v;                             // +0x00
    I3D_material *material;                 // +0x08
};

// A face filter (__stdcall): 0 the face blocks (the color is cleared, no more filtering), 1 it is
// hit (filters may tint the color), 2 the ray passes. `reserved` points to nothing (the original
// passes an unset local), `t` is the hit's distance along the frame's local ray.
typedef int __stdcall I3D_COLLISION_FILTER(const void *reserved, float t, I3D_frame *frame, void *user,
                                           const S_collisionTexel *texel);
// A hit callback (__stdcall): 0 / 1 the hit counts (1 also ends the walk), 2 it is ignored, 3 it
// is ignored and the visual's children are not walked.
typedef int __stdcall I3D_COLLISION_HIT(const S_vector *normal, float distance, I3D_frame *frame, void *user);

// IntersectFrame's result.
struct I3D_collision_info {
    float distance;                         // +0x00
    uint32_t unknown04[3];                  // +0x04 (not written)
    S_vector4 normal;                       // +0x10 world space, normalized
    uint16_t face[3];                       // +0x20 the hit face's vertex indices (0 for boxes)
    uint16_t unknown26;                     // +0x26
    I3D_face_group *faceGroup;              // +0x28
};

namespace ls3d::i3d {

// The face filters (FUN_10010a90, the default: texel alpha x opacity below 0.1 passes; 0x10064860,
// the lens flares' and the lightmaps': `user` is an S_vector color the faces tint, partly
// transparent ones passing).
int __stdcall AlphaFilter(const void *reserved, float t, I3D_frame *frame, void *user, const S_collisionTexel *texel);
int __stdcall TransmitFilter(const void *reserved, float t, I3D_frame *frame, void *user,
                             const S_collisionTexel *texel);

}  // namespace ls3d::i3d
