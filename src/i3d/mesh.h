// I3D_face_group, I3D_mesh_level, I3D_mesh: the visuals' geometry (0x10030990 - 0x10032230; the
// class names are the game SDK's).
//
// A mesh (vtable 0x1009ce60, 0x70 bytes) has up to ten detail levels. A level (vtable 0x1009ce10,
// 0x70 bytes) keeps 0x20-byte vertices (position, normal, texture coordinates) in a shared vertex
// buffer and a list of face groups (vtable 0x1009cdf0, 0x1C bytes): a material and its triangles
// in a shared index buffer. For drawing a level gets a second vertex buffer in the layout its
// materials need (CreateRenderVB / FillRenderVB): generated environment texture coordinates, or
// the level's extra per-vertex data.
//
// The driver makes them (slots 0x64 / 0x68 / 0x6C); the levels and meshes count references.
#pragma once

#include "i3d/i3d.h"
#include "i3d/vb_manager.h"

class I3D_face_group {
public:
    I3D_face_group();                       // 0x10030a20
    // 0x10030e20 (not virtual): releases the material and the faces and deletes the rest of the
    // list after this one.
    ~I3D_face_group();
    I3D_face_group(const I3D_face_group &) = delete;
    I3D_face_group &operator=(const I3D_face_group &) = delete;

    // 0x00 0x10030a40: the material (a reference).
    virtual void __stdcall SetMaterial(I3D_material *newMaterial);
    // 0x04 0x10030a70: `count` faces copied in (null: none); returns the faces, locked.
    virtual I3D_triface *__stdcall SetFaces(const I3D_triface *src, int count);
    // 0x08 0x10030ad0: room for `count` faces in the shared index buffer, locked.
    virtual I3D_triface *__stdcall AllocFaces(int count);
    // 0x0C 0x10030b40: the faces from the index buffer's lock (bit 0 of `flags`: read only);
    // null without faces.
    virtual I3D_triface *__stdcall Lock(uint32_t flags);
    virtual void __stdcall Unlock();        // 0x10 0x10030c40
    // 0x14 0x10030cc0: the faces and the material of another face group.
    virtual I3D_RESULT __stdcall Duplicate(I3D_face_group *src);
    // 0x18 0x10030d20: a material of its own (a copy) when the current one is shared.
    virtual I3D_RESULT __stdcall DuplicateMaterial();

    I3D_material *material;                 // +0x04
    int numFaces;                           // +0x08
    I3D_face_group *next;                   // +0x0C the level's list
    ls3d::S_IBAlloc faces;                  // +0x10 numFaces * 3 indices
};
static_assert(sizeof(I3D_face_group) == 0x1C, "I3D_face_group layout");

// I3D_mesh_level::flags.
enum : uint32_t {
    // How FillRenderVB makes the environment texture coordinates (the material's SetBits12
    // value): 2 - 4 from the position transformed by the matrix it gets (x z, y z, x y), 5 the
    // level's extra data.
    MLF_ENV_SOURCE_MASK = 0x07,
    MLF_ENV = 0x08,                         // a face group's material has an environment texture
    MLF_RENDER_STALE = 0x20,                // the render vertex buffer waits for FillRenderVB
};

// GetStats (the levels' sums for a mesh).
struct S_meshStats {
    uint32_t size;                          // +0x00 0x70
    int numVertices;                        // +0x04
    int vertexBytes;                        // +0x08 numVertices * 0x20
    int numFaces;                           // +0x0C
    int faceBytes;                          // +0x10 numFaces * 6
};

class I3D_mesh_level {
public:
    I3D_mesh_level();                       // 0x10030db0
    ~I3D_mesh_level();                      // (in Release)
    I3D_mesh_level(const I3D_mesh_level &) = delete;
    I3D_mesh_level &operator=(const I3D_mesh_level &) = delete;

    // 0x00 0x10030e70: the last reference deletes the level.
    virtual uint32_t __stdcall Release();
    // 0x04 0x10031380: the vertices from the vertex buffer's lock (bit 0 of `flags`:
    // read only); null without a buffer.
    virtual void *__stdcall LockVertices(uint32_t flags);
    virtual void __stdcall UnlockVertices();                    // 0x08 0x10031480
    // 0x0C 0x100314f0: the vertices, extra data, face groups and box of another level.
    virtual I3D_RESULT __stdcall Duplicate(I3D_mesh_level *src);
    virtual int __stdcall NumFaces();                           // 0x10 0x10031790 all face groups'
    virtual I3D_face_group *__stdcall GetFaceGroup(int index);  // 0x14 0x10031760
    virtual I3D_RESULT __stdcall GetStats(S_meshStats *out);    // 0x18 0x100317b0
    // 0x1C 0x100317f0: puts a face group first in the list (its material's environment texture
    // sets the level's MLF_ENV flags).
    virtual void __stdcall AddFaceGroup(I3D_face_group *group);
    // 0x20 0x10031890: takes a face group out of the list (I3DERR_OBJECTNOTFOUND).
    virtual I3D_RESULT __stdcall RemoveFaceGroup(I3D_face_group *group);
    // 0x24 0x10031020: `count` vertices copied in (null: none), then the box.
    virtual I3D_RESULT __stdcall SetVertices(const void *src, int count);
    // 0x28 0x100311e0: room for `count` vertices, locked (null on failure).
    virtual void *__stdcall AllocVertices(int count);
    // 0x2C 0x10030f30: the box of the vertices' positions.
    virtual void __stdcall ComputeBounds();
    // 0x30 0x10031900: the render vertex buffer (in `dest`, or the level's own): usage from
    // `flags` (bit 0 static, bit 1 software processing), the layout from the level's flags and
    // extra data (bit 4: no environment coordinates, bit 5: positions only); filled unless bit 3.
    virtual bool __stdcall CreateRenderVB(uint32_t flags, const S_matrix *envMatrix, ls3d::S_VBAlloc *dest);
    virtual void __stdcall FreeRenderVB();                      // 0x34 0x10031a00
    // 0x38 0x10031a30: copies the vertices into the render vertex buffer with what it adds;
    // `deferred` (or no buffer) only marks it stale.
    virtual void __stdcall FillRenderVB(bool deferred, const S_matrix *envMatrix, ls3d::S_VBAlloc *dest);
    // 0x3C 0x10031d20: the extra per-vertex data's layout (bits: 1 four bytes, 2 eight, 4 16, 8
    // 24); returns the new (uninitialised) array.
    virtual void *__stdcall SetExtraFormat(uint32_t format);
    virtual int __stdcall ExtraStride();                        // 0x40 0x10031ce0 bytes per vertex

    // Levels live in 16-byte aligned blocks.
    static void *operator new(size_t size) noexcept;
    static void operator delete(void *p);

    uint32_t unknown04[3];                  // +0x04 (not initialised)
    S_vector4 bboxMin;                      // +0x10 (w: the first vertex's normal x)
    S_vector4 bboxMax;                      // +0x20
    int refCount;                           // +0x30
    ls3d::S_VBAlloc vertices;               // +0x34
    ls3d::S_VBAlloc renderVertices;         // +0x40
    uint8_t *extraData;                     // +0x4C
    uint32_t extraFormat;                   // +0x50
    int numVertices;                        // +0x54
    int numFaceGroups;                      // +0x58
    I3D_face_group *faceGroups;             // +0x5C
    float lodDistance;                      // +0x60 the squared camera distance (x the LOD scale) the level starts at
    uint32_t flags;                         // +0x64 MLF_*
    float envScale;                         // +0x68 2 sqrt(2) / the material's 7 low flag bits
    uint32_t unknown6C;                     // +0x6C
};
static_assert(sizeof(I3D_mesh_level) == 0x70, "I3D_mesh_level layout");
static_assert(offsetof(I3D_mesh_level, vertices) == 0x34, "I3D_mesh_level layout");
static_assert(offsetof(I3D_mesh_level, faceGroups) == 0x5C, "I3D_mesh_level layout");

class I3D_mesh {
public:
    enum { kMaxLevels = 10 };

    I3D_mesh();                             // 0x10031d80
    ~I3D_mesh();                            // (in Release)
    I3D_mesh(const I3D_mesh &) = delete;
    I3D_mesh &operator=(const I3D_mesh &) = delete;

    virtual uint32_t __stdcall Release();                       // 0x00 0x10031dd0
    virtual I3D_mesh_level *__stdcall GetLevel(int index);      // 0x04 0x10031e20 (not checked)
    // 0x08 0x10031fa0: the level at `index` (a reference; null: none, and the levels counted end
    // there); then the bounds.
    virtual I3D_RESULT __stdcall SetLevel(I3D_mesh_level *level, int index);
    // 0x0C 0x10032020: copies of another mesh's levels.
    virtual I3D_RESULT __stdcall Duplicate(I3D_mesh *src);
    virtual I3D_RESULT __stdcall GetStats(S_meshStats *out);    // 0x10 0x100321d0
    // 0x14 0x10031e30: the box of the levels' boxes and the sphere around it.
    virtual void __stdcall ComputeBounds();
    // 0x18 0x10032130: every face group of every level gets a material of its own.
    virtual I3D_RESULT __stdcall DuplicateMaterials();
    // 0x1C 0x10032180: the same levels with the same vertex counts.
    virtual bool __stdcall SameLayout(const I3D_mesh *other);

    static void *operator new(size_t size) noexcept;
    static void operator delete(void *p);

    uint32_t unknown04[3];                  // +0x04 (not initialised)
    S_vector4 bboxMin;                      // +0x10
    S_vector4 bboxMax;                      // +0x20
    S_vector center;                        // +0x30 of the box
    float radius;                           // +0x3C half the box's diagonal
    int refCount;                           // +0x40
    uint32_t numLevels;                     // +0x44
    I3D_mesh_level *levels[kMaxLevels];     // +0x48
};
static_assert(sizeof(I3D_mesh) == 0x70, "I3D_mesh layout");
static_assert(offsetof(I3D_mesh, levels) == 0x48, "I3D_mesh layout");

namespace ls3d::i3d {

// The renderer's environment texture coordinates (0x10065a90): each vertex copied with (u, v)
// after it, from two axes of its position times `matrix`, scaled by `scale` and the driver's
// DRVSTATE_ENV_UV_SCALE / 2 sqrt(2).
void MakeEnvCoordinates(uint8_t *dst, int dstStride, const uint8_t *src, int srcStride, int count,
                        const S_matrix *matrix, float scale, int source);

}  // namespace ls3d::i3d
