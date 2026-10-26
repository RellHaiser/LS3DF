// IShow's DirectShow video renderer: copies each frame into the player's texture tiles
// (vtables 0x1009dae0 / 0x1009daa0 / 0x1009da8c / 0x1009da68 / 0x1009da54, 0x178 bytes;
// CLSID_TextureRenderer of the DirectX 8 SDK's DShowTextures sample).
#pragma once

#include "dshow/baseclasses.h"
#include "dx/d3d8_types.h"

class IShow;

class CTextureRenderer : public ls3d::dshow::CBaseVideoRenderer {
public:
    explicit CTextureRenderer(HRESULT *hr);
    ~CTextureRenderer() override;                                   // 0x10072eb0

    // RGB32 (ISHOW_TRUECOLOR with a true-colour texture format), else the 16-bit format's layout.
    HRESULT CheckMediaType(const ls3d::dshow::CMediaType *pmt) override;    // 0x10072f30
    // Picture size and the tiles.
    HRESULT SetMediaType(const ls3d::dshow::CMediaType *pmt) override;      // 0x10072fe0
    HRESULT DoRenderSample(IMediaSample *sample) override;          // 0x10073070

    int videoWidth;                                                 // +0x160
    int videoHeight;                                                // +0x164
    int videoPitch;                                                 // +0x168 bytes per row (DWORD aligned)
    IBaseFilter *soundRenderer;                                     // +0x16C the AVI's audio
    IShow *show;                                                    // +0x170
    D3DFORMAT format;                                               // +0x174 the tiles' texture format
};
static_assert(sizeof(CTextureRenderer) == 0x178, "CTextureRenderer layout");
