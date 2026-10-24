// The DirectShow base classes LS3DF links statically (strmbase.lib, 0x10086530 - 0x1008d41c; the
// DirectX 9 SDK's BaseClasses built without DEBUG / PERF): just what IShow's texture renderer
// (CBaseVideoRenderer) needs - COM helpers, media types, events, the filter / pin / input pin
// classes, the renderer and its position pass-through.
//
// Class layouts are the original's (offsets in the comments; checked by static_asserts), member
// names are the SDK's. Methods are ported from the binary, which in a few places differs from the
// later published SDK source (noted where it does).
#pragma once

#include <windows.h>

#include <dshow.h>

namespace ls3d::dshow {

const LONGLONG MAX_TIME = 0x7fffffffffffffffLL;
const LONGLONG UNITS = 10000000;                // REFERENCE_TIME units per second
const LONGLONG MILLISECONDS = 1000;

// -- combase ------------------------------------------------------------------------------------

// The objects alive (CBaseObject's count, 0x100d7340); the last one to go unloads OleAut32.
void BaseObjectCreated();
void BaseObjectDestroyed();                                         // 0x1008b280 (~CBaseObject)
HMODULE LoadOleAut32();                                             // 0x1008b2b0

HRESULT GetInterface(IUnknown *unknown, void **ppv);                // 0x1008b3b0

struct INonDelegatingUnknown {
    virtual HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) = 0;
    virtual ULONG __stdcall NonDelegatingAddRef() = 0;
    virtual ULONG __stdcall NonDelegatingRelease() = 0;
};

class CUnknown : public INonDelegatingUnknown {
public:
    CUnknown(const TCHAR *name, IUnknown *outer);                   // 0x1008b2d0
    virtual ~CUnknown() { BaseObjectDestroyed(); }

    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;  // 0x1008b300
    ULONG __stdcall NonDelegatingAddRef() override;                 // 0x1008b350
    ULONG __stdcall NonDelegatingRelease() override;                // 0x1008b370

    IUnknown *GetOwner() const { return m_pUnknown; }

protected:
    IUnknown *const m_pUnknown;                                     // +0x04 outer unknown (or this)
    volatile LONG m_cRef;                                           // +0x08
};
static_assert(sizeof(CUnknown) == 0x0c, "CUnknown layout");

// IUnknown of the interfaces of a CUnknown: through the owner.
#define LS3D_DECLARE_IUNKNOWN                                                                   \
    HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override                         \
    {                                                                                           \
        return GetOwner()->QueryInterface(riid, ppv);                                           \
    }                                                                                           \
    ULONG __stdcall AddRef() override { return GetOwner()->AddRef(); }                          \
    ULONG __stdcall Release() override { return GetOwner()->Release(); }

// -- wxutil -------------------------------------------------------------------------------------

class CCritSec {
public:
    CCritSec() { InitializeCriticalSection(&m_section); }
    ~CCritSec() { DeleteCriticalSection(&m_section); }              // 0x10086bb0
    CCritSec(const CCritSec &) = delete;
    CCritSec &operator=(const CCritSec &) = delete;
    void Lock() { EnterCriticalSection(&m_section); }
    void Unlock() { LeaveCriticalSection(&m_section); }

private:
    CRITICAL_SECTION m_section;
};
static_assert(sizeof(CCritSec) == 0x18, "CCritSec layout");

class CAutoLock {
public:
    explicit CAutoLock(CCritSec *lock) : m_pLock(lock) { m_pLock->Lock(); }
    ~CAutoLock() { m_pLock->Unlock(); }                             // 0x10086bc0
    CAutoLock(const CAutoLock &) = delete;
    CAutoLock &operator=(const CAutoLock &) = delete;

private:
    CCritSec *m_pLock;
};

class CAMEvent {
public:
    explicit CAMEvent(BOOL manualReset = FALSE);                    // 0x1008b6d0
    ~CAMEvent();                                                    // 0x1008b6f0
    CAMEvent(const CAMEvent &) = delete;
    CAMEvent &operator=(const CAMEvent &) = delete;
    operator HANDLE() const { return m_hEvent; }
    void Set() { SetEvent(m_hEvent); }
    void Reset() { ResetEvent(m_hEvent); }
    BOOL Wait(DWORD timeout = INFINITE) { return WaitForSingleObject(m_hEvent, timeout) == WAIT_OBJECT_0; }

private:
    HANDLE m_hEvent;
};

// Wide string helpers of wxutil (the original's own, 0x1008b700 - 0x1008b7b0).
WCHAR *lstrcpyWInternal(WCHAR *dst, const WCHAR *src);
WCHAR *lstrcpynWInternal(WCHAR *dst, const WCHAR *src, int count);
int lstrcmpWInternal(const WCHAR *a, const WCHAR *b);
int lstrlenWInternal(const WCHAR *s);

// a * b / c (+ d), rounded towards zero, saturated (0x1008b7d0).
LONGLONG llMulDiv(LONGLONG a, LONGLONG b, LONGLONG c, LONGLONG d);

// Waits on an event while dispatching the thread's sent messages (0x1008bb00).
DWORD WaitDispatchingMessages(HANDLE object, DWORD wait, HWND window = nullptr, UINT msg = 0,
                              HANDLE event = nullptr);

MMRESULT CompatibleTimeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user,
                                UINT flags);                        // 0x1008bd10

inline int TimeDiff(REFERENCE_TIME rt)
{
    if (rt < -(50 * UNITS))
        return static_cast<int>(-(50 * UNITS));
    if (rt > 50 * UNITS)
        return static_cast<int>(50 * UNITS);
    return static_cast<int>(rt);
}

// -- wxlist: the pin list of CEnumPins (CGenericList<CBasePin>) --------------------------------

class CBasePin;

class CPinList {
public:
    CPinList();                                                     // 0x1008d120
    ~CPinList();                                                    // 0x1008d140
    CPinList(const CPinList &) = delete;
    CPinList &operator=(const CPinList &) = delete;

    void RemoveAll();                                               // 0x1008d1d0
    bool Find(const CBasePin *pin) const;                           // 0x1008d210
    bool AddTail(CBasePin *pin);                                    // 0x1008d240
    bool AddTail(const CPinList &list);                             // 0x1008d2b0

private:
    struct CNode {
        CNode *m_pPrev;
        CNode *m_pNext;
        CBasePin *m_pObject;
    };
    CNode *m_pFirst;                                                // +0x00
    CNode *m_pLast;                                                 // +0x04
    LONG m_Count;                                                   // +0x08
    // CNodeCache: freed nodes for reuse.
    INT m_iCacheSize;                                               // +0x0C (10)
    INT m_iUsed;                                                    // +0x10
    CNode *m_pHead;                                                 // +0x14
};
static_assert(sizeof(CPinList) == 0x18, "CPinList layout");

// -- mtype --------------------------------------------------------------------------------------

class CMediaType : public AM_MEDIA_TYPE {
public:
    CMediaType();                                                   // 0x1008b420
    ~CMediaType();                                                  // 0x1008b3e0
    CMediaType(const CMediaType &) = delete;
    CMediaType &operator=(const CMediaType &) = delete;

    HRESULT Set(const AM_MEDIA_TYPE &type);                         // 0x1008b440
    void InitMediaType();                                           // 0x1008b4e0
    BOOL IsPartiallySpecified() const;                              // 0x1008b500
    BOOL MatchesPartial(const CMediaType *partial) const;           // 0x1008b540
    const GUID *Type() const { return &majortype; }
    const GUID *Subtype() const { return &subtype; }
    const GUID *FormatType() const { return &formattype; }
    BYTE *Format() const { return pbFormat; }
};
static_assert(sizeof(CMediaType) == 0x48, "CMediaType layout");

void FreeMediaType(AM_MEDIA_TYPE &type);
void DeleteMediaType(AM_MEDIA_TYPE *type);                          // 0x1008b610
HRESULT CopyMediaType(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src); // 0x1008b660

// -- amfilter -----------------------------------------------------------------------------------

// Filter registration data (combase.h of the SDK).
typedef REGPINTYPES AMOVIESETUP_MEDIATYPE;
typedef REGFILTERPINS AMOVIESETUP_PIN;
struct AMOVIESETUP_FILTER {
    const CLSID *clsID;
    const WCHAR *strName;
    DWORD dwMerit;
    UINT nPins;
    const AMOVIESETUP_PIN *lpPin;
};

class CRefTime {
public:
    REFERENCE_TIME m_time = 0;
};

class CBaseFilter : public CUnknown, public IBaseFilter, public IAMovieSetup {
public:
    CBaseFilter(const TCHAR *name, IUnknown *outer, CCritSec *lock, REFCLSID clsid);   // 0x10089030
    ~CBaseFilter() override;                                        // 0x10089090

    LS3D_DECLARE_IUNKNOWN
    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;  // 0x10088f50

    // IPersist
    HRESULT __stdcall GetClassID(CLSID *clsid) override;            // 0x10089100
    // IMediaFilter
    HRESULT __stdcall Stop() override;                              // 0x10089260
    HRESULT __stdcall Pause() override;                             // 0x10089310
    HRESULT __stdcall Run(REFERENCE_TIME start) override;           // 0x100893c0
    HRESULT __stdcall SetSyncSource(IReferenceClock *clock) override;  // 0x10089140
    HRESULT __stdcall GetSyncSource(IReferenceClock **clock) override; // 0x100891b0
    // IBaseFilter
    HRESULT __stdcall EnumPins(IEnumPins **pins) override;          // 0x100894f0
    HRESULT __stdcall QueryFilterInfo(FILTER_INFO *info) override;  // 0x10089580
    HRESULT __stdcall JoinFilterGraph(IFilterGraph *graph, LPCWSTR name) override;   // 0x100895d0
    HRESULT __stdcall QueryVendorInfo(LPWSTR *info) override;       // 0x100896b0
    // IAMovieSetup
    HRESULT __stdcall Register() override;                          // 0x10089720
    HRESULT __stdcall Unregister() override;                        // 0x10089790

    // Helpers (the virtual ones in the original's slot order).
    virtual HRESULT StreamTime(CRefTime &rtStream);                 // 0x100894b0
    virtual LONG GetPinVersion();                                   // 0x10089710
    virtual int GetPinCount() = 0;
    virtual CBasePin *GetPin(int n) = 0;
    virtual const AMOVIESETUP_FILTER *GetSetupData() { return nullptr; }   // 0x100885e0

    BOOL IsActive() const { return m_State == State_Paused || m_State == State_Running; }
    BOOL IsStopped() { return m_State == State_Stopped; }
    void IncrementPinVersion() { InterlockedIncrement(&m_PinVersion); }
    HRESULT NotifyEvent(long code, LONG_PTR param1, LONG_PTR param2);  // 0x100896c0

protected:
    FILTER_STATE m_State;                                           // +0x14
    IReferenceClock *m_pClock;                                      // +0x18
    CRefTime m_tStart;                                              // +0x20
    CLSID m_clsid;                                                  // +0x28
    CCritSec *m_pLock;                                              // +0x38
    WCHAR *m_pName;                                                 // +0x3C
    IFilterGraph *m_pGraph;                                         // +0x40
    IMediaEventSink *m_pSink;                                       // +0x44
    LONG m_PinVersion;                                              // +0x48

    friend class CBasePin;
    friend class CEnumPins;
};
static_assert(sizeof(CBaseFilter) == 0x50, "CBaseFilter layout");

class CEnumPins : public IEnumPins {
public:
    CEnumPins(CBaseFilter *filter, CEnumPins *copy);                // 0x10089810
    virtual ~CEnumPins();                                           // 0x10089900

    HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override;   // 0x10089960
    ULONG __stdcall AddRef() override;                              // 0x100899c0
    ULONG __stdcall Release() override;                             // 0x100899e0
    HRESULT __stdcall Next(ULONG count, IPin **pins, ULONG *fetched) override;   // 0x10089ab0
    HRESULT __stdcall Skip(ULONG count) override;                   // 0x10089bd0
    HRESULT __stdcall Reset() override;                             // 0x10089c10
    HRESULT __stdcall Clone(IEnumPins **out) override;              // 0x10089a10

private:
    BOOL AreWeOutOfSync() { return m_pFilter->GetPinVersion() != m_Version; }

    int m_Position;                                                 // +0x04
    int m_PinCount;                                                 // +0x08
    CBaseFilter *m_pFilter;                                         // +0x0C
    LONG m_Version;                                                 // +0x10
    volatile LONG m_cRef;                                           // +0x14
    CPinList m_PinCache;                                            // +0x18 pins already returned
};
static_assert(sizeof(CEnumPins) == 0x30, "CEnumPins layout");

class CEnumMediaTypes : public IEnumMediaTypes {
public:
    CEnumMediaTypes(CBasePin *pin, CEnumMediaTypes *copy);
    virtual ~CEnumMediaTypes();                                     // 0x10089c40

    HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override;   // 0x10089c70
    ULONG __stdcall AddRef() override;                              // 0x10089cd0
    ULONG __stdcall Release() override;                             // 0x10089cf0
    HRESULT __stdcall Next(ULONG count, AM_MEDIA_TYPE **types, ULONG *fetched) override;   // 0x10089de0
    HRESULT __stdcall Skip(ULONG count) override;                   // 0x10089f60
    HRESULT __stdcall Reset() override;                             // 0x1008a020
    HRESULT __stdcall Clone(IEnumMediaTypes **out) override;        // 0x10089d20

private:
    BOOL AreWeOutOfSync();

    int m_Position;                                                 // +0x04
    CBasePin *m_pPin;                                               // +0x08
    LONG m_Version;                                                 // +0x0C
    volatile LONG m_cRef;                                           // +0x10
};
static_assert(sizeof(CEnumMediaTypes) == 0x14, "CEnumMediaTypes layout");

class CBasePin : public CUnknown, public IPin, public IQualityControl {
public:
    CBasePin(const TCHAR *objectName, CBaseFilter *filter, CCritSec *lock, HRESULT *phr, LPCWSTR name,
             PIN_DIRECTION dir);                                    // 0x1008a040
    ~CBasePin() override;                                           // 0x1008a130

    LS3D_DECLARE_IUNKNOWN
    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;
    ULONG __stdcall NonDelegatingAddRef() override;                 // 0x1008a190
    ULONG __stdcall NonDelegatingRelease() override;                // 0x1008a1b0

    // IPin
    HRESULT __stdcall Connect(IPin *receivePin, const AM_MEDIA_TYPE *pmt) override;   // 0x1008a1d0
    HRESULT __stdcall ReceiveConnection(IPin *connector, const AM_MEDIA_TYPE *pmt) override;   // 0x1008a5a0
    HRESULT __stdcall Disconnect() override;                        // 0x1008a700
    HRESULT __stdcall ConnectedTo(IPin **pin) override;             // 0x1008a7b0
    HRESULT __stdcall ConnectionMediaType(AM_MEDIA_TYPE *pmt) override;   // 0x1008a7e0
    HRESULT __stdcall QueryPinInfo(PIN_INFO *info) override;        // 0x1008a890
    HRESULT __stdcall QueryDirection(PIN_DIRECTION *dir) override;  // 0x1008a900
    HRESULT __stdcall QueryAccept(const AM_MEDIA_TYPE *pmt) override;   // 0x1008a920
    HRESULT __stdcall EnumMediaTypes(IEnumMediaTypes **types) override; // 0x1008a950
    HRESULT __stdcall QueryInternalConnections(IPin **pins, ULONG *count) override;
    HRESULT __stdcall EndOfStream() override { return S_OK; }
    HRESULT __stdcall BeginFlush() override = 0;
    HRESULT __stdcall EndFlush() override = 0;
    HRESULT __stdcall NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;   // 0x1008aa50
    // IQualityControl
    HRESULT __stdcall SetSink(IQualityControl *sink) override;     // 0x1008aa20

    // Helpers (virtual ones in the original's slot order).
    virtual LONG GetMediaTypeVersion();                             // 0x1008aa10
    virtual HRESULT Active() { return NOERROR; }
    virtual HRESULT Inactive() { m_bRunTimeError = FALSE; return NOERROR; }
    virtual HRESULT Run(REFERENCE_TIME) { return NOERROR; }
    virtual HRESULT CheckMediaType(const CMediaType *pmt) = 0;
    virtual HRESULT SetMediaType(const CMediaType *pmt);            // 0x1008a550
    virtual HRESULT CheckConnect(IPin *pin);                        // 0x1008a570
    virtual HRESULT BreakConnect() { return NOERROR; }
    virtual HRESULT CompleteConnect(IPin *) { return NOERROR; }
    virtual HRESULT GetMediaType(int position, CMediaType *pmt);    // 0x1008aa00

    BOOL IsConnected() const { return m_Connected != nullptr; }
    IPin *GetConnected() const { return m_Connected; }
    BOOL IsStopped() const { return m_pFilter->m_State == State_Stopped; }
    void IncrementTypeVersion() { InterlockedIncrement(&m_TypeVersion); }

protected:
    HRESULT AttemptConnection(IPin *receivePin, const CMediaType *pmt);   // 0x1008a2e0
    HRESULT TryMediaTypes(IPin *receivePin, const CMediaType *pmt, IEnumMediaTypes *types);   // 0x1008a390
    HRESULT AgreeMediaType(IPin *receivePin, const CMediaType *pmt);      // 0x1008a470

    WCHAR *m_pName;                                                 // +0x14
    IPin *m_Connected;                                              // +0x18
    PIN_DIRECTION m_dir;                                            // +0x1C
    CCritSec *m_pLock;                                              // +0x20
    bool m_bRunTimeError;                                           // +0x24
    bool m_bCanReconnectWhenActive;                                 // +0x25
    bool m_bTryMyTypesFirst;                                        // +0x26
    CBaseFilter *m_pFilter;                                         // +0x28
    IQualityControl *m_pQSink;                                      // +0x2C
    LONG m_TypeVersion;                                             // +0x30
    CMediaType m_mt;                                                // +0x34
    CRefTime m_tStart;                                              // +0x80
    CRefTime m_tStop;                                               // +0x88
    double m_dRate;                                                 // +0x90

    friend class CEnumMediaTypes;
};
static_assert(sizeof(CBasePin) == 0x98, "CBasePin layout");

class CBaseInputPin : public CBasePin, public IMemInputPin {
public:
    CBaseInputPin(const TCHAR *objectName, CBaseFilter *filter, CCritSec *lock, HRESULT *phr,
                  LPCWSTR name);                                    // 0x1008aa90
    ~CBaseInputPin() override;                                      // 0x1008aaf0

    LS3D_DECLARE_IUNKNOWN
    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;  // 0x1008ab70

    // IMemInputPin
    HRESULT __stdcall GetAllocator(IMemAllocator **allocator) override;   // 0x1008ac20
    HRESULT __stdcall NotifyAllocator(IMemAllocator *allocator, BOOL readOnly) override;   // 0x1008acc0
    HRESULT __stdcall GetAllocatorRequirements(ALLOCATOR_PROPERTIES *props) override;      // 0x100896b0
    HRESULT __stdcall Receive(IMediaSample *sample) override;       // 0x1008ad80
    HRESULT __stdcall ReceiveMultiple(IMediaSample **samples, long count, long *processed) override;   // 0x1008aee0
    HRESULT __stdcall ReceiveCanBlock() override;                   // 0x1008af40
    // IPin
    HRESULT __stdcall BeginFlush() override;                        // 0x1008b000
    HRESULT __stdcall EndFlush() override;                          // 0x1008b030
    // IQualityControl
    HRESULT __stdcall Notify(IBaseFilter *self, Quality q) override;   // 0x1008b060

    HRESULT BreakConnect() override;                                // 0x1008ad40
    virtual HRESULT CheckStreaming();                               // 0x1008b080

    BOOL IsReadOnly() const { return m_bReadOnly; }
    BOOL IsFlushing() const { return m_bFlushing; }
    IMemAllocator *Allocator() const { return m_pAllocator; }
    AM_SAMPLE2_PROPERTIES *SampleProps() { return &m_SampleProps; }

protected:
    IMemAllocator *m_pAllocator;                                    // +0x9C
    BYTE m_bReadOnly;                                               // +0xA0
    BYTE m_bFlushing;                                               // +0xA1
    AM_SAMPLE2_PROPERTIES m_SampleProps;                            // +0xA8
};
static_assert(sizeof(CBaseInputPin) == 0xd8, "CBaseInputPin layout");

HRESULT AMovieSetupRegisterFilter(const AMOVIESETUP_FILTER *setup, IFilterMapper *mapper, BOOL reg);   // 0x1008b0b0

// -- ctlutil ------------------------------------------------------------------------------------

class CBaseDispatch {
public:
    ~CBaseDispatch();
    HRESULT GetTypeInfo(REFIID riid, UINT itinfo, LCID lcid, ITypeInfo **pptinfo);   // 0x1008bd30
    HRESULT GetIDsOfNames(REFIID riid, OLECHAR **names, UINT count, LCID lcid, DISPID *dispids);

private:
    ITypeInfo *m_pti = nullptr;
};

class CMediaPosition : public IMediaPosition, public CUnknown {
public:
    CMediaPosition(const TCHAR *name, IUnknown *outer);
    LS3D_DECLARE_IUNKNOWN
    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;
    // IDispatch
    HRESULT __stdcall GetTypeInfoCount(UINT *count) override;       // 0x1008be40
    HRESULT __stdcall GetTypeInfo(UINT itinfo, LCID lcid, ITypeInfo **pptinfo) override;   // 0x1008be60
    HRESULT __stdcall GetIDsOfNames(REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *dispids) override;   // 0x1008be90
    HRESULT __stdcall Invoke(DISPID dispid, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params,
                             VARIANT *result, EXCEPINFO *excepinfo, UINT *argErr) override;   // 0x1008bee0

private:
    CBaseDispatch m_basedisp;                                       // +0x14
};

class CPosPassThru : public IMediaSeeking, public CMediaPosition {
public:
    CPosPassThru(const TCHAR *name, IUnknown *outer, HRESULT *phr, IPin *pin);
    LS3D_DECLARE_IUNKNOWN
    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;  // 0x1008c000

    // IMediaSeeking
    HRESULT __stdcall GetCapabilities(DWORD *caps) override;        // 0x1008c080
    HRESULT __stdcall CheckCapabilities(DWORD *caps) override;      // 0x1008c0f0
    HRESULT __stdcall IsFormatSupported(const GUID *format) override;   // 0x1008c160
    HRESULT __stdcall QueryPreferredFormat(GUID *format) override;  // 0x1008c1d0
    HRESULT __stdcall GetTimeFormat(GUID *format) override;         // 0x1008c2b0
    HRESULT __stdcall IsUsingTimeFormat(const GUID *format) override;   // 0x1008c320
    HRESULT __stdcall SetTimeFormat(const GUID *format) override;   // 0x1008c240
    HRESULT __stdcall GetDuration(LONGLONG *duration) override;     // 0x1008c600
    HRESULT __stdcall GetStopPosition(LONGLONG *stop) override;     // 0x1008c590
    HRESULT __stdcall GetCurrentPosition(LONGLONG *current) override;   // 0x1008c500
    HRESULT __stdcall ConvertTimeFormat(LONGLONG *target, const GUID *targetFormat, LONGLONG source,
                                        const GUID *sourceFormat) override;   // 0x1008c390
    HRESULT __stdcall SetPositions(LONGLONG *current, DWORD currentFlags, LONGLONG *stop,
                                   DWORD stopFlags) override;       // 0x1008c410
    HRESULT __stdcall GetPositions(LONGLONG *current, LONGLONG *stop) override;   // 0x1008c490
    HRESULT __stdcall GetAvailable(LONGLONG *earliest, LONGLONG *latest) override;   // 0x1008c6e0
    HRESULT __stdcall SetRate(double rate) override;                // 0x1008c7c0
    HRESULT __stdcall GetRate(double *rate) override;               // 0x1008c750
    HRESULT __stdcall GetPreroll(LONGLONG *preroll) override;       // 0x1008c670
    // The current position the renderer knows of (0x1008cdf0: none).
    virtual HRESULT GetMediaTime(LONGLONG *start, LONGLONG *end) { return E_FAIL; }

    // IMediaPosition
    HRESULT __stdcall get_Duration(REFTIME *length) override;       // 0x1008c850
    HRESULT __stdcall put_CurrentPosition(REFTIME time) override;   // 0x1008c930
    HRESULT __stdcall get_CurrentPosition(REFTIME *time) override;  // 0x1008c8c0
    HRESULT __stdcall get_StopTime(REFTIME *time) override;         // 0x1008c9a0
    HRESULT __stdcall put_StopTime(REFTIME time) override;          // 0x1008ca10
    HRESULT __stdcall get_PrerollTime(REFTIME *time) override;      // 0x1008ca80
    HRESULT __stdcall put_PrerollTime(REFTIME time) override;       // 0x1008caf0
    HRESULT __stdcall put_Rate(double rate) override;               // 0x1008cbd0
    HRESULT __stdcall get_Rate(double *rate) override;              // 0x1008cb60
    HRESULT __stdcall CanSeekForward(LONG *can) override;           // 0x1008cc60
    HRESULT __stdcall CanSeekBackward(LONG *can) override;          // 0x1008ccd0

private:
    HRESULT GetPeer(IMediaPosition **out);
    HRESULT GetPeerSeeking(IMediaSeeking **out);
    HRESULT GetSeekingLongLong(HRESULT(__stdcall IMediaSeeking::*method)(LONGLONG *), LONGLONG *value);

    IPin *m_pPin;                                                   // +0x18 the renderer's input pin
};

class CRendererPosPassThru : public CPosPassThru {
public:
    CRendererPosPassThru(const TCHAR *name, IUnknown *outer, HRESULT *phr, IPin *pin);   // 0x1008cd50

    HRESULT RegisterMediaTime(IMediaSample *sample);                // 0x1008ceb0
    HRESULT GetMediaTime(LONGLONG *start, LONGLONG *end) override;  // 0x1008cf60
    HRESULT ResetMediaTime();                                       // 0x1008d020
    HRESULT EOS();                                                  // 0x1008d050

private:
    CCritSec m_PositionLock;                                        // +0x1C
    LONGLONG m_StartMedia;                                          // +0x38
    LONGLONG m_EndMedia;                                            // +0x40
    BOOL m_bReset;                                                  // +0x48
};
static_assert(sizeof(CRendererPosPassThru) == 0x50, "CRendererPosPassThru layout");

// -- renbase ------------------------------------------------------------------------------------

class CRendererInputPin;

class CBaseRenderer : public CBaseFilter {
public:
    CBaseRenderer(REFCLSID renderClass, const TCHAR *name, IUnknown *outer, HRESULT *phr);   // 0x10086530
    ~CBaseRenderer() override;                                      // 0x10086650

    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;  // 0x100868b0

    // IMediaFilter / IBaseFilter
    HRESULT __stdcall Stop() override;                              // 0x10086a60
    HRESULT __stdcall Pause() override;                             // 0x10086bd0
    HRESULT __stdcall Run(REFERENCE_TIME start) override;           // 0x10086d20
    HRESULT __stdcall GetState(DWORD msecs, FILTER_STATE *state) override;   // 0x100869a0
    HRESULT __stdcall FindPin(LPCWSTR id, IPin **pin) override;     // 0x10086fd0

    int GetPinCount() override { return 1; }                        // 0x10086e80
    CBasePin *GetPin(int n) override;                               // 0x10086e90

    // Virtuals in the original's slot order.
    virtual HRESULT GetMediaPositionInterface(REFIID riid, void **ppv);   // 0x10086780
    virtual HRESULT SourceThreadCanWait(BOOL canWait);              // 0x10086900
    virtual HRESULT WaitForRenderTime();                            // 0x10086930
    virtual HRESULT CompleteStateChange(FILTER_STATE oldState);     // 0x100869f0
    virtual void OnReceiveFirstSample(IMediaSample *) {}
    virtual void OnRenderStart(IMediaSample *) {}
    virtual void OnRenderEnd(IMediaSample *) {}
    virtual HRESULT OnStartStreaming() { return NOERROR; }
    virtual HRESULT OnStopStreaming() { return NOERROR; }
    virtual void OnWaitStart() {}
    virtual void OnWaitEnd() {}
    virtual void PrepareRender() {}
    virtual BOOL ScheduleSample(IMediaSample *sample);              // 0x100872c0
    virtual HRESULT GetSampleTimes(IMediaSample *sample, REFERENCE_TIME *start, REFERENCE_TIME *end);   // 0x10087220
    virtual HRESULT ShouldDrawSampleNow(IMediaSample *, REFERENCE_TIME *, REFERENCE_TIME *) { return S_FALSE; }   // 0x10087280
    virtual HRESULT SendEndOfStream();                              // 0x10087810
    virtual HRESULT ResetEndOfStream();                             // 0x10087970
    virtual HRESULT EndOfStream();                                  // 0x10087040
    virtual HRESULT CancelNotification();                           // 0x10087290
    virtual HRESULT ClearPendingSample();                           // 0x10087730
    virtual HRESULT Active() { return NOERROR; }                    // 0x10087170
    virtual HRESULT Inactive();                                     // 0x10087180
    virtual HRESULT StartStreaming();                               // 0x100879c0
    virtual HRESULT StopStreaming();                                // 0x10087a70
    virtual HRESULT BeginFlush();                                   // 0x10087080
    virtual HRESULT EndFlush();                                     // 0x10087110
    virtual HRESULT BreakConnect();                                 // 0x100871a0
    virtual HRESULT SetMediaType(const CMediaType *) { return NOERROR; }
    virtual HRESULT CompleteConnect(IPin *receivePin);              // 0x10087130
    virtual HRESULT PrepareReceive(IMediaSample *sample);           // 0x10087430
    virtual HRESULT Receive(IMediaSample *sample);                  // 0x100875d0
    virtual BOOL HaveCurrentSample();                               // 0x100873a0
    virtual IMediaSample *GetCurrentSample();                       // 0x100873d0
    virtual HRESULT Render(IMediaSample *sample);                   // 0x10087350
    virtual HRESULT DoRenderSample(IMediaSample *sample) = 0;
    virtual HRESULT CheckMediaType(const CMediaType *pmt) = 0;

    void TimerCallback();                                           // 0x100877b0
    void ResetEndOfStreamTimer();
    HRESULT NotifyEndOfStream();                                    // 0x100878d0
    void SetRepaintStatus(BOOL repaint);                            // 0x10087ae0
    void WaitForReceiveToComplete();
    void Ready() { m_evComplete.Set(); }
    void NotReady() { m_evComplete.Reset(); }
    BOOL IsStreaming() const { return m_bStreaming; }
    void SetAbortSignal(BOOL abort) { m_bAbort = abort; }
    CAMEvent *GetRenderEvent() { return &m_RenderEvent; }

protected:
    CRendererPosPassThru *m_pPosition;                              // +0x50
    CAMEvent m_RenderEvent;                                         // +0x54 clock advise fired
    CAMEvent m_ThreadSignal;                                        // +0x58 releases the source thread
    CAMEvent m_evComplete;                                          // +0x5C state change complete
    BOOL m_bAbort;                                                  // +0x60
    BOOL m_bStreaming;                                              // +0x64
    DWORD_PTR m_dwAdvise;                                           // +0x68
    IMediaSample *m_pMediaSample;                                   // +0x6C
    BOOL m_bEOS;                                                    // +0x70
    BOOL m_bEOSDelivered;                                           // +0x74
    CRendererInputPin *m_pInputPin;                                 // +0x78
    CCritSec m_InterfaceLock;                                       // +0x7C
    CCritSec m_RendererLock;                                        // +0x94
    IQualityControl *m_pQSink;                                      // +0xAC
    BOOL m_bRepaintStatus;                                          // +0xB0
    volatile BOOL m_bInReceive;                                     // +0xB4
    REFERENCE_TIME m_SignalTime;                                    // +0xB8
    UINT m_EndOfStreamTimer;                                        // +0xC0
    CCritSec m_ObjectCreationLock;                                  // +0xC4

    friend class CRendererInputPin;
};
static_assert(sizeof(CBaseRenderer) == 0xe0, "CBaseRenderer layout");

class CRendererInputPin : public CBaseInputPin {
public:
    CRendererInputPin(CBaseRenderer *renderer, HRESULT *phr, LPCWSTR name);

    // Override the pin and input pin methods
    HRESULT __stdcall EndOfStream() override;                       // 0x10087b50
    HRESULT __stdcall BeginFlush() override;                        // 0x10087bf0
    HRESULT __stdcall EndFlush() override;                          // 0x10087c90
    HRESULT __stdcall Receive(IMediaSample *sample) override;       // 0x10087d30
    HRESULT __stdcall QueryId(LPWSTR *id) override;                 // 0x10087ed0

    HRESULT BreakConnect() override;                                // 0x10087e80
    HRESULT CompleteConnect(IPin *receivePin) override;             // 0x10087ea0
    HRESULT SetMediaType(const CMediaType *pmt) override;           // 0x10087f40
    HRESULT CheckMediaType(const CMediaType *pmt) override;         // 0x10087f10
    HRESULT Active() override;                                      // 0x10087f20
    HRESULT Inactive() override;                                    // 0x10087f30

    const CMediaType *CurrentMediaType() const { return &m_mt; }
    BOOL CanReconnectWhenActive() const { return m_bCanReconnectWhenActive; }

private:
    CBaseRenderer *m_pRenderer;                                     // +0xD8
};
static_assert(sizeof(CRendererInputPin) == 0xe0, "CRendererInputPin layout");

class CBaseVideoRenderer : public CBaseRenderer, public IQualProp, public IQualityControl {
public:
    CBaseVideoRenderer(REFCLSID renderClass, const TCHAR *name, IUnknown *outer, HRESULT *phr);   // 0x10087f70
    ~CBaseVideoRenderer() override = default;                       // 0x100880c0

    LS3D_DECLARE_IUNKNOWN
    HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void **ppv) override;  // 0x10088de0

    HRESULT __stdcall JoinFilterGraph(IFilterGraph *graph, LPCWSTR name) override;   // 0x10088ea0

    // IQualityControl
    HRESULT __stdcall SetSink(IQualityControl *sink) override;     // 0x10088420
    HRESULT __stdcall Notify(IBaseFilter *self, Quality q) override;   // 0x10088430

    // IQualProp
    HRESULT __stdcall get_FramesDroppedInRenderer(int *frames) override;   // 0x10088ad0
    HRESULT __stdcall get_FramesDrawn(int *frames) override;        // 0x10088b10
    HRESULT __stdcall get_AvgFrameRate(int *rate) override;         // 0x10088b50
    HRESULT __stdcall get_Jitter(int *jitter) override;             // 0x10088db0
    HRESULT __stdcall get_AvgSyncOffset(int *offset) override;      // 0x10088bd0
    HRESULT __stdcall get_DevSyncOffset(int *deviation) override;   // 0x10088d80

    // Quality management.
    void OnRenderStart(IMediaSample *sample) override;              // 0x10088370
    void OnRenderEnd(IMediaSample *sample) override;                // 0x100883a0
    void OnWaitStart() override {}
    void OnWaitEnd() override {}
    HRESULT OnStartStreaming() override;                            // 0x100881b0
    HRESULT OnStopStreaming() override;                             // 0x100881c0
    BOOL ScheduleSample(IMediaSample *sample) override;             // 0x10088a50
    HRESULT ShouldDrawSampleNow(IMediaSample *sample, REFERENCE_TIME *start, REFERENCE_TIME *end) override;   // 0x100885f0

    virtual void RecordFrameLateness(int late, int frame);          // 0x100881f0
    virtual void OnDirectRender(IMediaSample *sample);              // 0x10088300
    virtual HRESULT ResetStreamingTimes();                          // 0x100880f0
    virtual HRESULT SendQuality(REFERENCE_TIME late, REFERENCE_TIME realStream);   // 0x10088470

protected:
    void ThrottleWait();
    void PreparePerformanceData(int late, int frame)
    {
        m_trLate = late;
        m_trFrame = frame;
    }
    HRESULT GetStdDev(int samples, int *result, LONGLONG sumSq, LONGLONG total);   // 0x10088c60

    int m_nNormal;                                                  // +0xE8 frames drawn on time (-1: dropped)
    BOOL m_bSupplierHandlingQuality;                                // +0xEC
    int m_trThrottle;                                               // +0xF0
    int m_trRenderAvg;                                              // +0xF4
    int m_trRenderLast;                                             // +0xF8
    int m_tRenderStart;                                             // +0xFC
    int m_trEarliness;                                              // +0x100
    int m_trTarget;                                                 // +0x104
    int m_trWaitAvg;                                                // +0x108
    int m_trFrameAvg;                                               // +0x10C
    int m_trDuration;                                               // +0x110
    REFERENCE_TIME m_trRememberStampForPerf;                        // +0x118
    int m_cFramesDropped;                                           // +0x120
    int m_cFramesDrawn;                                             // +0x124
    LONGLONG m_iTotAcc;                                             // +0x128
    LONGLONG m_iSumSqAcc;                                           // +0x130
    REFERENCE_TIME m_trLastDraw;                                    // +0x138
    LONGLONG m_iSumSqFrameTime;                                     // +0x140
    LONGLONG m_iSumFrameTime;                                       // +0x148
    int m_trLate;                                                   // +0x150
    int m_trFrame;                                                  // +0x154
    int m_tStreamingStart;                                          // +0x158
};
static_assert(sizeof(CBaseVideoRenderer) == 0x160, "CBaseVideoRenderer layout");

}  // namespace ls3d::dshow
