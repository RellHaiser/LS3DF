// The scene's ray queries (I3D_scene slots 0xFC - 0x104; 0x1000c7a0 - 0x10011530) and the face
// filters (0x10010a90, 0x10064860). (0x10011540 - 0x10011590 are the static initialisers of the
// collision code's vectors.)
//
// A ray walks the sector trees: a frame whose hierarchy box the ray's line misses is skipped with
// its children; a visual whose sphere the ray misses is not tested. A visual's test runs in its
// space: the ray against its local box, then against the faces of its mesh level (Moller-Trumbore;
// one-sided unless the material or the query says two), mirrors against their faces, skinned
// meshes against their joints' boxes. The nearest hit is kept by its squared world distance.
#include "i3d/collision_state.h"

#include <cmath>

#include "core/fpu.h"
#include "i3d/billboard.h"
#include "i3d/joint.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/mirror.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/singlemesh.h"
#include "i3d/vb_manager.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ls3d::i3d::col::q;

namespace ls3d::i3d::col {

S_query q;

S_vector NormalizedDir(const S_vector &d)
{
    S_vector n = d;
    NormalizeWithSquare(n, SquareZYX(d));
    return n;
}

}  // namespace ls3d::i3d::col

namespace {

const S_matrix &Identity()
{
    static const S_matrix s_identity = [] {
        S_matrix m;
        m.Identity();
        return m;
    }();
    return s_identity;
}

// What a filter's first argument points to (the original passes an unset local).
const S_vector s_filterReserved(0.0f, 0.0f, 0.0f);

// An all-hits query's color goes black at an opaque face, and the filtering ends.
void BlockColor()
{
    if (q.flags & I3DCOL_ALL_HITS) {
        // (The original also writes an unset w.)
        if (auto *c = static_cast<S_vector *>(q.user))
            *c = S_vector(0.0f, 0.0f, 0.0f);
        q.filter = nullptr;
    }
}

// 0x10010350 (also inlined by RayVisual): the parameter where the ray (from `o` along `d`) enters
// the box, the axis of the face in `normal`; 0 and z when it starts inside, 1e17 when it misses.
float RayBox(const S_vector &min, const S_vector &max, const S_vector &o, const S_vector &d, S_vector &normal)
{
    auto in = [](float v, float lo, float hi) { return !(v < lo) && v <= hi; };
    if (in(o.x, min.x, max.x) && in(o.y, min.y, max.y) && in(o.z, min.z, max.z)) {
        normal = S_vector(0.0f, 0.0f, 1.0f);
        return 0.0f;
    }
    float best = 1e17f;
    bool first = true;
    // A face at `plane` on `axis` (0 x, 1 y, 2 z) with the other two axes checked.
    auto face = [&](int axis, float plane, int a, int b) {
        const float da = (&d.x)[axis];
        if (da == 0.0f)
            return;
        const float t = (plane - (&o.x)[axis]) / da;
        if (t < 0.0f)
            return;
        const float pa = t * (&d.x)[a] + (&o.x)[a];
        const float pb = t * (&d.x)[b] + (&o.x)[b];
        if (!in(pa, (&min.x)[a], (&max.x)[a]) || !in(pb, (&min.x)[b], (&max.x)[b]))
            return;
        if (!first && !(best > t))
            return;
        best = t;
        normal = S_vector(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
    };
    face(2, min.z, 0, 1);
    first = false;
    face(1, min.y, 2, 0);
    face(0, min.x, 2, 1);
    face(0, max.x, 2, 1);
    face(1, max.y, 2, 0);
    face(2, max.z, 0, 1);
    return best;
}

// A ray against a triangle (v0, v1, v2) from `o` along `d`: one-sided (the faces' winding seen from
// the ray, determinant at least 1e-6) or two-sided (the inverse determinant in `inv`); false when
// it misses, else the parameter in `t`.
bool RayTriangleOneSided(const S_vector &v0, const S_vector &v1, const S_vector &v2, const S_vector &o,
                         const S_vector &d, float &t)
{
    const S_vector e1(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
    const S_vector e2(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z);
    const float px = e2.z * d.y - e2.y * d.z;
    const float py = e2.x * d.z - d.x * e2.z;
    const float pz = d.x * e2.y - e2.x * d.y;
    const float det = (px * e1.x + pz * e1.z) + py * e1.y;
    if (det < 1e-6)
        return false;
    const S_vector tv(o.x - v0.x, o.y - v0.y, o.z - v0.z);
    const float u = (tv.x * px + tv.z * pz) + tv.y * py;
    if (u < 0.0 || u > det)
        return false;
    const float qx = tv.y * e1.z - tv.z * e1.y;
    const float qy = e1.x * tv.z - tv.x * e1.z;
    const float qz = tv.x * e1.y - e1.x * tv.y;
    const float v = (qx * d.x + qy * d.y) + qz * d.z;
    if (v < 0.0 || u + v > det)
        return false;
    const float tt = (qx * e2.x + qz * e2.z) + qy * e2.y;
    if (tt < 0.0f)
        return false;
    t = tt * (1.0f / det);
    return true;
}
bool RayTriangleTwoSided(const S_vector &v0, const S_vector &v1, const S_vector &v2, const S_vector &o,
                         const S_vector &d, float &t, float &inv)
{
    const S_vector e1(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z);
    const S_vector e2(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z);
    const float px = e2.z * d.y - e2.y * d.z;
    const float py = d.z * e2.x - d.x * e2.z;
    const float pz = d.x * e2.y - d.y * e2.x;
    const float det = (e1.z * pz + py * e1.y) + px * e1.x;
    if (det > -1e-6 && det < 1e-6)
        return false;
    inv = 1.0f / det;
    const S_vector tv(o.x - v0.x, o.y - v0.y, o.z - v0.z);
    const float u = ((tv.z * pz + tv.y * py) + tv.x * px) * inv;
    if (u < 0.0 || u > 1.0)
        return false;
    const float qx = tv.y * e1.z - tv.z * e1.y;
    const float qy = tv.z * e1.x - e1.z * tv.x;
    const float qz = e1.y * tv.x - tv.y * e1.x;
    const float v = ((qy * d.y + qz * d.z) + d.x * qx) * inv;
    if (v < 0.0 || u + v > 1.0)
        return false;
    const float tt = ((qz * e2.z + qy * e2.y) + qx * e2.x) * inv;
    if (tt < 0.0f)
        return false;
    t = tt;
    return true;
}

// The hit's offset from the ray's start (o + d t - o, as the original rounds it).
S_vector HitOffset(const S_vector &o, const S_vector &d, float t)
{
    const float hx = d.x * t + o.x;
    const float hy = d.y * t + o.y;
    const float hz = d.z * t + o.z;
    return S_vector(hx - o.x, hy - o.y, hz - o.z);
}

// The face normal kept for a hit: (v2 - v1) x (v0 - v1), both scaled by 10000.
S_vector FaceNormal(const S_vector &v0, const S_vector &v1, const S_vector &v2)
{
    const S_vector a((v2.x - v1.x) * 10000.0f, (v2.y - v1.y) * 10000.0f, (v2.z - v1.z) * 10000.0f);
    const S_vector b((v0.x - v1.x) * 10000.0f, (v0.y - v1.y) * 10000.0f, (v0.z - v1.z) * 10000.0f);
    return S_vector(b.z * a.y - b.y * a.z, b.x * a.z - a.x * b.z, a.x * b.y - b.x * a.y);
}

// A material the filters need not see (it blocks): none, or fully opaque without alpha.
bool OpaqueMaterial(const I3D_material *m)
{
    if (!m)
        return true;
    const bool unitOpacity = IsTinyBits(std::fabs(m->opacity - 1.0f));
    if ((!m->texture || !m->texture->pixels) && unitOpacity)
        return true;
    return m->texture && m->matFlags <= 0x10000000u && unitOpacity;
}

// The texture coordinates of a hit at `hit` (local space) on the face (v0, v1, v2) with the
// coordinates uv0 - uv2: through the line from v0 over the hit to the opposite edge; false when
// the construction degenerates.
bool HitTexel(const S_vector &v0, const S_vector &v1, const S_vector &v2, const float *uv0, const float *uv1,
              const float *uv2, const S_vector &dirA, float &u, float &v)
{
    const S_vector dirB(v2.x - v1.x, v2.y - v1.y, v2.z - v1.z);
    float s1 = 0.0f, s2 = 0.0f;
    LineLineIntersect(v0, dirA, v1, dirB, s1, s2);
    if (IsTinyBits(std::fabs(s1)))
        return false;
    const S_vector p(s1 * dirA.x + v0.x, s1 * dirA.y + v0.y, s1 * dirA.z + v0.z);
    const float sumB = (dirB.z + dirB.y) + dirB.x;
    if (IsTinyBits(std::fabs(sumB)))
        return false;
    // (The edge parameter from the components' sums: the original's.)
    const float s = (((p.z - v1.z) + (p.y - v1.y)) + (p.x - v1.x)) / sumB;
    const float eu = (uv2[0] - uv1[0]) * s + uv1[0];
    const float ev = (uv2[1] - uv1[1]) * s + uv1[1];
    const float inv = 1.0f / s1;
    u = (eu - uv0[0]) * inv + uv0[0];
    v = (ev - uv0[1]) * inv + uv0[1];
    u = static_cast<float>(std::fmod(static_cast<double>(u), 1.0));
    v = static_cast<float>(std::fmod(static_cast<double>(v), 1.0));
    if (u < 0.0f)
        u = u + 1.0f;
    if (v < 0.0f)
        v = v + 1.0f;
    return true;
}

// The facing matrix of a billboard shown in the camera's orientation (modes 2 and 3) as the ray
// sees it: looking back along the ray, rows 1 and 2 swapped and scaled, at the frame's position.
S_matrix RayFacing(I3D_billboard *bb, const S_vector &scale)
{
    S_matrix m;
    m.Identity();
    m.SetDir3(S_vector(-q.dir.x, -q.dir.y, -q.dir.z), S_vector(0.0f, 1.0f, 0.0f));
    const S_vector r1 = m(1), r2 = m(2);
    m(0) = S_vector(scale.x * m.m[0][0], scale.x * m.m[0][1], scale.x * m.m[0][2]);
    // (The original copies the rows' w from an unset local; Inverse and the rotations ignore it.)
    m(1) = S_vector(scale.y * r2.x, scale.y * r2.y, scale.y * r2.z);
    m(2) = S_vector(r1.x * scale.z, r1.y * scale.z, r1.z * scale.z);
    const S_matrix &w = bb->GetMatrix();
    m(3) = w(3);
    m.m[3][3] = w.m[3][3];
    return m;
}

}  // namespace

S_matrix col::QueryFacing(I3D_billboard *bb)
{
    if (bb->mode == BBRDMODE_SCREEN_SCALED)
        return RayFacing(bb, bb->parent ? bb->GetMatrix().GetScale() : bb->scale);
    if (bb->mode == BBRDMODE_SCREEN)
        return RayFacing(bb, bb->scale);
    S_matrix m;
    bb->ComputeMatrix(q.from, m);
    return m;
}

namespace {

// 0x1000d050: a visual against the ray.
void RayVisual(I3D_visual *vis)
{
    const uint32_t type = vis->GetVisualType();
    const bool worldSpace = (vis->visualFlags & VISF_WORLD_SPACE) != 0;
    S_vector lfrom, ldir;
    S_matrix facing;
    const I3D_bbox *box;
    if (worldSpace) {
        ldir = q.dir;
        lfrom = q.from;
        vis->GetWorldBound();
        box = &vis->worldBound.bbox;
    } else {
        S_matrix inv;
        if (type == FourCC("BBRD")) {
            facing = col::QueryFacing(static_cast<I3D_billboard *>(vis));
            inv.Inverse(facing);
        } else {
            if (!(vis->visualFlags & VISF_INVMATRIX_VALID)) {
                vis->visualFlags |= VISF_INVMATRIX_VALID;
                vis->invWorldMatrix.Inverse(vis->GetMatrix());
            }
            inv = vis->invWorldMatrix;
        }
        lfrom = q.from * inv;
        ldir = q.dir.RotateByMatrix(inv);
        box = &vis->GetLocalBound().bbox;
    }

    // (Unset in the original: an all-hits unlimited query gives a box the ray misses to the hit
    // callback with it.)
    S_vector normal(0.0f, 0.0f, 0.0f);
    const float t = RayBox(box->min, box->max, lfrom, ldir, normal);
    float d2 = t * t;
    if (worldSpace)
        d2 = SquareXYZ(ldir) * d2;
    if (!(q.flags & I3DCOL_UNLIMITED) && q.length2 < d2)
        return;
    if (!(q.flags & I3DCOL_ALL_HITS) && q.bestDist2 < d2)
        return;

    // Takes a box hit (the skinned meshes' too) as the query's nearest.
    auto takeBoxHit = [&](float dist2) {
        q.bestDist2 = dist2;
        q.hitFrame = vis;
        q.hitFace[0] = q.hitFace[1] = q.hitFace[2] = 0;
        q.hitGroup = nullptr;
        q.normal = worldSpace ? normal : normal.RotateByMatrix(vis->GetMatrix());
    };
    if (q.flags & I3DCOL_BOX_ONLY) {
        if (q.hitCb) {
            q.cbResult = q.hitCb(&normal, std::sqrt(d2), vis, q.user);
            if (q.cbResult >= 2)
                return;
        }
        if (!(q.bestDist2 > d2))
            return;
        takeBoxHit(d2);
        return;
    }

    // The faces' hits are measured in world space through the matrix the visual is drawn with.
    // (Fix: a billboard in world space has no facing matrix made; the original reads it unset.)
    const S_matrix &drawMatrix = (type == FourCC("BBRD") && !worldSpace) ? facing
                                 : worldSpace                    ? Identity()
                                                                 : vis->GetMatrix();
    float best = 1e16f;
    bool front = true;
    const I3D_triface *hitFace = nullptr;
    I3D_face_group *hitGroup = nullptr;
    auto firstHit = [&] {
        q.hitFrame = vis;
        q.done = true;
    };
    // The visual's nearest face as the query's.
    auto finish = [&](bool frontFacing) {
        if (!(best < 1e16))
            return;
        if (!(q.flags & I3DCOL_UNLIMITED) && q.length2 < best)
            return;
        if (q.hitCb) {
            const S_vector wn = normal.RotateByMatrix(drawMatrix);
            q.cbResult = q.hitCb(&wn, std::sqrt(best), vis, q.user);
            if (q.cbResult >= 2)
                return;
        }
        if (!(q.bestDist2 > best))
            return;
        // (A face hit from behind loses to one in front at the same distance.)
        q.bestDist2 = frontFacing ? best : best + 0.001f;
        q.hitFrame = vis;
        q.normal = normal;
        q.hitFace[0] = hitFace->v[0];
        q.hitFace[1] = hitFace->v[1];
        q.hitFace[2] = hitFace->v[2];
        q.hitGroup = hitGroup;
    };

    if (type == FourCC("MIRR")) {
        auto *mirror = static_cast<I3D_mirror *>(vis);
        if (!mirror->numVertices || !mirror->numFaces || !mirror->vertices)
            return;
        const I3D_triface *f = mirror->LockFaces(0);
        const S_vector4 *vtx = mirror->vertices;
        for (int n = mirror->numFaces; n > 0; --n, ++f) {
            auto at = [vtx](uint16_t i) -> const S_vector & { return reinterpret_cast<const S_vector &>(vtx[i]); };
            const S_vector &v0 = at(f->v[0]), &v1 = at(f->v[1]), &v2 = at(f->v[2]);
            float tt;
            if (!RayTriangleOneSided(v0, v1, v2, lfrom, ldir, tt))
                continue;
            const S_vector r = HitOffset(lfrom, ldir, tt).RotateByMatrix(drawMatrix);
            const float dist2 = SquareXYZ(r);
            if (!(q.flags & I3DCOL_UNLIMITED) && q.length2 < dist2)
                continue;
            if (q.filter)
                BlockColor();
            if (q.flags & I3DCOL_FIRST_HIT) {
                firstHit();
                return;
            }
            if (!(best > dist2))
                continue;
            best = dist2;
            if (q.normalOut || q.hitCb)
                normal = FaceNormal(v0, v1, v2);
            hitFace = f;
            if (q.flags & I3DCOL_FIRST_FACE)
                break;
        }
        hitGroup = nullptr;
        finish(true);
        return;
    }

    auto *obj = static_cast<I3D_object *>(vis);
    if (!obj->mesh)
        return;
    const int lod = vis == q.lodFrame ? q.frameLodLevel : q.lodLevel;
    I3D_mesh_level *level = obj->mesh->levels[lod];
    if (!level)
        return;

    if (type == FourCC("SNGM") || type == FourCC("SMMR")) {
        // Skinned meshes: the bind pose's box, then each joint's box in the joint's space.
        if (q.flags & I3DCOL_NO_JOINTS)
            return;
        auto *sm = static_cast<I3D_singlemesh *>(vis);
        float bestT = 1e17f;
        if (const I3D_bbox *b = sm->GetJointBBox(-1))
            bestT = RayBox(b->min, b->max, lfrom, ldir, normal);
        for (int i = 0; i < sm->numJoints && sm->joints[i]; ++i) {
            const I3D_bbox *jb = sm->GetJointBBox(i);
            if (!jb)
                continue;
            S_matrix inv;
            inv.Inverse(sm->joints[i]->GetMatrix());
            const S_vector jf = q.from * inv;
            S_vector jd;
            jd.SetNormalized(q.dir.RotateByMatrix(inv));
            S_vector jn(0.0f, 0.0f, 0.0f);
            const float d = RayBox(jb->min, jb->max, jf, jd, jn);
            if (!(bestT <= d)) {
                bestT = d;
                normal = jn;
            }
        }
        if (bestT < 1e16f) {
            BlockColor();
            if (q.flags & I3DCOL_FIRST_HIT) {
                firstHit();
                return;
            }
        }
        float dist2 = bestT * bestT;
        if (worldSpace)
            dist2 = SquareXYZ(ldir) * dist2;
        if (!(q.flags & I3DCOL_UNLIMITED) && q.length2 < dist2)
            return;
        if (!(q.flags & I3DCOL_ALL_HITS) && q.bestDist2 < dist2)
            return;
        if (q.hitCb) {
            q.cbResult = q.hitCb(&normal, std::sqrt(dist2), vis, q.user);
            if (q.cbResult >= 2)
                return;
        }
        if (!(q.bestDist2 > dist2))
            return;
        takeBoxHit(dist2);
        return;
    }

    const auto *vtx = static_cast<const uint8_t *>(level->LockVertices(1));
    if (!vtx)
        return;
    auto vertex = [vtx](uint16_t i) -> const S_vector & { return *reinterpret_cast<const S_vector *>(vtx + i * 0x20); };
    for (I3D_face_group *fg = level->faceGroups; fg; fg = fg->next) {
        const bool twoSided =
            (q.flags & I3DCOL_TWO_SIDED) || (fg->material && (fg->material->matFlags & MATF_TWO_SIDED));
        const I3D_triface *f = fg->Lock(1);
        for (int n = fg->numFaces; n > 0; --n, ++f) {
            const S_vector &v0 = vertex(f->v[0]), &v1 = vertex(f->v[1]), &v2 = vertex(f->v[2]);
            float tt, inv = 1.0f;
            if (twoSided ? !RayTriangleTwoSided(v0, v1, v2, lfrom, ldir, tt, inv)
                         : !RayTriangleOneSided(v0, v1, v2, lfrom, ldir, tt))
                continue;
            const S_vector r = HitOffset(lfrom, ldir, tt).RotateByMatrix(drawMatrix);
            const float dist2 = SquareYZX(r);
            if (!(q.flags & I3DCOL_UNLIMITED) && q.length2 < dist2)
                continue;
            if (q.filter) {
                I3D_material *m = fg->material;
                int answer = 1;
                if (OpaqueMaterial(m)) {
                    answer = 0;
                } else {
                    S_collisionTexel texel = {0.0f, 0.0f, m};
                    bool call = true;
                    if (m->texture) {
                        // The texture coordinates from the level's vertices (0x20 bytes: uv at +0x18).
                        C_VertexBuffer *vb = level->vertices.buffer;
                        const uint8_t *base = vb->Lock(D3DLOCK_READONLY) + vb->stride * level->vertices.offset;
                        auto uv = [base](uint16_t i) { return reinterpret_cast<const float *>(base + i * 0x20 + 0x18); };
                        const S_vector hit(ldir.x * tt + lfrom.x, ldir.y * tt + lfrom.y, ldir.z * tt + lfrom.z);
                        const S_vector dirA(hit.x - v0.x, hit.y - v0.y, hit.z - v0.z);
                        call = HitTexel(v0, v1, v2, uv(f->v[0]), uv(f->v[1]), uv(f->v[2]), dirA, texel.u, texel.v);
                    }
                    if (call) {
                        answer = q.filter(&s_filterReserved, tt, vis, q.user, &texel);
                        if (answer == 2)
                            continue;
                    }
                }
                if (answer == 0) {
                    BlockColor();
                    if (q.flags & I3DCOL_FIRST_HIT) {
                        firstHit();
                        return;
                    }
                }
            } else if (q.flags & I3DCOL_FIRST_HIT) {
                firstHit();
                return;
            }
            if (!(best > dist2))
                continue;
            if (inv < 0.0f)
                front = false;
            best = dist2;
            if (q.normalOut || q.hitCb) {
                normal = FaceNormal(v0, v1, v2);
                if (inv < 0.0f && fg->material && (fg->material->matFlags & MATF_TWO_SIDED))
                    normal = S_vector(-normal.x, -normal.y, -normal.z);
            }
            hitFace = f;
            hitGroup = fg;
            if (q.flags & I3DCOL_FIRST_FACE)
                goto done;
        }
    }
done:
    finish(front);
}

// 0x1000cc50: a frame and its children against the ray.
void RayWalk(I3D_frame *f)
{
    if (f == q.skipped)
        return;
    if (!(f->frameFlags & FRMFLAGS_ON) && !(q.flags & I3DCOL_HIDDEN_TOO))
        return;
    const I3D_bbox &b = f->GetBound().bbox;
    if (!(b.min.x <= b.max.x)) {
        // An empty hierarchy: only sectors are walked on.
        if (f->type != FRAME_SECTOR)
            return;
    } else {
        const S_vector &o = q.from;
        const bool inside = !(o.x < b.min.x) && o.x <= b.max.x && !(o.y < b.min.y) && o.y <= b.max.y &&
                            !(o.z < b.min.z) && o.z <= b.max.z;
        if (!inside) {
            // The ray's line against the box (the cross product axes).
            const S_vector half = S_vector(b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z) * 0.5f;
            const S_vector center = half + b.min;
            const S_vector e = center - o;
            const float t = e.Dot(q.dir);
            const S_vector p = o + q.dir * t;
            const S_vector c = center - p;
            const S_vector &d = q.dir;
            const float ay = std::fabs(d.y), az = std::fabs(d.z), ax = std::fabs(d.x);
            if (az * half.y + ay * half.z < std::fabs(d.z * c.y - d.y * c.z))
                return;
            if (az * half.x + ax * half.z < std::fabs(d.x * c.z - c.x * d.z))
                return;
            if (ay * half.x + ax * half.y < std::fabs(c.x * d.y - d.x * c.y))
                return;
        }
    }
    if (f->type == FRAME_VISUAL && f != q.excluded) {
        auto *vis = static_cast<I3D_visual *>(f);
        vis->GetWorldBound();
        const S_vector &center = vis->worldBound.center;
        const float r = vis->worldBound.radius;
        const S_vector e(center.x - q.from.x, center.y - q.from.y, center.z - q.from.z);
        const float t = e.Dot(q.dir);
        if (!(-r > t) && ((q.flags & I3DCOL_UNLIMITED) || !(q.length + r < t))) {
            const S_vector c = (q.from + q.dir * t) - center;
            if (!(c.Dot(c) >= r * r) && (!(vis->visualFlags & 0x1000) || !(q.flags & I3DCOL_SKIP_FLAGGED))) {
                switch (vis->GetVisualType()) {
                case FourCC("LMAP"):
                case FourCC("BBRD"):
                case FourCC("MRPH"):
                case FourCC("SNGM"):
                case FourCC("SMMR"):
                case FourCC("MIRR"):
                case FourCC("OBJ_"):
                    RayVisual(vis);
                    if (q.cbResult == 3)
                        return;
                    break;
                default:
                    break;
                }
            }
        }
    }
    for (I3D_frame *c = f->firstChild; c; c = c->nextSibling) {
        if (q.done || q.cbResult == 1)
            return;
        RayWalk(c);
    }
}

// The start shared by the ray queries.
void BeginRay(I3D_scene *scene, const S_vector &from, const S_vector &dir, uint32_t flags, I3D_COLLISION_HIT *hitCb,
              void *user, I3D_COLLISION_FILTER *filter)
{
    q.done = false;
    q.from = from;
    q.dir = col::NormalizedDir(dir);
    q.user = user;
    q.hitCb = hitCb;
    q.cbResult = 0;
    q.length2 = SquareZYX(dir);
    q.length = std::sqrt(q.length2);
    q.filter = (flags & I3DCOL_FILTER) ? (filter ? filter : AlphaFilter) : nullptr;
    q.flags = flags;
    q.scene = scene;
}

// The frame argument of a ray query: skipped (with its subtree), tested at its own level, or
// not tested itself.
void SetQueryFrame(uint32_t flags, I3D_frame *frame, int lodLevel)
{
    q.frameLodLevel = lodLevel;
    q.skipped = nullptr;
    q.excluded = nullptr;
    q.lodFrame = nullptr;
    q.lodLevel = lodLevel;
    if (flags & I3DCOL_SKIP_FRAME) {
        // (Fix: the original leaves the excluded visual of the previous query.)
        q.skipped = frame;
    } else if (flags & I3DCOL_LOD_FRAME) {
        q.lodLevel = 0;
        q.lodFrame = frame;
    } else {
        q.excluded = frame;
    }
}

void WalkSectors(I3D_scene *scene, uint32_t flags, void (*walk)(I3D_frame *))
{
    if (flags & I3DCOL_NEAR_SECTOR)
        walk(scene->nearSector);
    walk(scene->primarySector);
    if (flags & I3DCOL_OMNI_SECTOR)
        walk(scene->omniSector);
}

// The hit normal in world space, normalized (TestRay, PickFrame).
S_vector WorldHitNormal()
{
    I3D_frame *f = q.hitFrame;
    if (f->type != FRAME_VISUAL || !(static_cast<I3D_visual *>(f)->visualFlags & VISF_WORLD_SPACE))
        q.normal = q.normal.RotateByMatrix(f->GetMatrix());
    NormalizeWithSquare(q.normal, SquareYZX(q.normal));
    return q.normal;
}

}  // namespace

// I3D_scene ----------------------------------------------------------------------------------------

// 0x10010c20
I3D_frame *I3D_scene::PickFrame(const S_vector &from, const S_vector &dir, uint32_t flags, S_vector4 *outPoint,
                                S_vector4 *outNormal, I3D_COLLISION_HIT *hitCb, void *user,
                                I3D_COLLISION_FILTER *filter, int lodLevel)
{
    BeginRay(this, from, dir, flags, hitCb, user, filter);
    q.frameLodLevel = lodLevel;
    q.lodLevel = lodLevel;
    q.lodFrame = nullptr;
    q.excluded = nullptr;
    q.skipped = nullptr;
    q.normalOut = outNormal;
    q.hitFrame = nullptr;
    q.bestDist2 = 1e16f;
    WalkSectors(this, flags, RayWalk);
    if (q.hitFrame) {
        if (outPoint) {
            // (The original writes an unset w.)
            const float dist = std::sqrt(q.bestDist2);
            *outPoint = S_vector4{q.dir.x * dist + from.x, q.dir.y * dist + from.y, q.dir.z * dist + from.z, 0.0f};
        }
        if (outNormal) {
            const S_vector n = WorldHitNormal();
            *outNormal = S_vector4{n.x, n.y, n.z, 0.0f};
        }
    }
    return q.hitFrame;
}

// 0x1000c7a0
I3D_frame *I3D_scene::TestRay(const S_vector &from, const S_vector &dir, uint32_t flags, float *outDistance,
                              S_vector4 *outNormal, I3D_COLLISION_HIT *hitCb, void *user,
                              I3D_COLLISION_FILTER *filter, I3D_frame *frame, int lodLevel)
{
    BeginRay(this, from, dir, flags, hitCb, user, filter);
    SetQueryFrame(flags, frame, lodLevel);
    q.normalOut = outNormal;
    q.hitFrame = nullptr;
    q.bestDist2 = 1e16f;
    WalkSectors(this, flags, RayWalk);
    if (q.hitFrame) {
        if (outDistance)
            *outDistance = std::sqrt(q.bestDist2);
        if (outNormal) {
            const S_vector n = WorldHitNormal();
            *outNormal = S_vector4{n.x, n.y, n.z, 0.0f};
        }
    }
    return q.hitFrame;
}

// 0x100110f0
I3D_frame *I3D_scene::IntersectFrame(const S_vector &from, const S_vector &dir, I3D_frame *frame,
                                     I3D_collision_info *info, uint32_t flags, I3D_frame *queryFrame,
                                     int lodLevel, I3D_COLLISION_HIT *hitCb)
{
    if (!frame)
        return nullptr;
    // (The original takes a set I3DCOL_DONE_AT_ROOT as a query already done.)
    BeginRay(this, from, dir, flags, hitCb, nullptr, nullptr);
    q.filter = (flags & I3DCOL_FILTER) ? AlphaFilter : nullptr;
    q.done = (flags & I3DCOL_DONE_AT_ROOT) != 0;
    SetQueryFrame(flags, queryFrame, lodLevel);
    // The normal is always wanted (0x100ad9c0, a static vector).
    static S_vector4 s_normal;
    q.normalOut = &s_normal;
    q.hitFrame = nullptr;
    q.bestDist2 = 1e16f;
    q.hitGroup = nullptr;
    RayWalk(frame);
    if (!q.hitFrame)
        return nullptr;
    info->distance = std::sqrt(q.bestDist2);
    S_vector n = q.normal.RotateByMatrix(q.hitFrame->GetMatrix());
    NormalizeWithSquare(n, SquareXYZ(n));
    // (The original copies the rotation's unset w too.)
    info->normal = S_vector4{n.x, n.y, n.z, 0.0f};
    info->face[0] = static_cast<uint16_t>(q.hitFace[0]);
    info->face[1] = static_cast<uint16_t>(q.hitFace[1]);
    info->face[2] = static_cast<uint16_t>(q.hitFace[2]);
    info->faceGroup = q.hitGroup;
    return q.hitFrame;
}

// The face filters ------------------------------------------------------------------------------------

namespace {

// The texel at a hit: the pixel copy's 32-bit BGRA (or its 1-bit mask, then white) at the
// coordinates rounded to the nearest texel.
struct S_texelSample {
    bool pixels;                            // the texture has a pixel copy
    float r, g, b;                          // (the mask: 1)
    uint8_t alpha;                          // the alpha byte (the mask: 0 / 255)
    const uint8_t *bgra;                    // the 32-bit texel (null for the mask)
};
S_texelSample SampleTexel(const S_collisionTexel *texel)
{
    S_texelSample s = {false, 1.0f, 1.0f, 1.0f, 0, nullptr};
    const ITexture *tex = texel->material->texture;
    if (!tex || !tex->pixels)
        return s;
    s.pixels = true;
    const int x = Fistp(static_cast<float>(static_cast<int>(tex->width - 1)) * texel->u);
    const int y = Fistp(static_cast<float>(static_cast<int>(tex->height - 1)) * texel->v);
    const int index = static_cast<int>(tex->width) * y + x;
    if (tex->flags & ITC_PIXEL_MASK) {
        s.alpha = (tex->pixels[index / 8] & (1 << (index % 8))) ? 0xff : 0;
    } else {
        s.bgra = tex->pixels + index * 4;
        s.alpha = s.bgra[3];
    }
    return s;
}

}  // namespace

namespace ls3d::i3d {

// 0x10010a90
int __stdcall AlphaFilter(const void *, float, I3D_frame *, void *, const S_collisionTexel *texel)
{
    const I3D_material *m = texel->material;
    const S_texelSample s = SampleTexel(texel);
    if (!s.pixels)
        return m->opacity < 0.1f ? 2 : 1;
    float a;
    if ((m->matFlags & MATF_ADDITIVE) && s.bgra) {
        // Additive materials: the texel's brightness.
        const float r = static_cast<float>(s.bgra[2]) * 0.00392f;
        const float g = static_cast<float>(s.bgra[1]) * 0.00392f;
        const float b = static_cast<float>(s.bgra[0]) * 0.00392f;
        a = (b * 0.1f + g * 0.6f) + r * 0.3f;
    } else {
        a = static_cast<float>(s.alpha) * 0.00392f;
    }
    return a * m->opacity < 0.1f ? 2 : 1;
}

// 0x10064860
int __stdcall TransmitFilter(const void *, float, I3D_frame *, void *user, const S_collisionTexel *texel)
{
    const I3D_material *m = texel->material;
    auto *color = static_cast<S_vector *>(user);
    const S_texelSample s = SampleTexel(texel);
    float a, cr, cg, cb;
    if (!s.pixels) {
        a = m->opacity;
        cr = m->diffuse.x;
        cg = m->diffuse.y;
        cb = m->diffuse.z;
    } else {
        cr = s.r;
        cg = s.g;
        cb = s.b;
        if (s.bgra) {
            cr = static_cast<float>(s.bgra[2]) * 0.00392f;
            cg = static_cast<float>(s.bgra[1]) * 0.00392f;
            cb = static_cast<float>(s.bgra[0]) * 0.00392f;
        }
        if ((m->matFlags & MATF_ADDITIVE) && s.bgra)
            a = (cb * 0.1f + cg * 0.6f) + cr * 0.3f;
        else
            a = static_cast<float>(s.alpha) * 0.00392f;
        a = a * m->opacity;
    }
    if (a < 0.1f)
        return 2;
    if (a > 0.95f)
        return 0;
    // The light passing: what the face lets through, tinted by its color.
    const float k = 1.0f - a;
    const float r = k * color->x, g = k * color->y, b = k * color->z;
    color->x = r + (cr * r) * a;
    color->y = g + (g * cg) * a;
    color->z = b + (b * cb) * a;
    return 1;
}

}  // namespace ls3d::i3d
