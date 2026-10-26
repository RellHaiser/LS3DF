// CTextureRenderer (0x10072e90 - 0x10073220), see texture_renderer.h.
#include "video/win32/texture_renderer.h"

#include <cstdlib>

using namespace ls3d::dshow;

namespace {
// {71771540-2017-11cf-ae26-0020afd79767} (0x100a8ba0)
const CLSID kClsidTextureRenderer = {0x71771540, 0x2017, 0x11cf, {0xae, 0x26, 0x00, 0x20, 0xaf, 0xd7, 0x97, 0x67}};
}  // namespace

// (inlined into IShow::CreateTextureRenderer): the result is always S_OK.
CTextureRenderer::CTextureRenderer(HRESULT *hr)
    : CBaseVideoRenderer(kClsidTextureRenderer, nullptr, nullptr, hr), soundRenderer(nullptr), sink(nullptr),
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
    const int frameFormat = sink->FrameFormat();
    const GUID *subtype = frameFormat == 32    ? &MEDIASUBTYPE_RGB32
                          : frameFormat == 565 ? &MEDIASUBTYPE_RGB565
                                               : &MEDIASUBTYPE_RGB555;
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
    format = sink->SetSize(videoWidth, videoHeight);
    return format ? S_OK : -1;
}

// 0x10073070: the frame is a bottom-up DIB, copied into the sink's tiles.
HRESULT CTextureRenderer::DoRenderSample(IMediaSample *sample)
{
    BYTE *bits;
    sample->GetPointer(&bits);
    sink->Frame(bits, videoPitch, videoHeight, format);
    return S_OK;
}
