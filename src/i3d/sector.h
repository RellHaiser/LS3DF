// I3D_sector: a space of the scene (FRAME_SECTOR; vtable 0x1009bc20, 0x290 bytes; methods
// 0x1004c9b0 - 0x1004e530). A sector is a convex hull (its faces' planes, in world space) with
// portals into its neighbours; it keeps the lights shining and the sounds playing in it, a sound
// environment (reverb parameters) and, on request, an occluder made of its geometry.
//
// Frames in a sector are positioned in its space; the frame code reads the sector's cached
// inverse world matrix and its box inline. Each scene has a "Primary sector" for the space outside
// all others (scene.h); the other sectors are in the scene's ring.
#pragma once

#include "core/c_vector.h"
#include "i3d/frame.h"

class I3D_light;
class I3D_occluder;
class I3D_portal;
class I3D_scene;
class I3D_sound;
class I3D_visual;

// Sector flags (I3D_sector::sectorFlags).
enum : uint32_t {
    SCTF_VISIBLE = 0x00000001,              // (renderer) rendered this frame (Init sets it)
    SCTF_RENDER_MASK = 0x00000093,          // (renderer) 1, 2: being looked into, 0x10, 0x80
    SCTF_ON_PATH = 0x00000080,              // (renderer) on the portal path back to the camera
    SCTF_INVMATRIX_VALID = 0x00000020,      // invWorldMatrix up to date
    SCTF_MAKE_OCCLUDER = 0x00000040,        // MakeReady builds an occluder of the geometry
    SCTF_PRIMARY = 0x00000200,              // the scene's primary sector
    SCTF_ENV_FADING_OUT = 0x00000400,       // (renderer) the listener blends out of its environment
    SCTF_ENV_CHANGED = 0x00000800,          // the sound environment changed
    SCTF_NO_PLANES = 0x00007200,            // (any of) no hull planes are computed
    SCTF_BACKDROP = 0x00001000,             // the scene's backdrop sector
    SCTF_OMNI = 0x00002000,                 // the scene's omni sector
    SCTF_NEAR = 0x00004000,                 // the scene's near sector
    SCTF_AMBIENT_CHANGED = 0x00008000,      // an ambient light changed: the ambient sum is stale
    SCTF_DUPLICATE_MASK = 0x00000140,       // the flags Duplicate copies
};

// The sound environment's parameters (SetEnvironmentParam). Indices 0 - 19 hold a float in 0 - 1
// except the milliseconds (ints) noted; 20 and 21 are vectors of length at most 1. The meanings
// follow the EAX 3 listener properties whose defaults, normalized to their ranges, are the
// sector's defaults (0x1004c690).
enum E_I3D_SECTOR_ENV : int {
    I3DENV_ROOM = 0,
    I3DENV_ROOM_HF = 1,
    I3DENV_DECAY_TIME = 2,                  // ms, 100 - 20000
    I3DENV_DECAY_HF_RATIO = 3,
    I3DENV_REFLECTIONS = 4,
    I3DENV_REFLECTIONS_DELAY = 5,           // ms, 0 - 300
    I3DENV_REVERB = 6,
    I3DENV_REVERB_DELAY = 7,                // ms, 0 - 100
    I3DENV_DIFFUSION = 8,
    I3DENV_FADE_TIME = 9,                   // ms (an int; default 500): the renderer blends the listener into it this long
    I3DENV_DECAY_LF_RATIO = 10,
    I3DENV_UNKNOWN11 = 11,
    I3DENV_ECHO_TIME = 12,                  // ms, 75 - 250
    I3DENV_ECHO_DEPTH = 13,
    I3DENV_MODULATION_TIME = 14,            // ms, 39 - 4000
    I3DENV_MODULATION_DEPTH = 15,
    I3DENV_ROOM_ROLLOFF = 16,
    I3DENV_AIR_ABSORPTION_HF = 17,
    I3DENV_HF_REFERENCE = 18,
    I3DENV_LF_REFERENCE = 19,
    I3DENV_REFLECTIONS_PAN = 20,            // S_vector4 *
    I3DENV_REVERB_PAN = 21,                 // S_vector4 *
    I3DENV_COUNT = 22,
};

// A sound environment (0x70 bytes; the sector's and the scene's).
struct I3D_environment {
    uint32_t params[20];                    // E_I3D_SECTOR_ENV 0 - 19 (floats / ints)
    S_vector4 reflectionsPan;               // I3DENV_REFLECTIONS_PAN
    S_vector4 reverbPan;                    // I3DENV_REVERB_PAN

    void SetDefaults();                     // 0x1004c690
};
static_assert(sizeof(I3D_environment) == 0x70, "I3D_environment layout");

// A triangle of the sector's hull (vertex indices).
class I3D_sector : public I3D_frame {
public:
    // 0x34 0x1004cd10: "sector <name>" (at most 0x40 characters of the name).
    I3D_RESULT __stdcall SetName(const char *name) override;
    // 0x4C 0x1004df60: also the geometry, portals, box and environment id of another sector.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x1004c9b0 / 0x1004ca80
    void Init() override;
    void Uninit() override;
    // 0x68 0x1004d1d0: the hull planes and portals follow the frame; a sector without a scene
    // takes that of the sector it is in.
    void MatrixChanged() override;
    // 0x70 0x1004e250: the frame, the 0x30 bytes from sectorFlags on, vertices, faces, portals.
    void Load() override;
    // 0x74 / 0x78 0x1004ceb0 / 0x1004d040: a light shines in the sector (each side's list).
    virtual void __stdcall AddLight(I3D_light *light);
    virtual I3D_RESULT __stdcall RemoveLight(I3D_light *light);
    // 0x7C / 0x80 0x1004dcb0 / 0x1004de20: a sound plays in the sector (each side's list).
    virtual void __stdcall AddSound(I3D_sound *sound);
    virtual I3D_RESULT __stdcall RemoveSound(I3D_sound *sound);
    // 0x84 0x1004d8c0: E_I3D_SECTOR_ENV `index`; `value` is the float's / int's bits or, for the
    // vectors, their address. The values are clamped to their ranges.
    virtual I3D_RESULT __stdcall SetEnvironmentParam(int index, uintptr_t value);
    // 0x88 0x1004dc70: the parameter's bits (the vectors: their address).
    virtual uintptr_t __stdcall GetEnvironmentParam(int index);
    // 0x8C 0x1004d6f0: `pos` (world) in the frame's bound and on the inner side of every plane.
    virtual bool __stdcall IsInside(const S_vector &pos);
    // 0x90 0x1004d7c0: the distance of an inside `pos` to the nearest plane (1e16 without planes),
    // -1e16 outside.
    virtual float __stdcall InsideDistance(const S_vector &pos);
    // 0x94 0x1004e510: the environment id (the sectors' preset, for the application).
    virtual void __stdcall SetEnvironment(int id);

    // 0x1004cd40: the scene ring (I3D_scene::firstSector); the portals and the occluder follow.
    void SetScene(I3D_scene *newScene);
    // 0x1004cb90: the box (the bound becomes stale).
    void SetBox(const I3D_bbox &newBox);
    // 0x1004d580: makes the occluder the sector asked for (SCTF_MAKE_OCCLUDER), then MatrixChanged.
    void MakeReady();

    // The inverse of the sector's world matrix, brought up to date.
    const S_matrix &GetInvMatrix()
    {
        if (!(sectorFlags & SCTF_INVMATRIX_VALID)) {
            sectorFlags |= SCTF_INVMATRIX_VALID;
            invWorldMatrix.Inverse(GetMatrix());
        }
        return invWorldMatrix;
    }

    // 0x1004cbc0: frees the vertices, faces, planes and portals; the incoming portals lose it.
    void FreeGeometry();

private:
    // 0x1004d270: the planes of the faces in world space, equal ones (to 0.001) once.
    void ComputePlanes();

public:
    S_matrix invWorldMatrix;                // +0x120 SCTF_INVMATRIX_VALID
    uint32_t sectorFlags;                   // +0x160 SCTF_*  (Load reads +0x160 - +0x18F)
    int environmentId;                      // +0x164 SetEnvironment
    int numVertices;                        // +0x168
    int numFaces;                           // +0x16C
    I3D_bbox box;                           // +0x170 the sector's space (local)
    S_vector4 *vertices;                    // +0x190 local (16-byte aligned block)
    I3D_triface *faces;                     // +0x194
    int lightChanges;                       // +0x198 counts the changes of the lights shining in it
    uint32_t ambientColor;                  // +0x19C (renderer) its ambient lights (SetMode bit 1), 0x00RRGGBB
    uint32_t ambientColor2;                 // +0x1A0 (renderer) the ambient lights with SetMode bit 0x10
    ls3d::C_vector<I3D_light *> lights;     // +0x1A4 AddLight
    int fogLightCount;                      // +0x1B4 lights of the fog types among them
    ls3d::C_vector<I3D_sound *> sounds;     // +0x1B8 AddSound
    uint32_t unknown1C8[2];                 // +0x1C8
    I3D_environment environment;            // +0x1D0 SetEnvironmentParam
    float environmentBlend;                 // +0x240 (renderer) 0 - 1 from the previous sector's
    int environmentTime;                    // +0x244 (renderer)
    I3D_light *fogLight;                    // +0x248 the fog light that is on, if any
    ls3d::C_vector<I3D_portal *> portals;   // +0x24C its own
    ls3d::C_vector<I3D_portal *> incomingPortals;  // +0x25C of other sectors, leading into it
    I3D_scene *scene;                       // +0x26C the scene the sector belongs to
    int numPlanes;                          // +0x270
    S_plane *planes;                        // +0x274 world space, the inside behind (16-byte aligned)
    I3D_sector *nextInScene;                // +0x278 scene ring (null: not in the ring)
    I3D_sector *prevInScene;                // +0x27C
    I3D_occluder *occluders;                // +0x280 (renderer) the occluders in it (RebuildOccluderLists)
    I3D_occluder *occluder;                 // +0x284 MakeReady's
    I3D_visual *renderVisuals;              // +0x288 (renderer) its visuals drawn this frame
    uint32_t padding28C;
};
static_assert(sizeof(I3D_sector) == 0x290, "I3D_sector layout");
static_assert(offsetof(I3D_sector, sectorFlags) == 0x160, "I3D_sector layout");
static_assert(offsetof(I3D_sector, box) == 0x170, "I3D_sector layout");
static_assert(offsetof(I3D_sector, lightChanges) == 0x198, "I3D_sector layout");
static_assert(offsetof(I3D_sector, lights) == 0x1A4, "I3D_sector layout");
static_assert(offsetof(I3D_sector, sounds) == 0x1B8, "I3D_sector layout");
static_assert(offsetof(I3D_sector, environment) == 0x1D0, "I3D_sector layout");
static_assert(offsetof(I3D_sector, fogLight) == 0x248, "I3D_sector layout");
static_assert(offsetof(I3D_sector, portals) == 0x24C, "I3D_sector layout");
static_assert(offsetof(I3D_sector, scene) == 0x26C, "I3D_sector layout");
static_assert(offsetof(I3D_sector, occluder) == 0x284, "I3D_sector layout");

namespace ls3d::i3d {

// Counts the sectors' changes of scene, made and gone (0x100bf354).
extern int g_sectorChanges;
// The renderer's sound environment: the camera's sector and the one it blends from (0x100d1284 /
// 0x100d1280); a sector going away is forgotten.
extern I3D_sector *g_envSector;
extern I3D_sector *g_envPrevSector;

}  // namespace ls3d::i3d
