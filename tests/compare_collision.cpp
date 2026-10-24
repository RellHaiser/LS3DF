// Differential test of the scene's collision queries (src/i3d/collision_ray.cpp,
// collision_sphere.cpp): PickFrame, TestRay, IntersectFrame, MoveSphere, MoveSphereFrame,
// MoveSphereVisual and SweepSpheres (I3D_scene slots 0xFC - 0x114) of the original LS3DF.dll and of
// the port run on the same scene, made by the original's driver: its primary, omni and near
// sectors with sectors of their own, dummies, object visuals, lit objects, morphs and billboards
// (every mode, with a render camera) in a random hierarchy, each with random meshes (1 - 3 levels,
// face groups with materials of random opacity, two-sided or not, additive, with fake textures of
// BGRA texels or a 1-bit mask, alpha / colour-keyed). Between the queries frames are moved,
// rotated, scaled, linked elsewhere, switched off, put into world space, given new meshes, their
// billboard modes and materials changed; the bounds are usually made again before a query (moved
// frames leave their ancestors' bounds as they were).
//
// The queries: random starts, directions toward the visuals or anywhere (short, long, unlimited),
// radii, every flag, frame arguments (skipped, excluded, tested at their level), LOD levels; with
// the test's hit callback and face filter (each logged, their answers from one random sequence for
// both sides; the filter tints the colour) or the built-in filters (the original's alpha / transmit
// filter against the port's). After each query: the result, every output (but the w components
// the original leaves unset), the colour, both call logs, and the queries' shared state (the
// original's 0x100ad9b0 - 0x100adb08 against the port's col::q) field by field.
//
// The port's code runs on the original's objects: their virtual methods (bounds, meshes, face
// groups, joints) are the original's; the world matrix and world bound updates and the
// billboard's facing matrix are forwarded to the original's functions. Each side computes the
// visuals' cached inverse matrices itself (they are invalidated before each side's query).
//
// The port's fixes the test steps around: the original's ray and sphere queries with
// I3DCOL_SKIP_FRAME keep the previous query's excluded frame, and its sphere queries the previous
// query's LOD frame (both cleared on the original's side first); a billboard in world space has no
// facing matrix in the original (not made). Values the original leaves unset are not compared: a
// filter's texture coordinates without a texture, the normal of a box the ray misses (a hit
// callback's, and q.normal unless the ray query hit and wanted a normal). The transmit filter
// always gets a colour (the original's reads it).
//
// Not covered: skinned meshes (joints), mirrors (their geometry comes from model files).
//
// Usage: compare_collision.exe [original LS3DF.dll] [operations] [seed] [diagnosis: 1 each visual's
//        ray, 2 the targeted rays' hit rate]
//
// test-sources: src/i3d/collision_ray.cpp src/i3d/collision_sphere.cpp src/math/math.cpp src/core/debug.cpp src/dx/dxerr8.cpp
#include <float.h>

#include <algorithm>
#include <cmath>

#include "../src/core/debug.h"
#include "../src/i3d/billboard.h"
#include "../src/i3d/collision.h"
#include "../src/i3d/collision_state.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/material.h"
#include "../src/i3d/mesh.h"
#include "../src/i3d/scene.h"
#include "../src/i3d/sector.h"
#include "../src/igraph/igraph_state.h"
#include "../src/igraph/itexture.h"
#include "buffer_check.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d::i3d {
uint32_t g_driverStates[DRVSTATE_COUNT];
}
namespace ls3d::ig {
IDirect3DDevice8 *g_pDevice;
}

// The original's functions for what the port's collision code calls outside it.
void I3D_frame::UpdateWMatrixProc() { OrigFn<void(__fastcall *)(I3D_frame *, void *)>(0x1001e390)(this, nullptr); }
void __stdcall I3D_visual::UpdateWBoundProc() { OrigFn<void(__stdcall *)(I3D_visual *)>(0x100571b0)(this); }
void I3D_billboard::ComputeMatrix(const S_vector &camera, S_matrix &out)
{
    OrigFn<void(__fastcall *)(I3D_billboard *, void *, const S_vector *, S_matrix *)>(0x10006230)(this, nullptr, &camera,
                                                                                                   &out);
}
// (Only textured materials' texture coordinates: the vertices are locked already then.)
uint8_t *C_VertexBuffer::Lock(uint32_t flags)
{
    if (!lockedData)
        vb->Lock(0, 0, &lockedData, flags);
    return lockedData;
}

using namespace ls3d::i3d;
using ls3d::i3d::col::q;

namespace {

uint8_t *g_origDriver;

template <class R, class... A> R CallV(void *object, int slot, A... args)
{
    using F = R(__stdcall *)(void *, A...);
    return (*reinterpret_cast<F **>(object))[slot / 4](object, args...);
}
template <class T> T &Field(void *object, size_t offset) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset); }

float RandF(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g_rng() % 1000001) / 1000000.0f; }

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

// ---------------------------------------------------------------- geometry

struct S_vtx {
    S_vector pos, normal;
    float u, v;
};

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

// A level's geometry: boxes, strips, fans and loose triangles around the origin (some of them
// facing away from each other, some degenerate).
void MakeGeometry(std::vector<S_vtx> &v, std::vector<I3D_triface> &f)
{
    const int pieces = 1 + Rand(3);
    for (int p = 0; p < pieces; ++p) {
        S_vector x, y, z;
        RandBasis(x, y, z);
        const S_vector o(RandF(-2, 2), RandF(-2, 2), RandF(-2, 2));
        switch (Rand(4)) {
        case 0: {
            const float h[3] = {RandF(0.2f, 1.5f), RandF(0.2f, 1.5f), RandF(0.2f, 1.5f)};
            const S_vector axes[3] = {x, y, z};
            for (int a = 0; a < 3; ++a) {
                for (int s = -1; s <= 1; s += 2) {
                    const S_vector &n = axes[a];
                    const S_vector &u = axes[(a + 1) % 3];
                    const S_vector &w = axes[(a + 2) % 3];
                    const float hu = h[(a + 1) % 3], hw = h[(a + 2) % 3], hn = h[a] * s;
                    const S_vector c(o.x + n.x * hn, o.y + n.y * hn, o.z + n.z * hn);
                    const S_vector nn(n.x * s, n.y * s, n.z * s);
                    int qd[4];
                    for (int k = 0; k < 4; ++k) {
                        const float a1 = (k == 1 || k == 2) ? hu : -hu, a2 = (k >= 2) ? hw : -hw;
                        qd[k] = AddVtx(v, At(c, u, w, n, a1, a2, 0), nn);
                    }
                    if (s > 0) {
                        AddTri(f, qd[0], qd[1], qd[2]);
                        AddTri(f, qd[0], qd[2], qd[3]);
                    } else {
                        AddTri(f, qd[0], qd[2], qd[1]);
                        AddTri(f, qd[0], qd[3], qd[2]);
                    }
                }
            }
            break;
        }
        case 1: {
            const int n = 2 + Rand(4);
            const float len = RandF(0.5f, 3), wid = RandF(0.3f, 1.5f);
            int prev[2] = {-1, -1};
            for (int i = 0; i <= n; ++i) {
                const float t = len * (static_cast<float>(i) / n - 0.5f);
                const int r0 = AddVtx(v, At(o, x, y, z, t, -wid, RandF(-0.3f, 0.3f)), z);
                const int r1 = AddVtx(v, At(o, x, y, z, t, wid, RandF(-0.3f, 0.3f)), z);
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
            const int n = 4 + Rand(8);
            const float r = RandF(0.3f, 2);
            const int c = AddVtx(v, o, z);
            int first = -1, prev = -1;
            for (int i = 0; i < n; ++i) {
                const float a = 6.2831853f * i / n;
                const int k = AddVtx(v, At(o, x, y, z, r * std::cos(a), r * std::sin(a), RandF(-0.4f, 0.4f)), z);
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
            const int n = 1 + Rand(4);
            for (int i = 0; i < n; ++i) {
                const S_vector a(RandF(-2, 2), RandF(-2, 2), RandF(-2, 2));
                S_vector b(a.x + RandF(-1.5f, 1.5f), a.y + RandF(-1.5f, 1.5f), a.z + RandF(-1.5f, 1.5f));
                S_vector c(a.x + RandF(-1.5f, 1.5f), a.y + RandF(-1.5f, 1.5f), a.z + RandF(-1.5f, 1.5f));
                if (Rand(12) == 0)
                    c = b;                  // (degenerate)
                const S_vector nn = RandUnit();
                AddTri(f, AddVtx(v, a, nn), AddVtx(v, b, nn), AddVtx(v, c, nn));
            }
            break;
        }
        }
    }
}

// ---------------------------------------------------------------- the scene

constexpr int kMats = 6, kTextures = 3;
uint8_t *g_mat[kMats];
// Fake textures (ITexture's fields the collision reads: +0x20 width, +0x22 height, +0x28 flags,
// +0x34 pixels: 32-bit BGRA, or a 1-bit alpha mask with ITC_PIXEL_MASK); every virtual a no-op.
alignas(16) uint8_t g_texture[kTextures][0x44];
std::vector<uint8_t> g_texels[kTextures];
void *g_textureVtable[64];

void MakeTextures()
{
    for (void *&slot : g_textureVtable)
        slot = reinterpret_cast<void *>(&NoOp);
    for (int k = 0; k < kTextures; ++k) {
        uint8_t *t = g_texture[k];
        std::memset(t, 0, sizeof g_texture[k]);
        Field<void **>(t, 0) = g_textureVtable;
        const uint16_t w = static_cast<uint16_t>(4 << Rand(3)), h = static_cast<uint16_t>(4 << Rand(3));
        Field<uint16_t>(t, 0x20) = w;
        Field<uint16_t>(t, 0x22) = h;
        const bool mask = k == 2;
        Field<uint32_t>(t, 0x28) = mask ? ITC_PIXEL_MASK : 0u;
        g_texels[k].resize(mask ? (w * h + 7) / 8 : static_cast<size_t>(w) * h * 4);
        for (uint8_t &b : g_texels[k])
            b = static_cast<uint8_t>(g_rng());
        Field<uint8_t *>(t, 0x34) = g_texels[k].data();
    }
}
uint8_t *g_scene;
uint8_t *g_sectors[6];                      // primary, omni, near, sectors in the primary
int g_numSectors;
std::vector<uint8_t *> g_frames;            // every frame (sectors first)
std::vector<uint8_t *> g_visuals;           // the visuals among them
uint8_t *g_camera;

uint32_t VisualType(uint8_t *f) { return CallV<uint32_t>(f, 0x04); }
bool IsVisual(uint8_t *f) { return Field<uint16_t>(f, 0xF8) == FRAME_VISUAL; }

uint8_t *MakeMesh(int levels)
{
    uint8_t *mesh = CallV<uint8_t *>(g_origDriver, 0x6C);
    for (int i = 0; i < levels; ++i) {
        std::vector<S_vtx> v;
        std::vector<I3D_triface> f;
        MakeGeometry(v, f);
        uint8_t *level = CallV<uint8_t *>(g_origDriver, 0x68);
        CallV<I3D_RESULT>(level, 0x24, static_cast<const void *>(v.data()), static_cast<int>(v.size()));
        const int groups = std::min(1 + Rand(3), static_cast<int>(f.size()));
        std::vector<std::vector<I3D_triface>> parts(groups);
        for (size_t k = 0; k < f.size(); ++k)
            parts[k < static_cast<size_t>(groups) ? k : Rand(groups)].push_back(f[k]);
        for (int g = 0; g < groups; ++g) {
            uint8_t *fg = CallV<uint8_t *>(g_origDriver, 0x64);
            if (Rand(8))
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

void GiveMesh(uint8_t *vis)
{
    uint8_t *mesh = MakeMesh(1 + Rand(3));
    CallV<I3D_RESULT>(vis, 0x9C, mesh, 0u);
    CallV<uint32_t>(mesh, 0x00);
}

void RandomizeMaterial(uint8_t *m)
{
    const S_vector4 d = {RandF(0, 1), RandF(0, 1), RandF(0, 1), 1};
    CallV<I3D_RESULT>(m, 0x08, &d);
    CallV<I3D_RESULT>(m, 0x18, Rand(2) ? 1.0f : RandF(0, 1));
    uint32_t &flags = Field<uint32_t>(m, 0x08);
    flags = Rand(3) ? (flags & ~MATF_TWO_SIDED) : (flags | MATF_TWO_SIDED);
    flags &= ~(MATF_ADDITIVE | MATF_ALPHA | MATF_COLORKEY);
    // A fake texture (its texels then decide: alpha / colour key / additive past the opaque test).
    Field<void *>(m, 0x0C) = Rand(2) ? g_texture[Rand(kTextures)] : nullptr;
    if (Rand(4) == 0)
        flags |= MATF_ADDITIVE;
    if (Field<void *>(m, 0x0C) && Rand(3))
        flags |= Rand(2) ? MATF_ALPHA : MATF_COLORKEY;
}

void Place(uint8_t *f)
{
    const S_vector pos(RandF(-6, 6), RandF(-6, 6), RandF(-6, 6));
    const S_vector dir = RandUnit();
    CallV<void>(f, 0x14, &pos, &dir, RandF(-3, 3));
}

// A frame's scale (the transform setters keep it; the local matrix is made again from it).
void Scale(uint8_t *f)
{
    const float s = Rand(3) ? 1.0f : RandF(0.3f, 2.5f);
    const S_vector sc = Rand(2) ? S_vector(s, s, s) : S_vector(RandF(0.4f, 2), RandF(0.4f, 2), RandF(0.4f, 2));
    Field<S_vector>(f, 0xD0) = sc;
    Field<uint32_t>(f, 0xE0) &= ~(FRMFLAGS_LMATRIX_VALID | FRMFLAGS_WMATRIX_VALID | FRMFLAGS_BOUND_VALID);
    Field<uint32_t>(f, 0xE0) |= FRMFLAGS_CHANGED;
    for (uint8_t *p = Field<uint8_t *>(f, 0x108); p; p = Field<uint8_t *>(p, 0x108))
        Field<uint32_t>(p, 0xE0) &= ~FRMFLAGS_BOUND_VALID;
}

// The render camera (billboard mode 4 faces it): the render context's, its inverse view matrix its
// world matrix (the renderer sets both).
void PlaceCamera()
{
    Place(g_camera);
    Orig<void *>(0x100d11f4) = g_camera;
    Field<S_matrix>(g_camera, 0x210) = reinterpret_cast<I3D_frame *>(g_camera)->GetMatrix();
}

void UpdateScene()
{
    for (int k = 0; k < g_numSectors; ++k)
        CallV<void>(g_sectors[k], 0x24);
    // Usually the hierarchy and world bounds made again too (moved frames leave their ancestors'
    // bounds as they were, and the walks prune by them).
    if (Rand(4)) {
        for (uint8_t *f : g_frames) {
            Field<uint32_t>(f, 0xE0) &= ~FRMFLAGS_BOUND_VALID;
            if (IsVisual(f))
                Field<uint32_t>(f, 0x1D0) &= ~(VISF_WBOUND_VALID | VISF_BOUND_VALID);
        }
    }
}

int FrameId(const void *f)
{
    if (!f)
        return -1;
    for (size_t k = 0; k < g_frames.size(); ++k) {
        if (g_frames[k] == f)
            return static_cast<int>(k);
    }
    return -2;
}

void BuildScene()
{
    g_scene = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_SCENE));
    g_sectors[0] = Field<uint8_t *>(g_scene, 0x1F8);
    g_sectors[1] = Field<uint8_t *>(g_scene, 0x200);
    g_sectors[2] = Field<uint8_t *>(g_scene, 0x204);
    if (!g_sectors[0] || !g_sectors[1] || !g_sectors[2]) {
        std::printf("the original's scene has no special sectors\n");
        std::exit(2);
    }
    g_numSectors = 3;
    for (int k = 0; k < 3; ++k) {
        uint8_t *s = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_SECTOR));
        CallV<I3D_RESULT>(s, 0x38, g_sectors[0], 0u);
        g_sectors[g_numSectors++] = s;
    }
    for (int k = 0; k < g_numSectors; ++k)
        g_frames.push_back(g_sectors[k]);
    MakeTextures();
    for (uint8_t *&m : g_mat) {
        m = CallV<uint8_t *>(g_origDriver, 0x58);
        RandomizeMaterial(m);
    }
    g_camera = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_CAMERA));
    CallV<I3D_RESULT>(g_camera, 0x38, g_sectors[0], 0u);
    PlaceCamera();
    static const uint32_t kTypes[] = {FourCC("OBJ_"), FourCC("OBJ_"), FourCC("LMAP"), FourCC("BBRD"), FourCC("MRPH")};
    for (int k = 0; k < 36; ++k) {
        uint8_t *f;
        if (Rand(5) == 0) {
            f = CallV<uint8_t *>(g_origDriver, 0x54, static_cast<int>(FRAME_DUMMY));
        } else {
            f = CallV<uint8_t *>(g_origDriver, 0x50, kTypes[Rand(5)]);
            GiveMesh(f);
            if (VisualType(f) == FourCC("BBRD")) {
                Field<int>(f, 0x210) = Rand(3);
                Field<int>(f, 0x214) = Rand(5);
            }
            g_visuals.push_back(f);
        }
        // Into a sector, or under an earlier frame.
        uint8_t *parent = Rand(2) ? g_sectors[Rand(g_numSectors)] : g_frames[Rand(static_cast<int>(g_frames.size()))];
        CallV<I3D_RESULT>(f, 0x38, parent, 0u);
        Place(f);
        if (Rand(4) == 0)
            Scale(f);
        g_frames.push_back(f);
    }
    UpdateScene();
}

// A change to the scene between queries.
void Mutate()
{
    uint8_t *f = g_frames[g_numSectors + Rand(static_cast<int>(g_frames.size()) - g_numSectors)];
    char text[160];
    switch (Rand(10)) {
    case 0:
    case 1:
        Place(f);
        std::snprintf(text, sizeof text, "move %d", FrameId(f));
        break;
    case 2:
        Scale(f);
        std::snprintf(text, sizeof text, "scale %d", FrameId(f));
        break;
    case 3: {
        const bool on = Rand(3) != 0;
        CallV<void>(f, 0x30, on);
        std::snprintf(text, sizeof text, "%d on %d", FrameId(f), on);
        break;
    }
    case 4: {
        // Elsewhere in the hierarchy (a sector or a frame not below it).
        uint8_t *parent = g_sectors[Rand(g_numSectors)];
        if (Rand(2)) {
            uint8_t *p = g_frames[Rand(static_cast<int>(g_frames.size()))];
            bool below = false;
            for (uint8_t *a = p; a; a = Field<uint8_t *>(a, 0x108))
                below |= a == f;
            if (!below)
                parent = p;
        }
        CallV<I3D_RESULT>(f, 0x38, parent, 0u);
        std::snprintf(text, sizeof text, "link %d to %d", FrameId(f), FrameId(parent));
        break;
    }
    case 5:
        if (IsVisual(f)) {
            GiveMesh(f);
            std::snprintf(text, sizeof text, "mesh %d", FrameId(f));
        } else {
            std::snprintf(text, sizeof text, "nothing");
        }
        break;
    case 6:
        if (IsVisual(f) && VisualType(f) != FourCC("BBRD")) {
            const bool on = Rand(3) == 0;
            CallV<void>(f, 0x7C, on);
            std::snprintf(text, sizeof text, "%d world space %d", FrameId(f), on);
        } else if (IsVisual(f)) {
            Field<int>(f, 0x210) = Rand(3);
            Field<int>(f, 0x214) = Rand(5);
            std::snprintf(text, sizeof text, "billboard %d axis %d mode %d", FrameId(f), Field<int>(f, 0x210),
                          Field<int>(f, 0x214));
        } else {
            std::snprintf(text, sizeof text, "nothing");
        }
        break;
    case 7:
        RandomizeMaterial(g_mat[Rand(kMats)]);
        std::snprintf(text, sizeof text, "material");
        break;
    case 8:
        if (IsVisual(f))
            Field<uint32_t>(f, 0x1D0) ^= 0x1000;
        std::snprintf(text, sizeof text, "flag 0x1000 of %d", FrameId(f));
        break;
    default:
        PlaceCamera();
        std::snprintf(text, sizeof text, "camera");
        break;
    }
    g_lastOp = text;
    if (Rand(2))
        UpdateScene();
}

// ---------------------------------------------------------------- callbacks

// The hit callbacks' and filters' calls of a query, per side, and their answers.
struct S_call {
    int kind;                               // 0 hit callback, 1 filter
    int frame;
    uint32_t a, b, c, d;                    // distance / t, normal or texel, ...
    const void *material;
    uint32_t color[3];
};
std::vector<S_call> g_calls[2];
long g_seenTexelCalls;
int g_side;
uint32_t g_answerSeed;
bool g_tintInFilter;

uint32_t Mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

uint32_t Bits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

int __stdcall HitCallback(const S_vector *normal, float distance, I3D_frame *frame, void *user)
{
    S_call c = {};
    c.kind = 0;
    c.frame = FrameId(frame);
    c.a = Bits(distance);
    if (normal) {
        c.b = Bits(normal->x);
        c.c = Bits(normal->y);
        c.d = Bits(normal->z);
    }
    if (user)
        std::memcpy(c.color, user, 12);
    std::vector<S_call> &log = g_calls[g_side];
    const uint32_t h = Mix(g_answerSeed + static_cast<uint32_t>(log.size()) * 0x9e3779b9u) % 20;
    log.push_back(c);
    return h < 12 ? 0 : h < 14 ? 1 : h < 18 ? 2 : 3;
}

int __stdcall TestFilter(const void *, float t, I3D_frame *frame, void *user, const S_collisionTexel *texel)
{
    S_call c = {};
    c.kind = 1;
    c.frame = FrameId(frame);
    c.a = Bits(t);
    c.b = Bits(texel->u);
    c.c = Bits(texel->v);
    c.material = texel->material;
    if (user)
        std::memcpy(c.color, user, 12);
    std::vector<S_call> &log = g_calls[g_side];
    const uint32_t h = Mix(g_answerSeed ^ (static_cast<uint32_t>(log.size()) * 0x85ebca6bu)) % 10;
    log.push_back(c);
    if (user && g_tintInFilter) {
        auto *col = static_cast<float *>(user);
        col[0] = col[0] * 0.75f;
        col[2] = col[2] * 0.5f + 0.125f;
    }
    return h < 3 ? 0 : h < 7 ? 1 : 2;
}

// ---------------------------------------------------------------- comparing

void CheckBits(const std::string &what, const void *a, const void *b, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, static_cast<const uint8_t *>(a) + i, 4);
        std::memcpy(&y, static_cast<const uint8_t *>(b) + i, 4);
        Check(what + " +" + std::to_string(i), static_cast<long>(x), static_cast<long>(y));
    }
}

void CompareCalls()
{
    Check("callback calls", static_cast<long>(g_calls[0].size()), static_cast<long>(g_calls[1].size()));
    for (size_t k = 0; k < g_calls[0].size(); ++k) {
        const S_call &a = g_calls[0][k], &b = g_calls[1][k];
        const std::string at = "call " + std::to_string(k) + (a.kind ? " (filter)" : " (hit)");
        Check(at + " kind", a.kind, b.kind);
        Check(at + " frame", a.frame, b.frame);
        Check(at + " distance / t", static_cast<long>(a.a), static_cast<long>(b.a));
        // (A filter's texture coordinates only with a texture, a hit's normal only for a box the ray
        // enters: the original leaves them unset.)
        float distance;
        std::memcpy(&distance, &a.a, 4);
        const bool values = a.kind ? (a.material && Field<void *>(const_cast<void *>(a.material), 0x0C) != nullptr)
                                   : distance < 1e16f;
        if (values && a.kind)
            ++g_seenTexelCalls;
        if (values) {
            Check(at + " b", static_cast<long>(a.b), static_cast<long>(b.b));
            Check(at + " c", static_cast<long>(a.c), static_cast<long>(b.c));
            if (!a.kind)
                Check(at + " d", static_cast<long>(a.d), static_cast<long>(b.d));
        }
        Check(at + " material", a.material == b.material, true);
        CheckBits(at + " colour", a.color, b.color, 12);
    }
}

// The queries' state: the original's globals against the port's col::q.
// (`normal`: the ray query hit and wanted a normal; else q.normal may hold what the original's
// box test left unset.)
void CompareQueryState(bool normal)
{
    auto o = [](uint32_t va) -> const uint8_t * { return &Orig<uint8_t>(va); };
    CheckBits("q.cbResult", o(0x100ad9b0), &q.cbResult, 4);
    if (normal)
        CheckBits("q.normal", o(0x100ad9e0), &q.normal, 12);
    Check("q.done", *o(0x100ad9d0) != 0, q.done);
    CheckBits("q.lodLevel", o(0x100ada00), &q.lodLevel, 4);
    CheckBits("q.length2", o(0x100ada04), &q.length2, 4);
    CheckBits("q.hitFrame", o(0x100ada08), &q.hitFrame, 4);
    CheckBits("q.hitGroup", o(0x100ada0c), &q.hitGroup, 4);
    CheckBits("q.flags", o(0x100ada14), &q.flags, 4);
    CheckBits("q.sweepMin", o(0x100ada20), &q.sweepMin, 12);
    CheckBits("q.sweepMax", o(0x100ada30), &q.sweepMax, 12);
    CheckBits("q.hitFace", o(0x100ada40), q.hitFace, 12);
    CheckBits("q.result", o(0x100ada80), &q.result, 12);
    CheckBits("q.frameLodLevel", o(0x100ada90), &q.frameLodLevel, 4);
    CheckBits("q.bestDist2", o(0x100ada94), &q.bestDist2, 4);
    CheckBits("q.from", o(0x100adaa0), &q.from, 12);
    CheckBits("q.scene", o(0x100adab0), &q.scene, 4);
    CheckBits("q.to", o(0x100adac0), &q.to, 12);
    CheckBits("q.length", o(0x100adad0), &q.length, 4);
    CheckBits("q.dir", o(0x100adae0), &q.dir, 12);
    CheckBits("q.radius", o(0x100adaf4), &q.radius, 4);
    CheckBits("q.hitCb", o(0x100adaf8), &q.hitCb, 4);
    // (The port's built-in filters stand for the original's.)
    void *portFilter = reinterpret_cast<void *>(q.filter);
    if (portFilter == reinterpret_cast<void *>(&AlphaFilter))
        portFilter = &Orig<uint8_t>(0x10010a90);
    else if (portFilter == reinterpret_cast<void *>(&TransmitFilter))
        portFilter = &Orig<uint8_t>(0x10064860);
    Check("q.filter", *reinterpret_cast<void *const *>(o(0x100adafc)) == portFilter, true);
    CheckBits("q.excluded", o(0x100adb00), &q.excluded, 4);
    CheckBits("q.skipped", o(0x100adb04), &q.skipped, 4);
    CheckBits("q.lodFrame", o(0x100adb08), &q.lodFrame, 4);
}

// The cached inverse matrices made again by whichever side queries next.
void InvalidateInverses()
{
    for (uint8_t *v : g_visuals)
        Field<uint32_t>(v, 0x1D0) &= ~VISF_INVMATRIX_VALID;
}

// ---------------------------------------------------------------- the queries

S_vector RandPoint()
{
    if (Rand(3) && !g_visuals.empty()) {
        // Near a visual.
        uint8_t *v = g_visuals[Rand(static_cast<int>(g_visuals.size()))];
        const S_vector &m = reinterpret_cast<I3D_frame *>(v)->GetMatrix()(3);
        return S_vector(m.x + RandF(-3, 3), m.y + RandF(-3, 3), m.z + RandF(-3, 3));
    }
    return S_vector(RandF(-10, 10), RandF(-10, 10), RandF(-10, 10));
}

S_vector RandDir(const S_vector &from)
{
    S_vector d;
    if (Rand(3)) {
        const S_vector to = RandPoint();
        d = S_vector(to.x - from.x, to.y - from.y, to.z - from.z);
        const float k = Rand(3) ? RandF(0.5f, 1.5f) : RandF(0.05f, 4);
        d = S_vector(d.x * k, d.y * k, d.z * k);
    } else {
        const S_vector u = RandUnit();
        const float len = Rand(4) ? RandF(0.2f, 15) : RandF(0, 0.05f);
        d = S_vector(u.x * len, u.y * len, u.z * len);
    }
    if (Rand(60) == 0)
        d = S_vector(0, 0, 0);
    return d;
}

uint32_t RandRayFlags()
{
    static const uint32_t kBits[] = {I3DCOL_BOX_ONLY, I3DCOL_NO_JOINTS, I3DCOL_ALL_HITS, I3DCOL_FILTER,
                                     I3DCOL_UNLIMITED, I3DCOL_TWO_SIDED, I3DCOL_FIRST_HIT, I3DCOL_LOD_FRAME,
                                     I3DCOL_SKIP_FLAGGED, I3DCOL_HIDDEN_TOO, I3DCOL_SKIP_FRAME, I3DCOL_OMNI_SECTOR,
                                     I3DCOL_NEAR_SECTOR, I3DCOL_FIRST_FACE};
    uint32_t f = 0;
    for (uint32_t b : kBits) {
        if (Rand(b == I3DCOL_BOX_ONLY || b == I3DCOL_FIRST_HIT ? 10 : 4) == 0)
            f |= b;
    }
    return f;
}

uint8_t *RandFrameArg()
{
    if (Rand(3) == 0)
        return nullptr;
    return g_frames[Rand(static_cast<int>(g_frames.size()))];
}

long g_seenCalls, g_seenQueries;
long g_seenKind[7], g_seenKindHits[7];

// A random point on a random face of a visual's first mesh level, in world space (false when it
// has none; billboards are drawn turned, so their points are only near their faces).
bool PointOnFace(uint8_t *v, S_vector &out)
{
    uint8_t *mesh = Field<uint8_t *>(v, 0x1E0);
    if (!mesh)
        return false;
    uint8_t *level = Field<uint8_t *>(mesh, 0x48);
    if (!level || !Field<int>(level, 0x54))
        return false;
    std::vector<uint8_t *> groups;
    for (uint8_t *fg = Field<uint8_t *>(level, 0x5C); fg; fg = Field<uint8_t *>(fg, 0x0C)) {
        if (Field<int>(fg, 0x08) > 0)
            groups.push_back(fg);
    }
    if (groups.empty())
        return false;
    uint8_t *fg = groups[Rand(static_cast<int>(groups.size()))];
    const auto *vtx = CallV<const uint8_t *>(level, 0x04, 1u);
    const auto *faces = CallV<const I3D_triface *>(fg, 0x0C, 1u);
    if (!vtx || !faces)
        return false;
    const I3D_triface &f = faces[Rand(Field<int>(fg, 0x08))];
    auto p = [vtx](uint16_t i) { return *reinterpret_cast<const S_vector *>(vtx + i * 0x20); };
    float a = RandF(0, 1), b = RandF(0, 1);
    if (a + b > 1.0f) {
        a = 1.0f - a;
        b = 1.0f - b;
    }
    const S_vector p0 = p(f.v[0]), p1 = p(f.v[1]), p2 = p(f.v[2]);
    const S_vector local(p0.x + (p1.x - p0.x) * a + (p2.x - p0.x) * b, p0.y + (p1.y - p0.y) * a + (p2.y - p0.y) * b,
                         p0.z + (p1.z - p0.z) * a + (p2.z - p0.z) * b);
    if (Field<uint32_t>(v, 0x1D0) & VISF_WORLD_SPACE)
        out = local;
    else
        out = local * reinterpret_cast<I3D_frame *>(v)->GetMatrix();
    return true;
}

// Both sides' outputs, filled with a pattern first.
struct S_out {
    float colour[4];
    float distance;
    S_vector4 point, normal;
    I3D_collision_info info;
};

void Fill(S_out &o)
{
    std::memset(&o, 0xcd, sizeof o);
    o.colour[0] = 1.0f;
    o.colour[1] = 0.8f;
    o.colour[2] = 0.6f;
    o.colour[3] = 0.0f;
}

void RunQuery()
{
    char text[300];
    const int kind = Rand(7);
    S_vector from, dir;
    uint8_t *targetVisual = g_visuals[Rand(static_cast<int>(g_visuals.size()))];
    if (Rand(10) < 7) {
        // At a visual: from outside (or inside) its bound toward a point on one of its faces (or near
        // its centre).
        auto *vis = reinterpret_cast<I3D_visual *>(targetVisual);
        vis->GetWorldBound();
        const S_vector &c = vis->worldBound.center;
        const float r = vis->worldBound.radius;
        S_vector target(c.x + RandF(-0.6f, 0.6f) * r, c.y + RandF(-0.6f, 0.6f) * r, c.z + RandF(-0.6f, 0.6f) * r);
        if (Rand(5))
            PointOnFace(reinterpret_cast<uint8_t *>(vis), target);
        const S_vector u = RandUnit();
        const float away = Rand(6) ? RandF(1.0f, 3.0f) * r + 0.5f : RandF(0, 0.5f) * r;
        from = S_vector(target.x + u.x * away, target.y + u.y * away, target.z + u.z * away);
        const float k = Rand(4) ? RandF(1.0f, 2.5f) : RandF(0.3f, 1.0f);
        dir = S_vector((target.x - from.x) * k, (target.y - from.y) * k, (target.z - from.z) * k);
    } else {
        from = RandPoint();
        dir = RandDir(from);
    }
    // (Sometimes only the sectors and the sidedness: hits, then, more often.)
    uint32_t flags = Rand(10) < 3 ? (RandRayFlags() & (I3DCOL_OMNI_SECTOR | I3DCOL_NEAR_SECTOR | I3DCOL_TWO_SIDED |
                                                       I3DCOL_UNLIMITED))
                                  : RandRayFlags();
    uint8_t *frame = RandFrameArg();
    const int lod = Rand(4) ? 0 : Rand(3);
    const bool withCb = Rand(2) != 0;
    bool withUser = Rand(4) != 0;
    const bool withOuts = Rand(6) != 0;
    // The filter: none, the test's, the built-in transmit filter (which needs a colour), the
    // built-in alpha (none given).
    const int filterKind = Rand(4);
    if (filterKind == 2)
        withUser = true;
    if (filterKind && Rand(3))
        flags |= I3DCOL_FILTER;
    g_tintInFilter = Rand(2) != 0;
    g_answerSeed = g_rng();
    const float radius = Rand(5) ? RandF(0.05f, 1.5f) : RandF(0.001f, 0.05f);
    // The other arguments some queries take.
    // The subtree queries' root: mostly the target or one of its ancestors.
    uint8_t *root = targetVisual;
    for (int up = Rand(3); up > 0 && Field<uint8_t *>(root, 0x108); --up)
        root = Field<uint8_t *>(root, 0x108);
    if (Rand(5) == 0)
        root = g_frames[Rand(static_cast<int>(g_frames.size()))];
    uint8_t *queryFrame = RandFrameArg();
    const uint32_t iflags = flags | (Rand(8) == 0 ? I3DCOL_DONE_AT_ROOT : 0);
    uint8_t *sphereVisual = Rand(5) ? targetVisual : g_visuals[Rand(static_cast<int>(g_visuals.size()))];
    const S_vector toA(from.x + dir.x, from.y + dir.y, from.z + dir.z);
    // (The other sphere crosses the first one's path, or not.)
    const float along = RandF(0, 1);
    const S_vector meet(from.x + dir.x * along, from.y + dir.y * along, from.z + dir.z * along);
    const S_vector across = RandUnit();
    const float off = RandF(0, 3);
    const S_vector fromB = Rand(3) ? S_vector(meet.x + across.x * off, meet.y + across.y * off, meet.z + across.z * off)
                                   : RandPoint();
    const S_vector toB = Rand(4) ? S_vector(meet.x - across.x * off, meet.y - across.y * off, meet.z - across.z * off) : fromB;
    const float radiusB = Rand(4) ? RandF(0.05f, 2) : 0.0f;
    std::snprintf(text, sizeof text,
                  "op %ld query %d from %g %g %g dir %g %g %g flags %x frame %d lod %d cb %d user %d filter %d r %g",
                  g_op, kind, from.x, from.y, from.z, dir.x, dir.y, dir.z, flags, FrameId(frame), lod, withCb, withUser,
                  filterKind, radius);
    g_lastOp = text;

    auto *portScene = reinterpret_cast<I3D_scene *>(g_scene);
    I3D_COLLISION_HIT *cb = withCb ? &HitCallback : nullptr;
    S_out out[2];
    void *result[2] = {nullptr, nullptr};
    float sweep[2] = {0, 0};
    const bool portFirst = Rand(2) != 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int side = (pass == 0) == portFirst ? 1 : 0;
        g_side = side;
        g_calls[side].clear();
        Fill(out[side]);
        InvalidateInverses();
        void *user = withUser ? out[side].colour : nullptr;
        I3D_COLLISION_FILTER *filter = nullptr;
        void *origFilter = nullptr;
        if (filterKind == 1) {
            filter = &TestFilter;
            origFilter = reinterpret_cast<void *>(&TestFilter);
        } else if (filterKind == 2) {
            filter = &TransmitFilter;
            origFilter = &Orig<uint8_t>(0x10064860);
        }
        float *dist = withOuts ? &out[side].distance : nullptr;
        S_vector4 *point = withOuts ? &out[side].point : nullptr;
        S_vector4 *normal = withOuts ? &out[side].normal : nullptr;
        if (side == 0) {
            // (The port clears what the original keeps from the previous query.)
            if ((flags & I3DCOL_SKIP_FRAME) && (kind == 1 || kind == 2 || kind == 3 || kind == 4))
                Orig<void *>(0x100adb00) = nullptr;
            if (kind >= 3 && kind <= 5)
                Orig<void *>(0x100adb08) = nullptr;
        }
        switch (kind) {
        case 0:
            result[side] = side ? portScene->I3D_scene::PickFrame(from, dir, flags, point, normal, cb, user, filter, lod)
                                : CallV<void *>(g_scene, 0xFC, &from, &dir, flags, point, normal, cb, user, origFilter, lod);
            break;
        case 1:
            result[side] = side ? portScene->I3D_scene::TestRay(from, dir, flags, dist, normal, cb, user, filter,
                                                                 reinterpret_cast<I3D_frame *>(frame), lod)
                                : CallV<void *>(g_scene, 0x100, &from, &dir, flags, dist, normal, cb, user, origFilter,
                                                frame, lod);
            break;
        case 2:
            result[side] = side ? portScene->I3D_scene::IntersectFrame(from, dir, reinterpret_cast<I3D_frame *>(root),
                                                                        &out[side].info, iflags,
                                                                        reinterpret_cast<I3D_frame *>(queryFrame), lod, cb)
                                : CallV<void *>(g_scene, 0x104, &from, &dir, root, &out[side].info, iflags, queryFrame,
                                                lod, cb);
            break;
        case 3:
            result[side] = side ? portScene->I3D_scene::MoveSphere(from, dir, radius, flags, point,
                                                                    reinterpret_cast<I3D_frame *>(frame), cb)
                                : CallV<void *>(g_scene, 0x108, &from, &dir, radius, flags, point, frame, cb);
            break;
        case 4:
            result[side] = side ? portScene->I3D_scene::MoveSphereFrame(from, dir, radius,
                                                                         reinterpret_cast<I3D_frame *>(root), flags,
                                                                         point, reinterpret_cast<I3D_frame *>(frame), cb)
                                : CallV<void *>(g_scene, 0x10C, &from, &dir, radius, root, flags, point, frame, cb);
            break;
        case 5:
            result[side] = side ? portScene->I3D_scene::MoveSphereVisual(from, dir, radius,
                                                                          reinterpret_cast<I3D_frame *>(sphereVisual),
                                                                          flags, point)
                                : CallV<void *>(g_scene, 0x110, &from, &dir, radius, sphereVisual, flags, point);
            break;
        default:
            sweep[side] = side ? portScene->I3D_scene::SweepSpheres(radius, from, toA, radiusB, fromB, toB)
                               : CallV<float>(g_scene, 0x114, radius, &from, &toA, radiusB, &fromB, &toB);
            break;
        }
    }
    g_side = 0;
    ++g_seenQueries;
    Check("result", FrameId(result[0]), FrameId(result[1]));
    ++g_seenKind[kind];
    if (result[0] || (kind == 6 && sweep[0] >= 0.0f))
        ++g_seenKindHits[kind];
    CheckBits("sweep", &sweep[0], &sweep[1], 4);
    CheckBits("colour", out[0].colour, out[1].colour, 12);
    CheckBits("distance", &out[0].distance, &out[1].distance, 4);
    CheckBits("point", &out[0].point, &out[1].point, 12);
    CheckBits("normal", &out[0].normal, &out[1].normal, 12);
    CheckBits("info distance", &out[0].info.distance, &out[1].info.distance, 4);
    CheckBits("info normal", &out[0].info.normal, &out[1].info.normal, 12);
    CheckBits("info face", out[0].info.face, out[1].info.face, 6);
    CheckBits("info face group", &out[0].info.faceGroup, &out[1].info.faceGroup, 4);
    CompareCalls();
    g_seenCalls += static_cast<long>(g_calls[0].size());
    if (kind != 6)
        CompareQueryState(kind <= 2 && result[0] && (kind == 2 || withCb || withOuts));
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 20000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    if (!SetUpBuffers(origPath))
        return 2;
    g_logDisabled = true;
    _controlfp(_PC_24, _MCW_PC);
    g_rng.seed(seed);
    g_ownerIndex = [](const void *, int, bool) { return 0; };
    g_origDriver = Orig<uint8_t *>(0x100d16f8);
    BuildScene();
    if (argc > 4 && std::atoi(argv[4]) == 1) {
        // (diagnosis) each visual: where it is, and an all-sides ray of the original's at it
        for (uint8_t *v : g_visuals) {
            auto *vis = reinterpret_cast<I3D_visual *>(v);
            vis->GetWorldBound();
            S_vector c = vis->worldBound.center;
            const bool onFace = PointOnFace(v, c);
            const S_vector from(c.x + 30, c.y + 7, c.z - 11);
            const S_vector dir(c.x - from.x, c.y - from.y, c.z - from.z);
            float dist = -1;
            void *hit = CallV<void *>(g_scene, 0x100, &from, &dir,
                                      I3DCOL_TWO_SIDED | I3DCOL_UNLIMITED | I3DCOL_HIDDEN_TOO | I3DCOL_OMNI_SECTOR |
                                          I3DCOL_NEAR_SECTOR,
                                      &dist, static_cast<S_vector4 *>(nullptr), static_cast<void *>(nullptr),
                                      static_cast<void *>(nullptr), static_cast<void *>(nullptr),
                                      static_cast<void *>(nullptr), 0);
            int depth = 0;
            uint8_t *top = v;
            while (Field<uint8_t *>(top, 0x108)) {
                top = Field<uint8_t *>(top, 0x108);
                ++depth;
            }
            const uint32_t t = VisualType(v);
            std::printf("visual %d %.4s face %d on %d flags %08x radius %g top %d depth %d -> hit %d dist %g\n",
                        FrameId(v), reinterpret_cast<const char *>(&t), onFace, Field<uint32_t>(v, 0xE0) & 1,
                        Field<uint32_t>(v, 0xE0),
                        vis->worldBound.radius, FrameId(top), depth, FrameId(hit), dist);
        }
        return 0;
    }
    if (argc > 4 && std::atoi(argv[4]) == 2) {
        // (diagnosis) the hit rate of face-targeted rays without flags, one- and two-sided
        for (uint32_t extra : {0u, static_cast<uint32_t>(I3DCOL_TWO_SIDED)}) {
            int hits = 0, n = 0;
            for (int k = 0; k < 2000; ++k) {
                uint8_t *v = g_visuals[Rand(static_cast<int>(g_visuals.size()))];
                S_vector target;
                if (!PointOnFace(v, target))
                    continue;
                const S_vector u = RandUnit();
                const S_vector from(target.x + u.x * 8, target.y + u.y * 8, target.z + u.z * 8);
                const S_vector dir((target.x - from.x) * 1.5f, (target.y - from.y) * 1.5f, (target.z - from.z) * 1.5f);
                void *hit = CallV<void *>(g_scene, 0x100, &from, &dir, extra | I3DCOL_OMNI_SECTOR | I3DCOL_NEAR_SECTOR,
                                          static_cast<float *>(nullptr), static_cast<S_vector4 *>(nullptr),
                                          static_cast<void *>(nullptr), static_cast<void *>(nullptr),
                                          static_cast<void *>(nullptr), static_cast<void *>(nullptr), 0);
                ++n;
                hits += hit != nullptr;
            }
            std::printf("flags %x: %d of %d rays hit\n", extra, hits, n);
        }
        return 0;
    }
    for (g_op = 0; g_op < ops; ++g_op) {
        if (Rand(3) == 0)
            Mutate();
        else
        {
            if (Rand(10) < 7)
                UpdateScene();
            RunQuery();
        }
    }
    static const char *const kNames[7] = {"PickFrame", "TestRay", "IntersectFrame", "MoveSphere", "MoveSphereFrame",
                                          "MoveSphereVisual", "SweepSpheres"};
    std::printf("coverage: %ld queries, callback / filter calls %ld (filter calls with a texel %ld); hits:",
                g_seenQueries, g_seenCalls, g_seenTexelCalls);
    for (int k = 0; k < 7; ++k)
        std::printf(" %s %ld / %ld%s", kNames[k], g_seenKindHits[k], g_seenKind[k], k < 6 ? "," : "\n");
    std::printf("compare_collision: %ld operations, seed %u: identical\n", ops, seed);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
