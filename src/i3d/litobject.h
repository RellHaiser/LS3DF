// I3D_lit_object: the lightmapped mesh visual (LMAP; vtable 0x1009c6d8, 0x370 bytes; 0x100232d0 -
// 0x1002b030).
//
// A mesh visual whose static lighting is baked per mesh level: as a color per vertex
// (LMSET_VERTEX_COLORS: the level's vertices with the colors, drawn unlit), or as lightmap patches
// (LMSET_TEXTURES): the level's faces, by face group, are covered by patches (S_lightmapPart)
// packed into the shared lightmap textures (C_lightmap, lightmap.h); the level gets vertices of
// its own (position, normal, lightmap uv, uv: a vertex for each vertex and patch it is in) and
// faces into them, drawn with the lightmap texture in stage 0 and the material's in stage 1, lit
// by the sector's lights. Six sets of lightmaps can be kept, one drawn (currentSet); they come
// from the scene's lightmap file (LoadLightmaps) or are computed from the scene's lights
// (Construct). A set not loaded draws as an object visual.
#pragma once

#include <cstring>

#include "i3d/lightmap.h"
#include "i3d/object.h"
#include "i3d/vb_manager.h"

class I3D_mesh;
class I3D_mesh_level;

// S_lightmapSet::flags.
enum : uint32_t {
    LMSET_VERTEX_COLORS = 0x01,             // a color per vertex (drawn unlit, no lightmap texture)
    LMSET_TEXTURES = 0x02,                  // lightmap patches in the atlas textures
    LMSET_VERTEX_COUNT = 0x04,              // (file) the vertex colors' count is given
};

// I3D_lit_object::Construct's flags.
enum : uint32_t {
    LMCONS_SHADOWS = 0x01,                  // lights with LIGHTMODE_LMSHADOW are blocked by the faces between
    LMCONS_BLUR = 0x04,                     // the patches blurred (1 2 1 kernel)
    LMCONS_ONE_PATCH = 0x10,                // a level's faces all in one patch, in the plane of the given normal
    LMCONS_TEST_PATTERN = 0x20,             // the patches not lit but drawn as a test pattern
};

// Construct's progress callback (the scene loader's message 8): `state` 0 started; 5 grouping faces
// into patches (`param`: the patch's faces << 16 | the faces left, both 16 bits; 1 << 16 for a
// patch's first face); 6 fitting a patch's rectangle (`param`: vertices done << 16 | the patch's
// vertices); 1 lighting the texels (`param`: the progress 0 - 1, a float's bits); 3 filling the
// texels no sample lit; 4 blurring.
typedef uint32_t __stdcall I3D_LIGHTMAP_PROGRESS(uint32_t message, uint32_t param, uint32_t state, void *context);

namespace ls3d::i3d {

// A lightmap vertex: the level's vertex in a patch (4 bytes).
struct S_lightmapVertex {
    uint16_t vertex;
    uint16_t part;
};

struct S_lightmapUV {
    float u, v;
};

// One mesh level's lightmaps (0x48 bytes).
struct S_lightmapLevel {
    S_lightmapLevel() { std::memset(this, 0, sizeof *this); }
    // 0x1002a530 (with operator delete: 0x100289b0)
    ~S_lightmapLevel() { Free(); }
    S_lightmapLevel(const S_lightmapLevel &) = delete;
    S_lightmapLevel &operator=(const S_lightmapLevel &) = delete;

    void Free();
    // 0x1002aac0: the patches into the atlas (C_lightmap), the vertices' uv into it; unless
    // `keepFaces`, the faces into the lightmap vertices (each face's corner the vertex of its
    // patch).
    bool Build(I3D_mesh_level *level, bool keepFaces, const I3D_frame *owner);
    // 0x1002a630: the vertex buffer made (when dirty, again) and filled from the level's vertices:
    // `flags` LMSET_VERTEX_COLORS the level's own layout with the colors, else the lightmap
    // vertices.
    bool FillVertices(I3D_mesh_level *level, I3D_mesh *mesh, uint32_t flags);
    // 0x1002aaa0: a face group's faces' patches (kept).
    void SetFaceParts(uint16_t *faceParts, int faceGroup);

    int numVertices;                        // +0x00 the lightmap vertices (vertex colors: the level's)
    S_lightmapUV *uvs;                      // +0x04 in its patch (0 - 1), after Build in the texture
    void *unknown08;                        // +0x08 (freed; nothing sets it)
    S_lightmapVertex *vertexMap;            // +0x0C
    uint32_t *colors;                       // +0x10 LMSET_VERTEX_COLORS: per vertex (ARGB)
    S_lightmapPart *parts;                  // +0x14
    int numParts;                           // +0x18
    C_lightmap *lightmap;                   // +0x1C
    int numFaceGroups;                      // +0x20
    uint16_t **faceParts;                   // +0x24 per face group: each face's patch
    S_IBAlloc faces;                        // +0x28 all face groups' faces into the lightmap vertices
    S_VBAlloc vertices;                     // +0x34
    int textureBytes;                       // +0x40 the texture memory the patches take
    bool dirty;                             // +0x44 the vertices made again when next drawn
    uint8_t padding45[3];
};
static_assert(sizeof(S_lightmapLevel) == 0x48, "S_lightmapLevel layout");

// A set of lightmaps (0x38 bytes).
struct S_lightmapSet {
    bool loaded;                            // +0x00 drawn with (else as an object visual)
    uint8_t padding01[3];
    uint32_t flags;                         // +0x04 LMSET_* (LMSET_TEXTURES)
    float resolution;                       // +0x08 (5) Construct: texels per unit
    float smoothAngle;                      // +0x0C (45) Construct: degrees between faces sharing a patch
    S_lightmapLevel *levels[10];            // +0x10 per mesh level
};
static_assert(sizeof(S_lightmapSet) == 0x38, "S_lightmapSet layout");

// The light at a point for the lightmaps (0x10064b90; lightmap_build.cpp): of the light sector's
// lights that shine into lightmaps (LIGHTMODE_LIGHTMAP), the ambient and the diffuse at `pos`
// facing `normal` (point and spot lights to their range, with their attenuation and cone; the
// diffuse doubled), through `material`'s colours (IGraph's default material without MATF_COLORS).
// With a scene and LMCONS_SHADOWS, a light with LIGHTMODE_LMSHADOW is traced from the point
// (0.0075 off the surface) through the scene's faces, which tint it (TransmitFilter); above level 0
// `self` is tested at `level`. When every traced ray ended on a face turned away from it (the point
// is inside something) `*blocked` is set and the colour is magenta.
void LightmapLightAt(S_vector &color, const S_vector &pos, const S_vector &normal, const I3D_material *material,
                     I3D_scene *scene, uint32_t flags, bool *blocked, I3D_frame *self, int level);

}  // namespace ls3d::i3d

class I3D_lit_object : public I3D_object {
public:
    // 0x4C 0x10029340: the current set freed; the frame, and of a mesh visual (or a lens: no
    // mesh) the mesh (the lightmaps kept), light level, alpha and bounds; of another lightmapped
    // visual also its sets' flags and parameters (not the lightmaps).
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x100232d0
    void Uninit() override;                                                    // 0x64 0x10023320
    // 0x94 0x10029700: the current set's level for the camera distance: its vertices (made when
    // dirty), lights, and an item per face group (the lightmap texture and the material's).
    void Prepare() override;
    // 0x98 0x10029c90
    void Draw(const ls3d::i3d::S_renderItem *item) override;
    // 0x9C 0x10029500: the mesh; the current set's lightmaps freed unless `keep`.
    I3D_RESULT __stdcall SetMesh(I3D_mesh *newMesh, uint32_t keep) override;
    // 0xAC / 0xB0 0x10029680 / 0x10029630: every set's level / levels made again when drawn.
    void __stdcall RefreshLevel(int level) override;
    void __stdcall RefreshLevels() override;

    // 0xB4 0x100237c0 (lightmap_build.cpp): the current set computed from the lights of the
    // object's sector, at `resolution` texels per unit (the set keeps it). `planeNormal`: the
    // patches' plane with LMCONS_ONE_PATCH; `scene` traces the shadows of LMCONS_SHADOWS.
    virtual I3D_RESULT __stdcall Construct(I3D_scene *scene, const S_vector *planeNormal, float resolution,
                                           uint32_t flags, I3D_LIGHTMAP_PROGRESS *progress, void *context);
    // 0xB8 0x10028a00: the current set's lightmaps freed (not loaded).
    virtual I3D_RESULT __stdcall FreeLightmaps(int unused);
    // 0xBC 0x10028a60: the sets in `file` (a dta file handle; the current set freed first).
    virtual I3D_RESULT __stdcall LoadLightmaps(int file);
    // 0xC0 0x100292f0: (not in a final build) I3DERR_GENERIC.
    virtual I3D_RESULT __stdcall SaveLightmaps(int file);
    // 0xC4 0x10029310: the current set's lightmaps freed, its flags set.
    virtual I3D_RESULT __stdcall SetSetFlags(uint32_t flags);
    // 0xC8 0x100233c0: the set drawn (the current one's levels made again when it is next).
    virtual void __stdcall SetCurrentSet(int set);
    // 0xCC 0x100296b0: the texture memory of every set's lightmaps.
    virtual int __stdcall GetTextureBytes();
    // 0xD0 / 0xD4 0x10023400 / 0x10023430: a set's level's patches.
    virtual int __stdcall GetNumParts(int level, int set);
    virtual ls3d::i3d::S_lightmapPart *__stdcall GetParts(int level, int set);
    // 0xD8 0x10023460: a set's level's patches written into the atlas texture again from their
    // pixels (32-bit lightmap textures only).
    virtual void __stdcall RewriteParts(int level, int set);
    // 0xDC 0x100236e0: a set's level's faces locked (I3D_OK; -1 without the level). (The
    // original never stores them in `faces`.)
    virtual I3D_RESULT __stdcall LockFaces(int level, int set, uint16_t **faces);

    ls3d::i3d::S_lightmapSet &CurrentSet() { return sets[currentSet]; }
    // A set's level (null when out of range).
    ls3d::i3d::S_lightmapLevel *LevelAt(int level, int set);

    int currentSet;                         // +0x210
    ls3d::i3d::S_lightmapSet sets[6];       // +0x214
    uint32_t unknown364[3];                 // +0x364
};
static_assert(sizeof(I3D_lit_object) == 0x370, "I3D_lit_object layout");
static_assert(offsetof(I3D_lit_object, sets) == 0x214, "I3D_lit_object layout");
