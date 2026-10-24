// Differential test of the I3D_frame base (src/i3d/frame.cpp) and the driver's frame services
// (driver_frames.cpp) with the simple frame types (user, dummy, target, volume, camera, light,
// sector and its portals, occluder, area, model): the original
// LS3DF.dll's frames (made by its driver) and the port's run the same random operations - world /
// local transform setters, LinkTo with every flag, Update, UpdateBound, names (the name tree,
// prefixes, FindChildFrame, EnumFrames, the driver's FindFrame / EnumNamedFrames /
// RebalanceNameTree), user properties, Duplicate, Release and reuse, targets and the frames
// following them, dummy boxes, volume types, camera projections and frustums, the lights' settings
// and Direct3D lights (each side on its own fake device, the LightEnable calls compared), the
// sectors' geometry (hull planes, portals; made in both with each side's allocators), lights,
// environments, inside tests and their scene rings (two zeroed blocks per side stand in for
// scenes), the occluders' meshes, scene rings and occlusion volumes (frame 4 is the render camera
// of both) and the renderer's occlusion passes over them, the areas' meshes, inside tests and
// scene ring (their sounds wait for the sound frame's port), and the models' animation: each model
// owns three named frames of its own (frames 40 - 51, not chosen by the other operations), plays
// animation sets built the same in both (keys, notes) on its stack, with poses and pose sets; the
// note and end callbacks are recorded and compared. After every operation all frame fields must be identical (pointers compared as frame
// indices, portals as (sector, index), strings by content), and so must the name trees, the
// target lists and the scene rings.
//
// The original's portal destructor leaves the portal in its scene's ring, and its FreeGeometry keeps
// the plane count (the port fixes both): before the original's Duplicate of a sector the test takes
// the old portals out of the ring, and after its FreeGeometry clears the count.
//
// The original's RebalanceNameTree leaves the left link of the last frame of a run of equal
// names as it was (frame.cpp fixes it); the test clears those links in the original after each of
// its rebalances.
//
// Magnitudes stay realistic (local scales 0.1 - 10, hierarchies of at most 5 levels, positions
// within 1e4): the x87 keeps its 15-bit exponent in registers under the 24-bit precision control,
// so where squares or determinants leave the float range (world values of 1e13 and more) the
// original still gets finite results and the port's float math does not.
//
// Usage: compare_frame.exe [original LS3DF.dll] [operations] [seed]
//
// test-sources: src/i3d/frame.cpp src/i3d/user.cpp src/i3d/dummy.cpp src/i3d/target.cpp src/i3d/volume.cpp src/i3d/camera.cpp src/i3d/light.cpp src/i3d/sector.cpp src/i3d/portal.cpp src/i3d/occluder.cpp src/i3d/render_state.cpp src/i3d/area.cpp src/i3d/model.cpp src/i3d/anim.cpp src/i3d/visual.cpp src/i3d/driver_frames.cpp src/math/math.cpp src/core/debug.cpp
#include <windows.h>

#include <float.h>
#include <malloc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/i3d/driver.h"
#include "../src/i3d/frame.h"
#include "../src/i3d/i3d_hooks.h"
#include "../src/i3d/camera.h"
#include "../src/i3d/dummy.h"
#include "../src/i3d/light.h"
#include "../src/i3d/anim.h"
#include "../src/i3d/area.h"
#include "../src/i3d/model.h"
#include "../src/i3d/occluder.h"
#include "../src/i3d/portal.h"
#include "../src/i3d/render_state.h"
#include "../src/i3d/scene.h"
#include "../src/i3d/sector.h"
#include "fake_d3d8_base.h"
#include "../src/igraph/igraph.h"
#include "../src/i3d/target.h"
#include "../src/i3d/user.h"
#include "../src/i3d/volume.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d::i3d {
I3D_driver *g_driver;
uint32_t g_driverStates[DRVSTATE_COUNT];
int g_loadFile;
HRESULT g_loadResult;
std::vector<I3D_frame *> g_freed;
void FreeFrameObject(I3D_frame *frame)
{
    frame->Uninit();
    g_freed.push_back(frame);
}
C_vector<I3D_target *> g_loadTargets;
I3D_sector *SceneLocateSector(I3D_scene *, const S_vector &, I3D_sector *) { return nullptr; }
void SceneAddFrame(I3D_frame *, I3D_frame *) {}
void SceneRemoveDummy(I3D_scene *, I3D_frame *) {}
void RemoveShadowCaster(I3D_frame *) {}
void ShadowSetSource(I3D_frame *, I3D_frame *) {}
I3D_RESULT OpenAnimationFile(const char *, I3D_animation_set *) { return I3DERR_NOFILE; }
I3D_RESULT OpenModelFile(const char *, uint32_t, void *, void *, I3D_frame *) { return I3DERR_NOFILE; }
void SkinFixJoints(I3D_frame *) {}
void SceneFrameReleased(I3D_frame *, I3D_frame *) {}
I3D_RESULT SceneRemoveSoundFrame(I3D_scene *, I3D_frame *) { return I3D_OK; }
void ObjectSetLightLevel(I3D_frame *, float) {}
void ObjectSetAlpha(I3D_frame *, float) {}
}  // namespace ls3d::i3d

I3D_driver::I3D_driver() : unknown04(0), mirrorStamp(0) {}
I3D_driver::~I3D_driver() {}
I3D_RESULT __stdcall I3D_driver::Init(const void *) { return 0; }
void __stdcall I3D_driver::Close() {}
void __stdcall I3D_driver::FreeUnusedResources() {}
void __stdcall I3D_driver::Tick(int) {}
int __stdcall I3D_driver::GetTime() { return 0; }
I3D_RESULT __stdcall I3D_driver::GetStats(int, void *) { return 0; }
I3D_RESULT __stdcall I3D_driver::SetState(int, uint32_t) { return 0; }
uint32_t __stdcall I3D_driver::GetState(int) { return 0; }
I3D_RESULT __stdcall I3D_driver::Render(I3D_scene *, bool) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawPoint(const S_vector &, const S_vector &, uint32_t) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawPoint2D(int, int, const S_vector &, uint32_t) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawLine(const S_vector &, const S_vector &, const S_vector &, uint32_t) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawLines(const void *, int, const uint16_t *, int, const S_vector &, int, uint32_t) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawTriangles(const void *, int, const uint16_t *, int, const S_vector &, int, uint32_t, const float *) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawCircle2D(const float *, float, const S_vector &, int, uint32_t) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawText3D(const S_vector &, const char *, uint32_t, float) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawText2D(int, int, const char *, uint32_t, float) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawSprite(const S_vector &, I3D_material *, uint32_t, float) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawBox(const I3D_bbox &, const S_vector *, const S_vector *, uint32_t) { return 0; }
I3D_RESULT __stdcall I3D_driver::DrawSphere(const S_matrix &, const I3D_bsphere &, const S_vector &, uint32_t) { return 0; }
I3D_frame *__stdcall I3D_driver::CreateVisual(uint32_t) { return nullptr; }
I3D_frame *__stdcall I3D_driver::CreateFrame(int) { return nullptr; }
I3D_material *__stdcall I3D_driver::CreateMaterial() { return nullptr; }
I3D_anim_material *__stdcall I3D_driver::CreateAnimMaterial() { return nullptr; }
I3D_triface *__stdcall I3D_driver::AllocFaces(int) { return nullptr; }
I3D_face_group *__stdcall I3D_driver::CreateFaceGroup() { return nullptr; }
I3D_mesh_level *__stdcall I3D_driver::CreateMeshLevel() { return nullptr; }
I3D_mesh *__stdcall I3D_driver::CreateMesh() { return nullptr; }
I3D_animation_set *__stdcall I3D_driver::CreateAnimationSet() { return nullptr; }
I3D_pose *__stdcall I3D_driver::CreatePose() { return nullptr; }
I3D_pose_set *__stdcall I3D_driver::CreatePoseSet() { return nullptr; }
const char *__stdcall I3D_driver::GetFrameTypeName(int) { return nullptr; }
const char *__stdcall I3D_driver::GetVisualTypeName(uint32_t) { return nullptr; }
I3D_RESULT __stdcall I3D_driver::EnumVisualTypes(I3D_VISUALTYPEPROC *, bool) { return 0; }
I3D_RESULT __stdcall I3D_driver::ResetDevice(I3D_scene *) { return 0; }

// The camera asks IGraph for the screen size: the original's IGraph serves both sides.
IGraph *g_pIGraph;
namespace ls3d::ig {
IDirect3DDevice8 *g_pDevice;
}

// Each side's device: logs LightEnable.
struct RecordingDevice : FakeDirect3DDevice8Base {
    std::vector<uint32_t> calls;
    HRESULT __stdcall LightEnable(DWORD index, BOOL enable) override
    {
        calls.push_back(index << 1 | (enable ? 1 : 0));
        return S_OK;
    }
};
RecordingDevice g_origDevice, g_portDevice;

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }
template <class F> F OrigFn(uint32_t va) { return reinterpret_cast<F>(g_base + (va - 0x10000000)); }

// A virtual __stdcall method of an original object.
template <class R, class... A> R CallV(void *object, int slot, A... args)
{
    using F = R(__stdcall *)(void *, A...);
    return (*reinterpret_cast<F **>(object))[slot / 4](object, args...);
}

uint8_t *g_origDriver;

constexpr int kFrames = 40;
constexpr int kModels = 4;              // frames 9, 19, 29, 39
constexpr int kModelFrames = 3;         // each model's own frames, after the others
constexpr int kTotal = kFrames + kModels * kModelFrames;
uint8_t *g_orig[kTotal];                // the original's frames
I3D_frame *g_port[kTotal];

// The frame types, by index (the models' own frames: user frames).
const uint16_t kTypes[10] = {FRAME_USER, FRAME_DUMMY, FRAME_TARGET, FRAME_VOLUME, FRAME_CAMERA, FRAME_LIGHT,
                              FRAME_SECTOR, FRAME_OCCLUDER, FRAME_AREA, FRAME_MODEL};
uint16_t TypeOf(int i) { return i < kFrames ? kTypes[i % 10] : FRAME_USER; }
int ModelFrame(int model, int k) { return kFrames + (model / 10) * kModelFrames + k; }

int IndexOf(const void *frame, bool orig)
{
    if (!frame)
        return -1;
    for (int i = 0; i < kTotal; ++i) {
        if (orig ? frame == g_orig[i] : frame == g_port[i])
            return i;
    }
    return -2;                          // not one of the frames
}

template <class T> T &Field(uint8_t *frame, size_t offset) { return *reinterpret_cast<T *>(frame + offset); }

// The original's allocators (its own CRT heap) and the portal constructor.
void *OrigAlignedAlloc(size_t size) { return OrigFn<void *(__cdecl *)(size_t, size_t)>(0x1008d42a)(size, 16); }
void *OrigNew(size_t size) { return OrigFn<void *(__cdecl *)(size_t)>(0x1008d41c)(size); }
void OrigDelete(void *p) { OrigFn<void(__cdecl *)(void *)>(0x1008d310)(p); }
using ThisFn = I3D_RESULT(__fastcall *)(void *self, void *edx);
using ThisPtrFn = void(__fastcall *)(void *self, void *edx, void *arg);
// A __thiscall virtual slot of an original object.
void CallThisV(void *object, int slot) { (*reinterpret_cast<ThisFn **>(object))[slot / 4](object, nullptr); }

// The original's C_vector of pointers at `v` (data, capacity, count, grow), as the port's.
void OrigPushBack(uint8_t *v, void *value)
{
    void **&data = Field<void **>(v, 0);
    int &capacity = Field<int>(v, 4);
    int &count = Field<int>(v, 8);
    if (capacity <= count) {
        capacity += Field<int>(v, 12);
        auto **grown = static_cast<void **>(OrigNew(sizeof(void *) * capacity));
        for (int k = 0; k < count; ++k)
            grown[k] = data[k];
        OrigDelete(data);
        data = grown;
    }
    data[count++] = value;
}
void OrigRemoveSwap(uint8_t *v, void *value)
{
    void **data = Field<void **>(v, 0);
    int &count = Field<int>(v, 8);
    for (int k = 0; k < count; ++k) {
        if (data[k] == value) {
            data[k] = data[--count];
            return;
        }
    }
}

// The stand-in scenes (zeroed: no camera, no primary sector).
alignas(16) uint8_t g_origScenes[2][0x450];
alignas(16) uint8_t g_portScenes[2][0x450];
I3D_scene *PortScene(int k) { return k >= 0 ? reinterpret_cast<I3D_scene *>(g_portScenes[k]) : nullptr; }
// Their primary sectors (not among the compared frames: no children, out of the rings).
uint8_t *g_origPrimary[2];
I3D_sector *g_portPrimary[2];
int SceneIndex(const void *scene, bool orig)
{
    if (!scene)
        return -1;
    for (int k = 0; k < 2; ++k) {
        if (scene == (orig ? static_cast<const void *>(g_origScenes[k]) : static_cast<const void *>(g_portScenes[k])))
            return k;
    }
    return -2;
}

// ---------------------------------------------------------------- callbacks shared by both sides

int __stdcall CallbackA(I3D_frame *, uint32_t, uint32_t, uint32_t) { return 0; }
int __stdcall CallbackB(I3D_frame *, uint32_t, uint32_t, uint32_t) { return 1; }

// EnumFrames / EnumNamedFrames: records the frames (as indices) and answers from a script.
std::vector<int> g_visits;
std::vector<uint32_t> g_answers;
bool g_visitOrig;
uint32_t __stdcall RecordProc(I3D_frame *frame, void *context)
{
    const size_t n = g_visits.size();
    g_visits.push_back(IndexOf(frame, g_visitOrig));
    if (context != reinterpret_cast<void *>(0x1234))
        g_visits.push_back(-100);
    return n < g_answers.size() ? g_answers[n] : 0;
}

// ---------------------------------------------------------------- comparison

std::string g_lastOp;
long g_op;
long g_seenInside;                     // IsInside answers true (coverage)
long g_seenCallbacks, g_seenTracks, g_seenPosed, g_seenPlaying;

[[noreturn]] void Fail(const char *what, int frame, size_t offset = 0)
{
    std::printf("MISMATCH after op %ld (%s): %s frame %d offset 0x%zx\n", g_op, g_lastOp.c_str(), what, frame, offset);
    if (frame >= 0 && offset >= 0x10 && offset < 0x540) {
        const size_t row = offset & ~size_t(0xF);
        for (int side = 0; side < 2; ++side) {
            const uint8_t *f = side ? reinterpret_cast<uint8_t *>(g_port[frame]) : g_orig[frame];
            std::printf("%s +0x%zx:", side ? "port" : "orig", row);
            for (int k = 0; k < 4; ++k) {
                float v;
                uint32_t u;
                std::memcpy(&v, f + row + 4 * k, 4);
                std::memcpy(&u, f + row + 4 * k, 4);
                std::printf("  %.9g (%08x)", v, u);
            }
            std::printf("\n");
        }
        if (TypeOf(frame) == FRAME_CAMERA) {
            const auto *c = static_cast<const I3D_camera *>(g_port[frame]);
            std::printf("  camera: fov %g near %g far %g nfov %g nnear %g nfar %g aspect %g ortho %d scale %g half %g %g\n",
                        c->fov, c->nearRange, c->farRange, c->nearSectorFov, c->nearSectorNear, c->nearSectorFar,
                        c->aspect, c->ortho, c->orthoScale, c->nearHalfWidth, c->nearHalfHeight);
            const S_matrix &w = c->worldMatrix;
            std::printf("  world: %g %g %g / %g %g %g / %g %g %g / %g %g %g\n", w.m[0][0], w.m[0][1], w.m[0][2],
                        w.m[1][0], w.m[1][1], w.m[1][2], w.m[2][0], w.m[2][1], w.m[2][2], w.m[3][0], w.m[3][1], w.m[3][2]);
        }
        int depth = 0;
        for (I3D_frame *f = g_port[frame]->parent; f; f = f->parent, ++depth) {
            const S_matrix &m = f->worldMatrix;
            std::printf("  ancestor %d (frame %d): world rows %g %g %g / %g %g %g / %g %g %g / %g %g %g  scale %g %g %g\n",
                        depth, IndexOf(f, false), m.m[0][0], m.m[0][1], m.m[0][2], m.m[1][0], m.m[1][1], m.m[1][2],
                        m.m[2][0], m.m[2][1], m.m[2][2], m.m[3][0], m.m[3][1], m.m[3][2], f->scale.x, f->scale.y,
                        f->scale.z);
        }
    }
    std::exit(1);
}

// Field equality: the same bits, or two NaNs (the x87 and SSE disagree on the signs of the NaNs
// invalid operations make; the engine never relies on them).
bool SameField(const uint8_t *a, const uint8_t *b)
{
    if (!std::memcmp(a, b, 4))
        return true;
    // Subnormal results may differ by one unit: the x87 rounds to 24 bits with its wide exponent,
    // then again when storing the float; SSE rounds once.
    uint32_t ua, ub;
    std::memcpy(&ua, a, 4);
    std::memcpy(&ub, b, 4);
    if (!(ua & 0x7f800000) && !(ub & 0x7f800000) && (ua ^ ub) < 0x80000000u && (ua - ub + 1) <= 2)
        return true;
    float fa, fb;
    std::memcpy(&fa, a, 4);
    std::memcpy(&fb, b, 4);
    return fa != fa && fb != fb;
}

// A portal as (sector frame, index in its portals) * 16; -1 for null, -2 if no sector has it.
int PortalId(const void *portal, bool orig)
{
    if (!portal)
        return -1;
    for (int i = 0; i < kFrames; ++i) {
        if (TypeOf(i) != FRAME_SECTOR)
            continue;
        if (orig) {
            uint8_t *o = g_orig[i];
            for (int k = 0; k < Field<int>(o, 0x254); ++k) {
                if (Field<void **>(o, 0x24C)[k] == portal)
                    return i * 16 + k;
            }
        } else {
            const auto *sector = static_cast<const I3D_sector *>(g_port[i]);
            for (int k = 0; k < sector->portals.Size(); ++k) {
                if (sector->portals[k] == portal)
                    return i * 16 + k;
            }
        }
    }
    return -2;
}

void ComparePortal(int i, int k, uint8_t *o, I3D_portal *portal)
{
    auto *p = reinterpret_cast<uint8_t *>(portal);
    const int n = portal->numVertices;
    char what[64];
    std::snprintf(what, sizeof what, "portal %d field", k);
    std::vector<size_t> raw = {0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x130, 0x134, 0x138, 0x13C,
                               0x180, 0x184, 0x188, 0x190, 0x1AC};
    for (int j = 0; j < n; ++j) {
        for (size_t c = 0; c < 16; c += 4)
            raw.push_back(0x30 + 16 * j + c);
        // (the world vertices' fourth dwords: unset)
        for (size_t c = 0; c < 12; c += 4)
            raw.push_back(0xB0 + 16 * j + c);
    }
    for (size_t off : raw) {
        if (!SameField(o + off, p + off)) {
            std::printf("portal %d offset 0x%zx: orig %08x port %08x\n", k, off, Field<uint32_t>(o, off),
                        Field<uint32_t>(p, off));
            Fail(what, i, 0x24C);
        }
    }
    if (o[0x1B0] != p[0x1B0])
        Fail("portal render flags", i, 0x24C);
    if (IndexOf(Field<void *>(o, 0x194), true) != IndexOf(portal->owner, false) ||
        IndexOf(Field<void *>(o, 0x198), true) != IndexOf(portal->connectedSector, false))
        Fail("portal sectors", i, 0x24C);
    // (Out of a scene the ring links are stale: leaving keeps them.)
    const bool ring = portal->scene != nullptr;
    if (PortalId(Field<void *>(o, 0x19C), true) != PortalId(portal->otherSide, false) ||
        (ring && PortalId(Field<void *>(o, 0x1A4), true) != PortalId(portal->prev, false)) ||
        (ring && PortalId(Field<void *>(o, 0x1A8), true) != PortalId(portal->next, false))) {
        std::printf("portal %d: other side %d / %d, prev %d / %d, next %d / %d\n", k,
                    PortalId(Field<void *>(o, 0x19C), true), PortalId(portal->otherSide, false),
                    PortalId(Field<void *>(o, 0x1A4), true), PortalId(portal->prev, false),
                    PortalId(Field<void *>(o, 0x1A8), true), PortalId(portal->next, false));
        Fail("portal links", i, 0x24C);
    }
    if (SceneIndex(Field<void *>(o, 0x1A0), true) != SceneIndex(portal->scene, false))
        Fail("portal scene", i, 0x24C);
}

void CompareSector(int i, uint8_t *o, I3D_sector *sector)
{
    auto *p = reinterpret_cast<uint8_t *>(sector);
    // (0x1C8 - 0x1CF and 0x28C: never written)
    std::vector<size_t> raw;
    for (size_t off = 0x120; off < 0x190; off += 4)
        raw.push_back(off);
    for (size_t off : {0x198, 0x19C, 0x1A0, 0x1A8, 0x1AC, 0x1B0, 0x1B4, 0x1BC, 0x1C0, 0x1C4, 0x250, 0x254, 0x258,
                       0x260, 0x264, 0x268, 0x270, 0x288})
        raw.push_back(off);
    for (size_t off = 0x1D0; off < 0x248; off += 4)
        raw.push_back(off);
    for (size_t off : raw) {
        if (!SameField(o + off, p + off))
            Fail("sector field", i, off);
    }
    if (!Field<void *>(o, 0x190) != !sector->vertices || !Field<void *>(o, 0x194) != !sector->faces ||
        !Field<void *>(o, 0x274) != !sector->planes)
        Fail("sector geometry pointers", i, 0x190);
    if (sector->numVertices && std::memcmp(Field<uint8_t *>(o, 0x190), sector->vertices, 16 * sector->numVertices))
        Fail("sector vertices", i, 0x190);
    if (sector->numFaces && sector->faces && std::memcmp(Field<uint8_t *>(o, 0x194), sector->faces, 6 * sector->numFaces))
        Fail("sector faces", i, 0x194);
    for (int k = 0; k < 4 * sector->numPlanes; ++k) {
        if (!SameField(Field<uint8_t *>(o, 0x274) + 4 * k, reinterpret_cast<uint8_t *>(sector->planes) + 4 * k))
            Fail("sector planes", i, 0x274);
    }
    for (int k = 0; k < sector->lights.Size(); ++k) {
        if (IndexOf(Field<void **>(o, 0x1A4)[k], true) != IndexOf(sector->lights[k], false))
            Fail("sector lights", i, 0x1A4);
    }
    for (size_t off : {0x248, 0x278, 0x27C, 0x280, 0x284}) {
        // (Out of a scene the ring links are stale: leaving keeps them.)
        if ((off == 0x278 || off == 0x27C) && !sector->scene)
            continue;
        if (IndexOf(Field<void *>(o, off), true) != IndexOf(Field<void *>(p, off), false))
            Fail("sector link", i, off);
    }
    if (SceneIndex(Field<void *>(o, 0x26C), true) != SceneIndex(sector->scene, false))
        Fail("sector scene", i, 0x26C);
    for (int k = 0; k < sector->portals.Size(); ++k)
        ComparePortal(i, k, Field<uint8_t **>(o, 0x24C)[k], sector->portals[k]);
    for (int k = 0; k < sector->incomingPortals.Size(); ++k) {
        if (PortalId(Field<void **>(o, 0x25C)[k], true) != PortalId(sector->incomingPortals[k], false))
            Fail("sector incoming portals", i, 0x25C);
    }
}

void CompareOccluder(int i, uint8_t *o, I3D_occluder *q)
{
    auto *p = reinterpret_cast<uint8_t *>(q);
    const int nv = q->numVertices, nf = q->numFaces;
    std::vector<size_t> raw = {0x120, 0x124, 0x380, 0x5A0, 0x5B0, 0x5B4, 0x5B8, 0x5BC, 0x5C0, 0x5D0, 0x5D4, 0x5D8,
                               0x5E0, 0x5E4, 0x5F0};
    for (int k = 0; k < 4 * q->numSilhouettePlanes && k < 4 * I3D_occluder::kMaxSilhouettePlanes; ++k)
        raw.push_back(0x150 + 4 * k);
    for (int k = 0; k < 4 * q->numFrontPlanes && k < 4 * I3D_occluder::kMaxFrontPlanes; ++k)
        raw.push_back(0x390 + 4 * k);
    for (size_t off : raw) {
        if (!SameField(o + off, p + off))
            Fail("occluder field", i, off);
    }
    // (the vertices' fourth dwords: unset)
    for (int k = 0; k < nv; ++k) {
        for (int c = 0; c < 3; ++c) {
            if (!SameField(Field<uint8_t *>(o, 0x128) + 16 * k + 4 * c, reinterpret_cast<uint8_t *>(q->vertices) + 16 * k + 4 * c) ||
                !SameField(Field<uint8_t *>(o, 0x12C) + 16 * k + 4 * c, reinterpret_cast<uint8_t *>(q->worldVertices) + 16 * k + 4 * c))
                Fail("occluder vertices", i, 0x128);
        }
    }
    if (nf && (std::memcmp(Field<uint8_t *>(o, 0x130), q->faces, 6 * nf) ||
               std::memcmp(Field<uint8_t *>(o, 0x13C), q->neighbors, 6 * nf) ||
               std::memcmp(Field<uint8_t *>(o, 0x140), q->neighborEdges, 3 * nf) ||
               std::memcmp(Field<uint8_t *>(o, 0x148), q->faceFlags, 2 * nf)))
        Fail("occluder faces / edges", i, 0x130);
    for (int k = 0; k < 4 * nf; ++k) {
        if (!SameField(Field<uint8_t *>(o, 0x144) + 4 * k, reinterpret_cast<uint8_t *>(q->facePlanes) + 4 * k))
            Fail("occluder face planes", i, 0x144);
    }
    const bool ring = q->scene != nullptr;
    for (size_t off : {0x134, 0x138, 0x5E8, 0x5EC}) {
        if ((off == 0x134 || off == 0x138) && !ring)
            continue;
        if (IndexOf(Field<void *>(o, off), true) != IndexOf(Field<void *>(p, off), false))
            Fail("occluder link", i, off);
    }
    if (SceneIndex(Field<void *>(o, 0x5C4), true) != SceneIndex(q->scene, false))
        Fail("occluder scene", i, 0x5C4);
}

void CompareArea(int i, uint8_t *o, I3D_area *a)
{
    auto *p = reinterpret_cast<uint8_t *>(a);
    const int nv = a->numVertices, nf = a->numFaces;
    for (size_t off : {0x120, 0x12C, 0x144, 0x14C, 0x150, 0x154, 0x158}) {
        if (!SameField(o + off, p + off))
            Fail("area field", i, off);
    }
    if (nv && std::memcmp(Field<uint8_t *>(o, 0x124), a->vertices, 16 * nv))
        Fail("area vertices", i, 0x124);
    for (int k = 0; k < nv; ++k) {
        for (int c = 0; c < 3; ++c) {
            if (!SameField(Field<uint8_t *>(o, 0x128) + 16 * k + 4 * c, reinterpret_cast<uint8_t *>(a->worldVertices) + 16 * k + 4 * c))
                Fail("area world vertices", i, 0x128);
        }
    }
    if (nf && std::memcmp(Field<uint8_t *>(o, 0x130), a->faces, 6 * nf))
        Fail("area faces", i, 0x130);
    for (int k = 0; k < 4 * nf; ++k) {
        if (!SameField(Field<uint8_t *>(o, 0x13C) + 4 * k, reinterpret_cast<uint8_t *>(a->planes) + 4 * k))
            Fail("area planes", i, 0x13C);
    }
    if (a->scene && (IndexOf(Field<void *>(o, 0x134), true) != IndexOf(a->prevInScene, false) ||
                     IndexOf(Field<void *>(o, 0x138), true) != IndexOf(a->nextInScene, false)))
        Fail("area ring", i, 0x134);
    if (SceneIndex(Field<void *>(o, 0x140), true) != SceneIndex(a->scene, false))
        Fail("area scene", i, 0x140);
}

// ---------------------------------------------------------------- animation

// The animation sets, poses and the pose set of both sides.
constexpr int kSets = 3, kPoses = 4;
uint8_t *g_origSets[kSets];
I3D_animation_set *g_portSets[kSets];
uint8_t *g_origPoses[kPoses];
I3D_pose *g_portPoses[kPoses];
uint8_t *g_origPoseSet;
I3D_pose_set *g_portPoseSet;
const char *const kTrackNames[4] = {"a", "b", "frame01", "zz"};

int SetIndex(const void *x, bool orig)
{
    if (!x)
        return -1;
    for (int k = 0; k < kSets; ++k) {
        if (x == (orig ? static_cast<const void *>(g_origSets[k]) : static_cast<const void *>(g_portSets[k])))
            return k;
    }
    return -2;
}
int PoseIndex(const void *x, bool orig)
{
    if (!x)
        return -1;
    for (int k = 0; k < kPoses; ++k) {
        if (x == (orig ? static_cast<const void *>(g_origPoses[k]) : static_cast<const void *>(g_portPoses[k])))
            return k;
    }
    return -2;
}

// The frame callbacks of the animation, as (frame, message, value, time) per side.
struct CallRecord {
    int frame;
    uint32_t message, a, b;
    bool operator==(const CallRecord &o) const { return frame == o.frame && message == o.message && a == o.a && b == o.b; }
};
std::vector<CallRecord> g_origCalls, g_portCalls;
int __stdcall RecordCallback(I3D_frame *frame, uint32_t message, uint32_t a, uint32_t b)
{
    const int o = IndexOf(frame, true);
    if (o >= 0)
        g_origCalls.push_back({o, message, a, b});
    else
        g_portCalls.push_back({IndexOf(frame, false), message, a, b});
    return 0;
}

bool SameString(const char *a, const char *b) { return !a == !b && (!a || !std::strcmp(a, b)); }

void CompareStack(int i, uint8_t *os, I3D_anim_stack *ps)
{
    if (IndexOf(Field<void *>(os, 0), true) != IndexOf(ps->owner, false) || os[4] != uint8_t(ps->dirty)) {
        std::printf("stack: owner %d / %d, dirty %d / %d, tracks %d / %d\n", IndexOf(Field<void *>(os, 0), true),
                    IndexOf(ps->owner, false), os[4], ps->dirty, Field<int>(os, 0xF0), ps->tracks.Size());
        Fail("stack owner / dirty", i);
    }
    for (int st = 0; st < 8; ++st) {
        uint8_t *o = os + 8 + 0x1C * st;
        const I3D_anim_stack::Stage &p = ps->stages[st];
        if (SetIndex(Field<void *>(o, 0), true) != SetIndex(p.set, false) || o[4] != uint8_t(p.active) ||
            std::memcmp(o + 8, &p.mode, 16) || !SameField(o + 0x18, reinterpret_cast<const uint8_t *>(&p.weight)))
            Fail("stack stage", i, st);
    }
    if (std::memcmp(os + 0xEC, reinterpret_cast<uint8_t *>(ps) + 0xEC, 12))
        Fail("stack tracks count", i);
    for (int k = 0; k < ps->tracks.Size(); ++k) {
        uint8_t *ot = Field<uint8_t **>(os, 0xE8)[k];
        I3D_anim_track *pt = ps->tracks[k];
        if (PoseIndex(Field<void *>(ot, 4), true) != PoseIndex(pt->pose, false) ||
            !SameField(ot + 8, reinterpret_cast<const uint8_t *>(&pt->poseWeight)) ||
            IndexOf(Field<void *>(ot, 0xEC), true) != IndexOf(pt->frame, false) || Field<uint8_t *>(ot, 0xF0) != os ||
            pt->stack != ps)
            Fail("track", i, k);
        for (int st = 0; st < 8; ++st) {
            uint8_t *ok = ot + 0xC + 0x1C * st;
            const I3D_anim_keys &pk = pt->stages[st];
            if (std::memcmp(ok + 0x10, &pk.numPositions, 10))
                Fail("track keys", i, k * 8 + st);
            if (!pk.flags)
                continue;
            // (the pointers relative to the stage's set's blob)
            const uint8_t *oData = Field<uint8_t *>(Field<uint8_t *>(os, 8 + 0x1C * st), 0x10);
            const uint8_t *pData = ps->stages[st].set->data;
            const uint16_t masks[4] = {ANIMTRACK_POSITION, ANIMTRACK_ROTATION, ANIMTRACK_SCALE,
                                       ANIMTRACK_NOTES16 | ANIMTRACK_NOTES32};
            for (int f = 0; f < 4; ++f) {
                if ((pk.flags & masks[f]) &&
                    Field<uint8_t *>(ok, 4 * f) - oData != reinterpret_cast<const uint8_t *const *>(&pk)[f] - pData)
                    Fail("track key pointers", i, k * 8 + st);
            }
        }
    }
}

void CompareModel(int i, uint8_t *o, I3D_model *m)
{
    if (std::memcmp(o + 0x124, reinterpret_cast<uint8_t *>(m) + 0x124, 12))
        Fail("model frames count", i, 0x124);
    for (int k = 0; k < m->frames.Size(); ++k) {
        if (IndexOf(Field<void **>(o, 0x120)[k], true) != IndexOf(m->frames[k], false))
            Fail("model frames", i, 0x120);
    }
    if (!Field<void *>(o, 0x134) != !m->fileName)
        Fail("model file name", i, 0x134);
    if (!Field<void *>(o, 0x130) != !m->animStack)
        Fail("model stack", i, 0x130);
    if (m->animStack)
        CompareStack(i, Field<uint8_t *>(o, 0x130), m->animStack);
}

void CompareAnimation()
{
    for (int k = 0; k < kSets; ++k) {
        if (std::memcmp(g_origSets[k] + 4, reinterpret_cast<uint8_t *>(g_portSets[k]) + 4, 8) ||
            !SameString(Field<const char *>(g_origSets[k], 0xC), g_portSets[k]->name))
            Fail("animation set", k);
    }
    for (int k = 0; k < kPoses; ++k) {
        uint8_t *o = g_origPoses[k];
        auto *p = reinterpret_cast<uint8_t *>(g_portPoses[k]);
        // (position.w: unset by the constructor)
        for (size_t off : {0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x48}) {
            if (!SameField(o + off, p + off)) {
                std::printf("pose %d offset 0x%zx: orig %08x port %08x\n", k, off, Field<uint32_t>(o, off),
                            Field<uint32_t>(p, off));
                Fail("pose", -1);
            }
        }
        if (!SameString(Field<const char *>(o, 0x44), g_portPoses[k]->name))
            Fail("pose name", k);
    }
    if (std::memcmp(g_origPoseSet + 4, reinterpret_cast<uint8_t *>(g_portPoseSet) + 4, 4) ||
        std::memcmp(g_origPoseSet + 0xC, reinterpret_cast<uint8_t *>(g_portPoseSet) + 0xC, 12))
        Fail("pose set", -1);
    for (int k = 0; k < g_portPoseSet->poses.Size(); ++k) {
        if (PoseIndex(Field<void **>(g_origPoseSet, 8)[k], true) != PoseIndex(g_portPoseSet->poses[k], false))
            Fail("pose set poses", -1);
    }
    if (!(g_origCalls == g_portCalls)) {
        std::printf("callbacks: orig %zu, port %zu\n", g_origCalls.size(), g_portCalls.size());
        for (size_t k = 0; k < g_origCalls.size() || k < g_portCalls.size(); ++k) {
            if (k < g_origCalls.size())
                std::printf("  orig %d %x %u %u\n", g_origCalls[k].frame, g_origCalls[k].message, g_origCalls[k].a, g_origCalls[k].b);
            if (k < g_portCalls.size())
                std::printf("  port %d %x %u %u\n", g_portCalls[k].frame, g_portCalls[k].message, g_portCalls[k].a, g_portCalls[k].b);
        }
        Fail("animation callbacks", -1);
    }
    g_seenCallbacks += static_cast<long>(g_portCalls.size());
    g_origCalls.clear();
    g_portCalls.clear();
}

void CompareFrame(int i)
{
    uint8_t *o = g_orig[i];
    auto *p = reinterpret_cast<uint8_t *>(g_port[i]);
    // Raw fields: 0x10 - 0xE8 except the box pads (+0x1C, +0x2C) and scaleW (+0xDC), which the
    // original fills with whatever was on the stack (0x04 - 0x0F is padding, never written).
    for (size_t off = 0x10; off < 0xE8; off += 4) {
        if (off == 0x1C || off == 0x2C || off == 0xDC)
            continue;
        if (!SameField(o + off, p + off))
            Fail("field", i, off);
    }
    const char *on = Field<const char *>(o, 0xE8), *pn = Field<const char *>(p, 0xE8);
    if (!on != !pn || (on && std::strcmp(on, pn)))
        Fail("name", i, 0xE8);
    if (std::memcmp(o + 0xEC, p + 0xEC, 4))
        Fail("data", i, 0xEC);
    const char *ou = Field<const char *>(o, 0xF0), *pu = Field<const char *>(p, 0xF0);
    if (!ou != !pu || (ou && (std::strcmp(ou, pu) || reinterpret_cast<const int16_t *>(ou)[-1] !=
                                                         reinterpret_cast<const int16_t *>(pu)[-1])))
        Fail("userProps", i, 0xF0);
    for (size_t off = 0xF4; off < 0x108; off += 4) {
        if (off == 0x100) {
            // (the owning model: a frame)
            if (IndexOf(Field<void *>(o, off), true) != IndexOf(Field<void *>(p, off), false))
                Fail("owner model", i, off);
            continue;
        }
        if (!SameField(o + off, p + off))
            Fail("field", i, off);
    }
    for (size_t off = 0x108; off < 0x11C; off += 4) {
        if (IndexOf(Field<void *>(o, off), true) != IndexOf(Field<void *>(p, off), false))
            Fail("link", i, off);
    }
    if (std::memcmp(o + 0x11C, p + 0x11C, 4))
        Fail("field", i, 0x11C);
    // The fields of the type (box pads left out: never or randomly written).
    std::vector<size_t> raw;
    switch (TypeOf(i)) {
    case FRAME_USER:
        raw = {0x120};
        break;
    case FRAME_DUMMY:
        raw = {0x120, 0x124, 0x128, 0x130, 0x134, 0x138, 0x140};
        break;
    case FRAME_VOLUME:
        raw = {0x120, 0x124, 0x128, 0x130, 0x134, 0x138, 0x140, 0x144, 0x148, 0x14C, 0x150, 0x154, 0x158};
        break;
    case FRAME_CAMERA:
        for (size_t off = 0x120; off < 0x540; off += 4) {
            // (padding, and the bool's padding bytes)
            if ((off >= 0x144 && off < 0x150) || (off >= 0x454 && off < 0x460) || (off >= 0x4C4 && off < 0x4D0) ||
                off >= 0x534)
                continue;
            if (off == 0x13C) {
                if (o[off] != p[off])
                    Fail("camera ortho", i, off);
                continue;
            }
            raw.push_back(off);
        }
        break;
    case FRAME_LIGHT:
        // (the sector list's data pointer, the colors' fourth dwords and 0x194 - 0x19F: unset)
        for (size_t off = 0x124; off < 0x25C; off += 4) {
            if (off == 0x13C || off == 0x14C || off == 0x15C || off == 0x1AC || off == 0x1BC || off == 0x1CC ||
                (off >= 0x194 && off < 0x1A0))
                continue;
            raw.push_back(off);
        }
        if (std::memcmp(o + 0x25C, p + 0x25C, 2))
            Fail("light index", i, 0x25C);
        break;
    case FRAME_SECTOR:
        CompareSector(i, o, static_cast<I3D_sector *>(g_port[i]));
        break;
    case FRAME_OCCLUDER:
        CompareOccluder(i, o, static_cast<I3D_occluder *>(g_port[i]));
        break;
    case FRAME_AREA:
        CompareArea(i, o, static_cast<I3D_area *>(g_port[i]));
        break;
    case FRAME_MODEL:
        CompareModel(i, o, static_cast<I3D_model *>(g_port[i]));
        break;
    case FRAME_TARGET: {
        raw = {0x124, 0x128, 0x12C, 0x130};
        const int n = Field<int>(o, 0x128);
        for (int k = 0; k < n; ++k) {
            if (IndexOf(Field<void **>(o, 0x120)[k], true) != IndexOf(Field<void **>(p, 0x120)[k], false))
                Fail("target frame list", i, 0x120);
        }
        break;
    }
    }
    for (size_t off : raw) {
        if (!SameField(o + off, p + off))
            Fail("field", i, off);
    }
}

void CompareAll()
{
    for (int i = 0; i < kTotal; ++i)
        CompareFrame(i);
    CompareAnimation();
    if (IndexOf(Orig<void *>(0x100be010), true) != IndexOf(g_nameRoot, false))
        Fail("name root", -1);
    if (Orig<int>(0x100d13c4) != g_lights.Size())
        Fail("light count", -1);
    for (int k = 0; k < g_lights.Size(); ++k) {
        if (IndexOf(Orig<void **>(0x100d13bc)[k], true) != IndexOf(g_lights[k], false))
            Fail("light list", -1);
    }
    if (g_origDevice.calls != g_portDevice.calls)
        Fail("LightEnable calls", -1);
    if (Orig<int>(0x100bf350) != g_portalChanges || Orig<int>(0x100bf354) != g_sectorChanges)
        Fail("portal / sector change counts", -1);
    for (int k = 0; k < 2; ++k) {
        uint8_t *os = g_origScenes[k];
        const I3D_scene *ps = PortScene(k);
        // Each side's portal ring: its count of live portals, linked both ways.
        for (int side = 0; side < 2; ++side) {
            const bool orig = side == 0;
            uint8_t *first = orig ? Field<uint8_t *>(os, 0x260) : reinterpret_cast<uint8_t *>(ps->firstPortal);
            const int count = orig ? Field<int>(os, 0x25C) : ps->portalCount;
            uint8_t *q = first;
            for (int m = 0; m < count; ++m) {
                if (!q || PortalId(q, orig) < 0 || Field<uint8_t *>(Field<uint8_t *>(q, 0x1A8), 0x1A4) != q ||
                    SceneIndex(Field<void *>(q, 0x1A0), orig) != k) {
                    std::printf("%s scene %d ring broken at %d of %d (portal %d)\n", orig ? "orig" : "port", k, m,
                                count, q ? PortalId(q, orig) : -1);
                    Fail("scene portal ring", -1);
                }
                q = Field<uint8_t *>(q, 0x1A8);
            }
            if (q != first)
                Fail("scene portal ring length", -1);
        }
        if (Field<int>(os, 0x25C) != ps->portalCount || Field<int>(os, 0x264) != ps->portalChangesSeen ||
            Field<int>(os, 0x268) != ps->sectorCount)
            Fail("scene counts", -1);
        if (PortalId(Field<void *>(os, 0x260), true) != PortalId(ps->firstPortal, false) ||
            IndexOf(Field<void *>(os, 0x26C), true) != IndexOf(ps->firstSector, false) ||
            IndexOf(Field<void *>(os, 0x278), true) != IndexOf(ps->firstOccluder, false))
            Fail("scene rings", -1);
        if (Field<int>(os, 0x274) != ps->occluderCount || os[0x27C] != uint8_t(ps->occludersChanged) ||
            os[0x27D] != uint8_t(ps->occluderListsStale))
            Fail("scene occluder state", -1);
        if (Field<int>(os, 0x280) != ps->areaCount || IndexOf(Field<void *>(os, 0x284), true) != IndexOf(ps->firstArea, false) ||
            os[0x288] != uint8_t(ps->areasChanged))
            Fail("scene areas", -1);
        if (IndexOf(Field<void *>(g_origPrimary[k], 0x280), true) != IndexOf(g_portPrimary[k]->occluders, false))
            Fail("primary sector occluders", -1);
    }
    const int n = Orig<int>(0x100c0578);
    if (n != g_targets.Size())
        Fail("target count", -1);
    for (int k = 0; k < n; ++k) {
        if (IndexOf(Orig<void **>(0x100c0570)[k], true) != IndexOf(g_targets[k], false))
            Fail("target list", -1);
    }
}

// After the original's RebalanceNameTree: the left links its build leaves stale (see the top).
void ClearStaleChainLinks(uint8_t *node)
{
    if (!node)
        return;
    uint8_t *right = Field<uint8_t *>(node, 0x118);
    const char *a = Field<const char *>(node, 0xE8);
    if (right) {
        const char *b = Field<const char *>(right, 0xE8);
        if (!std::strcmp(a ? a : "", b ? b : ""))
            Field<void *>(right, 0x114) = nullptr;
    }
    ClearStaleChainLinks(Field<uint8_t *>(node, 0x114));
    ClearStaleChainLinks(right);
}

// ---------------------------------------------------------------- random data

std::mt19937 g_rng;
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }
float RandF(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

float RandCoord()
{
    switch (Rand(10)) {
    case 0: return 0.0f;
    case 1: return RandF(-1e-5f, 1e-5f);
    case 2: return Rand(2) ? 1.0f : -1.0f;
    case 3: return RandF(-1000.0f, 1000.0f);
    default: return RandF(-10.0f, 10.0f);
    }
}
S_vector RandVector() { return S_vector(RandCoord(), RandCoord(), RandCoord()); }
S_quat RandQuat()
{
    S_quat q;
    q.v = S_vector(RandF(-1, 1), RandF(-1, 1), RandF(-1, 1));
    q.s = RandF(-1, 1);
    if (Rand(4)) {
        const float l = std::sqrt(q.v.Dot(q.v) + q.s * q.s);
        if (l > 1e-3f) {
            q.v *= 1.0f / l;
            q.s /= l;
        }
    }
    return q;
}
float RandAngle()
{
    switch (Rand(4)) {
    case 0: return 0.0f;
    case 1: return RandF(-1e-7f, 1e-7f);
    default: return RandF(-3.5f, 3.5f);
    }
}

const char *const kNames[] = {nullptr, "", "a", "b", "ab", "abc", "A", "B", "Ab", "x.y", "a.b", "frame01", "Frame01", "f?x", "zz", "a"};
const char *const kMasks[] = {nullptr, "", "a", "b", "A", "a*", "?b", "*", "ab?", "F*", "frame0?", "y", "a.b", "abc*"};
const char *const kPrefixes[] = {"m", "model", "x", "a"};
const char *const kProps[] = {nullptr, "", "abc", "key=1 other=2", "x"};

const char *RandName() { return kNames[Rand(sizeof kNames / sizeof *kNames)]; }

int Depth(I3D_frame *f)
{
    int d = 0;
    for (; f; f = f->parent)
        ++d;
    return d;
}
int SubtreeHeight(I3D_frame *f)
{
    int h = 0;
    for (I3D_frame *c = f->firstChild; c; c = c->nextSibling)
        h = std::max(h, SubtreeHeight(c));
    return h + 1;
}

bool IsDescendant(int frame, int of)
{
    for (I3D_frame *f = g_port[frame]; f; f = f->parent) {
        if (f == g_port[of])
            return true;
    }
    return false;
}

uint32_t RandEnumFlags()
{
    uint32_t f;
    switch (Rand(4)) {
    case 0: f = ENUMF_ALL_TYPES; break;
    case 1: f = 0x100; break;               // user frames
    case 2: f = 0; break;
    default: f = g_rng() & 0xffff; break;
    }
    if (Rand(2))
        f |= ENUMF_WILDMASK;
    if (Rand(2))
        f |= ENUMF_CASESENSITIVE;
    return f;
}

// ---------------------------------------------------------------- frames

uint8_t *OrigCreate(uint16_t type) { return CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(type)); }

I3D_frame *PortCreate(uint16_t type, I3D_frame *reuse)
{
    I3D_frame *f = reuse;
    if (!f) {
        switch (type) {
        case FRAME_DUMMY: f = new I3D_dummy; break;
        case FRAME_TARGET: f = new I3D_target; break;
        case FRAME_VOLUME: f = new I3D_volume; break;
        case FRAME_CAMERA: f = new I3D_camera; break;
        case FRAME_LIGHT: f = new I3D_light; break;
        case FRAME_SECTOR: f = new I3D_sector; break;
        case FRAME_OCCLUDER: f = new I3D_occluder; break;
        case FRAME_AREA: f = new I3D_area; break;
        case FRAME_MODEL: f = new I3D_model; break;
        default: f = new I3D_user; break;
        }
    }
    f->type = type;
    f->visualIndex = 0;
    f->refCount = 1;
    f->Init();
    return f;
}

using NamePrefixFn = I3D_RESULT(__fastcall *)(void *self, void *edx, const char *prefix);

// A sector's geometry, made the same in both: a box hull (sometimes with a degenerate and a
// repeated face) and up to three portals on its faces; then MatrixChanged in both.
void MakeGeometry(int i)
{
    uint8_t *o = g_orig[i];
    auto *sector = static_cast<I3D_sector *>(g_port[i]);
    const S_vector c(RandF(-20, 20), RandF(-20, 20), RandF(-20, 20));
    const S_vector h(RandF(0.5f, 10), RandF(0.5f, 10), RandF(0.5f, 10));
    I3D_bbox box;
    box.min = c - h;
    box.max = c + h;
    box.minW = RandF(0, 1);
    box.maxW = RandF(0, 1);
    std::vector<S_vector4> v;
    for (int k = 0; k < 8; ++k) {
        const S_vector p = box.Corner(k);
        v.push_back({p.x, p.y, p.z, Rand(2) ? 0.0f : RandF(-1, 1)});
    }
    // (Corner order: 0 min, 1 +x, 2 +x+y, 3 +y, 4 +y+z, 5 +x+y+z, 6 +x+z, 7 +z.)
    std::vector<I3D_triface> f = {{{0, 3, 2}}, {{0, 2, 1}}, {{7, 6, 5}}, {{7, 5, 4}}, {{0, 1, 6}}, {{0, 6, 7}},
                                  {{3, 4, 5}}, {{3, 5, 2}}, {{0, 7, 4}}, {{0, 4, 3}}, {{1, 2, 5}}, {{1, 5, 6}}};
    // (Half the time wound inwards: the planes then hold the inside behind them.)
    if (Rand(2)) {
        for (I3D_triface &t : f)
            std::swap(t.v[1], t.v[2]);
    }
    if (Rand(6) == 0)
        f.push_back({{0, 0, 1}});
    if (Rand(4) == 0)
        f.push_back(f[Rand(12)]);
    const int nv = Rand(10) ? 8 : 0;
    const int nf = nv ? static_cast<int>(f.size()) : 0;
    uint32_t flags = 0x801;
    if (Rand(8) == 0)
        flags |= 0x1000;                    // no planes
    struct Portal {
        int n;
        S_vector4 v[8];
        float head[8];                      // plane, flags, ranges, pad
    };
    std::vector<Portal> portals(nv ? Rand(4) : 0);
    for (Portal &pt : portals) {
        pt.n = 2 + Rand(7);
        const float x = Rand(2) ? box.max.x : box.min.x;
        for (int k = 0; k < pt.n; ++k)
            pt.v[k] = {x, RandF(box.min.y, box.max.y), RandF(box.min.z, box.max.z), RandF(-1, 1)};
        for (float &hd : pt.head)
            hd = RandF(-2, 2);
        const uint32_t pf = g_rng() & 0xFF;
        std::memcpy(&pt.head[4], &pf, 4);
        pt.head[5] = Rand(2) ? 0.0f : RandF(0, 400);
    }

    sector->numVertices = nv;
    sector->numFaces = nf;
    sector->box = box;
    sector->sectorFlags = flags;
    Field<int>(o, 0x168) = nv;
    Field<int>(o, 0x16C) = nf;
    std::memcpy(o + 0x170, &box, sizeof box);
    Field<uint32_t>(o, 0x160) = flags;
    if (nv) {
        sector->vertices = static_cast<S_vector4 *>(AlignedAlloc(16 * nv, 16));
        std::memcpy(sector->vertices, v.data(), 16 * nv);
        sector->faces = static_cast<I3D_triface *>(::operator new(6 * nf));
        std::memcpy(sector->faces, f.data(), 6 * nf);
        sector->planes = static_cast<S_plane *>(AlignedAlloc(16 * nf, 16));
        Field<void *>(o, 0x190) = OrigAlignedAlloc(16 * nv);
        std::memcpy(Field<void *>(o, 0x190), v.data(), 16 * nv);
        Field<void *>(o, 0x194) = OrigNew(6 * nf);
        std::memcpy(Field<void *>(o, 0x194), f.data(), 6 * nf);
        Field<void *>(o, 0x274) = OrigAlignedAlloc(16 * nf);
    }
    const int np = static_cast<int>(portals.size());
    // (Reserve in both: the arrays of earlier geometry stay allocated.)
    sector->portals.Reserve(np);
    if (Field<int>(o, 0x250) < np) {
        OrigDelete(Field<void *>(o, 0x24C));
        Field<void *>(o, 0x24C) = OrigNew(sizeof(void *) * np);
        Field<int>(o, 0x250) = np;
    }
    for (const Portal &pt : portals) {
        auto *portal = new I3D_portal;
        portal->numVertices = pt.n;
        std::memcpy(portal->vertices, pt.v, 16 * pt.n);
        std::memcpy(&portal->loadedPlane, pt.head, 0x1C);
        portal->owner = sector;
        sector->portals.PushBack(portal);
        auto *op = static_cast<uint8_t *>(OrigAlignedAlloc(0x1C0));
        OrigFn<void *(__fastcall *)(void *, void *)>(0x1003ed10)(op, nullptr);
        Field<int>(op, 0x1AC) = pt.n;
        std::memcpy(op + 0x30, pt.v, 16 * pt.n);
        std::memcpy(op + 0x10, pt.head, 0x1C);
        Field<void *>(op, 0x194) = o;
        Field<void **>(o, 0x24C)[Field<int>(o, 0x254)++] = op;
    }
    CallThisV(o, 0x68);
    sector->MatrixChanged();
}

// An occluder's mesh (Build in both from the same arrays: a box, a tetrahedron or a soup with
// edges shared by three faces), then MatrixChanged in both; its near plane (never initialized)
// the same in both.
void MakeOccluderMesh(int i)
{
    uint8_t *o = g_orig[i];
    auto *q = static_cast<I3D_occluder *>(g_port[i]);
    std::vector<S_vector4> v;
    std::vector<I3D_triface> f;
    const S_vector c(RandF(-10, 10), RandF(-10, 10), RandF(-10, 10));
    const int kind = Rand(4);
    if (kind == 0) {
        I3D_bbox box;
        const S_vector h(RandF(0.2f, 5), RandF(0.2f, 5), RandF(0.2f, 5));
        box.min = c - h;
        box.max = c + h;
        for (int k = 0; k < 8; ++k) {
            const S_vector pt = box.Corner(k);
            v.push_back({pt.x, pt.y, pt.z, 0.0f});
        }
        f = {{{0, 3, 2}}, {{0, 2, 1}}, {{7, 6, 5}}, {{7, 5, 4}}, {{0, 1, 6}}, {{0, 6, 7}},
             {{3, 4, 5}}, {{3, 5, 2}}, {{0, 7, 4}}, {{0, 4, 3}}, {{1, 2, 5}}, {{1, 5, 6}}};
        if (Rand(2))
            std::swap(f[0].v[1], f[0].v[2]);
    } else if (kind == 1) {
        for (int k = 0; k < 4; ++k)
            v.push_back({c.x + RandF(-3, 3), c.y + RandF(-3, 3), c.z + RandF(-3, 3), 0.0f});
        f = {{{0, 1, 2}}, {{0, 3, 1}}, {{1, 3, 2}}, {{2, 3, 0}}};
    } else if (kind == 2) {
        const int n = 3 + Rand(6);
        for (int k = 0; k < n; ++k)
            v.push_back({c.x + RandF(-3, 3), c.y + RandF(-3, 3), c.z + RandF(-3, 3), 0.0f});
        const int m = 1 + Rand(10);
        for (int k = 0; k < m; ++k)
            f.push_back({{uint16_t(Rand(n)), uint16_t(Rand(n)), uint16_t(Rand(n))}});
    }
    const int nv = static_cast<int>(v.size()), nf = static_cast<int>(f.size());
    // (12- or 16-byte vertices)
    const bool packed = Rand(2) != 0;
    std::vector<S_vector> v3;
    for (const S_vector4 &x : v)
        v3.push_back(S_vector(x.x, x.y, x.z));
    const void *src = packed ? static_cast<const void *>(v3.data()) : static_cast<const void *>(v.data());
    const int stride = packed ? 12 : 16;
    const I3D_RESULT ro = CallV<I3D_RESULT>(o, 0x74, src, nv, f.data(), nf, stride);
    const I3D_RESULT rp = q->Build(src, nv, f.data(), nf, stride);
    if (ro != rp)
        Fail("occluder Build result", i);
    CallThisV(o, 0x68);
    q->MatrixChanged();
}

// An area's mesh (Build in both from the same 16- or 20-byte strided vertices: a box wound either
// way or a random soup), then MatrixChanged in both.
void MakeAreaMesh(int i)
{
    uint8_t *o = g_orig[i];
    auto *a = static_cast<I3D_area *>(g_port[i]);
    std::vector<S_vector4> v;
    std::vector<I3D_triface> f;
    const S_vector c(RandF(-10, 10), RandF(-10, 10), RandF(-10, 10));
    if (Rand(3)) {
        I3D_bbox box;
        const S_vector h(RandF(0.5f, 8), RandF(0.5f, 8), RandF(0.5f, 8));
        box.min = c - h;
        box.max = c + h;
        for (int k = 0; k < 8; ++k) {
            const S_vector pt = box.Corner(k);
            v.push_back({pt.x, pt.y, pt.z, RandF(-1, 1)});
        }
        f = {{{0, 3, 2}}, {{0, 2, 1}}, {{7, 6, 5}}, {{7, 5, 4}}, {{0, 1, 6}}, {{0, 6, 7}},
             {{3, 4, 5}}, {{3, 5, 2}}, {{0, 7, 4}}, {{0, 4, 3}}, {{1, 2, 5}}, {{1, 5, 6}}};
        if (Rand(2)) {
            for (I3D_triface &t : f)
                std::swap(t.v[1], t.v[2]);
        }
    } else if (Rand(4)) {
        const int n = 3 + Rand(6);
        for (int k = 0; k < n; ++k)
            v.push_back({c.x + RandF(-3, 3), c.y + RandF(-3, 3), c.z + RandF(-3, 3), RandF(-1, 1)});
        const int m = 1 + Rand(10);
        for (int k = 0; k < m; ++k)
            f.push_back({{uint16_t(Rand(n)), uint16_t(Rand(n)), uint16_t(Rand(n))}});
    }
    const int nv = static_cast<int>(v.size()), nf = static_cast<int>(f.size());
    const int stride = Rand(2) ? 16 : 20;
    std::vector<uint8_t> src(stride * (nv + 1));
    for (int k = 0; k < nv; ++k)
        std::memcpy(src.data() + stride * k, &v[k], 16);
    const I3D_RESULT ro = CallV<I3D_RESULT>(o, 0x74, src.data(), nv, f.data(), nf, stride);
    const I3D_RESULT rp = a->Build(src.data(), nv, f.data(), nf, stride);
    if (ro != rp)
        Fail("area Build result", i);
    CallThisV(o, 0x68);
    a->MatrixChanged();
}

// An animation set's blob: 1 - 3 tracks named from kTrackNames with random keys of every kind
// (times in 40 ms frames, rising), built once and copied into each side's 16-byte aligned block,
// the name and track offsets made pointers (the loader's relocation). (The 32-bit note values are
// plain numbers: the original's loader would make them string pointers.)
void MakeSet(int k)
{
    std::vector<uint8_t> v;
    std::vector<size_t> pointers;
    const auto put16 = [&](uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    const auto put32 = [&](uint32_t x) { for (int b = 0; b < 4; ++b) v.push_back(uint8_t(x >> (8 * b))); };
    const auto putF = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(u); };
    const auto align = [&](size_t a) { while (v.size() % a) v.push_back(0); };
    const auto patch = [&](size_t at, uint32_t x) { std::memcpy(&v[at], &x, 4); pointers.push_back(at); };
    const auto times = [&](int n) {
        int t = Rand(3);
        for (int i = 0; i < n; ++i) {
            put16(t);
            t += 1 + Rand(12);
        }
    };
    const int nTracks = 1 + Rand(3);
    put16(nTracks);
    put16(10 + Rand(50));
    const size_t entries = v.size();
    for (int t = 0; t < 2 * nTracks; ++t)
        put32(0);
    for (int t = 0; t < nTracks; ++t) {
        patch(entries + 8 * t, uint32_t(v.size()));
        for (const char *c = kTrackNames[Rand(4)]; *c; ++c)
            v.push_back(uint8_t(*c));
        v.push_back(0);
    }
    for (int t = 0; t < nTracks; ++t) {
        align(16);
        patch(entries + 8 * t + 4, uint32_t(v.size()));
        uint32_t flags = g_rng() & (ANIMTRACK_POSITION | ANIMTRACK_ROTATION | ANIMTRACK_SCALE);
        flags |= Rand(3) == 0 ? 0 : Rand(2) ? ANIMTRACK_NOTES16 : ANIMTRACK_NOTES32;
        put32(flags);
        if (flags & ANIMTRACK_ROTATION) {
            const int n = 1 + Rand(5);
            put16(n);
            times(n);
            align(16);
            for (int i = 0; i < n; ++i) {
                S_quat q = RandQuat();
                q.Normalize();
                putF(q.v.x);
                putF(q.v.y);
                putF(q.v.z);
                putF(q.s);
            }
        }
        for (uint32_t kind : {ANIMTRACK_POSITION, ANIMTRACK_SCALE}) {
            if (!(flags & kind))
                continue;
            const int n = 1 + Rand(5);
            put16(n);
            times(n);
            if (!(n & 1))
                put16(0);
            for (int i = 0; i < 3 * n; ++i)
                putF(kind == ANIMTRACK_POSITION ? RandF(-5, 5) : RandF(0.5f, 2.0f));
        }
        if (flags & (ANIMTRACK_NOTES16 | ANIMTRACK_NOTES32)) {
            const int n = 1 + Rand(5);
            put16(n);
            times(n);
            if (flags & ANIMTRACK_NOTES16) {
                for (int i = 0; i < n; ++i)
                    put16(g_rng() & 0xffff);
            } else {
                if (!(n & 1))
                    put16(0);
                for (int i = 0; i < n; ++i)
                    put32(g_rng());
            }
        }
    }
    auto *od = static_cast<uint8_t *>(OrigAlignedAlloc(v.size()));
    auto *pd = static_cast<uint8_t *>(AlignedAlloc(v.size(), 16));
    std::memcpy(od, v.data(), v.size());
    std::memcpy(pd, v.data(), v.size());
    for (size_t at : pointers) {
        uint32_t x;
        std::memcpy(&x, &v[at], 4);
        const uint32_t ox = x + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(od));
        const uint32_t px = x + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pd));
        std::memcpy(od + at, &ox, 4);
        std::memcpy(pd + at, &px, 4);
    }
    Field<void *>(g_origSets[k], 0x10) = od;
    g_portSets[k]->data = pd;
}

// A model whose animation stack the operation makes: the original leaves the new stack's dirty
// flag unset; it gets the port's.
struct StackBirth {
    int model;
    bool had;
    explicit StackBirth(int m) : model(m), had(static_cast<I3D_model *>(g_port[m])->animStack != nullptr) {}
    ~StackBirth()
    {
        I3D_anim_stack *stack = static_cast<I3D_model *>(g_port[model])->animStack;
        if (!had && stack && Field<uint8_t *>(g_orig[model], 0x130))
            Field<uint8_t>(Field<uint8_t *>(g_orig[model], 0x130), 4) = uint8_t(stack->dirty);
    }
};

// A model's own frames: children of the model, owned by it (again after it was made anew).
void SetUpModel(int m)
{
    for (int k = 0; k < kModelFrames; ++k) {
        const int f = ModelFrame(m, k);
        CallV<I3D_RESULT>(g_orig[f], 0x38, g_orig[m], 0u);
        g_port[f]->LinkTo(g_port[m], 0);
        CallV<void>(g_orig[m], 0x7C, g_orig[f]);
        static_cast<I3D_model *>(g_port[m])->AddFrame(g_port[f]);
    }
}

// A fresh occluder's near plane, unset by Init: the same in both.
void ResetNearPlane(int i)
{
    auto *q = static_cast<I3D_occluder *>(g_port[i]);
    q->nearPlane.normal = S_vector(0.0f, 0.0f, 1.0f);
    q->nearPlane.d = 0.0f;
    std::memcpy(g_orig[i] + 0x5B0, &q->nearPlane, 16);
}

// Before the original frees a sector's portals: they leave their scene's ring, as the port's
// destructor makes them (the original's stay).
void OrigPortalsLeaveScene(uint8_t *o)
{
    for (int k = 0; k < Field<int>(o, 0x254); ++k)
        OrigFn<ThisPtrFn>(0x1003ee20)(Field<void **>(o, 0x24C)[k], nullptr, nullptr);
}

// Takes a portal (sector frame i, index k) off the sector it leads to, in both.
void Disconnect(int i, int k)
{
    uint8_t *op = Field<uint8_t **>(g_orig[i], 0x24C)[k];
    I3D_portal *pp = static_cast<I3D_sector *>(g_port[i])->portals[k];
    if (uint8_t *to = Field<uint8_t *>(op, 0x198))
        OrigRemoveSwap(to + 0x25C, op);
    if (pp->connectedSector)
        pp->connectedSector->incomingPortals.RemoveSwap(pp);
    Field<void *>(op, 0x198) = nullptr;
    pp->connectedSector = nullptr;
}

void RunOp()
{
    const int i = Rand(kFrames);
    uint8_t *o = g_orig[i];
    I3D_frame *p = g_port[i];
    char text[160];
    switch (Rand(54)) {
    case 0:
    case 1: {
        const S_vector v = RandVector();
        g_lastOp = "SetWorldPos";
        CallV<void>(o, 0x08, &v);
        p->SetWorldPos(v);
        break;
    }
    case 2: {
        const S_vector d = RandVector();
        const float r = RandAngle();
        g_lastOp = "SetWorldDir";
        CallV<void>(o, 0x0C, &d, r);
        p->SetWorldDir(d, r);
        break;
    }
    case 3: {
        const S_vector v = RandVector(), d = RandVector(), u = RandVector();
        g_lastOp = "SetWorldPosDir3";
        CallV<void>(o, 0x10, &v, &d, &u);
        p->SetWorldPosDir3(v, d, u);
        break;
    }
    case 4: {
        const S_vector v = RandVector(), d = RandVector();
        const float r = RandAngle();
        g_lastOp = "SetWorldPosDir";
        CallV<void>(o, 0x14, &v, &d, r);
        p->SetWorldPosDir(v, d, r);
        break;
    }
    case 5: {
        const S_vector v = RandVector();
        const S_quat q = RandQuat();
        g_lastOp = "SetWorldPosRot";
        CallV<void>(o, 0x18, &v, &q);
        p->SetWorldPosRot(v, q);
        break;
    }
    case 6:
    case 7: {
        const S_vector d = RandVector();
        const float r = RandAngle();
        g_lastOp = "SetDir";
        CallV<void>(o, 0x1C, &d, r);
        p->SetDir(d, r);
        break;
    }
    case 8:
    case 9:
    case 10: {
        int t = Rand(kFrames + 3) - 3;
        if (t >= 0 && (IsDescendant(t, i) || Depth(g_port[t]) + SubtreeHeight(p) > 5))
            t = -1;
        if (t >= 0 && TypeOf(i) == FRAME_AREA) {
            // (The original's area LinkTo loops for ever below a sector without a scene and reads
            // through null without a sector: only parents in a sector with a scene, for both.)
            I3D_frame *f = g_port[t];
            while (f && f->GetType() != FRAME_SECTOR)
                f = f->parent;
            if (!f || !static_cast<I3D_sector *>(f)->scene)
                t = -1;
        }
        const uint32_t flags = g_rng() & 7;
        std::snprintf(text, sizeof text, "LinkTo %d flags %u", t, flags);
        g_lastOp = text;
        CallV<I3D_RESULT>(o, 0x38, t >= 0 ? g_orig[t] : nullptr, flags);
        p->LinkTo(t >= 0 ? g_port[t] : nullptr, flags);
        break;
    }
    case 11:
    case 12:
        g_lastOp = "Update";
        CallV<void>(o, 0x24);
        p->Update();
        break;
    case 13:
        g_lastOp = "UpdateBound";
        CallV<void>(o, 0x58);
        p->UpdateBound();
        break;
    case 14:
        g_lastOp = "UpdateWMatrixProc";
        OrigFn<ThisFn>(0x1001e390)(o, nullptr);
        p->UpdateWMatrix();
        break;
    case 15:
    case 16: {
        const char *n = RandName();
        std::snprintf(text, sizeof text, "SetName %s", n ? n : "(null)");
        g_lastOp = text;
        const int before = Orig<int>(0x100be474);
        const I3D_RESULT ro = CallV<I3D_RESULT>(o, 0x34, n);
        const I3D_RESULT rp = p->SetName(n);
        if (ro != rp)
            Fail("SetName result", i);
        const int after = Orig<int>(0x100be474);
        for (int c = before + 1; c <= after; ++c) {
            if ((c & 0x7ff) == 0x400)
                ClearStaleChainLinks(Orig<uint8_t *>(0x100be010));
        }
        break;
    }
    case 17: {
        const char *pre = kPrefixes[Rand(4)];
        std::snprintf(text, sizeof text, "SetNamePrefix %s", pre);
        g_lastOp = text;
        const I3D_RESULT ro = OrigFn<NamePrefixFn>(0x1001d470)(o, nullptr, pre);
        const I3D_RESULT rp = p->SetNamePrefix(pre);
        if (ro != rp)
            Fail("SetNamePrefix result", i);
        break;
    }
    case 18:
        g_lastOp = "RemoveNamePrefix";
        OrigFn<ThisFn>(0x1001d5d0)(o, nullptr);
        p->RemoveNamePrefix();
        break;
    case 19: {
        const char *m = kMasks[Rand(sizeof kMasks / sizeof *kMasks)];
        const uint32_t flags = RandEnumFlags();
        std::snprintf(text, sizeof text, "FindChildFrame %s 0x%x", m ? m : "(null)", flags);
        g_lastOp = text;
        const int ro = IndexOf(CallV<void *>(o, 0x48, m, flags), true);
        const int rp = IndexOf(p->FindChildFrame(m, flags), false);
        if (ro != rp)
            Fail("FindChildFrame result", i);
        break;
    }
    case 20: {
        const char *m = kMasks[Rand(sizeof kMasks / sizeof *kMasks)];
        const uint32_t flags = RandEnumFlags();
        g_answers.clear();
        for (int k = 0; k < 40; ++k)
            g_answers.push_back(Rand(8) == 0 ? 2 : Rand(6) == 0 ? 1 : 0);
        std::snprintf(text, sizeof text, "EnumFrames %s 0x%x", m ? m : "(null)", flags);
        g_lastOp = text;
        g_visits.clear();
        g_visitOrig = true;
        const I3D_RESULT ro = CallV<I3D_RESULT>(o, 0x44, &RecordProc, reinterpret_cast<void *>(0x1234), flags, m);
        const std::vector<int> visitsOrig = g_visits;
        g_visits.clear();
        g_visitOrig = false;
        const I3D_RESULT rp = p->EnumFrames(&RecordProc, reinterpret_cast<void *>(0x1234), flags, m);
        if (ro != rp || visitsOrig != g_visits)
            Fail("EnumFrames visits", i);
        break;
    }
    case 21: {
        // (Not itself: the original reads its freed name then; the port copies it first.)
        int s = Rand(kFrames + 1) - 1;
        if (s == i || (s >= 0 && TypeOf(i) == FRAME_MODEL && TypeOf(s) == FRAME_MODEL))
            s = -1;
        std::snprintf(text, sizeof text, "Duplicate %d", s);
        g_lastOp = text;
        if (TypeOf(i) == FRAME_SECTOR && s >= 0 && TypeOf(s) == FRAME_SECTOR)
            OrigPortalsLeaveScene(o);
        const I3D_RESULT ro = CallV<I3D_RESULT>(o, 0x4C, s >= 0 ? g_orig[s] : nullptr);
        const I3D_RESULT rp = p->Duplicate(s >= 0 ? g_port[s] : nullptr);
        if ((TypeOf(i) == FRAME_OCCLUDER && s >= 0 && TypeOf(s) == FRAME_OCCLUDER) ||
            (TypeOf(i) == FRAME_AREA && s >= 0 && TypeOf(s) == FRAME_AREA)) {
            // The rebuilt face planes come with MatrixChanged: run it in both.
            CallThisV(o, 0x68);
            p->MatrixChanged();
        }
        if (TypeOf(i) == FRAME_SECTOR && s >= 0 && TypeOf(s) == FRAME_SECTOR) {
            Field<int>(o, 0x270) = 0;       // (the port's FreeGeometry clears the plane count)
            // The new portals' world values are unset until MatrixChanged: run it in both.
            CallThisV(o, 0x68);
            p->MatrixChanged();
        }
        if (ro != rp)
            Fail("Duplicate result", i);
        break;
    }
    case 22: {
        const char *u = kProps[Rand(5)];
        g_lastOp = "SetUserProps";
        CallV<I3D_RESULT>(o, 0x2C, u);
        p->SetUserProps(u);
        break;
    }
    case 23: {
        const bool on = Rand(3) != 0;
        g_lastOp = "SetOn";
        CallV<void>(o, 0x30, on);
        p->SetOn(on);
        break;
    }
    case 24: {
        I3D_CALLBACK *cb = Rand(3) == 0 ? nullptr : Rand(2) ? &CallbackA : &CallbackB;
        const uint32_t messages = g_rng();
        g_lastOp = "SetCallback";
        CallV<void>(o, 0x28, cb, messages);
        p->SetCallback(cb, messages);
        break;
    }
    case 25: {
        // The SDK's inline scale / rotation setters (the port's code, then the same bytes in both).
        if (Rand(2)) {
            p->scale = S_vector(RandF(0.1f, 3.0f), RandF(0.1f, 3.0f), RandF(0.1f, 3.0f));
            if (Rand(2))
                p->scale = S_vector(1.0f, 1.0f, 1.0f);
            if (!(p->frameFlags & FRMFLAGS_ROT_QUAT_VALID))
                p->rot.Make(p->localMatrix);
        } else {
            p->rot = RandQuat();
            p->rot.Normalize();
        }
        p->frameFlags = (p->frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
        g_lastOp = "inline SetScale / SetRot";
        std::memcpy(o + 0xC0, reinterpret_cast<uint8_t *>(p) + 0xC0, 0x24);
        break;
    }
    case 26: {
        g_lastOp = "Release / AddRef";
        if (Rand(3) == 0) {
            ++Field<int16_t>(o, 0xE4);
            ++p->refCount;
            break;
        }
        const uint32_t ro = CallV<uint32_t>(o, 0x00);
        const uint32_t rp = p->Release();
        if (ro != rp)
            Fail("Release result", i);
        if (ro == 0 && Field<int16_t>(o, 0xE4) == 0) {
            // Dead: made again from the free lists.
            if (OrigCreate(TypeOf(i)) != o)
                Fail("the original's free list returned another frame", i);
            PortCreate(TypeOf(i), p);
            if (TypeOf(i) == FRAME_CAMERA) {
                CallV<void>(o, 0x8C, 4.0f / 3.0f);
                static_cast<I3D_camera *>(p)->SetAspectRatio(4.0f / 3.0f);
            }
            if (TypeOf(i) == FRAME_SECTOR)
                MakeGeometry(i);
            if (TypeOf(i) == FRAME_OCCLUDER)
                ResetNearPlane(i);
            if (TypeOf(i) == FRAME_MODEL)
                SetUpModel(i);
        }
        break;
    }
    case 27: {
        const char *n = RandName();
        std::snprintf(text, sizeof text, "driver FindFrame %s", n ? n : "(null)");
        g_lastOp = text;
        if (!n)
            break;
        const int ro = IndexOf(CallV<void *>(g_origDriver, 0x88, nullptr, n), true);
        const int rp = IndexOf(g_driver->FindFrame(nullptr, n), false);
        if (ro != rp)
            Fail("driver FindFrame result", -1);
        break;
    }
    case 28: {
        g_lastOp = "driver EnumNamedFrames";
        g_answers.clear();
        for (int k = 0; k < 40; ++k)
            g_answers.push_back(Rand(20) == 0 ? 1 : 0);
        g_visits.clear();
        g_visitOrig = true;
        const I3D_RESULT ro = CallV<I3D_RESULT>(g_origDriver, 0x90, &RecordProc, reinterpret_cast<void *>(0x1234));
        const std::vector<int> visitsOrig = g_visits;
        g_visits.clear();
        g_visitOrig = false;
        const I3D_RESULT rp = g_driver->EnumNamedFrames(&RecordProc, reinterpret_cast<void *>(0x1234));
        if (ro != rp || visitsOrig != g_visits)
            Fail("driver EnumNamedFrames visits", -1);
        break;
    }
    case 29:
        g_lastOp = "driver RebalanceNameTree";
        CallV<void>(g_origDriver, 0x8C);
        ClearStaleChainLinks(Orig<uint8_t *>(0x100be010));
        g_driver->RebalanceNameTree();
        break;
    case 30:
    case 31: {
        // A target: AddFrame / RemoveFrame / SetMode.
        const int t = Rand(kFrames / 10) * 10 + 2;
        auto *target = static_cast<I3D_target *>(g_port[t]);
        const int f = Rand(kFrames);
        switch (Rand(4)) {
        case 0:
        case 1:
            std::snprintf(text, sizeof text, "target %d AddFrame %d", t, f);
            g_lastOp = text;
            CallV<void>(g_orig[t], 0x74, g_orig[f]);
            target->AddFrame(g_port[f]);
            break;
        case 2: {
            std::snprintf(text, sizeof text, "target %d RemoveFrame %d", t, f);
            g_lastOp = text;
            const I3D_RESULT ro = CallV<I3D_RESULT>(g_orig[t], 0x78, g_orig[f]);
            const I3D_RESULT rp = target->RemoveFrame(g_port[f]);
            if (ro != rp)
                Fail("RemoveFrame result", t);
            break;
        }
        default: {
            const uint32_t mode = g_rng() & 0xF;
            std::snprintf(text, sizeof text, "target %d SetMode %u", t, mode);
            g_lastOp = text;
            CallV<void>(g_orig[t], 0x7C, mode);
            target->SetMode(mode);
            break;
        }
        }
        break;
    }
    case 32: {
        // A dummy: a new box (written into both) or BakeBox.
        const int d = Rand(kFrames / 10) * 10 + 1;
        auto *dummy = static_cast<I3D_dummy *>(g_port[d]);
        if (Rand(2)) {
            g_lastOp = "dummy box";
            const S_vector a(RandF(-5, 5), RandF(-5, 5), RandF(-5, 5));
            dummy->box.min = a;
            dummy->box.max = S_vector(a.x + RandF(0, 4), a.y + RandF(0, 4), a.z + RandF(0, 4));
            std::memcpy(g_orig[d] + 0x120, reinterpret_cast<uint8_t *>(dummy) + 0x120, 0x20);
        } else {
            const int mode = Rand(3);
            std::snprintf(text, sizeof text, "dummy %d BakeBox %d", d, mode);
            g_lastOp = text;
            CallV<void>(g_orig[d], 0x74, mode);
            dummy->BakeBox(mode);
        }
        break;
    }
    case 33: {
        // A volume's type (as the loader sets it).
        const int v = Rand(kFrames / 10) * 10 + 3;
        const int type = Rand(7);
        g_lastOp = "volume type";
        static_cast<I3D_volume *>(g_port[v])->volumeType = type;
        Field<int>(g_orig[v], 0x140) = type;
        break;
    }
    case 34:
    case 35: {
        // A camera's projection settings, or its sphere test.
        const int c = Rand(kFrames / 10) * 10 + 4;
        auto *camera = static_cast<I3D_camera *>(g_port[c]);
        uint8_t *oc = g_orig[c];
        const float f1 = Rand(4) ? RandF(0.0f, 3.5f) : RandF(-1.0f, 200.0f);
        const float f2 = Rand(4) ? RandF(0.0f, 3.5f) : RandF(-1.0f, 2000.0f);
        std::snprintf(text, sizeof text, "camera %d op %g %g", c, f1, f2);
        g_lastOp = text;
        switch (Rand(8)) {
        // (Angles below 3: at pi the cotangent is ~0 and the frustum leaves the float range.)
        case 0: CallV<void>(oc, 0x74, std::min(f1, 3.0f)); camera->SetFOV(std::min(f1, 3.0f)); break;
        case 1: CallV<void>(oc, 0x78, std::min(f1, 3.0f)); camera->SetNearSectorFOV(std::min(f1, 3.0f)); break;
        case 2: CallV<void>(oc, 0x7C, f1, f2); camera->SetRange(f1, f2); break;
        case 3: CallV<void>(oc, 0x80, f1, f2); camera->SetNearSectorRange(f1, f2); break;
        case 4: CallV<void>(oc, 0x84, f1); camera->SetOrthoScale(f1); break;
        case 5: {
            const bool on = Rand(3) == 0;
            CallV<void>(oc, 0x88, on);
            camera->SetOrthogonal(on);
            break;
        }
        case 6: CallV<void>(oc, 0x8C, f1); camera->SetAspectRatio(f1); break;
        default: {
            S_vector4 sphere;
            sphere.x = RandF(-50, 50);
            sphere.y = RandF(-50, 50);
            sphere.z = RandF(-50, 50);
            sphere.w = RandF(0, 20);
            const bool ro = CallV<bool>(oc, 0x90, &sphere);
            const bool rp = camera->IsSphereVisible(sphere);
            if (ro != rp)
                Fail("IsSphereVisible", c);
            break;
        }
        }
        break;
    }
    case 36:
    case 37: {
        // A light's settings.
        const int l = Rand(kFrames / 10) * 10 + 5;
        auto *light = static_cast<I3D_light *>(g_port[l]);
        uint8_t *ol = g_orig[l];
        const float f1 = RandF(-0.5f, 4.0f), f2 = RandF(-0.5f, 4.0f), f3 = RandF(0.0f, 2.0f);
        // (16 bytes: the original copies the dword after the vector too)
        const S_vector4 c4 = {RandF(0, 1), RandF(0, 1), RandF(0, 1), RandF(0, 1)};
        const S_vector &c = reinterpret_cast<const S_vector &>(c4);
        const int k = Rand(13);
        std::snprintf(text, sizeof text, "light %d op %d %g %g %g", l, k, f1, f2, f3);
        g_lastOp = text;
        switch (k) {
        case 0: {
            const int t = 1 + Rand(7);
            CallV<void>(ol, 0x74, t);
            light->SetLightType(t);
            break;
        }
        case 1: CallV<void>(ol, 0x78, f1, f2, f3); light->SetColorRGB(f1, f2, f3); break;
        case 2: CallV<void>(ol, 0x7C, &c4); light->SetColor(c); break;
        case 3: CallV<void>(ol, 0x80, &c4); light->SetSpecularColor(c); break;
        case 4: CallV<void>(ol, 0x84, &c4); light->SetAmbientColor(c); break;
        case 5: CallV<void>(ol, 0x88, f1); light->SetPower(f1); break;
        case 6: CallV<void>(ol, 0x8C, f1); light->SetSpecularPower(f1); break;
        case 7: CallV<void>(ol, 0x90, f1); light->SetAmbientPower(f1); break;
        case 8: {
            const uint32_t m = g_rng() & 0xffff;
            CallV<void>(ol, 0x94, m);
            light->SetMode(m);
            break;
        }
        case 9: CallV<void>(ol, 0x98, f1 * 50.0f); light->SetRange(f1 * 50.0f); break;
        case 10: CallV<void>(ol, 0x9C, f1, f2, f3); light->SetAttenuation(f1, f2, f3); break;
        case 11: CallV<void>(ol, 0xA0, f1 * 10.0f, f2 * 100.0f); light->SetFogRange(f1 * 10.0f, f2 * 100.0f); break;
        default: CallV<void>(ol, 0xA4, f1, f2); light->SetCone(f1, f2); break;
        }
        break;
    }
    case 38: {
        // A light into / out of a sector.
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        const int l = Rand(kFrames / 10) * 10 + 5;
        auto *sector = static_cast<I3D_sector *>(g_port[sIdx]);
        auto *light = static_cast<I3D_light *>(g_port[l]);
        if (Rand(3)) {
            std::snprintf(text, sizeof text, "sector %d AddLight %d", sIdx, l);
            g_lastOp = text;
            CallV<void>(g_orig[sIdx], 0x74, g_orig[l]);
            sector->AddLight(light);
        } else {
            std::snprintf(text, sizeof text, "sector %d RemoveLight %d", sIdx, l);
            g_lastOp = text;
            const I3D_RESULT ro = CallV<I3D_RESULT>(g_orig[sIdx], 0x78, g_orig[l]);
            const I3D_RESULT rp = sector->RemoveLight(light);
            if (ro != rp)
                Fail("RemoveLight result", sIdx);
        }
        break;
    }
    case 39: {
        // The sound environment.
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        auto *sector = static_cast<I3D_sector *>(g_port[sIdx]);
        uint8_t *os = g_orig[sIdx];
        const int index = Rand(24);
        S_vector4 pan = {RandF(-1.5f, 1.5f), RandF(-1.5f, 1.5f), RandF(-1.5f, 1.5f), RandF(-1, 1)};
        if (Rand(4) == 0)
            pan.x = pan.y = pan.z = 0.0f;
        uint32_t value;
        switch (Rand(4)) {
        case 0: value = static_cast<uint32_t>(Rand(30000) - 1000); break;
        case 1: {
            const float fv = Rand(8) ? RandF(-0.5f, 1.5f) : std::nanf("");
            std::memcpy(&value, &fv, 4);
            break;
        }
        default: value = g_rng(); break;
        }
        const uintptr_t arg = index == 20 || index == 21 ? reinterpret_cast<uintptr_t>(&pan) : value;
        const int k = Rand(4);
        std::snprintf(text, sizeof text, "sector %d environment op %d index %d value %08x", sIdx, k, index, value);
        g_lastOp = text;
        if (k == 0) {
            const int id = Rand(30);
            CallV<void>(os, 0x94, id);
            sector->SetEnvironment(id);
        } else if (k == 1 && index < 22) {
            uintptr_t ro = CallV<uintptr_t>(os, 0x88, index);
            uintptr_t rp = sector->GetEnvironmentParam(index);
            if (index >= 20) {
                ro -= reinterpret_cast<uintptr_t>(os);
                rp -= reinterpret_cast<uintptr_t>(sector);
            }
            if (ro != rp)
                Fail("GetEnvironmentParam result", sIdx);
        } else {
            const I3D_RESULT ro = CallV<I3D_RESULT>(os, 0x84, index, arg);
            const I3D_RESULT rp = sector->SetEnvironmentParam(index, arg);
            if (ro != rp)
                Fail("SetEnvironmentParam result", sIdx);
        }
        break;
    }
    case 40: {
        // Inside tests (points around the box, in world space).
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        auto *sector = static_cast<I3D_sector *>(g_port[sIdx]);
        // (The world matrix as it is: GetMatrix would update the port's only.)
        const S_vector c = sector->worldMatrix(3);
        S_vector pos(c.x + RandF(-40, 40), c.y + RandF(-40, 40), c.z + RandF(-40, 40));
        if (Rand(3) == 0)
            pos = ((sector->box.min + sector->box.max) * 0.5f) * sector->worldMatrix;
        std::snprintf(text, sizeof text, "sector %d inside %g %g %g", sIdx, pos.x, pos.y, pos.z);
        g_lastOp = text;
        if (Rand(2)) {
            const bool ro = CallV<bool>(g_orig[sIdx], 0x8C, &pos);
            const bool rp = sector->IsInside(pos);
            if (ro != rp)
                Fail("IsInside result", sIdx);
            g_seenInside += rp;
        } else {
            const float ro = CallV<float>(g_orig[sIdx], 0x90, &pos);
            const float rp = sector->InsideDistance(pos);
            if (std::memcmp(&ro, &rp, 4))
                Fail("InsideDistance result", sIdx);
        }
        break;
    }
    case 41: {
        // A portal: on / off, or its connections (set in both, as the scene does).
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        auto *sector = static_cast<I3D_sector *>(g_port[sIdx]);
        if (!sector->portals.Size())
            break;
        const int k = Rand(sector->portals.Size());
        uint8_t *op = Field<uint8_t **>(g_orig[sIdx], 0x24C)[k];
        I3D_portal *pp = sector->portals[k];
        const int to = Rand(kFrames / 10) * 10 + 6;
        std::snprintf(text, sizeof text, "sector %d portal %d op (to %d)", sIdx, k, to);
        g_lastOp = text;
        switch (Rand(5)) {
        case 0: {
            const bool on = Rand(2) != 0;
            CallV<void>(op, 0x00, on);
            pp->SetOn(on);
            break;
        }
        case 1:
            if (CallV<bool>(op, 0x04) != pp->IsOn())
                Fail("portal IsOn", sIdx);
            break;
        case 2:
            // Leads to sector `to`.
            Disconnect(sIdx, k);
            Field<void *>(op, 0x198) = g_orig[to];
            OrigPushBack(g_orig[to] + 0x25C, op);
            pp->connectedSector = static_cast<I3D_sector *>(g_port[to]);
            pp->connectedSector->incomingPortals.PushBack(pp);
            break;
        case 3:
            Disconnect(sIdx, k);
            break;
        default: {
            // The other side: a portal of sector `to` (not itself).
            auto *other = static_cast<I3D_sector *>(g_port[to]);
            if (!other->portals.Size())
                break;
            const int m = Rand(other->portals.Size());
            uint8_t *oq = Field<uint8_t **>(g_orig[to], 0x24C)[m];
            I3D_portal *pq = other->portals[m];
            if (pq == pp)
                break;
            for (uint8_t *x : {op, oq}) {
                if (uint8_t *side = Field<uint8_t *>(x, 0x19C))
                    Field<void *>(side, 0x19C) = nullptr;
            }
            for (I3D_portal *x : {pp, pq}) {
                if (x->otherSide)
                    x->otherSide->otherSide = nullptr;
            }
            Field<void *>(op, 0x19C) = oq;
            Field<void *>(oq, 0x19C) = op;
            pp->otherSide = pq;
            pq->otherSide = pp;
            break;
        }
        }
        break;
    }
    case 42: {
        // A sector's scene.
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        const int k = Rand(3) - 1;
        std::snprintf(text, sizeof text, "sector %d SetScene %d", sIdx, k);
        g_lastOp = text;
        OrigFn<ThisPtrFn>(0x1004cd40)(g_orig[sIdx], nullptr, k >= 0 ? g_origScenes[k] : nullptr);
        static_cast<I3D_sector *>(g_port[sIdx])->SetScene(PortScene(k));
        break;
    }
    case 43: {
        // New geometry (the old freed in both).
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        std::snprintf(text, sizeof text, "sector %d new geometry", sIdx);
        g_lastOp = text;
        OrigPortalsLeaveScene(g_orig[sIdx]);
        OrigFn<ThisFn>(0x1004cbc0)(g_orig[sIdx], nullptr);
        Field<int>(g_orig[sIdx], 0x270) = 0;    // (as the port's FreeGeometry)
        static_cast<I3D_sector *>(g_port[sIdx])->FreeGeometry();
        MakeGeometry(sIdx);
        break;
    }
    case 44: {
        // An occluder: a new mesh, the renderer's view flag and depth, its sector (as MakeReady
        // links them), or its occlusion volume.
        const int q = Rand(kFrames / 10) * 10 + 7;
        auto *occ = static_cast<I3D_occluder *>(g_port[q]);
        uint8_t *oq = g_orig[q];
        const int k = Rand(5);
        std::snprintf(text, sizeof text, "occluder %d op %d", q, k);
        g_lastOp = text;
        switch (k) {
        case 0: MakeOccluderMesh(q); break;
        case 1:
            occ->frameFlags ^= FRMFLAGS_IN_VIEW;
            Field<uint32_t>(oq, 0xE0) ^= FRMFLAGS_IN_VIEW;
            break;
        case 2: {
            occ->viewDepth = Rand(4) ? RandF(0.0f, 60.0f) : RandF(-5.0f, 0.0f);
            std::memcpy(oq + 0x5F0, &occ->viewDepth, 4);
            break;
        }
        case 3: {
            const int sIdx = Rand(kFrames / 10) * 10 + 6;
            auto *sector = static_cast<I3D_sector *>(g_port[sIdx]);
            if (sector->occluder || occ->ownerSector)
                break;
            sector->occluder = occ;
            occ->ownerSector = sector;
            Field<void *>(g_orig[sIdx], 0x284) = oq;
            Field<void *>(oq, 0x5EC) = g_orig[sIdx];
            break;
        }
        default:
            OrigFn<void(__fastcall *)(void *, void *)>(0x100390f0)(oq, nullptr);
            occ->UpdateOcclusionVolume();
            break;
        }
        break;
    }
    case 45: {
        // The renderer's passes: a scene's occluder lists, a sector's portals, the occlusion of the
        // rendered sectors (sector flags 1 and 0x80 toggled in both first, now and then).
        const int k = Rand(2);
        const int sIdx = Rand(kFrames / 10) * 10 + 6;
        const int m = Rand(4);
        std::snprintf(text, sizeof text, "occlusion pass %d scene %d sector %d", m, k, sIdx);
        g_lastOp = text;
        switch (m) {
        case 0:
            OrigFn<void(__fastcall *)(void *)>(0x10039ee0)(g_origScenes[k]);
            RebuildOccluderLists(PortScene(k));
            break;
        case 1:
            OrigFn<void(__fastcall *)(void *)>(0x1003a5a0)(g_orig[sIdx]);
            OccludePortals(static_cast<I3D_sector *>(g_port[sIdx]));
            break;
        case 2:
            OrigFn<void(__fastcall *)(void *)>(0x1003a000)(g_origScenes[k]);
            OccludeSectors(PortScene(k));
            break;
        default: {
            const uint32_t bit = Rand(2) ? SCTF_VISIBLE : SCTF_ON_PATH;
            static_cast<I3D_sector *>(g_port[sIdx])->sectorFlags ^= bit;
            Field<uint32_t>(g_orig[sIdx], 0x160) ^= bit;
            break;
        }
        }
        break;
    }
    case 47:
    case 48: {
        // An area: a new mesh, an inside test, its unknown value.
        const int ai = Rand(kFrames / 10) * 10 + 8;
        auto *area = static_cast<I3D_area *>(g_port[ai]);
        uint8_t *oa = g_orig[ai];
        const int k = Rand(4);
        std::snprintf(text, sizeof text, "area %d op %d", ai, k);
        g_lastOp = text;
        switch (k) {
        case 0: MakeAreaMesh(ai); break;
        case 1: {
            const uint32_t value = Rand(4) ? g_rng() % 1000 : g_rng();
            if (CallV<uint32_t>(oa, 0x84, value) != area->SetFadeTime(value) ||
                CallV<uint32_t>(oa, 0x88) != area->GetFadeTime())
                Fail("area fadeTime", ai);
            break;
        }
        default: {
            // (around the world position, or the center of its world vertices)
            S_vector pos = area->worldMatrix(3) + S_vector(RandF(-12, 12), RandF(-12, 12), RandF(-12, 12));
            if (area->numVertices && Rand(2)) {
                S_vector sum(0, 0, 0);
                for (int n = 0; n < area->numVertices; ++n)
                    sum = sum + reinterpret_cast<const S_vector &>(area->worldVertices[n]);
                pos = sum * (1.0f / area->numVertices);
            }
            const bool ro = CallV<bool>(oa, 0x80, &pos);
            const bool rp = area->IsInside(pos);
            if (ro != rp)
                Fail("area IsInside", ai);
            g_seenInside += rp;
            break;
        }
        }
        break;
    }
    case 49:
    case 50: {
        // A model's animation stages.
        const int m = Rand(kModels) * 10 + 9;
        auto *model = static_cast<I3D_model *>(g_port[m]);
        uint8_t *om = g_orig[m];
        const StackBirth birth(m);
        const int stage = Rand(10) == 0 ? 8 + Rand(2) : Rand(8);
        const int k = Rand(12);
        std::snprintf(text, sizeof text, "model %d anim op %d stage %d", m, k, stage);
        g_lastOp = text;
        switch (k) {
        case 0: {
            const int set = Rand(kSets + 1) - 1;
            const float weight = Rand(3) ? RandF(0.0f, 1.0f) : RandF(-0.5f, 1.5f);
            const int mode = Rand(3);
            const I3D_RESULT ro = CallV<I3D_RESULT>(om, 0x84, set >= 0 ? g_origSets[set] : nullptr, stage, weight, mode);
            const I3D_RESULT rp = model->SetAnimation(set >= 0 ? g_portSets[set] : nullptr, stage, weight, mode);
            if (ro != rp)
                Fail("SetAnimation result", m);
            break;
        }
        case 1:
        case 11: {
            const int dt = k == 11 ? 500 + Rand(2500) : Rand(4) ? Rand(150) - 40 : 0;
            CallV<void>(om, 0x20, dt);
            model->Tick(dt);
            break;
        }
        case 2: {
            int deltas[8];
            for (int &d : deltas)
                d = Rand(3) ? Rand(150) - 40 : 0;
            CallV<void>(om, 0xB0, &deltas);
            model->TickStages(deltas);
            break;
        }
        case 3: CallV<void>(om, 0x8C, stage); model->StopAnimation(stage); break;
        case 4:
            // (Not a stage without a set: the original's next Tick reads the set through null.)
            if (model->GetAnimation(stage)) {
                CallV<void>(om, 0x90, stage);
                model->StartAnimation(stage);
            }
            break;
        case 5: {
            const float w = Rand(4) ? RandF(-0.5f, 1.5f) : std::nanf("");
            if (CallV<I3D_RESULT>(om, 0x94, stage, w) != model->SetAnimWeight(stage, w))
                Fail("SetAnimWeight result", m);
            break;
        }
        case 6: {
            const int t = Rand(3000) - 500;
            if (CallV<I3D_RESULT>(om, 0x9C, stage, t) != model->SetAnimTime(stage, t))
                Fail("SetAnimTime result", m);
            break;
        }
        case 7: {
            const int mode = Rand(3);
            if (CallV<I3D_RESULT>(om, 0xA4, stage, mode) != model->SetAnimMode(stage, mode))
                Fail("SetAnimMode result", m);
            break;
        }
        case 8: {
            const float wo = CallV<float>(om, 0x98, stage), wp = model->GetAnimWeight(stage);
            if (SetIndex(CallV<void *>(om, 0x88, stage), true) != SetIndex(model->GetAnimation(stage), false) ||
                !SameField(reinterpret_cast<const uint8_t *>(&wo), reinterpret_cast<const uint8_t *>(&wp)) ||
                CallV<int>(om, 0xA0, stage) != model->GetAnimTime(stage) ||
                CallV<int>(om, 0xA8, stage) != model->GetAnimMode(stage) ||
                CallV<bool>(om, 0xAC, stage) != model->IsAnimActive(stage))
                Fail("anim getters", m);
            break;
        }
        case 9: {
            // One of its frames out of the list and back (the list's order changes).
            const int f = ModelFrame(m, Rand(kModelFrames));
            const I3D_RESULT ro = CallV<I3D_RESULT>(om, 0x80, g_orig[f]);
            const I3D_RESULT rp = model->RemoveFrame(g_port[f]);
            if (ro != rp)
                Fail("RemoveFrame result", m);
            if (ro == I3D_OK) {
                CallV<void>(om, 0x7C, g_orig[f]);
                model->AddFrame(g_port[f]);
            }
            break;
        }
        default:
            if (CallV<int>(om, 0xD0) != model->NumTracks())
                Fail("NumTracks", m);
            break;
        }
        break;
    }
    case 51:
    case 52: {
        // Poses, the pose set and a model's poses.
        const int m = Rand(kModels) * 10 + 9;
        auto *model = static_cast<I3D_model *>(g_port[m]);
        uint8_t *om = g_orig[m];
        const StackBirth birth(m);
        const int pi = Rand(kPoses);
        uint8_t *op = g_origPoses[pi];
        I3D_pose *pp = g_portPoses[pi];
        const S_vector4 v4 = {RandF(-3, 3), RandF(-3, 3), RandF(-3, 3), RandF(-1, 1)};
        S_quat q = RandQuat();
        q.Normalize();
        const S_vector dir = RandVector();
        const float roll = RandAngle();
        const bool none = Rand(5) == 0;
        const char *name = kTrackNames[Rand(4)];
        const float w = Rand(4) ? RandF(-0.2f, 1.2f) : 1.0f;
        const int k = Rand(16);
        std::snprintf(text, sizeof text, "pose op %d pose %d model %d", k, pi, m);
        g_lastOp = text;
        switch (k) {
        case 0: CallV<void>(op, 0x0C, none ? nullptr : &v4); pp->SetPosition(none ? nullptr : &v4); break;
        case 1: CallV<void>(op, 0x10, none ? nullptr : &v4); pp->SetScale(none ? nullptr : &v4); break;
        case 2: CallV<void>(op, 0x14, none ? nullptr : &q); pp->SetRotationAdd(none ? nullptr : &q); break;
        case 3: CallV<void>(op, 0x18, none ? nullptr : &q); pp->SetRotationSet(none ? nullptr : &q); break;
        case 4: CallV<void>(op, 0x1C, &dir, roll); pp->SetDirAdd(dir, roll); break;
        case 5: CallV<void>(op, 0x20, none ? nullptr : &dir, roll); pp->SetDirAddOnly(none ? nullptr : &dir, roll); break;
        case 6: CallV<void>(op, 0x24, &dir, roll); pp->SetDirSet(dir, roll); break;
        case 7: CallV<void>(op, 0x08); pp->Clear(); break;
        case 8: CallV<void>(g_origPoseSet, 0x04, op, name); g_portPoseSet->AddPose(pp, name); break;
        case 9:
            if (CallV<I3D_RESULT>(g_origPoseSet, 0x08, op) != g_portPoseSet->RemovePose(pp))
                Fail("RemovePose result", -1);
            break;
        case 10: {
            const int f = ModelFrame(m, Rand(kModelFrames));
            if (CallV<I3D_RESULT>(om, 0xBC, g_orig[f], none ? nullptr : op) != model->SetPose(g_port[f], none ? nullptr : pp))
                Fail("SetPose result", m);
            break;
        }
        case 11:
            if (CallV<I3D_RESULT>(om, 0xB8, name, none ? nullptr : op) != model->SetPoseByName(name, none ? nullptr : pp))
                Fail("SetPoseByName result", m);
            break;
        case 12:
            if (CallV<I3D_RESULT>(om, 0xB4, g_origPoseSet) != model->SetPoseSet(g_portPoseSet))
                Fail("SetPoseSet result", m);
            break;
        case 13:
            if (CallV<I3D_RESULT>(om, 0xC0, w, g_origPoseSet) != model->SetPoseSetWeight(w, g_portPoseSet) ||
                CallV<I3D_RESULT>(om, 0xC4, w) != model->SetPoseWeight(w))
                Fail("SetPoseWeight results", m);
            break;
        case 14: {
            const float wo = CallV<float>(om, 0xC8), wp = model->GetPoseWeight();
            if (!SameField(reinterpret_cast<const uint8_t *>(&wo), reinterpret_cast<const uint8_t *>(&wp)))
                Fail("GetPoseWeight", m);
            break;
        }
        default:
            CallV<void>(om, 0xCC);
            model->ClearPoses();
            break;
        }
        break;
    }
    case 53: {
        // An animation set's keys.
        const int si = Rand(kSets);
        uint8_t *os = g_origSets[si];
        I3D_animation_set *ps = g_portSets[si];
        const int t = Rand(4) - (Rand(8) == 0 ? 1 : 0);
        const int key = Rand(6);
        std::snprintf(text, sizeof text, "set %d track %d key %d", si, t, key);
        g_lastOp = text;
        if (CallV<int>(os, 0x0C) != ps->NumTracks() || CallV<int>(os, 0x08) != ps->GetLength() ||
            (t >= 0 && !SameString(CallV<const char *>(os, 0x10, t), ps->GetTrackName(t))))
            Fail("set header", si);
        {
            const char *n = kTrackNames[Rand(4)];
            if (CallV<int>(os, 0x14, n) != ps->FindTrack(n))
                Fail("FindTrack", si);
        }
        if (CallV<int>(os, 0x18, t) != ps->NumPositionKeys(t) || CallV<int>(os, 0x20, t) != ps->NumRotationKeys(t) ||
            CallV<int>(os, 0x28, t) != ps->NumScaleKeys(t))
            Fail("key counts", si);
        int to = -1, tp = -1, io = -1, ip = -1;
        if (CallV<int>(os, 0x30, t, &io) != ps->NumNoteKeys(t, &ip) || io != ip)
            Fail("note key count", si);
        S_vector vo{}, vp{};
        S_quat qo{}, qp{};
        if (CallV<I3D_RESULT>(os, 0x1C, t, key, &to, &vo) != ps->GetPositionKey(t, key, &tp, &vp) || to != tp ||
            std::memcmp(&vo, &vp, 12))
            Fail("GetPositionKey", si);
        if (CallV<I3D_RESULT>(os, 0x24, t, key, &to, &qo) != ps->GetRotationKey(t, key, &tp, &qp) || to != tp ||
            std::memcmp(&qo, &qp, 16))
            Fail("GetRotationKey", si);
        if (CallV<I3D_RESULT>(os, 0x2C, t, key, &to, &vo) != ps->GetScaleKey(t, key, &tp, &vp) || to != tp ||
            std::memcmp(&vo, &vp, 12))
            Fail("GetScaleKey", si);
        uint16_t ho = 0, hp = 0;
        uint32_t wo = 0, wp = 0;
        if (CallV<I3D_RESULT>(os, 0x34, t, key, &to, &ho) != ps->GetNoteKey16(t, key, &tp, &hp) || to != tp || ho != hp)
            Fail("GetNoteKey16", si);
        if (CallV<I3D_RESULT>(os, 0x38, t, key, &to, &wo) != ps->GetNoteKey32(t, key, &tp, &wp) || to != tp || wo != wp)
            Fail("GetNoteKey32", si);
        break;
    }
    case 46: {
        // The render camera moves (frame 4, in both: a position and direction near the frames).
        const S_vector pos(RandF(-30, 30), RandF(-30, 30), RandF(-30, 30));
        const S_vector dir(RandF(-1, 1), RandF(-1, 1), RandF(-1, 1));
        g_lastOp = "render camera";
        CallV<void>(g_orig[4], 0x14, &pos, &dir, 0.0f);
        g_port[4]->SetWorldPosDir(pos, dir, 0.0f);
        break;
    }
    }
}

// Keeps the frames' values realistic (see the top): out-of-range scales and positions are reset
// in both, the way the SDK's inline setters would.
// How often the occlusion paths had something to work on (printed at the end).
long g_seenActive, g_seenStraddle, g_seenOccludedPortal, g_seenInScene, g_seenListed;
void CountCoverage()
{
    bool active = false, straddle = false, portal = false, inScene = false, listed = false;
    for (int i = 0; i < kFrames; ++i) {
        if (TypeOf(i) == FRAME_OCCLUDER) {
            const auto *q = static_cast<const I3D_occluder *>(g_port[i]);
            active |= (q->occluderFlags & OCCF_ACTIVE) != 0;
            straddle |= (q->occluderFlags & OCCF_STRADDLES_PORTALS) != 0;
            inScene |= q->scene != nullptr;
        } else if (TypeOf(i) == FRAME_SECTOR) {
            const auto *sector = static_cast<const I3D_sector *>(g_port[i]);
            listed |= sector->occluders != nullptr;
            for (const I3D_portal *pt : sector->portals)
                portal |= (pt->portalFlags & PRTF_OCCLUDED) != 0;
        }
    }
    for (int m = 9; m < kFrames; m += 10) {
        const I3D_anim_stack *stack = static_cast<const I3D_model *>(g_port[m])->animStack;
        if (!stack)
            continue;
        g_seenTracks += stack->tracks.Size() > 0;
        bool posed = false, playing = false;
        for (const I3D_anim_track *t : stack->tracks)
            posed |= t->pose != nullptr;
        for (const I3D_anim_stack::Stage &st : stack->stages)
            playing |= st.active && st.set;
        g_seenPosed += posed;
        g_seenPlaying += playing;
    }
    g_seenActive += active;
    g_seenStraddle += straddle;
    g_seenOccludedPortal += portal;
    g_seenInScene += inScene;
    g_seenListed += listed;
}

void Sanitize()
{
    for (int i = 0; i < kTotal; ++i) {
        I3D_frame *p = g_port[i];
        const S_vector &s = p->scale;
        bool fix = false;
        for (float c : {s.x, s.y, s.z})
            fix |= !(std::fabs(c) >= 0.1f && std::fabs(c) <= 10.0f);
        if (fix) {
            p->scale = S_vector(1.0f, 1.0f, 1.0f);
            if (!(p->frameFlags & FRMFLAGS_ROT_QUAT_VALID))
                p->rot.Make(p->localMatrix);
            p->frameFlags = (p->frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
            std::memcpy(g_orig[i] + 0xC0, reinterpret_cast<uint8_t *>(p) + 0xC0, 0x24);
        }
        const S_vector &pos = p->localMatrix(3);
        if (!(std::fabs(pos.x) <= 1e4f && std::fabs(pos.y) <= 1e4f && std::fabs(pos.z) <= 1e4f)) {
            p->localMatrix(3) = S_vector(0.0f, 0.0f, 0.0f);
            p->frameFlags = (p->frameFlags & ~0x120u) | FRMFLAGS_CHANGED;
            std::memcpy(g_orig[i] + 0xB0, reinterpret_cast<uint8_t *>(p) + 0xB0, 0xC);
            std::memcpy(g_orig[i] + 0xE0, reinterpret_cast<uint8_t *>(p) + 0xE0, 4);
        }
    }
}

// A crash names the operation it happened in.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
    std::printf("CRASH (0x%08lx at %p) in op %ld (%s)\n", info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress, g_op, g_lastOp.c_str());
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 200000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    HMODULE dll = LoadLibraryA(origPath);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", origPath, GetLastError());
        return 2;
    }
    g_base = reinterpret_cast<uint8_t *>(dll);
    Orig<uint8_t>(0x100d16f2) = 1;              // no log
    g_origDriver = Orig<uint8_t *>(0x100d16f8);
    if (!g_origDriver) {
        std::printf("the original's driver does not exist\n");
        return 2;
    }
    _controlfp(_PC_24, _MCW_PC);
    // Named frames enter the name tree on both sides.
    Orig<uint32_t>(0x100be058 + 4 * DRVSTATE_FRAME_NAMES) = 1;
    g_driverStates[DRVSTATE_FRAME_NAMES] = 1;
    g_driver = new I3D_driver;
    g_pIGraph = Orig<IGraph *>(0x100d16f4);
    Orig<IDirect3DDevice8 *>(0x100d216c) = &g_origDevice;
    ls3d::ig::g_pDevice = &g_portDevice;
    if (!g_pIGraph) {
        std::printf("the original's IGraph does not exist\n");
        return 2;
    }

    g_portalChanges = Orig<int>(0x100bf350);
    g_sectorChanges = Orig<int>(0x100bf354);
    g_rng.seed(seed);
    for (int i = 0; i < kFrames; ++i) {
        g_orig[i] = OrigCreate(TypeOf(i));
        g_port[i] = PortCreate(TypeOf(i), nullptr);
        // (A camera's view matrices and frustums are set by its first MatrixChanged; the aspect
        // ratio from IGraph's screen, which has no device here, is 0 / 0.)
        if (TypeOf(i) == FRAME_CAMERA) {
            CallV<void>(g_orig[i], 0x8C, 4.0f / 3.0f);
            static_cast<I3D_camera *>(g_port[i])->SetAspectRatio(4.0f / 3.0f);
            CallV<void>(g_orig[i], 0x24);
            g_port[i]->Update();
        }
        if (TypeOf(i) == FRAME_SECTOR)
            MakeGeometry(i);
        if (TypeOf(i) == FRAME_OCCLUDER)
            ResetNearPlane(i);
    }
    // The models' own frames (named after the tracks, with the recording callback), the
    // animation sets, poses and the pose set.
    for (int i = kFrames; i < kTotal; ++i) {
        g_orig[i] = OrigCreate(FRAME_USER);
        g_port[i] = PortCreate(FRAME_USER, nullptr);
        const char *n = kTrackNames[(i - kFrames) % kModelFrames];
        CallV<I3D_RESULT>(g_orig[i], 0x34, n);
        g_port[i]->SetName(n);
        CallV<void>(g_orig[i], 0x28, &RecordCallback, 0x1a00u);
        g_port[i]->SetCallback(&RecordCallback, 0x1a00);
    }
    for (int m = 9; m < kFrames; m += 10)
        SetUpModel(m);
    for (int k = 0; k < kSets; ++k) {
        g_origSets[k] = CallV<uint8_t *>(g_origDriver, 0x70);
        g_portSets[k] = new I3D_animation_set;
        MakeSet(k);
    }
    for (int k = 0; k < kPoses; ++k) {
        g_origPoses[k] = CallV<uint8_t *>(g_origDriver, 0x74);
        g_portPoses[k] = new I3D_pose;
    }
    g_origPoseSet = CallV<uint8_t *>(g_origDriver, 0x78);
    g_portPoseSet = new I3D_pose_set;
    // The stand-in scenes' primary sectors; frame 4 renders; occlusion on.
    for (int k = 0; k < 2; ++k) {
        g_origPrimary[k] = OrigCreate(FRAME_SECTOR);
        g_portPrimary[k] = static_cast<I3D_sector *>(PortCreate(FRAME_SECTOR, nullptr));
        Field<void *>(g_origPrimary[k], 0x26C) = g_origScenes[k];
        g_portPrimary[k]->scene = PortScene(k);
        Field<void *>(g_origScenes[k], 0x1F8) = g_origPrimary[k];
        PortScene(k)->primarySector = g_portPrimary[k];
    }
    Orig<void *>(0x100d127c) = g_orig[4];
    g_renderCamera = static_cast<I3D_camera *>(g_port[4]);
    Orig<uint32_t>(0x100be058 + 4 * DRVSTATE_OCCLUSION) = 1;
    g_driverStates[DRVSTATE_OCCLUSION] = 1;
    g_lastOp = "create";
    CompareAll();
    for (g_op = 0; g_op < ops; ++g_op) {
        RunOp();
        CompareAll();
        CountCoverage();
        Sanitize();
    }
    std::printf("coverage (operations after which): occluder active %ld, straddling %ld, portal occluded %ld, "
                "occluder in a scene %ld, sector occluder list %ld; IsInside true %ld\n",
                g_seenActive, g_seenStraddle, g_seenOccludedPortal, g_seenInScene, g_seenListed, g_seenInside);
    std::printf("animation coverage: callbacks %ld; model stacks with tracks %ld, posed %ld, playing %ld\n",
                g_seenCallbacks, g_seenTracks, g_seenPosed, g_seenPlaying);
    std::printf("compare_frame: %ld operations, seed %u: identical\n", ops, seed);
    return 0;
}
