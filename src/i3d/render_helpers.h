// Device state helpers of the visuals' draw code (0x100637a0 - 0x100647f0): z bias, materials,
// the sectors' ambient colors and the choice of the device lights a visual is lit by.
#pragma once

#include <cstdint>

#include "dx/d3d8.h"
#include "i3d/i3d.h"

class I3D_sector;
class I3D_visual;

// Light flags the renderer keeps in I3D_light::lightFlags.
enum : uint32_t {
    LIGHTF_DIRTY = 0x01,                    // d3dLight changed: SetLight before the next enable
    LIGHTF_MIRRORED = 0x02,                 // a copy made for a mirror's reflection (linkedLight: the source)
    LIGHTF_MOVED = 0x04,                    // changed since its mirrored copy was made
};

namespace ls3d::i3d {

// The device lights a visual is lit by (object, lightmap, billboard and single-mesh visuals keep
// this at +0x1F4).
struct S_visualLights {
    uint16_t indices[8];                    // +0x1F4 device light indices (g_lights)
    int count;                              // +0x204
    int stamp;                              // +0x208 the light sector's lightChanges + the visual's
                                            // changeCount when they were chosen (-1: never)
};
static_assert(sizeof(S_visualLights) == 0x18, "S_visualLights layout");

// Cached D3DRS_ZBIAS (0x100637a0).
void SetZBias(int bias);
// IGraph's texture stage caches (g_rs.colorOp / alphaOp; inlined in the original).
void SetColorOp(int stage, int op);
void SetAlphaOp(int stage, int op);
// D3DRS_FOGENABLE off through IGraph's cache (inlined).
void DisableFog();
// D3DRS_CULLMODE through IGraph's cache (0x1001b400): 1 none, 2 clockwise, 3 IGraph's default.
void SetCullState(int state);
// The vertex shader of an I3D vertex buffer format (0 - 9; cached in IGraph's g_rs.vertexShader;
// `force`: set even when cached) (0x1001b6b0).
void SetVertexFormat(int format, bool force = false);
// IGraph's alpha state through its cache (0x1001b4b0): 0 off, 1 blend + test (ref 1), 2 blend +
// test (ref 0), 3 test only (IGraph's reference).
void SetAlphaState(int state);
// IGraph's blend factors through its cache (0x1001b5c0): 0 alpha / inverse alpha, 1 alpha / one,
// 2 one / one, 3 dest color / zero, 4 dest color / src color.
void SetBlendState(int state);
// The device material: `material` with `alpha` (clamped to 0 - 1) as the diffuse alpha; with
// `level` below 1 a copy with every color scaled by it (0x10063810).
void SetMaterial(D3DMATERIAL8 &material, float alpha, float level);
// A sector's ambient colors (+0x19C its own ambient lights, +0x1A0 with SetMode bit 0x10) from
// its ambient lights when SCTF_AMBIENT_CHANGED is set (0x10063930).
void UpdateSectorAmbient(I3D_sector *sector);
// D3DRS_AMBIENT (cached) and the device lights `indices` enabled, the previous ones off (0x10063dc0).
void EnableLights(uint32_t ambient, int count, const uint16_t *indices);
// The sector SelectLights takes the lights of (0x10063ea0).
void SetLightSector(I3D_sector *sector);
I3D_sector *GetLightSector();
// The 8 strongest lights of the light sector that shine on the visual's world sphere (those whose
// mode has a bit of `mask`; point and spot lights by intensity / distance, directional ones by
// intensity), when the sector's lights or the visual changed (0x10063eb0).
void SelectLights(I3D_visual *visual, S_visualLights &lights, uint32_t mask);
// `dst` gets mirrored copies of the lights of `src` (g_renderMirror's reflection; the copies are
// device lights of their own, made on first use, refreshed once per mirror pass) (0x10064260).
void MirrorLights(const S_visualLights &src, S_visualLights &dst);
// (InvalidateLights, 0x100647f0: i3d_links.h.)

// 0x100695a0 (an I3D_bbox method): the corners of a box (16 bytes apart, w unset) in a static
// array: x from max to min, then y, then z from min to max.
const S_vector4 *BoxCorners(const I3D_bbox &box);

}  // namespace ls3d::i3d
