// I3D_light (FRAME_LIGHT; vtable 0x1009be08, 0x260 bytes; 0x10020c80 - 0x100232b0).
//
// A light has three colors with a power each (diffuse, specular, ambient: the color times power
// x 0.5 shines), a range with attenuation, a spot cone, and the Direct3D light it sets (d3dLight,
// in world space). Fog lights (types 5 - 7) use the range as the fog's (x 0.01 for Direct3D).
// The sectors it was added to (I3D_sector::AddLight) recount their lighting when it changes.
#pragma once

#include "dx/d3d8.h"

#include "core/c_vector.h"
#include "i3d/frame.h"

class I3D_sector;

// Light types (I3D_light::lightType).
enum : int {
    I3DLIGHT_POINT = 1,
    I3DLIGHT_SPOT = 2,
    I3DLIGHT_DIRECTIONAL = 3,
    I3DLIGHT_AMBIENT = 4,
    I3DLIGHT_FOG_FIRST = 5,                 // 5 - 7: fog (the sector's fog light)
    I3DLIGHT_FOG_LAST = 7,
};

// SetMode bits the engine reads.
enum : uint32_t {
    LIGHTMODE_LIGHTMAP = 0x02,              // shines into the lightmaps I3D_lit_object::Construct computes
    LIGHTMODE_SHADOW = 0x08,                // casts the shadows' shadows
    LIGHTMODE_LMSHADOW = 0x20,              // ... its light blocked by faces between (LMCONS_SHADOWS)
};

class I3D_light : public I3D_frame {
public:
    // 0x30 0x10022b90: (the sectors recount; no parent bounds cleared, unlike the base).
    void __stdcall SetOn(bool on) override;
    // 0x4C 0x10022cb0: also the light settings of another light.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10020c80 / 0x10021760
    void Init() override;
    void Uninit() override;
    // 0x68 0x10022500: the Direct3D light follows the frame.
    void MatrixChanged() override;
    // 0x70 0x10022e60: the frame, then the 0x60 bytes of settings.
    void Load() override;
    virtual void __stdcall SetLightType(int type);                     // 0x74 0x100217e0
    // (Distinct names: MSVC orders overloaded virtual functions in reverse in the vtable.)
    virtual void __stdcall SetColorRGB(float r, float g, float b);     // 0x78 0x10021b10
    virtual void __stdcall SetColor(const S_vector &color);            // 0x7C 0x100219e0
    virtual void __stdcall SetSpecularColor(const S_vector &color);    // 0x80 0x10021c50
    virtual void __stdcall SetAmbientColor(const S_vector &color);     // 0x84 0x10021db0
    virtual void __stdcall SetPower(float power);                      // 0x88 0x10021ee0
    virtual void __stdcall SetSpecularPower(float power);              // 0x8C 0x10022000
    virtual void __stdcall SetAmbientPower(float power);               // 0x90 0x10022120
    virtual void __stdcall SetMode(uint32_t mode);                     // 0x94 0x10022c30
    virtual void __stdcall SetRange(float range);                      // 0x98 0x10022240
    virtual void __stdcall SetAttenuation(float a0, float a1, float a2);   // 0x9C 0x100222d0
    // 0xA0 0x10022360: fog lights only.
    virtual void __stdcall SetFogRange(float nearRange, float farRange);
    // 0xA4 0x100223d0: the cone angles (radians; outer at most pi, inner at most outer).
    virtual void __stdcall SetCone(float inner, float outer);

    // The sectors recount their lighting (ambient lights: their ambient sum).
    void NotifySectors();

    ls3d::C_vector<I3D_sector *> sectors;   // +0x120 the sectors it shines in
    S_vector color;                         // +0x130
    float colorW;
    S_vector specularColor;                 // +0x140
    float specularColorW;
    S_vector ambientColor;                  // +0x150
    float ambientColorW;
    uint32_t mode;                          // +0x160
    int lightType;                          // +0x164 I3DLIGHT_*
    float power;                            // +0x168
    float specularPower;                    // +0x16C
    float ambientPower;                     // +0x170
    float range;                            // +0x174 (fog lights: the fog's far end)
    float falloff;                          // +0x178
    float attenuation[3];                   // +0x17C (fog lights: [0] the fog's near end)
    float coneInner;                        // +0x188
    float coneOuter;                        // +0x18C
    uint32_t lightFlags;                    // +0x190 (5: changed, for the renderer)
    uint32_t unknown194[3];                 // +0x194
    S_vector effectiveColor;                // +0x1A0 color x power x 0.5
    float effectiveColorW;
    S_vector effectiveSpecular;             // +0x1B0
    float effectiveSpecularW;
    S_vector effectiveAmbient;              // +0x1C0
    float effectiveAmbientW;
    float coneOuterCos;                     // +0x1D0 cos(outer / 2)
    float coneInnerCos;                     // +0x1D4
    float coneFalloff;                      // +0x1D8 1 / (inner cos - outer cos), 0 for none
    float coneRange;                        // +0x1DC range / (2 sin((pi - outer / 2) / 2))
    S_vector coneCenter;                    // +0x1E0 a spot's cone center: along its direction
    float coneRangeWorld;                   // +0x1EC coneRange x the world scale
    D3DLIGHT8 d3dLight;                     // +0x1F0 in world space
    int16_t linkedLight;                    // +0x258 index of a light this one owns, -1 if none
    int16_t unknown25A;                     // +0x25A (-1)
    int16_t hardwareIndex;                  // +0x25C its index in g_lights: the device light it is set as
    int16_t padding25E;
};
static_assert(sizeof(I3D_light) == 0x260, "I3D_light layout");
static_assert(offsetof(I3D_light, mode) == 0x160, "I3D_light layout");
static_assert(offsetof(I3D_light, lightFlags) == 0x190, "I3D_light layout");
static_assert(offsetof(I3D_light, d3dLight) == 0x1F0, "I3D_light layout");
static_assert(offsetof(I3D_light, hardwareIndex) == 0x25C, "I3D_light layout");

namespace ls3d::i3d {

// Every light, by device light index (0x100d13bc; render helpers 0x10063c20 / 0x10063ca0).
extern C_vector<I3D_light *> g_lights;
int RegisterLight(I3D_light *light);
// Takes the light out; the last one moves into its device light.
void UnregisterLight(I3D_light *light);

}  // namespace ls3d::i3d
