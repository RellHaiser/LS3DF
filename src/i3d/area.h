// I3D_area: a convex space for sounds (FRAME_AREA; vtable 0x1009b708, 0x160 bytes; 0x10005550 -
// 0x10005f80). Sounds added to an area play while the camera is inside it (the renderer tests the
// camera against the area's planes when the area or its sounds changed).
#pragma once

#include "core/c_vector.h"
#include "i3d/frame.h"

class I3D_scene;
class I3D_sound;
struct I3D_triface;

class I3D_area : public I3D_frame {
public:
    // 0x38 0x10005750: also the scene of the new parent's sector (or of the scene that loaded it);
    // always I3D_OK.
    I3D_RESULT __stdcall LinkTo(I3D_frame *newParent, uint32_t flags) override;
    // 0x4C 0x10005ab0: also the mesh of another area.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10005550 / 0x100055c0
    void Init() override;
    void Uninit() override;
    // 0x68 0x100057d0: world vertices and face planes.
    void MatrixChanged() override;
    // 0x70 0x10005f40: areas are not loaded from files (a message only; the data is not read).
    void Load() override;
    // 0x74 0x10005b10: the mesh from `numVerts` vertices `stride` bytes apart (16 bytes each read)
    // and their triangles.
    virtual I3D_RESULT __stdcall Build(const void *verts, int numVerts, const I3D_triface *tris, int numTris,
                                       int stride);
    // 0x78 / 0x7C 0x10005cd0 / 0x10005e30: a sound plays in the area (each side's list).
    virtual void __stdcall AddSound(I3D_sound *sound);
    virtual I3D_RESULT __stdcall RemoveSound(I3D_sound *sound);
    // 0x80 0x10005c70: `pos` (world) behind or on every face plane.
    virtual bool __stdcall IsInside(const S_vector &pos);
    // 0x84 / 0x88 0x1001bb30 / 0x1001bb60: the time (ms, at least 1; Init 500) its sounds fade in
    // and out over when the camera enters / leaves.
    virtual uint32_t __stdcall SetFadeTime(uint32_t value);
    virtual uint32_t __stdcall GetFadeTime();

    // 0x10005640: the scene's area ring (I3D_scene::firstArea); always leaves the old ring first.
    void SetScene(I3D_scene *newScene);

    int numVertices;                        // +0x120
    S_vector4 *vertices;                    // +0x124 local (16-byte aligned)
    S_vector4 *worldVertices;               // +0x128
    int numFaces;                           // +0x12C
    I3D_triface *faces;                     // +0x130
    I3D_area *prevInScene;                  // +0x134 scene ring
    I3D_area *nextInScene;                  // +0x138
    S_plane *planes;                        // +0x13C world space, per face (16-byte aligned)
    I3D_scene *scene;                       // +0x140
    // +0x144 (renderer) the changeCount the camera was last tested at; -1 when the sounds change.
    int testedChangeCount;
    ls3d::C_vector<I3D_sound *> sounds;     // +0x148 AddSound
    uint32_t fadeTime;                    // +0x158 SetFadeTime
    uint32_t padding15C;
};
static_assert(sizeof(I3D_area) == 0x160, "I3D_area layout");
static_assert(offsetof(I3D_area, planes) == 0x13C, "I3D_area layout");
static_assert(offsetof(I3D_area, sounds) == 0x148, "I3D_area layout");
static_assert(offsetof(I3D_area, fadeTime) == 0x158, "I3D_area layout");
