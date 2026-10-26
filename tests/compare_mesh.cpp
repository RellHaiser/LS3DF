// Differential test of the meshes (src/i3d/mesh.cpp: I3D_face_group, I3D_mesh_level, I3D_mesh)
// on the buffer managers (vb_manager.cpp, ib_manager.cpp): the original LS3DF.dll's objects (made
// by its driver) and the port's run the same random operations - vertices and faces set, allocated,
// locked and written through the returned pointers, face groups added, removed, duplicated, their
// materials shared and duplicated (plain and animated, with environment flags), levels duplicated,
// their extra data, render vertex buffers (every layout, generated environment coordinates through
// random matrices and DRVSTATE_ENV_UV_SCALE), bounds, statistics, meshes' levels set and cleared,
// meshes duplicated and compared, references dropped, the renderer's UnlockAll - each side on its
// own fake Direct3D device (buffer_check.h). After every operation everything must be identical:
// both managers node for node, every object's fields (levels a mesh made itself compared by
// content), the vertex, render vertex, extra and face data, the materials' references, the device
// logs; every fake buffer's bytes now and then.
//
// Left out, as the original misbehaves: removing a face group that is not a level's first (the
// original truncates the list there; mesh.cpp fixes it), Unlock of a face group without faces
// (reads through null), a mesh's GetStats with an empty level below its last (the same), and empty
// allocations (see compare_buffers.cpp): no zero counts, no empty
// face group joins a level or is a Duplicate source, no render buffer for a level without vertices.
// A mesh holding levels past its count is not released (the original leaks them, the port
// releases them). A level is not duplicated from one without face groups while it has some (the original keeps
// pointing at its deleted list). A level's extra data is made again after its vertex count changes (Duplicate copies the source's
// count of entries: the original would read past an array made for fewer vertices).
//
// Usage: compare_mesh.exe [original LS3DF.dll] [operations] [seed]
//
// test-sources: src/i3d/mesh.cpp src/i3d/material.cpp src/i3d/ib_manager.cpp src/i3d/vb_manager.cpp src/i3d/driver_factories.cpp src/i3d/anim.cpp src/math/math.cpp src/core/debug.cpp src/dx/dxerr8.cpp
// test-sources: src/gfx/d3d8/gfx_d3d8.cpp
#include <float.h>

#include <algorithm>
#include <cmath>

#include "../src/core/debug.h"
#include "../src/i3d/anim.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/i3d_hooks.h"
#include "../src/i3d/loader.h"
#include "../src/i3d/material.h"
#include "../src/i3d/mesh.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/igraph_state.h"
#include "buffer_check.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d::i3d {
I3D_driver *g_driver;
uint32_t g_driverStates[DRVSTATE_COUNT];
int g_loadFile;
int g_driverTime;
I3D_RESULT OpenAnimationFile(const char *, I3D_animation_set *) { return I3DERR_NOFILE; }
}  // namespace ls3d::i3d
IGraph *g_pIGraph;

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
const char *__stdcall I3D_driver::GetFrameTypeName(int) { return nullptr; }
const char *__stdcall I3D_driver::GetVisualTypeName(uint32_t) { return nullptr; }
I3D_RESULT __stdcall I3D_driver::EnumVisualTypes(I3D_VISUALTYPEPROC *, bool) { return 0; }
I3D_RESULT __stdcall I3D_driver::ResetDevice(I3D_scene *) { return 0; }
I3D_frame *__stdcall I3D_driver::FindFrame(I3D_scene *, const char *) { return nullptr; }
void __stdcall I3D_driver::RebalanceNameTree() {}
I3D_RESULT __stdcall I3D_driver::EnumNamedFrames(I3D_ENUMPROC *, void *) { return 0; }
void __stdcall I3D_driver::FrameReleased(I3D_frame *) {}
void __stdcall I3D_driver::RemoveShadowCaster(I3D_frame *) {}
void __stdcall I3D_driver::RemoveTargetFrame(I3D_frame *) {}
void __stdcall I3D_driver::SetHierarchyLightLevel(I3D_frame *, float) {}
void __stdcall I3D_driver::SetHierarchyAlpha(I3D_frame *, float) {}
I3D_bbox *__stdcall I3D_driver::GetHierarchyBound(I3D_bbox &out, I3D_frame *) { return &out; }
I3D_bbox *__stdcall I3D_driver::GetHierarchyBBox(I3D_bbox &out, I3D_frame *) { return &out; }

using namespace ls3d::i3d;

namespace {

uint8_t *g_origDriver;

template <class R, class... A> R CallV(void *object, int slot, A... args)
{
    using F = R(__stdcall *)(void *, A...);
    return (*reinterpret_cast<F **>(object))[slot / 4](object, args...);
}
template <class T> T &Field(void *object, size_t offset) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset); }

// ---------------------------------------------------------------- the objects

constexpr int kMats = 6, kGroups = 10, kLevels = 10, kMeshes = 5;
uint8_t *g_oMat[kMats];
I3D_material *g_pMat[kMats];
uint8_t *g_oGroup[kGroups];                 // face groups in no level
I3D_face_group *g_pGroup[kGroups];
uint8_t *g_oLevel[kLevels];                 // the test's references
I3D_mesh_level *g_pLevel[kLevels];
uint8_t *g_oMesh[kMeshes];
I3D_mesh *g_pMesh[kMeshes];
S_VBAlloc g_destAlloc[2];                   // a render buffer outside the levels
S_matrix g_matrix;

int MatIndex(const void *m, int side)
{
    if (!m)
        return -1;
    for (int k = 0; k < kMats; ++k) {
        if (m == (side ? static_cast<const void *>(g_pMat[k]) : g_oMat[k]))
            return k;
    }
    return -2;
}

// Every object reachable on one side, numbered in a fixed walk (slot levels, standalone groups,
// meshes' own levels, each level's groups): allocation records are known by (object, field).
struct Walk {
    std::map<const void *, int> ids;
    int next = 0;
    void Group(const uint8_t *g)
    {
        if (!g || ids.count(g))
            return;
        ids[g] = next;
        ids[g + 0x10] = next * 4 + 0x10000;     // its faces
        ++next;
    }
    void Level(const uint8_t *l)
    {
        if (!l || ids.count(l))
            return;
        ids[l] = next;
        ids[l + 0x34] = next * 4 + 0x10001;     // vertices
        ids[l + 0x40] = next * 4 + 0x10002;     // render vertices
        ++next;
        for (const uint8_t *g = Field<uint8_t *>(const_cast<uint8_t *>(l), 0x5C); g;
             g = Field<uint8_t *>(const_cast<uint8_t *>(g), 0xC))
            Group(g);
    }
};
Walk g_walk[2];

void BuildWalk(int side)
{
    Walk &w = g_walk[side];
    w = Walk{};
    for (int k = 0; k < kLevels; ++k)
        w.Level(side ? reinterpret_cast<uint8_t *>(g_pLevel[k]) : g_oLevel[k]);
    for (int k = 0; k < kGroups; ++k)
        w.Group(side ? reinterpret_cast<uint8_t *>(g_pGroup[k]) : g_oGroup[k]);
    for (int k = 0; k < kMeshes; ++k) {
        uint8_t *m = side ? reinterpret_cast<uint8_t *>(g_pMesh[k]) : g_oMesh[k];
        if (!m)
            continue;
        for (int i = 0; i < 10; ++i)
            w.Level(Field<uint8_t *>(m, 0x48 + 4 * i));
    }
    w.ids[&g_destAlloc[side]] = 0x20000;
}

int OwnerIndex(const void *owner, int side, bool)
{
    if (!owner)
        return -1;
    auto it = g_walk[side].ids.find(owner);
    return it == g_walk[side].ids.end() ? -2 : it->second;
}

// ---------------------------------------------------------------- comparing

void CheckBits(const std::string &what, const void *a, const void *b, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t x, y;
        std::memcpy(&x, static_cast<const uint8_t *>(a) + i, 4);
        std::memcpy(&y, static_cast<const uint8_t *>(b) + i, 4);
        Check(what + " +" + std::to_string(i), static_cast<long>(x), static_cast<long>(y));
    }
}

// The bytes of an allocation in each side's fake buffer.
const uint8_t *VertexData(const S_VBAlloc &a, int side, size_t bytes)
{
    if (!a.buffer)
        return nullptr;
    const int k = FakeIndex(a.vb, g_vbs[side]);
    if (k < 0)
        Fail("vertex data's buffer", k, k);
    const std::vector<uint8_t> &d = g_vbs[side][static_cast<size_t>(k)]->data;
    const size_t at = static_cast<size_t>(a.offset) * a.buffer->stride;
    if (at + bytes > d.size())
        Fail("vertex data past the buffer", static_cast<long>(at), static_cast<long>(bytes));
    return d.data() + at;
}

void CompareBytes(const std::string &what, const uint8_t *a, const uint8_t *b, size_t n)
{
    Check(what + " present", a != nullptr, b != nullptr);
    if (!a || !n)
        return;
    if (std::memcmp(a, b, n)) {
        size_t at = 0;
        while (a[at] == b[at])
            ++at;
        Fail(what + " byte", static_cast<long>(at), static_cast<long>(at));
    }
}

void CompareGroup(const std::string &what, uint8_t *o, I3D_face_group *p)
{
    Check(what, o != nullptr, p != nullptr);
    if (!o)
        return;
    Check(what + " material", MatIndex(Field<void *>(o, 4), 0), MatIndex(p->material, 1));
    Check(what + " numFaces", Field<int>(o, 8), p->numFaces);
    const auto &oa = Field<S_IBAlloc>(o, 0x10);
    CompareAlloc(what + " faces", oa, p->faces);
    if (oa.buffer && p->numFaces > 0) {
        const size_t n = static_cast<size_t>(p->numFaces) * 6;
        const int ko = FakeIndex(oa.ib, g_ibs[0]), kp = FakeIndex(p->faces.ib, g_ibs[1]);
        CompareBytes(what + " face data", g_ibs[0][static_cast<size_t>(ko)]->data.data() + oa.offset * 2,
                     g_ibs[1][static_cast<size_t>(kp)]->data.data() + p->faces.offset * 2, n);
    }
}

uint32_t RenderStride(const S_VBAlloc &a) { return a.buffer ? a.buffer->stride : 0; }

void CompareLevel(const std::string &what, uint8_t *o, I3D_mesh_level *p)
{
    Check(what, o != nullptr, p != nullptr);
    if (!o)
        return;
    CheckBits(what + " box", o + 0x10, &p->bboxMin, 0x20);
    Check(what + " refCount", Field<int>(o, 0x30), p->refCount);
    const auto &ov = Field<S_VBAlloc>(o, 0x34), &orv = Field<S_VBAlloc>(o, 0x40);
    CompareAlloc(what + " vertices", ov, p->vertices);
    CompareAlloc(what + " render vertices", orv, p->renderVertices);
    Check(what + " extraFormat", Field<uint32_t>(o, 0x50), p->extraFormat);
    Check(what + " numVertices", Field<int>(o, 0x54), p->numVertices);
    Check(what + " numFaceGroups", Field<int>(o, 0x58), p->numFaceGroups);
    CheckBits(what + " lodDistance", o + 0x60, &p->lodDistance, 4);
    Check(what + " flags", Field<uint32_t>(o, 0x64), p->flags);
    CheckBits(what + " envScale", o + 0x68, &p->envScale, 4);
    const size_t n = static_cast<size_t>(p->numVertices);
    CompareBytes(what + " vertex data", VertexData(ov, 0, n * 0x20), VertexData(p->vertices, 1, n * 0x20), n * 0x20);
    const size_t rs = RenderStride(p->renderVertices);
    CompareBytes(what + " render data", VertexData(orv, 0, n * rs), VertexData(p->renderVertices, 1, n * rs), n * rs);
    Check(what + " extra", Field<void *>(o, 0x4C) != nullptr, p->extraData != nullptr);
    if (p->extraData)
        CompareBytes(what + " extra data", Field<uint8_t *>(o, 0x4C), p->extraData,
                     n * static_cast<size_t>(p->ExtraStride()));
    uint8_t *og = Field<uint8_t *>(o, 0x5C);
    I3D_face_group *pg = p->faceGroups;
    for (int i = 0; og || pg; ++i) {
        CompareGroup(what + " group " + std::to_string(i), og, pg);
        og = Field<uint8_t *>(og, 0xC);
        pg = pg->next;
    }
}

// A mesh's level: the same slot of the test's, or a level of its own compared by content.
int LevelSlot(const void *l, int side)
{
    for (int k = 0; k < kLevels; ++k) {
        if (l && l == (side ? static_cast<const void *>(g_pLevel[k]) : g_oLevel[k]))
            return k;
    }
    return -1;
}

void CompareMesh(const std::string &what, uint8_t *o, I3D_mesh *p)
{
    Check(what, o != nullptr, p != nullptr);
    if (!o)
        return;
    CheckBits(what + " box and sphere", o + 0x10, &p->bboxMin, 0x30);
    Check(what + " refCount", Field<int>(o, 0x40), p->refCount);
    Check(what + " numLevels", Field<uint32_t>(o, 0x44), p->numLevels);
    for (int i = 0; i < 10; ++i) {
        uint8_t *ol = Field<uint8_t *>(o, 0x48 + 4 * i);
        I3D_mesh_level *pl = p->levels[i];
        const int ks = LevelSlot(ol, 0);
        Check(what + " level slot", ks, LevelSlot(pl, 1));
        if (ks < 0)
            CompareLevel(what + " level " + std::to_string(i), ol, pl);
    }
}

void CompareAll()
{
    BuildWalk(0);
    BuildWalk(1);
    CompareManagers();
    for (int k = 0; k < kMats; ++k) {
        Check("material refCount", Field<int>(g_oMat[k], 4), g_pMat[k]->refCount);
        Check("material flags", Field<uint32_t>(g_oMat[k], 8), g_pMat[k]->matFlags);
    }
    for (int k = 0; k < kGroups; ++k)
        CompareGroup("group " + std::to_string(k), g_oGroup[k], g_pGroup[k]);
    for (int k = 0; k < kLevels; ++k)
        CompareLevel("level " + std::to_string(k), g_oLevel[k], g_pLevel[k]);
    for (int k = 0; k < kMeshes; ++k)
        CompareMesh("mesh " + std::to_string(k), g_oMesh[k], g_pMesh[k]);
    CompareAlloc("dest alloc", g_destAlloc[0], g_destAlloc[1]);
    CompareLogs();
}

// ---------------------------------------------------------------- data

std::vector<uint8_t> RandVertices(int n)
{
    std::vector<uint8_t> v(static_cast<size_t>(n) * 0x20);
    for (size_t i = 0; i < v.size(); i += 4) {
        float f = Rand(50) == 0 ? (Rand(2) ? -0.0f : std::nanf("")) : (static_cast<float>(Rand(20001)) - 10000.0f) / 100.0f;
        std::memcpy(&v[i], &f, 4);
    }
    return v;
}

std::vector<uint8_t> RandFaces(int n)
{
    std::vector<uint8_t> v(static_cast<size_t>(n) * 6);
    for (uint8_t &b : v)
        b = static_cast<uint8_t>(g_rng());
    return v;
}

int RandCount(int most) { return Rand(8) == 0 ? 1 + Rand(3) : 1 + Rand(most); }

// A face group of either side: standalone (k < kGroups) or the n-th of a level.
struct GroupPair {
    uint8_t *o = nullptr;
    I3D_face_group *p = nullptr;
};
GroupPair PickGroup()
{
    if (Rand(2)) {
        const int k = Rand(kGroups);
        return {g_oGroup[k], g_pGroup[k]};
    }
    const int l = Rand(kLevels);
    if (!g_pLevel[l] || !g_pLevel[l]->numFaceGroups)
        return {};
    const int i = Rand(g_pLevel[l]->numFaceGroups);
    return {CallV<uint8_t *>(g_oLevel[l], 0x14, i), g_pLevel[l]->GetFaceGroup(i)};
}

bool InLevel(const I3D_face_group *g)
{
    for (I3D_mesh_level *l : g_pLevel) {
        for (I3D_face_group *x = l ? l->faceGroups : nullptr; x; x = x->next) {
            if (x == g)
                return true;
        }
    }
    for (I3D_mesh *m : g_pMesh) {
        for (int i = 0; m && i < 10; ++i) {
            for (I3D_face_group *x = m->levels[i] ? m->levels[i]->faceGroups : nullptr; x; x = x->next) {
                if (x == g)
                    return true;
            }
        }
    }
    return false;
}

void RandMatrix()
{
    for (auto &row : g_matrix.m)
        for (float &f : row)
            f = (static_cast<float>(Rand(2001)) - 1000.0f) / 1000.0f;
}

void DestroyOrigGroup(uint8_t *g) { OrigFn<void *(__fastcall *)(void *, void *, int)>(0x10030e20)(g, nullptr, 1); }

long g_seenEnv, g_seenRender, g_seenAnonymous, g_seenDuplicated;

// The constructors leave the boxes' w unset (and the mesh's box keeps it): the port's copies the
// original's.
void Birth(uint8_t *o, void *p)
{
    std::memcpy(static_cast<uint8_t *>(p) + 0x1C, o + 0x1C, 4);
    std::memcpy(static_cast<uint8_t *>(p) + 0x2C, o + 0x2C, 4);
}

// ---------------------------------------------------------------- operations

// New extra data for the level's vertex count (the array keeps the count it was made for: a
// later Duplicate would read past it).
void RefreshExtra(uint8_t *o, I3D_mesh_level *p, uint32_t format)
{
    void *ro = CallV<void *>(o, 0x3C, format);
    void *rp = p->SetExtraFormat(format);
    Check("SetExtraFormat", ro != nullptr, rp != nullptr);
    if (ro && rp) {
        const size_t n = static_cast<size_t>(p->numVertices * p->ExtraStride());
        for (size_t i = 0; i < n; ++i)
            static_cast<uint8_t *>(ro)[i] = static_cast<uint8_t *>(rp)[i] = static_cast<uint8_t>(g_rng());
    }
}

void LevelOp(int k)
{
    uint8_t *&o = g_oLevel[k];
    I3D_mesh_level *&p = g_pLevel[k];
    if (!o) {
        g_lastOp = "level create";
        o = CallV<uint8_t *>(g_origDriver, 0x68);
        p = g_driver->CreateMeshLevel();
        Birth(o, p);
        return;
    }
    switch (Rand(20)) {
    case 0: {
        if (Rand(3))
            break;
        g_lastOp = "level Release";
        Check("Release", static_cast<long>(CallV<uint32_t>(o, 0x00)), static_cast<long>(p->Release()));
        o = nullptr;
        p = nullptr;
        break;
    }
    case 1:
    case 2: {
        g_lastOp = "level SetVertices";
        const int n = RandCount(400);
        const std::vector<uint8_t> v = RandVertices(n);
        const bool none = Rand(10) == 0;
        Check("SetVertices", CallV<I3D_RESULT>(o, 0x24, none ? nullptr : v.data(), n),
              p->SetVertices(none ? nullptr : v.data(), n));
        if (p->extraFormat)
            RefreshExtra(o, p, p->extraFormat);
        break;
    }
    case 3: {
        g_lastOp = "level AllocVertices";
        const int n = RandCount(300);
        void *ro = CallV<void *>(o, 0x28, n);
        void *rp = p->AllocVertices(n);
        Check("AllocVertices", LockedAt(ro, g_vbs[0]), LockedAt(rp, g_vbs[1]));
        if (ro && rp) {
            const std::vector<uint8_t> v = RandVertices(n);
            std::memcpy(ro, v.data(), v.size());
            std::memcpy(rp, v.data(), v.size());
            if (Rand(2)) {
                CallV<void>(o, 0x2C);
                p->ComputeBounds();
            }
        }
        if (p->extraFormat)
            RefreshExtra(o, p, p->extraFormat);
        break;
    }
    case 4: {
        g_lastOp = "level LockVertices";
        const uint32_t f = static_cast<uint32_t>(Rand(2));
        void *ro = CallV<void *>(o, 0x04, f);
        void *rp = p->LockVertices(f);
        Check("LockVertices", LockedAt(ro, g_vbs[0]), LockedAt(rp, g_vbs[1]));
        if (ro && rp && !f && p->numVertices) {
            const int i = Rand(p->numVertices);
            const std::vector<uint8_t> v = RandVertices(1);
            std::memcpy(static_cast<uint8_t *>(ro) + 0x20 * i, v.data(), 0x20);
            std::memcpy(static_cast<uint8_t *>(rp) + 0x20 * i, v.data(), 0x20);
        }
        break;
    }
    case 5:
        g_lastOp = "level UnlockVertices";
        CallV<void>(o, 0x08);
        p->UnlockVertices();
        break;
    case 6:
        g_lastOp = "level ComputeBounds";
        CallV<void>(o, 0x2C);
        p->ComputeBounds();
        break;
    case 7: {
        g_lastOp = "level Duplicate";
        const int s = Rand(kLevels + 1);
        uint8_t *so = s < kLevels ? g_oLevel[s] : nullptr;
        I3D_mesh_level *sp = s < kLevels ? g_pLevel[s] : nullptr;
        if (sp && sp != p && !sp->faceGroups && p->faceGroups)
            break;                          // (the original keeps its deleted list)
        Check("Duplicate", CallV<I3D_RESULT>(o, 0x0C, so), p->Duplicate(sp));
        ++g_seenDuplicated;
        break;
    }
    case 8: {
        g_lastOp = "level queries";
        Check("NumFaces", CallV<int>(o, 0x10), p->NumFaces());
        S_meshStats so{}, sp{};
        Check("GetStats", CallV<I3D_RESULT>(o, 0x18, &so), p->GetStats(&sp));
        CheckBits("stats", &so, &sp, sizeof(so));
        const int i = Rand(p->numFaceGroups + 2) - 1;
        uint8_t *go = CallV<uint8_t *>(o, 0x14, i);
        I3D_face_group *gp = p->GetFaceGroup(i);
        BuildWalk(0);
        BuildWalk(1);
        Check("GetFaceGroup", OwnerIndex(go, 0, false), OwnerIndex(gp, 1, false));
        Check("ExtraStride", CallV<int>(o, 0x40), p->ExtraStride());
        break;
    }
    case 9:
    case 10: {
        // A standalone group with faces joins the level.
        const int g = Rand(kGroups);
        if (!g_pGroup[g] || !g_pGroup[g]->numFaces)
            break;
        g_lastOp = "level AddFaceGroup";
        CallV<void>(o, 0x1C, g_oGroup[g]);
        p->AddFaceGroup(g_pGroup[g]);
        g_oGroup[g] = nullptr;
        g_pGroup[g] = nullptr;
        g_seenEnv += (p->flags & MLF_ENV) != 0;
        break;
    }
    case 11: {
        g_lastOp = "level RemoveFaceGroup";
        if (!p->faceGroups || Rand(4) == 0) {
            // Not in the list.
            const int g = Rand(kGroups);
            Check("RemoveFaceGroup (absent)", CallV<I3D_RESULT>(o, 0x20, g_oGroup[g]), p->RemoveFaceGroup(g_pGroup[g]));
            break;
        }
        uint8_t *go = Field<uint8_t *>(o, 0x5C);
        I3D_face_group *gp = p->faceGroups;
        Check("RemoveFaceGroup", CallV<I3D_RESULT>(o, 0x20, go), p->RemoveFaceGroup(gp));
        int free = -1;
        for (int g = 0; g < kGroups; ++g) {
            if (!g_pGroup[g])
                free = g;
        }
        if (free < 0) {
            DestroyOrigGroup(go);
            delete gp;
        } else {
            g_oGroup[free] = go;
            g_pGroup[free] = gp;
        }
        break;
    }
    case 12:
    case 13: {
        if (!p->numVertices)
            break;
        g_lastOp = "level CreateRenderVB";
        static const uint32_t kBits[] = {1, 2, 8, 0x10, 0x20};
        uint32_t f = 0;
        for (uint32_t b : kBits)
            f |= Rand(3) == 0 ? b : 0;
        RandMatrix();
        const bool dest = Rand(4) == 0;
        if (dest) {
            OrigFn<void(__stdcall *)(S_VBAlloc *)>(0x10066e90)(&g_destAlloc[0]);
            FreeVB(&g_destAlloc[1]);
            g_destAlloc[0] = g_destAlloc[1] = S_VBAlloc{};
        } else {
            CallV<void>(o, 0x34);
            p->FreeRenderVB();
        }
        Check("CreateRenderVB", CallV<bool>(o, 0x30, f, &g_matrix, dest ? &g_destAlloc[0] : nullptr),
              p->CreateRenderVB(f, &g_matrix, dest ? &g_destAlloc[1] : nullptr));
        ++g_seenRender;
        break;
    }
    case 14: {
        g_lastOp = "level FillRenderVB";
        const bool deferred = Rand(4) == 0;
        const bool dest = Rand(4) == 0;
        // (The destination outside the level must fit this level's vertices.)
        if (dest && (!g_destAlloc[1].buffer || static_cast<int>(g_destAlloc[1].buffer->capacity - g_destAlloc[1].offset) < p->numVertices))
            break;
        if (!p->vertices.buffer && (dest || p->renderVertices.vb) && !deferred)
            break;                          // (no vertices to read: the original reads through null)
        RandMatrix();
        CallV<void>(o, 0x38, deferred ? 1 : 0, &g_matrix, dest ? &g_destAlloc[0] : nullptr);
        p->FillRenderVB(deferred, &g_matrix, dest ? &g_destAlloc[1] : nullptr);
        break;
    }
    case 15:
        g_lastOp = "level FreeRenderVB";
        CallV<void>(o, 0x34);
        p->FreeRenderVB();
        break;
    case 16: {
        g_lastOp = "level SetExtraFormat";
        static const uint32_t kFormats[] = {0, 1, 2, 4, 8, 3, 5, 9, 0xF};
        RefreshExtra(o, p, kFormats[Rand(9)]);
        break;
    }
    default: {
        // Face groups of the level.
        GroupPair g;
        if (p->numFaceGroups) {
            const int i = Rand(p->numFaceGroups);
            g = {CallV<uint8_t *>(o, 0x14, i), p->GetFaceGroup(i)};
        }
        if (!g.p)
            break;
        g_lastOp = "level group DuplicateMaterial";
        Check("DuplicateMaterial", CallV<I3D_RESULT>(g.o, 0x18), g.p->DuplicateMaterial());
        break;
    }
    }
}

void GroupOp()
{
    const int k = Rand(kGroups);
    if (!g_pGroup[k] && Rand(2)) {
        g_lastOp = "group create";
        g_oGroup[k] = CallV<uint8_t *>(g_origDriver, 0x64);
        g_pGroup[k] = g_driver->CreateFaceGroup();
        return;
    }
    GroupPair g = Rand(2) ? GroupPair{g_oGroup[k], g_pGroup[k]} : PickGroup();
    if (!g.p)
        return;
    const bool standalone = !InLevel(g.p);
    switch (Rand(9)) {
    case 0: {
        if (!standalone || Rand(2))
            break;
        g_lastOp = "group destroy";
        DestroyOrigGroup(g.o);
        delete g.p;
        for (int i = 0; i < kGroups; ++i) {
            if (g_pGroup[i] == g.p) {
                g_oGroup[i] = nullptr;
                g_pGroup[i] = nullptr;
            }
        }
        break;
    }
    case 1: {
        g_lastOp = "group SetMaterial";
        const int m = Rand(kMats + 1) - 1;
        CallV<void>(g.o, 0x00, m < 0 ? nullptr : g_oMat[m]);
        g.p->SetMaterial(m < 0 ? nullptr : g_pMat[m]);
        break;
    }
    case 2: {
        g_lastOp = "group SetFaces";
        const bool none = standalone && Rand(8) == 0;
        const int n = RandCount(300);
        const std::vector<uint8_t> f = RandFaces(n);
        const void *src = none ? nullptr : f.data();
        Check("SetFaces", LockedAt(CallV<void *>(g.o, 0x04, src, n), g_ibs[0]),
              LockedAt(g.p->SetFaces(static_cast<const I3D_triface *>(src), n), g_ibs[1]));
        break;
    }
    case 3: {
        g_lastOp = "group AllocFaces";
        const int n = RandCount(300);
        void *ro = CallV<void *>(g.o, 0x08, n);
        void *rp = g.p->AllocFaces(n);
        Check("AllocFaces", LockedAt(ro, g_ibs[0]), LockedAt(rp, g_ibs[1]));
        const std::vector<uint8_t> f = RandFaces(n);
        std::memcpy(ro, f.data(), f.size());
        std::memcpy(rp, f.data(), f.size());
        break;
    }
    case 4: {
        g_lastOp = "group Lock";
        const uint32_t fl = static_cast<uint32_t>(Rand(2));
        void *ro = CallV<void *>(g.o, 0x0C, fl);
        void *rp = g.p->Lock(fl);
        Check("Lock", LockedAt(ro, g_ibs[0]), LockedAt(rp, g_ibs[1]));
        if (ro && !fl) {
            const int i = Rand(g.p->numFaces);
            const std::vector<uint8_t> f = RandFaces(1);
            std::memcpy(static_cast<uint8_t *>(ro) + 6 * i, f.data(), 6);
            std::memcpy(static_cast<uint8_t *>(rp) + 6 * i, f.data(), 6);
        }
        break;
    }
    case 5:
        if (!g.p->faces.buffer)
            break;
        g_lastOp = "group Unlock";
        CallV<void>(g.o, 0x10);
        g.p->Unlock();
        break;
    case 6: {
        GroupPair s = PickGroup();
        if (!s.p || !s.p->numFaces)
            break;
        g_lastOp = "group Duplicate";
        Check("Duplicate", CallV<I3D_RESULT>(g.o, 0x14, s.o), g.p->Duplicate(s.p));
        break;
    }
    default:
        g_lastOp = "group DuplicateMaterial";
        Check("DuplicateMaterial", CallV<I3D_RESULT>(g.o, 0x18), g.p->DuplicateMaterial());
        break;
    }
}

void MeshOp(int k)
{
    uint8_t *&o = g_oMesh[k];
    I3D_mesh *&p = g_pMesh[k];
    if (!o) {
        g_lastOp = "mesh create";
        o = CallV<uint8_t *>(g_origDriver, 0x6C);
        p = g_driver->CreateMesh();
        Birth(o, p);
        return;
    }
    switch (Rand(9)) {
    case 0: {
        if (Rand(3))
            break;
        bool beyond = false;
        for (int i = static_cast<int>(p->numLevels); i < 10; ++i)
            beyond |= p->levels[i] != nullptr;
        if (beyond && p->refCount == 1)
            break;                          // (the original leaks those levels; mesh.cpp releases them)
        g_lastOp = "mesh Release";
        Check("Release", static_cast<long>(CallV<uint32_t>(o, 0x00)), static_cast<long>(p->Release()));
        o = nullptr;
        p = nullptr;
        break;
    }
    case 1:
    case 2: {
        g_lastOp = "mesh SetLevel";
        const int l = Rand(kLevels + 2) - 2;
        const int i = Rand(11);
        uint8_t *lo = l >= 0 ? g_oLevel[l] : nullptr;
        I3D_mesh_level *lp = l >= 0 ? g_pLevel[l] : nullptr;
        Check("SetLevel", CallV<I3D_RESULT>(o, 0x08, lo, i), p->SetLevel(lp, i));
        break;
    }
    case 3: {
        g_lastOp = "mesh GetLevel";
        const int i = Rand(10);
        Check("GetLevel", LevelSlot(CallV<void *>(o, 0x04, i), 0), LevelSlot(p->GetLevel(i), 1));
        break;
    }
    case 4: {
        g_lastOp = "mesh Duplicate";
        const int s = Rand(kMeshes + 1);
        uint8_t *so = s < kMeshes ? g_oMesh[s] : nullptr;
        I3D_mesh *sp = s < kMeshes ? g_pMesh[s] : nullptr;
        Check("Duplicate", CallV<I3D_RESULT>(o, 0x0C, so), p->Duplicate(sp));
        for (int i = 0; i < 10; ++i)
            g_seenAnonymous += p->levels[i] && LevelSlot(p->levels[i], 1) < 0;
        break;
    }
    case 5: {
        bool gap = false;
        for (uint32_t i = 0; i < p->numLevels; ++i)
            gap |= !p->levels[i];
        if (gap)
            break;                          // (the original reads through the empty level)
        g_lastOp = "mesh GetStats";
        S_meshStats so{}, sp{};
        Check("GetStats", CallV<I3D_RESULT>(o, 0x10, &so), p->GetStats(&sp));
        CheckBits("stats", &so, &sp, sizeof(so));
        break;
    }
    case 6:
        g_lastOp = "mesh ComputeBounds";
        CallV<void>(o, 0x14);
        p->ComputeBounds();
        break;
    case 7:
        g_lastOp = "mesh DuplicateMaterials";
        Check("DuplicateMaterials", CallV<I3D_RESULT>(o, 0x18), p->DuplicateMaterials());
        break;
    default: {
        const int s = Rand(kMeshes);
        if (!g_pMesh[s])
            break;
        g_lastOp = "mesh SameLayout";
        Check("SameLayout", CallV<bool>(o, 0x1C, g_oMesh[s]), p->SameLayout(g_pMesh[s]));
        break;
    }
    }
}

void RunOp()
{
    const int r = Rand(20);
    if (r < 9) {
        LevelOp(Rand(kLevels));
    } else if (r < 14) {
        GroupOp();
    } else if (r < 18) {
        MeshOp(Rand(kMeshes));
    } else if (r == 18) {
        g_lastOp = "UnlockAll";
        OrigFn<void(__fastcall *)(void *, void *)>(0x100587f0)(&Orig<uint8_t>(0x100c05f0), nullptr);
        g_ibManager.UnlockAll();
        OrigFn<void(__fastcall *)(void *, void *)>(0x10066eb0)(&Orig<uint8_t>(0x100d1400), nullptr);
        g_vbManager.UnlockAll();
    } else {
        // A material's environment flags, or the driver's coordinate scale.
        if (Rand(2)) {
            g_lastOp = "material flags";
            const int m = Rand(kMats);
            uint32_t f = (Field<uint32_t>(g_oMat[m], 8) & ~0x7f07fu) | static_cast<uint32_t>(Rand(Rand(8) ? 127 : 3) + 1) % 128;
            f = (f & ~(7u << 12)) | static_cast<uint32_t>(Rand(8)) << 12;
            f = Rand(3) ? f | MATF_ENV_TEXTURE : f & ~MATF_ENV_TEXTURE;
            Field<uint32_t>(g_oMat[m], 8) = f;
            g_pMat[m]->matFlags = f;
        } else {
            g_lastOp = "env scale";
            const float s = static_cast<float>(Rand(400)) / 100.0f - 1.0f;
            uint32_t bits;
            std::memcpy(&bits, &s, 4);
            Orig<uint32_t>(0x100be058 + 4 * DRVSTATE_ENV_UV_SCALE) = bits;
            g_driverStates[DRVSTATE_ENV_UV_SCALE] = bits;
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 200000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    if (!SetUpBuffers(origPath))
        return 2;
    g_logDisabled = true;
    _controlfp(_PC_24, _MCW_PC);
    g_rng.seed(seed);
    g_ownerIndex = &OwnerIndex;
    g_origDriver = Orig<uint8_t *>(0x100d16f8);
    g_driver = new I3D_driver;
    for (int k = 0; k < kMats; ++k) {
        const bool anim = k % 3 == 2;
        g_oMat[k] = CallV<uint8_t *>(g_origDriver, anim ? 0x5C : 0x58);
        g_pMat[k] = anim ? g_driver->CreateAnimMaterial() : g_driver->CreateMaterial();
    }

    CompareAll();
    for (g_op = 0; g_op < ops; ++g_op) {
        RunOp();
        CompareAll();
        if (g_op % 2000 == 1999)
            CompareBufferContents();
    }
    CompareBufferContents();
    std::printf("coverage: levels with environment flags %ld, render buffers %ld, duplicated levels %ld, mesh-made "
                "levels %ld\n", g_seenEnv, g_seenRender, g_seenDuplicated, g_seenAnonymous);
    std::printf("compare_mesh: %ld operations, seed %u: identical\n", ops, seed);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
