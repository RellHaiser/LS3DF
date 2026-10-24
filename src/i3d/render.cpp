// I3D_driver::Render (0x1005edc0) and the render core: the queue's sorts (0x1005e770, 0x10062620),
// RenderVisual (0x100632a0) and the device reset pass (0x100633b0, 0x10063560).
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "core/fpu.h"
#include "i3d/billboard.h"
#include "i3d/area.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/i3d_links.h"
#include "i3d/lens.h"
#include "i3d/light.h"
#include "i3d/mesh.h"
#include "i3d/mirror.h"
#include "i3d/object.h"
#include "i3d/occluder.h"
#include "i3d/render_helpers.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/sound.h"
#include "i3d/vb_manager.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "isnd/isnd.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ig::g_pDevice;
using ig::g_rs;

namespace {

// The scene's render callback (I3D_scene::SetRenderCallback): stage 0 before the scene is drawn,
// 1 after its visuals, 2 at the end (each only with its renderCallbackFlags bit 1 << stage).
using RenderCallback = void(__stdcall *)(int stage, uint32_t, uint32_t);

// ---------------------------------------------------------------- the radix sort (0x1005e770)

// Histograms of the four key bytes (0x100d0960, 0x100d0160, 0x100d0560, 0x100d0d60: counted
// together, each cleared when its pass runs) and the work buffer (0x100d1160, its size 0x100d1164).
int s_histogram[4][256];
uint8_t *s_sortBuffer;
uint32_t s_sortBufferSize;

// One pass: `from` scattered into `to` by byte `b` of the key. A pass whose byte is the same for
// every key (the first or the last bucket holds them all) is skipped. Returns whether it ran.
bool RadixPass(const S_sortKey *from, S_sortKey *to, int count, int b)
{
    int *h = s_histogram[b];
    if (h[0] == count) {
        h[0] = 0;
        return false;
    }
    if (h[255] == count) {
        h[255] = 0;
        return false;
    }
    int offset[256];
    int sum = 0;
    for (int i = 0; i < 256; ++i) {
        offset[i] = sum;
        sum += h[i];
        h[i] = 0;
    }
    for (int i = 0; i < count; ++i) {
        const int bucket = (from[i].key >> (b * 8)) & 0xff;
        to[offset[bucket]++] = from[i];
    }
    return true;
}

}  // namespace

// (Also the emitters' depth sort.)
void ls3d::i3d::RadixSort(S_sortKey *keys, int count)
{
    if (count == 0)
        return;
    for (int i = count - 1; i >= 0; --i) {
        const uint32_t k = keys[i].key;
        ++s_histogram[0][k & 0xff];
        ++s_histogram[2][(k >> 16) & 0xff];
        ++s_histogram[1][(k >> 8) & 0xff];
        ++s_histogram[3][k >> 24];
    }
    const uint32_t bytes = static_cast<uint32_t>(count) * 8;
    if (s_sortBufferSize < bytes) {
        ::operator delete(s_sortBuffer);
        s_sortBuffer = static_cast<uint8_t *>(::operator new(bytes));
        s_sortBufferSize = bytes;
    }
    // The data moves between the keys and the buffer with each pass that runs.
    S_sortKey *a = keys;
    S_sortKey *b = reinterpret_cast<S_sortKey *>(s_sortBuffer);
    for (int pass = 0; pass < 4; ++pass) {
        if (RadixPass(a, b, count, pass)) {
            S_sortKey *t = a;
            a = b;
            b = t;
        }
    }
    if (a != keys)
        std::memcpy(keys, a, bytes);
}

namespace {

// ---------------------------------------------------------------- the queue

// The keys of the queued items (from the last item), the key array first made as large as the
// item array.
void ReserveKeys()
{
    if (static_cast<uint32_t>(g_rc.keyCapacity) < static_cast<uint32_t>(g_rc.items.Capacity())) {
        const int capacity = g_rc.items.Capacity();
        if (g_rc.keys)
            ::operator delete(g_rc.keys);
        g_rc.keys = static_cast<S_sortKey *>(::operator new(sizeof(S_sortKey) * capacity, std::nothrow));
        if (g_rc.keys)
            g_rc.keyCapacity = capacity;
    }
}

void AddKey(uint32_t key, int index)
{
    if (g_rc.numKeys == g_rc.keyCapacity && !g_rc.GrowKeys(g_rc.keyCapacity * 2))
        return;
    g_rc.keys[g_rc.numKeys].key = key;
    g_rc.keys[g_rc.numKeys].index = index;
    ++g_rc.numKeys;
}

void BuildKeys()
{
    ReserveKeys();
    for (int i = g_rc.items.Size(); i != 0;) {
        --i;
        AddKey(g_rc.items[i].sortKey, i);
    }
}

// The point a visual's LOD and depth are measured at.
const S_vector4 &LodPoint(I3D_visual *v)
{
    if (v->visualFlags & VISF_LOD_AT_POSITION)
        return *reinterpret_cast<const S_vector4 *>(&v->GetMatrix()(3));
    if (!(v->visualFlags & VISF_WBOUND_VALID))
        v->UpdateWBoundProc();
    return *reinterpret_cast<const S_vector4 *>(&v->worldBound.center);
}

// The fog of an item's sector (its fog light; none with DRVSTATE_FOG off or VISF_NO_FOG).
void SetItemFog(const S_renderItem &item)
{
    I3D_light *fog = item.sector ? item.sector->fogLight : nullptr;
    if (!fog || !g_driverStates[DRVSTATE_FOG] || (item.visual->visualFlags & VISF_NO_FOG)) {
        SetFog(0, 0, 0.0f, 0.0f, 0);
        return;
    }
    auto color = [fog]() {
        const uint32_t r = static_cast<uint32_t>(Fistp(fog->color.x * 255.0f));
        const uint32_t g = static_cast<uint32_t>(Fistp(fog->color.y * 255.0f));
        const uint32_t b = static_cast<uint32_t>(Fistp(fog->color.z * 255.0f));
        return (((r | 0xffffff00u) << 8 | g) << 8) | b;
    };
    switch (fog->lightType) {
    case 5: {
        const float end = fog->d3dLight.Range * g_renderCamera->farRange;
        SetFog(1, 0, end * fog->d3dLight.Attenuation0, end, color());
        break;
    }
    case 6:
        SetFog(1, 1, fog->attenuation[0], 0.0f, color());
        break;
    case 7:
        SetFog(1, 2, fog->attenuation[0], 0.0f, color());
        break;
    default:
        break;
    }
}

void DrawItem(S_renderItem &item)
{
    SetItemFog(item);
    ig::SetZWrite(~item.visual->visualFlags >> 1 & 1);
    SetZBias(item.visual->visualFlags >> 7 & 2);
    item.visual->Draw(&item);
}

// The sorted queue drawn: the opaque items by key, the weather (main pass), the blended ones
// from the highest key down, the weather's second part.
// (The original has this inline five times; the blended loop there reads the item's sector
// without a null check.)
void DrawQueue()
{
    g_vbManager.UnlockAll();
    g_ibManager.UnlockAll();
    FlushParticles();
    g_pDevice->SetRenderState(D3DRS_LIGHTING, TRUE);
    for (int i = 0; i < g_rc.numOpaque; ++i)
        DrawItem(g_rc.items[g_rc.keys[i].index]);
    if (g_mainPass && !g_inRenderCallback)
        g_renderScene->DrawWeather();
    if (g_rc.numBlended != 0) {
        for (int i = g_rc.items.Size() - 1; i >= g_rc.numOpaque; --i)
            DrawItem(g_rc.items[g_rc.keys[i].index]);
    }
    if (g_mainPass)
        g_renderScene->DrawWeather();
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
}

// A sector's visuals of this frame queued.
void PrepareSector(I3D_sector *sector)
{
    g_rc.sector = sector == g_renderScene->nearSector ? g_renderCamera->sector : sector;
    UpdateSectorAmbient(sector);
    for (I3D_visual *v = sector->renderVisuals; v; v = v->nextRenderVisual) {
        if (v->renderStamp == g_renderStamp)
            RenderVisual(v);
    }
}

// ---------------------------------------------------------------- the blended items' sort

// The 8 corners of a visual's box (the billboard's own box through its matrix; objects,
// lightmaps and land patches their local box through the world matrix; the others their world
// box), pairs of (min z, max z) (0x10063080).
void VisualCorners(I3D_visual *v, S_vector4 out[8])
{
    const uint32_t fourCC = v->GetVisualType();
    auto box = [out](const I3D_bbox &b) {
        S_vector4 *p = out;
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                p->x = i == 0 ? b.max.x : b.min.x;
                p->y = j == 0 ? b.max.y : b.min.y;
                p->z = b.min.z;
                ++p;
                p->x = i == 0 ? b.max.x : b.min.x;
                p->y = j == 0 ? b.max.y : b.min.y;
                p->z = b.max.z;
                ++p;
            }
        }
    };
    if (fourCC == FourCC("BBRD")) {
        // The mesh's box (its 4-float corners) through the facing matrix.
        auto *bb = static_cast<I3D_billboard *>(v);
        const I3D_bbox &b = *reinterpret_cast<const I3D_bbox *>(&bb->mesh->bboxMin);
        box(b);
        const S_matrix &m = bb->matrix;
        for (int i = 0; i < 8; ++i)
            *reinterpret_cast<S_vector *>(&out[i]) *= m;
        return;
    }
    if (fourCC == FourCC("LNDP") || fourCC == FourCC("LMAP") || fourCC == FourCC("OBJ_")) {
        box(v->GetLocalBound().bbox);
        for (int i = 0; i < 8; ++i)
            *reinterpret_cast<S_vector *>(&out[i]) *= v->GetMatrix();
        return;
    }
    box(v->GetWorldBound());
}

// The order of two blended items (keys' cache slots assigned on first use): -1 `a` first, 1 `b`
// first, 0 (mode 2) apart in depth (0x10062620). Mode 2 compares the depth ranges along the view
// direction only; mode 0 then the view-space boxes and each one's plane (through its box's first
// corner, along its shortest edge pair) against the other's center.
int CompareBlended(S_sortKey *ka, S_sortKey *kb)
{
    S_renderItem *items[2] = { &g_rc.items[ka->index], &g_rc.items[kb->index] };
    if (ka->key == 0xffffffffu)
        ka->key = static_cast<uint32_t>(++g_alphaCacheNext);
    if (kb->key == 0xffffffffu)
        kb->key = static_cast<uint32_t>(++g_alphaCacheNext);
    const uint32_t slots[2] = { ka->key, kb->key };
    S_vector4 corners[2][8];
    bool haveCorners[2] = { false, false };
    S_vector4 boxMin[2] = {}, boxMax[2] = {};
    S_alphaCacheEntry *cache[2];

    const I3D_camera *cam = g_renderCamera;
    for (int i = 0; i < 2; ++i) {
        S_alphaCacheEntry &c = g_alphaCache[slots[i]];
        float lo, hi;
        if (!(c.flags & ACF_DEPTH)) {
            VisualCorners(items[i]->visual, corners[i]);
            haveCorners[i] = true;
            lo = 1e9f;
            hi = -1e9f;
            for (int k = 0; k < 8; ++k) {
                const S_vector4 &p = corners[i][k];
                const float d = ((p.z * cam->frustum[0].normal.z + p.y * cam->frustum[0].normal.y) +
                                 cam->frustumD[0]) + p.x * cam->frustum[0].normal.x;
                if (d < lo)
                    lo = d;
                if (d > hi)
                    hi = d;
            }
            c.boxMin.z = lo;
            c.boxMax.z = hi;
            c.flags |= ACF_DEPTH;
        } else {
            lo = c.boxMin.z;
            hi = c.boxMax.z;
        }
        boxMin[i].z = lo;
        boxMax[i].z = hi;
    }
    if (g_alphaCompareMode == 1) {
        // (Never used; b's center is computed as max / 2 + min.)
        if (boxMax[1].z * 0.5f + boxMin[1].z > (boxMax[0].z + boxMin[0].z) * 0.5f)
            return -1;
        return 1;
    }
    const bool overlap = (boxMin[0].z > boxMin[1].z && boxMin[0].z < boxMax[1].z) ||
        (boxMin[1].z > boxMin[0].z && boxMin[1].z < boxMax[0].z);
    if (!overlap) {
        if (g_alphaCompareMode == 2)
            return 0;
        return boxMin[0].z < boxMin[1].z ? -1 : 1;
    }
    if (g_alphaCompareMode == 2)
        return 1;

    for (int i = 0; i < 2; ++i) {
        S_alphaCacheEntry &c = g_alphaCache[slots[i]];
        if (!(c.flags & ACF_BOX)) {
            // (The original computes the corners here only when the depth pass computed none:
            // with one item's depth cached and the other's not, it read the stack's leftovers.)
            if (!haveCorners[i]) {
                VisualCorners(items[i]->visual, corners[i]);
                haveCorners[i] = true;
            }
            S_vector4 *p = corners[i];
            auto edge2 = [p](int k) {
                const float dx = p[k].x - p[0].x, dy = p[k].y - p[0].y, dz = p[k].z - p[0].z;
                return (dz * dz + dy * dy) + dx * dx;
            };
            const float e1 = edge2(1), e2 = edge2(2), e4 = edge2(4);
            if (e1 < e2 && e1 < e4)
                c.flags |= ACF_EDGE01;
            else if (e2 < e1 && e2 < e4)
                c.flags |= ACF_EDGE02;
            else if (e4 < e1 && e4 < e2)
                c.flags |= ACF_EDGE04;
            boxMin[i].x = 1e9f;
            boxMax[i].x = -1e9f;
            boxMin[i].y = 1e9f;
            boxMax[i].y = -1e9f;
            for (int k = 0; k < 8; ++k) {
                S_vector &q = *reinterpret_cast<S_vector *>(&p[k]);
                q *= g_renderCamera->viewMatrix;
                if (q.x < boxMin[i].x)
                    boxMin[i].x = q.x;
                if (q.x > boxMax[i].x)
                    boxMax[i].x = q.x;
                if (q.y < boxMin[i].y)
                    boxMin[i].y = q.y;
                if (q.y > boxMax[i].y)
                    boxMax[i].y = q.y;
            }
            std::memcpy(c.points, p, sizeof c.points);
            c.boxMin = boxMin[i];
            c.boxMax = boxMax[i];
            c.flags |= ACF_BOX;
        } else {
            boxMin[i] = c.boxMin;
            boxMax[i] = c.boxMax;
        }
        cache[i] = &c;
    }
    // In view space: any overlap of x or y (the last test compares b's max y where b's min y
    // belongs: the original's).
    const bool screenOverlap = (boxMin[0].x > boxMin[1].x && boxMin[0].x < boxMax[1].x) ||
        (boxMin[1].x > boxMin[0].x && boxMin[1].x < boxMax[0].x) ||
        (boxMin[0].y > boxMin[1].y && boxMin[0].y < boxMax[1].y) ||
        (boxMax[1].y > boxMin[0].y && boxMax[1].y < boxMax[0].y);
    if (!screenOverlap)
        return -1;

    // Each item's plane against the other's center: a's plane must not have b's center in front
    // (above 0), b's must not have a's behind.
    S_vector4 n = { 0.0f, 0.0f, 0.0f, 0.0f };   // (an edge test can fail on NaN: then the last n)
    for (int k = 0; k < 2; ++k) {
        const int o = (k - 1) & 1;
        const float ocx = (boxMax[o].x + boxMin[o].x) * 0.5f;
        const float ocy = (boxMax[o].y + boxMin[o].y) * 0.5f;
        const float ocz = (boxMax[o].z + boxMin[o].z) * 0.5f;
        const float cx = (boxMin[k].x + boxMax[k].x) * 0.5f;
        const float cy = (boxMin[k].y + boxMax[k].y) * 0.5f;
        const float cz = (boxMin[k].z + boxMax[k].z) * 0.5f;
        S_alphaCacheEntry &c = *cache[k];
        const S_vector4 *p = c.points;
        if (!(c.flags & ACF_PLANE)) {
            if (c.flags & (ACF_EDGE01 | ACF_EDGE02 | ACF_EDGE04)) {
                // u, v: the two longer edges from corner 0.
                int ui, vi;
                if (c.flags & ACF_EDGE01) {
                    ui = 4;
                    vi = 2;
                } else if (c.flags & ACF_EDGE02) {
                    ui = 1;
                    vi = 4;
                } else {
                    ui = 2;
                    vi = 1;
                }
                const S_vector u(p[ui].x - p[0].x, p[ui].y - p[0].y, p[ui].z - p[0].z);
                const S_vector w(p[vi].x - p[0].x, p[vi].y - p[0].y, p[vi].z - p[0].z);
                n.x = w.z * u.y - u.z * w.y;
                if (c.flags & ACF_EDGE01)
                    n.y = u.z * w.x - w.z * u.x;
                else if (c.flags & ACF_EDGE02)
                    n.y = w.x * u.z - u.x * w.z;
                else
                    n.y = u.z * w.x - u.x * w.z;
                n.z = u.x * w.y - w.x * u.y;
                if ((n.z * cz + n.y * cy) + n.x * cx < 0.0f) {
                    n.x = -n.x;
                    n.y = -n.y;
                    n.z = -n.z;
                }
            }
            c.points[3] = n;
            c.flags |= ACF_PLANE;
        } else {
            n = c.points[3];
        }
        const float d = (n.x * ocx + (n.z * ocz + n.y * ocy)) - ((n.z * p[0].z + n.y * p[0].y) + n.x * p[0].x);
        if (k == 0) {
            if (d > 0.0f)
                return -1;
        } else if (d < 0.0f) {
            return -1;
        }
    }
    return 1;
}

// The blended items (keys numOpaque .. numKeys) sorted by CompareBlended: from the end, each run
// of depth-overlapping neighbours is bubble sorted.
// (The original's bubble pass compares the run's pairs but swaps the pairs at the start of the
// blended keys instead, and makes one comparison fewer than the run has pairs; kept.)
bool SortBlended()
{
    if (g_alphaCacheSize < g_rc.numBlended) {
        g_alphaCacheSize = g_rc.numBlended + 100;
        if (g_alphaCacheSize < 300)
            g_alphaCacheSize = 300;
        AlignedFree(g_alphaCache);
        g_alphaCache = static_cast<S_alphaCacheEntry *>(
            AlignedAlloc(sizeof(S_alphaCacheEntry) * static_cast<size_t>(g_alphaCacheSize), 0x10));
        if (!g_alphaCache) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return false;
        }
    }
    S_sortKey *keys = g_rc.keys + g_rc.numOpaque;
    for (int i = 0; i < g_alphaCacheSize; ++i)
        g_alphaCache[i].flags = 0;
    for (int i = 0; i < g_rc.numBlended; ++i)
        keys[i].key = 0xffffffffu;
    g_alphaCacheNext = -1;
    int end = g_rc.numBlended - 1;
    while (end > 0) {
        g_alphaCompareMode = 2;
        while (end > 0 && CompareBlended(&keys[end - 1], &keys[end]) == 0)
            --end;
        int run = 0;
        for (int m = end; m > 0; --m, ++run) {
            if (CompareBlended(&keys[m - 1], &keys[m]) == 0)
                break;
        }
        const int start = end - run;
        g_alphaCompareMode = 0;
        for (int pass = run, n = run - 1; pass != 0; --pass, --n) {
            for (int j = 0; j < n; ++j) {
                if (CompareBlended(&keys[start + j], &keys[start + j + 1]) > 0) {
                    const S_sortKey t = keys[j + 1];
                    keys[j + 1] = keys[j];
                    keys[j] = t;
                }
            }
        }
        end = start;
    }
    return true;
}

// ---------------------------------------------------------------- the device reset pass

// The visuals of a hierarchy prepared once (each LOD level of an object), so their device
// buffers exist before the first frame (0x100633b0).
void PrepareHierarchy(I3D_frame *f)
{
    for (; f; f = f->nextSibling) {
        if (!(f->frameFlags & FRMFLAGS_ON))
            continue;
        if (f->type == FRAME_VISUAL) {
            I3D_visual *v = static_cast<I3D_visual *>(f);
            I3D_mesh *mesh = (v->visualFlags & VISF_OBJECT_PARAMS) ? static_cast<I3D_object *>(v)->mesh : nullptr;
            if (mesh) {
                const int levels = (v->visualFlags & VISF_TEXTURE_LOD) ? 1 : mesh->numLevels;
                float prev = 0.0f;
                for (int i = 0; i < levels; ++i) {
                    g_rc.lodPosition = LodPoint(v);
                    g_lodScale = 1.0f / v->worldScale;
                    // The LOD point moved to the middle of the level's distance range.
                    const float d = std::sqrt(std::sqrt(mesh->GetLevel(i)->lodDistance));
                    g_rc.lodPosition.z = (d + prev) * 0.5f + g_rc.lodPosition.z;
                    prev = d;
                    v->Prepare();
                }
            } else {
                g_rc.lodPosition = LodPoint(v);
                g_lodScale = 1.0f / v->worldScale;
                v->Prepare();
            }
            g_rc.items.Clear();
        }
        PrepareHierarchy(f->firstChild);
    }
}

}  // namespace

namespace ls3d::i3d {

// 0x100632a0
void RenderVisual(I3D_visual *visual)
{
    I3D_frame *owner = visual->ownerModel;
    if (owner && owner->type == FRAME_MODEL)
        owner->renderStamp = g_renderStamp;
    const uint32_t fourCC = visual->GetVisualType();
    if (fourCC == FourCC("PROJ")) {
        if (g_driverStates[DRVSTATE_PROJECTORS])
            visual->Prepare();
        return;
    }
    if (fourCC != FourCC("LENS")) {
        visual->Prepare();
        return;
    }
    I3D_lens *lens = static_cast<I3D_lens *>(visual);
    C_vector<I3D_frame *> &list = g_rc.scene->lensFlares;
    bool listed = false;
    for (int i = 0; i < list.Size(); ++i) {
        if (list[i] == lens) {
            listed = true;
            break;
        }
    }
    if (!listed) {
        ++lens->refCount;
        list.PushBack(lens);
        lens->testTime = 0;
        lens->updateTime = timeGetTime();
    }
    lens->lensFlags |= LENSF_SEEN;
}

}  // namespace ls3d::i3d

// 0x10063560
I3D_RESULT I3D_driver::ResetDevice(I3D_scene *scene)
{
    if (!scene) {
        g_vbManager.ReleaseDefaultPool();
        g_ibManager.ReleaseDefaultPool();
        return InitParticleBuffers();
    }
    I3D_frame *temp = CreateFrame(FRAME_SECTOR);
    g_rc.ResetQueue();
    g_rc.scene = scene;
    g_rc.camera = scene->activeCamera;
    g_rc.sector = static_cast<I3D_sector *>(temp);
    PrepareHierarchy(scene->primarySector->firstChild);
    PrepareHierarchy(scene->omniSector->firstChild);
    PrepareHierarchy(scene->backdropSector->firstChild);
    PrepareHierarchy(scene->nearSector->firstChild);
    temp->Release();
    g_rc.ResetQueue();
    return I3D_OK;
}

// 0x1005edc0
I3D_RESULT I3D_driver::Render(I3D_scene *scene, bool keepScene)
{
    if (!scene)
        return I3DERR_GENERIC;
    if (!g_driverInitialized)
        return I3DERR_NOTINITIALIZED;
    const uint16_t fpuStatus = X87StatusWord();
    if (fpuStatus & 0x80)
        dbgPrintf("!! FPU exception flag set on entry I3D_driver::Render()  status:0x%x", fpuStatus);
    if (g_rendering && !g_inRenderCallback) {
        dbgPrintf("!! Rendering is NOT reentrant   Render()");
        return I3DERR_GENERIC;
    }
    g_rendering = true;
    g_renderScene = scene;
    g_prevRenderStamp = scene->renderStamp;
    g_renderStamp = scene->renderStamp + 1;
    scene->renderStamp = g_renderStamp;
    g_renderCamera = scene->activeCamera;
    auto background = [scene]() {
        const uint32_t r = static_cast<uint32_t>(Fistp(scene->backgroundColor.x * 255.0f));
        const uint32_t g = static_cast<uint32_t>(Fistp(scene->backgroundColor.y * 255.0f));
        const uint32_t b = static_cast<uint32_t>(Fistp(scene->backgroundColor.z * 255.0f));
        return (((r | 0xffffff00u) << 8 | g) << 8) | b;
    };
    const DWORD clearDepth = (ig::g_caps & 0x10 | 8) >> 2;     // z (and stencil)
    if (!g_renderCamera) {
        DWORD clear = g_driverStates[DRVSTATE_CLEAR] != 0 ? D3DCLEAR_TARGET : 0;
        if (g_driverStates[DRVSTATE_CLEAR_Z])
            clear |= clearDepth;
        g_pDevice->Clear(0, nullptr, clear, background(), 1.0f, 0);
        if (!g_inRenderCallback)
            g_rendering = false;
        return I3DERR_GENERIC;
    }
    g_renderCamera->UpdateSector();
    scene->UpdateTrackedModels(g_renderCamera->sector);
    DWORD clear = g_inRenderCallback ? 0 : (g_driverStates[DRVSTATE_CLEAR] != 0 ? D3DCLEAR_TARGET : 0);
    if (g_driverStates[DRVSTATE_CLEAR_Z])
        clear |= clearDepth;
    RECT rc;
    rc.left = scene->viewportLeft;
    rc.top = scene->viewportTop;
    rc.right = scene->viewportRight - rc.left;
    rc.bottom = scene->viewportBottom - rc.top;
    if (!EqualRect(&rc, reinterpret_cast<const RECT *>(&ig::g_viewport))) {
        CopyRect(reinterpret_cast<RECT *>(&ig::g_viewport), &rc);
        if (g_pDevice->SetViewport(&ig::g_viewport) < 0) {
            if (!g_inRenderCallback)
                g_rendering = false;
            return I3DERR_NOTINITIALIZED;
        }
    }
    if (clear)
        g_pDevice->Clear(0, nullptr, clear, background(), 1.0f, 0);
    // (Returns with g_rendering still set: the original's.)
    if (!g_renderCamera->sector)
        return I3DERR_GENERIC;
    if (!g_inRenderCallback) {
        if (!keepScene)
            g_pIGraph->BeginScene();
        I3D_camera *cam = g_renderCamera;
        if (scene->renderCallbackFlags & 1) {
            g_inRenderCallback = true;
            reinterpret_cast<RenderCallback>(scene->renderCallback)(0, 0, 0);
            g_renderCamera = cam;
            g_inRenderCallback = false;
        }
    }
    I3D_camera *cam = g_renderCamera;
    {
        const uint32_t lodBits = g_driverStates[DRVSTATE_LOD_SCALE];
        float lod;
        std::memcpy(&lod, &lodBits, 4);
        g_fovScale = 1.57079637f / cam->fov;
        g_fovScaleSq = g_fovScale * g_fovScale;
        g_lodScale = lod * g_fovScale;
    }
    g_cameraChanged = cam->changeCount != cam->seenChangeCount ||
        cam->sector->changeCount != cam->seenSectorChangeCount || g_forceCameraChanged;
    g_forceCameraChanged = false;

    bool soundPass = !g_inRenderCallback;
    if (g_cameraChanged) {
        cam->seenChangeCount = cam->changeCount;
        cam->seenSectorChangeCount = cam->sector->changeCount;
        if (soundPass && scene->listener) {
            scene->listener->SetPosition(cam->GetMatrix()(3));
            scene->listener->SetOrientation(cam->GetMatrix()(2), cam->GetMatrix()(1));
            scene->listener->Apply();
        }
    }
    if (soundPass) {
        const DWORD now = timeGetTime();
        // The areas the camera is in; the sounds of those it is not in are to leave.
        if (scene->firstArea) {
            const S_vector4 camPos = *reinterpret_cast<const S_vector4 *>(&cam->GetMatrix()(3));
            I3D_area *area = scene->firstArea;
            for (int n = scene->areaCount; n != 0; --n, area = area->nextInScene) {
                if (!g_cameraChanged && area->changeCount == area->testedChangeCount)
                    continue;
                area->testedChangeCount = area->changeCount;
                if (area->IsInside(*reinterpret_cast<const S_vector *>(&camPos))) {
                    bool active = false;
                    for (int i = 0; i < g_activeAreas.Size(); ++i) {
                        if (g_activeAreas[i] == area) {
                            active = true;
                            break;
                        }
                    }
                    if (!active)
                        g_activeAreas.PushBack(area);
                } else {
                    g_activeAreas.RemoveSwap(area);
                    for (int i = area->sounds.Size(); i != 0; --i) {
                        I3D_sound *s = area->sounds[i - 1];
                        if (s->state != SNDST_OUT_OF_RANGE && !(s->soundFlags & SNDF_AREA_LEAVING))
                            g_areaSoundsLeaving.PushBack(s);
                    }
                }
            }
            // The leaving sounds fade over the fade time of the area the walk ended at (the ring
            // wraps: the first area).
            I3D_area *const ringEnd = area;
            for (int a = 0; a < g_activeAreas.Size(); ++a) {
                I3D_area *active = g_activeAreas[a];
                for (int i = active->sounds.Size(); i != 0; --i) {
                    I3D_sound *s = active->sounds[i - 1];
                    g_areaSoundsLeaving.RemoveSwap(s);
                    if (s->soundFlags & SNDF_AREA_ACTIVE)
                        continue;
                    if (s->state == SNDST_OUT_OF_RANGE)
                        s->state = SNDST_AUDIBLE;
                    s->soundFlags = (s->soundFlags & ~SNDF_AREA_LEAVING) | SNDF_AREA_ACTIVE;
                    s->areaFadeTime = now;
                    if (s->areaFade < 0.0f) {
                        s->areaFade = 0.0f;
                        s->areaFadeDuration = active->fadeTime;
                    }
                }
            }
            while (g_areaSoundsLeaving.Size() != 0) {
                I3D_sound *s = g_areaSoundsLeaving[g_areaSoundsLeaving.Size() - 1];
                g_areaSoundsLeaving.PopBack();
                s->areaFadeTime = now;
                if (s->soundFlags & SNDF_AREA_ACTIVE) {
                    s->soundFlags = (s->soundFlags & ~SNDF_AREA_ACTIVE) | SNDF_AREA_LEAVING;
                    if (!(s->areaFade < 0.0f))
                        continue;
                } else {
                    s->soundFlags |= SNDF_AREA_LEAVING;
                }
                s->areaFade = 1.0f;
                s->areaFadeDuration = ringEnd->fadeTime;
            }
            g_areaSoundsLeaving.Clear();
        }

        // The listener's environment: the camera's sector blends in, the previous one out; their
        // sounds follow the blend.
        I3D_sector *camSector = cam->sector;
        auto addFading = [this](I3D_sector *s) {
            for (int i = 0; i < envSectors.Size(); ++i)
                if (envSectors[i] == s)
                    return;
            envSectors.PushBack(s);
        };
        auto takeSounds = [](I3D_sector *s, int i) {
            I3D_sound *snd = s->sounds[i];
            if (snd->soundFlags & SNDF_IN_CAMERA_SECTOR)
                return;
            snd->envSector = s;
            if (snd->state == SNDST_OUT_OF_RANGE)
                snd->state = SNDST_AUDIBLE;
            snd->soundFlags |= SNDF_IN_CAMERA_SECTOR;
        };
        if (!g_envSector && camSector) {
            g_envSector = camSector;
            camSector->sectorFlags &= ~SCTF_ENV_FADING_OUT;
            camSector->environmentTime = static_cast<int>(now);
            addFading(camSector);
            for (int i = camSector->sounds.Size(); i != 0; --i)
                takeSounds(camSector, i - 1);
        } else if (camSector != g_envSector) {
            for (int i = g_envSector->sounds.Size(); i != 0; --i) {
                I3D_sound *snd = g_envSector->sounds[i - 1];
                if (!(snd->soundFlags & SNDF_IN_CAMERA_SECTOR))
                    continue;
                bool inNew = false;
                for (int j = 0; j < camSector->sounds.Size(); ++j) {
                    if (camSector->sounds[j] == snd) {
                        inNew = true;
                        break;
                    }
                }
                if (inNew)
                    continue;
                snd->envSector = g_envSector;
                snd->soundFlags &= ~SNDF_IN_CAMERA_SECTOR;
            }
            g_envPrevSector = g_envSector;
            g_envSector->sectorFlags |= SCTF_ENV_FADING_OUT;
            g_envSector->environmentTime = static_cast<int>(now);
            addFading(g_envSector);
            g_envSector = camSector;
            camSector->sectorFlags &= ~SCTF_ENV_FADING_OUT;
            camSector->environmentTime = static_cast<int>(now);
            addFading(camSector);
            for (int i = 0; i < camSector->sounds.Size(); ++i)
                takeSounds(camSector, i);
        }

        // The sound frames: those that moved or whose camera moved update.
        for (int i = soundFrames.Size(); i != 0; --i) {
            I3D_sound *s = static_cast<I3D_sound *>(soundFrames[i - 1]);
            if (!(s->frameFlags & FRMFLAGS_ON) || s->state == SNDST_STOPPED)
                continue;
            if (s->changeCount != s->changeBase) {
                s->changeCount = s->changeBase;
                s->soundFlags |= SNDF_POSITION | SNDF_DIRECTION | SNDF_SCALE;
            } else if (!g_cameraChanged && !s->envSector && s->soundFlags == 0) {
                continue;
            }
            s->Refresh();
        }

        // The environment blends step.
        for (int i = 0; i < envSectors.Size(); ++i) {
            I3D_sector *s = envSectors[i];
            const int dt = static_cast<int>(now) - s->environmentTime;
            if (dt <= 0)
                continue;
            s->environmentTime = static_cast<int>(now);
            const float step = (1.0f / static_cast<float>(static_cast<int>(s->environment.params[I3DENV_FADE_TIME]))) *
                static_cast<float>(dt);
            float f;
            if (s->sectorFlags & SCTF_ENV_FADING_OUT) {
                if (IsTinyBits(s->environmentBlend)) {
                    envSectors.RemoveAtSwap(i--);
                    continue;
                }
                f = s->environmentBlend - step;
                if (!(f > 0.0f))
                    f = 0.0f;
            } else {
                if (IsTinyBits(1.0f - s->environmentBlend)) {
                    envSectors.RemoveAtSwap(i--);
                    continue;
                }
                f = step + s->environmentBlend;
                if (!(f < 1.0f))
                    f = 1.0f;
            }
            s->environmentBlend = f;
            if (s == g_envSector && g_envPrevSector)
                scene->SetSoundEnvironment(s, g_envPrevSector, s->environmentBlend);
        }
        if (cam->sector->sectorFlags & SCTF_ENV_CHANGED)
            scene->SetSoundEnvironment(cam->sector, nullptr, 1.0f);
        for (int i = 0; i < scene->mirrors.Size(); ++i)
            scene->mirrors[i]->Tick(0);
    }

    // ---------------------------------------------------------------- the passes
    g_rc.ResetQueue();
    g_rc.camera = cam;
    g_rc.lodPosition = *reinterpret_cast<const S_vector4 *>(&cam->GetMatrix()(3));
    g_rc.scene = scene;
    g_rc.sector = nullptr;
    if (g_driverStates[DRVSTATE_SHADOWS]) {
        for (int i = 0; i < scene->shadows.Size(); ++i)
            PrepareShadow(scene->shadows[i]);
    }
    g_viewProjection = &cam->viewProjection;
    TraverseSectors(scene);
    OccludeSectors(scene);
    if (!g_inRenderCallback && g_driverStates[DRVSTATE_DRAW_VISUALS]) {
        // The backdrop, around the camera: back to front by the distance from it.
        if (scene->backdropSector->sectorFlags & SCTF_VISIBLE) {
            g_viewProjection = &cam->backdropViewProjection;
            scene->SetScreenProjection(*g_viewProjection);
            g_pDevice->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX *>(&cam->viewRotation));
            g_pDevice->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX *>(&cam->backdropProjection));
            PrepareSector(scene->backdropSector);
            ReserveKeys();
            for (int i = g_rc.items.Size(); i != 0;) {
                --i;
                const S_vector4 &p = LodPoint(g_rc.items[i].visual);
                const int32_t d = Fistp(((p.x * p.x + p.y * p.y) + p.z * p.z) * 65536.0f);
                AddKey(static_cast<uint32_t>(-d), i);
            }
            RadixSort(g_rc.keys, g_rc.numKeys);
            g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
            DrawQueue();
            g_rc.ResetQueue();
        }
        if (g_driverStates[DRVSTATE_ZBUFFER])
            g_pDevice->SetRenderState(D3DRS_ZENABLE, cam->ortho ? TRUE : ig::g_zBufferType);
        // The mirrors' reflections.
        if (scene->mirrors.Size() != 0) {
            g_viewProjection = &cam->viewProjection;
            bool anyVisible = false;
            scene->SetScreenProjection(*g_viewProjection);
            g_pDevice->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX *>(&cam->viewMatrix));
            g_pDevice->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX *>(&cam->projection));
            g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
            for (int m = 0; m < scene->mirrors.Size(); ++m) {
                I3D_mirror *mirror = static_cast<I3D_mirror *>(scene->mirrors[m]);
                MirrorPrepare(mirror);
                const uint32_t f = mirror->mirrorFlags;
                if (!(f & MIRF_VISIBLE))
                    continue;
                anyVisible = true;
                if (!(f & MIRF_REFLECTS) || !MirrorBeginReflection(mirror))
                    continue;
                g_rs.cullDefault = D3DCULL_CW;
                if (g_rs.cull != 1) {
                    g_rs.cull = 1;
                    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                }
                BuildKeys();
                RadixSort(g_rc.keys, g_rc.numKeys);
                DrawQueue();
                g_rc.ResetQueue();
            }
            g_pDevice->SetRenderState(D3DRS_ZENABLE, ig::g_zBufferType);
            g_rs.cullDefault = D3DCULL_CCW;
            if (g_rs.cull != 1) {
                g_rs.cull = 1;
                g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            }
            if (g_rs.cull != 3) {
                g_rs.cull = 3;
                g_pDevice->SetRenderState(D3DRS_CULLMODE, g_rs.cullDefault);
            }
            if (anyVisible) {
                // The mirror surfaces into the z buffer (those that reflect without color).
                g_pDevice->Clear(0, nullptr, clearDepth, 0, 1.0f, 0);
                g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
                if (g_rs.alpha != 0) {
                    g_rs.alpha = 0;
                    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                }
                for (int m = 0; m < scene->mirrors.Size(); ++m) {
                    I3D_mirror *mirror = static_cast<I3D_mirror *>(scene->mirrors[m]);
                    const uint32_t f = mirror->mirrorFlags;
                    if (!(f & MIRF_VISIBLE))
                        continue;
                    if (!(f & MIRF_REFLECTS)) {
                        MirrorDrawSurface(mirror);
                    } else {
                        DWORD colorWrite;
                        g_pDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
                        g_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
                        MirrorDrawSurface(mirror);
                        g_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
                    }
                }
                g_pDevice->SetRenderState(D3DRS_LIGHTING, TRUE);
            }
        }
    }

    // The main pass.
    g_viewProjection = &cam->viewProjection;
    scene->SetScreenProjection(*g_viewProjection);
    g_pDevice->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX *>(&cam->viewMatrix));
    g_pDevice->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX *>(&cam->projection));
    if (g_driverStates[DRVSTATE_ZBUFFER])
        g_pDevice->SetRenderState(D3DRS_ZENABLE, cam->ortho ? TRUE : ig::g_zBufferType);
    if (g_driverStates[DRVSTATE_DRAW_VISUALS]) {
        I3D_sector *s = scene->firstSector;
        for (int n = scene->sectorCount; n != 0; --n, s = s->nextInScene) {
            if (s->sectorFlags & SCTF_VISIBLE)
                PrepareSector(s);
        }
        if (scene->primarySector->sectorFlags & SCTF_VISIBLE)
            PrepareSector(scene->primarySector);
        PrepareSector(scene->omniSector);
    }
    if (g_driverStates[DRVSTATE_PROJECTORS])
        RenderProjectors();
    if (g_driverStates[DRVSTATE_SHADOWS])
        RenderShadows();
    scene->UpdateWeather();
    BuildKeys();
    RadixSort(g_rc.keys, g_rc.numKeys);
    if (g_driverStates[DRVSTATE_SORT_BLENDED] && g_rc.numBlended != 0) {
        if (!SortBlended())
            return I3DERR_OUTOFMEM;
    }
    g_mainPass = true;
    DrawQueue();
    g_mainPass = false;

    if (!g_inRenderCallback) {
        auto modulateStates = []() {
            if (g_rs.alphaOp[0] != D3DTOP_MODULATE) {
                g_rs.alphaOp[0] = D3DTOP_MODULATE;
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            }
            if (g_rs.colorOp[0] != D3DTOP_MODULATE) {
                g_rs.colorOp[0] = D3DTOP_MODULATE;
                g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            }
            if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
                g_rs.colorOp[1] = D3DTOP_DISABLE;
                g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            }
            if (g_rs.alpha != 1) {
                g_rs.alpha = 1;
                g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                g_pDevice->SetRenderState(D3DRS_ALPHAREF, 1);
            }
            if (g_rs.blend != 0) {
                g_rs.blend = 0;
                g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            }
        };
        modulateStates();
        ig::SetZWrite(1);
        ig::SetSpecular(0);
        SetZBias(0);
        if (scene->renderCallbackFlags & 2) {
            g_inRenderCallback = true;
            SetZBias(0);
            reinterpret_cast<RenderCallback>(scene->renderCallback)(1, 0, 0);
            g_renderScene = scene;
            g_inRenderCallback = false;
        }
        g_renderCamera = cam;

        // The near sector (first-person geometry) over everything, with its own projection.
        if (scene->nearSector->firstChild && g_driverStates[DRVSTATE_DRAW_VISUALS]) {
            g_rc.ResetQueue();
            g_pDevice->Clear(0, nullptr, clearDepth, 0, 1.0f, 0);
            const uint32_t lodBits = g_driverStates[DRVSTATE_NEAR_LOD_SCALE];
            float lod;
            std::memcpy(&lod, &lodBits, 4);
            g_fovScale = 1.57079637f / cam->nearSectorFov;
            g_viewProjection = &cam->nearSectorViewProjection;
            g_fovScaleSq = g_fovScale * g_fovScale;
            g_lodScale = lod * g_fovScale;
            scene->SetScreenProjection(*g_viewProjection);
            g_pDevice->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX *>(&cam->nearSectorProjection));
            PrepareSector(scene->nearSector);
            BuildKeys();
            RadixSort(g_rc.keys, g_rc.numKeys);
            DrawQueue();
            g_viewProjection = &cam->viewProjection;
            scene->SetScreenProjection(*g_viewProjection);
            g_pDevice->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX *>(&cam->projection));
            g_fovScale = 1.57079637f / cam->fov;
            g_fovScaleSq = g_fovScale * g_fovScale;
        }

        // The lens flares, additive, without z: those not seen fade out and leave the list.
        if (scene->lensFlares.Size() != 0) {
            g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
            const float a = cam->fov * 0.5f;
            const float b = cam->nearSectorFov * 0.5f;
            const float num = static_cast<float>(std::sin(static_cast<double>(b)) * std::cos(static_cast<double>(a)));
            const float den = static_cast<float>(std::cos(static_cast<double>(b)) * std::sin(static_cast<double>(a)));
            g_lensFlareScale = num / den;
            if (g_rs.colorOp[0] != D3DTOP_MODULATE) {
                g_rs.colorOp[0] = D3DTOP_MODULATE;
                g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            }
            if (g_rs.alphaOp[0] != D3DTOP_MODULATE) {
                g_rs.alphaOp[0] = D3DTOP_MODULATE;
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            }
            if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
                g_rs.colorOp[1] = D3DTOP_DISABLE;
                g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            }
            if (g_rs.alpha != 1) {
                g_rs.alpha = 1;
                g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                g_pDevice->SetRenderState(D3DRS_ALPHAREF, 1);
            }
            if (g_rs.blend != 1) {
                g_rs.blend = 1;
                g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            }
            if (g_rs.fog != 0) {
                g_rs.fog = 0;
                g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
            }
            ig::SetSpecular(0);
            ig::SetZWrite(1);
            SetZBias(0);
            C_vector<I3D_frame *> &list = scene->lensFlares;
            for (int i = 0; i < list.Size(); ++i) {
                I3D_lens *lens = static_cast<I3D_lens *>(list[i]);
                if (!(lens->lensFlags & LENSF_SEEN)) {
                    lens->DrawFlare(false);
                    if (IsTinyBits(std::fabs(lens->brightness))) {
                        lens->Release();
                        list.RemoveAtSwap(i--);
                    }
                } else {
                    lens->DrawFlare(true);
                    lens->lensFlags &= ~LENSF_SEEN;
                }
            }
            g_pDevice->SetRenderState(D3DRS_ZENABLE, ig::g_zBufferType);
        }
        if (!g_inRenderCallback) {
            EndParticles();
            if (scene->renderCallbackFlags & 4) {
                modulateStates();
                ig::SetZWrite(1);
                SetFog(0, 0, 0.0f, 0.0f, 0);
                ig::SetSpecular(0);
                g_inRenderCallback = true;
                SetZBias(0);
                reinterpret_cast<RenderCallback>(scene->renderCallback)(2, 0, 0);
                g_inRenderCallback = false;
            }
            if (!keepScene)
                g_pIGraph->EndScene();
            g_rendering = false;
        }
    }
    g_renderScene = nullptr;
    return I3D_OK;
}
