// Projected shadows: I3D_shadow (FRAME_SHADOW; vtable 0x1009bf30, 0x210 bytes; 0x1004e5d0 -
// 0x10050770) and its visual I3D_shadow_visual ('SHD'; vtable 0x1009c8a0, 0x230 bytes; 0x100507e0 -
// 0x10051d20).
//
// A shadow is linked into a model; its shadow file (.6ds: named parts of a simplified mesh) picks
// the model's frames whose names match the parts (the casters). Each frame the renderer finds the
// strongest light of the shadow's sector that casts shadows (SetMode bit 8), looks along it,
// rasterizes the casters' silhouette into an A4R4G4B4 alpha texture and moves the visual (a box
// along the light: the shadow volume); the visual projects that texture onto the meshes it meets.
#pragma once

#include "core/c_vector.h"
#include "i3d/vb_manager.h"
#include "i3d/visual.h"

class I3D_scene;
class I3D_sector;
class I3D_shadow_visual;
class ITexture;

namespace ls3d::i3d {
struct S_renderItem;
}

// I3D_shadow::shadowFlags.
enum : uint32_t {
    SHDF_RASTERIZED = 0x0001,               // the map was drawn this frame
    SHDF_LIGHT_CHANGED = 0x0002,            // the light direction changed
    SHDF_MOVED = 0x0004,                    // the casters moved
    SHDF_FADED = 0x0008,                    // the intensity changed
    SHDF_RECEIVERS_CHANGED = 0x0010,        // the meshes under the shadow changed
    SHDF_MAP_VALID = 0x0400,                // the map holds the casters
    SHDF_LIGHT_VALID = 0x0800,              // the light was looked up for the current light state
    SHDF_CASTERS_VALID = 0x1000,            // the casters' world vertices are current
    SHDF_BUILT = 0x2000,                    // the visual's geometry is built
    SHDF_FAILED = 0x8000,                   // no shadow (no light, texture failure)
};

// The shadow file: a mesh in named parts, each the vertices and faces cast by the frame of that
// name (refcounted, shared by Duplicate).
struct S_shadowPart {
    char *name;
    int numVertices;
    int numFaces;
};
struct S_shadowData {
    int refCount;                           // +0x00
    ls3d::C_vector<S_vector4> vertices;     // +0x04 (16-byte aligned blocks in the original; grow 8)
    ls3d::C_vector<uint16_t> indices;       // +0x14 (grow 0x40)
    ls3d::C_vector<S_shadowPart> parts;     // +0x24 (grow 10)

    S_shadowData() : refCount(1), vertices(8), indices(0x40), parts(10) {}
    // 0x10050770: the parts' names freed.
    ~S_shadowData();
};
static_assert(sizeof(S_shadowData) == 0x34, "S_shadowData layout");

// A caster: a frame and how many of the shadow's vertices it moves.
struct S_shadowCaster {
    I3D_frame *frame;                       // null once the frame went away
    int numVertices;
};

class I3D_shadow : public I3D_frame {
public:
    // 0x38 0x1004ef20: the casters are looked up in the new parent's hierarchy.
    I3D_RESULT __stdcall LinkTo(I3D_frame *parent, uint32_t flags) override;
    // 0x4C 0x1004e840: also the shadow file and settings of another shadow.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x1004e5d0
    void Uninit() override;                                                    // 0x64 0x1004e7a0
    // 0x68 0x1004ef80: without a scene, joins the scene of the sectors above it.
    void MatrixChanged() override;
    // 0x74 0x1004f120: the shadow file `fileName` with its extension replaced by ".6ds".
    virtual I3D_RESULT __stdcall Open(const char *fileName, uint32_t flags);
    // 0x78 0x1004f030: the file, the casters and the map freed.
    virtual void __stdcall Close();
    // 0x7C 0x1004f560: the map size (powers of 2 from 4, at most 128; both from `width`).
    virtual I3D_RESULT __stdcall SetSize(int width, int height);
    // 0x80 0x1004f340: the length of the shadow volume along the light (above 0).
    virtual void __stdcall SetDepth(float depth);
    // 0x84 0x1004f380: how far below the frame the volume may start (0 - 300).
    virtual I3D_RESULT __stdcall SetMaxHeight(float height);
    // 0x88 0x1004ef50: 0 dynamic, 1 static casters, 2 also a static light, 3 also static geometry
    // (kept in device buffers).
    virtual void __stdcall SetMode(int mode);
    virtual void __stdcall SetOpacity(float opacity);                          // 0x8C 0x1004f4c0 (0 - 1)
    // 0x90 0x1004f3f0: the shadow's color (x, y, z clamped to 0 - 1).
    virtual void __stdcall SetColor(const S_vector4 &color);
    virtual void __stdcall SetOrthogonal(bool on);                             // 0x94 0x1004f520

    // 0x1004e970: the casters looked up in `source`'s hierarchy (null: none; the scene is left).
    void SetSource(I3D_frame *source);

    bool orthogonal;                        // +0x120 (1) the volume is a box, not a pyramid
    uint8_t padding121[3];
    S_shadowData *data;                     // +0x124 the shadow file
    S_vector4 *localVertices;               // +0x128 the casters' parts (16-byte aligned, then the world ones)
    S_vector4 *worldVertices;               // +0x12C
    float (*points)[2];                     // +0x130 the vertices in light space (x, y)
    int numVertices;                        // +0x134
    uint16_t *faces;                        // +0x138 3 per face
    int numFaces;                           // +0x13C
    ITexture *texture;                      // +0x140 the map
    uint32_t unknown144[3];                 // +0x144
    I3D_bbox bounds;                        // +0x150 the casters in light space
    S_vector4 color;                        // +0x170
    I3D_scene *scene;                       // +0x180 the scene whose shadow list holds it
    I3D_sector *lightSector;                // +0x184 the sector above it
    S_shadowCaster *casters;                // +0x188
    int numCasters;                         // +0x18C
    I3D_shadow_visual *visual;              // +0x190
    int mapWidth;                           // +0x194 (64)
    int mapHeight;                          // +0x198 (64)
    float depth;                            // +0x19C (8)
    S_vector4 lightDir;                     // +0x1A0 from the light
    S_matrix lightMatrix;                   // +0x1B0 world to light space (rotation)
    int mode;                               // +0x1F0
    float intensity;                        // +0x1F4 the shadow's strength (0 - 1)
    float maxHeight;                        // +0x1F8 (1)
    float range;                            // +0x1FC (100) drawn this close to the camera (x the fov scale)
    int casterStamp;                        // +0x200 the casters' change counts when moved
    uint32_t shadowFlags;                   // +0x204 SHDF_*
    int lightStamp;                         // +0x208 the light sector's and the frame's changes when lit
    float opacity;                          // +0x20C (0.117)
};
static_assert(sizeof(I3D_shadow) == 0x210, "I3D_shadow layout");
static_assert(offsetof(I3D_shadow, data) == 0x124, "I3D_shadow layout");
static_assert(offsetof(I3D_shadow, bounds) == 0x150, "I3D_shadow layout");
static_assert(offsetof(I3D_shadow, lightMatrix) == 0x1B0, "I3D_shadow layout");
static_assert(offsetof(I3D_shadow, opacity) == 0x20C, "I3D_shadow layout");

class I3D_shadow_visual : public I3D_visual {
public:
    void Init() override;                                                      // 0x60 0x100507e0
    void Uninit() override;                                                    // 0x64 0x10050920
    // 0x94 0x100509c0: queued for the shadow pass (or its kept geometry queued at once).
    void Prepare() override;
    void Draw(const ls3d::i3d::S_renderItem *item) override;                  // 0x98 0x100519b0

    // 0x10050ac0: the geometry's items queued (blended, the first part first).
    void QueueItems();
    // 0x10050cf0: the meshes under the shadow projected into its volume.
    void Build();

    struct S_vertex {
        S_vector pos;
        uint32_t color;
        float u, v;
    };

    S_vector4 center;                       // +0x1E0 (renderer) the blended part's center (never set)
    I3D_shadow *owner;                      // +0x1F0
    S_vertex *vertices;                     // +0x1F4 (modes 0 - 2)
    int vertexCapacity;                     // +0x1F8
    int numVertices;                        // +0x1FC
    uint16_t *faceIndices;                  // +0x200 (modes 0 - 2)
    int faceCapacity;                       // +0x204
    int numOpaqueFaces;                     // +0x208
    int numBlendedFaces;                    // +0x20C
    ls3d::S_VBAlloc vb;                     // +0x210 (mode 3)
    ls3d::S_IBAlloc ib;                     // +0x21C (mode 3)
    int numReceivers;                       // +0x228 the meshes under it, last built
    int receiverStamp;                      // +0x22C their changes
};
static_assert(sizeof(I3D_shadow_visual) == 0x230, "I3D_shadow_visual layout");
static_assert(offsetof(I3D_shadow_visual, owner) == 0x1F0, "I3D_shadow_visual layout");
static_assert(offsetof(I3D_shadow_visual, vb) == 0x210, "I3D_shadow_visual layout");

namespace ls3d::i3d {

// Every shadow (0x100bf380).
extern C_vector<I3D_shadow *> g_shadows;

}  // namespace ls3d::i3d
