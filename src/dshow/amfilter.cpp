// DirectShow base classes: CBaseFilter, CEnumPins, CEnumMediaTypes, CBasePin, CBaseInputPin
// (amfilter, 0x10088f50 - 0x1008b280).
#include "dshow/baseclasses.h"

#include <cstring>
#include <new>

namespace ls3d::dshow {

// -- CBaseFilter --------------------------------------------------------------------------------

// 0x10089030
CBaseFilter::CBaseFilter(const TCHAR *name, IUnknown *outer, CCritSec *lock, REFCLSID clsid)
    : CUnknown(name, outer), m_State(State_Stopped), m_pClock(nullptr), m_clsid(clsid), m_pLock(lock),
      m_pName(nullptr), m_pGraph(nullptr), m_pSink(nullptr), m_PinVersion(1)
{
}

// 0x10089090
CBaseFilter::~CBaseFilter()
{
    delete[] m_pName;
    // (graph and sink references are not held)
    if (m_pClock) {
        m_pClock->Release();
        m_pClock = nullptr;
    }
}

// 0x10088f50
HRESULT __stdcall CBaseFilter::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IBaseFilter)
        return GetInterface(static_cast<IBaseFilter *>(this), ppv);
    if (riid == IID_IMediaFilter)
        return GetInterface(static_cast<IMediaFilter *>(this), ppv);
    if (riid == IID_IPersist)
        return GetInterface(static_cast<IPersist *>(this), ppv);
    if (riid == IID_IAMovieSetup)
        return GetInterface(static_cast<IAMovieSetup *>(this), ppv);
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

// 0x10089100
HRESULT __stdcall CBaseFilter::GetClassID(CLSID *clsid)
{
    if (!clsid)
        return E_POINTER;
    *clsid = m_clsid;
    return NOERROR;
}

// 0x10089260: inactivates the connected pins (all of them, keeping the first error).
HRESULT __stdcall CBaseFilter::Stop()
{
    CAutoLock lock(m_pLock);
    HRESULT hr = NOERROR;
    if (m_State != State_Stopped) {
        const int count = GetPinCount();
        for (int i = 0; i < count; ++i) {
            CBasePin *pin = GetPin(i);
            if (pin->IsConnected()) {
                const HRESULT r = pin->Inactive();
                if (FAILED(r) && SUCCEEDED(hr))
                    hr = r;
            }
        }
    }
    m_State = State_Stopped;
    return hr;
}

// 0x10089310
HRESULT __stdcall CBaseFilter::Pause()
{
    CAutoLock lock(m_pLock);
    if (m_State == State_Stopped) {
        const int count = GetPinCount();
        for (int i = 0; i < count; ++i) {
            CBasePin *pin = GetPin(i);
            if (pin->IsConnected()) {
                const HRESULT hr = pin->Active();
                if (FAILED(hr))
                    return hr;
            }
        }
    }
    m_State = State_Paused;
    return S_OK;
}

// 0x100893c0
HRESULT __stdcall CBaseFilter::Run(REFERENCE_TIME start)
{
    CAutoLock lock(m_pLock);
    m_tStart.m_time = start;
    if (m_State == State_Stopped) {
        const HRESULT hr = Pause();
        if (FAILED(hr))
            return hr;
    }
    if (m_State != State_Running) {
        const int count = GetPinCount();
        for (int i = 0; i < count; ++i) {
            CBasePin *pin = GetPin(i);
            if (pin->IsConnected()) {
                const HRESULT hr = pin->Run(start);
                if (FAILED(hr))
                    return hr;
            }
        }
    }
    m_State = State_Running;
    return S_OK;
}

// 0x100894b0: reference time minus the run start.
HRESULT CBaseFilter::StreamTime(CRefTime &rtStream)
{
    if (!m_pClock)
        return VFW_E_NO_CLOCK;
    const HRESULT hr = m_pClock->GetTime(&rtStream.m_time);
    if (FAILED(hr))
        return hr;
    rtStream.m_time -= m_tStart.m_time;
    return S_OK;
}

// 0x10089140
HRESULT __stdcall CBaseFilter::SetSyncSource(IReferenceClock *clock)
{
    CAutoLock lock(m_pLock);
    if (clock)
        clock->AddRef();
    if (m_pClock)
        m_pClock->Release();
    m_pClock = clock;
    return NOERROR;
}

// 0x100891b0
HRESULT __stdcall CBaseFilter::GetSyncSource(IReferenceClock **clock)
{
    if (!clock)
        return E_POINTER;
    CAutoLock lock(m_pLock);
    if (m_pClock)
        m_pClock->AddRef();
    *clock = m_pClock;
    return NOERROR;
}

// 0x100894f0
HRESULT __stdcall CBaseFilter::EnumPins(IEnumPins **pins)
{
    if (!pins)
        return E_POINTER;
    *pins = new (std::nothrow) CEnumPins(this, nullptr);
    return *pins ? NOERROR : E_OUTOFMEMORY;
}

// 0x10089580
HRESULT __stdcall CBaseFilter::QueryFilterInfo(FILTER_INFO *info)
{
    if (!info)
        return E_POINTER;
    if (m_pName)
        lstrcpynWInternal(info->achName, m_pName, sizeof(info->achName) / sizeof(WCHAR));
    else
        info->achName[0] = L'\0';
    info->pGraph = m_pGraph;
    if (m_pGraph)
        m_pGraph->AddRef();
    return NOERROR;
}

// 0x100895d0: the graph and its event sink are kept without references.
HRESULT __stdcall CBaseFilter::JoinFilterGraph(IFilterGraph *graph, LPCWSTR name)
{
    CAutoLock lock(m_pLock);
    m_pGraph = graph;
    if (m_pGraph) {
        if (SUCCEEDED(m_pGraph->QueryInterface(IID_IMediaEventSink, reinterpret_cast<void **>(&m_pSink))))
            m_pSink->Release();
    } else {
        m_pSink = nullptr;
    }
    if (m_pName) {
        delete[] m_pName;
        m_pName = nullptr;
    }
    if (name) {
        const DWORD length = lstrlenWInternal(name) + 1;
        m_pName = new (std::nothrow) WCHAR[length];
        if (m_pName)
            memcpy(m_pName, name, length * sizeof(WCHAR));
    }
    return NOERROR;
}

// 0x100896b0
HRESULT __stdcall CBaseFilter::QueryVendorInfo(LPWSTR *)
{
    return E_NOTIMPL;
}

// 0x100896c0: EC_COMPLETE carries the filter.
HRESULT CBaseFilter::NotifyEvent(long code, LONG_PTR param1, LONG_PTR param2)
{
    IMediaEventSink *sink = m_pSink;
    if (!sink)
        return E_NOTIMPL;
    if (code == EC_COMPLETE)
        param2 = reinterpret_cast<LONG_PTR>(static_cast<IBaseFilter *>(this));
    return sink->Notify(code, param1, param2);
}

// 0x10089710
LONG CBaseFilter::GetPinVersion()
{
    return m_PinVersion;
}

// 0x10089720
HRESULT __stdcall CBaseFilter::Register()
{
    const AMOVIESETUP_FILTER *setup = GetSetupData();
    if (!setup)
        return S_FALSE;
    CoInitialize(nullptr);
    IFilterMapper *mapper;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper,
                                   reinterpret_cast<void **>(&mapper)))) {
        AMovieSetupRegisterFilter(setup, mapper, TRUE);
        mapper->Release();
    }
    CoFreeUnusedLibraries();
    CoUninitialize();
    return NOERROR;
}

// 0x10089790
HRESULT __stdcall CBaseFilter::Unregister()
{
    const AMOVIESETUP_FILTER *setup = GetSetupData();
    if (!setup)
        return S_FALSE;
    CoInitialize(nullptr);
    IFilterMapper *mapper;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper,
                                  reinterpret_cast<void **>(&mapper));
    if (SUCCEEDED(hr)) {
        hr = AMovieSetupRegisterFilter(setup, mapper, FALSE);
        mapper->Release();
    }
    CoFreeUnusedLibraries();
    CoUninitialize();
    // (the filter not being registered is fine)
    return hr == static_cast<HRESULT>(0x80070002) ? NOERROR : hr;
}

// 0x1008b0b0
HRESULT AMovieSetupRegisterFilter(const AMOVIESETUP_FILTER *setup, IFilterMapper *mapper, BOOL reg)
{
    if (!setup)
        return S_FALSE;
    HRESULT hr = mapper->UnregisterFilter(*setup->clsID);
    if (reg) {
        hr = mapper->RegisterFilter(*setup->clsID, setup->strName, setup->dwMerit);
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < setup->nPins; ++i) {
                const AMOVIESETUP_PIN &pin = setup->lpPin[i];
                hr = mapper->RegisterPin(*setup->clsID, pin.strName, pin.bRendered, pin.bOutput, pin.bZero,
                                         pin.bMany, *pin.clsConnectsToFilter, pin.strConnectsToPin);
                if (FAILED(hr))
                    break;
                for (UINT j = 0; j < pin.nMediaTypes; ++j) {
                    hr = mapper->RegisterPinType(*setup->clsID, pin.strName, *pin.lpMediaType[j].clsMajorType,
                                                 *pin.lpMediaType[j].clsMinorType);
                    if (FAILED(hr))
                        break;
                }
                if (FAILED(hr))
                    break;
            }
        }
    }
    return hr == static_cast<HRESULT>(0x80070002) ? NOERROR : hr;
}

// -- CEnumPins ----------------------------------------------------------------------------------

// 0x10089810
CEnumPins::CEnumPins(CBaseFilter *filter, CEnumPins *copy)
    : m_Position(0), m_PinCount(0), m_pFilter(filter), m_Version(0), m_cRef(1)
{
    static_cast<IBaseFilter *>(m_pFilter)->AddRef();
    if (!copy) {
        m_Version = m_pFilter->GetPinVersion();
        m_PinCount = m_pFilter->GetPinCount();
    } else {
        m_Position = copy->m_Position;
        m_PinCount = copy->m_PinCount;
        m_Version = copy->m_Version;
        m_PinCache.AddTail(copy->m_PinCache);
    }
}

// 0x10089900
CEnumPins::~CEnumPins()
{
    static_cast<IBaseFilter *>(m_pFilter)->Release();
}

// 0x10089960
HRESULT __stdcall CEnumPins::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IEnumPins || riid == IID_IUnknown)
        return GetInterface(static_cast<IEnumPins *>(this), ppv);
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// 0x100899c0
ULONG __stdcall CEnumPins::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

// 0x100899e0
ULONG __stdcall CEnumPins::Release()
{
    const ULONG count = InterlockedDecrement(&m_cRef);
    if (count == 0)
        delete this;
    return count;
}

// 0x10089ab0: pins not returned before (the cache survives a refresh after the pins changed).
HRESULT __stdcall CEnumPins::Next(ULONG count, IPin **pins, ULONG *fetched)
{
    if (!pins)
        return E_POINTER;
    if (fetched)
        *fetched = 0;
    else if (count > 1)
        return E_INVALIDARG;
    ULONG n = 0;
    if (AreWeOutOfSync()) {
        // Refresh
        m_Version = m_pFilter->GetPinVersion();
        m_PinCount = m_pFilter->GetPinCount();
        m_Position = 0;
    }
    int left = m_PinCount - m_Position;
    if (left > static_cast<int>(count))
        left = static_cast<int>(count);
    if (left == 0)
        return S_FALSE;
    while (left && m_PinCount - m_Position) {
        CBasePin *pin = m_pFilter->GetPin(m_Position++);
        if (!pin)
            return VFW_E_ENUM_OUT_OF_SYNC;
        if (!m_PinCache.Find(pin)) {
            *pins = pin;
            pin->AddRef();
            ++n;
            ++pins;
            m_PinCache.AddTail(pin);
            --left;
        }
    }
    if (fetched)
        *fetched = n;
    return count == n ? NOERROR : S_FALSE;
}

// 0x10089bd0
HRESULT __stdcall CEnumPins::Skip(ULONG count)
{
    if (AreWeOutOfSync())
        return VFW_E_ENUM_OUT_OF_SYNC;
    const ULONG left = static_cast<ULONG>(m_PinCount - m_Position);
    if (count > left)
        return S_FALSE;
    m_Position += count;
    return NOERROR;
}

// 0x10089c10
HRESULT __stdcall CEnumPins::Reset()
{
    m_Version = m_pFilter->GetPinVersion();
    m_PinCount = m_pFilter->GetPinCount();
    m_Position = 0;
    m_PinCache.RemoveAll();
    return S_OK;
}

// 0x10089a10
HRESULT __stdcall CEnumPins::Clone(IEnumPins **out)
{
    if (!out)
        return E_POINTER;
    if (AreWeOutOfSync()) {
        *out = nullptr;
        return VFW_E_ENUM_OUT_OF_SYNC;
    }
    *out = new (std::nothrow) CEnumPins(m_pFilter, this);
    return *out ? NOERROR : E_OUTOFMEMORY;
}

// -- CEnumMediaTypes ----------------------------------------------------------------------------

// (inlined into CBasePin::EnumMediaTypes and Clone)
CEnumMediaTypes::CEnumMediaTypes(CBasePin *pin, CEnumMediaTypes *copy)
    : m_Position(0), m_pPin(pin), m_Version(0), m_cRef(1)
{
    static_cast<IPin *>(m_pPin)->AddRef();
    if (!copy) {
        m_Version = m_pPin->GetMediaTypeVersion();
        return;
    }
    m_Position = copy->m_Position;
    m_Version = copy->m_Version;
}

// 0x10089c40
CEnumMediaTypes::~CEnumMediaTypes()
{
    static_cast<IPin *>(m_pPin)->Release();
}

BOOL CEnumMediaTypes::AreWeOutOfSync()
{
    return m_pPin->GetMediaTypeVersion() != m_Version;
}

// 0x10089c70
HRESULT __stdcall CEnumMediaTypes::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IEnumMediaTypes || riid == IID_IUnknown)
        return GetInterface(static_cast<IEnumMediaTypes *>(this), ppv);
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// 0x10089cd0
ULONG __stdcall CEnumMediaTypes::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

// 0x10089cf0
ULONG __stdcall CEnumMediaTypes::Release()
{
    const ULONG count = InterlockedDecrement(&m_cRef);
    if (count == 0)
        delete this;
    return count;
}

// 0x10089de0: each type in its own task memory block (the format block is handed over).
HRESULT __stdcall CEnumMediaTypes::Next(ULONG count, AM_MEDIA_TYPE **types, ULONG *fetched)
{
    if (!types)
        return E_POINTER;
    if (AreWeOutOfSync())
        return VFW_E_ENUM_OUT_OF_SYNC;
    if (fetched)
        *fetched = 0;
    else if (count > 1)
        return E_INVALIDARG;
    ULONG n = 0;
    while (count) {
        CMediaType mt;
        if (m_pPin->GetMediaType(m_Position++, &mt) != S_OK)
            break;
        *types = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!*types)
            break;
        **types = mt;
        mt.pbFormat = nullptr;
        mt.cbFormat = 0;
        mt.pUnk = nullptr;
        ++types;
        ++n;
        --count;
    }
    if (fetched)
        *fetched = n;
    return count == 0 ? NOERROR : S_FALSE;
}

// 0x10089f60
HRESULT __stdcall CEnumMediaTypes::Skip(ULONG count)
{
    if (count == 0)
        return S_OK;
    if (AreWeOutOfSync())
        return VFW_E_ENUM_OUT_OF_SYNC;
    m_Position += count;
    CMediaType mt;
    return m_pPin->GetMediaType(m_Position - 1, &mt) == S_OK ? S_OK : S_FALSE;
}

// 0x1008a020
HRESULT __stdcall CEnumMediaTypes::Reset()
{
    m_Position = 0;
    m_Version = m_pPin->GetMediaTypeVersion();
    return NOERROR;
}

// 0x10089d20
HRESULT __stdcall CEnumMediaTypes::Clone(IEnumMediaTypes **out)
{
    if (!out)
        return E_POINTER;
    if (AreWeOutOfSync()) {
        *out = nullptr;
        return VFW_E_ENUM_OUT_OF_SYNC;
    }
    *out = new (std::nothrow) CEnumMediaTypes(m_pPin, this);
    return *out ? NOERROR : E_OUTOFMEMORY;
}

// -- CBasePin -----------------------------------------------------------------------------------

// 0x1008a040: the pin is not aggregated; its reference count is the filter's.
CBasePin::CBasePin(const TCHAR *objectName, CBaseFilter *filter, CCritSec *lock, HRESULT *, LPCWSTR name,
                   PIN_DIRECTION dir)
    : CUnknown(objectName, nullptr), m_pName(nullptr), m_Connected(nullptr), m_dir(dir), m_pLock(lock),
      m_bRunTimeError(false), m_bCanReconnectWhenActive(false), m_bTryMyTypesFirst(false), m_pFilter(filter),
      m_pQSink(nullptr), m_TypeVersion(1), m_dRate(1.0)
{
    m_tStop.m_time = MAX_TIME;
    if (name) {
        const DWORD length = lstrlenWInternal(name) + 1;
        m_pName = new (std::nothrow) WCHAR[length];
        if (m_pName)
            memcpy(m_pName, name, length * sizeof(WCHAR));
    }
}

// 0x1008a130
CBasePin::~CBasePin()
{
    delete[] m_pName;
}

HRESULT __stdcall CBasePin::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IPin)
        return GetInterface(static_cast<IPin *>(this), ppv);
    if (riid == IID_IQualityControl)
        return GetInterface(static_cast<IQualityControl *>(this), ppv);
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

// 0x1008a190
ULONG __stdcall CBasePin::NonDelegatingAddRef()
{
    return static_cast<IBaseFilter *>(m_pFilter)->AddRef();
}

// 0x1008a1b0
ULONG __stdcall CBasePin::NonDelegatingRelease()
{
    return static_cast<IBaseFilter *>(m_pFilter)->Release();
}

// 0x1008a1d0
HRESULT __stdcall CBasePin::Connect(IPin *receivePin, const AM_MEDIA_TYPE *pmt)
{
    if (!receivePin)
        return E_POINTER;
    CAutoLock lock(m_pLock);
    if (m_Connected)
        return VFW_E_ALREADY_CONNECTED;
    if (!IsStopped() && !m_bCanReconnectWhenActive)
        return VFW_E_NOT_STOPPED;
    const HRESULT hr = AgreeMediaType(receivePin, static_cast<const CMediaType *>(pmt));
    if (FAILED(hr)) {
        BreakConnect();
        return hr;
    }
    return NOERROR;
}

// 0x1008a2e0
HRESULT CBasePin::AttemptConnection(IPin *receivePin, const CMediaType *pmt)
{
    HRESULT hr = CheckConnect(receivePin);
    if (FAILED(hr)) {
        BreakConnect();
        return hr;
    }
    hr = CheckMediaType(pmt);
    if (hr == NOERROR) {
        // Look connected, so that ReceiveConnection can complete.
        m_Connected = receivePin;
        m_Connected->AddRef();
        hr = SetMediaType(pmt);
        if (SUCCEEDED(hr)) {
            hr = receivePin->ReceiveConnection(static_cast<IPin *>(this), pmt);
            if (SUCCEEDED(hr)) {
                hr = CompleteConnect(receivePin);
                if (SUCCEEDED(hr))
                    return hr;
                receivePin->Disconnect();
            }
        }
    } else if (SUCCEEDED(hr) || hr == E_FAIL || hr == E_INVALIDARG) {
        hr = VFW_E_TYPE_NOT_ACCEPTED;
    }
    BreakConnect();
    if (m_Connected) {
        m_Connected->Release();
        m_Connected = nullptr;
    }
    return hr;
}

// 0x1008a390
HRESULT CBasePin::TryMediaTypes(IPin *receivePin, const CMediaType *pmt, IEnumMediaTypes *types)
{
    HRESULT hr = types->Reset();
    if (FAILED(hr))
        return hr;
    CMediaType *type = nullptr;
    ULONG count = 0;
    HRESULT failure = S_OK;
    for (;;) {
        hr = types->Next(1, reinterpret_cast<AM_MEDIA_TYPE **>(&type), &count);
        if (hr != S_OK) {
            if (failure == S_OK)
                failure = VFW_E_NO_ACCEPTABLE_TYPES;
            return failure;
        }
        if (!pmt || type->MatchesPartial(pmt)) {
            hr = AttemptConnection(receivePin, type);
            if (FAILED(hr) && SUCCEEDED(failure) && hr != E_FAIL && hr != E_INVALIDARG &&
                hr != VFW_E_TYPE_NOT_ACCEPTED)
                failure = hr;
        } else {
            hr = VFW_E_NO_ACCEPTABLE_TYPES;
        }
        DeleteMediaType(type);
        if (hr == S_OK)
            return hr;
    }
}

// 0x1008a470: the given type when fully specified, else the other pin's types, then ours.
HRESULT CBasePin::AgreeMediaType(IPin *receivePin, const CMediaType *pmt)
{
    if (pmt && !pmt->IsPartiallySpecified())
        return AttemptConnection(receivePin, pmt);
    HRESULT failure = VFW_E_NO_ACCEPTABLE_TYPES;
    for (int i = 0; i < 2; ++i) {
        IEnumMediaTypes *types = nullptr;
        HRESULT hr;
        if (i == static_cast<int>(m_bTryMyTypesFirst))
            hr = receivePin->EnumMediaTypes(&types);
        else
            hr = EnumMediaTypes(&types);
        if (SUCCEEDED(hr)) {
            hr = TryMediaTypes(receivePin, pmt, types);
            types->Release();
            if (SUCCEEDED(hr))
                return NOERROR;
            if (hr != E_FAIL && hr != E_INVALIDARG && hr != VFW_E_TYPE_NOT_ACCEPTED)
                failure = hr;
        }
    }
    return failure;
}

// 0x1008a550
HRESULT CBasePin::SetMediaType(const CMediaType *pmt)
{
    const HRESULT hr = m_mt.Set(*pmt);
    return FAILED(hr) ? hr : NOERROR;
}

// 0x1008a570
HRESULT CBasePin::CheckConnect(IPin *pin)
{
    PIN_DIRECTION dir;
    pin->QueryDirection(&dir);
    return dir == m_dir ? VFW_E_INVALID_DIRECTION : NOERROR;
}

// 0x1008a5a0
HRESULT __stdcall CBasePin::ReceiveConnection(IPin *connector, const AM_MEDIA_TYPE *pmt)
{
    if (!connector || !pmt)
        return E_POINTER;
    CAutoLock lock(m_pLock);
    if (m_Connected)
        return VFW_E_ALREADY_CONNECTED;
    if (!IsStopped() && !m_bCanReconnectWhenActive)
        return VFW_E_NOT_STOPPED;
    HRESULT hr = CheckConnect(connector);
    if (FAILED(hr)) {
        BreakConnect();
        return hr;
    }
    const CMediaType *type = static_cast<const CMediaType *>(pmt);
    hr = CheckMediaType(type);
    if (hr != NOERROR) {
        BreakConnect();
        if (SUCCEEDED(hr) || hr == E_FAIL || hr == E_INVALIDARG)
            hr = VFW_E_TYPE_NOT_ACCEPTED;
        return hr;
    }
    m_Connected = connector;
    m_Connected->AddRef();
    hr = SetMediaType(type);
    if (SUCCEEDED(hr)) {
        hr = CompleteConnect(connector);
        if (SUCCEEDED(hr))
            return NOERROR;
    }
    m_Connected->Release();
    m_Connected = nullptr;
    BreakConnect();
    return hr;
}

// 0x1008a700
HRESULT __stdcall CBasePin::Disconnect()
{
    CAutoLock lock(m_pLock);
    if (!IsStopped())
        return VFW_E_NOT_STOPPED;
    if (!m_Connected)
        return S_FALSE;
    const HRESULT hr = BreakConnect();
    if (FAILED(hr))
        return hr;
    m_Connected->Release();
    m_Connected = nullptr;
    return S_OK;
}

// 0x1008a7b0
HRESULT __stdcall CBasePin::ConnectedTo(IPin **pin)
{
    if (!pin)
        return E_POINTER;
    IPin *connected = m_Connected;
    *pin = connected;
    if (!connected)
        return VFW_E_NOT_CONNECTED;
    connected->AddRef();
    return S_OK;
}

// 0x1008a7e0
HRESULT __stdcall CBasePin::ConnectionMediaType(AM_MEDIA_TYPE *pmt)
{
    if (!pmt)
        return E_POINTER;
    CAutoLock lock(m_pLock);
    if (IsConnected()) {
        CopyMediaType(pmt, &m_mt);
        return S_OK;
    }
    static_cast<CMediaType *>(pmt)->InitMediaType();
    return VFW_E_NOT_CONNECTED;
}

// 0x1008a890
HRESULT __stdcall CBasePin::QueryPinInfo(PIN_INFO *info)
{
    if (!info)
        return E_POINTER;
    info->pFilter = m_pFilter;
    if (m_pFilter)
        static_cast<IBaseFilter *>(m_pFilter)->AddRef();
    if (m_pName)
        lstrcpynWInternal(info->achName, m_pName, sizeof(info->achName) / sizeof(WCHAR));
    else
        info->achName[0] = L'\0';
    info->dir = m_dir;
    return NOERROR;
}

// 0x1008a900
HRESULT __stdcall CBasePin::QueryDirection(PIN_DIRECTION *dir)
{
    if (!dir)
        return E_POINTER;
    *dir = m_dir;
    return NOERROR;
}

// 0x1008a920: S_OK or S_FALSE.
HRESULT __stdcall CBasePin::QueryAccept(const AM_MEDIA_TYPE *pmt)
{
    if (!pmt)
        return E_POINTER;
    const HRESULT hr = CheckMediaType(static_cast<const CMediaType *>(pmt));
    return FAILED(hr) ? S_FALSE : hr;
}

// 0x1008a950
HRESULT __stdcall CBasePin::EnumMediaTypes(IEnumMediaTypes **types)
{
    if (!types)
        return E_POINTER;
    *types = new (std::nothrow) CEnumMediaTypes(this, nullptr);
    return *types ? NOERROR : E_OUTOFMEMORY;
}

// 0x10087e70
HRESULT __stdcall CBasePin::QueryInternalConnections(IPin **, ULONG *)
{
    return E_NOTIMPL;
}

// 0x1008aa50
HRESULT __stdcall CBasePin::NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate)
{
    m_tStart.m_time = start;
    m_tStop.m_time = stop;
    m_dRate = rate;
    return S_OK;
}

// 0x1008aa20
HRESULT __stdcall CBasePin::SetSink(IQualityControl *sink)
{
    CAutoLock lock(m_pLock);
    m_pQSink = sink;
    return NOERROR;
}

// 0x1008aa10
LONG CBasePin::GetMediaTypeVersion()
{
    return m_TypeVersion;
}

// 0x1008aa00
HRESULT CBasePin::GetMediaType(int, CMediaType *)
{
    return E_UNEXPECTED;
}

// -- CBaseInputPin ------------------------------------------------------------------------------

// 0x1008aa90
CBaseInputPin::CBaseInputPin(const TCHAR *objectName, CBaseFilter *filter, CCritSec *lock, HRESULT *phr,
                             LPCWSTR name)
    : CBasePin(objectName, filter, lock, phr, name, PINDIR_INPUT), m_pAllocator(nullptr), m_bReadOnly(FALSE),
      m_bFlushing(FALSE)
{
    memset(&m_SampleProps, 0, sizeof(m_SampleProps));
}

// 0x1008aaf0
CBaseInputPin::~CBaseInputPin()
{
    if (m_pAllocator) {
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
}

// 0x1008ab70
HRESULT __stdcall CBaseInputPin::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IMemInputPin)
        return GetInterface(static_cast<IMemInputPin *>(this), ppv);
    return CBasePin::NonDelegatingQueryInterface(riid, ppv);
}

// 0x1008ac20: a standard memory allocator when the upstream pin has none to offer.
HRESULT __stdcall CBaseInputPin::GetAllocator(IMemAllocator **allocator)
{
    if (!allocator)
        return E_POINTER;
    CAutoLock lock(m_pLock);
    if (!m_pAllocator) {
        const HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_IMemAllocator, reinterpret_cast<void **>(&m_pAllocator));
        if (FAILED(hr))
            return hr;
    }
    *allocator = m_pAllocator;
    m_pAllocator->AddRef();
    return NOERROR;
}

// 0x1008acc0
HRESULT __stdcall CBaseInputPin::NotifyAllocator(IMemAllocator *allocator, BOOL readOnly)
{
    if (!allocator)
        return E_POINTER;
    CAutoLock lock(m_pLock);
    IMemAllocator *old = m_pAllocator;
    allocator->AddRef();
    m_pAllocator = allocator;
    if (old)
        old->Release();
    m_bReadOnly = static_cast<BYTE>(readOnly);
    return NOERROR;
}

// 0x100896b0
HRESULT __stdcall CBaseInputPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES *)
{
    return E_NOTIMPL;
}

// 0x1008ad40
HRESULT CBaseInputPin::BreakConnect()
{
    if (m_pAllocator) {
        const HRESULT hr = m_pAllocator->Decommit();
        if (FAILED(hr))
            return hr;
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    return S_OK;
}

// 0x1008ad80: records the sample's properties; a type the pin does not accept is a run-time error.
HRESULT __stdcall CBaseInputPin::Receive(IMediaSample *sample)
{
    if (!sample)
        return E_POINTER;
    HRESULT hr = CheckStreaming();
    if (hr != S_OK)
        return hr;
    IMediaSample2 *sample2;
    if (SUCCEEDED(sample->QueryInterface(IID_IMediaSample2, reinterpret_cast<void **>(&sample2)))) {
        hr = sample2->GetProperties(sizeof(m_SampleProps), reinterpret_cast<BYTE *>(&m_SampleProps));
        sample2->Release();
        if (FAILED(hr))
            return hr;
    } else {
        m_SampleProps.cbData = sizeof(m_SampleProps);
        m_SampleProps.dwTypeSpecificFlags = 0;
        m_SampleProps.dwStreamId = AM_STREAM_MEDIA;
        m_SampleProps.dwSampleFlags = 0;
        if (sample->IsDiscontinuity() == S_OK)
            m_SampleProps.dwSampleFlags |= AM_SAMPLE_DATADISCONTINUITY;
        if (sample->IsPreroll() == S_OK)
            m_SampleProps.dwSampleFlags |= AM_SAMPLE_PREROLL;
        if (sample->IsSyncPoint() == S_OK)
            m_SampleProps.dwSampleFlags |= AM_SAMPLE_SPLICEPOINT;
        if (SUCCEEDED(sample->GetTime(&m_SampleProps.tStart, &m_SampleProps.tStop)))
            m_SampleProps.dwSampleFlags |= AM_SAMPLE_TIMEVALID | AM_SAMPLE_STOPVALID;
        if (sample->GetMediaType(&m_SampleProps.pMediaType) == S_OK)
            m_SampleProps.dwSampleFlags |= AM_SAMPLE_TYPECHANGED;
        sample->GetPointer(&m_SampleProps.pbBuffer);
        m_SampleProps.lActual = sample->GetActualDataLength();
        m_SampleProps.cbBuffer = sample->GetSize();
    }
    if (!(m_SampleProps.dwSampleFlags & AM_SAMPLE_TYPECHANGED))
        return NOERROR;
    hr = CheckMediaType(static_cast<CMediaType *>(m_SampleProps.pMediaType));
    if (hr == NOERROR)
        return NOERROR;
    m_bRunTimeError = true;
    EndOfStream();
    m_pFilter->NotifyEvent(EC_ERRORABORT, VFW_E_TYPE_NOT_ACCEPTED, 0);
    return VFW_E_INVALIDMEDIATYPE;
}

// 0x1008aee0
HRESULT __stdcall CBaseInputPin::ReceiveMultiple(IMediaSample **samples, long count, long *processed)
{
    if (!samples)
        return E_POINTER;
    HRESULT hr = S_OK;
    *processed = 0;
    while (count-- > 0) {
        hr = Receive(samples[*processed]);
        if (hr != S_OK)
            break;
        ++*processed;
    }
    return hr;
}

// 0x1008af40: whether the output pins downstream can block (none: yes).
HRESULT __stdcall CBaseInputPin::ReceiveCanBlock()
{
    const int count = m_pFilter->GetPinCount();
    int outputs = 0;
    for (int i = 0; i < count; ++i) {
        CBasePin *pin = m_pFilter->GetPin(i);
        PIN_DIRECTION dir;
        HRESULT hr = pin->QueryDirection(&dir);
        if (FAILED(hr))
            return hr;
        if (dir == PINDIR_OUTPUT) {
            IPin *connected;
            if (SUCCEEDED(pin->ConnectedTo(&connected))) {
                ++outputs;
                IMemInputPin *input;
                hr = connected->QueryInterface(IID_IMemInputPin, reinterpret_cast<void **>(&input));
                connected->Release();
                if (FAILED(hr))
                    return S_OK;
                hr = input->ReceiveCanBlock();
                input->Release();
                if (hr != S_FALSE)
                    return S_OK;
            }
        }
    }
    return outputs == 0 ? S_OK : S_FALSE;
}

// 0x1008b000
HRESULT __stdcall CBaseInputPin::BeginFlush()
{
    CAutoLock lock(m_pLock);
    m_bFlushing = TRUE;
    return S_OK;
}

// 0x1008b030
HRESULT __stdcall CBaseInputPin::EndFlush()
{
    CAutoLock lock(m_pLock);
    m_bFlushing = FALSE;
    m_bRunTimeError = false;
    return S_OK;
}

// 0x1008b060
HRESULT __stdcall CBaseInputPin::Notify(IBaseFilter *self, Quality)
{
    return self ? NOERROR : E_POINTER;
}

// 0x1008b080
HRESULT CBaseInputPin::CheckStreaming()
{
    if (IsStopped())
        return VFW_E_WRONG_STATE;
    if (m_bFlushing)
        return S_FALSE;
    if (m_bRunTimeError)
        return VFW_E_RUNTIME_ERROR;
    return S_OK;
}

}  // namespace ls3d::dshow
