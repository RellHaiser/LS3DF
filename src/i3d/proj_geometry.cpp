// The projected geometry of projectors and shadows (0x10001000 - 0x10004c60).
#include "i3d/proj_geometry.h"

#include <cmath>
#include <cstring>
#include <utility>

#include "core/fpu.h"
#include "i3d/i3d.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/render_passes.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace ls3d::i3d {
// The driver's 1 / state 0x35 (driver_core.cpp): the z step perspective polygons are cut at.
extern float g_state35Inverse;
}

namespace {

// The outside flags of a vertex: beyond x -1 / 1, y -1 / 1 (perspective: -z / z), z 0 / 1.
enum : uint32_t {
    PCLIP_LEFT = 0x01,
    PCLIP_RIGHT = 0x02,
    PCLIP_BOTTOM = 0x04,
    PCLIP_TOP = 0x08,
    PCLIP_NEAR = 0x10,
    PCLIP_FAR = 0x20,
};

// A vertex of the mesh being projected, or one the clipping made.
struct S_projVertex {
    S_vector4 pos;                          // +0x00 volume space (w: copied along, unused)
    int outIndex;                           // +0x10 its index in the result, -1 none yet
    uint32_t clip;                          // +0x14 PCLIP_* (the low 16 bits are tested)
    uint32_t padding[2];
};
static_assert(sizeof(S_projVertex) == 0x20, "S_projVertex layout");

// The volume (Setup).
const S_matrix *s_projection;               // 0x100ad4b4
const S_matrix *s_world;                    // 0x100ad078 (never read)
bool s_orthogonal;                          // 0x100ad01c
float s_normalBias;                         // 0x100ad4a4
float s_edgeExpand;                         // 0x100ad030
float s_opacity;                            // 0x100ad074
float s_invOpacity;                         // 0x100ad070 1 / opacity (1e8 for none)
float s_scaleX;                             // 0x100ad4b0 the projection's x / z and y / z axis length ratios
float s_scaleY;                             // 0x100ad4ac
bool s_bothSides;                           // 0x100ad010 opacity above 0.9999: no face culled
uint32_t s_withBlended;                     // 0x100ad054 the current mesh's

// The current mesh's vertices and those the clipping adds (16-byte aligned).
S_projVertex *s_temp;                       // 0x100ad4b8
int s_tempCapacity;                         // 0x100ad4a8 (at most 65000)
int s_tempLimit;                            // 0x100ad018 the faces stop when this many exist
int s_numTemp;                              // 0x100ad8d8
S_projVertex *s_tempNext;                   // 0x100ad024

// The faces: one block, the blended list in its second half.
uint16_t *s_faces;                          // 0x100ad480
uint16_t *s_blendedFaces;                   // 0x100ad4bc
int s_faceCapacity;                         // 0x100ad028 faces per list (at most 32000)
int s_numFaces;                             // 0x100ad014
int s_numBlendedFaces;                      // 0x100ad4c4
uint16_t *s_faceOut;                        // 0x100ad034 where the current mesh's go
uint16_t *s_blendedFaceOut;                 // 0x100ad4c0
int s_newFaces;                             // 0x100ad4c8 the current mesh's
int s_newBlendedFaces;                      // 0x100ad020
uint32_t s_groupBlended;                    // 0x100ad02c the current face group's material blends
uint32_t s_twoSided;                        // 0x100ad4d0 ... is two-sided (no culling)

// The result's vertices (16-byte aligned).
S_vector4 *s_out;                           // 0x100ad050
int s_outCapacity;                          // 0x100ad484
int s_numOut;                               // 0x100ad4cc
S_vector s_blendedSum;                      // 0x100ad040
int s_blendedSumCount;                      // 0x100ad4d4

// The polygon being clipped, in two buffers the passes alternate between (0x100ad4d8 /
// 0x100ad080), and its vertex count (0x100ad4a0).
uint16_t s_polyA[512];
uint16_t s_polyB[512];
int s_polyCount;

S_projVertex &V(uint16_t index) { return s_temp[index]; }

// A new vertex at the end of the mesh's (index s_numTemp).
S_projVertex &NewVertex(uint16_t *&out)
{
    *out++ = static_cast<uint16_t>(s_numTemp);
    return *s_tempNext;
}
void EndVertex()
{
    s_tempNext->outIndex = -1;
    ++s_tempNext;
    ++s_numTemp;
}

// The x / y flags against -bound / bound.
uint32_t ClipXY(float x, float y, float bound)
{
    uint32_t c = x < -bound ? PCLIP_LEFT : x > bound ? PCLIP_RIGHT : 0;
    if (y < -bound)
        c |= PCLIP_BOTTOM;
    else if (y > bound)
        c |= PCLIP_TOP;
    return c;
}

// One Sutherland-Hodgman pass over the polygon `in` (`n` vertices, in[n] = in[0]) against the
// planes of `low` / `high` into `out`; `cut(p, q, high, out)` adds the crossing of edge p -> q.
// Returns the new count (out[count] = out[0]).
template <class Cut>
int ClipPass(const uint16_t *in, int n, uint16_t *out, uint32_t low, uint32_t high, Cut cut)
{
    uint16_t *o = out;
    for (int k = 0; k < n; ++k) {
        const S_projVertex &p = V(in[k]);
        const S_projVertex &q = V(in[k + 1]);
        if (!(p.clip & low)) {
            if (!(p.clip & high)) {
                *o++ = in[k];
                if (q.clip & low)
                    cut(p, q, false, o);
                else if (q.clip & high)
                    cut(p, q, true, o);
            } else if (q.clip & low) {
                cut(p, q, true, o);
                cut(p, q, false, o);
            } else if (!(q.clip & high)) {
                cut(p, q, true, o);
            }
        } else if (!(q.clip & low)) {
            cut(p, q, false, o);
            if (q.clip & high)
                cut(p, q, true, o);
        }
    }
    *o = out[0];
    return static_cast<int>(o - out);
}

// The flags of the polygon's vertices together (the low 16 bits).
uint32_t PolyClip(const uint16_t *poly, int n)
{
    if (n < 1)
        return 0;
    uint16_t c = static_cast<uint16_t>(V(poly[0]).clip);
    for (int k = 1; k < n; ++k)
        c |= static_cast<uint16_t>(V(poly[k]).clip);
    return c;
}

// The faces behind the volume's direction culled, unless two-sided: those facing away (z of the
// screen-space normal above 0) when the volume is (nearly) fully transparent, or when their
// normal is too close to the screen plane for the opacity.
bool Culled(const S_projVertex &v0, const S_projVertex &v1, const S_projVertex &v2)
{
    if (s_twoSided)
        return false;
    const float bx = v2.pos.x - v1.pos.x, by = v2.pos.y - v1.pos.y, bz = v2.pos.z - v1.pos.z;
    const float ax = v0.pos.x - v1.pos.x, ay = v0.pos.y - v1.pos.y, az = v0.pos.z - v1.pos.z;
    const float n0 = az * by - ay * bz;
    const float n1 = ax * bz - bx * az;
    const float n2 = bx * ay - ax * by;
    if (!(n2 > 0.0f))
        return false;
    if (IsTinyBits(s_opacity))
        return true;
    const float a = s_scaleX * n0;
    const float b = s_scaleY * n1;
    const float r = (s_invOpacity * n2) * n2;
    return (b * b + a * a) + n2 * n2 < r;
}

// The fan / zipper of a clipped polygon added; its new vertices (index from `firstNew`) moved by
// the bias along the face's normal and by the expansion away from the polygon's center.
void PushNewVertices(const uint16_t *poly, int n, const S_projVertex &v0, const S_projVertex &v1,
                     const S_projVertex &v2, int firstNew)
{
    const float ax = v0.pos.x - v1.pos.x, ay = v0.pos.y - v1.pos.y, az = v0.pos.z - v1.pos.z;
    const float bx = v2.pos.x - v1.pos.x, by = v2.pos.y - v1.pos.y, bz = v2.pos.z - v1.pos.z;
    float nx = ay * bz - by * az;
    float ny = az * bx - ax * bz;
    float nz = ax * by - bx * ay;
    const float sq = (nz * nz + ny * ny) + nx * nx;
    S_vector bias;
    if (!(sq < kEpsilon)) {
        const float s = s_normalBias / std::sqrt(sq);
        bias = S_vector(s * nx, s * ny, s * nz);
    } else {
        if (nx != 0.0f)
            nx = nx < 0.0f ? -1.0f : 1.0f;
        else if (nz != 0.0f)
            nz = nz < 0.0f ? -1.0f : 1.0f;
        else
            ny = ny < 0.0f ? -1.0f : 1.0f;
        bias = S_vector(nx * s_normalBias, ny * s_normalBias, nz * s_normalBias);
    }
    S_vector sum(V(poly[0]).pos.x, V(poly[0]).pos.y, V(poly[0]).pos.z);
    for (int k = 1; k < n; ++k) {
        const S_vector4 &p = V(poly[k]).pos;
        sum.x = sum.x + p.x;
        sum.y = sum.y + p.y;
        sum.z = sum.z + p.z;
    }
    const float inv = 1.0f / static_cast<float>(n);
    const S_vector center(sum.x * inv, sum.y * inv, sum.z * inv);
    for (int k = 0; k < n; ++k) {
        if (poly[k] < firstNew)
            continue;
        S_vector4 &p = V(poly[k]).pos;
        p.x = bias.x + p.x;
        p.y = bias.y + p.y;
        p.z = bias.z + p.z;
        if (!(s_edgeExpand > 0.0f))
            continue;
        float dx = p.x - center.x;
        float dy = p.y - center.y;
        float dz = p.z - center.z;
        const float dsq = (dx * dx + dz * dz) + dy * dy;
        if (!(dsq < kEpsilon)) {
            const float s = s_edgeExpand / std::sqrt(dsq);
            dx = s * dx;
            dy = dy * s;
            dz = s * dz;
        } else {
            // (The original sets no unit z when only z is non-zero.)
            if (dx != 0.0f)
                dx = dx < 0.0f ? -1.0f : 1.0f;
            else if (dz == 0.0f)
                dy = dy < 0.0f ? -1.0f : 1.0f;
            dx = dx * s_edgeExpand;
            dy = dy * s_edgeExpand;
            dz = dz * s_edgeExpand;
        }
        p.x = dx + p.x;
        p.y = dy + p.y;
        p.z = dz + p.z;
    }
}

// 0x10001b20: an orthogonal volume's faces, clipped to it, into the current list.
void ClipOrthogonal(const uint16_t *faces, int numFaces)
{
    const int firstNew = s_numTemp;
    uint16_t *outFaces = s_groupBlended ? s_blendedFaceOut : s_faceOut;
    auto cutZ = [](const S_projVertex &p, const S_projVertex &q, bool farPlane, uint16_t *&o) {
        S_projVertex &v = NewVertex(o);
        const float c = farPlane ? 1.0f : 0.0f;
        v.pos.z = c;
        const float t = (farPlane ? 1.0f - p.pos.z : -p.pos.z) / (q.pos.z - p.pos.z);
        v.pos.x = (q.pos.x - p.pos.x) * t + p.pos.x;
        v.pos.y = (q.pos.y - p.pos.y) * t + p.pos.y;
        v.clip = ClipXY(v.pos.x, v.pos.y, 1.0f);
        EndVertex();
    };
    auto cutX = [](const S_projVertex &p, const S_projVertex &q, bool right, uint16_t *&o) {
        S_projVertex &v = NewVertex(o);
        const float c = right ? 1.0f : -1.0f;
        v.pos.x = c;
        const float t = (c - p.pos.x) / (q.pos.x - p.pos.x);
        v.pos.y = (q.pos.y - p.pos.y) * t + p.pos.y;
        v.pos.z = (q.pos.z - p.pos.z) * t + p.pos.z;
        v.clip = v.pos.y < -1.0f ? PCLIP_BOTTOM : v.pos.y > 1.0f ? PCLIP_TOP : 0;
        EndVertex();
    };
    auto cutY = [](const S_projVertex &p, const S_projVertex &q, bool top, uint16_t *&o) {
        S_projVertex &v = NewVertex(o);
        const float c = top ? 1.0f : -1.0f;
        v.pos.y = c;
        const float t = (c - p.pos.y) / (q.pos.y - p.pos.y);
        v.pos.x = (q.pos.x - p.pos.x) * t + p.pos.x;
        v.pos.z = (q.pos.z - p.pos.z) * t + p.pos.z;
        // (The flags stay as the buffer held them; nothing reads them.)
        EndVertex();
    };
    for (; numFaces; --numFaces, faces += 3) {
        const S_projVertex &v0 = V(faces[0]);
        const S_projVertex &v1 = V(faces[1]);
        const S_projVertex &v2 = V(faces[2]);
        if (v2.clip & v0.clip & v1.clip)
            continue;
        if (Culled(v0, v1, v2))
            continue;
        uint32_t clip = static_cast<uint16_t>(v0.clip | v1.clip | v2.clip);
        if (!clip) {
            std::memcpy(outFaces, faces, 6);
            outFaces += 3;
            continue;
        }
        uint16_t *in = s_polyA, *out = s_polyB;
        in[0] = faces[0];
        in[1] = faces[1];
        in[2] = faces[2];
        in[3] = faces[0];
        s_polyCount = 3;
        if (clip & (PCLIP_NEAR | PCLIP_FAR)) {
            s_polyCount = ClipPass(in, s_polyCount, out, PCLIP_NEAR, PCLIP_FAR, cutZ);
            std::swap(in, out);
            clip = PolyClip(in, s_polyCount);
        }
        if (clip & (PCLIP_LEFT | PCLIP_RIGHT)) {
            s_polyCount = ClipPass(in, s_polyCount, out, PCLIP_LEFT, PCLIP_RIGHT, cutX);
            std::swap(in, out);
            clip = PolyClip(in, s_polyCount);
        }
        if (clip & (PCLIP_BOTTOM | PCLIP_TOP)) {
            s_polyCount = ClipPass(in, s_polyCount, out, PCLIP_BOTTOM, PCLIP_TOP, cutY);
            std::swap(in, out);
        }
        if (s_polyCount > 2) {
            for (int k = 2; k < s_polyCount; ++k) {
                outFaces[0] = in[0];
                outFaces[1] = in[k - 1];
                outFaces[2] = in[k];
                outFaces += 3;
            }
            PushNewVertices(in, s_polyCount, v0, v1, v2, firstNew);
        }
        if (s_numTemp >= s_tempLimit)
            break;
    }
    if (!s_groupBlended) {
        s_newFaces += static_cast<int>(outFaces - s_faceOut) / 3;
        s_faceOut = outFaces;
    } else {
        s_newBlendedFaces += static_cast<int>(outFaces - s_blendedFaceOut) / 3;
        s_blendedFaceOut = outFaces;
    }
}

// 0x10002ce0: a perspective volume's faces, clipped to its pyramid, the polygons cut into z steps
// of 1 / driver state 0x35 and zipped into triangles from their nearest vertex.
bool ClipPerspective(const uint16_t *faces, int numFaces)
{
    const int firstNew = s_numTemp;
    uint16_t *outFaces = s_groupBlended ? s_blendedFaceOut : s_faceOut;
    auto cutZ = [](const S_projVertex &p, const S_projVertex &q, bool farPlane, uint16_t *&o) {
        S_projVertex &v = NewVertex(o);
        v.pos.z = farPlane ? 1.0f : 0.0f;
        const float t = (farPlane ? 1.0f - p.pos.z : -p.pos.z) / (q.pos.z - p.pos.z);
        v.pos.x = (q.pos.x - p.pos.x) * t + p.pos.x;
        v.pos.y = (q.pos.y - p.pos.y) * t + p.pos.y;
        v.clip = ClipXY(v.pos.x, v.pos.y, farPlane ? 1.0f : 0.0f);
        EndVertex();
    };
    auto cutX = [](const S_projVertex &p, const S_projVertex &q, bool right, uint16_t *&o) {
        S_projVertex &v = NewVertex(o);
        // The crossing of x = -z / x = z.
        const float t = right ? (p.pos.z - p.pos.x) / (((q.pos.x - p.pos.x) - q.pos.z) + p.pos.z)
                              : (p.pos.z + p.pos.x) / (((p.pos.z + p.pos.x) - q.pos.x) - q.pos.z);
        v.pos.x = (q.pos.x - p.pos.x) * t + p.pos.x;
        v.pos.y = (q.pos.y - p.pos.y) * t + p.pos.y;
        v.pos.z = right ? v.pos.x : -v.pos.x;
        v.clip = !(std::fabs(v.pos.y) > v.pos.z) ? 0 : v.pos.y < 0.0f ? PCLIP_BOTTOM : PCLIP_TOP;
        EndVertex();
    };
    auto cutY = [](const S_projVertex &p, const S_projVertex &q, bool top, uint16_t *&o) {
        S_projVertex &v = NewVertex(o);
        const float t = top ? (p.pos.z - p.pos.y) / (((q.pos.y - p.pos.y) - q.pos.z) + p.pos.z)
                            : (p.pos.z + p.pos.y) / (((p.pos.z + p.pos.y) - q.pos.y) - q.pos.z);
        v.pos.x = (q.pos.x - p.pos.x) * t + p.pos.x;
        v.pos.y = (q.pos.y - p.pos.y) * t + p.pos.y;
        v.pos.z = top ? v.pos.y : -v.pos.y;
        EndVertex();
    };
    for (; numFaces; --numFaces, faces += 3) {
        const S_projVertex &v0 = V(faces[0]);
        const S_projVertex &v1 = V(faces[1]);
        const S_projVertex &v2 = V(faces[2]);
        if (v2.clip & v0.clip & v1.clip)
            continue;
        if (Culled(v0, v1, v2))
            continue;
        uint32_t clip = static_cast<uint16_t>(v0.clip | v1.clip | v2.clip);
        uint16_t *in = s_polyA, *out = s_polyB;
        in[0] = faces[0];
        in[1] = faces[1];
        in[2] = faces[2];
        in[3] = faces[0];
        s_polyCount = 3;
        if (clip & (PCLIP_NEAR | PCLIP_FAR)) {
            s_polyCount = ClipPass(in, s_polyCount, out, PCLIP_NEAR, PCLIP_FAR, cutZ);
            std::swap(in, out);
            clip = PolyClip(in, s_polyCount);
        }
        if (clip & (PCLIP_LEFT | PCLIP_RIGHT)) {
            s_polyCount = ClipPass(in, s_polyCount, out, PCLIP_LEFT, PCLIP_RIGHT, cutX);
            std::swap(in, out);
            clip = PolyClip(in, s_polyCount);
        }
        if (clip & (PCLIP_BOTTOM | PCLIP_TOP)) {
            s_polyCount = ClipPass(in, s_polyCount, out, PCLIP_BOTTOM, PCLIP_TOP, cutY);
            std::swap(in, out);
        }
        if (s_polyCount > 2) {
            // The edges cut at every z step (`out` gets the polygon), the nearest vertex found.
            const float step = g_state35Inverse;
            float minZ = 10.0f;
            int nearest = 0;
            int count = 0;
            for (int k = 0; k < s_polyCount; ++k) {
                const S_projVertex &p = V(in[k]);
                if (minZ > p.pos.z) {
                    minZ = p.pos.z;
                    nearest = count;
                }
                out[count++] = in[k];
                const S_projVertex &q = V(in[k + 1]);
                const float d = std::fabs(p.pos.z - q.pos.z);
                if (!(d > step))
                    continue;
                int m = Ftol(d / step);
                // (The original has no bound: with a small step the points overran the polygon
                // buffers.)
                const int room = 510 - count;
                if (m > room)
                    m = room > 0 ? room : 0;
                const float inv = 1.0f / d;
                const float sx = ((q.pos.x - p.pos.x) * inv) * step;
                const float sy = ((q.pos.y - p.pos.y) * inv) * step;
                const float sz = ((q.pos.z - p.pos.z) * inv) * step;
                S_vector4 at;
                if (!(p.pos.z >= q.pos.z)) {
                    at = p.pos;
                } else {
                    // (From q's end, the last point on q; the original's w is unset.)
                    const float fm = static_cast<float>(m);
                    at.x = q.pos.x - sx * fm;
                    at.y = q.pos.y - sy * fm;
                    at.z = q.pos.z - sz * fm;
                    at.w = 0.0f;
                }
                for (int j = 0; j < m; ++j) {
                    at.x = sx + at.x;
                    at.y = at.y + sy;
                    at.z = at.z + sz;
                    out[count++] = static_cast<uint16_t>(s_numTemp);
                    s_tempNext->pos = at;
                    // (The flags stay as the buffer held them.)
                    EndVertex();
                }
            }
            s_polyCount = count;
            if (count == 3) {
                outFaces[0] = out[0];
                outFaces[1] = out[1];
                outFaces[2] = out[2];
            } else {
                // `in` gets the polygon from the vertex after the nearest one round to the one
                // before it; the nearest is the apex of the first triangle, the rest zips the two
                // sides together by z.
                uint16_t *ring = in;
                int r = 0;
                for (int k = nearest + 1; k < count; ++k)
                    ring[r++] = out[k];
                for (int k = 0; k < nearest; ++k)
                    ring[r++] = out[k];
                int hi = count - 2;
                int lo = 0;
                outFaces[0] = ring[hi];
                outFaces[1] = out[nearest];
                outFaces[2] = ring[0];
                outFaces += 3;
                int nextHi = count - 3;
                int nextLo = 1;
                while (nextHi > nextLo) {
                    if (!(V(ring[nextHi]).pos.z < V(ring[nextLo]).pos.z)) {
                        outFaces[0] = ring[hi];
                        outFaces[1] = ring[lo];
                        outFaces[2] = ring[nextLo];
                        lo = nextLo++;
                    } else {
                        outFaces[0] = ring[hi];
                        outFaces[1] = ring[lo];
                        outFaces[2] = ring[nextHi];
                        hi = nextHi--;
                    }
                    outFaces += 3;
                }
                outFaces[0] = ring[hi];
                outFaces[1] = ring[lo];
                outFaces[2] = ring[nextHi];
            }
            outFaces += 3;
            PushNewVertices(out, count, v0, v1, v2, firstNew);
        }
        if (s_numTemp >= s_tempLimit)
            break;
    }
    if (s_groupBlended) {
        s_newBlendedFaces += static_cast<int>(outFaces - s_blendedFaceOut) / 3;
        s_blendedFaceOut = outFaces;
    } else {
        s_newFaces += static_cast<int>(outFaces - s_faceOut) / 3;
        s_faceOut = outFaces;
    }
    return true;
}

// The vertex count a perspective polygon may grow by (1 / step + 1, as the original's __ftol).
int StepFactor() { return Ftol(1.0f / g_state35Inverse + 1.0f); }

// 0x10001230: room for a mesh: its vertices and 12 per face (perspective: times the steps) and
// 8 faces per face plus the opaque faces held.
bool Reserve(int numVerts, int numFaces)
{
    const bool stepped = !s_orthogonal && g_state35Inverse < 1.0f;
    if (s_tempCapacity < 65000) {
        int need = numVerts + numFaces * 12;
        if (stepped)
            need *= StepFactor();
        if (need > 65000)
            need = 65000;
        if (s_tempCapacity < need) {
            if (need < 0x200)
                need = 0x200;
            s_tempCapacity = need;
            AlignedFree(s_temp);
            s_temp = static_cast<S_projVertex *>(AlignedAlloc(static_cast<size_t>(need) * sizeof(S_projVertex), 16));
            if (!s_temp) {
                s_tempCapacity = 0;
                return false;
            }
            s_tempLimit = stepped ? s_tempCapacity - Ftol((1.0f / g_state35Inverse + 1.0f) * 15.0f)
                                  : s_tempCapacity - 15;
        }
    }
    if (s_faceCapacity < 32000) {
        int need = numFaces * 8;
        if (stepped)
            need *= StepFactor();
        need += s_numFaces;
        if (need > 32000)
            need = 32000;
        if (s_faceCapacity < need) {
            if (need < 0x80)
                need = 0x80;
            auto *block = static_cast<uint16_t *>(::operator new(static_cast<size_t>(need) * 12));
            if (!block)
                return false;
            if (s_numFaces)
                std::memcpy(block, s_faces, static_cast<size_t>(s_numFaces) * 6);
            if (s_numBlendedFaces)
                std::memcpy(block + need * 3, s_blendedFaces, static_cast<size_t>(s_numBlendedFaces) * 6);
            ::operator delete(s_faces);
            s_faceCapacity = need;
            s_faces = block;
            s_blendedFaces = block + need * 3;
        }
    }
    return true;
}

// 0x100017b0: the face groups' faces (the materials' kind chooses the list; blended ones only
// with s_withBlended), clipped; then the vertices they use numbered into the result.
bool CollectFaces(I3D_face_group *group)
{
    s_tempNext = s_temp + s_numTemp;
    s_faceOut = s_faces + s_numFaces * 3;
    s_blendedFaceOut = s_blendedFaces + s_numBlendedFaces * 3;
    s_newFaces = 0;
    s_newBlendedFaces = 0;
    if (!group)
        return false;
    for (; group; group = group->next) {
        const I3D_material *m = group->material;
        if (!m) {
            s_twoSided = 0;
            s_groupBlended = 0;
        } else if (!(m->matFlags & (MATF_ADDITIVE | MATF_ALPHA | MATF_COLORKEY)) && !(m->opacity < 1.0f)) {
            s_groupBlended = 0;
            s_twoSided = m->matFlags & MATF_TWO_SIDED;
        } else {
            s_groupBlended = 1;
            if (!s_withBlended)
                continue;
            s_twoSided = m->matFlags & MATF_TWO_SIDED;
        }
        if (s_bothSides)
            s_twoSided = 1;
        // (The faces stay locked, as with the renderer's buffers.)
        const auto *faces = reinterpret_cast<const uint16_t *>(group->Lock(1));
        if (s_orthogonal) {
            ClipOrthogonal(faces, group->numFaces);
        } else if (!ClipPerspective(faces, group->numFaces)) {
            return false;
        }
    }
    if (!s_newFaces && !s_newBlendedFaces)
        return false;
    // (The original compares these with the vertex capacity.)
    if (s_numFaces > s_tempCapacity || s_numTemp > s_tempCapacity)
        return false;
    int next = s_numOut;
    if (s_newFaces) {
        uint16_t *f = s_faces + s_numFaces * 3;
        s_numFaces += s_newFaces;
        if (s_numFaces > s_faceCapacity)
            return false;
        for (int k = s_newFaces * 3; k; --k, ++f) {
            S_projVertex &v = V(*f);
            if (v.outIndex < 0) {
                v.outIndex = static_cast<int16_t>(next);
                *f = static_cast<uint16_t>(next);
                ++next;
            } else {
                *f = static_cast<uint16_t>(v.outIndex);
            }
        }
    }
    if (s_newBlendedFaces) {
        uint16_t *f = s_blendedFaces + s_numBlendedFaces * 3;
        s_numBlendedFaces += s_newBlendedFaces;
        // (The original tests the opaque count here.)
        if (s_numFaces > s_faceCapacity)
            return false;
        for (int k = s_newBlendedFaces * 3; k; --k, ++f) {
            S_projVertex &v = V(*f);
            if (v.outIndex < 0) {
                v.outIndex = static_cast<int16_t>(next);
                *f = static_cast<uint16_t>(next);
                ++next;
                s_blendedSum.x = s_blendedSum.x + v.pos.x;
                s_blendedSum.y = s_blendedSum.y + v.pos.y;
                s_blendedSum.z = s_blendedSum.z + v.pos.z;
            } else {
                *f = static_cast<uint16_t>(v.outIndex);
            }
        }
    }
    // (A 16-bit count: never above 65000.)
    const int total = static_cast<int16_t>(next);
    if (total > 65000) {
        s_numFaces -= s_newFaces;
        s_numBlendedFaces -= s_newBlendedFaces;
        return false;
    }
    if (s_outCapacity < total) {
        const int capacity = total < 0x200 ? 0x200 : total;
        auto *out = static_cast<S_vector4 *>(AlignedAlloc(static_cast<size_t>(capacity) * sizeof(S_vector4), 16));
        if (!out)
            return false;
        if (s_numOut)
            std::memcpy(out, s_out, static_cast<size_t>(s_numOut) * sizeof(S_vector4));
        AlignedFree(s_out);
        s_outCapacity = capacity;
        s_out = out;
    }
    int added = 0;
    for (int i = 0; i < s_numTemp; ++i) {
        if (s_temp[i].outIndex >= 0) {
            s_out[s_temp[i].outIndex] = s_temp[i].pos;
            ++added;
        }
    }
    s_numOut += added;
    return true;
}

// The vertices into volume space with their flags (0x10001500 and its variants, `stride`
// bytes apart), then the faces.
void Project(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended,
             const S_matrix *world, int stride)
{
    if (!Reserve(numVerts, numFaces))
        return;
    s_withBlended = withBlended;
    const S_matrix m = world ? *world * *s_projection : *s_projection;
    const auto *src = static_cast<const uint8_t *>(verts);
    S_projVertex *v = s_temp;
    for (int i = numVerts; i; --i, ++v, src += stride) {
        const S_vector p = *reinterpret_cast<const S_vector *>(src) * m;
        // (The original copies 16 bytes of the 12-byte result: w is unset.)
        v->pos.x = p.x;
        v->pos.y = p.y;
        v->pos.z = p.z;
        if (s_orthogonal) {
            uint32_t c = ClipXY(p.x, p.y, 1.0f);
            if (p.z < 0.0f)
                c |= PCLIP_NEAR;
            else if (p.z > 1.0f)
                c |= PCLIP_FAR;
            v->clip = c;
        } else if (p.z < 0.0f) {
            const float nz = -p.z;
            v->clip = PCLIP_NEAR;
            if (std::fabs(p.x) > nz)
                v->clip = p.x < 0.0f ? PCLIP_NEAR | PCLIP_LEFT : PCLIP_NEAR | PCLIP_RIGHT;
            if (std::fabs(p.y) > nz)
                v->clip |= p.y < 0.0f ? PCLIP_BOTTOM : PCLIP_TOP;
        } else {
            v->clip = p.z > 1.0f ? PCLIP_FAR : 0;
            if (std::fabs(p.x) > p.z)
                v->clip |= p.x < 0.0f ? PCLIP_LEFT : PCLIP_RIGHT;
            if (std::fabs(p.y) > p.z)
                v->clip |= p.y < 0.0f ? PCLIP_BOTTOM : PCLIP_TOP;
        }
        v->outIndex = -1;
    }
    s_numTemp = numVerts;
    CollectFaces(groups);
}

// The length of a matrix column (0 / 1 when (nearly) so).
float ColumnLength(const S_matrix &m, int c)
{
    const float sq = (m.m[2][c] * m.m[2][c] + m.m[1][c] * m.m[1][c]) + m.m[0][c] * m.m[0][c];
    if (IsTinyBits(sq))
        return 0.0f;
    if (std::fabs(sq - 1.0f) < kEpsilon)
        return 1.0f;
    return std::sqrt(sq);
}

}  // namespace

namespace ls3d::i3d {

// 0x10001470
void FreeProjectionBuffers()
{
    AlignedFree(s_temp);
    s_temp = nullptr;
    s_tempCapacity = 0;
    ::operator delete(s_faces);
    s_faces = nullptr;
    s_numFaces = 0;
    s_blendedFaces = nullptr;
    AlignedFree(s_out);
    s_out = nullptr;
    s_numOut = 0;
    s_outCapacity = 0;
    s_faceCapacity = 0;
}

}  // namespace ls3d::i3d

namespace ls3d::i3d::proj {

I3D_visual *g_receivers[0x200];

// 0x10001200
void Begin()
{
    s_numOut = 0;
    s_numFaces = 0;
    s_numBlendedFaces = 0;
    s_blendedSum = S_vector(0.0f, 0.0f, 0.0f);
    s_blendedSumCount = 0;
}

// 0x10001010
void Setup(const S_matrix *projection, const S_matrix *world, bool orthogonal, float normalBias, float edgeExpand,
           float opacity)
{
    s_projection = projection;
    s_world = world;
    s_orthogonal = orthogonal;
    const float lx = ColumnLength(*projection, 0);
    const float ly = ColumnLength(*projection, 1);
    const float invZ = 1.0f / ColumnLength(*projection, 2);
    s_opacity = opacity;
    s_normalBias = normalBias;
    s_edgeExpand = edgeExpand;
    s_scaleX = lx * invZ;
    s_scaleY = invZ * ly;
    s_invOpacity = !(opacity < kEpsilon) ? 1.0f / opacity : 1e8f;
    s_bothSides = opacity > 0.9999f;
}

void ProjectMesh(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended,
                 const S_matrix *world)
{
    Project(verts, numVerts, groups, numFaces, withBlended, world, 0x20);
}

void ProjectMesh4(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended,
                  const S_matrix *world)
{
    Project(verts, numVerts, groups, numFaces, withBlended, world, 0x10);
}

void ProjectWorldMesh(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended)
{
    Project(verts, numVerts, groups, numFaces, withBlended, nullptr, 0x20);
}

void ProjectWorldMesh4(const void *verts, int numVerts, I3D_face_group *groups, int numFaces, uint32_t withBlended)
{
    Project(verts, numVerts, groups, numFaces, withBlended, nullptr, 0x10);
}

const S_vector4 *Vertices() { return s_out; }
int NumVertices() { return s_numOut; }
const uint16_t *OpaqueFaces() { return s_faces; }
int NumOpaqueFaces() { return s_numFaces; }
const uint16_t *BlendedFaces() { return s_blendedFaces; }
int NumBlendedFaces() { return s_numBlendedFaces; }
const S_vector &BlendedSum() { return s_blendedSum; }
int BlendedSumCount() { return s_blendedSumCount; }

}  // namespace ls3d::i3d::proj
