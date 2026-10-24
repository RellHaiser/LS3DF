// I3D_lit_object::Construct (0x100237c0, slot 0xB4) and the light it samples points with
// (0x10064b90): the current set's lightmaps computed from the lights of the object's sector.
//
// Each mesh level is lit as the set's flags ask:
//  - vertex colours (no LMSET_TEXTURES): each vertex at its position and normal, the colour x 127.5
//    (2.0 is white), the alpha the opacity of the face group's material;
//  - LMSET_TEXTURES: the level's faces are grouped into patches: from the first face left, the
//    faces sharing two corners with one of the patch's faces and within the set's smoothing angle
//    of its first face. A patch's vertices are projected onto the plane of its summed normal; of
//    the rectangles around them with a side along two of the vertices the smallest is gridded at
//    the set's resolution (level + 1 times coarser for the level), 4 - 256 texels a side with half
//    a texel and more of margin. A texel is the mean of a grid of samples (DRVSTATE_LM_SAMPLES a
//    side) on the patch's faces (a sample on none is moved onto the nearest face's edge, then
//    0.0075 into it), texels no sample lit take the mean of their lit neighbours until none is
//    left, the patch may be blurred, and a patch of one colour is kept as a uniform 3 x 3 patch.
//    The lightmap vertices are the patches' vertices (at the level's vertex index where it is
//    free) with their place in their patch; S_lightmapLevel::Build packs the patches into the
//    atlas.
//
// The original keeps the per-level working arrays in C_vectors (the vertices and faces 16-byte
// aligned) and the face groups' face-to-patch lists in a fixed array of 100; the port uses
// std::vector (an allocation failure reports I3DERR_OUTOFMEM, as the original's checks do).
#include "i3d/litobject.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "core/debug.h"
#include "core/fpu.h"
#include "i3d/collision.h"
#include "i3d/driver.h"
#include "i3d/light.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/render_helpers.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "igraph/igraph_state.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

constexpr float kPi = 3.14159265f;
// How far shadow rays start off the surface, and outside samples are moved into their face.
constexpr float kLift = 0.0075f;
// The colour of a texel no sample lit (and of the border of a patch's working copy): BGR magenta.
constexpr uint8_t kMarker[3] = {0xff, 0x00, 0xff};
// The blur's weights (0x1009cbd0), by row.
constexpr float kBlurWeights[9] = {1.0f, 2.0f, 1.0f, 2.0f, 4.0f, 2.0f, 1.0f, 2.0f, 1.0f};

// A mesh level's vertex (0x20 bytes).
struct S_srcVertex {
    S_vector pos;
    S_vector normal;
    float u, v;
};

// A level vertex being baked (0x40 bytes in the original).
struct S_bakeVertex {
    S_vector pos;                           // in world space
    S_vector2 texel;                        // in the current patch's texel grid
    S_vector2 uv;                           // in the current patch (0 - 1)
    bool projected;                         // uv set for the current patch (vertex colours: lit)
    uint16_t index;                         // the level's vertex
    S_vector normal;                        // in world space, normalized
};

// A level face being baked (0x20 bytes).
struct S_bakeFace {
    I3D_face_group *group;
    uint16_t groupIndex;
    uint16_t face;                          // in its group
    uint16_t v[3];
    bool unassigned;                        // in no patch yet
    S_vector normal;
};

// A lightmap vertex as the patches make them (12 bytes).
struct S_bakeLmVertex {
    uint16_t vertex;
    uint16_t part;
    S_lightmapUV uv;
};

// A face's patch, in its face group's list.
struct S_facePart {
    uint16_t face;
    uint16_t part;
};

uint32_t FloatBits(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

bool IsMarker(const uint8_t *p) { return p[2] == kMarker[2] && p[1] == kMarker[1] && p[0] == kMarker[0]; }

void SetMarker(uint8_t *p)
{
    p[2] = kMarker[2];
    p[1] = kMarker[1];
    p[0] = kMarker[0];
}

// A patch's texel count along a side.
int ClampSize(int n) { return n > 256 ? 256 : (n < 4 ? 4 : n); }

// The 0 - 2 clamp of a light value (a NaN stays).
float Clamp02(float x)
{
    if (x < 0.0f)
        return 0.0f;
    if (x > 2.0f)
        return 2.0f;
    return x;
}

// 0x10028920
float Dot2(const S_vector2 &p, const S_vector2 &n) { return n.y * p.y + n.x * p.x; }

}  // namespace

// 0x10064b90 (__fastcall in the original)
void ls3d::i3d::LightmapLightAt(S_vector &color, const S_vector &pos, const S_vector &normal,
                                const I3D_material *material, I3D_scene *scene, uint32_t flags, bool *blocked,
                                I3D_frame *self, int level)
{
    I3D_sector *sector = GetLightSector();
    S_vector ambient(0.0f, 0.0f, 0.0f);
    S_vector diffuse(0.0f, 0.0f, 0.0f);
    int numTraced = 0;
    int numInside = 0;
    for (int n = sector->lights.Size(), i = 0; n != 0; --n, ++i) {
        I3D_light *light = sector->lights[i];
        if (!(light->mode & LIGHTMODE_LIGHTMAP) || !(light->frameFlags & FRMFLAGS_ON))
            continue;
        if (IsTinyBits(std::fabs(light->power)))
            continue;
        uint32_t rayFlags = I3DCOL_NO_JOINTS | I3DCOL_ALL_HITS | I3DCOL_FILTER | I3DCOL_TWO_SIDED | I3DCOL_OMNI_SECTOR;
        if (level > 0)
            rayFlags |= I3DCOL_LOD_FRAME;
        const D3DLIGHT8 &d3d = light->d3dLight;
        const S_vector &c = light->effectiveColor;
        S_vector lit(0.0f, 0.0f, 0.0f);     // the light's diffuse at the point
        S_vector from(0.0f, 0.0f, 0.0f);    // its shadow ray
        S_vector dir(0.0f, 0.0f, 0.0f);
        switch (light->lightType) {
        case I3DLIGHT_POINT: {
            const S_vector &at = light->GetMatrix()(3);
            const S_vector d(at.x - pos.x, at.y - pos.y, at.z - pos.z);
            const float dist2 = SquareXZY(d);
            if (dist2 > d3d.Range * d3d.Range)
                continue;
            ambient += light->effectiveAmbient;
            const float cosine = (d.x * normal.x + normal.z * d.z) + normal.y * d.y;
            if (cosine < 0.0f || cosine == 0.0f)
                continue;
            const float dist = std::sqrt(dist2);
            if (IsTinyBits(dist))
                continue;
            const float k = cosine / dist;
            const float att = 1.0f / ((dist * d3d.Attenuation1 + dist2 * d3d.Attenuation2) + d3d.Attenuation0);
            lit = S_vector(k * c.x * att, k * c.y * att, k * c.z * att);
            from = S_vector(normal.x * kLift + pos.x, normal.y * kLift + pos.y, normal.z * kLift + pos.z);
            dir = S_vector(at.x - from.x, at.y - from.y, at.z - from.z);
            break;
        }
        case I3DLIGHT_SPOT: {
            const S_matrix &m = light->GetMatrix();
            S_vector axis = m(2);
            NormalizeWithSquare(axis, SquareXYZ(axis));
            const S_vector toward(-axis.x, -axis.y, -axis.z);
            const S_vector &at = m(3);
            const S_vector d(at.x - pos.x, at.y - pos.y, at.z - pos.z);
            const float dist2 = SquareZYX(d);
            if (dist2 > d3d.Range * d3d.Range)
                continue;
            const float dist = std::sqrt(dist2);
            if (IsTinyBits(dist))
                continue;
            const float inv = 1.0f / dist;
            const float cone = ((toward.z * d.z + toward.y * d.y) + toward.x * d.x) * inv;
            if (cone < light->coneOuterCos || cone == light->coneOuterCos)
                continue;
            ambient += light->effectiveAmbient;
            const float cosine = (normal.z * d.z + normal.y * d.y) + normal.x * d.x;
            if (cosine < 0.0f || cosine == 0.0f)
                continue;
            float att = 1.0f / ((dist * d3d.Attenuation1 + dist2 * d3d.Attenuation2) + d3d.Attenuation0);
            if (cone < light->coneInnerCos || cone == light->coneInnerCos)
                att = att * ((cone - light->coneOuterCos) * light->coneFalloff);
            lit = S_vector(att * c.x * cosine * inv, att * c.y * cosine * inv, att * c.z * cosine * inv);
            from = S_vector(normal.x * kLift + pos.x, normal.y * kLift + pos.y, normal.z * kLift + pos.z);
            dir = S_vector(at.x - from.x, at.y - from.y, at.z - from.z);
            break;
        }
        case I3DLIGHT_DIRECTIONAL: {
            ambient += light->effectiveAmbient;
            const D3DVECTOR &ld = d3d.Direction;
            const float cosine = (ld.x * normal.x + normal.z * ld.z) + normal.y * ld.y;
            if (cosine >= 0.0f)
                continue;
            const float k = -cosine;
            lit = S_vector(k * c.x, k * c.y, k * c.z);
            from = S_vector(normal.x * kLift + pos.x, normal.y * kLift + pos.y, normal.z * kLift + pos.z);
            dir = S_vector(-ld.x, -ld.y, -ld.z);
            rayFlags |= I3DCOL_UNLIMITED;
            break;
        }
        case I3DLIGHT_AMBIENT:
            ambient += light->effectiveColor;
            continue;
        default:
            // (Fog lights add nothing. Fix: a type above 7 makes the original add an unset light.)
            continue;
        }
        lit.x = lit.x + lit.x;
        lit.y = lit.y + lit.y;
        lit.z = lit.z + lit.z;
        if (scene && (flags & LMCONS_SHADOWS) && (light->mode & LIGHTMODE_LMSHADOW)) {
            if (IsTinyBits(std::fabs((lit.z + lit.y) + lit.x)))
                continue;
            ++numTraced;
            // (16 bytes, as the original's: its collision code clears all four floats of the colour
            // at an opaque face.)
            S_vector4 transmit = {1.0f, 1.0f, 1.0f, 0.0f};
            float distance;
            S_vector4 hitNormal;
            I3D_frame *hit = scene->TestRay(from, dir, rayFlags, &distance, &hitNormal, nullptr, &transmit,
                                            TransmitFilter, level > 0 ? self : nullptr, level);
            if (hit && !(hit->type == FRAME_VISUAL && hit->GetVisualType() == FourCC("BBRD"))) {
                NormalizeWithSquare(dir, SquareZYX(dir));
                if (!((dir.z * hitNormal.z + dir.y * hitNormal.y) + dir.x * hitNormal.x < 0.0f))
                    ++numInside;
            }
            lit.x = transmit.x * lit.x;
            lit.y = transmit.y * lit.y;
            lit.z = transmit.z * lit.z;
        }
        diffuse += lit;
    }
    if (blocked && numInside && numInside == numTraced) {
        color = S_vector(1.0f, 0.0f, 1.0f);
        *blocked = true;
        return;
    }
    if (material && (material->matFlags & MATF_COLORS)) {
        const float ax = ambient.x * material->ambient.x;
        const float ay = ambient.y * material->ambient.y;
        const float az = ambient.z * material->ambient.z;
        color.x = (diffuse.x * material->diffuse.x + (ax + ax)) + material->emissive.x;
        color.y = (diffuse.y * material->diffuse.y + (ay + ay)) + material->emissive.y;
        color.z = (diffuse.z * material->diffuse.z + (az + az)) + material->emissive.z;
    } else {
        const D3DMATERIAL8 &m = ig::g_defaultMaterial;
        const float ax = ambient.x * m.Ambient.r;
        const float ay = ambient.y * m.Ambient.g;
        const float az = ambient.z * m.Ambient.b;
        color.x = (diffuse.x * m.Diffuse.r + (ax + ax)) + m.Emissive.r;
        color.y = (diffuse.y * m.Diffuse.g + (ay + ay)) + m.Emissive.g;
        color.z = (diffuse.z * m.Diffuse.b + (az + az)) + m.Emissive.b;
    }
    if (blocked)
        *blocked = false;
}

namespace {

// The lightmaps of one lit object, level by level.
class C_lightmapBaker {
public:
    C_lightmapBaker(I3D_lit_object *object, I3D_scene *scene, const S_vector *planeNormal, uint32_t flags,
                    I3D_LIGHTMAP_PROGRESS *progress, void *context);
    // The level's lightmaps into `l` (cleared): vertex colours or patches by the set's flags.
    I3D_RESULT Bake(int level, I3D_mesh_level *ml, S_lightmapLevel *l);

private:
    I3D_RESULT BakeColors(I3D_mesh_level *ml, S_lightmapLevel *l);
    I3D_RESULT BakeTextures(I3D_mesh_level *ml, S_lightmapLevel *l);
    // The level's vertices and faces in world space.
    void Collect(I3D_mesh_level *ml);
    // Two faces have two corners in the same place.
    bool Adjacent(const S_bakeFace &a, const S_bakeFace &b) const;
    I3D_RESULT BakePatch(const std::vector<uint16_t> &patch, S_vector normal);
    // The smallest rectangle around the patch's vertices in its plane: m_axes, m_min, m_max.
    void FitRectangle(const S_vector &normal);
    void LightTexels(const std::vector<uint16_t> &patch, uint8_t *pixels, int w, int h, float patchShare);
    // One sample at (x, y) of the patch's texel grid: false when it is on no face of the patch or
    // its light was blocked.
    bool Sample(const std::vector<uint16_t> &patch, float x, float y, S_vector &color);
    // A sample outside its face onto the face's edge `edge`, then into the face.
    void MoveOntoFace(const S_bakeFace &face, int edge, float x, float y, S_vector &pos) const;
    void DrawTestPattern(uint8_t *pixels, int w, int h);
    // `ext` ((w + 2) x (h + 2)) the patch's texels inside a border of markers.
    static void CopyWithBorder(uint8_t *ext, const uint8_t *pixels, int w, int h);
    // Texels no sample lit take the mean of their lit neighbours, until none is left.
    void FillGaps(uint8_t *ext, uint8_t *pixels, int w, int h);
    static void Blur(const uint8_t *ext, uint8_t *pixels, int w, int h);
    // The patches, lightmap vertices and face-to-patch maps into the level, then the atlas.
    I3D_RESULT Finish(I3D_mesh_level *ml, S_lightmapLevel *l);
    // The pixels of the patches not given to a level.
    void FreeParts();
    void ReportFaces(int faces);
    void ReportTexels(int done, int total, float patchShare);
    I3D_RESULT OutOfMemory();

    I3D_lit_object *m_object;
    I3D_scene *m_scene;
    const S_vector *m_planeNormal;
    uint32_t m_flags;
    I3D_LIGHTMAP_PROGRESS *m_progress;
    void *m_context;
    S_lightmapSet *m_set;
    int m_level = 0;
    S_matrix m_matrix;                      // the object's draw matrix
    DWORD m_lastReport;
    float m_progressValue = 0.0f;           // the last progress sent
    int m_totalFaces = 0;                   // all levels'
    int m_facesLeft;                        // not in a patch yet (counted only with a callback)
    float m_facesDone = 0.0f;               // in patches done
    const S_srcVertex *m_src = nullptr;
    std::vector<S_bakeVertex> m_vertices;
    std::vector<S_bakeFace> m_faces;
    std::vector<uint16_t> m_patchVertices;
    std::vector<S_lightmapPart> m_parts;
    std::vector<S_bakeLmVertex> m_lmVertices;
    std::vector<std::vector<S_facePart>> m_faceParts;   // per face group
    S_matrix m_axes;                        // the patch's rectangle: its x and y, the plane's -normal
    S_vector m_min, m_max;                  // ... its extent along them
};

C_lightmapBaker::C_lightmapBaker(I3D_lit_object *object, I3D_scene *scene, const S_vector *planeNormal,
                                 uint32_t flags, I3D_LIGHTMAP_PROGRESS *progress, void *context)
    : m_object(object), m_scene(scene), m_planeNormal(planeNormal), m_flags(flags), m_progress(progress),
      m_context(context), m_set(&object->CurrentSet())
{
    m_lastReport = timeGetTime();
    const I3D_mesh *mesh = object->mesh;
    for (uint32_t i = 0; i < mesh->numLevels; ++i)
        m_totalFaces += mesh->levels[i]->NumFaces();
    m_facesLeft = m_totalFaces;
}

I3D_RESULT C_lightmapBaker::OutOfMemory()
{
    ReportError(I3DERR_OUTOFMEM, kSevFatal);
    return I3DERR_OUTOFMEM;
}

void C_lightmapBaker::FreeParts()
{
    for (S_lightmapPart &p : m_parts) {
        if (!p.IsUniform())
            ::operator delete(p.pixels);
    }
    m_parts.clear();
}

void C_lightmapBaker::ReportFaces(int faces)
{
    if (!m_progress)
        return;
    const DWORD now = timeGetTime();
    if (static_cast<int>(now - m_lastReport) < 250)
        return;
    m_progressValue = 100.0f / static_cast<float>(m_totalFaces) * static_cast<float>(faces) * 0.01f;
    m_progress(8, FloatBits(m_progressValue), 1, m_context);
    m_lastReport = now;
}

void C_lightmapBaker::ReportTexels(int done, int total, float patchShare)
{
    if (!m_progress)
        return;
    const DWORD now = timeGetTime();
    if (static_cast<int>(now - m_lastReport) < 250)
        return;
    const float perFace = 100.0f / static_cast<float>(m_totalFaces);
    m_progressValue =
        (static_cast<float>(done) / static_cast<float>(total) * patchShare + perFace * m_facesDone) * 0.01f;
    m_progress(8, FloatBits(m_progressValue), 1, m_context);
    m_lastReport = now;
}

I3D_RESULT C_lightmapBaker::Bake(int level, I3D_mesh_level *ml, S_lightmapLevel *l)
{
    m_level = level;
    // 0x10028980
    m_matrix = (m_object->visualFlags & VISF_WORLD_SPACE) ? kIdentityMatrix : m_object->GetMatrix();
    try {
        const I3D_RESULT r = (m_set->flags & LMSET_TEXTURES) ? BakeTextures(ml, l) : BakeColors(ml, l);
        if (r != I3D_OK)
            FreeParts();
        return r;
    } catch (const std::bad_alloc &) {
        FreeParts();
        return OutOfMemory();
    }
}

// ---------------------------------------------------------------- vertex colours

I3D_RESULT C_lightmapBaker::BakeColors(I3D_mesh_level *ml, S_lightmapLevel *l)
{
    int n = ml->numVertices;
    l->numVertices = n;
    l->colors = static_cast<uint32_t *>(::operator new(sizeof(uint32_t) * static_cast<size_t>(n), std::nothrow));
    if (!l->colors)
        return OutOfMemory();
    const auto *src = static_cast<const S_srcVertex *>(ml->LockVertices(1));
    if (!src)
        n = 0;                              // (fix: the original reads through the missing buffer)
    m_vertices.assign(static_cast<size_t>(n), S_bakeVertex{});
    for (int j = 0; j < n; ++j) {
        S_bakeVertex &v = m_vertices[j];
        v.pos = src[j].pos * m_matrix;
        v.projected = false;
        v.index = static_cast<uint16_t>(j);
        v.normal = src[j].normal.RotateByMatrix(m_matrix);
        NormalizeWithSquare(v.normal, SquareXYZ(v.normal));
    }
    ml->UnlockVertices();
    int faces = 0;
    I3D_face_group *fg = ml->faceGroups;
    for (int g = 0; g < ml->numFaceGroups && fg; ++g, fg = fg->next) {
        uint32_t alpha = 0xff000000u;
        if (fg->material)
            alpha = static_cast<uint32_t>(Fistp(fg->material->opacity * 255.0f)) << 24;
        const I3D_triface *tri = fg->numFaces ? fg->Lock(1) : nullptr;
        for (int f = 0; f < fg->numFaces; ++f) {
            ++faces;
            for (int c = 2; c >= 0; --c) {
                const uint16_t vi = tri[f].v[c];
                // (Fix: an index past the level's vertices; the original reads and writes past them.)
                if (vi < n && !m_vertices[vi].projected) {
                    S_bakeVertex &v = m_vertices[vi];
                    S_vector color;
                    LightmapLightAt(color, v.pos, v.normal, fg->material, m_scene, m_flags, nullptr, m_object, m_level);
                    const uint32_t r = static_cast<uint32_t>(Fistp(Clamp02(color.x) * 127.5f));
                    const uint32_t gr = static_cast<uint32_t>(Fistp(Clamp02(color.y) * 127.5f));
                    const uint32_t b = static_cast<uint32_t>(Fistp(Clamp02(color.z) * 127.5f));
                    l->colors[vi] = (((r << 8 | gr) << 8) | alpha) | b;
                    v.projected = true;
                }
                ReportFaces(faces);
            }
        }
    }
    return I3D_OK;
}

// ---------------------------------------------------------------- lightmap patches

void C_lightmapBaker::Collect(I3D_mesh_level *ml)
{
    int n = ml->numVertices;
    // (The original never unlocks them here.)
    m_src = static_cast<const S_srcVertex *>(ml->LockVertices(1));
    if (!m_src)
        n = 0;                              // (fix: the original reads through the missing buffer)
    m_vertices.assign(static_cast<size_t>(n), S_bakeVertex{});
    for (int j = 0; j < n; ++j) {
        S_bakeVertex &v = m_vertices[j];
        v.pos = m_src[j].pos * m_matrix;
        v.texel = {0.0f, 0.0f};
        v.projected = false;
        v.index = static_cast<uint16_t>(j);
        v.normal = m_src[j].normal.RotateByMatrix(m_matrix);
        NormalizeWithSquare(v.normal, SquareZYX(v.normal));
    }
    m_faces.clear();
    m_faces.reserve(static_cast<size_t>(ml->NumFaces()));
    I3D_face_group *fg = ml->faceGroups;
    for (int g = 0; g < ml->numFaceGroups && fg; ++g, fg = fg->next) {
        const I3D_triface *tri = fg->numFaces ? fg->Lock(1) : nullptr;
        for (int f = 0; f < fg->numFaces; ++f) {
            S_bakeFace face;
            face.group = fg;
            face.groupIndex = static_cast<uint16_t>(g);
            face.face = static_cast<uint16_t>(f);
            face.v[0] = tri[f].v[0];
            face.v[1] = tri[f].v[1];
            face.v[2] = tri[f].v[2];
            // (Fix: a face with an index past the level's vertices is left out (its patch 0); the
            // original reads past them.)
            if (face.v[0] >= n || face.v[1] >= n || face.v[2] >= n)
                continue;
            face.unassigned = true;
            const S_vector &p0 = m_vertices[face.v[0]].pos;
            const S_vector &p1 = m_vertices[face.v[1]].pos;
            const S_vector &p2 = m_vertices[face.v[2]].pos;
            const S_vector a = (p2 - p1) * 10000.0f;
            const S_vector b = (p0 - p1) * 10000.0f;
            S_vector normal(b.z * a.y - b.y * a.z, b.x * a.z - a.x * b.z, a.x * b.y - b.x * a.y);
            NormalizeWithSquare(normal, SquareZYX(normal));
            if (SquareZYX(normal) < 1e-16f)      // (not after the normalization)
                normal = S_vector(0.0f, 0.0f, 1.0f);
            face.normal = normal;
            m_faces.push_back(face);
        }
    }
}

bool C_lightmapBaker::Adjacent(const S_bakeFace &a, const S_bakeFace &b) const
{
    uint8_t shared = 0;
    for (int i = 0; i < 3; ++i) {
        const S_vector &p = m_vertices[a.v[i]].pos;
        for (int j = 0; j < 3; ++j) {
            if (static_cast<double>(SquareXYZ(p - m_vertices[b.v[j]].pos)) <= 1e-6)
                ++shared;
        }
    }
    return shared > 1;
}

I3D_RESULT C_lightmapBaker::BakeTextures(I3D_mesh_level *ml, S_lightmapLevel *l)
{
    m_parts.clear();
    m_lmVertices.clear();
    m_faceParts.assign(static_cast<size_t>(ml->numFaceGroups), {});
    Collect(ml);
    // (The x87 code rounds the product with the double 1 / 180 once, to float precision.)
    const float maxAngle = static_cast<float>(static_cast<double>(m_set->smoothAngle * kPi) * (1.0 / 180.0));
    std::vector<uint16_t> patch;
    patch.reserve(m_faces.size());
    bool onePatchMade = false;
    for (;;) {
        patch.clear();
        S_vector normal(0.0f, 0.0f, 0.0f);  // (set with the patch's first face)
        if (m_flags & LMCONS_ONE_PATCH) {
            // (Fix: the original makes this patch again while faces of later levels are left, or
            // forever without a callback (which counts them), and none for those levels.)
            if (m_facesLeft <= 0 || onePatchMade)
                break;
            onePatchMade = true;
            normal = *m_planeNormal;
            for (size_t i = 0; i < m_faces.size(); ++i) {
                patch.push_back(static_cast<uint16_t>(i));
                m_faces[i].unassigned = false;
                if (m_progress) {
                    --m_facesLeft;
                    m_progress(8, (static_cast<uint32_t>(m_facesLeft) & 0xffff) | 0x10000, 5, m_context);
                }
            }
        } else {
            for (int i = 0; i < static_cast<int>(m_faces.size()); ++i) {
                S_bakeFace &face = m_faces[i];
                if (!face.unassigned)
                    continue;
                if (patch.empty()) {
                    patch.push_back(static_cast<uint16_t>(i));
                    normal = face.normal;
                    face.unassigned = false;
                    if (m_progress) {
                        --m_facesLeft;
                        m_progress(8, (static_cast<uint32_t>(m_facesLeft) & 0xffff) | 0x10000, 5, m_context);
                    }
                    continue;
                }
                for (size_t k = 0; k < patch.size(); ++k) {
                    if (!Adjacent(m_faces[patch[k]], face))
                        continue;
                    if (!(m_faces[patch[0]].normal.AngleTo(face.normal) <= maxAngle))
                        continue;
                    patch.push_back(static_cast<uint16_t>(i));
                    normal += face.normal;
                    if (m_progress) {
                        --m_facesLeft;
                        m_progress(8, (static_cast<uint32_t>(patch.size()) & 0xffff) << 16 |
                                          (static_cast<uint32_t>(m_facesLeft) & 0xffff),
                                   5, m_context);
                    }
                    face.unassigned = false;
                    // (The scan starts again from face 1: face 0 is not looked at again.)
                    i = 0;
                    break;
                }
            }
        }
        if (patch.empty())
            break;
        const I3D_RESULT r = BakePatch(patch, normal);
        if (r != I3D_OK)
            return r;
    }
    return Finish(ml, l);
}

void C_lightmapBaker::FitRectangle(const S_vector &normal)
{
    const int nv = static_cast<int>(m_patchVertices.size());
    auto pos = [this](int k) -> const S_vector & { return m_vertices[m_patchVertices[k]].pos; };
    const S_vector &first = pos(0);
    const float d = -((normal.z * first.z + normal.y * first.y) + normal.x * first.x);
    float bestArea = 1e16f;
    S_matrix axes;
    axes.Identity();
    axes(2) = S_vector(-normal.x, -normal.y, -normal.z);
    // (Fix: the original keeps what its stack held when no two vertices are apart.)
    m_axes = axes;
    m_min = m_max = S_vector(0.0f, 0.0f, 0.0f);
    for (int a = nv - 1; a >= 0; --a) {
        if (m_progress)
            m_progress(8, (static_cast<uint32_t>(nv - a) & 0xffff) << 16 | (static_cast<uint32_t>(nv) & 0xffff), 6,
                       m_context);
        const S_vector &va = pos(a);
        const float ta = ((normal.z * va.z + normal.y * va.y) + normal.x * va.x) + d;
        const S_vector pa(va.x - normal.x * ta, va.y - ta * normal.y, va.z - normal.z * ta);
        for (int b = a - 1; b >= 0; --b) {
            const S_vector &vb = pos(b);
            const float tb = ((normal.x * vb.x + normal.z * vb.z) + normal.y * vb.y) + d;
            const S_vector pb(vb.x - normal.x * tb, vb.y - tb * normal.y, vb.z - normal.z * tb);
            S_vector x(pb.x - pa.x, pb.y - pa.y, pb.z - pa.z);
            const float sq = SquareXYZ(x);
            if (IsTinyBits(sq))
                continue;
            NormalizeWithSquare(x, sq);
            axes(0) = x;
            axes(1) = x.Cross(axes(2));
            S_matrix inv;
            inv.Inverse(axes);
            S_vector lo = pos(nv - 1) * inv;
            S_vector hi = lo;
            for (int k = nv - 2; k >= 0; --k) {
                const S_vector p = pos(k) * inv;
                if (lo.x > p.x)
                    lo.x = p.x;
                if (hi.x < p.x)
                    hi.x = p.x;
                if (lo.y > p.y)
                    lo.y = p.y;
                if (hi.y < p.y)
                    hi.y = p.y;
                if (lo.z > p.z)
                    lo.z = p.z;
                if (hi.z < p.z)
                    hi.z = p.z;
            }
            const float area = (hi.x - lo.x) * (hi.y - lo.y);
            if (bestArea > area) {
                bestArea = area;
                m_min = lo;
                m_max = hi;
                m_axes = axes;
            }
        }
    }
}

I3D_RESULT C_lightmapBaker::BakePatch(const std::vector<uint16_t> &patch, S_vector normal)
{
    NormalizeWithSquare(normal, SquareZYX(normal));
    const int count = static_cast<int>(patch.size());
    // The patch's vertices, from its last face to its first.
    m_patchVertices.clear();
    m_patchVertices.reserve(static_cast<size_t>(count) * 3);
    for (int k = count - 1; k >= 0; --k) {
        for (int c = 0; c < 3; ++c) {
            const uint16_t v = m_faces[patch[k]].v[c];
            if (std::find(m_patchVertices.begin(), m_patchVertices.end(), v) == m_patchVertices.end())
                m_patchVertices.push_back(v);
        }
    }
    FitRectangle(normal);

    // The texel grid: the rectangle with a margin of half a texel and more, into the patch's frame.
    const float scale = static_cast<float>(m_level + 1);
    const float res = m_set->resolution;
    const float width0 = ((m_max.x - m_min.x) * res + 1.0f) / scale;
    const int w0 = ClampSize(Ftol(width0));
    const float height0 = ((m_max.y - m_min.y) * res + 1.0f) / scale;
    const int h0 = ClampSize(Ftol(height0));
    const float half = scale * 0.5f / res;
    const float marginX = (width0 / static_cast<float>(w0) + 1.0f) * half;
    const float marginY = (height0 / static_cast<float>(h0) + 1.0f) * half;
    m_min.x = m_min.x - marginX;
    m_min.y = m_min.y - marginY;
    m_max.x = m_max.x + marginX;
    m_max.y = m_max.y + marginY;
    const S_vector origin = m_min.RotateByMatrix(m_axes);
    const float sizeY = m_max.y - m_min.y;
    const float sizeX = m_max.x - m_min.x;
    S_matrix size;
    size.Identity();
    size.m[0][0] = sizeX;
    size.m[1][1] = sizeY;
    S_matrix toWorld = size.RotateByMatrix(m_axes);
    toWorld(3) = origin;
    S_matrix toPatch;
    toPatch.Inverse(toWorld);
    m_parts.push_back(S_lightmapPart{});
    const int partIndex = static_cast<int>(m_parts.size()) - 1;
    S_lightmapPart &part = m_parts.back();
    part.width = static_cast<int16_t>(ClampSize(Ftol((sizeX * res + 1.0f) / scale)));
    part.height = static_cast<int16_t>(ClampSize(Ftol((sizeY * res + 1.0f) / scale)));
    for (int k = 0; k < count; ++k) {
        const S_bakeFace &face = m_faces[patch[k]];
        for (int c = 2; c >= 0; --c) {
            S_bakeVertex &v = m_vertices[face.v[c]];
            if (v.projected)
                continue;
            const S_vector p = v.pos * toPatch;
            v.uv = {p.x, p.y};
            v.projected = true;
        }
    }
    const float maxU = static_cast<float>(part.width - 1);
    const float maxV = static_cast<float>(part.height - 1);
    for (S_bakeVertex &v : m_vertices) {
        const S_vector p = v.pos * toPatch;
        v.texel = {p.x * maxU, p.y * maxV};
    }
    part.sourceWidth = part.width;
    part.sourceHeight = part.height;
    const int w = part.width;
    const int h = part.height;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    // (Fix: the original does not check this allocation.)
    auto *pixels = static_cast<uint8_t *>(::operator new(bytes, std::nothrow));
    if (!pixels)
        return OutOfMemory();
    std::memset(pixels, 0xff, bytes);
    part.pixels = pixels;

    if (m_progress)
        m_progress(8, FloatBits(m_progressValue), 1, m_context);
    const float patchShare = static_cast<float>(count) * (100.0f / static_cast<float>(m_totalFaces));
    if (m_flags & LMCONS_TEST_PATTERN) {
        DrawTestPattern(pixels, w, h);
    } else {
        LightTexels(patch, pixels, w, h, patchShare);
        const int extW = static_cast<uint16_t>(w + 2);
        const int extH = static_cast<uint16_t>(h + 2);
        std::vector<uint8_t> ext(static_cast<size_t>(extW) * static_cast<size_t>(extH) * 3);
        if (m_progress)
            m_progress(8, 0, 3, m_context);
        FillGaps(ext.data(), pixels, w, h);
        // A patch of one colour is kept as a uniform 3 x 3 patch (unlit: black).
        const uint8_t b0 = pixels[0], g0 = pixels[1], r0 = pixels[2];
        bool uniform = true;
        for (int y = 0; y < h && uniform; ++y) {
            const uint8_t *p = pixels + static_cast<size_t>(y) * w * 3;
            for (int x = 0; x < w; ++x, p += 3) {
                if (p[2] != r0 || p[1] != g0 || p[0] != b0) {
                    uniform = false;
                    break;
                }
            }
        }
        S_lightmapPart &done = m_parts[partIndex];
        if (uniform) {
            done.sourceWidth = done.sourceHeight = done.width = done.height = 3;
            done.area = 9;
            ::operator delete(pixels);
            if (r0 == kMarker[2] && g0 == kMarker[1] && b0 == kMarker[0])
                done.color = 0xff000000u;
            else
                done.color = ((0xffffff00u | r0) << 8 | g0) << 8 | b0;
        } else if (m_flags & LMCONS_BLUR) {
            if (m_progress)
                m_progress(8, 0, 4, m_context);
            CopyWithBorder(ext.data(), pixels, w, h);
            Blur(ext.data(), pixels, w, h);
        }
    }

    // The faces' patch, the lightmap vertices made by this patch.
    for (int k = 0; k < count; ++k) {
        const S_bakeFace &face = m_faces[patch[k]];
        m_faceParts[face.groupIndex].push_back({face.face, static_cast<uint16_t>(partIndex)});
        for (int c = 0; c < 3; ++c) {
            S_bakeVertex &v = m_vertices[face.v[c]];
            if (!v.projected)
                continue;
            m_lmVertices.push_back({v.index, static_cast<uint16_t>(partIndex), {v.uv.x, v.uv.y}});
            v.projected = false;
        }
    }
    m_facesDone = static_cast<float>(count) + m_facesDone;
    return I3D_OK;
}

void C_lightmapBaker::DrawTestPattern(uint8_t *pixels, int w, int h)
{
    // Red, green and blue rings at the border, a black and white checker inside.
    bool odd = false;
    for (int y = 0; y < h; ++y, odd = !odd) {
        uint8_t *p = pixels + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x, p += 3) {
            uint8_t r, g, b;
            if (x == 0 || x == w - 1 || y == 0 || y == h - 1) {
                r = 0xff, g = 0, b = 0;
            } else if (x == 1 || x == w - 2 || y == 1 || y == h - 2) {
                r = 0, g = 0xff, b = 0;
            } else if (x == 2 || x == w - 3 || y == 2 || y == h - 3) {
                r = 0, g = 0, b = 0xff;
            } else {
                r = g = b = ((x + (odd ? 1 : 0)) % 2 == 0) ? 0xff : 0;
            }
            p[2] = r;
            p[1] = g;
            p[0] = b;
        }
    }
    // (And the first three texels the marker colour.)
    SetMarker(pixels);
    SetMarker(pixels + 3);
    SetMarker(pixels + 6);
}

void C_lightmapBaker::LightTexels(const std::vector<uint16_t> &patch, uint8_t *pixels, int w, int h,
                                  float patchShare)
{
    const float samples = static_cast<float>(static_cast<int>(g_driverStates[DRVSTATE_LM_SAMPLES]));
    const float step = 1.0f / samples;
    const float jitter = 0.5f * step - 0.5f;
    for (int y = 0; y < h; ++y) {
        uint8_t *p = pixels + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x, p += 3) {
            S_vector sum(0.0f, 0.0f, 0.0f);
            int hits = 0;
            if (0.0f < samples) {
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                int sy = 0;
                do {
                    const float vy = step * static_cast<float>(sy) + jitter + fy;
                    int sx = 0;
                    do {
                        const float vx = step * static_cast<float>(sx) + jitter + fx;
                        S_vector color;
                        if (Sample(patch, vx, vy, color)) {
                            ++hits;
                            sum.x = sum.x + color.x;
                            sum.y = sum.y + color.y;
                            sum.z = sum.z + color.z;
                        }
                    } while (static_cast<float>(++sx) < samples);
                } while (static_cast<float>(++sy) < samples);
            }
            if (!hits) {
                SetMarker(p);
            } else {
                const float inv = 1.0f / static_cast<float>(hits);
                p[2] = static_cast<uint8_t>(Fistp(Clamp02(sum.x * inv) * 127.5f));
                p[1] = static_cast<uint8_t>(Fistp(Clamp02(sum.y * inv) * 127.5f));
                p[0] = static_cast<uint8_t>(Fistp(Clamp02(sum.z * inv) * 127.5f));
            }
            ReportTexels(y * w, w * h, patchShare);
        }
    }
}

bool C_lightmapBaker::Sample(const std::vector<uint16_t> &patch, float x, float y, S_vector &color)
{
    const S_vector2 at = {x, y};
    // The face it is on; else the face it is least outside of (the sum of its distances out of
    // the edges, counted while they decrease), and that face's nearest edge.
    int inside = -1;
    int nearest = -1;
    int nearestEdge = 0;
    float nearestSum = 1e16f;
    for (int k = static_cast<int>(patch.size()) - 1; k >= 0; --k) {
        const S_bakeFace &face = m_faces[patch[k]];
        bool in = true;
        bool tooFar = false;
        int edge = -1;
        float least = 1e16f;
        float sum = 0.0f;
        // The edges 0-2, 2-1 and 1-0 (each numbered by its second corner).
        for (int from = 0, e = 2; e >= 0; from = e, --e) {
            const S_vector2 &a = m_vertices[face.v[from]].texel;
            const S_vector2 &b = m_vertices[face.v[e]].texel;
            S_vector2 out = {-(a.y - b.y), a.x - b.x};
            const float len = std::sqrt(out.x * out.x + out.y * out.y);     // 0x10028900
            if (len > 1e-8f) {
                const float inv = 1.0f / len;
                out.x = out.x * inv;
                out.y = out.y * inv;
            }
            const float dist = Dot2(at, out) - Dot2(b, out);
            if (dist < 0.001f) {
                in = false;
                if (!(dist > -2.1f)) {
                    tooFar = true;
                    break;
                }
                if (least > -dist) {
                    least = -dist;
                    sum = -dist + sum;
                    edge = e;
                }
            }
        }
        if (tooFar)
            continue;
        if (in) {
            inside = k;
            break;
        }
        if (edge != -1 && nearestSum > sum) {
            nearestSum = sum;
            nearestEdge = edge;
            nearest = k;
        }
    }
    const int chosen = inside != -1 ? inside : nearest;
    if (chosen == -1)
        return false;

    // Its place in the face: the line from a corner through the sample meets the opposite edge at
    // t2 along it, the sample t1 of the way there (another corner when the lines are parallel).
    const S_bakeFace &face = m_faces[patch[chosen]];
    const S_vector s(x, y, 0.0f);
    S_vector corner[3];
    for (int c = 0; c < 3; ++c) {
        const S_vector2 &t = m_vertices[face.v[c]].texel;
        corner[c] = S_vector(t.x, t.y, 0.0f);
    }
    auto meet = [&s](const S_vector &a, const S_vector &b, const S_vector &c, float &t1, float &t2) {
        return LineLineIntersect(a, S_vector(s.x - a.x, s.y - a.y, 0.0f), b, S_vector(c.x - b.x, c.y - b.y, 0.0f),
                                 t1, t2);
    };
    float t1, t2;
    int order[3];
    if (meet(corner[0], corner[1], corner[2], t1, t2) && !(std::fabs(t1) < 1e-8f)) {
        order[0] = 0, order[1] = 1, order[2] = 2;
    } else if (meet(corner[2], corner[0], corner[1], t1, t2) && !(std::fabs(t1) < 1e-8f)) {
        order[0] = 2, order[1] = 0, order[2] = 1;
    } else if (meet(corner[1], corner[2], corner[0], t1, t2) && !IsTinyBits(std::fabs(t1))) {
        order[0] = 1, order[1] = 2, order[2] = 0;
    } else {
        return false;
    }
    const S_srcVertex &va = m_src[face.v[order[0]]];
    const S_srcVertex &vb = m_src[face.v[order[1]]];
    const S_srcVertex &vc = m_src[face.v[order[2]]];
    const float inv = 1.0f / t1;
    S_vector pos(((vc.pos.x - vb.pos.x) * t2 + vb.pos.x - va.pos.x) * inv + va.pos.x,
                 ((vc.pos.y - vb.pos.y) * t2 + vb.pos.y - va.pos.y) * inv + va.pos.y,
                 ((vc.pos.z - vb.pos.z) * t2 + vb.pos.z - va.pos.z) * inv + va.pos.z);
    pos *= m_matrix;
    const float u = 1.0f - t2;
    const float w = 1.0f - inv;
    S_vector normal(va.normal.x * w + inv * (vb.normal.x * u + vc.normal.x * t2),
                    w * va.normal.y + inv * (vb.normal.y * u + t2 * vc.normal.y),
                    w * va.normal.z + inv * (t2 * vc.normal.z + u * vb.normal.z));
    normal = normal.RotateByMatrix(m_matrix);
    NormalizeWithSquare(normal, SquareZYX(normal));
    if (inside == -1)
        MoveOntoFace(face, nearestEdge, x, y, pos);
    bool blocked = false;
    LightmapLightAt(color, pos, normal, face.group->material, m_scene, m_flags, &blocked, m_object, m_level);
    return !blocked;
}

void C_lightmapBaker::MoveOntoFace(const S_bakeFace &face, int edge, float x, float y, S_vector &pos) const
{
    const uint16_t i0 = face.v[edge];
    const uint16_t i1 = face.v[(edge + 1) % 3];
    const S_vector &p0 = m_src[i0].pos;
    const S_vector &p1 = m_src[i1].pos;
    const S_vector2 &t0 = m_vertices[i0].texel;
    const S_vector2 &t1 = m_vertices[i1].texel;
    const float ex = t1.x - t0.x;
    const float ey = t1.y - t0.y;
    const float len2 = ex * ex + ey * ey;
    if (!(len2 > 1e-8f))
        return;
    // Its place along the edge, not within 1.5 % of the corners.
    float t = ((x - t0.x) * ex + (y - t0.y) * ey) / len2;
    if (0.985f < t)
        t = 0.985f;
    if (0.015f > t)
        t = 0.015f;
    pos = S_vector(t * (p1.x - p0.x) + p0.x, t * (p1.y - p0.y) + p0.y, t * (p1.z - p0.z) + p0.z);
    pos *= m_matrix;
    // Into the face: across the edge, in the face's plane.
    const S_vector &f0 = m_src[face.v[0]].pos;
    const S_vector &f1 = m_src[face.v[1]].pos;
    const S_vector &f2 = m_src[face.v[2]].pos;
    const S_vector side = S_vector(f2.x - f1.x, f2.y - f1.y, f2.z - f1.z).Cross(f0 - f1);
    const S_vector below(p0.x - side.x, p0.y - side.y, p0.z - side.z);
    S_vector in = S_vector(below.x - p1.x, below.y - p1.y, below.z - p1.z).Cross(p0 - p1);
    in = in.RotateByMatrix(m_matrix);
    const float sq = SquareXZY(in);
    if (IsTinyBits(sq))
        return;
    float len = 1.0f;
    if (!(std::fabs(sq - 1.0f) < kEpsilon)) {
        len = std::sqrt(sq);
        if (!(len > kEpsilon))
            return;
    }
    const float inv = 1.0f / len;
    pos.x = in.x * kLift * inv + pos.x;
    pos.y = pos.y + in.y * kLift * inv;
    pos.z = pos.z + in.z * kLift * inv;
}

void C_lightmapBaker::CopyWithBorder(uint8_t *ext, const uint8_t *pixels, int w, int h)
{
    const int extW = w + 2;
    const int extH = h + 2;
    for (int y = 0; y < extH; ++y) {
        uint8_t *e = ext + static_cast<size_t>(y) * extW * 3;
        for (int x = 0; x < extW; ++x, e += 3) {
            if (x == 0 || x == w + 1 || y == 0 || y == h + 1) {
                SetMarker(e);
            } else {
                const uint8_t *p = pixels + (static_cast<size_t>(y - 1) * w + (x - 1)) * 3;
                e[0] = p[0];
                e[1] = p[1];
                e[2] = p[2];
            }
        }
    }
}

void C_lightmapBaker::FillGaps(uint8_t *ext, uint8_t *pixels, int w, int h)
{
    const int extW = w + 2;
    const int extH = h + 2;
    for (;;) {
        CopyWithBorder(ext, pixels, w, h);
        int markers = 0;
        for (int y = 1; y < extH - 1; ++y) {
            for (int x = 1; x < extW - 1; ++x) {
                if (!IsMarker(ext + (static_cast<size_t>(y) * extW + x) * 3))
                    continue;
                ++markers;
                float r = 0.0f, g = 0.0f, b = 0.0f, n = 0.0f;
                for (int dy = -1; dy < 2; ++dy) {
                    for (int dx = -1; dx < 2; ++dx) {
                        if (!dy && !dx)
                            continue;
                        const uint8_t *q = ext + (static_cast<size_t>(y + dy) * extW + dx + x) * 3;
                        if (IsMarker(q))
                            continue;
                        r = r + static_cast<float>(q[2]);
                        g = g + static_cast<float>(q[1]);
                        b = b + static_cast<float>(q[0]);
                        n = n + 1.0f;
                    }
                }
                if (n == 0.0f)
                    continue;
                const float inv = 1.0f / n;
                uint8_t *p = pixels + (static_cast<size_t>(y - 1) * w + (x - 1)) * 3;
                p[2] = static_cast<uint8_t>(Ftol(r * inv));
                p[1] = static_cast<uint8_t>(Ftol(g * inv));
                p[0] = static_cast<uint8_t>(Ftol(b * inv));
            }
        }
        if (!markers || markers == w * h)
            break;
    }
}

void C_lightmapBaker::Blur(const uint8_t *ext, uint8_t *pixels, int w, int h)
{
    const int extW = w + 2;
    for (int y = 0; y < h; ++y) {
        uint8_t *p = pixels + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x, p += 3) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            uint16_t total = 0;
            for (int dy = 0; dy < 3; ++dy) {
                for (int dx = 0; dx < 3; ++dx) {
                    const uint8_t *q = ext + (static_cast<size_t>(y + dy) * extW + x + dx) * 3;
                    if (IsMarker(q))
                        continue;
                    const float weight = kBlurWeights[dy * 3 + dx];
                    r = r + static_cast<float>(q[2]) * weight;
                    g = g + static_cast<float>(q[1]) * weight;
                    b = b + static_cast<float>(q[0]) * weight;
                    total = static_cast<uint16_t>(Ftol(static_cast<float>(total) + weight));
                }
            }
            // (The original's clamp to 255 tests the truncated byte, so it never clamps.)
            const float inv = 1.0f / static_cast<float>(total);
            p[2] = static_cast<uint8_t>(Ftol(r * inv));
            p[1] = static_cast<uint8_t>(Ftol(g * inv));
            p[0] = static_cast<uint8_t>(Ftol(b * inv));
        }
    }
}

I3D_RESULT C_lightmapBaker::Finish(I3D_mesh_level *ml, S_lightmapLevel *l)
{
    // The lightmap vertices: each at its vertex's index when that is free, else at the last free
    // place.
    const int n = static_cast<int>(m_lmVertices.size());
    l->uvs = static_cast<S_lightmapUV *>(::operator new(sizeof(S_lightmapUV) * static_cast<size_t>(n), std::nothrow));
    if (!l->uvs)
        return OutOfMemory();
    l->numVertices = n;
    l->vertexMap =
        static_cast<S_lightmapVertex *>(::operator new(sizeof(S_lightmapVertex) * static_cast<size_t>(n), std::nothrow));
    if (!l->vertexMap)
        return OutOfMemory();
    for (int i = 0; i < n; ++i)
        l->uvs[i].u = -1.0f;
    int lastFree = n - 1;
    for (const S_bakeLmVertex &v : m_lmVertices) {
        int slot = v.vertex;
        // (Fix: a vertex index past the lightmap vertices (vertices in no face) takes a free place;
        // the original writes past the arrays.)
        if (slot >= n || l->uvs[slot].u != -1.0f) {
            while (lastFree >= 0 && l->uvs[lastFree].u != -1.0f)
                --lastFree;
            if (lastFree < 0)
                break;                      // (cannot happen: one place per vertex)
            slot = lastFree--;
        }
        l->uvs[slot] = v.uv;
        l->vertexMap[slot].vertex = v.vertex;
        l->vertexMap[slot].part = v.part;
    }
    // The patches.
    const int numParts = static_cast<int>(m_parts.size());
    auto *parts =
        static_cast<S_lightmapPart *>(::operator new(sizeof(S_lightmapPart) * static_cast<size_t>(numParts), std::nothrow));
    if (!parts)
        return OutOfMemory();
    std::memset(parts, 0, sizeof(S_lightmapPart) * static_cast<size_t>(numParts));
    for (int i = 0; i < numParts; ++i) {
        const S_lightmapPart &src = m_parts[i];
        S_lightmapPart &p = parts[i];
        p.pixels = src.pixels;              // (or the uniform colour)
        p.sourceWidth = src.sourceWidth;
        p.sourceHeight = src.sourceHeight;
        p.width = src.width;
        p.height = src.height;
        p.area = static_cast<int>(src.height) * static_cast<int>(src.width);
    }
    l->parts = parts;
    l->numParts = numParts;
    m_parts.clear();                        // (the level has the pixels now)
    // Each face group's faces' patches.
    const int numGroups = ml->numFaceGroups;
    l->faceParts = static_cast<uint16_t **>(::operator new(sizeof(uint16_t *) * static_cast<size_t>(numGroups), std::nothrow));
    if (!l->faceParts)
        return OutOfMemory();
    std::memset(l->faceParts, 0, sizeof(uint16_t *) * static_cast<size_t>(numGroups));
    l->numFaceGroups = numGroups;
    I3D_face_group *fg = ml->faceGroups;
    for (int g = 0; g < numGroups && fg; ++g, fg = fg->next) {
        const int faces = fg->numFaces;
        auto *fp = static_cast<uint16_t *>(::operator new(sizeof(uint16_t) * static_cast<size_t>(faces), std::nothrow));
        if (!fp)
            return OutOfMemory();
        // (Fix: zeroed, and only the group's entries read; the original reads as many entries as
        // the group has faces, leaving faces in no patch unset.)
        std::memset(fp, 0, sizeof(uint16_t) * static_cast<size_t>(faces));
        const std::vector<S_facePart> &list = m_faceParts[g];
        const int entries = std::min(faces, static_cast<int>(list.size()));
        for (int i = 0; i < entries; ++i) {
            if (list[i].face < faces)
                fp[list[i].face] = list[i].part;
        }
        l->SetFaceParts(fp, g);
    }
    if (!l->Build(ml, false, m_object))
        return I3DERR_GENERIC;
    return I3D_OK;
}

}  // namespace

I3D_RESULT I3D_lit_object::Construct(I3D_scene *scene, const S_vector *planeNormal, float resolution,
                                     uint32_t flags, I3D_LIGHTMAP_PROGRESS *progress, void *context)
{
    FreeLightmaps(0);
    if (!mesh)
        return I3DERR_INVALIDPARAMS;
    // (Fix: the original reads through a missing plane normal.)
    if ((flags & LMCONS_ONE_PATCH) && !planeNormal)
        return I3DERR_INVALIDPARAMS;
    ig::g_noActivation = true;
    if (progress)
        progress(8, 0, 0, context);
    S_lightmapSet &set = CurrentSet();
    set.resolution = resolution;
    // The lights of the sector the object is in. (Fix: the original walks past the root when no
    // sector is above it.)
    I3D_frame *sector = parent;
    while (sector && sector->type != FRAME_SECTOR)
        sector = sector->parent;
    if (!sector) {
        ig::g_noActivation = false;
        return I3DERR_INVALIDPARAMS;
    }
    SetLightSector(static_cast<I3D_sector *>(sector));
    C_lightmapBaker baker(this, scene, planeNormal, flags, progress, context);
    for (uint32_t i = 0; i < mesh->numLevels; ++i) {
        S_lightmapLevel *l = set.levels[i];
        if (l) {
            l->Free();                      // (FreeLightmaps deleted them: not reached)
        } else if (!(l = new (std::nothrow) S_lightmapLevel)) {
            // (Fix: on this and the other failures the original leaves g_noActivation set.)
            ig::g_noActivation = false;
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        const I3D_RESULT r = baker.Bake(static_cast<int>(i), mesh->levels[i], l);
        if (r != I3D_OK) {
            if (set.levels[i] == l)
                set.levels[i] = nullptr;
            delete l;
            ig::g_noActivation = false;
            return r;
        }
        set.levels[i] = l;
    }
    set.loaded = true;
    ig::g_noActivation = false;
    lights.stamp = 0;                       // the device lights chosen again
    return I3D_OK;
}
