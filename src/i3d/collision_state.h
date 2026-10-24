// The collision query in progress (0x100ad9b0 - 0x100adb08), shared by the ray and the sphere
// queries (collision_ray.cpp, collision_sphere.cpp).
#pragma once

#include "i3d/collision.h"

class I3D_billboard;
class I3D_scene;

namespace ls3d::i3d::col {

struct S_query {
    int cbResult;                           // 0x100ad9b0 the hit callback's last answer
    S_vector normal;                        // 0x100ad9e0 the best hit's normal (its frame's space)
    bool done;                              // 0x100ad9d0 a first hit ended the query
    int lodLevel;                           // 0x100ada00 the mesh level tested
    float length2;                          // 0x100ada04 the ray's squared length
    I3D_frame *hitFrame;                    // 0x100ada08
    I3D_face_group *hitGroup;               // 0x100ada0c
    uint32_t flags;                         // 0x100ada14 I3DCOL_*
    S_vector sweepMin;                      // 0x100ada20 (spheres) the swept box
    S_vector sweepMax;                      // 0x100ada30
    uint32_t hitFace[3];                    // 0x100ada40 the best hit face's vertex indices
    void *normalOut;                        // 0x100ada70 non-null: the hit normal is wanted
    S_vector4 result;                       // 0x100ada80 (spheres) where the sphere stops
    int frameLodLevel;                      // 0x100ada90 the level lodFrame is tested at
    float bestDist2;                        // 0x100ada94 the nearest hit's squared distance
    S_vector from;                          // 0x100adaa0
    I3D_scene *scene;                       // 0x100adab0
    S_vector to;                            // 0x100adac0 (spheres) from + dir
    float length;                           // 0x100adad0
    S_vector dir;                           // 0x100adae0 normalized
    void *user;                             // 0x100adaf0 the callbacks' (the filters': an S_vector color)
    float radius;                           // 0x100adaf4 (spheres)
    I3D_COLLISION_HIT *hitCb;               // 0x100adaf8
    I3D_COLLISION_FILTER *filter;           // 0x100adafc
    I3D_frame *excluded;                    // 0x100adb00 a visual not tested (its children are)
    I3D_frame *skipped;                     // 0x100adb04 a subtree not walked
    I3D_frame *lodFrame;                    // 0x100adb08 the frame tested at frameLodLevel
};

extern S_query q;

// The SDK's inline normalization of a query direction (z, y, x summed).
S_vector NormalizedDir(const S_vector &d);
// The matrix a billboard is tested with: in the camera's orientation (modes 2 and 3) looking back
// along the query's direction, else facing the query's start (I3D_billboard::ComputeMatrix).
S_matrix QueryFacing(I3D_billboard *bb);

}  // namespace ls3d::i3d::col
