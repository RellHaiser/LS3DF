// I3D_occluder: a convex mesh that hides what is behind it (FRAME_OCCLUDER; vtable 0x1009b8e8,
// 0x600 bytes; 0x10038490 - 0x1003ab60).
//
// Its faces are planes in world space (MatrixChanged); each frame the renderer builds the volume
// it hides as seen from the camera (UpdateOcclusionVolume): the planes of the faces turned to the
// camera ("front" planes) and, through the camera, of the silhouette edges between a face turned
// to the camera and one turned away. Occluders then hide farther occluders, visuals and portals
// that lie entirely inside those planes (OccludeSectors, OccludePortals). Sectors make one of
// their geometry on request (I3D_sector::MakeReady).
#pragma once

#include "i3d/frame.h"

class I3D_scene;
class I3D_sector;
struct I3D_triface;

// Occluder flags (I3D_occluder::occluderFlags).
enum : uint32_t {
    OCCF_ACTIVE = 0x2,                      // has an occlusion volume this frame (silhouette planes)
    OCCF_STRADDLES_PORTALS = 0x4,           // the camera sees both sides of its sector's portals
};

// (renderer) The frame is inside the view this frame.
constexpr uint32_t FRMFLAGS_IN_VIEW = 0x00010000;

class I3D_occluder : public I3D_frame {
public:
    static constexpr int kMaxSilhouettePlanes = 35;    // 32 + the 3 edges of the last face
    static constexpr int kMaxFrontPlanes = 33;

    // 0x38 0x10038730: also the scene of the new parent's sector (or of the scene that loaded it).
    I3D_RESULT __stdcall LinkTo(I3D_frame *newParent, uint32_t flags) override;
    // 0x4C 0x10038be0: also the mesh of another occluder.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10038490 / 0x10038550
    void Init() override;
    void Uninit() override;
    // 0x68 0x100387c0: world vertices, face planes and the bounding sphere.
    void MatrixChanged() override;
    // 0x70 0x1003a960: the counts, vertices and faces.
    void Load() override;
    // 0x74 0x10038c40: the mesh from `numVerts` vertices `stride` bytes apart and their triangles.
    virtual I3D_RESULT __stdcall Build(const void *verts, int numVerts, const I3D_triface *tris, int numTris,
                                       int stride);

    // 0x10038610: the scene's occluder ring (I3D_scene::firstOccluder); always leaves the old ring
    // first, so the same scene moves the occluder to the ring's end.
    void SetScene(I3D_scene *newScene);
    // 0x100390f0: the occlusion volume as seen from g_renderCamera (OCCF_ACTIVE when it has one).
    // An occluder outside a scene takes its sector's scene instead.
    void UpdateOcclusionVolume();

private:
    // 0x10038d60: the face across each edge (neighbors, neighborEdges) and the face flags.
    I3D_RESULT BuildEdges();

public:
    int numVertices;                        // +0x120
    int numFaces;                           // +0x124
    S_vector4 *vertices;                    // +0x128 local (16-byte aligned; the fourth dword unset)
    S_vector4 *worldVertices;               // +0x12C
    I3D_triface *faces;                     // +0x130
    I3D_occluder *prevInScene;              // +0x134 scene ring
    I3D_occluder *nextInScene;              // +0x138
    uint16_t *neighbors;                    // +0x13C per face and edge: the face across it
    uint8_t *neighborEdges;                 // +0x140 ... and that face's edge
    S_plane *facePlanes;                    // +0x144 world space (16-byte aligned)
    uint16_t *faceFlags;                    // +0x148 bit 0: turned to the camera
    uint32_t padding14C;                    // +0x14C
    S_plane silhouettePlanes[kMaxSilhouettePlanes];    // +0x150 the inside is in front
    int numSilhouettePlanes;                // +0x380
    uint32_t padding384[3];
    S_plane frontPlanes[kMaxFrontPlanes];   // +0x390 the faces turned to the camera
    int numFrontPlanes;                     // +0x5A0
    uint32_t padding5A4[3];
    // +0x5B0 the view direction's plane through the farthest silhouette vertex: what lies beyond
    // it by more than its radius is hidden inside the silhouette planes, the rest must also be
    // behind the front planes.
    S_plane nearPlane;
    uint32_t occluderFlags;                 // +0x5C0 OCCF_*
    I3D_scene *scene;                       // +0x5C4
    uint32_t padding5C8[2];
    S_vector center;                        // +0x5D0 world space, of the vertices' box
    float centerW;                          // +0x5DC (MatrixChanged stores an unset value)
    float radius;                           // +0x5E0
    float radiusSquared;                    // +0x5E4
    I3D_occluder *nextInSector;             // +0x5E8 (renderer) the sector's list (I3D_sector::occluders)
    I3D_sector *ownerSector;                // +0x5EC the sector that made it (MakeReady)
    float viewDepth;                        // +0x5F0 (renderer) its center along the view direction
    uint32_t padding5F4[3];
};
static_assert(sizeof(I3D_occluder) == 0x600, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, prevInScene) == 0x134, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, silhouettePlanes) == 0x150, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, numSilhouettePlanes) == 0x380, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, frontPlanes) == 0x390, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, nearPlane) == 0x5B0, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, center) == 0x5D0, "I3D_occluder layout");
static_assert(offsetof(I3D_occluder, viewDepth) == 0x5F0, "I3D_occluder layout");

namespace ls3d::i3d {

// The renderer's occlusion passes.
// 0x10039ee0: when the scene's occluders changed, every sector's list of the occluders in it
// (not inside cameras or other sectors), the primary sector's last.
void RebuildOccluderLists(I3D_scene *scene);
// 0x1003a5a0: the sector's occluders get their volumes; its portals entirely hidden by one get
// PRTF_OCCLUDED (outgoing ones the camera sees from the front, incoming ones without another side
// from the back).
void OccludePortals(I3D_sector *sector);
// 0x1003a000: with occlusion on (driver state 0x27), in each rendered sector the nearer occluders
// hide farther ones, then the visuals of its render list.
void OccludeSectors(I3D_scene *scene);

}  // namespace ls3d::i3d
