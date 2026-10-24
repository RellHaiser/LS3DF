// The renderer's sector pass (0x1003f380 - 0x10042620, 0x10042fc0): from the camera's sector
// through the open portals, each sector seen gets the visuals inside the view (the frustum, or
// the portal's clip rect) chained in its renderVisuals.
#include <cmath>
#include <cstring>

#include "core/debug.h"
#include "i3d/camera.h"
#include "i3d/i3d_hooks.h"
#include "i3d/occluder.h"
#include "i3d/portal.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/visual.h"
#include "igraph/igraph.h"

using namespace ls3d;
using namespace ls3d::i3d;

// Frame flags of the sector pass.
enum : uint32_t {
    FRMFLAGS_PORTAL_TEST = 0x00010000,      // inside the range: the portal pass tests it against its clip planes
    FRMFLAGS_CLIPPED = 0x00200000,          // crosses a view plane (set, never read)
};

namespace {

// The view planes of the current sector (0x100be910: 0 the depth plane, 1 - 4 the sides through
// the eye and the clip rect's corners; their distances again at 0x100be734), the depth range
// (0x100be730 / 0x100be768), the portal looked through (0x100be764; null: the whole view), the
// special sector's projection (0x100be900: 0, 0x1000 backdrop, 0x4000 near sector).
S_plane s_clipPlanes[5];
float s_clipD[5];
float s_clipNear;
float s_clipFar;
I3D_portal *s_clipPortal;
uint32_t s_clipMode;
// A portal beyond its range or smaller than 10 pixels (0x100be760): only sectors' own frames
// below a sector without SCTF 0x100 are walked further.
bool s_portalFar;
// The sector whose render list gets the visuals (0x100be960).
I3D_sector *s_listSector;
// The sectors being looked into (0x100be770, 100 entries; 0x100be964 the top, 0x100be968 set
// when it overflowed).
I3D_sector *s_sectorStack[100];
I3D_sector **s_sectorTop = s_sectorStack;
bool s_sectorStackOverflow;

// A float sign bit test as the original's integer compare (bits above 0x80000000: negative, not
// -0).
bool Negative(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b > 0x80000000u;
}

// Whether every corner of the box is on the negative side of plane `i` (the original's walk
// through the corners, one axis step at a time).
bool BoxOutside(int i, const S_vector4 &mn, const S_vector4 &mx)
{
    const S_vector &n = s_clipPlanes[i].normal;
    float v = ((s_clipD[i] + n.x * mn.x) + n.y * mn.y) + n.z * mn.z;
    if (!Negative(v))
        return false;
    const float dz = n.z * mx.z - n.z * mn.z;
    v = v + dz;
    if (!Negative(v))
        return false;
    const float dy = n.y * mx.y - n.y * mn.y;
    v = v + dy;
    if (!Negative(v))
        return false;
    v = v - dz;
    if (!Negative(v))
        return false;
    const float dx = n.x * mx.x - n.x * mn.x;
    v = v + dx;
    if (!Negative(v))
        return false;
    v = v + dz;
    if (!Negative(v))
        return false;
    v = v - dy;
    if (!Negative(v))
        return false;
    return Negative(v - dz);
}

float PlaneDistance(int i, const S_vector &c)
{
    const S_vector &n = s_clipPlanes[i].normal;
    return ((n.z * c.z + n.y * c.y) + n.x * c.x) + s_clipD[i];
}

// The side planes 1 - 4: a frame's bound sphere (center c, radius r) against them. False: outside
// one; `mask` gets the planes it crosses (then its box decides).
bool SidePlanes(const S_vector &c, float r, float dist[4], uint32_t &mask)
{
    const float nr = -r;
    for (int i = 0; i < 4; ++i) {
        dist[i] = PlaneDistance(i + 1, c);
        if (!(dist[i] > nr))
            return false;
    }
    mask = 0;
    for (int i = 0; i < 4; ++i) {
        if (r > dist[i])
            mask |= 1u << i;
    }
    return true;
}

// A crossed frame's box: outside a crossed plane after all.
bool BoxCulled(I3D_frame *f, uint32_t mask)
{
    if (!(f->frameFlags & FRMFLAGS_BOUND_VALID))
        f->UpdateBound();
    const S_vector4 mn = *reinterpret_cast<const S_vector4 *>(&f->bound.bbox.min);
    if (!(f->frameFlags & FRMFLAGS_BOUND_VALID))
        f->UpdateBound();
    const S_vector4 mx = *reinterpret_cast<const S_vector4 *>(&f->bound.bbox.max);
    for (int i = 0; i < 4; ++i) {
        if ((mask & (1u << i)) && !BoxOutside(i + 1, mn, mx))
            mask ^= 1u << i;
    }
    return mask != 0;
}

void AddToList(I3D_frame *f, uint32_t clipFlags)
{
    f->frameFlags |= clipFlags;
    f->renderStamp = g_renderStamp;
    if (f->type == FRAME_VISUAL) {
        I3D_visual *v = static_cast<I3D_visual *>(f);
        v->nextRenderVisual = s_listSector->renderVisuals;
        s_listSector->renderVisuals = v;
    }
}

// A float of the camera object at a byte offset; the original's portal-pass clip test reads
// "planes" 0x50 bytes apart, past the camera's end for the backdrop and near sector frustums:
// those read as 0 here.
float CameraFloat(const I3D_camera *cam, size_t offset)
{
    if (offset + 4 > sizeof(I3D_camera))
        return 0.0f;
    float f;
    std::memcpy(&f, reinterpret_cast<const uint8_t *>(cam) + offset, 4);
    return f;
}

// The frame flag of a portal-pass frame crossing the camera's side planes (as the original
// indexes them: plane k at +0x50 k, its distance at +0x14 k).
uint32_t CameraClipFlags(const S_vector &c, float r)
{
    const I3D_camera *cam = g_renderCamera;
    size_t planes, dists;
    if (s_clipMode == SCTF_BACKDROP) {
        planes = offsetof(I3D_camera, backdropFrustum);
        dists = offsetof(I3D_camera, backdropFrustumD);
    } else if (s_clipMode == SCTF_NEAR) {
        planes = offsetof(I3D_camera, nearSectorFrustum);
        dists = offsetof(I3D_camera, nearSectorFrustumD);
    } else {
        planes = offsetof(I3D_camera, frustum);
        dists = offsetof(I3D_camera, frustumD);
    }
    uint32_t flags = 0;
    for (size_t k = 1; k <= 4; ++k) {
        const size_t p = planes + 0x50 * k;
        const float d = ((c.z * CameraFloat(cam, p + 8) + c.y * CameraFloat(cam, p + 4)) +
                         c.x * CameraFloat(cam, p)) + CameraFloat(cam, dists + 0x14 * k);
        if (d < r)
            flags = FRMFLAGS_CLIPPED;
    }
    return flags;
}

// A view plane through the eye and an edge: n = a x b (a the edge, b the eye minus its start),
// normalized; returns d.
S_vector EdgeNormal(const S_vector &a, const S_vector &b)
{
    S_vector n;
    n.x = b.z * a.y - b.y * a.z;
    n.y = b.x * a.z - a.x * b.z;
    n.z = a.x * b.y - b.x * a.y;
    return n;
}

}  // namespace

// The frames below `parent` within the depth range and the side planes join the list sector's
// render list (with a portal: marked for the portal pass); a sector then tests its occluders
// against the camera's whole view (0x1003f380).
static void CollectFrames(I3D_frame *parent)
{
    for (I3D_frame *f = parent->firstChild; f; f = f->nextSibling) {
        f->frameFlags &= ~(FRMFLAGS_PORTAL_TEST | FRMFLAGS_CLIPPED);
        if ((f->frameFlags & FRMFLAGS_CAMERA_PROJECTION) || !(f->frameFlags & FRMFLAGS_ON))
            continue;
        if (!(f->frameFlags & FRMFLAGS_BOUND_VALID))
            f->UpdateBound();
        const S_vector c = f->bound.center;
        const float r = f->bound.radius;
        const float d0 = PlaneDistance(0, c);
        if (f->type == FRAME_VISUAL) {
            const float t = d0 + 10000.0f;
            static_cast<I3D_visual *>(f)->depth = t < 0.0f ? 0.0f : t * 1000.0f;
        }
        if (r + d0 < s_clipNear)
            continue;
        if (d0 - s_clipNear < r || d0 - s_clipNear == r)
            f->frameFlags |= FRMFLAGS_CLIPPED;
        if (d0 - r > s_clipFar)
            continue;
        if (s_clipFar - d0 < r || s_clipFar - d0 == r)
            f->frameFlags |= FRMFLAGS_CLIPPED;
        if (s_clipPortal) {
            f->frameFlags |= FRMFLAGS_PORTAL_TEST;
        } else {
            float dist[4];
            uint32_t mask;
            if (!SidePlanes(c, r, dist, mask))
                continue;
            if (mask && BoxCulled(f, mask))
                continue;
            uint32_t clip = 0;
            for (int i = 0; i < 4; ++i) {
                if (r > dist[i])
                    clip = FRMFLAGS_CLIPPED;
            }
            AddToList(f, clip);
        }
        if (f->firstChild)
            CollectFrames(f);
    }
    if (parent->type != FRAME_SECTOR)
        return;

    // The sector's occluders against the camera's whole view.
    I3D_camera *cam = g_renderCamera;
    const float w = cam->nearHalfWidth * 100.0f;
    const float h = cam->nearHalfHeight * 100.0f;
    const float n = cam->nearRange * 100.0f;
    const S_vector q0 = S_vector(-w, h, n) * cam->invViewMatrix;
    const S_vector q1 = S_vector(-w, -h, n) * cam->invViewMatrix;
    const S_vector q2 = S_vector(w, h, n) * cam->invViewMatrix;
    const S_vector q3 = S_vector(w, -h, n) * cam->invViewMatrix;
    const S_vector eye = cam->GetMatrix()(3);
    const S_vector p0 = cam->frustum[0].normal;
    const float d0 = cam->frustumD[0];
    S_vector na = EdgeNormal(q1 - q0, eye - q0);
    NormalizeWithSquare(na, SquareXZY(na));
    const float da = -((eye.x * na.x + eye.z * na.z) + eye.y * na.y);
    S_vector nb = EdgeNormal(q2 - q3, eye - q3);
    NormalizeWithSquare(nb, SquareZYX(nb));
    const float db = -((eye.x * nb.x + nb.z * eye.z) + nb.y * eye.y);
    S_vector nc = EdgeNormal(q0 - q2, eye - q2);
    NormalizeWithSquare(nc, SquareZYX(nc));
    const float dc = -((eye.x * nc.x + nc.z * eye.z) + nc.y * eye.y);
    S_vector nd = EdgeNormal(q3 - q1, eye - q1);
    NormalizeWithSquare(nd, SquareZYX(nd));
    const float dd = -((nd.z * eye.z + nd.y * eye.y) + nd.x * eye.x);
    I3D_sector *sector = static_cast<I3D_sector *>(parent);
    for (I3D_occluder *o = sector->occluders; o; o = o->nextInSector) {
        const S_vector &oc = o->center;
        const float depth = ((oc.z * p0.z + oc.y * p0.y) + oc.x * p0.x) + d0;
        o->viewDepth = depth;
        if (!(depth + o->radius > cam->nearRange))
            continue;
        if (!(depth - o->radius < cam->farRange))
            continue;
        const float nr = -o->radius;
        if (!(((na.z * oc.z + na.y * oc.y) + na.x * oc.x) + da > nr))
            continue;
        if (!(((nb.z * oc.z + nb.y * oc.y) + nb.x * oc.x) + db > nr))
            continue;
        if (!(((nc.z * oc.z + nc.y * oc.y) + nc.x * oc.x) + dc > nr))
            continue;
        if (!(((oc.y * nd.y + nd.x * oc.x) + nd.z * oc.z) + dd > nr))
            continue;
        o->frameFlags |= FRMFLAGS_PORTAL_TEST;
    }
}

// The frames marked by CollectFrames tested against the portal's clip planes; those inside join
// the list sector's render list (0x100406c0).
static void CollectPortalFrames(I3D_frame *parent)
{
    if (!s_clipPortal)
        return;
    for (I3D_frame *f = parent->firstChild; f; f = f->nextSibling) {
        if (f->renderStamp != g_renderStamp) {
            if (!(f->frameFlags & FRMFLAGS_PORTAL_TEST))
                continue;
            if (!(f->frameFlags & FRMFLAGS_BOUND_VALID))
                f->UpdateBound();
            const S_vector c = f->bound.center;
            const float r = f->bound.radius;
            float dist[4];
            uint32_t mask;
            if (!SidePlanes(c, r, dist, mask))
                continue;
            if (mask && BoxCulled(f, mask))
                continue;
            AddToList(f, CameraClipFlags(c, r));
        }
        if (f->firstChild &&
            (!s_portalFar || (f->parent->type == FRAME_SECTOR &&
                              !(static_cast<I3D_sector *>(f->parent)->sectorFlags & 0x100))))
            CollectPortalFrames(f);
    }
}

// The view planes for `sector` seen through `portal` (null: the whole view) in projection
// `mode`, then its frames (once per frame: SCTF 0x10) and the portal pass (0x10040ea0).
static void CollectVisuals(I3D_sector *sector, I3D_portal *portal, uint32_t mode)
{
    I3D_camera *cam = g_renderCamera;
    s_clipPortal = portal;
    s_clipMode = mode;
    const float w = cam->nearHalfWidth * 100.0f;
    const float h = cam->nearHalfHeight * 100.0f;
    const float n = cam->nearRange * 100.0f;
    S_matrix m;
    std::memcpy(&m, mode == SCTF_BACKDROP ? &cam->invViewRotation : &cam->invViewMatrix, sizeof m);
    const S_vector c(m.m[2][0] * n + m.m[3][0], m.m[2][1] * n + m.m[3][1], m.m[2][2] * n + m.m[3][2]);
    const S_vector x(m.m[0][0] * w, m.m[0][1] * w, m.m[0][2] * w);
    const S_vector y(m.m[1][0] * h, m.m[1][1] * h, m.m[1][2] * h);
    S_vector p0, p1, p2, p3;                // corners: (-1, -1), (1, 1), (1, -1), (-1, 1)
    if (!portal) {
        p0 = S_vector((c.x - y.x) - x.x, (c.y - y.y) - x.y, (c.z - y.z) - x.z);
        p1 = S_vector((c.x + y.x) + x.x, (c.y + y.y) + x.y, (c.z + y.z) + x.z);
        p2 = S_vector((c.x - y.x) + x.x, (c.y - y.y) + x.y, (c.z - y.z) + x.z);
        p3 = S_vector((c.x + y.x) - x.x, (c.y + y.y) - x.y, (c.z + y.z) - x.z);
    } else {
        // A portal seen as less than 10 pixels wide and high counts as far.
        const float dx = portal->clipMax.x - portal->clipMin.x;
        if (dx < (2.0f / static_cast<float>(g_pIGraph->ScrnX())) * 10.0f) {
            const float dy = portal->clipMax.y - portal->clipMin.y;
            if (dy < (2.0f / static_cast<float>(g_pIGraph->ScrnY())) * 10.0f)
                s_portalFar = true;
        }
        const float x0 = portal->clipMin.x, y0 = portal->clipMin.y;
        const float x1 = portal->clipMax.x, y1 = portal->clipMax.y;
        auto corner = [&](float sx, float sy) {
            return S_vector((c.x + sy * y.x) + sx * x.x, (c.y + sy * y.y) + sx * x.y, (c.z + sy * y.z) + sx * x.z);
        };
        p0 = corner(x0, y0);
        p1 = corner(x1, y1);
        p2 = corner(x1, y0);
        p3 = corner(x0, y1);
    }
    S_vector eye;
    if (mode == SCTF_BACKDROP) {
        // The backdrop is drawn around the camera: the eye at the origin.
        eye = S_vector(0.0f, 0.0f, 0.0f);
        s_clipPlanes[0] = cam->backdropFrustum[0];
        s_clipD[0] = cam->backdropFrustumD[0];
        s_clipNear = sector->scene->backdropNear;
        s_clipFar = sector->scene->backdropFar;
    } else if (mode == SCTF_NEAR) {
        eye = cam->GetMatrix()(3);
        s_clipPlanes[0] = cam->nearSectorFrustum[0];
        s_clipD[0] = cam->nearSectorFrustumD[0];
        s_clipNear = cam->nearSectorNear;
        s_clipFar = cam->nearSectorFar;
    } else {
        eye = cam->GetMatrix()(3);
        s_clipPlanes[0] = cam->frustum[0];
        s_clipD[0] = cam->frustumD[0];
        s_clipNear = cam->nearRange;
        s_clipFar = cam->farRange;
    }
    auto side = [&](int i, const S_vector &a, const S_vector &pivot, bool xzy) {
        S_vector nn = EdgeNormal(a, eye - pivot);
        NormalizeWithSquare(nn, xzy ? SquareXZY(nn) : SquareZYX(nn));
        const float d = -((eye.z * nn.z + eye.y * nn.y) + nn.x * eye.x);
        s_clipPlanes[i].normal = nn;
        s_clipPlanes[i].d = d;
        s_clipD[i] = d;
    };
    side(1, p0 - p3, p3, false);
    side(2, p1 - p2, p2, false);
    side(3, p3 - p1, p1, true);
    side(4, p2 - p0, p0, true);
    if (!(sector->sectorFlags & 0x10)) {
        CollectFrames(sector);
        sector->sectorFlags |= 0x10;
    }
    CollectPortalFrames(sector);
}

// Looks into the sector `portal` leads to (`backwards`: an incoming portal, into its owner) when
// its projection overlaps the clip rect of the portal it is seen through (0x10041dc0).
static void ParsePortal(I3D_portal *prev, I3D_portal *portal, bool backwards);

// Enters `sector` (through `portal`, null for the camera's): its visuals, its occluders, then the
// portals the camera sees (0x10042620).
static void TraverseSector(I3D_sector *sector, I3D_portal *portal)
{
    if (!(sector->frameFlags & FRMFLAGS_ON))
        return;
    I3D_camera *cam = g_renderCamera;
    if (portal) {
        if (!(portal->portalFlags & PRTF_ON))
            return;
        const S_vector &e = cam->GetMatrix()(3);
        const float dx = portal->center.x - e.x, dy = portal->center.y - e.y, dz = portal->center.z - e.z;
        const float d2 = (dz * dz + dy * dy) + dx * dx;
        const float reach = static_cast<float>(static_cast<double>(cam->farRange * cam->farRange) * 1.2) +
            portal->radius * portal->radius;
        if (reach < d2)
            return;
    }
    s_portalFar = false;
    if (portal) {
        if (portal->range > 0.0f) {
            const S_vector &e = cam->GetMatrix()(3);
            const float dx = portal->center.x - e.x, dy = portal->center.y - e.y, dz = portal->center.z - e.z;
            const float d2 = ((dz * dz + dy * dy) + dx * dx) / g_fovScaleSq;
            if (d2 > portal->range) {
                if (portal->portalFlags & PRTF_RANGE_CLIP)
                    return;
                s_portalFar = true;
            }
        }
        portal->portalFlags |= PRTF_VISITED;
        if (!portal->otherSide && portal->connectedSector == sector && s_sectorTop < s_sectorStack + 100 &&
            s_sectorTop > s_sectorStack) {
            // A one-way portal into a sector being looked into: the sectors since are on its path.
            I3D_sector **p = s_sectorTop;
            do {
                I3D_sector *s = *--p;
                if (s == sector)
                    break;
                s->sectorFlags |= SCTF_ON_PATH;
            } while (p > s_sectorStack);
        }
    }
    if (sector->sectorFlags & 2)
        return;
    sector->sectorFlags |= 3;
    // (The original pops even when the push overflowed, walking the stack pointer below the
    // array; the port pops only what it pushed.)
    bool pushed = false;
    if (s_sectorTop < s_sectorStack + 100) {
        *s_sectorTop++ = sector;
        pushed = true;
    } else {
        s_sectorStackOverflow = true;
    }
    s_listSector = sector;
    if (!(sector->sectorFlags & 0x10)) {
        sector->renderVisuals = nullptr;
        CollectVisuals(sector, portal, 0);
        OccludePortals(sector);
    } else {
        CollectVisuals(sector, portal, 0);
    }
    const S_vector e = cam->GetMatrix()(3);
    for (int i = 0; i < sector->portals.Size(); ++i) {
        I3D_portal *p = sector->portals[i];
        const float d = ((e.y * p->plane.normal.y + e.z * p->plane.normal.z) + e.x * p->plane.normal.x) + p->plane.d;
        if (d > 0.0f && !(p->portalFlags & PRTF_OCCLUDED))
            ParsePortal(portal, p, false);
    }
    for (int i = 0; i < sector->incomingPortals.Size(); ++i) {
        I3D_portal *p = sector->incomingPortals[i];
        if (p->otherSide)
            continue;
        const float d = ((e.y * p->plane.normal.y + e.z * p->plane.normal.z) + e.x * p->plane.normal.x) + p->plane.d;
        if (d < 0.0f && !(p->portalFlags & PRTF_OCCLUDED))
            ParsePortal(portal, p, true);
    }
    if (pushed)
        --s_sectorTop;
    sector->sectorFlags ^= 2;
}

static void ParsePortal(I3D_portal *prev, I3D_portal *portal, bool backwards)
{
    I3D_sector *target = backwards ? portal->owner : portal->connectedSector;
    if (portal->renderFlags & PRTR_PARTLY_BEHIND)
        portal->renderFlags ^= PRTR_PARTLY_BEHIND;
    portal->renderFlags |= PRTR_ALL_BEHIND;
    // The vertices projected (x, y; z: w), the double list lets runs wrap.
    S_vector4 proj[2 * I3D_portal::kMaxVertices];
    const S_matrix &m = *g_viewProjection;
    const int count = portal->numVertices;
    auto fullView = [&]() {
        portal->clipMin = S_vector4{ -1.01f, -1.01f, 0.0f, 0.0f };
        portal->clipMax = S_vector4{ 1.01f, 1.01f, 0.0f, 0.0f };
        TraverseSector(target, portal);
    };
    for (int i = 0; i < count; ++i) {
        const S_vector &v = *reinterpret_cast<const S_vector *>(&portal->worldVertices[i]);
        const float wv = ((m.m[2][3] * v.z + m.m[1][3] * v.y) + m.m[0][3] * v.x) + m.m[3][3];
        if (wv == 0.0f) {
            fullView();
            return;
        }
        if (wv < 0.0f)
            portal->renderFlags |= PRTR_PARTLY_BEHIND;
        else if (portal->renderFlags & PRTR_ALL_BEHIND)
            portal->renderFlags ^= PRTR_ALL_BEHIND;
        const float inv = 1.0f / wv;
        const S_vector p = v * m;
        proj[i] = S_vector4{ inv * p.x, inv * p.y, wv, 0.0f };
    }
    if (portal->renderFlags & PRTR_ALL_BEHIND)
        return;
    if (portal->renderFlags & PRTR_PARTLY_BEHIND) {
        // From the first vertex in front after one behind, around to the last in front: the
        // rect of those, open towards the sides where the edges go behind the camera.
        for (int i = 0; i < count; ++i)
            proj[count + i] = proj[i];
        S_vector4 *p = proj;
        for (int k = count; k != 0;) {
            --k;
            if (!(p->z > 0.0f))
                break;
            ++p;
        }
        if (p->z > 0.0f) {
            dbgPrintf("!! ParsePortal ERROR 1 !!");
            fullView();
            return;
        }
        for (int k = count; k != 0;) {
            --k;
            if (p->z > 0.0f || std::isnan(p->z))
                break;
            ++p;
        }
        if (!(p->z > 0.0f || std::isnan(p->z))) {
            dbgPrintf("!! ParsePortal ERROR 2 !!");
            fullView();
            return;
        }
        const float dx1 = p->x - p[-1].x;
        const float dy1 = p->y - p[-1].y;
        portal->rectMin = *p;
        portal->rectMax = *p;
        if (p->z > 0.0f) {
            do {
                if (portal->rectMin.x > p->x)
                    portal->rectMin.x = p->x;
                if (portal->rectMin.y > p->y)
                    portal->rectMin.y = p->y;
                if (portal->rectMax.x < p->x)
                    portal->rectMax.x = p->x;
                if (portal->rectMax.y < p->y)
                    portal->rectMax.y = p->y;
                ++p;
            } while (p->z > 0.0f);
        }
        const S_vector4 *q = p - 1;
        const float dx2 = q->x - q[1].x;
        const float dy2 = q->y - q[1].y;
        if (dx1 >= 0.0f || dx2 >= 0.0f)
            portal->rectMax.x = 1.01f;
        if (dx1 <= 0.0f || dx2 <= 0.0f)
            portal->rectMin.x = -1.01f;
        if (dy1 >= 0.0f || dy2 >= 0.0f)
            portal->rectMax.y = 1.01f;
        if (dy1 <= 0.0f || dy2 <= 0.0f)
            portal->rectMin.y = -1.01f;
    } else {
        portal->rectMin = proj[0];
        portal->rectMax = proj[0];
        float minX = proj[0].x, minY = proj[0].y, maxX = proj[0].x, maxY = proj[0].y;
        for (int i = 1; i < count; ++i) {
            if (minX > proj[i].x)
                minX = proj[i].x;
            if (minY > proj[i].y)
                minY = proj[i].y;
            if (maxX < proj[i].x)
                maxX = proj[i].x;
            if (maxY < proj[i].y)
                maxY = proj[i].y;
        }
        portal->rectMin.x = minX;
        portal->rectMin.y = minY;
        portal->rectMax.x = maxX;
        portal->rectMax.y = maxY;
    }
    if (!prev) {
        portal->clipMin = portal->rectMin;
        portal->clipMax = portal->rectMax;
        auto clamp = [](float &f) {
            if (f < -1.01f)
                f = -1.01f;
            if (f > 1.01f)
                f = 1.01f;
        };
        clamp(portal->clipMin.x);
        clamp(portal->clipMin.y);
        clamp(portal->clipMax.x);
        clamp(portal->clipMax.y);
        if (portal->clipMin.x == portal->clipMax.x || portal->clipMin.y == portal->clipMax.y)
            return;
        TraverseSector(target, portal);
        return;
    }
    // Within the previous portal's clip rect.
    if (!(prev->clipMin.x <= portal->rectMax.x) || !(prev->clipMax.x >= portal->rectMin.x) ||
        !(prev->clipMin.y <= portal->rectMax.y) || !(prev->clipMax.y >= portal->rectMin.y))
        return;
    portal->clipMin.x = prev->clipMin.x > portal->rectMin.x ? prev->clipMin.x : portal->rectMin.x;
    portal->clipMax.x = prev->clipMax.x < portal->rectMax.x ? prev->clipMax.x : portal->rectMax.x;
    portal->clipMin.y = prev->clipMin.y > portal->rectMin.y ? prev->clipMin.y : portal->rectMin.y;
    portal->clipMax.y = prev->clipMax.y < portal->rectMax.y ? prev->clipMax.y : portal->rectMax.y;
    TraverseSector(target, portal);
}

namespace ls3d::i3d {

// 0x10042fc0
void TraverseSectors(I3D_scene *scene)
{
    if (!scene)
        return;
    if (scene->sectorChangesSeen != g_sectorChanges) {
        scene->sectorChangesSeen = g_sectorChanges;
        scene->ReadySectors();
    }
    if (scene->portalChangesSeen != g_portalChanges) {
        scene->portalChangesSeen = g_portalChanges;
        BuildPortals(scene);
        if (I3D_portal *p = scene->firstPortal) {
            do {
                if (p->portalFlags & PRTF_DIVIDES_SECTOR) {
                    const char *to = p->connectedSector->name ? p->connectedSector->name : "";
                    const char *from = p->owner->name ? p->owner->name : "";
                    dbgPrintf("!! Portal divides sector (portal in sector \"%s\" to sector \"%s\" - autorepair CAN'T be used )",
                              from, to);
                }
                p = p->next;
            } while (p != scene->firstPortal);
        }
    }
    RebuildOccluderLists(scene);
    I3D_sector *start = g_renderCamera->sector;
    if (!start)
        return;
    // (The first portal of the ring keeps its state: the original's loop starts at the second.)
    if (I3D_portal *first = scene->firstPortal) {
        for (I3D_portal *p = first->next; p != first; p = p->next) {
            p->renderFlags = 0;
            p->portalFlags &= ~PRTF_VISITED;
        }
    }
    I3D_sector *s = scene->firstSector;
    for (int n = scene->sectorCount; n != 0; --n, s = s->nextInScene)
        s->sectorFlags &= ~0x91u;
    scene->primarySector->renderVisuals = nullptr;
    scene->backdropSector->renderVisuals = nullptr;
    scene->omniSector->renderVisuals = nullptr;
    scene->nearSector->renderVisuals = nullptr;
    scene->primarySector->sectorFlags &= ~0x91u;
    s_sectorStackOverflow = false;
    s_sectorTop = s_sectorStack;
    TraverseSector(start, nullptr);
    scene->backdropSector->sectorFlags &= ~0x91u;
    s_listSector = scene->backdropSector;
    CollectVisuals(s_listSector, nullptr, SCTF_BACKDROP);
    scene->omniSector->sectorFlags &= ~0x91u;
    s_listSector = scene->omniSector;
    CollectVisuals(s_listSector, nullptr, 0);
    scene->nearSector->sectorFlags &= ~0x91u;
    s_listSector = scene->nearSector;
    CollectVisuals(s_listSector, nullptr, SCTF_NEAR);
}

}  // namespace ls3d::i3d

namespace ls3d::i3d {

// Every portal without a connection paired with the portal of the other side (the same opening
// in another sector: overlapping boxes, centers within a tenth of the box), else connected to
// the sector its first vertex is in, else to the primary sector (0x10042940).
void BuildPortals(I3D_scene *scene)
{
    I3D_portal *const first = scene->firstPortal;
    if (!first)
        return;
    I3D_portal *p = first;
    do {
        if (!p->connectedSector && p->next != first) {
            for (I3D_portal *q = p->next; q != scene->firstPortal;) {
                if (q->connectedSector) {
                    q = q->next;
                    continue;
                }
                // The box of both portals' vertices and each portal's center.
                const S_vector4 *v = p->worldVertices;
                S_vector mn(v[0].x, v[0].y, v[0].z), mx = mn, sp = mn;
                for (int i = 1; i < p->numVertices; ++i) {
                    if (mn.x > v[i].x)
                        mn.x = v[i].x;
                    if (mn.y > v[i].y)
                        mn.y = v[i].y;
                    if (mn.z > v[i].z)
                        mn.z = v[i].z;
                    if (mx.x < v[i].x)
                        mx.x = v[i].x;
                    if (mx.y < v[i].y)
                        mx.y = v[i].y;
                    if (mx.z < v[i].z)
                        mx.z = v[i].z;
                    sp.x = sp.x + v[i].x;
                    sp.y = sp.y + v[i].y;
                    sp.z = sp.z + v[i].z;
                }
                const float invP = 1.0f / static_cast<float>(p->numVertices);
                const S_vector pc(sp.x * invP, sp.y * invP, invP * sp.z);
                S_vector sq(0.0f, 0.0f, 0.0f);
                const S_vector4 *w = q->worldVertices;
                for (int i = 0; i < q->numVertices; ++i) {
                    if (mn.x > w[i].x)
                        mn.x = w[i].x;
                    if (mn.y > w[i].y)
                        mn.y = w[i].y;
                    if (mn.z > w[i].z)
                        mn.z = w[i].z;
                    if (mx.x < w[i].x)
                        mx.x = w[i].x;
                    if (mx.y < w[i].y)
                        mx.y = w[i].y;
                    if (mx.z < w[i].z)
                        mx.z = w[i].z;
                    sq.x = sq.x + w[i].x;
                    sq.y = sq.y + w[i].y;
                    sq.z = sq.z + w[i].z;
                }
                const float invQ = 1.0f / static_cast<float>(q->numVertices);
                const S_vector qc(sq.x * invQ, sq.y * invQ, invQ * sq.z);
                if (mx.x - mn.x < 0.01f)
                    mx.x = mn.x + 0.1f;
                if (mx.y - mn.y < 0.01f)
                    mx.y = mn.y + 0.1f;
                if (mx.z - mn.z < 0.01f)
                    mx.z = mn.z + 0.1f;
                if (!((mx.x - mn.x) * 0.1f >= std::fabs(qc.x - pc.x)) ||
                    !((mx.y - mn.y) * 0.1f >= std::fabs(qc.y - pc.y)) ||
                    !((mx.z - mn.z) * 0.1f >= std::fabs(qc.z - pc.z))) {
                    q = q->next;
                    continue;
                }
                if (q->owner == p->owner) {
                    p->portalFlags |= PRTF_IN_PORTAL;
                    q->portalFlags |= PRTF_IN_PORTAL;
                    q = q->next;
                    dbgPrintf("!! Portal in portal (sector \"%s\")", p->owner->name ? p->owner->name : "");
                    continue;
                }
                p->otherSide = q;
                q->otherSide = p;
                p->connectedSector = q->owner;
                q->connectedSector = p->owner;
                p->owner->incomingPortals.PushBack(q);
                q->owner->incomingPortals.PushBack(p);
                q = scene->firstPortal;
            }
        }
        if (!p->connectedSector) {
            p->otherSide = nullptr;
            const S_vector4 pos = p->worldVertices[0];
            I3D_sector *s = scene->firstSector;
            for (int n = scene->sectorCount; n != 0; --n, s = s->nextInScene) {
                if (s->IsInside(*reinterpret_cast<const S_vector *>(&pos)) && s != p->owner) {
                    p->connectedSector = s;
                    s->incomingPortals.PushBack(p);
                    break;
                }
            }
            if (!p->connectedSector) {
                p->connectedSector = scene->primarySector;
                scene->primarySector->incomingPortals.PushBack(p);
            }
        }
        p = p->next;
    } while (p != scene->firstPortal);
}

}  // namespace ls3d::i3d
