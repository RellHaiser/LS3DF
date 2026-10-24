// I3D_portal (0x1003ed10 - 0x1003f370, Load 0x100431a0).
#include "i3d/portal.h"

#include <cmath>
#include <cstring>

#include "external/rw_data.h"
#include "i3d/loader.h"
#include "i3d/scene.h"
#include "i3d/sector.h"

namespace ls3d::i3d {

int g_portalChanges;

}  // namespace ls3d::i3d

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

const S_vector &AsVector(const S_vector4 &v) { return reinterpret_cast<const S_vector &>(v); }

}  // namespace

void *I3D_portal::operator new(size_t size) noexcept { return AlignedAlloc(size, 16); }
void I3D_portal::operator delete(void *p) { AlignedFree(p); }

I3D_portal::I3D_portal()
{
    numVertices = 0;
    owner = nullptr;
    connectedSector = nullptr;
    renderFlags = 0;
    scene = nullptr;
    prev = nullptr;
    next = nullptr;
    otherSide = nullptr;
    range = 0.0f;
    portalFlags = PRTF_ON;
    ++g_portalChanges;
}

I3D_portal::~I3D_portal()
{
    ++g_portalChanges;
    // The original passes the portal's own scene here, which changes nothing: a portal destroyed
    // while in a scene (I3D_sector::Duplicate frees the old ones) stayed in the scene's ring,
    // dangling. It leaves the ring.
    SetScene(nullptr);
    if (otherSide) {
        otherSide->otherSide = nullptr;
        otherSide = nullptr;
    }
}

void I3D_portal::SetOn(bool on)
{
    if (on)
        portalFlags |= PRTF_ON;
    else
        portalFlags &= ~PRTF_ON;
    if (otherSide) {
        if (on)
            otherSide->portalFlags |= PRTF_ON;
        else
            otherSide->portalFlags &= ~PRTF_ON;
    }
}

bool I3D_portal::IsOn() { return (portalFlags & PRTF_ON) != 0; }

I3D_RESULT I3D_portal::Duplicate(const I3D_portal *source)
{
    if (otherSide) {
        otherSide->otherSide = nullptr;
        otherSide = nullptr;
    }
    numVertices = source->numVertices;
    loadedPlane = source->loadedPlane;
    portalFlags = source->portalFlags & PRTF_DUPLICATE_MASK;
    range = source->range;
    range2 = source->range2;
    std::memcpy(vertices, source->vertices, sizeof vertices);
    return I3D_OK;
}

void I3D_portal::SetScene(I3D_scene *newScene)
{
    if (scene == newScene)
        return;
    if (scene) {
        --scene->portalCount;
        if (scene->firstPortal == this)
            scene->firstPortal = next;
        if (prev == this) {
            scene->firstPortal = nullptr;
        } else {
            prev->next = next;
            next->prev = prev;
        }
    }
    scene = newScene;
    if (newScene) {
        I3D_portal *const first = newScene->firstPortal;
        if (!first) {
            prev = this;
            next = this;
            newScene->firstPortal = this;
        } else {
            first->prev->next = this;
            prev = first->prev;
            next = first;
            first->prev = this;
        }
        ++newScene->portalCount;
    }
}

void I3D_portal::MatrixChanged()
{
    const S_matrix &m = owner->GetMatrix();
    S_vector lo(1e6f, 1e6f, 1e6f);
    S_vector hi(-1e6f, -1e6f, -1e6f);
    for (int i = 0; i < numVertices; ++i) {
        // (The original copies 16 bytes of the 12-byte product: the fourth is stack garbage.)
        const S_vector w = AsVector(vertices[i]) * m;
        worldVertices[i].x = w.x;
        worldVertices[i].y = w.y;
        worldVertices[i].z = w.z;
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
    // (centerW: the original stores an unset stack value.)
    center = S_vector((hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f);
    radius = MagnitudeOfSquare(SquareZYX(hi - lo)) * 0.5f;

    // The plane through the first, second and last vertex. (Without vertices the original reads
    // the entry before worldVertices, the last local vertex.)
    const S_vector4 &last = numVertices > 0 ? worldVertices[numVertices - 1] : vertices[kMaxVertices - 1];
    const S_vector &w0 = AsVector(worldVertices[0]);
    const S_vector e1 = AsVector(worldVertices[1]) - w0;
    const S_vector e2 = AsVector(last) - w0;
    S_vector n = e1.Cross(e2);
    NormalizeWithSquare(n, SquareZYX(n));
    plane.d = -n.Dot(AsVector(last));
    plane.normal = n;
    if (owner)
        SetScene(owner->scene);
}

void I3D_portal::Load()
{
    numVertices = 0;
    dtaRead(g_loadFile, &numVertices, 1);
    // The plane, flags and ranges, then the vertices. (The original reads every vertex the file
    // has: over 8 they overrun the portal. The extra ones are skipped.)
    const int stored = numVertices < kMaxVertices ? numVertices : kMaxVertices;
    dtaRead(g_loadFile, &loadedPlane, (stored + 2) * 0x10);
    for (int i = stored; i < numVertices; ++i) {
        S_vector4 skipped;
        dtaRead(g_loadFile, &skipped, sizeof skipped);
    }
    numVertices = stored;
}
