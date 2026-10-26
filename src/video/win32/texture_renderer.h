// The DirectShow video renderer of the video device's graphs: hands each frame to the graph's
// FrameSink (IShow's tiles) (vtables 0x1009dae0 / 0x1009daa0 / 0x1009da8c / 0x1009da68 /
// 0x1009da54, 0x178 bytes; CLSID_TextureRenderer of the DirectX 8 SDK's DShowTextures sample).
#pragma once

#include "dshow/baseclasses.h"
#include "dx/d3d8_types.h"
#include "video/video.h"

class CTextureRenderer : public ls3d::dshow::CBaseVideoRenderer {
public:
    explicit CTextureRenderer(HRESULT *hr);
    ~CTextureRenderer() override;                                   // 0x10072eb0

    // The sink's format: RGB32, or the 16-bit layout (565 / 555).
    HRESULT CheckMediaType(const ls3d::dshow::CMediaType *pmt) override;    // 0x10072f30
    // Picture size and the sink's tiles.
    HRESULT SetMediaType(const ls3d::dshow::CMediaType *pmt) override;      // 0x10072fe0
    HRESULT DoRenderSample(IMediaSample *sample) override;          // 0x10073070

    int videoWidth;                                                 // +0x160
    int videoHeight;                                                // +0x164
    int videoPitch;                                                 // +0x168 bytes per row (DWORD aligned)
    IBaseFilter *soundRenderer;                                     // +0x16C the AVI's audio
    ls3d::video::FrameSink *sink;                                   // +0x170 (the original's IShow)
    D3DFORMAT format;                                               // +0x174 the tiles' texture format
};
static_assert(sizeof(CTextureRenderer) == 0x178, "CTextureRenderer layout");
