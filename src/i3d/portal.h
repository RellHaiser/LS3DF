// I3D_portal: an opening of a sector into a neighbouring one (vtable 0x1009d070, 0x1C0 bytes;
// 0x1003ed10 - 0x1003f370, Load 0x100431a0). A sector owns its portals (I3D_sector::portals); the
// scene connects each to the sector it leads to (connectedSector, which lists it in its
// incomingPortals) and, for a two-way opening, to the portal of the other side.
//
// The vertices are in the owner's space; MatrixChanged puts them, the portal's plane and its bounding
// sphere into world space. The renderer looks through the portals that are on (0x10041dc0).
#pragma once

#include "i3d/i3d.h"

class I3D_scene;
class I3D_sector;

// Portal flags (I3D_portal::portalFlags).
enum : uint32_t {
    PRTF_OCCLUDED = 0x02,                   // (renderer) hidden by an occluder this frame
    PRTF_ON = 0x04,                         // open (SetOn; a closed portal also dampens sound)
    PRTF_VISITED = 0x08,                    // (renderer) looked through this frame
    PRTF_RANGE_CLIP = 0x10,                 // (renderer) not looked through beyond range
    PRTF_DIVIDES_SECTOR = 0x20,             // cuts its sector in two: reported
    PRTF_IN_PORTAL = 0x80,                  // (BuildPortals) matches another portal of its own sector
    PRTF_DUPLICATE_MASK = 0xF4,             // the flags Duplicate copies
};

// I3D_portal::renderFlags (the projection in ParsePortal).
enum : uint8_t {
    PRTR_PARTLY_BEHIND = 0x01,              // a vertex is behind the camera
    PRTR_ALL_BEHIND = 0x02,                 // every vertex is
};

class I3D_portal final {
public:
    static constexpr int kMaxVertices = 8;

    I3D_portal();                           // 0x1003ed10
    // 0x1003ed60 (not virtual: the vtable has the three methods only): leaves the scene and the
    // other side.
    ~I3D_portal();
    I3D_portal(const I3D_portal &) = delete;
    I3D_portal &operator=(const I3D_portal &) = delete;

    // 0x00 0x1003ef10: also the portal of the other side.
    virtual void __stdcall SetOn(bool on);
    // 0x04 0x1003ef60
    virtual bool __stdcall IsOn();
    // 0x08 0x1003eda0: the geometry, flags and ranges of another portal (the other side is lost).
    virtual I3D_RESULT __stdcall Duplicate(const I3D_portal *source);

    // 0x1003ef70: world vertices, bounding sphere and plane from the owner's world matrix; the
    // portal joins the owner's scene.
    void MatrixChanged();
    // 0x1003ee20: the scene's portal ring (I3D_scene::portalCount / firstPortal).
    void SetScene(I3D_scene *newScene);
    // 0x100431a0: the vertex count byte, then the plane, flags, ranges and vertices.
    void Load();

    uint32_t padding04[3];                  // +0x04 (16-byte alignment of the vectors)
    S_plane loadedPlane;                   // +0x10 (as loaded; the portal code uses the world plane)
    uint32_t portalFlags;                   // +0x20 PRTF_*
    float range;                            // +0x24 (renderer) beyond it the portal fades / clips
    float range2;                           // +0x28 (copied, not used by the portal code)
    uint32_t padding2C;                     // +0x2C
    S_vector4 vertices[kMaxVertices];       // +0x30 in the owner's space
    S_vector4 worldVertices[kMaxVertices];  // +0xB0 (the fourth dwords: whatever the transform left)
    S_plane plane;                          // +0x130 world space; the owner's side is in front
    S_vector4 rectMin;                      // +0x140 (renderer) the projected extent (x, y in -1 .. 1)
    S_vector4 rectMax;                      // +0x150
    S_vector4 clipMin;                      // +0x160 (renderer) rect within the clip rect of the portal it is seen through
    S_vector4 clipMax;                      // +0x170
    S_vector center;                        // +0x180 world space, of the vertices' box
    float centerW;                          // +0x18C (never set: stack garbage in the original)
    float radius;                           // +0x190 half the box' diagonal
    I3D_sector *owner;                      // +0x194
    I3D_sector *connectedSector;            // +0x198 the sector it leads to
    I3D_portal *otherSide;                  // +0x19C the connected sector's portal back, if any
    I3D_scene *scene;                       // +0x1A0
    I3D_portal *prev;                       // +0x1A4 scene ring
    I3D_portal *next;                       // +0x1A8
    int numVertices;                        // +0x1AC
    uint8_t renderFlags;                    // +0x1B0 (renderer) PRTR_*
    uint8_t padding1B1[0xF];

    // Portals live in 16-byte aligned blocks (AlignedAlloc; null when out of memory).
    static void *operator new(size_t size) noexcept;
    static void operator delete(void *p);
};
static_assert(sizeof(I3D_portal) == 0x1C0, "I3D_portal layout");
static_assert(offsetof(I3D_portal, loadedPlane) == 0x10, "I3D_portal layout");
static_assert(offsetof(I3D_portal, vertices) == 0x30, "I3D_portal layout");
static_assert(offsetof(I3D_portal, plane) == 0x130, "I3D_portal layout");
static_assert(offsetof(I3D_portal, center) == 0x180, "I3D_portal layout");
static_assert(offsetof(I3D_portal, owner) == 0x194, "I3D_portal layout");
static_assert(offsetof(I3D_portal, numVertices) == 0x1AC, "I3D_portal layout");

namespace ls3d::i3d {

// Counts the portals made and destroyed (0x100bf350; the scene rebuilds its portal data).
extern int g_portalChanges;

}  // namespace ls3d::i3d
