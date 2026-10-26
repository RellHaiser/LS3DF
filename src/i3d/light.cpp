// I3D_light (0x10020c80 - 0x100232b0).
#include "i3d/light.h"

#include <cmath>

#include "core/debug.h"
#include "external/rw_data.h"
#include "i3d/i3d_hooks.h"
#include "i3d/loader.h"
#include "i3d/sector.h"
#include "igraph/igraph_state.h"

namespace ls3d::i3d {

C_vector<I3D_light *> g_lights(0x100);

int RegisterLight(I3D_light *light)
{
    g_lights.PushBack(light);
    return g_lights.Size() - 1;
}

void UnregisterLight(I3D_light *light)
{
    if (!(light->lightFlags & 2) && static_cast<uint16_t>(light->linkedLight) != 0xffff) {
        g_lights[static_cast<uint16_t>(light->linkedLight)]->Release();
        light->linkedLight = -1;
    }
    const uint16_t index = static_cast<uint16_t>(light->hardwareIndex);
    gfx::g_pGfx->LightEnable(index, FALSE);
    g_lights.PopBack();
    const int count = g_lights.Size();
    if (count)
        g_lights[index] = g_lights[count];
    if (index < count) {
        I3D_light *moved = g_lights[index];
        gfx::g_pGfx->LightEnable(static_cast<uint16_t>(moved->hardwareIndex), FALSE);
        moved->hardwareIndex = static_cast<int16_t>(index);
        moved->lightFlags |= 1;
        if (static_cast<uint16_t>(moved->linkedLight) != 0xffff)
            g_lights[static_cast<uint16_t>(moved->linkedLight)]->linkedLight = moved->hardwareIndex;
        for (I3D_sector *s : moved->sectors)
            ++s->lightChanges;
    }
}

}  // namespace ls3d::i3d

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

constexpr float kPi = 3.14159274f;

bool IsFogType(int type) { return type >= I3DLIGHT_FOG_FIRST && type <= I3DLIGHT_FOG_LAST; }

// range / (2 sin((pi - outer / 2) / 2)) (FSIN in extended precision; the sum and the division
// round to the FPU's 24 bits).
float ConeRange(float range, float outer)
{
    const double s = std::sin(static_cast<double>((kPi - outer * 0.5f) * 0.5f));
    const float twoSin = static_cast<float>(s + s);
    return range / twoSin;
}

}  // namespace

void I3D_light::NotifySectors()
{
    if (lightType == I3DLIGHT_AMBIENT) {
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->sectorFlags |= SCTF_AMBIENT_CHANGED;
    } else {
        for (int i = sectors.Size(); i > 0; --i)
            ++sectors[i - 1]->lightChanges;
    }
}

// The cone's cosines and the falloff between them (FCOS keeps extended precision for the
// difference).
static void SetConeCosines(I3D_light &l)
{
    const double outerCos = std::cos(static_cast<double>(l.coneOuter * 0.5f));
    l.coneOuterCos = static_cast<float>(outerCos);
    const double innerCos = std::cos(static_cast<double>(l.coneInner * 0.5f));
    l.coneInnerCos = static_cast<float>(innerCos);
    const float diff = static_cast<float>(innerCos - outerCos);
    l.coneFalloff = diff <= 0.0f ? 0.0f : 1.0f / diff;
}

// The cone's center: coneRangeWorld along the normalized z axis (the squared length summed
// with `square`) from the position.
template <float Square(const S_vector &)> static void SetConeCenter(I3D_light &l)
{
    S_vector dir = l.GetMatrix()(2);
    NormalizeWithSquare(dir, Square(dir));
    const float r = l.coneRangeWorld;
    const S_matrix &w = l.GetMatrix();
    l.coneCenter = S_vector(dir.x * r + w.m[3][0], dir.y * r + w.m[3][1], dir.z * r + w.m[3][2]);
}

void I3D_light::Init()
{
    I3D_frame::Init();
    color = S_vector(1.0f, 1.0f, 1.0f);
    specularColor = S_vector(1.0f, 1.0f, 1.0f);
    ambientColor = S_vector(0.0f, 0.0f, 0.0f);
    power = 1.0f;
    specularPower = 1.0f;
    ambientPower = 1.0f;
    falloff = 1.0f;
    mode = 0xff6b;
    lightType = I3DLIGHT_DIRECTIONAL;
    range = 100.0f;
    attenuation[0] = 0.0f;
    attenuation[1] = 0.1f;
    attenuation[2] = 0.0f;
    coneInner = 0x1.657186p-2f;             // 0x3eb2b8c3: 20 degrees
    coneOuter = 0x1.657186p-1f;             // 0x3f32b8c3: 40 degrees
    SetDir(S_vector(1.0f, -1.0f, 1.0f), 0.0f);
    // (The colors' fourth dwords: the 1.0 the original has on the stack there.)
    effectiveColor = S_vector((power * color.x) * 0.5f, (color.y * power) * 0.5f, (color.z * power) * 0.5f);
    effectiveColorW = 1.0f;
    effectiveSpecular = S_vector((specularPower * specularColor.x) * 0.5f, (specularColor.y * specularPower) * 0.5f,
                                 (specularColor.z * specularPower) * 0.5f);
    effectiveSpecularW = 1.0f;
    effectiveAmbient = S_vector((ambientPower * ambientColor.x) * 0.5f, (ambientColor.y * ambientPower) * 0.5f,
                                (ambientColor.z * ambientPower) * 0.5f);
    effectiveAmbientW = 1.0f;
    coneRange = ConeRange(range, coneOuter);
    coneRangeWorld = coneRange * worldScale;
    SetConeCenter<SquareXYZ>(*this);
    Update();
    SetConeCosines(*this);
    d3dLight.Position = D3DVECTOR{0.0f, 0.0f, 0.0f};
    // The local direction: the SDK's inline GetLocalDir once per component (only the first can
    // find the matrix stale, the others normalize the row).
    const auto localDir = [this](int c) {
        if (!(frameFlags & FRMFLAGS_LMATRIX_VALID)) {
            frameFlags |= FRMFLAGS_LMATRIX_VALID;
            rot.Normalize();
            localMatrix.SetRot3(rot);
            return localMatrix.m[2][c];
        }
        S_vector d = localMatrix(2);
        NormalizeWithSquare(d, SquareXYZ(d));
        return c == 0 ? d.x : c == 1 ? d.y : d.z;
    };
    d3dLight.Direction.x = localDir(0);
    d3dLight.Direction.y = localDir(1);
    d3dLight.Direction.z = localDir(2);
    d3dLight.Range = range * worldScale;
    d3dLight.Diffuse = D3DCOLORVALUE{effectiveColor.x, effectiveColor.y, effectiveColor.z, 0.0f};
    d3dLight.Specular = D3DCOLORVALUE{effectiveSpecular.x, effectiveSpecular.y, effectiveSpecular.z, 0.0f};
    d3dLight.Ambient = D3DCOLORVALUE{effectiveAmbient.x, effectiveAmbient.y, effectiveAmbient.z, 0.0f};
    d3dLight.Phi = coneOuter;
    d3dLight.Attenuation0 = attenuation[0];
    d3dLight.Type = D3DLIGHT_DIRECTIONAL;
    d3dLight.Falloff = 1.0f;
    d3dLight.Theta = coneInner;
    linkedLight = -1;
    unknown25A = -1;
    lightFlags = 5;
    d3dLight.Attenuation1 = attenuation[1] / worldScale;
    d3dLight.Attenuation2 = attenuation[2] / (worldScale * worldScale);
    hardwareIndex = static_cast<int16_t>(RegisterLight(this));
}

void I3D_light::Uninit()
{
    UnregisterLight(this);
    if (lightType == I3DLIGHT_AMBIENT) {
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->sectorFlags |= SCTF_AMBIENT_CHANGED;
    }
    // (Each RemoveLight swaps the sector out of `sectors`; the walk still meets every sector once,
    // as the original's: the entries after the current one keep their places.)
    for (I3D_sector *s : sectors)
        s->RemoveLight(this);
    sectors.Clear();
    I3D_frame::Uninit();
}

void __stdcall I3D_light::SetOn(bool on)
{
    if (on == ((frameFlags & FRMFLAGS_ON) != 0))
        return;
    if (on)
        frameFlags |= FRMFLAGS_ON;
    else
        frameFlags &= ~FRMFLAGS_ON;
    NotifySectors();
    MatrixChanged();
}

I3D_RESULT __stdcall I3D_light::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type == FRAME_LIGHT) {
        auto *src = static_cast<I3D_light *>(source);
        color = src->color;
        colorW = src->colorW;
        effectiveColor = src->effectiveColor;
        effectiveColorW = src->effectiveColorW;
        specularColor = src->specularColor;
        specularColorW = src->specularColorW;
        effectiveSpecular = src->effectiveSpecular;
        effectiveSpecularW = src->effectiveSpecularW;
        ambientColor = src->ambientColor;
        ambientColorW = src->ambientColorW;
        effectiveAmbient = src->effectiveAmbient;
        effectiveAmbientW = src->effectiveAmbientW;
        mode = src->mode;
        lightType = src->lightType;
        power = src->power;
        specularPower = src->specularPower;
        ambientPower = src->ambientPower;
        range = src->range;
        falloff = src->falloff;
        attenuation[0] = src->attenuation[0];
        attenuation[1] = src->attenuation[1];
        attenuation[2] = src->attenuation[2];
        coneInner = src->coneInner;
        coneOuter = src->coneOuter;
        d3dLight = src->d3dLight;
        MatrixChanged();
    }
    return I3D_OK;
}

void I3D_light::MatrixChanged()
{
    lightFlags |= 5;
    for (I3D_sector *s : sectors)
        ++s->lightChanges;
    switch (lightType) {
    case I3DLIGHT_POINT: {
        const S_vector &pos = GetMatrix()(3);
        d3dLight.Position = D3DVECTOR{pos.x, pos.y, pos.z};
        d3dLight.Range = range * worldScale;
        d3dLight.Attenuation1 = attenuation[1] / worldScale;
        d3dLight.Attenuation2 = attenuation[2] / (worldScale * worldScale);
        break;
    }
    case I3DLIGHT_SPOT: {
        const S_vector &pos = GetMatrix()(3);
        d3dLight.Position = D3DVECTOR{pos.x, pos.y, pos.z};
        S_vector dir = GetMatrix()(2);
        NormalizeWithSquare(dir, SquareYZX(dir));
        d3dLight.Direction = D3DVECTOR{dir.x, dir.y, dir.z};
        d3dLight.Range = range * worldScale;
        d3dLight.Attenuation1 = attenuation[1] / worldScale;
        d3dLight.Attenuation2 = attenuation[2] / (worldScale * worldScale);
        coneRangeWorld = coneRange * worldScale;
        SetConeCenter<SquareYZX>(*this);
        break;
    }
    case I3DLIGHT_DIRECTIONAL: {
        S_vector dir = GetMatrix()(2);
        NormalizeWithSquare(dir, SquareXYZ(dir));
        d3dLight.Direction = D3DVECTOR{dir.x, dir.y, dir.z};
        break;
    }
    case 5:
    case 6:
    case 7:
        d3dLight.Attenuation0 = attenuation[0] * 0.01f;
        d3dLight.Range = range * 0.01f;
        if (!(frameFlags & FRMFLAGS_ON)) {
            // The sectors that had it as their fog take the first other fog light that is on.
            for (I3D_sector *s : sectors) {
                if (s->fogLight != this)
                    continue;
                s->fogLight = nullptr;
                for (I3D_light *l : s->lights) {
                    if ((l->frameFlags & FRMFLAGS_ON) && IsFogType(l->lightType)) {
                        s->fogLight = l;
                        break;
                    }
                }
            }
        } else {
            for (I3D_sector *s : sectors)
                s->fogLight = this;
        }
        break;
    }
}

void I3D_light::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &color, 0x60);      // the colors, mode, type, powers, range ... cone
    // (The loaded colors shine without the factor 0.5 the setters apply. The original copies each
    // as 16 bytes of a stack temporary: their w words get a value it never wrote; the port keeps
    // Init's.)
    effectiveColor = S_vector(power * color.x, color.y * power, color.z * power);
    effectiveSpecular = S_vector(specularPower * specularColor.x, specularColor.y * specularPower,
                                 specularColor.z * specularPower);
    effectiveAmbient = S_vector(ambientPower * ambientColor.x, ambientColor.y * ambientPower,
                                ambientColor.z * ambientPower);
    coneRange = ConeRange(range, coneOuter);
    coneRangeWorld = coneRange * worldScale;
    SetConeCenter<SquareYZX>(*this);
    SetConeCosines(*this);
    if (lightType == I3DLIGHT_POINT)
        d3dLight.Type = D3DLIGHT_POINT;
    else if (lightType == I3DLIGHT_SPOT)
        d3dLight.Type = D3DLIGHT_SPOT;
    else if (lightType == I3DLIGHT_DIRECTIONAL)
        d3dLight.Type = D3DLIGHT_DIRECTIONAL;
    d3dLight.Diffuse.r = effectiveColor.x;
    d3dLight.Range = range * worldScale;
    d3dLight.Diffuse.g = effectiveColor.y;
    d3dLight.Diffuse.b = effectiveColor.z;
    d3dLight.Specular.r = effectiveSpecular.x;
    d3dLight.Specular.g = effectiveSpecular.y;
    d3dLight.Specular.b = effectiveSpecular.z;
    d3dLight.Ambient.r = effectiveAmbient.x;
    d3dLight.Ambient.g = effectiveAmbient.y;
    d3dLight.Ambient.b = effectiveAmbient.z;
    d3dLight.Falloff = falloff;
    d3dLight.Attenuation0 = attenuation[0];
    d3dLight.Theta = coneInner;
    d3dLight.Phi = coneOuter;
    lightFlags = 5;
    d3dLight.Attenuation1 = attenuation[1] / worldScale;
    d3dLight.Attenuation2 = attenuation[2] / (worldScale * worldScale);
}

void __stdcall I3D_light::SetLightType(int newType)
{
    const int old = lightType;
    if (old == newType)
        return;
    lightFlags |= 5;
    if (newType == I3DLIGHT_POINT)
        d3dLight.Type = D3DLIGHT_POINT;
    else if (newType == I3DLIGHT_SPOT)
        d3dLight.Type = D3DLIGHT_SPOT;
    else if (newType == I3DLIGHT_DIRECTIONAL)
        d3dLight.Type = D3DLIGHT_DIRECTIONAL;
    if (old == I3DLIGHT_AMBIENT) {
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->sectorFlags |= SCTF_AMBIENT_CHANGED;
    } else if (IsFogType(old)) {
        // A fog light leaves its sectors' fog to come back as its new type. (Each RemoveLight also
        // takes the sector off `sectors`, so the second loop finds none: the light ends up in no
        // sector. Kept.)
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->RemoveLight(this);
        lightType = newType;
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->AddLight(this);
        return;
    }
    lightType = newType;
    switch (newType) {
    case I3DLIGHT_SPOT:
        for (int i = sectors.Size(); i > 0; --i)
            ++sectors[i - 1]->lightChanges;
        coneRange = ConeRange(range, coneOuter);
        break;
    case I3DLIGHT_DIRECTIONAL:
        for (int i = sectors.Size(); i > 0; --i)
            ++sectors[i - 1]->lightChanges;
        break;
    case I3DLIGHT_AMBIENT:
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->sectorFlags |= SCTF_AMBIENT_CHANGED;
        break;
    case 5:
    case 6:
    case 7:
        for (int i = sectors.Size(); i > 0; --i)
            sectors[i - 1]->AddLight(this);
        break;
    default:
        // (A point light: the sectors are not told.)
        break;
    }
}

// (The original copies a fourth dword after the colors given, and an unset value into the
// effective colors' fourth dwords.)
void __stdcall I3D_light::SetColorRGB(float r, float g, float b)
{
    color = S_vector(r, g, b);
    lightFlags |= 5;
    effectiveColor = S_vector((power * color.x) * 0.5f, (color.y * power) * 0.5f, (color.z * power) * 0.5f);
    d3dLight.Diffuse.r = effectiveColor.x;
    d3dLight.Diffuse.b = effectiveColor.z;
    d3dLight.Diffuse.g = effectiveColor.y;
    NotifySectors();
}

void __stdcall I3D_light::SetColor(const S_vector &c)
{
    lightFlags |= 5;
    color = c;
    effectiveColor = S_vector((power * color.x) * 0.5f, (color.y * power) * 0.5f, (color.z * power) * 0.5f);
    d3dLight.Diffuse.r = effectiveColor.x;
    d3dLight.Diffuse.b = effectiveColor.z;
    d3dLight.Diffuse.g = effectiveColor.y;
    NotifySectors();
}

void __stdcall I3D_light::SetSpecularColor(const S_vector &c)
{
    lightFlags |= 5;
    specularColor = c;
    effectiveSpecular = S_vector((specularPower * specularColor.x) * 0.5f, (specularColor.y * specularPower) * 0.5f,
                                 (specularColor.z * specularPower) * 0.5f);
    d3dLight.Specular.r = effectiveSpecular.x;
    d3dLight.Specular.b = effectiveSpecular.z;
    d3dLight.Specular.g = effectiveSpecular.y;
    NotifySectors();
}

void __stdcall I3D_light::SetAmbientColor(const S_vector &c)
{
    lightFlags |= 5;
    ambientColor = c;
    effectiveAmbient = S_vector((ambientPower * ambientColor.x) * 0.5f, (ambientColor.y * ambientPower) * 0.5f,
                                (ambientColor.z * ambientPower) * 0.5f);
    d3dLight.Ambient.r = effectiveAmbient.x;
    d3dLight.Ambient.b = effectiveAmbient.z;
    d3dLight.Ambient.g = effectiveAmbient.y;
    NotifySectors();
}

void __stdcall I3D_light::SetPower(float p)
{
    power = p;
    lightFlags |= 5;
    effectiveColor = S_vector((power * color.x) * 0.5f, (color.y * power) * 0.5f, (color.z * power) * 0.5f);
    d3dLight.Diffuse.r = effectiveColor.x;
    d3dLight.Diffuse.b = effectiveColor.z;
    d3dLight.Diffuse.g = effectiveColor.y;
    NotifySectors();
}

void __stdcall I3D_light::SetSpecularPower(float p)
{
    specularPower = p;
    lightFlags |= 5;
    effectiveSpecular = S_vector((specularPower * specularColor.x) * 0.5f, (specularColor.y * specularPower) * 0.5f,
                                 (specularColor.z * specularPower) * 0.5f);
    d3dLight.Specular.r = effectiveSpecular.x;
    d3dLight.Specular.b = effectiveSpecular.z;
    d3dLight.Specular.g = effectiveSpecular.y;
    NotifySectors();
}

void __stdcall I3D_light::SetAmbientPower(float p)
{
    ambientPower = p;
    lightFlags |= 5;
    effectiveAmbient = S_vector((ambientPower * ambientColor.x) * 0.5f, (ambientColor.y * ambientPower) * 0.5f,
                                (ambientColor.z * ambientPower) * 0.5f);
    d3dLight.Ambient.r = effectiveAmbient.x;
    d3dLight.Ambient.b = effectiveAmbient.z;
    d3dLight.Ambient.g = effectiveAmbient.y;
    NotifySectors();
}

void __stdcall I3D_light::SetMode(uint32_t m)
{
    if (mode == m)
        return;
    mode = m;
    NotifySectors();
}

void __stdcall I3D_light::SetRange(float r)
{
    lightFlags |= 5;
    d3dLight.Range = r * worldScale;
    range = r;
    for (int i = sectors.Size(); i > 0; --i)
        ++sectors[i - 1]->lightChanges;
    if (lightType == I3DLIGHT_SPOT)
        coneRange = ConeRange(range, coneOuter);
}

void __stdcall I3D_light::SetAttenuation(float a0, float a1, float a2)
{
    lightFlags |= 5;
    attenuation[0] = a0;
    attenuation[2] = a2;
    attenuation[1] = a1;
    d3dLight.Attenuation0 = a0;
    d3dLight.Attenuation1 = a1 / worldScale;
    d3dLight.Attenuation2 = a2 / (worldScale * worldScale);
    for (int i = sectors.Size(); i > 0; --i)
        ++sectors[i - 1]->lightChanges;
}

void __stdcall I3D_light::SetFogRange(float nearRange, float farRange)
{
    if (!IsFogType(lightType)) {
        dbgPrintf("ERROR !! I3D_light::SetFogRange() - Fog ranges can't be set on nofog light !!");
        return;
    }
    lightFlags |= 5;
    d3dLight.Attenuation0 = nearRange * 0.01f;
    attenuation[0] = nearRange;
    range = farRange;
    d3dLight.Range = farRange * 0.01f;
}

void __stdcall I3D_light::SetCone(float inner, float outer)
{
    lightFlags |= 5;
    coneOuter = outer > kPi ? kPi : outer;
    coneInner = inner > coneOuter ? coneOuter : inner;
    d3dLight.Phi = coneOuter;
    d3dLight.Theta = coneInner;
    SetConeCosines(*this);
    for (int i = sectors.Size(); i > 0; --i)
        ++sectors[i - 1]->lightChanges;
    coneRange = ConeRange(range, coneOuter);
}
