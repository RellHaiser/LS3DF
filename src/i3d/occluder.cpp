// I3D_occluder (0x10038490 - 0x1003ab60; 0x1003aa80 is the static initialiser of this file's copy
// of a header's zero vector).
#include "i3d/occluder.h"

#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <vector>

#include "external/rw_data.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/loader.h"
#include "i3d/portal.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/visual.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

const S_vector &AsVector(const S_vector4 &v) { return reinterpret_cast<const S_vector &>(v); }

// A plane's distance of `p`, summed z, y, x (the products in either order) then d.
float DistanceZYX(const S_plane &plane, const S_vector &p)
{
    return ((plane.normal.z * p.z + plane.normal.y * p.y) + plane.normal.x * p.x) + plane.d;
}

// A sphere hidden by an active occluder (0x1003a280 / 0x1003a430): inside every silhouette plane
// and, unless it lies beyond the near plane by more than its radius, behind every front plane.
bool HidesSphere(const I3D_occluder &q, const S_vector &center, float radius)
{
    if (!(DistanceZYX(q.nearPlane, center) > radius)) {
        for (int i = 0; i < q.numFrontPlanes; ++i) {
            if (DistanceZYX(q.frontPlanes[i], center) < radius)
                return false;
        }
    }
    for (int i = 0; i < q.numSilhouettePlanes; ++i) {
        if (DistanceZYX(q.silhouettePlanes[i], center) < radius)
            return false;
    }
    return true;
}

// 0x1003a280: from the nearest active occluder outwards, each hides the farther ones inside its
// volume.
void OccludeOccluders(I3D_sector *sector)
{
    float lastDepth = 0.0f;
    for (;;) {
        I3D_occluder *nearest = nullptr;
        float best = 1e12f;
        for (I3D_occluder *q = sector->occluders; q; q = q->nextInSector) {
            if ((q->occluderFlags & OCCF_ACTIVE) && q->viewDepth > lastDepth && best > q->viewDepth) {
                nearest = q;
                best = q->viewDepth;
            }
        }
        if (!nearest)
            return;
        for (I3D_occluder *q = sector->occluders; q; q = q->nextInSector) {
            const uint32_t flags = q->occluderFlags;
            if ((flags & OCCF_ACTIVE) && q->viewDepth > best && HidesSphere(*nearest, q->center, q->radius))
                q->occluderFlags = flags & ~OCCF_ACTIVE;
        }
        lastDepth = best;
    }
}

// 0x1003a430: the visuals of the sector's render list that an active occluder hides lose this
// frame's stamp (as do those without it).
void CullVisuals(I3D_sector *sector)
{
    if (!sector->occluders)
        return;
    for (I3D_visual *v = sector->renderVisuals; v; v = v->nextRenderVisual) {
        if (v->renderStamp == g_renderStamp) {
            if (!(v->visualFlags & VISF_WBOUND_VALID))
                v->UpdateWBoundProc();
            bool hidden = false;
            for (I3D_occluder *q = sector->occluders; q && !hidden; q = q->nextInSector) {
                if (q->occluderFlags & OCCF_ACTIVE)
                    hidden = HidesSphere(*q, v->worldBound.center, v->worldBound.radius);
            }
            if (!hidden)
                continue;
        }
        v->renderStamp = g_renderStamp - 1;
    }
}

// The occluders made by a sector on the renderer's portal path back to the camera are not used.
void DropOccludersOnPath(I3D_sector *sector)
{
    for (I3D_occluder *q = sector->occluders; q; q = q->nextInSector) {
        if (q->ownerSector && (q->ownerSector->sectorFlags & SCTF_ON_PATH))
            q->occluderFlags &= ~OCCF_ACTIVE;
    }
}

// RebuildOccluderLists' recursion (0x10039e70): the occluders below `frame`, not looking into
// cameras and sectors, appended to s_collectSector's list (0x100be680; the last: 0x100be6a4).
I3D_sector *s_collectSector;
I3D_occluder *s_lastCollected;

void CollectOccluders(I3D_frame *frame)
{
    for (I3D_frame *c = frame->firstChild; c; c = c->nextSibling) {
        if (c->type == FRAME_CAMERA || c->type == FRAME_SECTOR)
            continue;
        if (c->type == FRAME_OCCLUDER) {
            auto *occ = static_cast<I3D_occluder *>(c);
            if (!s_collectSector->occluders)
                s_collectSector->occluders = occ;
            else
                s_lastCollected->nextInSector = occ;
            s_lastCollected = occ;
            occ->nextInSector = nullptr;
        }
        CollectOccluders(c);
    }
}

}  // namespace

// ---------------------------------------------------------------- the frame slots

void I3D_occluder::Init()
{
    I3D_frame::Init();
    frameFlags |= 0x08100000;
    numSilhouettePlanes = 0;
    numFrontPlanes = 0;
    occluderFlags = 0;
    nextInScene = nullptr;
    prevInScene = nullptr;
    numVertices = 0;
    numFaces = 0;
    nextInSector = nullptr;
    ownerSector = nullptr;
    faceFlags = nullptr;
    worldVertices = nullptr;
    vertices = nullptr;
    faces = nullptr;
    neighborEdges = nullptr;
    neighbors = nullptr;
    facePlanes = nullptr;
    scene = nullptr;
    center = S_vector(0.0f, 0.0f, 0.0f);
    centerW = 0.0f;
    radius = 0.0f;
    radiusSquared = 0.0f;
    viewDepth = 0.0f;
}

void I3D_occluder::Uninit()
{
    // (The pointers stay until Init.)
    ::operator delete(faceFlags);
    AlignedFree(worldVertices);
    AlignedFree(vertices);
    ::operator delete(faces);
    ::operator delete(neighborEdges);
    ::operator delete(neighbors);
    AlignedFree(facePlanes);
    if (ownerSector) {
        ownerSector->occluder = nullptr;
        ownerSector = nullptr;
    }
    SetScene(nullptr);
    I3D_frame::Uninit();
}

I3D_RESULT I3D_occluder::LinkTo(I3D_frame *newParent, uint32_t flags)
{
    if (newParent == parent)
        return I3D_OK;
    SetScene(nullptr);
    if (newParent) {
        I3D_frame *owner = newParent->ownerModel;
        I3D_scene *newScene;
        if (owner && owner->GetType() == FRAME_SCENE) {
            newScene = static_cast<I3D_scene *>(owner);
        } else {
            I3D_frame *f = newParent;
            while (f->GetType() != FRAME_SECTOR) {
                f = f->parent;
                if (!f)
                    return I3D_frame::LinkTo(newParent, flags);
            }
            newScene = static_cast<I3D_sector *>(f)->scene;
        }
        if (newScene)
            SetScene(newScene);
    }
    return I3D_frame::LinkTo(newParent, flags);
}

I3D_RESULT I3D_occluder::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    const I3D_RESULT result = I3D_frame::Duplicate(source);
    if (source->GetType() == FRAME_OCCLUDER) {
        const auto *src = static_cast<const I3D_occluder *>(source);
        Build(src->vertices, src->numVertices, src->faces, src->numFaces, sizeof(S_vector4));
    }
    return result;
}

void I3D_occluder::MatrixChanged()
{
    if (!numVertices)
        return;
    const S_matrix &m = GetMatrix();
    for (int i = 0; i < numVertices; ++i) {
        // (The original copies 16 bytes of the 12-byte product: the fourth is stack garbage.)
        const S_vector w = AsVector(vertices[i]) * m;
        worldVertices[i].x = w.x;
        worldVertices[i].y = w.y;
        worldVertices[i].z = w.z;
    }
    for (int k = 0; k < numFaces; ++k) {
        const I3D_triface &f = faces[k];
        const S_vector &a = AsVector(worldVertices[f.v[0]]);
        const S_vector &b = AsVector(worldVertices[f.v[1]]);
        const S_vector &c = AsVector(worldVertices[f.v[2]]);
        S_vector n = (c - b).Cross(a - b);
        NormalizeWithSquare(n, SquareZXY(n));
        facePlanes[k].d = -n.Dot(a);
        facePlanes[k].normal = n;
    }
    S_vector lo = AsVector(worldVertices[0]);
    S_vector hi = lo;
    for (int i = 1; i < numVertices; ++i) {
        const S_vector &w = AsVector(worldVertices[i]);
        if (w.x < lo.x)
            lo.x = w.x;
        if (w.y < lo.y)
            lo.y = w.y;
        if (w.z < lo.z)
            lo.z = w.z;
        if (hi.x < w.x)
            hi.x = w.x;
        if (hi.y < w.y)
            hi.y = w.y;
        if (hi.z < w.z)
            hi.z = w.z;
    }
    // (centerW: the original stores an unset local.)
    center = S_vector((hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f);
    const float hx = (hi.x - lo.x) * 0.5f, hy = (hi.y - lo.y) * 0.5f, hz = (hi.z - lo.z) * 0.5f;
    radius = std::sqrt((hz * hz + hy * hy) + hx * hx);
    radiusSquared = radius * radius;
}

void I3D_occluder::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &numVertices, 8);           // numVertices, numFaces
    vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVertices, 16));
    worldVertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVertices, 16));
    faces = static_cast<I3D_triface *>(::operator new(sizeof(I3D_triface) * numFaces, std::nothrow));
    if (!vertices || !faces || !worldVertices) {
        AlignedFree(vertices);
        vertices = nullptr;
        numVertices = 0;
        AlignedFree(worldVertices);
        worldVertices = nullptr;
        ::operator delete(faces);
        faces = nullptr;
        numFaces = 0;
        g_loadResult = I3DERR_OUTOFMEM;
        return;
    }
    dtaRead(g_loadFile, vertices, sizeof(S_vector4) * numVertices);
    dtaRead(g_loadFile, faces, sizeof(I3D_triface) * numFaces);
    const I3D_RESULT result = BuildEdges();
    if (result != I3D_OK)
        g_loadResult = result;
}

I3D_RESULT I3D_occluder::Build(const void *verts, int numVerts, const I3D_triface *tris, int numTris, int stride)
{
    // (The original frees its arrays first: duplicating itself read them freed. They go after
    // the copy.)
    S_vector4 *const oldVertices = vertices;
    S_vector4 *const oldWorldVertices = worldVertices;
    I3D_triface *const oldFaces = faces;
    vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVerts, 16));
    worldVertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVerts, 16));
    faces = static_cast<I3D_triface *>(::operator new(sizeof(I3D_triface) * numTris, std::nothrow));
    if (!vertices || !worldVertices || !faces) {
        AlignedFree(oldVertices);
        AlignedFree(oldWorldVertices);
        ::operator delete(oldFaces);
        return I3DERR_OUTOFMEM;
    }
    numVertices = numVerts;
    const auto *src = static_cast<const uint8_t *>(verts);
    for (int i = 0; i < numVerts; ++i, src += stride) {
        // (the fourth dword stays unset)
        const auto *v = reinterpret_cast<const S_vector *>(src);
        vertices[i].x = v->x;
        vertices[i].y = v->y;
        vertices[i].z = v->z;
    }
    numFaces = numTris;
    std::memcpy(faces, tris, sizeof(I3D_triface) * numTris);
    AlignedFree(oldVertices);
    AlignedFree(oldWorldVertices);
    ::operator delete(oldFaces);
    return BuildEdges();
}

I3D_RESULT I3D_occluder::BuildEdges()
{
    // (The original sizes its edge pool with a 16-bit count, and an occluder built again keeps
    // the arrays of its earlier mesh allocated: both fixed.)
    ::operator delete(neighbors);
    ::operator delete(neighborEdges);
    AlignedFree(facePlanes);
    ::operator delete(faceFlags);
    neighbors = static_cast<uint16_t *>(::operator new(sizeof(uint16_t) * 3 * numFaces, std::nothrow));
    neighborEdges = static_cast<uint8_t *>(::operator new(3 * static_cast<size_t>(numFaces), std::nothrow));
    facePlanes = static_cast<S_plane *>(AlignedAlloc(sizeof(S_plane) * numFaces, 16));
    faceFlags = static_cast<uint16_t *>(::operator new(sizeof(uint16_t) * numFaces, std::nothrow));
    if (!neighbors || !neighborEdges || !facePlanes || !faceFlags)
        return I3DERR_OUTOFMEM;

    // Every face edge keyed by its vertex pair (smaller index high); the edges of one key form a
    // ring in the order they come, and each edge's neighbour is the next in its ring (itself for
    // an open edge). (The original keeps the keys in a binary tree, 0x1003aaa0 inserting; the rings
    // are the same.)
    struct Edge {
        uint16_t face, edge;
        int next;
    };
    std::vector<Edge> edges;
    edges.reserve(3 * static_cast<size_t>(numFaces));
    std::map<uint32_t, std::pair<int, int>> rings;      // key -> first, last
    for (int k = 0; k < numFaces; ++k) {
        for (int e = 0; e < 3; ++e) {
            const uint16_t a = faces[k].v[e], b = faces[k].v[(e + 1) % 3];
            const uint32_t key = a < b ? (uint32_t(a) << 16 | b) : (uint32_t(b) << 16 | a);
            const int index = static_cast<int>(edges.size());
            edges.push_back({static_cast<uint16_t>(k), static_cast<uint16_t>(e), index});
            auto it = rings.find(key);
            if (it == rings.end()) {
                rings.emplace(key, std::make_pair(index, index));
            } else {
                edges[it->second.second].next = index;
                edges[index].next = it->second.first;
                it->second.second = index;
            }
        }
    }
    for (const Edge &edge : edges) {
        const Edge &across = edges[edge.next];
        neighbors[edge.face * 3 + edge.edge] = across.face;
        neighborEdges[edge.face * 3 + edge.edge] = static_cast<uint8_t>(across.edge);
    }
    std::memset(faceFlags, 0, sizeof(uint16_t) * numFaces);
    radius = 1.0f;
    return I3D_OK;
}

void I3D_occluder::SetScene(I3D_scene *newScene)
{
    if (scene && prevInScene) {
        if (scene->firstOccluder == this)
            scene->firstOccluder = nextInScene;
        if (prevInScene == this) {
            scene->firstOccluder = nullptr;
        } else {
            prevInScene->nextInScene = nextInScene;
            nextInScene->prevInScene = prevInScene;
        }
        --scene->occluderCount;
        scene->occluderListsStale = true;
        scene = nullptr;
    }
    if (newScene) {
        scene = newScene;
        I3D_occluder *const first = newScene->firstOccluder;
        if (!first) {
            prevInScene = this;
            nextInScene = this;
            newScene->firstOccluder = this;
        } else {
            first->prevInScene->nextInScene = this;
            prevInScene = first->prevInScene;
            nextInScene = first;
            first->prevInScene = this;
        }
        ++newScene->occluderCount;
        newScene->occludersChanged = true;
        newScene->occluderListsStale = true;
    }
}

// ---------------------------------------------------------------- the occlusion volume

void I3D_occluder::UpdateOcclusionVolume()
{
    occluderFlags &= ~(OCCF_ACTIVE | OCCF_STRADDLES_PORTALS);
    if (!numVertices)
        return;
    if (!scene) {
        for (I3D_frame *f = parent; f; f = f->parent) {
            if (f->GetType() == FRAME_SECTOR) {
                if (I3D_scene *s = static_cast<I3D_sector *>(f)->scene)
                    SetScene(s);
                return;
            }
        }
        return;
    }
    if (!(frameFlags & FRMFLAGS_IN_VIEW) || !(frameFlags & FRMFLAGS_ON))
        return;
    if (parent && !(parent->frameFlags & FRMFLAGS_ON))
        return;

    // The faces the camera sees.
    for (int k = 0; k < numFaces; ++k) {
        const S_vector &eye = g_renderCamera->GetMatrix()(3);
        const S_plane &p = facePlanes[k];
        const float d = ((p.normal.z * eye.z + p.normal.y * eye.y) + eye.x * p.normal.x) + p.d;
        if (d > 0.0f)
            faceFlags[k] |= 1;
        else
            faceFlags[k] &= ~1;
    }
    numSilhouettePlanes = 0;
    numFrontPlanes = 0;
    const S_vector &eye = g_renderCamera->GetMatrix()(3);
    S_vector dir = g_renderCamera->GetMatrix()(2);
    NormalizeWithSquare(dir, SquareYZX(dir));
    nearPlane.normal = dir;
    nearPlane.d = 0.0f;
    g_renderCamera->GetMatrix();
    nearPlane.d = -(((eye.z * dir.z + eye.y * dir.y) + eye.x * dir.x) + nearPlane.d);

    // A silhouette edge from `from` to `to`: the plane through it and the camera; the near plane
    // moves out to `to` when that lies beyond it.
    const auto addEdge = [&](const S_vector &from, const S_vector &to, bool squareZYX) {
        S_vector n = (from - to).Cross(eye - to);
        NormalizeWithSquare(n, squareZYX ? SquareZYX(n) : SquareXZY(n));
        S_plane &plane = silhouettePlanes[numSilhouettePlanes++];
        plane.d = -n.Dot(eye);
        plane.normal = n;
        const float depth = ((to.y * nearPlane.normal.y + to.z * nearPlane.normal.z) + to.x * nearPlane.normal.x) +
                            nearPlane.d;
        if (depth > 0.0f)
            nearPlane.d = nearPlane.d - depth;
    };
    const uint16_t *across = neighbors;
    for (int k = 0; k < numFaces; ++k, across += 3) {
        if (faceFlags[k] & 1) {
            const I3D_triface &f = faces[k];
            const S_vector &v0 = AsVector(worldVertices[f.v[0]]);
            const S_vector &v1 = AsVector(worldVertices[f.v[1]]);
            const S_vector &v2 = AsVector(worldVertices[f.v[2]]);
            S_vector n = (v2 - v0).Cross(v1 - v0);
            NormalizeWithSquare(n, SquareXZY(n));
            S_plane &front = frontPlanes[numFrontPlanes++];
            front.d = -n.Dot(v1);
            front.normal = n;
            if (!(faceFlags[across[0]] & 1))
                addEdge(v0, v1, false);
            if (!(faceFlags[across[1]] & 1))
                addEdge(v1, v2, false);
            if (!(faceFlags[across[2]] & 1))
                addEdge(v2, v0, true);
        }
        if (numSilhouettePlanes > 32 || numFrontPlanes > 32)
            return;
    }
    if (numSilhouettePlanes)
        occluderFlags |= OCCF_ACTIVE;
}

// ---------------------------------------------------------------- the renderer's passes

namespace ls3d::i3d {

void RebuildOccluderLists(I3D_scene *scene)
{
    if (!scene->occluderListsStale)
        return;
    I3D_sector *s = scene->firstSector;
    for (int n = scene->sectorCount; n; --n) {
        s_collectSector = s;
        s->occluders = nullptr;
        CollectOccluders(s);
        s = s->nextInScene;
    }
    s_collectSector = scene->primarySector;
    scene->primarySector->occluders = nullptr;
    CollectOccluders(scene->primarySector);
    scene->occluderListsStale = false;
}

void OccludePortals(I3D_sector *sector)
{
    for (I3D_occluder *q = sector->occluders; q; q = q->nextInSector)
        q->UpdateOcclusionVolume();
    const S_vector eye = g_renderCamera->GetMatrix()(3);
    for (I3D_occluder *q = sector->occluders; q; q = q->nextInSector) {
        I3D_sector *const owner = q->ownerSector;
        if (!owner || !owner->portals.Size())
            continue;
        bool front = false, back = false;
        for (I3D_portal *p : owner->portals) {
            if (DistanceZYX(p->plane, eye) < 0.0f)
                back = true;
            else
                front = true;
        }
        if (front && back)
            q->occluderFlags |= OCCF_STRADDLES_PORTALS;
    }
    // A portal is hidden by an occluder when all its vertices are beyond the near plane (by 0.01,
    // compared in double) and inside the silhouette planes. The two kinds sum in their own orders.
    const auto hiddenBy = [](const I3D_occluder &q, const I3D_portal &p, bool incoming) {
        for (int k = 0; k < p.numVertices; ++k) {
            const S_vector &v = AsVector(p.worldVertices[k]);
            const S_plane &n = q.nearPlane;
            const float depth = incoming ? ((v.z * n.normal.z + v.x * n.normal.x) + v.y * n.normal.y) + n.d
                                         : ((n.normal.x * v.x + v.y * n.normal.y) + v.z * n.normal.z) + n.d;
            if (depth < 0.01)
                return false;
            for (int i = 0; i < q.numSilhouettePlanes; ++i) {
                const S_plane &s = q.silhouettePlanes[i];
                const float d = incoming ? DistanceZYX(s, v)
                                         : ((s.normal.y * v.y + s.normal.z * v.z) + s.normal.x * v.x) + s.d;
                if (d < 0.0f)
                    return false;
            }
        }
        return true;
    };
    const auto occlude = [&](I3D_portal *p, bool incoming) {
        for (I3D_occluder *q = sector->occluders; q; q = q->nextInSector) {
            if ((q->occluderFlags & OCCF_ACTIVE) && !(q->occluderFlags & OCCF_STRADDLES_PORTALS) &&
                hiddenBy(*q, *p, incoming)) {
                p->portalFlags |= PRTF_OCCLUDED;
                return;
            }
        }
    };
    for (I3D_portal *p : sector->portals) {
        if (!(DistanceZYX(p->plane, eye) > 0.0f))
            continue;
        p->portalFlags &= ~PRTF_OCCLUDED;
        occlude(p, false);
    }
    for (I3D_portal *p : sector->incomingPortals) {
        if (!(DistanceZYX(p->plane, eye) < 0.0f))
            continue;
        p->portalFlags &= ~PRTF_OCCLUDED;
        if (!p->otherSide)
            occlude(p, true);
    }
}

void OccludeSectors(I3D_scene *scene)
{
    if (!g_driverStates[DRVSTATE_OCCLUSION] || !scene)
        return;
    I3D_sector *s = scene->firstSector;
    for (int n = scene->sectorCount; n; --n) {
        if ((s->sectorFlags & SCTF_VISIBLE) && s->occluders) {
            DropOccludersOnPath(s);
            OccludeOccluders(s);
            CullVisuals(s);
        }
        s = s->nextInScene;
    }
    I3D_sector *const primary = scene->primarySector;
    if (primary->sectorFlags & SCTF_VISIBLE) {
        DropOccludersOnPath(primary);
        OccludeOccluders(primary);
        CullVisuals(primary);
    }
}

}  // namespace ls3d::i3d
