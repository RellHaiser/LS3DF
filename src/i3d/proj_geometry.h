// The projected geometry of projectors and shadows (0x10001000 - 0x10004c60): the faces of
// meshes inside a projection volume, clipped to it, in the volume's space. Begin starts a
// collection, Setup sets the volume (its projection matrix maps the volume to x, y in -1 - 1 and
// z in 0 - 1; perspective volumes map x, y to -z - z), each Project* call adds a mesh; the result
// is one vertex array and two face lists (the faces of opaque and of blended materials).
#pragma once

#include <cstdint>

#include "math/ls3d_math.h"

class I3D_face_group;

class I3D_visual;

namespace ls3d::i3d::proj {

// The meshes a projector or shadow found under it (0x100be9f0).
extern I3D_visual *g_receivers[0x200];

// 0x10001200: an empty collection.
void Begin();
// 0x10001010: the projection (volume space from the meshes' world space), orthogonal or
// perspective; the clipped polygons' new vertices move `normalBias` along their face's normal and
// `edgeExpand` away from the polygon's center (both in volume units); `opacity` (0 - 1) culls
// faces turned away from the projection more strongly the lower it is (above 0.9999: both sides
// kept). (`world`: kept, unused.)
void Setup(const S_matrix *projection, const S_matrix *world, bool orthogonal, float normalBias, float edgeExpand,
           float opacity);
// A mesh's faces (`groups`; `numFaces` their total, for the buffers) into the collection;
// `withBlended`: also the faces of blended materials. The vertices are local (`world`: to world
// space) or in world space, render vertices (32 bytes) or S_vector4 (16 bytes).
void ProjectMesh(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended,
                 const S_matrix *world);                                    // 0x10001500
void ProjectMesh4(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended,
                  const S_matrix *world);                                   // 0x10004630
void ProjectWorldMesh(const void *verts, int numVerts, I3D_face_group *groups, int numFaces,
                      uint32_t withBlended);                                // 0x10004380
void ProjectWorldMesh4(const void *verts, int numVerts, I3D_face_group *groups, int numFaces,
                       uint32_t withBlended);                               // 0x100048e0
// (The buffers are freed by FreeProjectionBuffers, render_passes.h.)

// The collection: the vertices (volume space), the opaque materials' faces, then the blended
// ones' (indices into the vertices).
const S_vector4 *Vertices();
int NumVertices();
const uint16_t *OpaqueFaces();
int NumOpaqueFaces();
const uint16_t *BlendedFaces();
int NumBlendedFaces();
// The sum of the blended faces' vertices and a count meant for it (never counted: always 0).
const S_vector &BlendedSum();
int BlendedSumCount();

}  // namespace ls3d::i3d::proj
