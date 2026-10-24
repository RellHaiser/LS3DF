// Differential test of I3D_lit_object::Construct (src/i3d/lightmap_build.cpp: the lightmap baker
// and the light at a point): two lit objects made by the original LS3DF.dll's driver, in the same
// sector with the same lights and the same mesh, are computed - one by the original's Construct
// (slot 0xB4), one by the port's - and their current set's levels compared: the vertex colours, or
// the patches (sizes, pixels, uniform colours), the lightmap vertices (vertex, patch, uv bits) and
// every face's patch; the return values and the progress messages (but the timed ones, state 1).
// Packing the patches into the atlas (S_lightmapLevel::Build) is left out on both sides: the
// original's call is patched to a stub, the port's Build is the test's.
//
// Each round: 1 - 3 mesh levels of boxes, bent strips, bumpy discs and loose triangles (corners
// shared by position, by index, or not at all; face groups with their own materials: colours,
// opacity, MATF_COLORS or not), 0 - 6 lights of every type with random colours, powers, ranges,
// attenuations, cones, modes and on / off, the object's place (or world space), the set's resolution,
// smoothing angle and kind (vertex colours or patches), the samples per texel, Construct's flags
// (blur, test pattern, one patch per level for one-level meshes, ignored bits), a progress callback
// or none.
//
// Shadows: the objects are in the primary sector of a scene of the original's with an object
// visual of loose shapes among them and the lights; with LMCONS_SHADOWS and the scene the lights'
// rays are traced by the original's TestRay through it and the lit object itself (only the one
// being computed is on, so both sides see the same faces).
//
// Left out: meshes the original misreads (faces with all three corners in one place: it keeps its
// stack's rectangle; LMCONS_ONE_PATCH on several levels, or without a callback: it makes the patch
// again, without a callback for ever).
//
// Both runs start from the same state: the original's TestRay keeps part of each query's scratch
// state (the last hit face) for the next one, so it is saved before the original's run and put back
// before the port's; each twin's bounds are made again before its run (the collision culls by them,
// and the test sets VISF_WORLD_SPACE past the engine's bookkeeping); the lights must not change.
//
// Usage: compare_lightmap.exe [original LS3DF.dll] [rounds] [seed] [scene: 1 (default) / 0]
//        [watchdog seconds a round may take: 120] [1: the original computes both (harness check)]
//        [trace: 1 - each sample of the port's run lit by both light functions from the same collision
//        state, the first difference printed with the lights one by one; 2 - both runs' TestRay queries
//        logged, the first different one printed]
//
// test-sources: src/i3d/lightmap_build.cpp src/math/math.cpp src/core/debug.cpp src/dx/dxerr8.cpp
#include <float.h>

#include <algorithm>
#include <cmath>

#include "../src/core/debug.h"
#include "../src/i3d/collision.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/light.h"
#include "../src/i3d/litobject.h"
#include "../src/i3d/material.h"
#include "../src/i3d/mesh.h"
#include "../src/i3d/render_helpers.h"
#include "../src/i3d/scene.h"
#include "../src/i3d/sector.h"
#include "../src/igraph/igraph_state.h"
#include "buffer_check.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d::i3d {
uint32_t g_driverStates[DRVSTATE_COUNT];
namespace {
I3D_sector *s_lightSector;
}
void SetLightSector(I3D_sector *sector) { s_lightSector = sector; }
I3D_sector *GetLightSector() { return s_lightSector; }
// The original's filter on both sides (the test is about Construct).
int __stdcall TransmitFilter(const void *reserved, float t, I3D_frame *frame, void *user, const S_collisionTexel *texel)
{
    return OrigFn<I3D_COLLISION_FILTER *>(0x10064860)(reserved, t, frame, user, texel);
}

// The port's level methods Construct calls (Build: the atlas is not made).
void S_lightmapLevel::Free()
{
    ::operator delete(uvs);
    uvs = nullptr;
    ::operator delete(unknown08);
    unknown08 = nullptr;
    if (faceParts) {
        for (int i = 0; i < numFaceGroups; ++i)
            ::operator delete(faceParts[i]);
        ::operator delete(faceParts);
    }
    faceParts = nullptr;
    for (int i = 0; i < numParts; ++i) {
        if (!parts[i].IsUniform())
            ::operator delete(parts[i].pixels);
    }
    numParts = 0;
    ::operator delete(parts);
    parts = nullptr;
    ::operator delete(vertexMap);
    vertexMap = nullptr;
    ::operator delete(colors);
    colors = nullptr;
}
bool S_lightmapLevel::Build(I3D_mesh_level *, bool, const I3D_frame *) { return true; }
void S_lightmapLevel::SetFaceParts(uint16_t *p, int faceGroup)
{
    if (faceGroup < numFaceGroups)
        faceParts[faceGroup] = p;
}
}  // namespace ls3d::i3d
namespace ls3d::ig {
IDirect3DDevice8 *g_pDevice;
bool g_noActivation;
D3DMATERIAL8 g_defaultMaterial;
}  // namespace ls3d::ig

// The original's world matrix update (a __thiscall).
void I3D_frame::UpdateWMatrixProc() { OrigFn<void(__fastcall *)(I3D_frame *, void *)>(0x1001e390)(this, nullptr); }

using namespace ls3d::i3d;

namespace {

uint8_t *g_origDriver;
volatile long g_roundStarted;               // GetTickCount when the current round began
volatile int g_side = -1;                   // 0 the original computing, 1 the port, -1 neither
bool g_bothOriginal;                        // (harness check: the original computes both objects)

DWORD WINAPI Watchdog(void *limitSeconds)
{
    const DWORD limit = static_cast<DWORD>(reinterpret_cast<uintptr_t>(limitSeconds)) * 1000;
    for (;;) {
        Sleep(1000);
        const DWORD started = static_cast<DWORD>(g_roundStarted);
        if (started && GetTickCount() - started > limit) {
            std::printf("STUCK in round %ld (%s) on the %s side\n", g_op, g_lastOp.c_str(),
                        g_side == 0 ? "original's" : g_side == 1 ? "port's" : "test's");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 4);
        }
    }
}

template <class R, class... A> R CallV(void *object, int slot, A... args)
{
    using F = R(__stdcall *)(void *, A...);
    return (*reinterpret_cast<F **>(object))[slot / 4](object, args...);
}
template <class T> T &Field(void *object, size_t offset) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset); }

float RandF(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g_rng() % 1000001) / 1000000.0f; }

// The original's Build, as Construct calls it (__thiscall, 3 arguments).
bool __fastcall OrigBuildStub(void *, void *, void *, int, void *) { return true; }

void PatchOrigBuild()
{
    uint8_t *call = &Orig<uint8_t>(0x10027c6b);
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    if (call[0] != 0xe8 || call + 5 + rel != &Orig<uint8_t>(0x1002aac0)) {
        std::printf("the original's Build call is not where it was\n");
        std::exit(2);
    }
    DWORD old;
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    rel = static_cast<int32_t>(reinterpret_cast<uint8_t *>(&OrigBuildStub) - (call + 5));
    std::memcpy(call + 1, &rel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
}

// ---------------------------------------------------------------- the scene

constexpr int kLights = 6, kMats = 4;
uint8_t *g_scene;
uint8_t *g_sector;
uint8_t *g_blocker;                         // an object visual casting shadows
uint8_t *g_light[kLights];
uint8_t *g_mat[kMats];
uint8_t *g_obj[2];                          // [0] computed by the original, [1] by the port
uint8_t *g_mesh;

// The progress messages of each side (but state 1).
std::vector<uint32_t> g_progress[2];

uint32_t __stdcall Progress(uint32_t message, uint32_t param, uint32_t state, void *context)
{
    const int side = static_cast<int>(reinterpret_cast<uintptr_t>(context));
    if (state != 1) {
        g_progress[side].push_back(message);
        g_progress[side].push_back(param);
        g_progress[side].push_back(state);
    }
    return 0;
}

S_vector RandUnit()
{
    for (;;) {
        const S_vector v(RandF(-1, 1), RandF(-1, 1), RandF(-1, 1));
        const float sq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (sq > 0.01f && sq <= 1.0f) {
            const float k = 1.0f / std::sqrt(sq);
            return S_vector(v.x * k, v.y * k, v.z * k);
        }
    }
}

struct S_vtx {
    S_vector pos, normal;
    float u, v;
};

// A random rotation's rows.
void RandBasis(S_vector &x, S_vector &y, S_vector &z)
{
    z = RandUnit();
    S_vector t = std::fabs(z.y) < 0.9f ? S_vector(0, 1, 0) : S_vector(1, 0, 0);
    x = t.Cross(z);
    const float k = 1.0f / std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
    x = S_vector(x.x * k, x.y * k, x.z * k);
    y = z.Cross(x);
}

S_vector At(const S_vector &o, const S_vector &x, const S_vector &y, const S_vector &z, float a, float b, float c)
{
    return S_vector(o.x + x.x * a + y.x * b + z.x * c, o.y + x.y * a + y.y * b + z.y * c, o.z + x.z * a + y.z * b + z.z * c);
}

void AddTri(std::vector<I3D_triface> &f, int a, int b, int c)
{
    I3D_triface t;
    t.v[0] = static_cast<uint16_t>(a);
    t.v[1] = static_cast<uint16_t>(b);
    t.v[2] = static_cast<uint16_t>(c);
    f.push_back(t);
}

int AddVtx(std::vector<S_vtx> &v, const S_vector &p, const S_vector &n)
{
    v.push_back({p, n, RandF(0, 1), RandF(0, 1)});
    return static_cast<int>(v.size()) - 1;
}

// A level's geometry: a few pieces around the origin.
void MakeGeometry(std::vector<S_vtx> &v, std::vector<I3D_triface> &f)
{
    const int pieces = 1 + Rand(3);
    for (int p = 0; p < pieces; ++p) {
        S_vector x, y, z;
        RandBasis(x, y, z);
        const S_vector o(RandF(-3, 3), RandF(-3, 3), RandF(-3, 3));
        switch (Rand(4)) {
        case 0: {
            // A box: four vertices a side (corners shared by position).
            const float h[3] = {RandF(0.2f, 2), RandF(0.2f, 2), RandF(0.2f, 2)};
            const S_vector axes[3] = {x, y, z};
            for (int a = 0; a < 3; ++a) {
                for (int s = -1; s <= 1; s += 2) {
                    const S_vector &n = axes[a];
                    const S_vector &u = axes[(a + 1) % 3];
                    const S_vector &w = axes[(a + 2) % 3];
                    const float hu = h[(a + 1) % 3], hw = h[(a + 2) % 3], hn = h[a] * s;
                    const S_vector c(o.x + n.x * hn, o.y + n.y * hn, o.z + n.z * hn);
                    const S_vector nn(n.x * s, n.y * s, n.z * s);
                    int q[4];
                    for (int k = 0; k < 4; ++k) {
                        const float a1 = (k == 1 || k == 2) ? hu : -hu, a2 = (k >= 2) ? hw : -hw;
                        q[k] = AddVtx(v, At(c, u, w, n, a1, a2, 0), nn);
                    }
                    AddTri(f, q[0], q[1], q[2]);
                    AddTri(f, q[0], q[2], q[3]);
                }
            }
            break;
        }
        case 1: {
            // A strip bent along its middle (vertices shared by index).
            const int n = 2 + Rand(4);
            const float len = RandF(0.5f, 4), wid = RandF(0.3f, 2), bend = RandF(0, 1.2f);
            int prev[2] = {-1, -1};
            for (int i = 0; i <= n; ++i) {
                const float t = len * (static_cast<float>(i) / n - 0.5f);
                const float ang = t > 0 ? bend : 0.0f;
                const float a = t > 0 ? t * std::cos(ang) : t, c = t > 0 ? t * std::sin(ang) : 0.0f;
                const S_vector nn = At(S_vector(0, 0, 0), x, y, z, -std::sin(ang), 0, std::cos(ang));
                const int r0 = AddVtx(v, At(o, x, y, z, a, -wid, c), nn);
                const int r1 = AddVtx(v, At(o, x, y, z, a, wid, c), nn);
                if (prev[0] >= 0) {
                    AddTri(f, prev[0], prev[1], r1);
                    AddTri(f, prev[0], r1, r0);
                }
                prev[0] = r0;
                prev[1] = r1;
            }
            break;
        }
        case 2: {
            // A bumpy disc around a centre vertex.
            const int n = 4 + Rand(8);
            const float r = RandF(0.3f, 3), bump = RandF(0, 0.6f);
            const int c = AddVtx(v, o, z);
            int first = -1, prev = -1;
            for (int i = 0; i < n; ++i) {
                const float a = 6.2831853f * i / n;
                const int k = AddVtx(v, At(o, x, y, z, r * std::cos(a), r * std::sin(a), RandF(-bump, bump)), RandUnit());
                if (prev >= 0)
                    AddTri(f, c, prev, k);
                else
                    first = k;
                prev = k;
            }
            AddTri(f, c, prev, first);
            break;
        }
        default: {
            // Loose triangles.
            const int n = 1 + Rand(4);
            for (int i = 0; i < n; ++i) {
                const S_vector a(RandF(-3, 3), RandF(-3, 3), RandF(-3, 3));
                const S_vector b(a.x + RandF(-2, 2), a.y + RandF(-2, 2), a.z + RandF(-2, 2));
                const S_vector c(a.x + RandF(-2, 2), a.y + RandF(-2, 2), a.z + RandF(-2, 2));
                const S_vector nn = RandUnit();
                AddTri(f, AddVtx(v, a, nn), AddVtx(v, b, nn), AddVtx(v, c, nn));
            }
            break;
        }
        }
    }
}

void RandomizeMaterials()
{
    for (uint8_t *m : g_mat) {
        const S_vector4 d = {RandF(0, 1), RandF(0, 1), RandF(0, 1), 1}, a = {RandF(0, 1), RandF(0, 1), RandF(0, 1), 1},
                        e = {RandF(0, 0.3f), RandF(0, 0.3f), RandF(0, 0.3f), 1};
        CallV<I3D_RESULT>(m, 0x08, &d);
        CallV<I3D_RESULT>(m, 0x04, &a);
        CallV<I3D_RESULT>(m, 0x0C, &e);
        CallV<I3D_RESULT>(m, 0x18, Rand(3) ? 1.0f : RandF(0, 1));
        uint32_t &flags = Field<uint32_t>(m, 0x08);
        flags = Rand(4) ? (flags | MATF_COLORS) : (flags & ~MATF_COLORS);
    }
}

uint8_t *MakeMesh(int levels)
{
    uint8_t *mesh = CallV<uint8_t *>(g_origDriver, 0x6C);
    for (int i = 0; i < levels; ++i) {
        std::vector<S_vtx> v;
        std::vector<I3D_triface> f;
        MakeGeometry(v, f);
        uint8_t *level = CallV<uint8_t *>(g_origDriver, 0x68);
        CallV<I3D_RESULT>(level, 0x24, static_cast<const void *>(v.data()), static_cast<int>(v.size()));
        // The faces into 1 - 3 face groups (none empty).
        const int groups = std::min(1 + Rand(3), static_cast<int>(f.size()));
        std::vector<std::vector<I3D_triface>> parts(groups);
        for (size_t k = 0; k < f.size(); ++k)
            parts[k < static_cast<size_t>(groups) ? k : Rand(groups)].push_back(f[k]);
        for (int g = 0; g < groups; ++g) {
            uint8_t *fg = CallV<uint8_t *>(g_origDriver, 0x64);
            CallV<void>(fg, 0x00, g_mat[Rand(kMats)]);
            CallV<I3D_triface *>(fg, 0x04, static_cast<const I3D_triface *>(parts[g].data()),
                                 static_cast<int>(parts[g].size()));
            CallV<void>(level, 0x1C, fg);
        }
        CallV<I3D_RESULT>(mesh, 0x08, level, i);
        CallV<uint32_t>(level, 0x00);
    }
    return mesh;
}

void NewMesh(int levels)
{
    uint8_t *mesh = MakeMesh(levels);
    for (uint8_t *o : g_obj)
        CallV<I3D_RESULT>(o, 0x9C, mesh, 0u);
    CallV<uint32_t>(mesh, 0x00);
    g_mesh = mesh;
    if (g_blocker) {
        uint8_t *other = MakeMesh(1);
        CallV<I3D_RESULT>(g_blocker, 0x9C, other, 0u);
        CallV<uint32_t>(other, 0x00);
        const S_vector pos(RandF(-4, 4), RandF(-4, 4), RandF(-4, 4));
        const S_vector dir = RandUnit();
        CallV<void>(g_blocker, 0x14, &pos, &dir, 0.0f);
        CallV<void>(g_blocker, 0x30, Rand(4) != 0);
    }
}

void RandomizeLights()
{
    for (uint8_t *l : g_light) {
        const int t = Rand(8) ? 1 + Rand(4) : 5 + Rand(3);
        CallV<void>(l, 0x74, t);
        const S_vector4 c = {RandF(0, 1), RandF(0, 1), RandF(0, 1), 0}, a = {RandF(0, 0.5f), RandF(0, 0.5f), RandF(0, 0.5f), 0};
        CallV<void>(l, 0x7C, &c);
        CallV<void>(l, 0x84, &a);
        CallV<void>(l, 0x88, Rand(6) ? RandF(0.1f, 4) : 0.0f);
        CallV<void>(l, 0x90, RandF(0, 2));
        CallV<void>(l, 0x98, RandF(0.5f, 40));
        CallV<void>(l, 0x9C, Rand(10) ? RandF(0.2f, 1) : 0.0f, RandF(0, 0.5f), RandF(0, 0.1f));
        const float outer = RandF(0.2f, 3.1f);
        CallV<void>(l, 0xA4, RandF(0, outer), outer);
        CallV<void>(l, 0x94, static_cast<uint32_t>((Rand(5) ? LIGHTMODE_LIGHTMAP : 0) | (g_rng() & 0x7d) | (Rand(2) ? 0x20 : 0)));
        const S_vector pos(RandF(-8, 8), RandF(-8, 8), RandF(-8, 8));
        const S_vector dir = RandUnit();
        CallV<void>(l, 0x14, &pos, &dir, RandF(-1, 1));
        CallV<void>(l, 0x30, Rand(8) != 0);
    }
}

void PlaceObjects()
{
    const S_vector pos(RandF(-2, 2), RandF(-2, 2), RandF(-2, 2));
    const S_vector dir = RandUnit();
    const float roll = RandF(-3, 3);
    const bool world = Rand(4) == 0;
    for (uint8_t *o : g_obj) {
        CallV<void>(o, 0x14, &pos, &dir, roll);
        uint32_t &vf = Field<uint32_t>(o, 0x1D0);
        vf = world ? (vf | VISF_WORLD_SPACE) : (vf & ~VISF_WORLD_SPACE);
    }
}

// The bounds of an object and its sector made again when next asked for (the flag set above
// bypasses the engine's bookkeeping; the collision culls by them).
void InvalidateBounds(uint8_t *o)
{
    Field<uint32_t>(o, 0x1D0) &= ~(VISF_BOUND_VALID | VISF_WBOUND_VALID | VISF_INVMATRIX_VALID);
    Field<uint32_t>(o, 0xE0) &= ~FRMFLAGS_BOUND_VALID;
    Field<uint32_t>(g_sector, 0xE0) &= ~FRMFLAGS_BOUND_VALID;
}

// ---------------------------------------------------------------- comparing

void CheckBits(const std::string &what, const void *a, const void *b, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, static_cast<const uint8_t *>(a) + i, std::min<size_t>(4, n - i));
        std::memcpy(&y, static_cast<const uint8_t *>(b) + i, std::min<size_t>(4, n - i));
        Check(what + " +" + std::to_string(i), static_cast<long>(x), static_cast<long>(y));
    }
}

long g_seenParts, g_seenUniform, g_seenColors, g_seenVertices, g_seenProbes, g_seenBlocked;

// ---------------------------------------------------------------- tracing (diagnosis)

// Every sample's light inputs and result, per side: with --trace the original's texture-path call
// of its light function and the port's LightmapLightAt both log here (the port's then computes
// with the original's function), and a mismatch prints the first sample whose inputs differ.
struct S_trace {
    float in[8];                            // pos, normal, (material, level as bits)
    float out[3];
    bool blocked;
};
std::vector<S_trace> g_trace[2];
bool g_tracing;
using OrigLightFn = void(__fastcall *)(float *, const float *, const float *, void *, void *, uint32_t, bool *, void *,
                                       int);

void TraceOne(int side, float *color, const float *pos, const float *normal, const void *mat, int level, bool *blocked)
{
    S_trace t = {};
    std::memcpy(t.in, pos, 12);
    std::memcpy(t.in + 3, normal, 12);
    std::memcpy(&t.in[6], &mat, 4);
    std::memcpy(&t.in[7], &level, 4);
    std::memcpy(t.out, color, 12);
    t.blocked = blocked && *blocked;
    g_trace[side].push_back(t);
}

void __fastcall OrigLightTrace(float *color, const float *pos, const float *normal, void *mat, void *scene,
                               uint32_t flags, bool *blocked, void *self, int level)
{
    OrigFn<OrigLightFn>(0x10064b90)(color, pos, normal, mat, scene, flags, blocked, self, level);
    TraceOne(0, color, pos, normal, mat, level, blocked);
}

uint8_t g_entryBytes[5];
void SetEntryJump(bool on);

// (trace) the scene's TestRay slot, logging each query of both runs
using TestRayFn = void *(__stdcall *)(void *, const float *, const float *, uint32_t, float *, float *, void *, void *,
                                      void *, void *, int);
TestRayFn g_origTestRay;
bool g_logRays;
// Every query of each side's Construct run (self and the other twin normalised).
struct S_rayRecord {
    float from[4], dir[3];
    uint32_t flags;
    int frame, lod, hit;
    float dist, user[4], normal[3];
};
std::vector<S_rayRecord> g_rays[2];
int RayFrameId(void *f, int side)
{
    if (!f)
        return 0;
    if (f == g_obj[side])
        return 1;                           // the object being computed
    if (f == g_obj[1 - side])
        return 2;                           // its twin (off)
    if (f == g_blocker)
        return 3;
    return 4;
}
void *__stdcall TestRayLog(void *scene, const float *from, const float *dir, uint32_t flags, float *dist, float *normal,
                           void *hitCb, void *user, void *filter, void *frame, int lod)
{
    void *hit = g_origTestRay(scene, from, dir, flags, dist, normal, hitCb, user, filter, frame, lod);
    if (g_side >= 0) {
        S_rayRecord r = {};
        std::memcpy(r.from, from, 16);
        std::memcpy(r.dir, dir, 12);
        r.flags = flags;
        r.frame = RayFrameId(frame, g_side);
        r.lod = lod;
        r.hit = RayFrameId(hit, g_side);
        r.dist = hit && dist ? *dist : -1.0f;
        if (user)
            std::memcpy(r.user, user, 16);
        if (hit && normal)
            std::memcpy(r.normal, normal, 12);
        g_rays[g_side].push_back(r);
    }
    if (g_logRays)
        std::printf("    TestRay from %g %g %g dir %g %g %g flags %x cb %p user %p filter %p frame %p lod %d -> %p\n",
                    from[0], from[1], from[2], dir[0], dir[1], dir[2], flags, hitCb, user, filter, frame, lod, hit);
    return hit;
}
void HookTestRay()
{
    void **slot = *reinterpret_cast<void ***>(g_scene) + 0x100 / 4;
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    g_origTestRay = reinterpret_cast<TestRayFn>(*slot);
    *slot = reinterpret_cast<void *>(&TestRayLog);
    VirtualProtect(slot, 4, old, &old);
}

// Both light functions on the port's sample: the first difference is printed and ends the run.
void PortLightTrace(S_vector &color, const S_vector &pos, const S_vector &normal, const I3D_material *mat,
                    I3D_scene *scene, uint32_t flags, bool *blocked, I3D_frame *self, int level)
{
    const S_vector4 p4 = {pos.x, pos.y, pos.z, 0}, n4 = {normal.x, normal.y, normal.z, 0};
    S_vector4 c4 = {};
    bool bo = false, bp = false;
    // Both from the same collision state; the states they leave compared too (the original's
    // TestRay reads part of what the previous query left).
    uint8_t *ray = &Orig<uint8_t>(0x100ad900);
    std::vector<uint8_t> start(ray, ray + 0x300);
    OrigFn<OrigLightFn>(0x10064b90)(&c4.x, &p4.x, &n4.x, const_cast<I3D_material *>(mat), scene, flags,
                                    blocked ? &bo : nullptr, self, level);
    std::vector<uint8_t> afterOrig(ray, ray + 0x300);
    std::memcpy(ray, start.data(), start.size());
    SetEntryJump(false);
    S_vector cp;
    LightmapLightAt(cp, pos, normal, mat, scene, flags, blocked ? &bp : nullptr, self, level);
    SetEntryJump(true);
    for (size_t o = 0; o < 0x300; o += 4) {
        // (pointers into each caller's frame differ)
        const uint32_t a = 0x100ad900 + static_cast<uint32_t>(o);
        if (a == 0x100ada70 || a == 0x100adaf0 || a == 0x100adafc)
            continue;
        if (std::memcmp(&afterOrig[o], ray + o, 4)) {
            uint32_t x, y;
            std::memcpy(&x, &afterOrig[o], 4);
            std::memcpy(&y, ray + o, 4);
            std::printf("collision state differs at %08x: original's call left %08x, port's %08x\n", a, x, y);
        }
    }
    if (std::memcmp(&c4, &cp, 12) || bo != bp) {
        uint32_t b[6];
        std::memcpy(b, &pos, 12);
        std::memcpy(b + 3, &normal, 12);
        std::printf("light function difference (level %d, flags %x, material %p, self %p):\n"
                    "  pos %08x %08x %08x normal %08x %08x %08x\n"
                    "  original %.9g %.9g %.9g blocked %d\n  port     %.9g %.9g %.9g blocked %d\n",
                    level, flags, static_cast<const void *>(mat), static_cast<void *>(self), b[0], b[1], b[2], b[3],
                    b[4], b[5], c4.x, c4.y, c4.z, bo, cp.x, cp.y, cp.z, bp);
        // Both again with their rays logged (from the same collision state).
        std::memcpy(ray, start.data(), start.size());
        g_logRays = true;
        std::printf("  original's rays:\n");
        OrigFn<OrigLightFn>(0x10064b90)(&c4.x, &p4.x, &n4.x, const_cast<I3D_material *>(mat), scene, flags,
                                        blocked ? &bo : nullptr, self, level);
        std::memcpy(ray, start.data(), start.size());
        std::printf("  port's rays:\n");
        SetEntryJump(false);
        LightmapLightAt(cp, pos, normal, mat, scene, flags, blocked ? &bp : nullptr, self, level);
        SetEntryJump(true);
        g_logRays = false;
        // One light at a time (the sector's list cut to it).
        auto *sector = reinterpret_cast<I3D_sector *>(g_sector);
        I3D_light **all = sector->lights.Data();
        const int count = sector->lights.Size();
        for (int k = 0; k < count; ++k) {
            I3D_light *only = all[k];
            Field<I3D_light **>(sector, 0x1A4) = &only;
            Field<int>(sector, 0x1AC) = 1;
            S_vector4 o1 = {};
            bool b1 = false, b2 = false;
            OrigFn<OrigLightFn>(0x10064b90)(&o1.x, &p4.x, &n4.x, const_cast<I3D_material *>(mat), scene, flags,
                                            blocked ? &b1 : nullptr, self, level);
            SetEntryJump(false);
            S_vector p1;
            LightmapLightAt(p1, pos, normal, mat, scene, flags, blocked ? &b2 : nullptr, self, level);
            SetEntryJump(true);
            std::printf("  light %d type %d mode %x power %g on %d: original %.9g %.9g %.9g %d, port %.9g %.9g %.9g %d\n",
                        k, only->lightType, only->mode, only->power, (only->frameFlags & 1) != 0, o1.x, o1.y, o1.z,
                        b1, p1.x, p1.y, p1.z, b2);
            Field<I3D_light **>(sector, 0x1A4) = all;
            Field<int>(sector, 0x1AC) = count;
        }
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 5);
    }
    color = cp;
    if (blocked) {
        *blocked = bp;
        TraceOne(1, &c4.x, &p4.x, &n4.x, mat, level, blocked);
    }
}

void PatchCall(uint8_t *call, const void *target)
{
    DWORD old;
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    const int32_t rel = static_cast<int32_t>(static_cast<const uint8_t *>(target) - (call + 5));
    std::memcpy(call + 1, &rel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
}

void StartTracing()
{
    uint8_t *call = &Orig<uint8_t>(0x10026b7b);
    if (call[0] != 0xe8)
        std::exit(2);
    PatchCall(call, reinterpret_cast<const void *>(&OrigLightTrace));
    // The port's function starts with a jump to the logger.
    std::memcpy(g_entryBytes, reinterpret_cast<const void *>(&LightmapLightAt), 5);
    SetEntryJump(true);
    g_tracing = true;
    HookTestRay();
}

void SetEntryJump(bool on)
{
    auto *entry = reinterpret_cast<uint8_t *>(&LightmapLightAt);
    DWORD old;
    VirtualProtect(entry, 5, PAGE_EXECUTE_READWRITE, &old);
    if (on) {
        entry[0] = 0xe9;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t *>(&PortLightTrace) - (entry + 5));
        std::memcpy(entry + 1, &rel, 4);
    } else {
        std::memcpy(entry, g_entryBytes, 5);
    }
    VirtualProtect(entry, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, 5);
}

void ReportTrace()
{
    const size_t n = std::min(g_trace[0].size(), g_trace[1].size());
    std::printf("trace: original %zu samples, port %zu\n", g_trace[0].size(), g_trace[1].size());
    for (size_t k = 0; k < n; ++k) {
        const S_trace &a = g_trace[0][k], &b = g_trace[1][k];
        if (std::memcmp(a.in, b.in, sizeof a.in) || std::memcmp(a.out, b.out, sizeof a.out) || a.blocked != b.blocked) {
            std::printf("first difference at sample %zu (level %d):\n", k, *reinterpret_cast<const int *>(&a.in[7]));
            for (int i = 0; i < 6; ++i) {
                uint32_t x, y;
                std::memcpy(&x, &a.in[i], 4);
                std::memcpy(&y, &b.in[i], 4);
                std::printf("  in[%d] %.9g (%08x) vs %.9g (%08x)\n", i, a.in[i], x, b.in[i], y);
            }
            for (int i = 0; i < 3; ++i)
                std::printf("  out[%d] %.9g vs %.9g\n", i, a.out[i], b.out[i]);
            std::printf("  blocked %d vs %d\n", a.blocked, b.blocked);
            return;
        }
    }
    std::printf("no difference in the common samples\n");
}

// The light at random points, by the original's function (0x10064b90, __fastcall) and the port's.
void ProbeLights(int levels)
{
    using OrigLightAt = void(__fastcall *)(float *, const float *, const float *, void *, void *, uint32_t, bool *,
                                           void *, int);
    Orig<void *>(0x100d13d4) = g_sector;        // the original's light sector
    SetLightSector(reinterpret_cast<I3D_sector *>(g_sector));
    char text[200];
    for (int k = 0; k < 40; ++k) {
        const int self = Rand(2);
        CallV<void>(g_obj[self], 0x30, true);
        CallV<void>(g_obj[1 - self], 0x30, false);
        CallV<void>(g_sector, 0x24);
        const S_vector4 pos = {RandF(-5, 5), RandF(-5, 5), RandF(-5, 5), 0};
        const S_vector n = RandUnit();
        const S_vector4 nrm = {n.x, n.y, n.z, 0};
        void *mat = Rand(5) ? g_mat[Rand(kMats)] : nullptr;
        const bool shadows = g_scene && Rand(3) != 0;
        const uint32_t flags = shadows ? LMCONS_SHADOWS : 0u;
        void *scene = shadows ? g_scene : nullptr;
        const int level = Rand(levels);
        const bool askBlocked = Rand(4) != 0;
        std::snprintf(text, sizeof text, "probe %d: self %d, level %d, %s, at %g %g %g", k, self, level,
                      shadows ? "shadows" : "no shadows", pos.x, pos.y, pos.z);
        g_lastOp = text;
        S_vector4 co = {}, cp = {};
        bool bo = false, bp = false;
        OrigFn<OrigLightAt>(0x10064b90)(&co.x, &pos.x, &nrm.x, mat, scene, flags, askBlocked ? &bo : nullptr,
                                        g_obj[self], level);
        S_vector colour;
        LightmapLightAt(colour, reinterpret_cast<const S_vector &>(pos), n, static_cast<const I3D_material *>(mat),
                        static_cast<I3D_scene *>(scene), flags, askBlocked ? &bp : nullptr,
                        reinterpret_cast<I3D_frame *>(g_obj[self]), level);
        Check("probe blocked", bo, bp);
        CheckBits("probe colour", &co, &colour, 12);
        ++g_seenProbes;
        g_seenBlocked += bo;
    }
}

void CompareLevel(int i, const uint8_t *o, const S_lightmapLevel *p, bool textures, I3D_mesh_level *ml)
{
    const std::string at = "level " + std::to_string(i);
    if (!o || !p) {
        Check(at + " exists", o != nullptr, p != nullptr);
        return;
    }
    const auto &ol = *reinterpret_cast<const S_lightmapLevel *>(o);
    Check(at + " numVertices", ol.numVertices, p->numVertices);
    if (!textures) {
        CheckBits(at + " colors", ol.colors, p->colors, sizeof(uint32_t) * static_cast<size_t>(p->numVertices));
        g_seenColors += p->numVertices;
        return;
    }
    CheckBits(at + " uvs", ol.uvs, p->uvs, sizeof(S_lightmapUV) * static_cast<size_t>(p->numVertices));
    CheckBits(at + " vertexMap", ol.vertexMap, p->vertexMap, sizeof(S_lightmapVertex) * static_cast<size_t>(p->numVertices));
    g_seenVertices += p->numVertices;
    Check(at + " numParts", ol.numParts, p->numParts);
    for (int k = 0; k < p->numParts; ++k) {
        const S_lightmapPart &a = ol.parts[k], &b = p->parts[k];
        const std::string pa = at + " part " + std::to_string(k);
        Check(pa + " sourceWidth", a.sourceWidth, b.sourceWidth);
        Check(pa + " sourceHeight", a.sourceHeight, b.sourceHeight);
        Check(pa + " width", a.width, b.width);
        Check(pa + " height", a.height, b.height);
        Check(pa + " area", a.area, b.area);
        CheckBits(pa + " u v", &a.u, &b.u, 8);
        if (b.IsUniform()) {
            Check(pa + " color", static_cast<long>(a.color), static_cast<long>(b.color));
            ++g_seenUniform;
        } else {
            Check(pa + " uniform", a.IsUniform(), false);
            CheckBits(pa + " pixels", a.pixels, b.pixels, static_cast<size_t>(b.sourceWidth) * b.sourceHeight * 3);
        }
        ++g_seenParts;
    }
    Check(at + " numFaceGroups", ol.numFaceGroups, p->numFaceGroups);
    I3D_face_group *fg = ml->faceGroups;
    for (int g = 0; g < p->numFaceGroups && fg; ++g, fg = fg->next)
        CheckBits(at + " faceParts " + std::to_string(g), ol.faceParts[g], p->faceParts[g],
                  sizeof(uint16_t) * static_cast<size_t>(fg->numFaces));
}

void RunRound()
{
    char text[200];
    const int levels = 1 + Rand(3);
    RandomizeMaterials();
    NewMesh(levels);
    RandomizeLights();
    PlaceObjects();
    CallV<void>(g_sector, 0x24);                // the matrices and the lights' Direct3D state
    for (uint8_t *l : g_light)
        reinterpret_cast<I3D_frame *>(l)->UpdateWMatrix();
    for (uint8_t *o : g_obj)
        reinterpret_cast<I3D_frame *>(o)->UpdateWMatrix();

    const int set = Rand(6);
    const bool textures = Rand(5) != 0;
    const float smooth = Rand(4) ? RandF(5, 89) : RandF(0, 180);
    for (uint8_t *o : g_obj) {
        auto *lo = reinterpret_cast<I3D_lit_object *>(o);
        lo->currentSet = set;
        lo->sets[set].flags = textures ? LMSET_TEXTURES : 0u;
        lo->sets[set].smoothAngle = smooth;
    }
    const uint32_t samples = Rand(12) ? 1 + Rand(3) : 0;
    Orig<uint32_t>(0x100be058 + 4 * DRVSTATE_LM_SAMPLES) = samples;
    g_driverStates[DRVSTATE_LM_SAMPLES] = samples;
    const float resolution = Rand(10) ? RandF(0.5f, 4) : RandF(0.05f, 0.5f);
    uint32_t flags = 0;
    if (Rand(3) == 0)
        flags |= LMCONS_BLUR;
    if (Rand(10) == 0)
        flags |= LMCONS_TEST_PATTERN;
    if (levels == 1 && Rand(6) == 0)
        flags |= LMCONS_ONE_PATCH;
    if (Rand(4) == 0)
        flags |= g_rng() & 0x14b;               // other bits (shadows without a scene, ...)
    if (levels > 1)
        flags &= ~LMCONS_ONE_PATCH;
    const S_vector4 plane = {RandUnit().x, RandUnit().y, RandUnit().z, 0};
    // (The original stops making the one patch only when the callback's count of faces left
    // runs out: without a callback it makes it for ever.)
    const bool callback = (flags & LMCONS_ONE_PATCH) || Rand(2) != 0;
    const bool shadows = g_scene && Rand(2) != 0;
    if (shadows)
        flags |= LMCONS_SHADOWS;
    void *scene = shadows ? g_scene : nullptr;
    std::snprintf(text, sizeof text, "round %ld: %d levels, set %d, %s, res %g, smooth %g, samples %u, flags %x, %s%s",
                  g_op, levels, set, textures ? "patches" : "colours", resolution, smooth, samples, flags,
                  callback ? "callback" : "no callback", shadows ? ", shadows" : "");
    g_lastOp = text;

    ProbeLights(levels);
    g_lastOp = text;
    g_trace[0].clear();
    g_trace[1].clear();
    g_rays[0].clear();
    g_rays[1].clear();
    g_progress[0].clear();
    g_progress[1].clear();
    g_side = 0;
    // Only the object being computed is on (the rays see its faces, not the other's).
    CallV<void>(g_obj[0], 0x30, true);
    CallV<void>(g_obj[1], 0x30, false);
    CallV<void>(g_sector, 0x24);
    InvalidateBounds(g_obj[0]);
    // The collision queries' scratch state (the original's TestRay reads some of what the previous
    // query left) and the lights, as the original's run starts: the port's run starts alike.
    std::vector<uint8_t> rayState(&Orig<uint8_t>(0x100ad900), &Orig<uint8_t>(0x100adc00));
    std::vector<uint8_t> lightState[kLights];
    for (int k = 0; k < kLights; ++k)
        lightState[k].assign(g_light[k], g_light[k] + sizeof(I3D_light));
    const I3D_RESULT ro = CallV<I3D_RESULT>(g_obj[0], 0xB4, scene, &plane, resolution, flags,
                                            callback ? reinterpret_cast<void *>(&Progress) : nullptr,
                                            reinterpret_cast<void *>(0));
    CallV<void>(g_obj[0], 0x30, false);
    CallV<void>(g_obj[1], 0x30, true);
    CallV<void>(g_sector, 0x24);
    InvalidateBounds(g_obj[1]);
    std::memcpy(&Orig<uint8_t>(0x100ad900), rayState.data(), rayState.size());
    for (int k = 0; k < kLights; ++k) {
        for (size_t o = 0; o < sizeof(I3D_light); o += 4) {
            if (std::memcmp(&lightState[k][o], g_light[k] + o, 4)) {
                uint32_t x, y;
                std::memcpy(&x, &lightState[k][o], 4);
                std::memcpy(&y, g_light[k] + o, 4);
                Fail("light " + std::to_string(k) + " +" + std::to_string(o) + " changed during the original's run",
                     static_cast<long>(x), static_cast<long>(y));
            }
        }
    }
    g_side = 1;
    auto *port = reinterpret_cast<I3D_lit_object *>(g_obj[1]);
    const I3D_RESULT rp =
        g_bothOriginal
            ? CallV<I3D_RESULT>(g_obj[1], 0xB4, scene, &plane, resolution, flags,
                                callback ? reinterpret_cast<void *>(&Progress) : nullptr, reinterpret_cast<void *>(1))
            : port->I3D_lit_object::Construct(static_cast<I3D_scene *>(scene),
                                              reinterpret_cast<const S_vector *>(&plane), resolution, flags,
                                              callback ? &Progress : nullptr, reinterpret_cast<void *>(1));
    g_side = -1;
    if (g_tracing) {
        // (Compared here so that a mismatch below has the trace printed first.)
        bool same = g_trace[0].size() == g_trace[1].size();
        for (size_t k = 0; same && k < g_trace[0].size(); ++k)
            same = !std::memcmp(&g_trace[0][k], &g_trace[1][k], offsetof(S_trace, blocked)) &&
                   g_trace[0][k].blocked == g_trace[1][k].blocked;
        if (!same)
            ReportTrace();
        // The queries of the two runs.
        const size_t nr = std::min(g_rays[0].size(), g_rays[1].size());
        std::printf("queries: original %zu, port %zu\n", g_rays[0].size(), g_rays[1].size());
        for (size_t k = 0; k < nr; ++k) {
            const S_rayRecord &a = g_rays[0][k], &b = g_rays[1][k];
            if (std::memcmp(a.from, b.from, 12) || std::memcmp(a.dir, b.dir, 12) || a.flags != b.flags ||
                a.frame != b.frame || a.lod != b.lod || a.hit != b.hit || std::memcmp(&a.dist, &b.dist, 4) ||
                std::memcmp(a.user, b.user, 12) || std::memcmp(a.normal, b.normal, 12)) {
                std::printf("first different query %zu:\n", k);
                for (const S_rayRecord *r : {&a, &b}) {
                    uint32_t w;
                    std::memcpy(&w, &r->from[3], 4);
                    std::printf("  from %.9g %.9g %.9g (w %08x) dir %.9g %.9g %.9g flags %x frame %d lod %d -> hit %d "
                                "dist %.9g user %.9g %.9g %.9g normal %.9g %.9g %.9g\n",
                                r->from[0], r->from[1], r->from[2], w, r->dir[0], r->dir[1], r->dir[2], r->flags,
                                r->frame, r->lod, r->hit, r->dist, r->user[0], r->user[1], r->user[2], r->normal[0],
                                r->normal[1], r->normal[2]);
                }
                break;
            }
        }
    }
    Check("result", ro, rp);
    auto *orig = reinterpret_cast<I3D_lit_object *>(g_obj[0]);
    Check("loaded", orig->sets[set].loaded, port->sets[set].loaded);
    CheckBits("resolution", &orig->sets[set].resolution, &port->sets[set].resolution, 4);
    Check("light stamp", orig->lights.stamp, port->lights.stamp);
    Check("progress messages", static_cast<long>(g_progress[0].size()), static_cast<long>(g_progress[1].size()));
    for (size_t k = 0; k < g_progress[0].size(); ++k)
        Check("progress " + std::to_string(k / 3), static_cast<long>(g_progress[0][k]), static_cast<long>(g_progress[1][k]));
    auto *mesh = reinterpret_cast<I3D_mesh *>(g_mesh);
    for (uint32_t i = 0; i < mesh->numLevels; ++i)
        CompareLevel(static_cast<int>(i), reinterpret_cast<const uint8_t *>(orig->sets[set].levels[i]),
                     port->sets[set].levels[i], textures, mesh->levels[i]);

    // The port's levels are the test's: freed here (the original would free them on its heap).
    if (g_bothOriginal)
        return;                             // (the original's next Construct frees them)
    for (S_lightmapLevel *&l : port->sets[set].levels) {
        delete l;
        l = nullptr;
    }
    port->sets[set].loaded = false;
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long rounds = argc > 2 ? std::atol(argv[2]) : 300;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    if (!SetUpBuffers(origPath))
        return 2;
    g_logDisabled = true;
    _controlfp(_PC_24, _MCW_PC);
    g_rng.seed(seed);
    g_origDriver = Orig<uint8_t *>(0x100d16f8);
    std::memcpy(&ig::g_defaultMaterial, &Orig<D3DMATERIAL8>(0x100d1ba0), sizeof(D3DMATERIAL8));
    PatchOrigBuild();

    // A scene of the original's for the shadow rays (its primary sector holds everything), else a
    // sector of its own.
    const bool useScene = argc > 4 ? std::atoi(argv[4]) != 0 : true;
    if (useScene) {
        g_scene = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_SCENE));
        g_sector = g_scene ? Field<uint8_t *>(g_scene, 0x1F8) : nullptr;
        if (!g_sector)
            g_scene = nullptr;
    }
    if (!g_sector)
        g_sector = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_SECTOR));
    for (uint8_t *&l : g_light) {
        l = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_LIGHT));
        CallV<I3D_RESULT>(l, 0x38, g_sector, 0u);
        CallV<void>(g_sector, 0x74, l);
    }
    for (uint8_t *&m : g_mat)
        m = CallV<uint8_t *>(g_origDriver, 0x58);
    for (uint8_t *&o : g_obj) {
        o = CallV<uint8_t *>(g_origDriver, 0x50, FourCC("LMAP"));
        CallV<I3D_RESULT>(o, 0x38, g_sector, 0u);
    }
    if (g_scene) {
        g_blocker = CallV<uint8_t *>(g_origDriver, 0x50, FourCC("OBJ_"));
        CallV<I3D_RESULT>(g_blocker, 0x38, g_sector, 0u);
    }
    if (!g_obj[0] || !g_obj[1] || !g_sector) {
        std::printf("the original's driver made no lit objects\n");
        return 2;
    }
    const long watchdog = argc > 5 ? std::atol(argv[5]) : 120;
    g_bothOriginal = argc > 6 && std::atoi(argv[6]) != 0;
    const int trace = argc > 7 ? std::atoi(argv[7]) : 0;
    if (trace == 1) {
        StartTracing();
    } else if (trace == 2) {
        // Only the queries of both runs logged and compared.
        g_tracing = true;
        HookTestRay();
    }
    CreateThread(nullptr, 0, &Watchdog, reinterpret_cast<void *>(static_cast<uintptr_t>(watchdog)), 0, nullptr);
    const DWORD t0 = GetTickCount();
    for (g_op = 0; g_op < rounds; ++g_op) {
        g_roundStarted = static_cast<long>(GetTickCount());
        RunRound();
        if (g_op % 25 == 24) {
            std::printf("  %ld rounds, %lu s\n", g_op + 1, (GetTickCount() - t0) / 1000);
            std::fflush(stdout);
        }
    }
    g_roundStarted = 0;
    std::printf("coverage: patches %ld (uniform %ld), lightmap vertices %ld, vertex colours %ld, light probes %ld "
                "(blocked %ld), %s\n", g_seenParts, g_seenUniform, g_seenVertices, g_seenColors, g_seenProbes,
                g_seenBlocked, g_scene ? "with a scene" : "no scene");
    std::printf("compare_lightmap: %ld rounds, seed %u: identical\n", rounds, seed);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
