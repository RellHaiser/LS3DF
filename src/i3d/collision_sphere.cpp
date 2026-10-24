// The scene's moving-sphere queries (I3D_scene slots 0x108 - 0x114; 0x100115a0 - 0x10015050).
// (0x100145b0 is the static initialiser of an unused vector of the collision code.)
//
// A sphere moving from `from` by `dir` against the faces of the visuals' mesh levels (one-sided:
// the faces it moves toward): a face it already touches at the start is hit at once (the nearest
// point of the face, or of one of its edges, is the contact); else the first moment it touches the
// face's plane, at a point inside the face or near an edge. The query's result is the contact
// point of the nearest hit in world space.
#include "i3d/collision_state.h"

#include <cmath>

#include "i3d/billboard.h"
#include "i3d/mesh.h"
#include "i3d/scene.h"
#include "i3d/sector.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ls3d::i3d::col::q;

namespace {

// The contact point of a visual's best face (0x100ada60; its space, then world space).
S_vector s_contact;
// What the hit callback gets as the normal (0x100ad9f0).
const S_vector s_noNormal(0.0f, 0.0f, 0.0f);

bool InBox(const S_vector &p, const S_vector &mn, const S_vector &mx)
{
    auto in = [](float v, float lo, float hi) { return !(v < lo) && v <= hi; };
    return in(p.x, mn.x, mx.x) && in(p.y, mn.y, mx.y) && in(p.z, mn.z, mx.z);
}

// The box of a triangle grown by `r`.
void TriangleBox(const S_vector &v0, const S_vector &v1, const S_vector &v2, float r, S_vector &mn, S_vector &mx)
{
    for (int i = 0; i < 3; ++i) {
        const float a = (&v0.x)[i], b = (&v1.x)[i], c = (&v2.x)[i];
        float lo = a < b ? a : b;
        if (lo > c)
            lo = c;
        float hi = a > b ? a : b;
        if (hi < c)
            hi = c;
        (&mn.x)[i] = lo - r;
        (&mx.x)[i] = hi + r;
    }
}

// Whether `p` lies inside the triangle (v0, v0 + e0, v0 + e1) by its barycentric coordinates.
bool InTriangle(const S_vector &p, const S_vector &v0, const S_vector &e0, const S_vector &e1)
{
    const float d00 = SquareXZY(e0);
    const float d01 = (e1.x * e0.x + e1.z * e0.z) + e1.y * e0.y;
    const float d11 = SquareXZY(e1);
    const S_vector w(p.x - v0.x, p.y - v0.y, p.z - v0.z);
    const float d20 = (w.x * e0.x + w.z * e0.z) + w.y * e0.y;
    const float d21 = (w.x * e1.x + w.z * e1.z) + w.y * e1.y;
    const float u = d20 * d11 - d21 * d01;
    const float v = d21 * d00 - d20 * d01;
    return u >= 0.0 && v >= 0.0 && std::fabs(d11 * d00 - d01 * d01) >= v + u;
}

// The point `c` of the line from `a` along `e` nearest `p` (no farther from `a` than the edge is
// long, on either side), and whether `p` is within r of it; the start and the sweep tests sum the
// distance in their own orders.
bool EdgeTouchStart(const S_vector &p, const S_vector &a, const S_vector &e, float r2, S_vector &c)
{
    S_vector en = e;
    NormalizeWithSquare(en, SquareXZY(e));
    const S_vector w(p.x - a.x, p.y - a.y, p.z - a.z);
    const float s = (w.x * en.x + en.z * w.z) + en.y * w.y;
    c = S_vector(en.x * s + a.x, a.y + s * en.y, a.z + s * en.z);
    const S_vector d(p.x - c.x, p.y - c.y, p.z - c.z);
    return s * s <= SquareXZY(e) && SquareXZY(d) <= r2;
}
bool EdgeTouchSweepA(const S_vector &p, const S_vector &a, const S_vector &e, float r2, S_vector &c)
{
    S_vector en = e;
    NormalizeWithSquare(en, SquareXZY(e));
    const S_vector w(p.x - a.x, p.y - a.y, p.z - a.z);
    const float s = (en.x * w.x + en.z * w.z) + en.y * w.y;
    c = S_vector(en.x * s + a.x, a.y + s * en.y, a.z + s * en.z);
    const S_vector d(p.x - c.x, p.y - c.y, p.z - c.z);
    return s * s <= SquareXZY(e) && SquareZYX(d) <= r2;
}
bool EdgeTouchSweepB(const S_vector &p, const S_vector &a, const S_vector &b, float r2, S_vector &c)
{
    S_vector en;
    en.SetNormalized(b - a);
    const float s = en.Dot(p - a);
    c = a + en * s;
    return s * s <= (b - a).Square() && (p - c).Square() <= r2;
}

// 0x10011da0: a visual against the moving sphere.
void SphereVisual(I3D_visual *vis)
{
    const uint32_t type = vis->GetVisualType();
    const bool worldSpace = (vis->visualFlags & VISF_WORLD_SPACE) != 0;
    S_vector lfrom, lto;
    S_matrix inv;
    float r;
    if (worldSpace) {
        lfrom = q.from;
        lto = q.to;
        r = q.radius;
    } else {
        if (type == FourCC("BBRD")) {
            inv.Inverse(col::QueryFacing(static_cast<I3D_billboard *>(vis)));
        } else {
            if (!(vis->visualFlags & VISF_INVMATRIX_VALID)) {
                vis->visualFlags |= VISF_INVMATRIX_VALID;
                vis->invWorldMatrix.Inverse(vis->GetMatrix());
            }
            inv = vis->invWorldMatrix;
        }
        lfrom = q.from * inv;
        lto = q.to * inv;
        // (By the inverse matrix's scale: the original's.)
        r = q.radius / inv.GetUScale();
    }
    const float r2 = r * r;
    auto *obj = static_cast<I3D_object *>(vis);
    if (!obj->mesh)
        return;
    const int lod = vis == q.lodFrame ? q.frameLodLevel : q.lodLevel;
    I3D_mesh_level *level = obj->mesh->levels[lod];
    if (!level || type == FourCC("SNGM") || type == FourCC("SMMR"))
        return;
    const auto *vtx = static_cast<const uint8_t *>(level->LockVertices(1));
    if (!vtx)
        return;
    auto vertex = [vtx](uint16_t i) -> const S_vector & { return *reinterpret_cast<const S_vector *>(vtx + i * 0x20); };

    float best = 1e16f;
    for (I3D_face_group *fg = level->faceGroups; fg; fg = fg->next) {
        const I3D_triface *f = fg->Lock(1);
        for (int n = fg->numFaces; n > 0; --n, ++f) {
            const S_vector &v0 = vertex(f->v[0]), &v1 = vertex(f->v[1]), &v2 = vertex(f->v[2]);
            const S_vector a(v2.x - v1.x, v2.y - v1.y, v2.z - v1.z);
            const S_vector b(v0.x - v1.x, v0.y - v1.y, v0.z - v1.z);
            S_vector nrm(b.z * a.y - b.y * a.z, b.x * a.z - a.x * b.z, a.x * b.y - b.x * a.y);
            NormalizeWithSquare(nrm, SquareZYX(nrm));
            const S_vector fv(lfrom.x - v0.x, lfrom.y - v0.y, lfrom.z - v0.z);
            const float d0 = (nrm.z * fv.z + nrm.y * fv.y) + fv.x * nrm.x;
            const S_vector gv(lto.x - v0.x, lto.y - v0.y, lto.z - v0.z);
            const float d1 = (nrm.z * gv.z + nrm.y * gv.y) + gv.x * nrm.x;
            const S_vector e0(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
            const S_vector e1(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z);
            S_vector mn, mx;
            bool hit = false;

            if (std::fabs(d0) <= r) {
                // Touching the plane at the start: the plane point nearest the start.
                S_vector p;
                if (d0 < 0.0f) {
                    const float ad = std::fabs(d0);
                    p = S_vector(nrm.x * ad + lfrom.x, lfrom.y + nrm.y * ad, lfrom.z + nrm.z * ad);
                } else {
                    p = S_vector(lfrom.x - nrm.x * d0, lfrom.y - nrm.y * d0, lfrom.z - nrm.z * d0);
                }
                TriangleBox(v0, v1, v2, r, mn, mx);
                if (InBox(p, mn, mx)) {
                    S_vector c;
                    const S_vector e21(v1.x - v2.x, v1.y - v2.y, v1.z - v2.z);
                    if (InTriangle(p, v0, e0, e1)) {
                        c = p;
                        hit = true;
                    } else {
                        hit = EdgeTouchStart(p, v0, e0, r2, c) || EdgeTouchStart(p, v2, e21, r2, c) ||
                              EdgeTouchStart(p, v0, e1, r2, c);
                    }
                    if (hit) {
                        // (Without a distance check: at the start, nothing is nearer.)
                        best = 0.0f;
                        s_contact = c;
                    }
                }
            } else if (d0 > r && d1 < r) {
                // Reaching the plane while moving: where the sphere's front touches it.
                const float t = (d0 - r) / (d0 - d1);
                const float g = 1.0f - t;
                const S_vector at(lto.x * t, t * lto.y, t * lto.z);
                const S_vector bt(lfrom.x * g, lfrom.y * g, lfrom.z * g);
                const S_vector center(at.x + bt.x, at.y + bt.y, at.z + bt.z);
                const S_vector p(center.x - nrm.x * r, center.y - nrm.y * r, center.z - nrm.z * r);
                TriangleBox(v0, v1, v2, r, mn, mx);
                if (!InBox(p, mn, mx))
                    continue;
                if (InTriangle(p, v0, e0, e1)) {
                    if (!(best > t))
                        continue;
                    best = t;
                    s_contact = p;
                    hit = true;
                } else {
                    // The first edge it touches decides (a later contact than the best skips the
                    // face).
                    S_vector c;
                    if (!EdgeTouchSweepA(p, v0, e0, r2, c) && !EdgeTouchSweepB(p, v2, v1, r2, c) &&
                        !EdgeTouchSweepB(p, v0, v2, r2, c))
                        continue;
                    if (!(best > t))
                        continue;
                    best = t;
                    s_contact = c;
                    hit = true;
                }
            }
            if (hit && (q.flags & I3DCOL_FIRST_FACE))
                goto done;
        }
    }
done:
    level->UnlockVertices();
    if (!(best < 1e16f))
        return;
    if (!worldSpace) {
        // (A billboard's contact goes through the inverse of its matrix again: the original's.)
        s_contact = s_contact * (type == FourCC("BBRD") ? inv : vis->GetMatrix());
    }
    const float d2 = (s_contact - q.from).Square();
    if (q.hitCb) {
        q.cbResult = q.hitCb(&s_noNormal, std::sqrt(d2), vis, q.user);
        if (q.cbResult >= 2)
            return;
    }
    if (!(q.bestDist2 > d2))
        return;
    q.bestDist2 = d2;
    q.result = S_vector4{s_contact.x, s_contact.y, s_contact.z, 0.0f};
    q.hitFrame = vis;
}

// Whether the moving sphere's line passes within its radius of a bound sphere (0x10014dc0 and
// SphereWalk's inline tests; the start guard can never hold: the original's).
bool SphereNear(const S_vector &center, float radius, bool zxyDot)
{
    const float rr = q.radius + radius;
    const S_vector e(center.x - q.from.x, center.y - q.from.y, center.z - q.from.z);
    const float t = zxyDot ? (q.dir.z * e.z + q.dir.x * e.x) + q.dir.y * e.y : e.Dot(q.dir);
    if (t < -rr && q.length + rr < t)
        return false;
    const S_vector p(q.from.x + q.dir.x * t, q.from.y + q.dir.y * t, q.dir.z * t + q.from.z);
    const S_vector d(p.x - center.x, p.y - center.y, p.z - center.z);
    return zxyDot ? SquareZYX(d) < rr * rr : d.Square() < rr * rr;
}

bool SphereTestable(I3D_visual *vis)
{
    switch (vis->GetVisualType()) {
    case FourCC("LMAP"):
    case FourCC("BBRD"):
    case FourCC("MRPH"):
    case FourCC("SNGM"):
    case FourCC("SMMR"):
    case FourCC("MIRR"):
    case FourCC("OBJ_"):
        return true;
    default:
        return false;
    }
}

// 0x100119a0: a frame and its children against the moving sphere.
void SphereWalk(I3D_frame *f)
{
    if (f == q.skipped)
        return;
    if (!(f->frameFlags & FRMFLAGS_ON) && !(q.flags & I3DCOL_HIDDEN_TOO))
        return;
    const I3D_bound_volume &b = f->GetBound();
    if (!(b.radius > 0.0f)) {
        if (f->type != FRAME_SECTOR)
            return;
    } else if (!SphereNear(b.center, b.radius, false)) {
        return;
    }
    if (f->type == FRAME_VISUAL && f != q.excluded) {
        auto *vis = static_cast<I3D_visual *>(f);
        const I3D_bbox &wb = vis->GetWorldBound();
        if (q.sweepMin.x <= wb.max.x && wb.min.x <= q.sweepMax.x && q.sweepMin.y <= wb.max.y &&
            wb.min.y <= q.sweepMax.y && q.sweepMin.z <= wb.max.z && wb.min.z <= q.sweepMax.z) {
            const float rr = q.radius + vis->worldBound.radius;
            const S_vector &c = vis->worldBound.center;
            const S_vector e(c.x - q.from.x, c.y - q.from.y, c.z - q.from.z);
            const float t = e.Dot(q.dir);
            // (Never false: the original's.)
            if (!(t < -rr) || !(t > q.length + rr)) {
                const S_vector p(q.dir.x * t, q.dir.y * t, q.dir.z * t);
                const S_vector p2(q.from.x + p.x, p.y + q.from.y, p.z + q.from.z);
                const S_vector d(p2.x - c.x, p2.y - c.y, p2.z - c.z);
                if (rr * rr > d.Square() && (!(vis->visualFlags & 0x1000) || !(q.flags & I3DCOL_SKIP_FLAGGED)) &&
                    SphereTestable(vis)) {
                    q.cbResult = 0;
                    SphereVisual(vis);
                    if (q.cbResult == 3)
                        return;
                }
            }
        }
    }
    for (I3D_frame *c = f->firstChild; c && !q.done && q.cbResult != 1; c = c->nextSibling)
        SphereWalk(c);
}

// The start shared by the sphere queries.
void BeginSphere(I3D_scene *scene, const S_vector &from, const S_vector &dir, float radius, uint32_t flags,
                 I3D_COLLISION_HIT *hitCb)
{
    q.from = from;
    q.dir = col::NormalizedDir(dir);
    q.length2 = SquareZYX(dir);
    q.length = std::sqrt(q.length2);
    q.radius = radius;
    q.hitFrame = nullptr;
    q.bestDist2 = 1e16f;
    q.flags = flags;
    q.hitCb = hitCb;
    q.cbResult = 0;
    q.user = nullptr;
    q.scene = scene;
    // (Fix: the original keeps the previous query's frame; its levels stay.)
    q.lodFrame = nullptr;
}

// The box of a sweep along the direction only (slots 0x10C and 0x110): from the start backed off
// by the radius to the end pushed on by it.
void AxialSweepBox(float radius)
{
    const S_vector d(q.dir.x * radius, q.dir.y * radius, q.dir.z * radius);
    q.sweepMin = S_vector(q.from.x - d.x, q.from.y - d.y, q.from.z - d.z);
    q.sweepMax = q.sweepMin;
    const S_vector farEnd(d.x + q.to.x, d.y + q.to.y, d.z + q.to.z);
    I3D_bbox box;
    box.min = q.sweepMin;
    box.max = q.sweepMax;
    box.Expand(farEnd);
    q.sweepMin = box.min;
    q.sweepMax = box.max;
}

void SetSphereFrame(uint32_t flags, I3D_frame *frame)
{
    if (flags & I3DCOL_SKIP_FRAME) {
        q.skipped = frame;
        // (Fix: the original leaves the excluded visual of the previous query.)
        q.excluded = nullptr;
    } else {
        q.excluded = frame;
        q.skipped = nullptr;
    }
}

}  // namespace

// 0x100115a0
I3D_frame *I3D_scene::MoveSphere(const S_vector &from, const S_vector &dir, float radius, uint32_t flags,
                                 S_vector4 *outPoint, I3D_frame *frame, I3D_COLLISION_HIT *hitCb)
{
    q.done = false;
    BeginSphere(this, from, dir, radius, flags, hitCb);
    q.to = S_vector(dir.x + from.x, from.y + dir.y, from.z + dir.z);
    for (int i = 0; i < 3; ++i) {
        const float a = (&from.x)[i], b = (&q.to.x)[i];
        float lo = a, hi = a;
        if (b < a)
            lo = b;
        if (a < b)
            hi = b;
        (&q.sweepMin.x)[i] = lo - radius;
        (&q.sweepMax.x)[i] = hi + radius;
    }
    SetSphereFrame(flags, frame);
    SphereWalk(primarySector);
    if (flags & I3DCOL_OMNI_SECTOR)
        SphereWalk(omniSector);
    if (flags & I3DCOL_NEAR_SECTOR)
        SphereWalk(nearSector);
    if (q.hitFrame && outPoint)
        *outPoint = q.result;
    return q.hitFrame;
}

// 0x100145e0
I3D_frame *I3D_scene::MoveSphereFrame(const S_vector &from, const S_vector &dir, float radius, I3D_frame *root,
                                      uint32_t flags, S_vector4 *outPoint, I3D_frame *frame,
                                      I3D_COLLISION_HIT *hitCb)
{
    q.done = false;
    BeginSphere(this, from, dir, radius, flags, hitCb);
    q.to = S_vector(from.x + dir.x, from.y + dir.y, from.z + dir.z);
    AxialSweepBox(radius);
    SetSphereFrame(flags, frame);
    SphereWalk(root);
    if (q.hitFrame && outPoint)
        *outPoint = q.result;
    return q.hitFrame;
}

// 0x10014940
I3D_frame *I3D_scene::MoveSphereVisual(const S_vector &from, const S_vector &dir, float radius, I3D_frame *visual,
                                       uint32_t flags, S_vector4 *outPoint)
{
    BeginSphere(this, from, dir, radius, flags, nullptr);
    q.to = S_vector(from.x + dir.x, from.y + dir.y, from.z + dir.z);
    AxialSweepBox(radius);
    if (!(visual->frameFlags & FRMFLAGS_ON) && !(flags & I3DCOL_HIDDEN_TOO))
        return nullptr;
    auto *vis = static_cast<I3D_visual *>(visual);
    const I3D_bbox &wb = vis->GetWorldBound();
    if (q.sweepMin.x <= wb.max.x && wb.min.x <= q.sweepMax.x && q.sweepMin.y <= wb.max.y &&
        wb.min.y <= q.sweepMax.y && q.sweepMin.z <= wb.max.z && wb.min.z <= q.sweepMax.z &&
        SphereNear(vis->worldBound.center, vis->worldBound.radius, true) &&
        (!(vis->visualFlags & 0x1000) || !(flags & I3DCOL_SKIP_FLAGGED))) {
        if (!SphereTestable(vis))
            return nullptr;
        SphereVisual(vis);
    }
    if (q.hitFrame && outPoint)
        *outPoint = q.result;
    return q.hitFrame;
}

// 0x10014ed0
float I3D_scene::SweepSpheres(float radiusA, const S_vector &fromA, const S_vector &toA, float radiusB,
                              const S_vector &fromB, const S_vector &toB)
{
    const S_vector va(toA.x - fromA.x, toA.y - fromA.y, toA.z - fromA.z);
    const S_vector vb(toB.x - fromB.x, toB.y - fromB.y, toB.z - fromB.z);
    const S_vector d(fromB.x - fromA.x, fromB.y - fromA.y, fromB.z - fromA.z);
    const S_vector v(vb.x - va.x, vb.y - va.y, vb.z - va.z);
    const float rr = radiusA + radiusB;
    const float a = SquareZYX(v);
    const float r2 = rr * rr;
    // (A relative motion shorter than the radii counts as a hit at once: the original's.)
    if (a <= r2)
        return 0.0f;
    float b = (v.z * d.z + v.y * d.y) + v.x * d.x;
    b = b + b;
    const float disc = b * b - ((SquareZYX(d) - r2) * a) * 4.0f;
    if (disc < 0.0f)
        return -1.0f;
    const float root = std::sqrt(disc);
    const float inv = 0.5f / a;
    float t = (root - b) * inv;
    const float t2 = (-b - root) * inv;
    if (!(t <= t2))
        t = t2;
    if (!(t <= 1.0f) || t < 0.0f)
        return -1.0f;
    return t;
}
