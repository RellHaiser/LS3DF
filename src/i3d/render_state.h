// The renderer's state shared by the render passes and the visuals' prepare / draw slots
// (I3D_driver::Render, render.cpp; the visuals' slots 0x94 / 0x98).
//
// A frame is rendered sector by sector: the portal pass (portal_render.cpp) chains each sector's
// visible visuals (I3D_sector::renderVisuals), each visual's prepare slot (0x94) queues its
// primitives (S_renderItem), the queue is sorted by key and each item drawn by its visual's draw
// slot (0x98): the opaque ones by key (texture), then the blended ones back to front.
#pragma once

#include <cstdint>

#include "core/c_vector.h"
#include "math/ls3d_math.h"

class I3D_area;
class I3D_camera;
class I3D_scene;
class I3D_sector;
class I3D_sound;
class I3D_visual;
class ITexture;

namespace ls3d::i3d {

// A primitive a visual queued (0x1C bytes).
struct S_renderItem {
    I3D_visual *visual;                 // +0x00
    uint32_t sortKey;                   // +0x04 RIKEY_* (Render sorts by it)
    uint32_t data08;                    // +0x08 (the visual's)
    void *data0C;                       // +0x0C (object visuals: the face group)
    I3D_sector *sector;                 // +0x10 the sector it is drawn in (its fog light)
    ITexture *texture;                  // +0x14 (object visuals: the material's textures)
    ITexture *texture2;                 // +0x18
};
static_assert(sizeof(S_renderItem) == 0x1C, "S_renderItem layout");

// Sort key bits: opaque items carry their texture id (0x7FFF), the color-keyed ones with
// RIKEY_COLORKEY after the plain ones; blended items RIKEY_BLENDED and their view depth (30 bits),
// drawn from the highest key down.
enum : uint32_t {
    RIKEY_TEXTURE_MASK = 0x00007FFF,
    RIKEY_COLORKEY = 0x40000000,
    RIKEY_BLENDED = 0x80000000,
    RIKEY_DEPTH_MASK = 0x3FFFFFFF,
};

// A sort key with the index of its item (8 bytes, radix sorted).
struct S_sortKey {
    uint32_t key;                       // the alpha sort reuses it as the item's cache slot
    int index;
};

// The current pass (0x100d11e0 - 0x100d1223; made by 0x100636f0, freed by 0x1005eb60).
struct S_renderContext {
    S_vector4 lodPosition;              // +0x00 0x100d11e0 the point LOD distances are measured from
    uint32_t unknown10;                 // +0x10 (0)
    I3D_camera *camera;                 // +0x14 0x100d11f4
    I3D_sector *sector;                 // +0x18 0x100d11f8 the sector being drawn (lights, fog)
    I3D_scene *scene;                   // +0x1C 0x100d11fc
    C_vector<S_renderItem> items;       // +0x20 0x100d1200 (grow 0x400)
    S_sortKey *keys;                    // +0x30 0x100d1210
    int keyCapacity;                    // +0x34
    int numKeys;                        // +0x38 0x100d1218
    int numOpaque;                      // +0x3C 0x100d121c the items with keys below RIKEY_BLENDED
    int numBlended;                     // +0x40 0x100d1220

    S_renderContext() : unknown10(0), camera(nullptr), sector(nullptr), scene(nullptr),
        items(0x400), keys(nullptr), keyCapacity(0), numKeys(0), numOpaque(0), numBlended(0) {}
    ~S_renderContext()                  // 0x1005eb60
    {
        ::operator delete(keys);
        keys = nullptr;
        keyCapacity = 0;
        numKeys = 0;
    }
    // Empties the queue (keeps the storage).
    void ResetQueue()
    {
        numOpaque = 0;
        numBlended = 0;
        items.Clear();
        numKeys = 0;
    }
    // Room for `capacity` keys, the old ones copied (0x1003eb90; false if out of memory).
    bool GrowKeys(int capacity);
};
extern S_renderContext g_rc;

// Sorts keys by `key`, ascending: four 8-bit radix passes, stable (0x1005e770, __fastcall;
// render.cpp).
void RadixSort(S_sortKey *keys, int count);

// The camera of the frame being rendered (0x100d127c).
extern I3D_camera *g_renderCamera;
// The scene being rendered (0x100d1278; null outside Render).
extern I3D_scene *g_renderScene;
// Counts the rendered frames (0x100adf34): a frame drawn this time has renderStamp equal to it.
extern int g_renderStamp;
// The scene's stamp before this frame's (0x100be170; written, never read).
extern int g_prevRenderStamp;

// Projection scales of the current pass: (pi / 2) / fov (0x100d1230), its square (0x100d1254),
// and the LOD distance scale (0x100d1234: driver state DRVSTATE_LOD_SCALE x the first).
extern float g_fovScale;
extern float g_fovScaleSq;
extern float g_lodScale;
// The view x projection matrix of the current pass (0x100d1250: one of the camera's).
extern const S_matrix *g_viewProjection;
// The lens flares' fov factor (0x100d1264; Render, before drawing them).
extern float g_lensFlareScale;

// Render's state flags.
extern bool g_rendering;                // 0x100d1299 inside Render (not reentrant)
extern bool g_inRenderCallback;         // 0x100d1298 the scene's render callback runs
extern bool g_cameraChanged;            // 0x100d1260 the camera moved or changed sector
extern bool g_forceCameraChanged;       // 0x100a7258 (initially set; SetState sets it)
extern bool g_mainPass;                 // 0x100d1258 the main pass draws (the weather goes with it)

// The areas the camera is inside (0x100d1268, grow 0x20) and the sounds that leave with an area
// (0x100d11c0, grow 0x20).
extern C_vector<I3D_area *> g_activeAreas;
extern C_vector<I3D_sound *> g_areaSoundsLeaving;

// The blended items' sort (state DRVSTATE_SORT_BLENDED): a cache entry per item (0x80 bytes,
// 16-byte aligned array 0x100d129c of g_alphaCacheSize 0x100d12a0), handed out by g_alphaCacheNext
// (0x100d1170); g_alphaCompareMode (0x100d125c): 2 the depth overlap test only, 0 the whole test,
// 1 (never set) the depth centers.
struct S_alphaCacheEntry {
    S_vector4 points[5];                // +0x00 the first corners in view space; [3]: the plane normal
    S_vector4 boxMin;                   // +0x50 view space x, y; z: the depth (frustum plane 0)
    S_vector4 boxMax;                   // +0x60
    uint32_t flags;                     // +0x70 ACF_*
    uint32_t padding74[3];
};
static_assert(sizeof(S_alphaCacheEntry) == 0x80, "S_alphaCacheEntry layout");
enum : uint32_t {
    ACF_BOX = 0x02,                     // boxMin / boxMax and points
    ACF_PLANE = 0x04,                   // points[3] is the plane normal
    ACF_DEPTH = 0x08,                   // boxMin.z / boxMax.z
    ACF_EDGE01 = 0x10,                  // the shortest box edge: corner 0 - 1
    ACF_EDGE02 = 0x20,                  // ... 0 - 2
    ACF_EDGE04 = 0x40,                  // ... 0 - 4
};
extern S_alphaCacheEntry *g_alphaCache;
extern int g_alphaCacheSize;
extern int g_alphaCacheNext;
extern int g_alphaCompareMode;

// The queue emptied and its storage freed (the scene's Close).
void ResetRenderLists();

}  // namespace ls3d::i3d
