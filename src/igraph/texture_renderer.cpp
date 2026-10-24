// CTextureRenderer (0x10072e90 - 0x10073220), see texture_renderer.h.
#include "igraph/texture_renderer.h"

#include <cstdlib>
#include <cstring>

#include "core/debug.h"
#include "igraph/igraph_state.h"
#include "igraph/ishow.h"
#include "igraph/itexture.h"

using namespace ls3d::dshow;
using namespace ls3d::ig;

namespace {
// {71771540-2017-11cf-ae26-0020afd79767} (0x100a8ba0)
const CLSID kClsidTextureRenderer = {0x71771540, 0x2017, 0x11cf, {0xae, 0x26, 0x00, 0x20, 0xaf, 0xd7, 0x97, 0x67}};
}  // namespace

// (inlined into IShow::CreateTextureRenderer): the result is always S_OK.
CTextureRenderer::CTextureRenderer(HRESULT *hr)
    : CBaseVideoRenderer(kClsidTextureRenderer, nullptr, nullptr, hr), soundRenderer(nullptr), show(nullptr),
      format(D3DFMT_UNKNOWN)
{
    *hr = S_OK;
}

// 0x10072eb0
CTextureRenderer::~CTextureRenderer()
{
    if (soundRenderer)
        soundRenderer->Release();
}

// 0x10072f30
HRESULT CTextureRenderer::CheckMediaType(const CMediaType *pmt)
{
    if (*pmt->FormatType() != FORMAT_VideoInfo)
        return E_INVALIDARG;
    const GUID *subtype;
    if ((show->flags & ISHOW_TRUECOLOR) && g_fmtTrueColor)
        subtype = &MEDIASUBTYPE_RGB32;
    else if (g_fmt16Bit == D3DFMT_R5G6B5)
        subtype = &MEDIASUBTYPE_RGB565;
    else
        subtype = &MEDIASUBTYPE_RGB555;
    if (*pmt->Type() != MEDIATYPE_Video)
        return E_FAIL;
    return *pmt->Subtype() == *subtype ? S_OK : E_FAIL;
}

// 0x10072fe0
HRESULT CTextureRenderer::SetMediaType(const CMediaType *pmt)
{
    const VIDEOINFO *info = reinterpret_cast<const VIDEOINFO *>(pmt->Format());
    videoWidth = info->bmiHeader.biWidth;
    videoHeight = std::abs(info->bmiHeader.biHeight);
    if (info->bmiHeader.biBitCount == 32)
        videoPitch = videoWidth * 4;
    else
        videoPitch = (videoWidth & 1) ? videoWidth * 2 + 2 : videoWidth * 2;   // 16 bits, DWORD rows
    format = CreateShowTiles(videoWidth, videoHeight, show);
    return format ? S_OK : -1;
}

// 0x10073070: the frame is a bottom-up DIB.
HRESULT CTextureRenderer::DoRenderSample(IMediaSample *sample)
{
    BYTE *bits;
    sample->GetPointer(&bits);
    for (int i = show->tiles.Size(); i--;) {
        S_showTile &tile = show->tiles[i];
        int pitch;
        BYTE *dst = static_cast<BYTE *>(tile.texture->Lock(nullptr, &pitch, 0));
        int bytes = 0;
        switch (format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8: bytes = 4; break;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5: bytes = 2; break;
        default:
            dbgPrintf("IShow::DoRenderSample() - ERROR! Unsuppoted destination texture format: %s !",
                      g_pIGraph->GetFormatName(format));
        }
        if (bytes) {
            const BYTE *src = bits + (videoHeight - tile.y - 1) * videoPitch + tile.x * bytes;
            for (int y = tile.height; y; --y) {
                std::memcpy(dst, src, static_cast<size_t>(tile.width) * bytes);
                dst += pitch;
                src -= videoPitch;
            }
        }
        tile.texture->Unlock();
    }
    ++show->frameCount;
    return S_OK;
}
