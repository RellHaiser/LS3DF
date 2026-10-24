// DirectShow base classes: CBaseDispatch, CMediaPosition, CPosPassThru, CRendererPosPassThru
// (ctlutil, 0x1008bd30 - 0x1008d120). The renderer passes seeking calls up to the pin it is
// connected to, and answers the current position from the last sample's times.
#include "dshow/baseclasses.h"

namespace ls3d::dshow {

namespace {
// LIBID_QuartzTypeLib (0x100a1ed8)
const GUID kQuartzTypeLib = {0x56a868b0, 0x0ad4, 0x11ce, {0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

typedef HRESULT(STDAPICALLTYPE *LoadTypeLibFn)(const OLECHAR *file, ITypeLib **lib);
typedef HRESULT(STDAPICALLTYPE *LoadRegTypeLibFn)(REFGUID guid, WORD major, WORD minor, LCID lcid, ITypeLib **lib);

inline HRESULT HresultFromWin32(DWORD error)
{
    return static_cast<HRESULT>(0x80070000 | error);
}
}  // namespace

// -- CBaseDispatch ------------------------------------------------------------------------------

CBaseDispatch::~CBaseDispatch()
{
    if (m_pti)
        m_pti->Release();
}

// 0x1008bd30: the type info from Quartz's type library (registered, else control.tlb), through
// OleAut32 loaded on demand.
HRESULT CBaseDispatch::GetTypeInfo(REFIID riid, UINT itinfo, LCID lcid, ITypeInfo **pptinfo)
{
    if (!pptinfo)
        return E_POINTER;
    *pptinfo = nullptr;
    if (itinfo != 0)
        return TYPE_E_ELEMENTNOTFOUND;
    if (!m_pti) {
        HMODULE oleAut = LoadOleAut32();
        if (!oleAut)
            return HresultFromWin32(GetLastError());
        auto loadRegTypeLib = reinterpret_cast<LoadRegTypeLibFn>(GetProcAddress(oleAut, "LoadRegTypeLib"));
        if (!loadRegTypeLib)
            return HresultFromWin32(GetLastError());
        ITypeLib *lib;
        HRESULT hr = loadRegTypeLib(kQuartzTypeLib, 1, 0, lcid, &lib);
        if (FAILED(hr)) {
            auto loadTypeLib = reinterpret_cast<LoadTypeLibFn>(GetProcAddress(oleAut, "LoadTypeLib"));
            if (!loadTypeLib)
                return HresultFromWin32(GetLastError());
            hr = loadTypeLib(L"control.tlb", &lib);
            if (FAILED(hr))
                return hr;
        }
        hr = lib->GetTypeInfoOfGuid(riid, &m_pti);
        lib->Release();
        if (FAILED(hr))
            return hr;
    }
    *pptinfo = m_pti;
    m_pti->AddRef();
    return S_OK;
}

HRESULT CBaseDispatch::GetIDsOfNames(REFIID riid, OLECHAR **names, UINT count, LCID lcid, DISPID *dispids)
{
    ITypeInfo *ti;
    HRESULT hr = GetTypeInfo(riid, 0, lcid, &ti);
    if (SUCCEEDED(hr)) {
        hr = ti->GetIDsOfNames(names, count, dispids);
        ti->Release();
    }
    return hr;
}

// -- CMediaPosition -----------------------------------------------------------------------------

CMediaPosition::CMediaPosition(const TCHAR *name, IUnknown *outer) : CUnknown(name, outer)
{
}

HRESULT __stdcall CMediaPosition::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IMediaPosition)
        return GetInterface(static_cast<IMediaPosition *>(this), ppv);
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

// 0x1008be40
HRESULT __stdcall CMediaPosition::GetTypeInfoCount(UINT *count)
{
    if (!count)
        return E_POINTER;
    *count = 1;
    return S_OK;
}

// 0x1008be60
HRESULT __stdcall CMediaPosition::GetTypeInfo(UINT itinfo, LCID lcid, ITypeInfo **pptinfo)
{
    return m_basedisp.GetTypeInfo(IID_IMediaPosition, itinfo, lcid, pptinfo);
}

// 0x1008be90
HRESULT __stdcall CMediaPosition::GetIDsOfNames(REFIID, LPOLESTR *names, UINT count, LCID lcid, DISPID *dispids)
{
    return m_basedisp.GetIDsOfNames(IID_IMediaPosition, names, count, lcid, dispids);
}

// 0x1008bee0
HRESULT __stdcall CMediaPosition::Invoke(DISPID dispid, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params,
                                         VARIANT *result, EXCEPINFO *excepinfo, UINT *argErr)
{
    if (riid != IID_NULL)
        return DISP_E_UNKNOWNINTERFACE;
    ITypeInfo *ti;
    HRESULT hr = GetTypeInfo(0, lcid, &ti);
    if (FAILED(hr))
        return hr;
    hr = ti->Invoke(static_cast<IMediaPosition *>(this), dispid, flags, params, result, excepinfo, argErr);
    ti->Release();
    return hr;
}

// -- CPosPassThru -------------------------------------------------------------------------------

CPosPassThru::CPosPassThru(const TCHAR *name, IUnknown *outer, HRESULT *phr, IPin *pin)
    : CMediaPosition(name, outer), m_pPin(pin)
{
    if (!pin)
        *phr = E_POINTER;
}

// 0x1008c000
HRESULT __stdcall CPosPassThru::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IMediaSeeking)
        return GetInterface(static_cast<IMediaSeeking *>(this), ppv);
    return CMediaPosition::NonDelegatingQueryInterface(riid, ppv);
}

HRESULT CPosPassThru::GetPeer(IMediaPosition **out)
{
    *out = nullptr;
    IPin *connected;
    if (FAILED(m_pPin->ConnectedTo(&connected)))
        return E_NOTIMPL;
    IMediaPosition *position;
    const HRESULT hr = connected->QueryInterface(IID_IMediaPosition, reinterpret_cast<void **>(&position));
    connected->Release();
    if (FAILED(hr))
        return E_NOTIMPL;
    *out = position;
    return S_OK;
}

HRESULT CPosPassThru::GetPeerSeeking(IMediaSeeking **out)
{
    *out = nullptr;
    IPin *connected;
    if (FAILED(m_pPin->ConnectedTo(&connected)))
        return E_NOTIMPL;
    IMediaSeeking *seeking;
    const HRESULT hr = connected->QueryInterface(IID_IMediaSeeking, reinterpret_cast<void **>(&seeking));
    connected->Release();
    if (FAILED(hr))
        return E_NOTIMPL;
    *out = seeking;
    return S_OK;
}

HRESULT CPosPassThru::GetSeekingLongLong(HRESULT(__stdcall IMediaSeeking::*method)(LONGLONG *), LONGLONG *value)
{
    IMediaSeeking *seeking;
    HRESULT hr = GetPeerSeeking(&seeking);
    if (SUCCEEDED(hr)) {
        hr = (seeking->*method)(value);
        seeking->Release();
    }
    return hr;
}

// The IMediaSeeking calls go to the peer (E_NOTIMPL without one).
#define LS3D_PASS_SEEKING(call)                                                                    \
    IMediaSeeking *seeking;                                                                        \
    HRESULT hr = GetPeerSeeking(&seeking);                                                         \
    if (FAILED(hr))                                                                                \
        return hr;                                                                                 \
    hr = seeking->call;                                                                            \
    seeking->Release();                                                                            \
    return hr

#define LS3D_PASS_POSITION(call)                                                                   \
    IMediaPosition *position;                                                                      \
    HRESULT hr = GetPeer(&position);                                                               \
    if (FAILED(hr))                                                                                \
        return hr;                                                                                 \
    hr = position->call;                                                                           \
    position->Release();                                                                           \
    return hr

// 0x1008c080 - 0x1008c7c0
HRESULT __stdcall CPosPassThru::GetCapabilities(DWORD *caps) { LS3D_PASS_SEEKING(GetCapabilities(caps)); }
HRESULT __stdcall CPosPassThru::CheckCapabilities(DWORD *caps) { LS3D_PASS_SEEKING(CheckCapabilities(caps)); }
HRESULT __stdcall CPosPassThru::IsFormatSupported(const GUID *format) { LS3D_PASS_SEEKING(IsFormatSupported(format)); }
HRESULT __stdcall CPosPassThru::QueryPreferredFormat(GUID *format) { LS3D_PASS_SEEKING(QueryPreferredFormat(format)); }
HRESULT __stdcall CPosPassThru::SetTimeFormat(const GUID *format) { LS3D_PASS_SEEKING(SetTimeFormat(format)); }
HRESULT __stdcall CPosPassThru::GetTimeFormat(GUID *format) { LS3D_PASS_SEEKING(GetTimeFormat(format)); }
HRESULT __stdcall CPosPassThru::IsUsingTimeFormat(const GUID *format) { LS3D_PASS_SEEKING(IsUsingTimeFormat(format)); }

HRESULT __stdcall CPosPassThru::ConvertTimeFormat(LONGLONG *target, const GUID *targetFormat, LONGLONG source,
                                                  const GUID *sourceFormat)
{
    LS3D_PASS_SEEKING(ConvertTimeFormat(target, targetFormat, source, sourceFormat));
}

HRESULT __stdcall CPosPassThru::SetPositions(LONGLONG *current, DWORD currentFlags, LONGLONG *stop, DWORD stopFlags)
{
    LS3D_PASS_SEEKING(SetPositions(current, currentFlags, stop, stopFlags));
}

HRESULT __stdcall CPosPassThru::GetPositions(LONGLONG *current, LONGLONG *stop)
{
    LS3D_PASS_SEEKING(GetPositions(current, stop));
}

HRESULT __stdcall CPosPassThru::GetAvailable(LONGLONG *earliest, LONGLONG *latest)
{
    LS3D_PASS_SEEKING(GetAvailable(earliest, latest));
}

HRESULT __stdcall CPosPassThru::GetRate(double *rate) { LS3D_PASS_SEEKING(GetRate(rate)); }

HRESULT __stdcall CPosPassThru::SetRate(double rate)
{
    if (rate == 0.0)
        return E_INVALIDARG;
    LS3D_PASS_SEEKING(SetRate(rate));
}

HRESULT __stdcall CPosPassThru::GetDuration(LONGLONG *duration)
{
    return GetSeekingLongLong(&IMediaSeeking::GetDuration, duration);
}

HRESULT __stdcall CPosPassThru::GetStopPosition(LONGLONG *stop)
{
    return GetSeekingLongLong(&IMediaSeeking::GetStopPosition, stop);
}

HRESULT __stdcall CPosPassThru::GetPreroll(LONGLONG *preroll)
{
    return GetSeekingLongLong(&IMediaSeeking::GetPreroll, preroll);
}

// 0x1008c500: the renderer's own idea of the position first.
HRESULT __stdcall CPosPassThru::GetCurrentPosition(LONGLONG *current)
{
    if (SUCCEEDED(GetMediaTime(current, nullptr)))
        return NOERROR;
    return GetSeekingLongLong(&IMediaSeeking::GetCurrentPosition, current);
}

// 0x1008c850 - 0x1008ccd0
HRESULT __stdcall CPosPassThru::get_Duration(REFTIME *length) { LS3D_PASS_POSITION(get_Duration(length)); }
HRESULT __stdcall CPosPassThru::get_CurrentPosition(REFTIME *time) { LS3D_PASS_POSITION(get_CurrentPosition(time)); }
HRESULT __stdcall CPosPassThru::put_CurrentPosition(REFTIME time) { LS3D_PASS_POSITION(put_CurrentPosition(time)); }
HRESULT __stdcall CPosPassThru::get_StopTime(REFTIME *time) { LS3D_PASS_POSITION(get_StopTime(time)); }
HRESULT __stdcall CPosPassThru::put_StopTime(REFTIME time) { LS3D_PASS_POSITION(put_StopTime(time)); }
HRESULT __stdcall CPosPassThru::get_PrerollTime(REFTIME *time) { LS3D_PASS_POSITION(get_PrerollTime(time)); }
HRESULT __stdcall CPosPassThru::put_PrerollTime(REFTIME time) { LS3D_PASS_POSITION(put_PrerollTime(time)); }
HRESULT __stdcall CPosPassThru::get_Rate(double *rate) { LS3D_PASS_POSITION(get_Rate(rate)); }
HRESULT __stdcall CPosPassThru::CanSeekForward(LONG *can) { LS3D_PASS_POSITION(CanSeekForward(can)); }
HRESULT __stdcall CPosPassThru::CanSeekBackward(LONG *can) { LS3D_PASS_POSITION(CanSeekBackward(can)); }

HRESULT __stdcall CPosPassThru::put_Rate(double rate)
{
    if (rate == 0.0)
        return E_INVALIDARG;
    LS3D_PASS_POSITION(put_Rate(rate));
}

#undef LS3D_PASS_SEEKING
#undef LS3D_PASS_POSITION

// -- CRendererPosPassThru -----------------------------------------------------------------------

// 0x1008cd50
CRendererPosPassThru::CRendererPosPassThru(const TCHAR *name, IUnknown *outer, HRESULT *phr, IPin *pin)
    : CPosPassThru(name, outer, phr, pin), m_StartMedia(0), m_EndMedia(0), m_bReset(TRUE)
{
}

// 0x1008ceb0: the sample's stream times (GetTime, despite the name).
HRESULT CRendererPosPassThru::RegisterMediaTime(IMediaSample *sample)
{
    CAutoLock lock(&m_PositionLock);
    LONGLONG start, end;
    const HRESULT hr = sample->GetTime(&start, &end);
    if (FAILED(hr))
        return hr;
    m_StartMedia = start;
    m_EndMedia = end;
    m_bReset = FALSE;
    return NOERROR;
}

// 0x1008cf60
HRESULT CRendererPosPassThru::GetMediaTime(LONGLONG *start, LONGLONG *end)
{
    CAutoLock lock(&m_PositionLock);
    if (m_bReset == TRUE)
        return E_FAIL;
    HRESULT hr = ConvertTimeFormat(start, nullptr, m_StartMedia, &TIME_FORMAT_MEDIA_TIME);
    if (end && SUCCEEDED(hr))
        hr = ConvertTimeFormat(end, nullptr, m_EndMedia, &TIME_FORMAT_MEDIA_TIME);
    return hr;
}

// 0x1008d020
HRESULT CRendererPosPassThru::ResetMediaTime()
{
    CAutoLock lock(&m_PositionLock);
    m_StartMedia = 0;
    m_EndMedia = 0;
    m_bReset = TRUE;
    return NOERROR;
}

// 0x1008d050: at the end of the stream the position is the stop position.
HRESULT CRendererPosPassThru::EOS()
{
    if (m_bReset == TRUE)
        return E_FAIL;
    LONGLONG stop;
    const HRESULT hr = GetStopPosition(&stop);
    if (SUCCEEDED(hr)) {
        CAutoLock lock(&m_PositionLock);
        m_StartMedia = m_EndMedia = stop;
    }
    return hr;
}

}  // namespace ls3d::dshow
