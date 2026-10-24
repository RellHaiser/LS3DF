// Device state helpers of the visuals' draw code (0x100637a0 - 0x100647f0).
#include "i3d/render_helpers.h"

#include <cmath>
#include <cstring>

#include "core/fpu.h"
#include "i3d/driver.h"
#include "i3d/i3d_links.h"
#include "i3d/light.h"
#include "i3d/mirror.h"
#include "i3d/sector.h"
#include "i3d/visual.h"
#include "igraph/igraph_state.h"

namespace ls3d::i3d {

using ig::g_pDevice;

I3D_mirror *g_renderMirror;

namespace {

// The device lights enabled last (0x100d1364, count 0x100d13d0) and D3DRS_AMBIENT's cache
// (0x100a72d8; 0x00FF00FF: not set yet).
uint16_t s_enabledLights[8];
int s_numEnabledLights;
uint32_t s_ambient = 0x00ff00ff;
// The sector SelectLights reads (0x100d13d4).
I3D_sector *s_lightSector;
// SetMaterial's scaled copy (0x100d1378; the ambient, specular and emissive alphas stay 0).
D3DMATERIAL8 s_scaledMaterial;

// A 0 - 1 clamp as the x87 code does it: a NaN passes.
float Clamp01(float x)
{
    if (x < 0.0f)
        return 0.0f;
    if (x > 1.0f)
        return 1.0f;
    return x;
}

}  // namespace

// 0x100637a0
void SetZBias(int bias)
{
    if (ig::g_rs.zBias != bias) {
        ig::g_rs.zBias = bias;
        g_pDevice->SetRenderState(D3DRS_ZBIAS, bias);
    }
}

// 0x1001b6b0
void SetVertexFormat(int format, bool force)
{
    if (ig::g_rs.vertexShader == format && !force)
        return;
    ig::g_rs.vertexShader = format;
    switch (format) {
    case 0:
    case 8:
        g_pDevice->SetVertexShader(ig::g_vsNormalTex1);
        break;
    case 1:
        g_pDevice->SetVertexShader(ig::g_vsNormalTex2);
        break;
    case 2:
        g_pDevice->SetVertexShader(ig::g_vsColorTex1);
        break;
    case 3:
        g_pDevice->SetVertexShader(ig::g_vsColorTex2);
        break;
    case 4:
        g_pDevice->SetVertexShader(0x40);
        break;
    case 5:
        g_pDevice->SetVertexShader(0x140);
        break;
    case 6:
        g_pDevice->SetVertexShader(0x1c4);
        break;
    case 7:
        g_pDevice->SetVertexShader(0x2c4);
        break;
    case 9:
        g_pDevice->SetVertexShader(ig::g_vsFarPlane);
        break;
    default:
        break;
    }
}

void SetColorOp(int stage, int op)
{
    if (ig::g_rs.colorOp[stage] != op) {
        ig::g_rs.colorOp[stage] = op;
        g_pDevice->SetTextureStageState(stage, D3DTSS_COLOROP, op);
    }
}

void SetAlphaOp(int stage, int op)
{
    if (ig::g_rs.alphaOp[stage] != op) {
        ig::g_rs.alphaOp[stage] = op;
        g_pDevice->SetTextureStageState(stage, D3DTSS_ALPHAOP, op);
    }
}

void DisableFog()
{
    if (ig::g_rs.fog != 0) {
        ig::g_rs.fog = 0;
        g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    }
}

// 0x1001b400
void SetCullState(int state)
{
    if (ig::g_rs.cull == state)
        return;
    ig::g_rs.cull = state;
    g_pDevice->SetRenderState(D3DRS_CULLMODE, state == 3 ? ig::g_rs.cullDefault : state);
}

// 0x1001b4b0
void SetAlphaState(int state)
{
    if (ig::g_rs.alpha == state)
        return;
    ig::g_rs.alpha = state;
    switch (state) {
    case 0:
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        break;
    case 1:
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHAREF, 1);
        break;
    case 2:
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHAREF, 0);
        break;
    case 3:
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHAREF, ig::g_rs.alphaRef);
        break;
    default:
        break;
    }
}

// 0x1001b5c0
void SetBlendState(int state)
{
    if (ig::g_rs.blend == state)
        return;
    ig::g_rs.blend = state;
    static const DWORD kBlend[5][2] = {
        { D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA }, { D3DBLEND_SRCALPHA, D3DBLEND_ONE },
        { D3DBLEND_ONE, D3DBLEND_ONE }, { D3DBLEND_DESTCOLOR, D3DBLEND_ZERO },
        { D3DBLEND_DESTCOLOR, D3DBLEND_SRCCOLOR },
    };
    if (state < 0 || state > 4)
        return;
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, kBlend[state][0]);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, kBlend[state][1]);
}

// 0x10063810
void SetMaterial(D3DMATERIAL8 &material, float alpha, float level)
{
    alpha = Clamp01(alpha);
    if (level < 1.0f) {
        s_scaledMaterial.Ambient.r = level * material.Ambient.r;
        s_scaledMaterial.Ambient.g = level * material.Ambient.g;
        s_scaledMaterial.Ambient.b = level * material.Ambient.b;
        s_scaledMaterial.Diffuse.a = alpha;
        s_scaledMaterial.Diffuse.r = level * material.Diffuse.r;
        s_scaledMaterial.Diffuse.g = level * material.Diffuse.g;
        s_scaledMaterial.Diffuse.b = level * material.Diffuse.b;
        s_scaledMaterial.Specular.r = level * material.Specular.r;
        s_scaledMaterial.Specular.g = level * material.Specular.g;
        s_scaledMaterial.Specular.b = level * material.Specular.b;
        s_scaledMaterial.Emissive.r = level * material.Emissive.r;
        s_scaledMaterial.Emissive.g = level * material.Emissive.g;
        s_scaledMaterial.Emissive.b = level * material.Emissive.b;
        s_scaledMaterial.Power = material.Power;
        g_pDevice->SetMaterial(&s_scaledMaterial);
        return;
    }
    material.Diffuse.a = alpha;
    g_pDevice->SetMaterial(&material);
}

// 0x10063930
void UpdateSectorAmbient(I3D_sector *sector)
{
    if (!(sector->sectorFlags & SCTF_AMBIENT_CHANGED))
        return;
    float r = 0.0f, g = 0.0f, b = 0.0f;         // the lights in SetMode bit 1
    float r2 = 0.0f, g2 = 0.0f, b2 = 0.0f;      // ... bit 0x10
    for (int i = sector->lights.Size(); i != 0; --i) {
        const I3D_light *light = sector->lights[i - 1];
        if (light->lightType != I3DLIGHT_AMBIENT || !(light->frameFlags & FRMFLAGS_ON))
            continue;
        if (light->mode & 1) {
            r = r + light->effectiveColor.x;
            g = g + light->effectiveColor.y;
            b = b + light->effectiveColor.z;
        }
        if (light->mode & 0x10) {
            r2 = r2 + light->effectiveColor.x;
            g2 = g2 + light->effectiveColor.y;
            b2 = b2 + light->effectiveColor.z;
        }
    }
    // (Without lights the sums are used as they are: 0.)
    if (sector->lights.Size() != 0) {
        r = Clamp01(r);
        g = Clamp01(g);
        b = Clamp01(b);
    }
    sector->ambientColor = (static_cast<uint32_t>(Fistp(r * 255.0f)) << 16) |
        (static_cast<uint32_t>(Fistp(g * 255.0f)) << 8) | static_cast<uint32_t>(Fistp(b * 255.0f));
    r2 = Clamp01(r2);
    g2 = Clamp01(g2);
    b2 = Clamp01(b2);
    sector->ambientColor2 = (static_cast<uint32_t>(Fistp(r2 * 255.0f)) << 16) |
        (static_cast<uint32_t>(Fistp(g2 * 255.0f)) << 8) | static_cast<uint32_t>(Fistp(b2 * 255.0f));
    sector->sectorFlags &= ~SCTF_AMBIENT_CHANGED;
}

// 0x10063dc0
void EnableLights(uint32_t ambient, int count, const uint16_t *indices)
{
    if (ambient != s_ambient) {
        s_ambient = ambient;
        g_pDevice->SetRenderState(D3DRS_AMBIENT, ambient);
    }
    for (int i = 0; i < s_numEnabledLights; ++i)
        g_pDevice->LightEnable(s_enabledLights[i], FALSE);
    for (int i = 0; i < count; ++i) {
        const uint16_t index = indices[i];
        I3D_light *light = g_lights[index];
        if (light->lightFlags & LIGHTF_DIRTY) {
            g_pDevice->SetLight(index, &light->d3dLight);
            light->lightFlags &= ~LIGHTF_DIRTY;
        }
        g_pDevice->LightEnable(index, TRUE);
    }
    std::memcpy(s_enabledLights, indices, sizeof(uint16_t) * static_cast<size_t>(count));
    s_numEnabledLights = count;
}

// 0x10063ea0
void SetLightSector(I3D_sector *sector) { s_lightSector = sector; }
I3D_sector *GetLightSector() { return s_lightSector; }

// 0x10063eb0
void SelectLights(I3D_visual *visual, S_visualLights &lights, uint32_t mask)
{
    const int stamp = s_lightSector->lightChanges + visual->changeCount;
    if (stamp == lights.stamp)
        return;
    lights.stamp = stamp;
    if (!(visual->visualFlags & VISF_WBOUND_VALID))
        visual->UpdateWBoundProc();
    const S_vector center = visual->worldBound.center;
    const float radius = visual->worldBound.radius;

    // The best ones so far (0x100d12d0, 0x10 bytes each: intensity, light at +0xC).
    struct Choice {
        float intensity;
        I3D_light *light;
    } best[8];
    int count = 0;
    for (int n = s_lightSector->lights.Size(), i = 0; n != 0; --n, ++i) {
        I3D_light *light = s_lightSector->lights[i];
        if (!(light->mode & mask) || !(light->frameFlags & FRMFLAGS_ON))
            continue;
        if (IsTinyBits(std::fabs(light->power)) && IsTinyBits(std::fabs(light->ambientPower)))
            continue;
        float intensity;
        switch (light->lightType) {
        case I3DLIGHT_POINT: {
            const S_vector &pos = light->GetMatrix()(3);
            const float dx = center.x - pos.x, dy = center.y - pos.y, dz = center.z - pos.z;
            const float d2 = (dz * dz + dy * dy) + dx * dx;
            const float reach = radius + light->d3dLight.Range;
            if (reach * reach <= d2)
                continue;
            intensity = ((light->effectiveColor.z + light->effectiveColor.y) + light->effectiveColor.x) /
                std::sqrt(d2);
            break;
        }
        case I3DLIGHT_SPOT: {
            const S_vector &pos = light->coneCenter;
            const float dx = center.x - pos.x, dy = center.y - pos.y, dz = center.z - pos.z;
            const float d2 = (dz * dz + dy * dy) + dx * dx;
            const float reach = radius + light->coneRangeWorld;
            if (reach * reach <= d2)
                continue;
            intensity = ((light->effectiveColor.z + light->effectiveColor.y) + light->effectiveColor.x) /
                std::sqrt(d2);
            break;
        }
        case I3DLIGHT_DIRECTIONAL:
            intensity = (light->effectiveColor.z + light->effectiveColor.y) + light->effectiveColor.x;
            break;
        default:
            continue;
        }
        // Below 8 lights the new one is added; then it replaces the first weaker one (not the
        // weakest: the original's choice).
        int slot;
        if (count < 8) {
            slot = count++;
        } else {
            slot = -1;
            for (int j = 0; j < count; ++j) {
                if (best[j].intensity < intensity) {
                    slot = j;
                    break;
                }
            }
        }
        if (slot >= 0) {
            best[slot].light = light;
            best[slot].intensity = intensity;
        }
    }
    for (int i = 0; i < count; ++i)
        lights.indices[i] = static_cast<uint16_t>(best[i].light->hardwareIndex);
    lights.count = count;
}

// 0x10064260
void MirrorLights(const S_visualLights &src, S_visualLights &dst)
{
    const uint16_t stamp = g_renderMirror->stamp;
    const S_matrix &m = g_renderMirror->reflection;
    for (int i = 0; i < src.count; ++i) {
        const uint16_t index = src.indices[i];
        I3D_light *light = g_lights[index];
        I3D_light *copy;
        if (static_cast<uint16_t>(light->linkedLight) == 0xffff) {
            copy = static_cast<I3D_light *>(g_driver->CreateFrame(FRAME_LIGHT));
            light->linkedLight = copy->hardwareIndex;
            light->unknown25A = static_cast<int16_t>(stamp);
            copy->lightFlags |= LIGHTF_MIRRORED;
            copy->linkedLight = static_cast<int16_t>(index);
        } else {
            copy = g_lights[static_cast<uint16_t>(light->linkedLight)];
            if (static_cast<uint16_t>(light->unknown25A) != stamp) {
                light->unknown25A = static_cast<int16_t>(stamp);
            } else if (!(light->lightFlags & LIGHTF_MOVED)) {
                dst.indices[i] = static_cast<uint16_t>(light->linkedLight);
                continue;
            }
        }
        copy->d3dLight = light->d3dLight;
        switch (light->lightType) {
        case I3DLIGHT_POINT: {
            const S_vector pos = S_vector(light->d3dLight.Position.x, light->d3dLight.Position.y,
                                          light->d3dLight.Position.z) * m;
            copy->d3dLight.Position.x = pos.x;
            copy->d3dLight.Position.y = pos.y;
            copy->d3dLight.Position.z = pos.z;
            break;
        }
        case I3DLIGHT_SPOT: {
            const S_vector pos = S_vector(light->d3dLight.Position.x, light->d3dLight.Position.y,
                                          light->d3dLight.Position.z) * m;
            copy->d3dLight.Position.x = pos.x;
            copy->d3dLight.Position.y = pos.y;
            copy->d3dLight.Position.z = pos.z;
            S_vector dir = S_vector(light->d3dLight.Direction.x, light->d3dLight.Direction.y,
                                    light->d3dLight.Direction.z).RotateByMatrix(m);
            NormalizeWithSquare(dir, SquareZYX(dir));
            copy->d3dLight.Direction.x = dir.x;
            copy->d3dLight.Direction.y = dir.y;
            copy->d3dLight.Direction.z = dir.z;
            break;
        }
        case I3DLIGHT_DIRECTIONAL: {
            S_vector dir = S_vector(light->d3dLight.Direction.x, light->d3dLight.Direction.y,
                                    light->d3dLight.Direction.z).RotateByMatrix(m);
            NormalizeWithSquare(dir, SquareZYX(dir));
            copy->d3dLight.Direction.x = dir.x;
            copy->d3dLight.Direction.y = dir.y;
            copy->d3dLight.Direction.z = dir.z;
            break;
        }
        default:
            break;
        }
        copy->lightFlags |= LIGHTF_DIRTY;
        light->lightFlags &= ~LIGHTF_MOVED;
        dst.indices[i] = static_cast<uint16_t>(light->linkedLight);
    }
    dst.count = src.count;
}

// 0x100647f0
void InvalidateLights()
{
    s_ambient = 0x00ff00ff;
    s_numEnabledLights = 0;
    for (int i = g_lights.Size(); i != 0; --i) {
        I3D_light *light = g_lights[i - 1];
        if (light->lightType == I3DLIGHT_POINT || light->lightType == I3DLIGHT_SPOT ||
            light->lightType == I3DLIGHT_DIRECTIONAL) {
            light->lightFlags |= LIGHTF_DIRTY;
            g_pDevice->LightEnable(static_cast<uint16_t>(light->hardwareIndex), FALSE);
        }
    }
}

// 0x100695a0
const S_vector4 *BoxCorners(const I3D_bbox &box)
{
    static S_vector4 s_corners[8];
    S_vector4 *c = s_corners;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k, ++c) {
                c->x = i ? box.min.x : box.max.x;
                c->y = j ? box.min.y : box.max.y;
                c->z = k ? box.max.z : box.min.z;
            }
        }
    }
    return s_corners;
}

}  // namespace ls3d::i3d
