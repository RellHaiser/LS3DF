// The scene's weather (rain / snow particles around the camera): moved and written into the
// shared weather vertex buffer each frame (0x1004a290), drawn with the main pass (0x1004b720).
#include <cmath>
#include <cstring>

#include "core/fpu.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/light.h"
#include "i3d/render_helpers.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/vb_manager.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;
using ig::g_rs;

namespace {

// The weather's clock (0x100bf240; 0x100bf244 bit 1: set) and the camera position of the last
// frame (0x100bf210; bit 2).
int s_lastTime;
S_vector4 s_lastCameraPos;
uint32_t s_initialized;
// The sectors near the camera the particles may fall in (WEATHER_PARAM1; 0x100bf248, grow
// 0x100bf254).
C_vector<I3D_sector *> s_weatherSectors(0x20);

const S_matrix kIdentity = { { { 1.0f, 0.0f, 0.0f, 0.0f },
                               { 0.0f, 1.0f, 0.0f, 0.0f },
                               { 0.0f, 0.0f, 1.0f, 0.0f },
                               { 0.0f, 0.0f, 0.0f, 1.0f } } };

uint32_t FogColor(const I3D_light *fog)
{
    const uint32_t r = static_cast<uint32_t>(Fistp(fog->color.x * 255.0f));
    const uint32_t g = static_cast<uint32_t>(Fistp(fog->color.y * 255.0f));
    const uint32_t b = static_cast<uint32_t>(Fistp(fog->color.z * 255.0f));
    return (((r | 0xffffff00u) << 8 | g) << 8) | b;
}

// A point inside a drawn dummy's box (within each face plane's extent).
bool InsideBox(const S_dummyBox &box, const S_vector &p)
{
    for (int i = 0; i < 3; ++i) {
        const S_plane &pl = box.planes[i];
        const float d = ((p.z * pl.normal.z + p.y * pl.normal.y) + p.x * pl.normal.x) + pl.d;
        if (!(d >= 0.0f) || !(d <= box.extents[i]))
            return false;
    }
    return true;
}

float Dist2XZY(const S_vector &a, const S_vector &b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return (dx * dx + dz * dz) + dy * dy;
}

}  // namespace

// 0x1004a290
void I3D_scene::UpdateWeather()
{
    if (!(weatherFlags & WEATHER_ON) || !(primarySector->sectorFlags & SCTF_VISIBLE) || weatherCount == 0) {
        g_weatherDrawn = 0;
        return;
    }
    if (!(s_initialized & 1)) {
        s_initialized |= 1;
        s_lastTime = g_driverTime;
    }
    const uint32_t dt = static_cast<uint32_t>(g_driverTime - s_lastTime);
    s_lastTime = g_driverTime;
    g_weatherDrawn = 0;
    I3D_camera *cam = g_renderCamera;
    // The cone around the view direction the particles are drawn in.
    const float coneCos = static_cast<float>(
        std::cos(static_cast<double>((std::sqrt(cam->aspect * cam->aspect + 1.0f) * cam->fov) * 0.5f)));
    const S_vector4 camPos = *reinterpret_cast<const S_vector4 *>(&cam->GetMatrix()(3));
    S_vector fwd = cam->GetMatrix()(2);
    NormalizeWithSquare(fwd, SquareYZX(fwd));
    if (!(s_initialized & 2)) {
        s_initialized |= 2;
        s_lastCameraPos = camPos;
    }
    // The camera's motion since the last frame (backwards): the particles stream past it.
    const float backX = s_lastCameraPos.x - camPos.x;
    const float backZ = s_lastCameraPos.z - camPos.z;
    s_lastCameraPos = camPos;
    if (weatherFlags & WEATHER_SNOW) {
        const S_matrix windTurn = RotateYMatrix(static_cast<float>(g_driverTime) * 0.0007f);
        const S_vector d = *reinterpret_cast<const S_vector *>(&weatherWind) * windTurn;
        weatherWindStart.x = d.x;
        weatherWindStart.y = d.y;
        weatherWindStart.z = d.z;
    } else {
        S_vector4 dir;
        if (IsTinyBits(backX * backX + backZ * backZ)) {
            dir = weatherWind;
        } else {
            const float t = static_cast<float>(dt);
            S_vector v(((t * weatherWind.x) * weatherFallSpeed) + backX, (t * weatherWind.y) * weatherFallSpeed,
                       ((t * weatherWind.z) * weatherFallSpeed) + backZ);
            NormalizeWithSquare(v, SquareXZY(v));
            dir = S_vector4{ v.x, v.y, v.z, 0.0f };
        }
        const float ddx = weatherWindStart.x - dir.x, ddy = weatherWindStart.y - dir.y, ddz = weatherWindStart.z - dir.z;
        if (!IsTinyBits((ddx * ddx + ddz * ddz) + ddy * ddy)) {
            // The streaks turn towards the new direction over 100 ms.
            float f = static_cast<float>(dt) * 0.01f;
            if (f > 1.0f)
                f = 1.0f;
            const float g = 1.0f - f;
            weatherWindStart.x = dir.x * f + g * weatherWindStart.x;
            weatherWindStart.y = dir.y * f + g * weatherWindStart.y;
            weatherWindStart.z = dir.z * f + g * weatherWindStart.z;
            weatherWindStart.w = 0.0f;          // (the original stores an unset local)
        }
    }
    const S_vector streak(-weatherWindStart.x * weatherParam6, -weatherWindStart.y * weatherParam6,
                          -weatherWindStart.z * weatherParam6);
    // The particles' frame (their shapes' axes) along the current direction, every frame.
    weatherDirection.SetDir(*reinterpret_cast<const S_vector *>(&weatherWindStart));
    if (!g_weatherVB.vb || g_weatherCapacity < weatherCount)
        SetWeather(10, static_cast<uint32_t>(weatherCount));
    C_VertexBuffer *vb = g_weatherVB.buffer;
    float *dst = reinterpret_cast<float *>(vb->Lock(0) + vb->stride * g_weatherVB.offset);
    if ((weatherFlags & WEATHER_RECOLOR) && (weatherFlags &= ~WEATHER_RECOLOR, g_weatherCapacity != 0)) {
        // (The colors as dwords: they are NaNs as floats.)
        uint32_t *c = reinterpret_cast<uint32_t *>(dst) + 3;
        for (int n = g_weatherCapacity; n != 0; --n, c += 30) {
            c[0] = weatherColor2;
            c[6] = weatherColor2;
            c[12] = weatherColor2;
            c[18] = weatherColor2;
            c[24] = weatherColor1;
        }
    }
    if (weatherFlags & WEATHER_RESHAPE) {
        // Four particle shapes, 45 degrees apart: two side points, the head and the tail.
        weatherFlags &= ~WEATHER_RESHAPE;
        float angle = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const S_matrix spin = RotateZMatrix(angle);
            S_matrix &shape = weatherMatrices[k];
            shape(1) = S_vector(-weatherParam7, 0.0f, 0.0f) * spin;
            shape(2) = S_vector(weatherParam7, 0.0f, 0.0f) * spin;
            if (!(weatherFlags & WEATHER_SNOW)) {
                shape(0) = S_vector(0.0f, 0.0f, -weatherParam6) * spin;
                shape(3) = S_vector(0.0f, 0.0f, weatherParam6 * 0.166666672f) * spin;
            } else {
                shape(0) = S_vector(0.0f, 0.0f, weatherParam6 * -0.5f) * spin;
                shape(3) = S_vector(0.0f, 0.0f, weatherParam6 * 0.5f) * spin;
            }
            angle = angle + 0.785398185f;
        }
    }
    s_weatherSectors.Clear();
    if (weatherFlags & WEATHER_PARAM1) {
        s_weatherSectors.Reserve(sectorCount < 0 ? 0 : sectorCount);
        I3D_sector *s = firstSector;
        for (int n = sectorCount; n != 0; --n, s = s->nextInScene) {
            if (!(s->frameFlags & FRMFLAGS_BOUND_VALID))
                s->UpdateBound();
            const float reach = s->bound.radius + weatherRadius;
            if (Dist2XZY(*reinterpret_cast<const S_vector *>(&camPos), s->bound.center) < reach * reach)
                s_weatherSectors.PushBack(s);
        }
    }
    const float t = static_cast<float>(dt);
    const float speed = weatherFallSpeed;
    S_vector4 *part = weatherParticles.Data();
    for (int k = weatherCount; k != 0; ++part) {
        --k;
        const int seed = k * 0x4d2;
        S_vector p(((t * weatherWind.x) * speed) + part->x, ((t * weatherWind.y) * speed) + part->y,
                   ((t * weatherWind.z) * speed) + part->z);
        S_vector rel(p.x - camPos.x, p.y - camPos.y, p.z - camPos.z);
        const float r2 = weatherRadius * weatherRadius;
        const float d2 = ((p.x - camPos.x) * (p.x - camPos.x) + rel.z * rel.z) + rel.y * rel.y;
        if (p.y < part->w || p.y == part->w || r2 < d2 || r2 == d2) {
            // A new particle in the box around the camera.
            const S_vector base(camPos.x - weatherRangeXZ, camPos.y - weatherRangeY, camPos.z - weatherRangeXZ);
            float r = RandFloat();
            p.x = (r * weatherRangeXZ + r * weatherRangeXZ) + base.x;
            r = RandFloat();
            p.y = (r * weatherRangeY + r * weatherRangeY) + base.y;
            r = RandFloat();
            p.z = (r * weatherRangeXZ + r * weatherRangeXZ) + base.z;
            part->w = camPos.y - weatherRangeY;
            rel = S_vector(p.x - camPos.x, p.y - camPos.y, p.z - camPos.z);
        }
        if ((weatherFlags & WEATHER_SNOW) && dt != 0) {
            // (x87: the whole int times 0.002f rounded once, FSIN, times 0.004f; both constants
            // floats.)
            const int phase = seed + g_driverTime;
            const float a = static_cast<float>(static_cast<double>(phase) * 0.002f);
            const float b = static_cast<float>(static_cast<double>(phase + 500) * 0.002f);
            p.x = p.x - static_cast<float>(std::sin(static_cast<double>(a)) * 0.004f);
            p.z = static_cast<float>(std::sin(static_cast<double>(b)) * 0.004f) + p.z;
        }
        part->x = p.x;
        part->y = p.y;
        part->z = p.z;
        const float dot = ((fwd.x * rel.x) + (fwd.z * rel.z)) + fwd.y * rel.y;
        if (dot < 0.0f || dot == 0.0f)
            continue;
        S_vector n = rel;
        NormalizeWithSquare(n, SquareXZY(n));
        const float dot2 = ((fwd.x * n.x) + (fwd.z * n.z)) + fwd.y * n.y;
        if (dot2 < coneCos || dot2 == coneCos)
            continue;
        const S_vector tail(streak.x + p.x, streak.y + p.y, streak.z + p.z);
        if (s_weatherSectors.Size() > 0) {
            if (LocateSectorIn(p, s_weatherSectors.Data(), s_weatherSectors.Size()) != primarySector ||
                LocateSectorIn(tail, s_weatherSectors.Data(), s_weatherSectors.Size()) != primarySector) {
                part->w = 1e6f;
                continue;
            }
        }
        if (weatherFlags & WEATHER_PARAM2) {
            // Drawn dummies' boxes block the particles.
            bool blocked = false;
            for (int i = drawnDummies.Size(); i != 0 && !blocked;) {
                --i;
                S_dummyBox &box = drawnDummies[i];
                if (box.changeCount != box.frame->changeCount)
                    MakeDummyBox(box, box.frame);
                if ((Dist2XZY(box.center, p) <= box.radiusSquare || Dist2XZY(box.center, tail) <= box.radiusSquare) &&
                    (InsideBox(box, p) || InsideBox(box, tail)))
                    blocked = true;
            }
            if (blocked) {
                part->w = 1e6f;
                continue;
            }
        }
        // The five vertices (position, color, uv: 6 floats): head, two sides, tail, center.
        const S_matrix &shape = weatherMatrices[k & 3];
        const S_matrix &d = weatherDirection;
        dst[0] = shape.m[0][2] * d.m[2][0] + p.x;
        dst[1] = shape.m[0][2] * d.m[2][1] + p.y;
        dst[2] = shape.m[0][2] * d.m[2][2] + p.z;
        dst[6] = (d.m[1][0] * shape.m[1][1] + d.m[0][0] * shape.m[1][0]) + p.x;
        dst[7] = (d.m[1][1] * shape.m[1][1] + d.m[0][1] * shape.m[1][0]) + p.y;
        dst[8] = (d.m[1][2] * shape.m[1][1] + d.m[0][2] * shape.m[1][0]) + p.z;
        dst[12] = (shape.m[2][1] * d.m[1][0] + shape.m[2][0] * d.m[0][0]) + p.x;
        dst[13] = (shape.m[2][1] * d.m[1][1] + shape.m[2][0] * d.m[0][1]) + p.y;
        dst[14] = (shape.m[2][1] * d.m[1][2] + shape.m[2][0] * d.m[0][2]) + p.z;
        dst[18] = shape.m[3][2] * d.m[2][0] + p.x;
        dst[19] = shape.m[3][2] * d.m[2][1] + p.y;
        dst[20] = shape.m[3][2] * d.m[2][2] + p.z;
        dst[24] = p.x;
        dst[25] = p.y;
        dst[26] = p.z;
        ++g_weatherDrawn;
        dst += 30;
    }
}

// 0x1004b720
void I3D_scene::DrawWeather()
{
    if (!g_weatherDrawn || !(weatherFlags & WEATHER_ON))
        return;
    // The primary sector's fog applies (no fog light: the fog state stays as it is).
    if (I3D_light *fog = primarySector->fogLight; fog && g_driverStates[DRVSTATE_FOG]) {
        switch (fog->lightType) {
        case 5: {
            const float end = g_renderCamera->farRange * fog->d3dLight.Range;
            SetFog(1, 0, end * fog->d3dLight.Attenuation0, end, FogColor(fog));
            break;
        }
        case 6:
            SetFog(1, 1, fog->attenuation[0], 0.0f, FogColor(fog));
            break;
        case 7:
            SetFog(1, 2, fog->attenuation[0], 0.0f, FogColor(fog));
            break;
        default:
            break;
        }
    }
    if (g_rs.colorOp[0] != D3DTOP_MODULATE2X) {
        g_rs.colorOp[0] = D3DTOP_MODULATE2X;
        g_pGfx->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    }
    if (g_rs.alphaOp[0] != D3DTOP_MODULATE) {
        g_rs.alphaOp[0] = D3DTOP_MODULATE;
        g_pGfx->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    }
    if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
        g_rs.colorOp[1] = D3DTOP_DISABLE;
        g_pGfx->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    SetAlphaState(1);
    SetBlendState(0);
    ITexture *texture = ((weatherFlags & WEATHER_SNOW) && g_snowTexture) ? g_snowTexture : ig::g_defaultTexture;
    g_pGfx->SetTexture(0, texture->deviceTexture);
    ig::SetZWrite(0);
    g_pGfx->SetRenderState(D3DRS_LIGHTING, FALSE);
    if (g_rs.cull != 1) {
        g_rs.cull = 1;
        g_pGfx->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
    g_pIGraph->SetWorldMatrix(&kIdentity);
    SetVertexFormat(2);
    g_pGfx->SetIndices(g_weatherIB.ib, g_weatherVB.offset);
    g_pGfx->SetStreamSource(0, g_weatherVB.vb, 0x18);
    g_pGfx->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, g_weatherDrawn * 5, g_weatherIB.offset, g_weatherDrawn * 4);
    g_pGfx->SetRenderState(D3DRS_LIGHTING, TRUE);
}
