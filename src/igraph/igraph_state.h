// IGraph state shared with the rest of the engine (the I3D renderer reads the device, the formats
// and the render-state cache directly, as the original did with IGraph.cpp's globals).
#pragma once

#include <cstdint>

#include <windows.h>
#include <mmsystem.h>

#include "core/c_vector.h"
#include "core/i3d_result.h"
#include "dx/d3d8.h"
#include "igraph/igraph.h"

class ITexture;
class IShow;

namespace ls3d::ig {

extern IDirect3D8 *g_pD3D;                      // 0x100d1a94
extern IDirect3DDevice8 *g_pDevice;             // 0x100d216c
extern S_IGraphInit g_init;                     // 0x100d1d18
extern D3DPRESENT_PARAMETERS g_presentParams;   // 0x100d1740
extern D3DVIEWPORT8 g_viewport;                 // 0x100d1cf0 the device viewport (the I3D renderer sets it too)
extern D3DDEVTYPE g_deviceType;                 // 0x100a7804 (HAL; REF with IG_REFERENCE)
extern HWND g_mainWnd;                          // 0x100d19ec
extern HWND g_childWnd;                         // 0x100d1894
extern bool g_initialized;                      // 0x100d2195
extern bool g_inScene;                          // 0x100d2194 between BeginScene and EndScene
extern bool g_inactive;                         // 0x100d2190 the application is in the background
extern bool g_noActivation;                     // 0x100d2191 ignore WM_ACTIVATE

extern uint32_t g_caps;                         // 0x100d1dac IGCAPS_*
extern uint32_t g_quirks;                       // 0x100d215c IGQ_* below
extern int g_maxTextureWidth;                   // 0x100d2128
extern int g_maxTextureHeight;                  // 0x100d1ce8

// Texture formats chosen in Init (D3DFMT_UNKNOWN: not available).
extern D3DFORMAT g_fmt16Bit;                    // 0x100d1da8 R5G6B5 / X1R5G5B5 / A1R5G5B5
extern D3DFORMAT g_fmt1BitAlpha;                // 0x100d1a74 A1R5G5B5
extern D3DFORMAT g_fmtAlpha;                    // 0x100d1a78 A4R4G4B4 / A8R8G8B8
extern D3DFORMAT g_fmtTrueColor;                // 0x100d1774 A8R8G8B8
extern D3DFORMAT g_fmtDxt1;                     // 0x100d1898
extern D3DFORMAT g_fmtDxt3;                     // 0x100d1edc
extern D3DFORMAT g_fmtDxt5;                     // 0x100d1a90
extern D3DFORMAT g_fmtBump;                     // 0x100d1d10 V8U8

// Vertex shaders (declarations of D3DVSD_REG(0) position, ..., created by the device reset).
extern DWORD g_vsNormalTex1;                    // 0x100d2198 position, normal, uv
extern DWORD g_vsNormalTex2;                    // 0x100d219c position, normal, uv, uv
extern DWORD g_vsColorTex1;                     // 0x100d21ac position, colour, uv
extern DWORD g_vsColorTex2;                     // 0x100d21b0 position, colour, uv, uv
extern DWORD g_vsFarPlane;                      // 0x100d21b4 transformed by c0..c3 with z = w (sky)

// Texture coordinate generation matrices (D3DTS_TEXTURE0/1 in the default states): clip space
// x, y to u, v (0x100d1a30) and x, z to u, v (0x100d19f0).
extern D3DMATRIX g_texProjectXY;
extern D3DMATRIX g_texProjectXZ;

// IGraph::SetState's cache of device states (-1 / unset values force the next set).
struct S_stateCache {
    int alpha;                                  // 0x100d1ef0 IGS_ALPHA
    int blend;                                  // 0x100d1a88 IGS_BLEND
    int cull;                                   // 0x100d1cec IGS_CULL
    int cullDefault;                            // 0x100d2130 D3DCULL that IGS_CULL 3 means
    int colorOp[3];                             // 0x100d1a70, 0x100d214c, 0x100d2150
    int alphaOp[2];                             // 0x100d2160, 0x100d1f04
    int zBias;                                  // 0x100d177c
    int addressU0;                              // 0x100d2170
    int addressV0;                              // 0x100d2174
    int alphaRef;                               // 0x100a7800 (0x20 at start)
    int zFunc;                                  // 0x100a7808 (1 at start)
    int zWrite;                                 // 0x100d1d14
    int fog;                                    // 0x100d2120 D3DRS_FOGENABLE
    int specular;                               // 0x100d19c4
    int vertexShader;                           // 0x100d1a98 the renderer's vertex format index
                                                // (DrawPrimitive: 2 colour+uv, 6 / 7 transformed)
    int fogMode;                                // 0x100d1d64 0 linear, 1 / 2 exponential
    float fogStart;                             // 0x100d2154 (density for exponential fog)
    float fogEnd;                               // 0x100d1d08
    uint32_t fogColor;                          // 0x100d1778
    int unknown19c0;                            // 0x100d19c0 (renderer, 0x10037010)
    int unknown1888;                            // 0x100d1888 (renderer, 0x10037010)
    int unknown1780;                            // 0x100d1780 (renderer, 0x10037010)
};
extern S_stateCache g_rs;
// D3DRS_ZENABLE value of IGS_ZENABLE (0x100a6340): D3DZB_TRUE, or D3DZB_USEW (the driver's w-buffer state).
extern int g_zBufferType;

// The renderer's material for meshes without colors of their own (0x100d1ba0: white diffuse,
// ambient and emissive, no specular).
extern D3DMATERIAL8 g_defaultMaterial;

// Textures (LoadTexture shares the cached ones), video players.
extern C_vector<ITexture *> g_cachedTextures;   // 0x100d19dc
extern C_vector<ITexture *> g_uncachedTextures; // 0x100d19cc
extern C_vector<IShow *> g_shows;               // 0x100d1ee0
extern ITexture *g_defaultTexture;              // 0x100d21b8 8 x 8 white
extern ITexture *g_bumpAnimTextures[17];        // 0x100d1df8 BumpAnim_00.dds ...
extern uint16_t g_textureId;                    // 0x100d2192 last ITexture::id given out

// The texture loading thread (IG_TEXTURE_THREAD).
extern CRITICAL_SECTION g_textureLock;          // 0x100d1720
extern volatile int g_frameCounter;             // 0x100a7864 Flip count, under g_textureLock
extern HANDLE g_textureThread;                  // 0x100d21c8
extern bool g_textureThreadRun;                 // 0x100d21c7

// Debug overlays toggled by Ctrl+Alt+Shift+F12 / F11 / F10.
extern bool g_showFps;                          // 0x100d21c4
extern bool g_showSoundStats;                   // 0x100d21c5
extern bool g_logSoundFailures;                 // 0x100d21c6 F10: ISND_sound::Play logs failed plays

// Hardware quirks (g_quirks), from the device caps and known adapters.
enum : uint32_t {
    IGQ_NO_BLENDFACTORALPHA = 0x010,
    IGQ_NO_BLENDCURRENTALPHA = 0x020,
    IGQ_NO_TEXOP_ADD = 0x040,
    IGQ_MATROX = 0x080,
    IGQ_ATI = 0x100,
    IGQ_LIMITED = 0x200,                        // no multitexturing / 256-pixel textures / old chips
    IGQ_NO_DXT = 0x008,
};

// Cached D3DRS_ZWRITEENABLE / D3DRS_SPECULARENABLE (0x10063720 / 0x10063760, render helpers).
inline void SetZWrite(int on)
{
    if (g_rs.zWrite != on) {
        g_rs.zWrite = on;
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, on == 1);
    }
}
inline void SetSpecular(int on)
{
    if (g_rs.specular != on) {
        g_rs.specular = on;
        g_pDevice->SetRenderState(D3DRS_SPECULARENABLE, on == 1);
    }
}

// Input (igraph_input.cpp).
void ResetInputState();
HRESULT InitDirectInput(HINSTANCE instance);
void ReleaseDirectInput();
void SetSwapMouseButtons(bool swap);
int MouseMode();
bool KeyboardUsesMessages();
void QueueChar(int ch);
void KeyFromMessage(const MSG &msg);

// IGraph::ResizeImage (resize_image.cpp).
bool ResizeImage(int srcWidth, int srcHeight, int srcPitch, const uint8_t *src, int dstWidth, int dstHeight,
                 int dstPitch, uint8_t **dst, int bpp, bool filter);

// Applies the engine's default device states and resets the cache (0x1006a710).
void ResetDeviceStates();
// After a lost device or at Init: releases the default-pool buffers, resets the device (if
// `reset`) and re-creates the vertex shaders (0x1006be50).
HRESULT RestoreDevice(bool reset);
// The "!! Error detected" log line of a failed DirectX call (IGraph.cpp line numbers kept).
void LogDxError(HRESULT hr, int line);

}  // namespace ls3d::ig

// The 17 frames of the animated bump map, BumpAnim_00.dds ... (0x100699a0; the renderer loads them
// on first use).
bool LoadBumpAnimTextures(IGraph *igraph);
