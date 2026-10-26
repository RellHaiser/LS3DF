// I3D_lens (0x10007dd0 - 0x10009610; 0x100097f0 / 0x10009810 are the static initialisers of this
// file's copies of a header's zero vector and identity quaternion).
#include "i3d/lens.h"

#include <cmath>
#include <cstdlib>

#include "core/fpu.h"
#include "external/rw_data.h"
#include "i3d/camera.h"
#include "i3d/collision.h"
#include "i3d/loader.h"
#include "i3d/material.h"
#include "i3d/render_helpers.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;
using ig::g_viewport;

namespace {

// A value fading toward `target` by `step` (not past it).
float Approach(float value, float target, float step)
{
    if (value < target) {
        const float v = step + value;
        return v < target ? v : target;
    }
    if (value > target) {
        const float v = value - step;
        return v > target ? v : target;
    }
    return value;
}

// A material's texture size (64 x 64 without one).
void SpriteSize(const I3D_material *m, int &w, int &h)
{
    // (Fix: the original reads an element's material unchecked; a corrupted file leaves it null.)
    const ITexture *t = m ? m->texture : nullptr;
    w = t ? t->width : 0x40;
    h = t ? t->height : 0x40;
}

// A screen-space sprite of the lens (four 0x20-byte pre-transformed vertices: position, rhw,
// diffuse, specular, uv) at `p` (world space): false (nothing drawn) when it is behind the camera.
bool DrawSprite(S_vector p, float w, float h, float alpha, const S_vector4 &color, I3D_material *m)
{
    const S_matrix &vp = *g_viewProjection;
    const float pw = ((p.z * vp.m[2][3] + p.y * vp.m[1][3]) + p.x * vp.m[0][3]) + vp.m[3][3];
    if (pw < 0.0f || pw == 0.0f)
        return false;
    const float inv = 1.0f / pw;
    p *= vp;
    const float sx = (static_cast<float>(g_viewport.Width) * 0.5f) * (p.x * inv + 1.0f) +
                     static_cast<float>(static_cast<int32_t>(g_viewport.X));
    const float sy = (static_cast<float>(g_viewport.Height) * 0.5f) * (1.0f - p.y * inv) +
                     static_cast<float>(static_cast<int32_t>(g_viewport.Y));
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float left = sx - hw, top = sy - hh, right = sx + hw, bottom = sy + hh;
    if (alpha < 0.0f)
        alpha = 0.0f;
    else if (alpha > 1.0f)
        alpha = 1.0f;
    const uint32_t a = static_cast<uint32_t>(Fistp(alpha * 255.0f));
    const uint32_t r = static_cast<uint32_t>(Fistp(color.x * 255.0f));
    const uint32_t g = static_cast<uint32_t>(Fistp(color.y * 255.0f));
    const uint32_t b = static_cast<uint32_t>(Fistp(color.z * 255.0f));
    const uint32_t argb = (((a << 8 | r) << 8) | g) << 8 | b;
    struct S_vertex {
        float x, y, z, rhw;
        uint32_t diffuse, specular;
        float u, v;
    };
    const S_vertex v[4] = {
        {left, top, 0.0f, 1.0f, argb, 0, 0.0f, 0.0f},
        {right, top, 0.0f, 1.0f, argb, 0, 1.0f, 0.0f},
        {left, bottom, 0.0f, 1.0f, argb, 0, 0.0f, 1.0f},
        {right, bottom, 0.0f, 1.0f, argb, 0, 1.0f, 1.0f},
    };
    gfx::Texture *texture = nullptr;
    if (m && m->texture) {
        if (m->matFlags & MATF_ANIMATED)
            m->UpdateTexture();
        texture = m->texture->deviceTexture;
    }
    g_pGfx->SetTexture(0, texture);
    SetVertexFormat(6);
    g_pGfx->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(S_vertex));
    return true;
}

}  // namespace

// 0x10007dd0
void I3D_lens::Init()
{
    I3D_visual::Init();
    // (The colors' w stay as they were: the original writes an unset local.)
    transmit.x = transmit.y = transmit.z = 1.0f;
    color.x = color.y = color.z = 1.0f;
    lensFlags = 0;
    testInterval = 150;
    testTime = 0;
    updateTime = 0;
    brightness = 0.0f;
    target = 1.0f;
    sizeScale = 1.0f;
    intensity = 1.0f;
    fadeIn = 15.0f;
    fadeOut = 8.0f;
    range = 300.0f;
    nearRange = 0.0f;
    farStart = 0.0f;
    nearScale = 1.0f;
}

// 0x10007ea0
void I3D_lens::Uninit()
{
    for (S_lensElement &e : elements) {
        if (e.material)
            e.material->Release();
    }
    elements.Clear();
    I3D_frame::Uninit();
}

// 0x10007ef0
I3D_RESULT I3D_lens::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    auto *srcVisual = static_cast<I3D_visual *>(source);
    if (srcVisual->visualFlags & VISF_WORLD_SPACE)
        visualFlags |= VISF_WORLD_SPACE;
    if (srcVisual->GetVisualType() != FourCC("LENS"))
        return I3D_OK;
    auto *src = static_cast<I3D_lens *>(source);
    testInterval = src->testInterval;
    nearScale = src->nearScale;
    sizeScale = src->sizeScale;
    fadeIn = src->fadeIn;
    fadeOut = src->fadeOut;
    range = src->range;
    nearRange = src->nearRange;
    farStart = src->farStart;
    // (Fix: the original drops the old elements without releasing their materials.)
    for (S_lensElement &e : elements) {
        if (e.material)
            e.material->Release();
    }
    elements.Free();
    elements.ResizeDiscard(src->elements.Size());
    for (int i = elements.Size() - 1; i >= 0; --i)
        elements[i] = src->elements[i];
    visualFlags = src->visualFlags & 0x2ff52;
    localBound = src->localBound;
    if (!(src->visualFlags & VISF_WORLD_SPACE)) {
        visualFlags &= ~VISF_WBOUND_VALID;
    } else {
        src->GetWorldBound();
        worldBound = src->worldBound;
    }
    for (S_lensElement &e : elements) {
        if (e.material)
            ++e.material->refCount;
    }
    return I3D_OK;
}

// 0x10009610
void I3D_lens::Load()
{
    I3D_frame::Load();
    int count = 0;
    dtaRead(g_loadFile, &count, 1);
    if (!count)
        return;
    // (Fix: an element without a material ID takes the previous one's, which the original leaves
    // unset for the first and shares without a reference.)
    I3D_material *material = nullptr;
    while (count--) {
        S_lensElement e;
        dtaRead(g_loadFile, &e, 8);
        int id = 0;
        dtaRead(g_loadFile, &id, 2);
        if (id > g_loadNumMaterials) {
            g_loadResult = I3DERR_FILECORRUPTED;
            material = nullptr;
        } else if (id > 0) {
            material = g_loadMaterials[id - 1];
        }
        if (material)
            ++material->refCount;
        e.material = material;
        elements.PushBack(e);
    }
    // A small box; its center stays where it was. (The original also writes unset w values.)
    localBound.bbox.min = S_vector(-0.05f, -0.05f, -0.05f);
    localBound.bbox.max = S_vector(0.05f, 0.05f, 0.05f);
    localBound.radius = 0.01f;
}

// 0x100080d0
void I3D_lens::DrawFlare(bool seen)
{
    const S_vector pos = GetMatrix()(3);
    I3D_camera *cam = g_renderCamera;
    const S_vector camPos = cam->GetMatrix()(3);
    const int n = elements.Size();
    if (!n)
        return;
    // The visibility ray: to a glow, or far along a flare's direction.
    S_vector ray;
    float dist = 0.0f;
    if (n == 1) {
        ray = S_vector(pos.x - camPos.x, pos.y - camPos.y, pos.z - camPos.z);
        dist = MagnitudeOfSquare(SquareXZY(ray));
        if (dist > range) {
            brightness = 0.0f;
            return;
        }
    } else {
        ray = pos;
        NormalizeWithSquare(ray, SquareXZY(pos));
        const float reach = cam->farRange + 100.0f;
        ray = S_vector(ray.x * reach, ray.y * reach, ray.z * reach);
    }

    const uint32_t now = timeGetTime();
    if (!seen) {
        target = 0.0f;
    } else if (now - testTime >= testInterval) {
        testTime = now + static_cast<uint32_t>(std::rand() % 100);
        target = 1.0f;
        transmit.x = transmit.y = transmit.z = 1.0f;
        I3D_scene *scene = g_renderScene;
        auto blocked = [this](I3D_frame *hit) {
            return hit && IsTinyBits((transmit.z + transmit.y) + transmit.x);
        };
        if (blocked(scene->TestRay(camPos, ray, 0x21030000, nullptr, nullptr, nullptr, &transmit, TransmitFilter,
                                   parent, 0))) {
            target = 0.0f;
        } else if (scene->nearSector->firstChild) {
            // Through the near sector, drawn with its own field of view: the ray's sideways part
            // scaled by the ratio of the two.
            S_vector z = cam->GetMatrix()(2);
            NormalizeWithSquare(z, SquareXYZ(z));
            const float dot = (ray.x * z.x + z.z * ray.z) + z.y * ray.y;
            const S_vector along(z.x * dot, z.y * dot, z.z * dot);
            const float s = g_lensFlareScale;
            const S_vector nearRay((ray.x - along.x) * s + along.x, (ray.y - along.y) * s + along.y,
                                   (ray.z - along.z) * s + along.z);
            if (blocked(scene->TestRay(camPos, nearRay, 0x51030000, nullptr, nullptr, nullptr, &transmit,
                                       TransmitFilter, scene->primarySector, 0)))
                target = 0.0f;
        }
    }

    // What got through, at most white.
    float top = transmit.y < transmit.x ? transmit.x : transmit.y;
    if (top < transmit.z)
        top = transmit.z;
    if (top > 1.0) {
        const float inv = 1.0f / top;
        transmit.x = inv * transmit.x;
        transmit.y = inv * transmit.y;
        transmit.z = inv * transmit.z;
    }

    if (now != updateTime) {
        const uint32_t dt = now - updateTime;
        const float step = (static_cast<float>(dt) * (IsTinyBits(target) ? fadeOut : fadeIn)) * 0.001f;
        updateTime = now;
        brightness = Approach(brightness, target, step);
        color.x = Approach(color.x, transmit.x, step);
        color.y = Approach(color.y, transmit.y, step);
        color.z = Approach(color.z, transmit.z, step);
    }
    if (IsTinyBits(brightness))
        return;

    if (n == 1) {
        // A glow at the bound's center: larger close up, shrinking toward its range.
        GetWorldBound();
        const S_vector c = worldBound.center;
        I3D_material *m = elements[0].material;
        int tw, th;
        SpriteSize(m, tw, th);
        float f;
        if (dist < nearRange) {
            f = (1.0f - dist / nearRange) * nearScale;
            if (1.0f > f)
                f = 1.0f;
        } else if (dist > farStart) {
            f = 1.0f - (dist - farStart) / (range - farStart);
        } else {
            f = 1.0f;
        }
        const float s = sizeScale * brightness;
        const float w = (static_cast<float>(tw) * f) * s;
        const float h = (static_cast<float>(th) * f) * s;
        if (!DrawSprite(c, w, h, intensity * brightness, color, m))
            brightness = 0.0f;
        return;
    }

    // A flare: the elements along the line from the light's direction through the screen's
    // center, two units in front of the camera; the main one grows toward the view's center.
    S_vector axis = pos;
    NormalizeWithSquare(axis, SquareXZY(pos));
    S_vector z = cam->GetMatrix()(2);
    NormalizeWithSquare(z, SquareXYZ(z));
    const float angle = static_cast<float>(AcosD((z.x * axis.x + z.z * axis.z) + z.y * axis.y) /
                                           static_cast<double>(cam->fov * 0.5f));
    float center = (1.0f - angle) * 0.8f + 0.2f;
    if (center < 0.0f)
        center = 0.0f;
    const S_vector base((z.x + z.x) + camPos.x, (z.y + z.y) + camPos.y, (z.z + z.z) + camPos.z);
    for (int i = n - 1; i >= 0; --i) {
        const S_lensElement &e = elements[i];
        const S_vector p(axis.x * e.position + base.x, axis.y * e.position + base.y, axis.z * e.position + base.z);
        int tw, th;
        SpriteSize(e.material, tw, th);
        const float s = (e.size * sizeScale) * brightness;
        float w = static_cast<float>(tw) * s;
        float h = static_cast<float>(th) * s;
        if (i == 0) {
            const float grow = center + 1.0f;
            w = w * grow;
            h = h * grow;
        }
        DrawSprite(p, w, h, ((i == 0 ? 1.0f : center) * intensity) * brightness, color, e.material);
    }
}
