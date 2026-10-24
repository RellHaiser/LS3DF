// Projected shadows: I3D_shadow (0x1004e5d0 - 0x10050770), I3D_shadow_visual (0x100507e0 -
// 0x10051d20) and the shadow map rasterizer (0x10015c40 - 0x10016390).
#include "i3d/shadow.h"

#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "core/fpu.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/light.h"
#include "i3d/mesh.h"
#include "i3d/object.h"
#include "i3d/proj_geometry.h"
#include "i3d/render_helpers.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/vb_manager.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ig::g_pDevice;

namespace ls3d::i3d {

C_vector<I3D_shadow *> g_shadows;

// The driver's states 0x36 / 0x37 as used (driver_core.cpp).
extern int g_state37Value;

}  // namespace ls3d::i3d

namespace {

// The identity matrix world-space visuals are drawn with (0x1009d270).
const S_matrix &IdentityMatrix()
{
    static S_matrix s_identity = [] {
        S_matrix m;
        m.Identity();
        return m;
    }();
    return s_identity;
}

// The casters SetSource found: a frame and its part (0x100bf394, grow 0x10), the parts' face and
// vertex totals (0x100bf3a4 / 0x100bf3c0) and the parts being matched (0x100bf390, count 0x100bf3a8).
struct S_found {
    I3D_frame *frame;
    int part;
};
C_vector<S_found> s_found(0x10);
int s_foundFaces;
int s_foundVertices;
const S_shadowPart *s_parts;
int s_numParts;

// The shadow visuals to build this frame (0x100bf370).
C_vector<I3D_shadow_visual *> s_visibleShadows;

// The name a part matches (a frame's name without its model prefix; "" without one).
const char *MatchName(const I3D_frame *f) { return f->name ? f->name + f->nameOffset : ""; }

// The frame as a caster when its name matches a part.
bool MatchPart(I3D_frame *f)
{
    for (int i = 0; i < s_numParts; ++i) {
        if (!std::strcmp(MatchName(f), s_parts[i].name)) {
            s_found.PushBack(S_found{f, i});
            s_foundFaces += s_parts[i].numFaces;
            s_foundVertices += s_parts[i].numVertices;
            return true;
        }
    }
    return false;
}

// 0x1004edc0: the frame and its descendants that are on.
void CollectCasters(I3D_frame *f)
{
    if (!(f->frameFlags & FRMFLAGS_ON))
        return;
    MatchPart(f);
    for (I3D_frame *c = f->firstChild; c; c = c->nextSibling)
        CollectCasters(c);
}

// Shadow map rasterization (the driver's, 0x10015c40 - 0x10016390) ------------------------------

// The span of each row of a triangle (0x100be178: 256 rows of min / max x).
uint8_t s_spans[256][2];
// The 2 x 2 supersampled coverage (0x100ae010: 256 x 256 bytes).
uint8_t s_coverage[256][256];
// The points in map coordinates (0x100be030, a static C_vector).
C_vector<int> s_mapPoints;

// The spans of a triangle's three edges (x stepped in 1 / 128 pixels).
void TriangleSpans(const int *points, const uint16_t *face, int &yMin, int &yMax)
{
    const int x0 = points[face[0] * 2], y0 = points[face[0] * 2 + 1];
    const int x1 = points[face[1] * 2], y1 = points[face[1] * 2 + 1];
    const int x2 = points[face[2] * 2], y2 = points[face[2] * 2 + 1];
    for (auto &s : s_spans) {
        s[0] = 0xff;
        s[1] = 0;
    }
    yMin = y0 < y1 ? y0 : y1;
    if (y2 < yMin)
        yMin = y2;
    yMax = y0 > y1 ? y0 : y1;
    if (y2 > yMax)
        yMax = y2;
    auto edge = [](int xa, int ya, int xb, int yb) {
        if (ya == yb)
            return;
        if (yb < ya) {
            std::swap(xa, xb);
            std::swap(ya, yb);
        }
        const int step = (xb - xa) * 128 / (yb - ya);
        int x = xa * 128;
        for (int y = ya; y <= yb; ++y, x += step) {
            const int px = x >> 7;
            if (px < s_spans[y][0])
                s_spans[y][0] = static_cast<uint8_t>(px);
            if (px > s_spans[y][1])
                s_spans[y][1] = static_cast<uint8_t>(px);
        }
    };
    edge(x0, y0, x1, y1);
    edge(x1, y1, x2, y2);
    edge(x2, y2, x0, y0);
}

// 0x10015c40: the triangles filled with 0xF000 (alpha 15) into a locked A4R4G4B4 map. (The last
// row and each span's last pixel are left out.)
void FillTriangles16(const int *points, const uint16_t *faces, int numFaces, uint8_t *bits, int pitch)
{
    for (; numFaces; --numFaces, faces += 3) {
        int yMin, yMax;
        TriangleSpans(points, faces, yMin, yMax);
        uint8_t *row = bits + yMin * pitch;
        for (int y = yMin; y < yMax; ++y, row += pitch) {
            const uint8_t count = static_cast<uint8_t>(s_spans[y][1] - s_spans[y][0]);
            auto *p = reinterpret_cast<uint16_t *>(row) + s_spans[y][0];
            for (int i = 0; i < count; ++i)
                p[i] = 0xf000;
        }
    }
}

// 0x10015e90: the same into the coverage (0xFF).
void FillTriangles8(const int *points, const uint16_t *faces, int numFaces)
{
    for (; numFaces; --numFaces, faces += 3) {
        int yMin, yMax;
        TriangleSpans(points, faces, yMin, yMax);
        for (int y = yMin; y < yMax; ++y) {
            const uint8_t count = static_cast<uint8_t>(s_spans[y][1] - s_spans[y][0]);
            std::memset(&s_coverage[y][s_spans[y][0]], 0xff, count);
        }
    }
}

// 0x100160d0: the triangles of `points` (light space, `bounds` x / y mapped to the map) into the
// map: 2 x 2 supersampled (driver state 0x30) or hard edged.
void RasterizeShadowMap(const float (*points)[2], int numPoints, const uint16_t *faces, int numFaces,
                        ITexture *map, const I3D_bbox &bounds)
{
    s_mapPoints.ResizeDiscard(numPoints * 2);
    int *p = s_mapPoints.Data();
    if (g_driverStates[0x30]) {
        std::memset(s_coverage, 0, static_cast<size_t>(map->height) * 0x200);
        const float sx = static_cast<float>(map->width * 2 - 5) / (bounds.max.x - bounds.min.x);
        const float sy = static_cast<float>(map->height * 2 - 5) / (bounds.max.y - bounds.min.y);
        for (int i = 0; i < numPoints; ++i) {
            p[i * 2] = Fistp((points[i][0] - bounds.min.x) * sx);
            p[i * 2 + 1] = Fistp((points[i][1] - bounds.min.y) * sy);
        }
        FillTriangles8(p, faces, numFaces);
        int pitch;
        auto *bits = static_cast<uint8_t *>(map->Lock(nullptr, &pitch, 0));
        // Each pixel inside the border: the 4 samples' sum / 64 as the alpha. (The rows are
        // counted with the width.)
        const int n = map->width - 2;
        uint8_t *row = bits + pitch + 2;
        for (int y = 0; y < n; ++y, row += pitch) {
            auto *dst = reinterpret_cast<uint16_t *>(row);
            const uint8_t *s0 = s_coverage[y * 2];
            const uint8_t *s1 = s_coverage[y * 2 + 1];
            for (int x = 0; x < n; ++x)
                dst[x] = static_cast<uint16_t>(((s0[x * 2] + s0[x * 2 + 1] + s1[x * 2] + s1[x * 2 + 1]) >> 6) << 12);
        }
        map->Unlock();
        return;
    }
    map->Fill(nullptr, 0, 0);
    const float sx = static_cast<float>(map->width - 1) / (bounds.max.x - bounds.min.x);
    const float sy = static_cast<float>(map->height - 1) / (bounds.max.y - bounds.min.y);
    for (int i = 0; i < numPoints; ++i) {
        p[i * 2] = Fistp((points[i][0] - bounds.min.x) * sx);
        p[i * 2 + 1] = Fistp((points[i][1] - bounds.min.y) * sy);
    }
    int pitch;
    auto *bits = static_cast<uint8_t *>(map->Lock(nullptr, &pitch, 0));
    FillTriangles16(p, faces, numFaces, bits, pitch);
    map->Unlock();
}

// The visual's corners against its volume (local x, y -1 - 1, z 0 - 1): each bit some corner is
// inside of one side.
uint32_t CornerSides(const S_vector4 *corners, const S_matrix &m)
{
    uint32_t sides = 0;
    for (int i = 0; i < 8; ++i) {
        const S_vector p = *reinterpret_cast<const S_vector *>(&corners[i]) * m;
        if (p.x < 1.0f)
            sides |= 0x01;
        if (p.x > -1.0f)
            sides |= 0x02;
        if (p.y < 1.0f)
            sides |= 0x04;
        if (p.y > -1.0f)
            sides |= 0x08;
        if (p.z > 0.0f)
            sides |= 0x10;
        if (p.z < 1.0f)
            sides |= 0x20;
    }
    return sides;
}

// The A component of a D3DCOLOR from a 0 - 1 value.
int Byte255(float v) { return Fistp(v * 255.0f); }

}  // namespace

// S_shadowData ------------------------------------------------------------------------------------

// 0x10050770
S_shadowData::~S_shadowData()
{
    for (S_shadowPart &p : parts)
        ::operator delete(p.name);
}

// I3D_shadow --------------------------------------------------------------------------------------

// 0x1004e5d0
void I3D_shadow::Init()
{
    I3D_frame::Init();
    mapHeight = 0x40;
    mapWidth = 0x40;
    orthogonal = true;
    data = nullptr;
    scene = nullptr;
    lightSector = nullptr;
    localVertices = nullptr;
    worldVertices = nullptr;
    points = nullptr;
    numVertices = 0;
    faces = nullptr;
    numFaces = 0;
    casters = nullptr;
    numCasters = 0;
    texture = nullptr;
    depth = 8.0f;
    opacity = 0.117f;
    lightMatrix.Identity();
    intensity = 1.0f;
    maxHeight = 1.0f;
    mode = 0;
    range = 100.0f;
    color.x = 0.0f;
    color.y = 0.0f;
    color.z = 0.0f;
    casterStamp = -1;
    lightStamp = -1;
    // (w: an unset value in the original.)
    lightDir = {0.0f, 0.0f, 1.0f, 0.0f};
    shadowFlags = 0;
    visual = static_cast<I3D_shadow_visual *>(CreateVisualObject(11));
    if (!visual) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    visual->owner = this;
    visual->visualFlags |= VISF_NO_ZWRITE;
    g_shadows.PushBack(this);
}

// 0x1004e7a0
void I3D_shadow::Uninit()
{
    Close();
    if (scene)
        scene->RemoveShadow(this);
    if (visual)
        visual->Release();
    g_shadows.RemoveSwap(this);
    I3D_frame::Uninit();
}

// 0x1004e840
I3D_RESULT I3D_shadow::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    Close();
    if (source->type != FRAME_SHADOW)
        return I3D_OK;
    auto *src = static_cast<I3D_shadow *>(source);
    if (src->data) {
        data = src->data;
        ++data->refCount;
    }
    maxHeight = src->maxHeight;
    depth = src->depth;
    mapWidth = src->mapWidth;
    mapHeight = src->mapHeight;
    range = src->range;
    SetOpacity(src->opacity);
    SetColor(src->color);
    SetMode(src->mode);
    SetOrthogonal(src->orthogonal);
    return I3D_OK;
}

// 0x1004e970
void I3D_shadow::SetSource(I3D_frame *source)
{
    if (!source && scene)
        scene->RemoveShadow(this);
    if (visual && visual->parent)
        visual->LinkTo(nullptr, 0);
    for (int i = 0; i < numCasters; ++i) {
        if (casters[i].frame)
            casters[i].frame->frameFlags &= ~FRMFLAGS_SHADOW_CASTER;
    }
    ::operator delete(casters);
    casters = nullptr;
    numCasters = 0;
    AlignedFree(localVertices);
    localVertices = nullptr;
    worldVertices = nullptr;
    ::operator delete(points);
    points = nullptr;
    numVertices = 0;
    ::operator delete(faces);
    casterStamp = -1;
    lightStamp = -1;
    faces = nullptr;
    numFaces = 0;
    shadowFlags = 0;
    if (!data || !source)
        return;
    s_found.Clear();
    s_foundVertices = 0;
    s_foundFaces = 0;
    s_parts = data->parts.Data();
    s_numParts = data->parts.Size();
    if (!(source->frameFlags & FRMFLAGS_ON))
        return;
    MatchPart(source);
    for (I3D_frame *c = source->firstChild; c; c = c->nextSibling)
        CollectCasters(c);
    if (!s_foundFaces)
        return;
    faces = static_cast<uint16_t *>(::operator new(static_cast<size_t>(s_foundFaces) * 6));
    localVertices = static_cast<S_vector4 *>(AlignedAlloc(static_cast<size_t>(s_foundVertices) * 32, 16));
    points = static_cast<float(*)[2]>(::operator new(static_cast<size_t>(s_foundVertices) * 8));
    auto *found = static_cast<S_shadowCaster *>(::operator new(static_cast<size_t>(s_found.Size()) * 8));
    casters = found;
    if (!faces || !localVertices || !points || !found) {
        AlignedFree(localVertices);
        localVertices = nullptr;
        ::operator delete(points);
        points = nullptr;
        ::operator delete(faces);
        faces = nullptr;
        ::operator delete(casters);
        casters = nullptr;
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    worldVertices = localVertices + s_foundVertices;
    numVertices = s_foundVertices;
    numFaces = s_foundFaces;
    numCasters = s_found.Size();
    uint16_t *f = faces;
    S_vector4 *v = localVertices;
    int base = 0;
    for (const S_found &c : s_found) {
        int vertexOffset = 0, faceOffset = 0;
        for (int i = 0; i < c.part; ++i) {
            vertexOffset += s_parts[i].numVertices;
            faceOffset += s_parts[i].numFaces;
        }
        const S_shadowPart &part = s_parts[c.part];
        const uint16_t *src = data->indices.Data() + faceOffset * 3;
        for (int k = part.numFaces * 3; k; --k)
            *f++ = static_cast<uint16_t>(*src++ + base);
        std::memcpy(v, data->vertices.Data() + vertexOffset, static_cast<size_t>(part.numVertices) * sizeof(S_vector4));
        v += part.numVertices;
        found->frame = c.frame;
        found->numVertices = part.numVertices;
        ++found;
        base += part.numVertices;
        c.frame->frameFlags |= FRMFLAGS_SHADOW_CASTER;
    }
}

// 0x1004ef20
I3D_RESULT I3D_shadow::LinkTo(I3D_frame *newParent, uint32_t flags)
{
    SetSource(newParent);
    return I3D_frame::LinkTo(newParent, flags);
}

// 0x1004ef50
void I3D_shadow::SetMode(int newMode)
{
    mode = newMode;
    shadowFlags = 0;
    lightStamp = -1;
}

// 0x1004ef80
void I3D_shadow::MatrixChanged()
{
    if (scene)
        return;
    // (Every sector above joins, not only the nearest.)
    for (I3D_frame *p = parent; p; p = p->parent) {
        if (p->type == FRAME_SECTOR) {
            static_cast<I3D_sector *>(p)->scene->AddShadow(this);
            shadowFlags = 0;
            lightStamp = -1;
        }
    }
}

// 0x1004f030
void I3D_shadow::Close()
{
    for (int i = 0; i < numCasters; ++i) {
        if (casters[i].frame)
            casters[i].frame->frameFlags &= ~FRMFLAGS_SHADOW_CASTER;
    }
    ::operator delete(casters);
    casters = nullptr;
    numCasters = 0;
    if (data && --data->refCount == 0)
        delete data;
    data = nullptr;
    AlignedFree(localVertices);
    localVertices = nullptr;
    numVertices = 0;
    ::operator delete(points);
    points = nullptr;
    ::operator delete(faces);
    faces = nullptr;
    numFaces = 0;
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
    shadowFlags = 0;
    casterStamp = -1;
    lightStamp = -1;
}

// 0x1004f120
I3D_RESULT I3D_shadow::Open(const char *fileName, uint32_t flags)
{
    Close();
    auto *d = new (std::nothrow) S_shadowData;
    if (d) {
        d->parts.Reserve(20);
        d->vertices.Reserve(160);
        d->indices.Reserve(240);
    }
    data = d;
    if (!d)
        return I3DERR_OUTOFMEM;
    const I3D_RESULT r = OpenShadowFile(fileName, flags, d);
    if (r < 0)
        return r;
    if (parent)
        SetSource(parent);
    return I3D_OK;
}

// 0x1004f340
void I3D_shadow::SetDepth(float newDepth)
{
    if (newDepth > 0.0f)
        depth = newDepth;
    lightDir.x = 0.0f;
    lightDir.y = 0.0f;
    lightDir.z = 0.0f;
    shadowFlags = 0;
}

// 0x1004f380
I3D_RESULT I3D_shadow::SetMaxHeight(float height)
{
    if (!(height >= 0.0f) || !(height <= 300.0f))
        return I3DERR_INVALIDPARAMS;
    maxHeight = height;
    lightDir.x = 0.0f;
    lightDir.y = 0.0f;
    lightDir.z = 0.0f;
    lightStamp = -1;
    shadowFlags = 0;
    return I3D_OK;
}

// 0x1004f3f0
void I3D_shadow::SetColor(const S_vector4 &c)
{
    color = c;
    if (color.x < 0.0f)
        color.x = 0.0f;
    else if (color.x > 1.0f)
        color.x = 1.0f;
    if (color.y < 0.0f)
        color.y = 0.0f;
    else if (color.y > 1.0f)
        color.y = 1.0f;
    if (color.z < 0.0f)
        color.z = 0.0f;
    else if (color.z > 1.0f)
        color.z = 1.0f;
    shadowFlags = 0;
    lightStamp = -1;
}

// 0x1004f4c0
void I3D_shadow::SetOpacity(float value)
{
    opacity = value;
    if (value < 0.0f) {
        opacity = 0.0f;
        shadowFlags = 0;
        return;
    }
    shadowFlags = 0;
    if (value > 1.0f)
        opacity = 1.0f;
}

// 0x1004f520
void I3D_shadow::SetOrthogonal(bool on)
{
    orthogonal = on;
    lightStamp = -1;
    lightDir.x = 0.0f;
    lightDir.y = 0.0f;
    lightDir.z = 0.0f;
    shadowFlags = 0;
}

// 0x1004f560
I3D_RESULT I3D_shadow::SetSize(int width, int height)
{
    if (width > 0x80 || height > 0x80)
        return I3DERR_INVALIDPARAMS;
    mapWidth = 4;
    while (mapWidth < width)
        mapWidth *= 2;
    // (The height follows the width too.)
    mapHeight = 4;
    while (mapHeight < width)
        mapHeight *= 2;
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
    lightStamp = -1;
    shadowFlags = 0;
    return I3D_OK;
}

namespace {

// 0x1004f5f0: the casters' world vertices, when they moved (or, from mode 1, once).
bool UpdateCasters(I3D_shadow *s)
{
    if (!s->data || !s->localVertices || !s->numCasters || !s->casters)
        return false;
    if (s->mode && (s->shadowFlags & SHDF_CASTERS_VALID))
        return true;
    int stamp = 0;
    for (int i = 0; i < s->numCasters; ++i) {
        if (s->casters[i].frame)
            stamp += s->casters[i].frame->changeCount;
    }
    if (s->casterStamp == stamp)
        return true;
    s->casterStamp = stamp;
    const S_vector4 *src = s->localVertices;
    S_vector4 *dst = s->worldVertices;
    for (int i = 0; i < s->numCasters; ++i) {
        I3D_frame *f = s->casters[i].frame;
        // (The vertices of a caster gone away are not skipped: the rest shift onto them.)
        if (!f)
            continue;
        const S_matrix &m = f->GetMatrix();
        for (int k = s->casters[i].numVertices; k; --k, ++src, ++dst) {
            const S_vector p = *reinterpret_cast<const S_vector *>(src) * m;
            dst->x = p.x;
            dst->y = p.y;
            dst->z = p.z;
        }
    }
    s->shadowFlags = (s->shadowFlags & ~SHDF_FAILED) | SHDF_CASTERS_VALID | SHDF_MOVED;
    return true;
}

// A light's direction into the shadow's (unit) when it cast the shadow.
void NormalizeLightDir(S_vector &d, float sq) { NormalizeWithSquare(d, sq); }

// 0x1004f760: the strongest shadow-casting light of the sector above the shadow (point / spot
// lights by attenuation and cone, directional ones by their color, ambient ones only adding to
// the total) gives the intensity (its share of the light, faded when a second light is nearly as
// strong) and the direction; the casters go into light space and the visual becomes the volume.
bool UpdateLight(I3D_shadow *s)
{
    if (s->mode >= 2 && (s->shadowFlags & SHDF_LIGHT_VALID))
        return true;
    I3D_frame *p = s->parent;
    while (p && p->type != FRAME_SECTOR)
        p = p->parent;
    auto fail = [s] {
        s->intensity = 0.0f;
        s->shadowFlags |= SHDF_FAILED | SHDF_LIGHT_VALID | SHDF_FADED | SHDF_LIGHT_CHANGED;
        return false;
    };
    if (!p)
        return fail();
    if (s->lightSector != p) {
        s->lightSector = static_cast<I3D_sector *>(p);
        s->lightStamp = -1;
    }
    const int stamp = s->lightSector->lightChanges + s->changeCount;
    if (stamp != s->lightStamp) {
        s->lightStamp = stamp;
        s->shadowFlags &= ~SHDF_LIGHT_VALID;
    }
    const S_matrix &sm = s->GetMatrix();
    const S_vector pos = sm(3);
    if (!(s->shadowFlags & SHDF_LIGHT_VALID)) {
        I3D_light *const *lights = s->lightSector->lights.Data();
        int count = s->lightSector->lights.Size();
        float best = 0.0f;                  // the strongest light's intensity
        float second = 0.0f;                // the next one's
        float total = 0.0f;
        I3D_light *bestLight = nullptr;
        if (!count)
            return fail();
        for (; count; --count, ++lights) {
            I3D_light *l = *lights;
            if (!(l->mode & LIGHTMODE_SHADOW) || !(l->frameFlags & FRMFLAGS_ON))
                continue;
            const S_vector &c = l->effectiveColor;
            float value;
            switch (l->lightType) {
            case I3DLIGHT_AMBIENT:
                total = ((c.z + c.y) + c.x) + total;
                continue;
            case I3DLIGHT_SPOT: {
                const S_vector lp = l->GetMatrix()(3);
                const S_vector d(pos.x - lp.x, pos.y - lp.y, pos.z - lp.z);
                const float sq = SquareXZY(d);
                const float r = l->d3dLight.Range;
                if (r * r < sq || r * r == sq)
                    continue;
                value = (c.z + c.y) + c.x;
                if (IsTinyBits(sq))
                    break;
                const float dist = std::sqrt(sq);
                value = value / ((dist * l->d3dLight.Attenuation1 + sq * l->d3dLight.Attenuation2) +
                                 l->d3dLight.Attenuation0);
                S_vector dir = l->GetMatrix()(2);
                NormalizeWithSquare(dir, SquareXYZ(dir));
                const float cosine = ((dir.x * d.x + dir.z * d.z) + dir.y * d.y) / dist;
                if (cosine < l->coneOuterCos || cosine == l->coneOuterCos)
                    continue;
                if (cosine < l->coneInnerCos || cosine == l->coneInnerCos)
                    value = value * ((cosine - l->coneOuterCos) * l->coneFalloff);
                break;
            }
            case I3DLIGHT_POINT: {
                const S_vector lp = l->GetMatrix()(3);
                const S_vector d(lp.x - pos.x, lp.y - pos.y, lp.z - pos.z);
                const float sq = SquareZYX(d);
                const float r = l->d3dLight.Range;
                if (r * r < sq || r * r == sq)
                    continue;
                value = ((c.z + c.y) + c.x) / ((std::sqrt(sq) * l->d3dLight.Attenuation1 +
                                                sq * l->d3dLight.Attenuation2) +
                                               l->d3dLight.Attenuation0);
                break;
            }
            case I3DLIGHT_DIRECTIONAL:
                value = (c.z + c.y) + c.x;
                break;
            default:
                continue;
            }
            total = total + value;
            if (second < value) {
                if (best < value) {
                    second = best;
                    best = value;
                    bestLight = l;
                } else {
                    second = value;
                }
            }
        }
        if (total < 0.00390625f)
            return fail();
        if (!bestLight) {
            s->intensity = 0.0f;
            s->shadowFlags |= SHDF_FAILED | SHDF_LIGHT_VALID | SHDF_FADED | SHDF_LIGHT_CHANGED;
            return false;
        }
        float value = ((best / total) * best) * 0.333333343f;
        const float close = best * 0.949999988f;
        if (second > close)
            value = (1.0f - (second - close) / (best * 0.0500000119f)) * value;
        if (value < 0.00390625f) {
            s->shadowFlags |= SHDF_FAILED | SHDF_LIGHT_VALID | SHDF_FADED | SHDF_LIGHT_CHANGED;
            s->intensity = 0.0f;
            return false;
        }
        s->shadowFlags &= ~SHDF_FAILED;
        if (value > 1.0f)
            value = 1.0f;
        if (!IsTinyBits(std::fabs(s->intensity - value))) {
            s->intensity = value;
            s->shadowFlags |= SHDF_FADED;
        }
        S_vector dir;
        float dirW = 0.0f;
        if (bestLight->lightType == I3DLIGHT_DIRECTIONAL) {
            const S_matrix &lm = bestLight->GetMatrix();
            dir = lm(2);
            dirW = lm.m[2][3];
            NormalizeWithSquare(dir, SquareYZX(dir));
        } else {
            const S_vector lp = bestLight->GetMatrix()(3);
            dir = S_vector(pos.x - lp.x, pos.y - lp.y, pos.z - lp.z);
            NormalizeWithSquare(dir, SquareZYX(dir));
        }
        const float change = (std::fabs(dir.z - s->lightDir.z) + std::fabs(dir.y - s->lightDir.y)) +
                             std::fabs(dir.x - s->lightDir.x);
        if (IsTinyBits(change)) {
            s->shadowFlags |= SHDF_LIGHT_VALID;
        } else {
            s->lightDir = {dir.x, dir.y, dir.z, dirW};
            S_matrix m;
            m.SetDir(*reinterpret_cast<const S_vector *>(&s->lightDir), S_vector(0.0f, 1.0f, 0.0f));
            s->lightMatrix.Inverse(m);
            s->shadowFlags |= SHDF_LIGHT_VALID | SHDF_LIGHT_CHANGED;
        }
    }
    if ((s->shadowFlags & SHDF_LIGHT_VALID) && !(s->shadowFlags & (SHDF_LIGHT_CHANGED | SHDF_MOVED)))
        return !(s->shadowFlags & SHDF_FAILED);
    // The casters in light space: their x / y for the map, the box around them.
    I3D_bbox &b = s->bounds;
    S_vector q = reinterpret_cast<const S_vector *>(s->worldVertices)->RotateByMatrix(s->lightMatrix);
    s->points[0][0] = q.x;
    b.max.x = b.min.x = q.x;
    s->points[0][1] = q.y;
    b.max.y = b.min.y = q.y;
    b.min.z = q.z;
    for (int i = 1; i < s->numVertices; ++i) {
        q = reinterpret_cast<const S_vector *>(&s->worldVertices[i])->RotateByMatrix(s->lightMatrix);
        s->points[i][0] = q.x;
        s->points[i][1] = q.y;
        if (q.x < b.min.x)
            b.min.x = q.x;
        else if (q.x > b.max.x)
            b.max.x = q.x;
        if (q.y < b.min.y)
            b.min.y = q.y;
        else if (q.y > b.max.y)
            b.max.y = q.y;
        if (q.z < b.min.z)
            b.min.z = q.z;
    }
    if (IsTinyBits(std::fabs(b.max.x - b.min.x)) || IsTinyBits(std::fabs(b.max.y - b.min.y)))
        return fail();
    // The volume starts at most maxHeight from the frame's own depth along the light.
    const S_matrix &lm = s->lightMatrix;
    const float base = (lm.m[2][2] * pos.z + lm.m[1][2] * pos.y) + lm.m[0][2] * pos.x;
    const float below = b.min.z - base;
    if (std::fabs(below) > s->maxHeight)
        b.min.z = (below > 0.0f ? s->maxHeight : -s->maxHeight) + base;
    I3D_shadow_visual *v = s->visual;
    v->SetDir(*reinterpret_cast<const S_vector *>(&s->lightDir), 0.0f);
    v->scale = S_vector((b.max.x - b.min.x) * 0.5f, (b.max.y - b.min.y) * 0.5f, s->depth);
    // (scaleW gets an unset value in the original.)
    if (!(v->frameFlags & FRMFLAGS_ROT_QUAT_VALID))
        v->rot.Make(v->localMatrix);
    v->frameFlags = (v->frameFlags & ~0x134u) | FRMFLAGS_CHANGED | FRMFLAGS_ROT_QUAT_VALID;
    S_matrix toWorld;
    toWorld.SetDir3(*reinterpret_cast<const S_vector *>(&s->lightDir), S_vector(0.0f, 1.0f, 0.0f));
    const S_vector center =
        S_vector((b.max.x + b.min.x) * 0.5f, (b.min.y + b.max.y) * 0.5f, b.min.z).RotateByMatrix(toWorld);
    if (!(v->frameFlags & FRMFLAGS_POS_LOCKED)) {
        v->localMatrix(3) = center;
        v->frameFlags = (v->frameFlags & ~0x120u) | FRMFLAGS_CHANGED;
    }
    v->MatrixChanged();
    return !(s->shadowFlags & SHDF_FAILED);
}

// 0x100504c0: the casters into the map when the casters or the light changed (from mode 2: once).
bool UpdateMap(I3D_shadow *s)
{
    const uint32_t f = s->shadowFlags;
    if ((f & SHDF_MAP_VALID) && (!(f & (SHDF_LIGHT_CHANGED | SHDF_MOVED)) || s->mode >= 2))
        return true;
    if (f & SHDF_FAILED)
        return false;
    if (!s->texture) {
        ITexture *t = g_pIGraph->CreateITexture();
        s->texture = t;
        if (t->Create(nullptr, nullptr, ITC_EMPTY | ITC_USE_ALPHA, static_cast<uint16_t>(s->mapWidth),
                      static_cast<uint16_t>(s->mapHeight), 0) < 0) {
            s->shadowFlags |= SHDF_FAILED;
            return false;
        }
        g_driver->SlotB4();
    }
    RasterizeShadowMap(s->points, s->numVertices, s->faces, s->numFaces, s->texture, s->bounds);
    s->shadowFlags |= SHDF_MAP_VALID | SHDF_RASTERIZED;
    return true;
}

// The visual taken out of its sector.
void Unlink(I3D_shadow_visual *v)
{
    if (v && v->parent)
        v->LinkTo(nullptr, 0);
}

}  // namespace

namespace ls3d::i3d {

void ShadowSetSource(I3D_frame *shadow, I3D_frame *source) { static_cast<I3D_shadow *>(shadow)->SetSource(source); }

// 0x1004efd0
void RemoveShadowCaster(I3D_frame *frame)
{
    for (I3D_shadow *s : g_shadows) {
        for (int i = 0; i < s->numCasters; ++i) {
            if (s->casters[i].frame && s->casters[i].frame == frame) {
                frame->frameFlags &= ~FRMFLAGS_SHADOW_CASTER;
                s->casters[i].frame = nullptr;
            }
        }
    }
}

I3D_frame *NewShadowFrame() { return new I3D_shadow; }

// 0x10050590: before the sector pass: a shadow near enough to the camera is brought up to date
// and its visual put into the scene's omni sector; else the visual is taken out.
void PrepareShadow(I3D_shadow *s)
{
    if (!s->parent) {
        if (s->scene)
            s->scene->RemoveShadow(s);
        Unlink(s->visual);
        s->shadowFlags = 0;
        return;
    }
    if (!(s->frameFlags & FRMFLAGS_ON) || !(s->parent->frameFlags & FRMFLAGS_ON)) {
        Unlink(s->visual);
        s->shadowFlags = 0;
        return;
    }
    if (!s->data)
        return;
    const float range = g_fovScale * s->range;
    I3D_camera *cam = g_renderCamera;
    const S_vector cp = cam->GetMatrix()(3);
    const S_vector sp = s->GetMatrix()(3);
    const float dx = sp.x - cp.x, dy = sp.y - cp.y, dz = sp.z - cp.z;
    const float sq = (dz * dz + dx * dx) + dy * dy;
    if (range * range < sq) {
        Unlink(s->visual);
        s->shadowFlags &= ~SHDF_BUILT;
        return;
    }
    if (s->mode == 3 && (s->shadowFlags & SHDF_BUILT))
        return;
    if (!s->scene) {
        s->MatrixChanged();
        if (!s->scene)
            return;
    }
    s->shadowFlags &= ~0x1fu;
    if (!UpdateCasters(s) || !UpdateLight(s) || !UpdateMap(s)) {
        Unlink(s->visual);
        s->shadowFlags &= ~SHDF_BUILT;
        return;
    }
    if (!s->visual->parent)
        s->visual->LinkTo(s->scene->omniSector, 0);
}

// 0x10050cc0: after the visuals: the shadows queued by their visuals built.
void RenderShadows()
{
    for (I3D_shadow_visual *v : s_visibleShadows)
        v->Build();
    s_visibleShadows.Clear();
}

}  // namespace ls3d::i3d

// I3D_shadow_visual -------------------------------------------------------------------------------

// 0x100507e0
void I3D_shadow_visual::Init()
{
    I3D_visual::Init();
    vertices = nullptr;
    vertexCapacity = 0;
    numVertices = 0;
    faceIndices = nullptr;
    faceCapacity = 0;
    numOpaqueFaces = 0;
    numBlendedFaces = 0;
    center = {0.0f, 0.0f, 0.0f, 0.0f};
    owner = nullptr;
    numReceivers = 0;
    receiverStamp = 0;
    vb = {};
    ib = {};
    // The volume: x and y -1 - 1, z 0 - 1. (The boxes' w: unset values in the original.)
    localBound.bbox.min = S_vector(-1.0f, -1.0f, 0.0f);
    localBound.bbox.max = S_vector(1.0f, 1.0f, 1.0f);
    localBound.center = S_vector(0.0f, 0.0f, 0.5f);
    localBound.radius = 1.5f;
}

// 0x10050920
void I3D_shadow_visual::Uninit()
{
    ::operator delete(vertices);
    ::operator delete(faceIndices);
    FreeVB(&vb);
    FreeIB(&ib);
    I3D_frame::Uninit();
}

// 0x100509c0
void I3D_shadow_visual::Prepare()
{
    if (!g_driverStates[DRVSTATE_SHADOWS]) {
        LinkTo(nullptr, 0);
        if (owner)
            owner->shadowFlags = 0;
        return;
    }
    if (owner->mode == 3 && (owner->shadowFlags & SHDF_BUILT)) {
        if (!numVertices)
            return;
        if (vb.vb) {
            QueueItems();
            return;
        }
        owner->shadowFlags &= ~SHDF_BUILT;
    }
    s_visibleShadows.PushBack(this);
}

// 0x10050ac0
void I3D_shadow_visual::QueueItems()
{
    if (!numVertices)
        return;
    if (numOpaqueFaces) {
        S_renderItem &item = g_rc.items.PushBackUninit();
        item.visual = this;
        item.sector = owner->lightSector;
        item.texture = owner->texture;
        item.sortKey = 0xbfffffffu;
        ++g_rc.numBlended;
    }
    if (numBlendedFaces) {
        S_renderItem &item = g_rc.items.PushBackUninit();
        item.visual = this;
        item.sector = owner->lightSector;
        item.texture = owner->texture;
        const I3D_camera *cam = g_renderCamera;
        const S_vector &n = cam->frustum[0].normal;
        const float d = ((center.z * n.z + center.y * n.y) + center.x * n.x) + cam->frustumD[0];
        item.sortKey = (static_cast<uint32_t>(Fistp(d * 100.0f)) & RIKEY_DEPTH_MASK) | RIKEY_BLENDED;
        ++g_rc.numBlended;
    }
}

// 0x10050cf0
void I3D_shadow_visual::Build()
{
    Update();
    GetWorldBound();
    if (!owner->lightSector)
        return;
    const int mask = owner->lightSector->environmentId;
    auto finish = [this] { owner->shadowFlags = (owner->shadowFlags & ~0x1fu) | SHDF_BUILT; };
    // The meshes queued this frame that take shadows (mode A), in the shadow's sector or one of
    // the same environment, whose world box meets the volume's and that are not under the
    // shadow's own model.
    int n = 0;
    I3D_visual *prev = nullptr;
    const S_renderItem *item = g_rc.items.Data();
    for (int i = g_rc.items.Size(); i; --i, ++item) {
        I3D_visual *v = item->visual;
        if (v == prev)
            continue;
        prev = v;
        if (!v)
            continue;
        const uint32_t kind = v->GetVisualType();
        if (kind != FourCC("LMAP") && kind != FourCC("MRPH") && kind != FourCC("SNGM") && kind != FourCC("SMMR") &&
            kind != FourCC("OBJ_"))
            continue;
        if (!(v->visualFlags & VISF_MODE_A1))
            continue;
        if (item->sector != owner->lightSector && !(item->sector->environmentId & mask))
            continue;
        const I3D_bbox &r = v->GetWorldBound();
        const I3D_bbox &s = worldBound.bbox;
        if (r.min.x > s.max.x || r.min.y > s.max.y || r.min.z > s.max.z)
            continue;
        if (r.max.x < s.min.x || r.max.y < s.min.y || r.max.z < s.min.z)
            continue;
        bool own = false;
        for (I3D_frame *f = v; f; f = f->parent) {
            if (f == owner->parent) {
                own = true;
                break;
            }
            if (f->type == FRAME_MODEL || f->type == FRAME_SECTOR)
                break;
        }
        if (own)
            continue;
        proj::g_receivers[n++] = v;
        if (n >= 0x200)
            break;
    }
    if (!n) {
        numReceivers = 0;
        numVertices = 0;
        numOpaqueFaces = 0;
        numBlendedFaces = 0;
        finish();
        return;
    }
    int stamp = 0;
    for (int i = 0; i < n; ++i) {
        const auto *o = static_cast<const I3D_object *>(proj::g_receivers[i]);
        stamp += o->changeCount + ((o->visualFlags & VISF_TEXTURE_LOD) ? 0 : o->textureLevel);
    }
    if (n != numReceivers || stamp != receiverStamp) {
        numReceivers = n;
        receiverStamp = stamp;
        owner->shadowFlags |= SHDF_RECEIVERS_CHANGED;
    }
    if ((owner->shadowFlags & SHDF_BUILT) && !(owner->shadowFlags & 0x1e)) {
        if (numVertices)
            QueueItems();
        return;
    }
    // The volume's space: x / y -1 - 1, z 0 - 1 along the light.
    const S_matrix *inv;
    if (visualFlags & VISF_WORLD_SPACE) {
        inv = &IdentityMatrix();
    } else {
        if (!(visualFlags & VISF_INVMATRIX_VALID)) {
            visualFlags |= VISF_INVMATRIX_VALID;
            invWorldMatrix.Inverse(GetMatrix());
        }
        inv = &invWorldMatrix;
    }
    proj::Begin();
    const float bias = DriverStateFloat(0x3d);
    const S_matrix *world = (visualFlags & VISF_WORLD_SPACE) ? &IdentityMatrix() : &GetMatrix();
    proj::Setup(inv, world, owner->orthogonal, bias, bias, owner->opacity);
    for (int i = 0; i < n; ++i) {
        auto *r = static_cast<I3D_object *>(proj::g_receivers[i]);
        const I3D_bound_volume &lb = r->GetLocalBound();
        const S_vector4 *corners = BoxCorners(lb.bbox);
        const S_matrix m = r->GetMatrix() * *inv;
        if (CornerSides(corners, m) != 0x3f)
            continue;
        I3D_mesh *mesh = r->mesh;
        if (!mesh)
            continue;
        I3D_mesh_level *level = mesh->GetLevel((r->visualFlags & VISF_TEXTURE_LOD) ? 0 : r->textureLevel);
        if (!level)
            continue;
        const uint32_t kind = r->GetVisualType();
        if (kind == FourCC("SNGM") || kind == FourCC("SMMR")) {
            const S_vector4 *skinned =
                SkinnedVertices(r, (r->visualFlags & VISF_TEXTURE_LOD) ? 0 : r->textureLevel);
            if (!skinned)
                continue;
            const uint32_t blended = r->visualFlags & VISF_MODE_B2;
            if (r->visualFlags & VISF_WORLD_SPACE)
                proj::ProjectWorldMesh4(skinned, level->numVertices, level->faceGroups, level->NumFaces(), blended);
            else
                proj::ProjectMesh4(skinned, level->numVertices, level->faceGroups, level->NumFaces(), blended,
                                   &r->GetMatrix());
        } else {
            const uint32_t blended = r->visualFlags & VISF_MODE_A2;
            const int numFaces = level->NumFaces();
            const void *verts = level->LockVertices(1);
            if (r->visualFlags & VISF_WORLD_SPACE)
                proj::ProjectWorldMesh(verts, level->numVertices, level->faceGroups, numFaces, blended);
            else
                proj::ProjectMesh(verts, level->numVertices, level->faceGroups, numFaces, blended, &r->GetMatrix());
        }
    }
    const int nv = proj::NumVertices();
    if (!nv) {
        numVertices = 0;
        numOpaqueFaces = 0;
        numBlendedFaces = 0;
        finish();
        return;
    }
    S_vertex *dst;
    if (owner->mode == 3) {
        if (vertices) {
            ::operator delete(vertices);
            vertices = nullptr;
            vertexCapacity = 0;
            ::operator delete(faceIndices);
            faceIndices = nullptr;
            faceCapacity = 0;
        }
        if (vertexCapacity < nv || !vb.vb) {
            FreeVB(&vb);
            vertexCapacity = nv + 8;
            if (!g_vbManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, sizeof(S_vertex), nv + 8, &vb, 2, false)) {
                vertexCapacity = 0;
                vb.buffer = nullptr;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return;
            }
        }
        dst = reinterpret_cast<S_vertex *>(vb.buffer->Lock(0) + vb.buffer->stride * vb.offset);
    } else {
        if (vb.buffer) {
            FreeVB(&vb);
            vb.buffer = nullptr;
            vertexCapacity = 0;
            FreeIB(&ib);
            ib.buffer = nullptr;
            faceCapacity = 0;
        }
        if (vertexCapacity < nv) {
            ::operator delete(vertices);
            vertexCapacity = nv + 0x10 < 100 ? 100 : nv + 0x10;
            vertices = static_cast<S_vertex *>(::operator new(static_cast<size_t>(vertexCapacity) * sizeof(S_vertex)));
            if (!vertices) {
                vertexCapacity = 0;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return;
            }
        }
        dst = vertices;
    }
    // The projected positions; u / v across the volume, the alpha fading along it.
    numVertices = nv;
    const S_vector4 *src = proj::Vertices();
    for (int i = 0; i < nv; ++i, ++src, ++dst) {
        dst->pos = S_vector(src->x, src->y, src->z);
        dst->u = (src->x + 1.0f) * 0.5f;
        dst->v = (src->y + 1.0f) * 0.5f;
        const int a = Byte255((1.0f - src->z) * owner->intensity);
        const int r = Byte255(owner->color.x);
        const int g = Byte255(owner->color.y);
        const int b = Byte255(owner->color.z);
        dst->color = static_cast<uint32_t>(((a << 8 | r) << 8 | g) << 8 | b);
    }
    const int opaque = proj::NumOpaqueFaces();
    const int blendedFaces = proj::NumBlendedFaces();
    uint16_t *fdst;
    if (owner->mode == 3) {
        if (faceCapacity < blendedFaces + opaque) {
            FreeIB(&ib);
            faceCapacity = blendedFaces + opaque + 4;
            if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, static_cast<uint32_t>(faceCapacity) * 3, &ib)) {
                ib.buffer = nullptr;
                faceCapacity = 0;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return;
            }
        }
        fdst = ib.buffer->Lock(0) + ib.offset;
    } else {
        if (faceCapacity < blendedFaces + opaque) {
            ::operator delete(faceIndices);
            faceCapacity = blendedFaces + opaque + 0x10 < 100 ? 100 : blendedFaces + opaque + 0x10;
            faceIndices = static_cast<uint16_t *>(::operator new(static_cast<size_t>(faceCapacity) * 6));
            if (!faceIndices) {
                faceCapacity = 0;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return;
            }
        }
        fdst = faceIndices;
    }
    std::memcpy(fdst, proj::OpaqueFaces(), static_cast<size_t>(opaque) * 6);
    numOpaqueFaces = opaque;
    std::memcpy(fdst + opaque * 3, proj::BlendedFaces(), static_cast<size_t>(blendedFaces) * 6);
    numBlendedFaces = blendedFaces;
    if (proj::BlendedSumCount() > 0) {
        const S_matrix &m = (visualFlags & VISF_WORLD_SPACE) ? IdentityMatrix() : GetMatrix();
        const float invCount = 1.0f / static_cast<float>(proj::BlendedSumCount());
        const S_vector &s = proj::BlendedSum();
        const S_vector c = S_vector(s.x * invCount, s.y * invCount, s.z * invCount) * m;
        center.x = c.x;
        center.y = c.y;
        center.z = c.z;
    }
    QueueItems();
    finish();
}

// 0x100519b0
void I3D_shadow_visual::Draw(const S_renderItem *item)
{
    SetColorOp(0, D3DTOP_MODULATE);
    SetColorOp(1, D3DTOP_DISABLE);
    SetAlphaOp(0, D3DTOP_MODULATE);
    SetAlphaState(1);
    SetBlendState(0);
    SetCullState(3);
    g_pDevice->SetTexture(0, owner->texture->d3dTexture);
    g_pIGraph->SetWorldMatrix(&GetMatrix());
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    SetZBias(g_state37Value);
    // (Both queued items carry the map: each draws the opaque materials' part; the blended part
    // is drawn only for an item without a texture.)
    const bool first = item->texture != nullptr;
    if (owner->mode == 3) {
        SetVertexFormat(2);
        g_pDevice->SetStreamSource(0, vb.vb, sizeof(S_vertex));
        if (first) {
            g_pDevice->SetIndices(ib.ib, vb.offset);
            g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, numVertices, ib.offset, numOpaqueFaces);
        } else {
            g_pDevice->SetIndices(ib.ib, numOpaqueFaces * 3 + vb.offset);
            g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, numVertices, ib.offset, numBlendedFaces);
        }
    } else if (first) {
        g_pIGraph->DrawIndexedPrimitive(IGP_TRIANGLES, numVertices, faceIndices, numOpaqueFaces, vertices, IGV_COLOR_TEX);
    } else {
        g_pIGraph->DrawIndexedPrimitive(IGP_TRIANGLES, numVertices, faceIndices + numOpaqueFaces * 3, numBlendedFaces,
                                        vertices, IGV_COLOR_TEX);
    }
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, TRUE);
}
