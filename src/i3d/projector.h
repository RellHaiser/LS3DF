// I3D_projector: the texture projector visual (PROJ; vtable 0x1009c1f0, 0x270 bytes; 0x10043730 -
// 0x100462b0).
//
// Projects its texture (or its material's) along the frame's z onto the receiving meshes in its
// volume (x, y -1 - 1, z 0 - 1 in the frame's space; orthogonal or perspective): the receivers'
// faces inside are clipped to the volume (proj_geometry.h) and drawn blended with texture
// coordinates from the volume position and a color faded by the mode over the depth. The geometry
// is made again when a receiver or the projector changes, by the renderer after the frame's
// passes (RenderProjectors), or once by Build.
#pragma once

#include "i3d/vb_manager.h"
#include "i3d/visual.h"

class I3D_material;
class I3D_sector;
class ITexture;

// I3D_projector::projFlags.
enum : uint32_t {
    PROJF_BUFFERED = 0x00000001,            // the geometry is in the vertex / index buffers
};

// A projected vertex (0x18 bytes).
struct S_projectedVertex {
    float x, y, z;                          // volume space
    uint32_t color;                         // ARGB
    float u, v;
};

class I3D_projector : public I3D_visual {
public:
    // 0x4C 0x10043960: the frame; of another projector its parameters, of a mesh visual its
    // first face group's material (of a lens none).
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x10043730
    void Uninit() override;                                                    // 0x64 0x100438f0
    // 0x68 0x10043d50: a moved projector looks for its sector again.
    void MatrixChanged() override;
    // 0x70 0x10046130: the frame, then orthogonal, the mode and the material.
    void Load() override;
    // 0x94 0x10043fe0: in range, the projected geometry queued (or the projector waits for
    // RenderProjectors to make it).
    void Prepare() override;
    // 0x98 0x10045dd0: the projected faces, blended (additive in modes 0, 4 and 8).
    void Draw(const ls3d::i3d::S_renderItem *item) override;

    virtual void __stdcall SetTexture(ITexture *newTexture);                  // 0x9C 0x10043af0
    virtual void __stdcall SetMaterial(I3D_material *newMaterial);            // 0xA0 0x10043b20
    virtual void __stdcall SetOrthogonal(bool on);                           // 0xA4 0x10043b50
    virtual void __stdcall SetIntensity(float value);                        // 0xA8 0x10043c50 (at least 0)
    virtual void __stdcall SetOpacity(float value);                          // 0xAC 0x10043ca0 (0 - 1)
    virtual void __stdcall SetColor(const S_vector4 &newColor);               // 0xB0 0x10043b70 (0 - 1)
    virtual void __stdcall SetMode(int newMode);                             // 0xB4 0x10043d10
    virtual void __stdcall SetBuffered(uint32_t value);                      // 0xB8 0x10043d30
    // 0xBC 0x10045ab0: the geometry made now from the receivers below `root` (null: the scene's
    // primary sector, with flag 2 also its omni sector; flag 8: the projector's sector only), in
    // the buffers. Flags: 4 models are skipped, 8 other sectors are, 0x10 children are not walked.
    virtual void __stdcall Build(uint32_t flags, I3D_frame *root);

    // 0x100441c0: the receivers' projected geometry into the projector's vertices and faces.
    void BuildGeometry();
    // 0x10045860: the receivers from the frame's render queue; the geometry made again when they
    // or the projector changed, then queued.
    void Rebuild();
    // 0x10043d80: the queue items (opaque and blended faces).
    void QueueItems();

    bool orthogonal;                        // +0x1E0 (1)
    uint8_t padding1E1[3];
    float range;                            // +0x1E4 (100) drawn within range x the projection scale
    float intensity;                        // +0x1E8 (1)
    uint32_t unknown1EC;                    // +0x1EC
    S_vector4 color;                        // +0x1F0 (1, 1, 1)
    uint32_t buffered;                      // +0x200 0: the geometry in system memory (Build: 3)
    ITexture *texture;                      // +0x204 (a reference)
    I3D_material *material;                 // +0x208 (a reference; its texture when none)
    int mode;                               // +0x20C how the color fades over the depth (0, 4, 5, 8, 9)
    float opacity;                          // +0x210 (0.117) faces turned away are culled more the lower it is
    uint32_t projFlags;                     // +0x214 PROJF_*
    S_projectedVertex *vertices;            // +0x218 system memory
    int vertexCapacity;                     // +0x21C
    int numVertices;                        // +0x220
    uint16_t *faces;                        // +0x224 system memory (opaque, then blended)
    ls3d::S_VBAlloc vb;                     // +0x228
    ls3d::S_IBAlloc ib;                     // +0x234
    int faceCapacity;                       // +0x240
    int numOpaque;                          // +0x244
    int numBlended;                         // +0x248
    uint32_t unknown24C;
    S_vector4 blendedCenter;                // +0x250 world space (the blended item's depth)
    int buildStamp;                         // +0x260 (-1) changeCount when the geometry was made
    I3D_sector *sector;                     // +0x264 the sector it is in (found when null)
    int numReceivers;                       // +0x268 ... and the receivers then
    int receiverSum;                        // +0x26C ... and their change counts' sum
};
static_assert(sizeof(I3D_projector) == 0x270, "I3D_projector layout");
static_assert(offsetof(I3D_projector, texture) == 0x204, "I3D_projector layout");
static_assert(offsetof(I3D_projector, vb) == 0x228, "I3D_projector layout");
static_assert(offsetof(I3D_projector, sector) == 0x264, "I3D_projector layout");
