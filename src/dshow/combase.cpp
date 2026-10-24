// DirectShow base classes: COM helpers (combase), events and wide strings (wxutil), the pin list
// (wxlist) and media types (mtype). 0x1008b280 - 0x1008bd30, 0x1008d120 - 0x1008d310.
#include "dshow/baseclasses.h"

#include <cstring>
#include <new>

namespace ls3d::dshow {

namespace {
volatile LONG g_objects;                    // 0x100d7340
HMODULE g_oleAut32;                         // 0x100d733c
UINT g_unblockMessage;                      // 0x100d7344 "AMUnblock"
}  // namespace

// -- combase ------------------------------------------------------------------------------------

void BaseObjectCreated()
{
    InterlockedIncrement(&g_objects);
}

// 0x1008b280
void BaseObjectDestroyed()
{
    if (InterlockedDecrement(&g_objects) == 0 && g_oleAut32) {
        FreeLibrary(g_oleAut32);
        g_oleAut32 = nullptr;
    }
}

// 0x1008b2b0
HMODULE LoadOleAut32()
{
    if (!g_oleAut32)
        g_oleAut32 = LoadLibraryA("OleAut32.dll");
    return g_oleAut32;
}

// 0x1008b2d0
CUnknown::CUnknown(const TCHAR *, IUnknown *outer)
    : m_pUnknown(outer ? outer : reinterpret_cast<IUnknown *>(static_cast<INonDelegatingUnknown *>(this))),
      m_cRef(0)
{
    BaseObjectCreated();
}

// 0x1008b300
HRESULT __stdcall CUnknown::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown)
        return GetInterface(reinterpret_cast<IUnknown *>(static_cast<INonDelegatingUnknown *>(this)), ppv);
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// 0x1008b350
ULONG __stdcall CUnknown::NonDelegatingAddRef()
{
    InterlockedIncrement(&m_cRef);
    return m_cRef > 1 ? static_cast<ULONG>(m_cRef) : 1;
}

// 0x1008b370
ULONG __stdcall CUnknown::NonDelegatingRelease()
{
    if (InterlockedDecrement(&m_cRef) == 0) {
        // (one more reference so that the destructor can call AddRef / Release safely)
        ++m_cRef;
        delete this;
        return 0;
    }
    return m_cRef > 1 ? static_cast<ULONG>(m_cRef) : 1;
}

// 0x1008b3b0
HRESULT GetInterface(IUnknown *unknown, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = unknown;
    unknown->AddRef();
    return NOERROR;
}

// -- wxutil -------------------------------------------------------------------------------------

// 0x1008b6d0
CAMEvent::CAMEvent(BOOL manualReset) : m_hEvent(CreateEventA(nullptr, manualReset, FALSE, nullptr))
{
}

// 0x1008b6f0
CAMEvent::~CAMEvent()
{
    if (m_hEvent)
        CloseHandle(m_hEvent);
}

// 0x1008b700
WCHAR *lstrcpyWInternal(WCHAR *dst, const WCHAR *src)
{
    WCHAR *const result = dst;
    while ((*dst++ = *src++) != 0) {
    }
    return result;
}

// 0x1008b730: copies at most count - 1 characters and always terminates.
WCHAR *lstrcpynWInternal(WCHAR *dst, const WCHAR *src, int count)
{
    WCHAR *const result = dst;
    if (count) {
        while (--count) {
            if ((*dst++ = *src++) == 0)
                return result;
        }
        *dst = 0;
    }
    return result;
}

// 0x1008b770
int lstrcmpWInternal(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        const WCHAR ca = *a, cb = *b;
        if (ca != cb)
            return static_cast<int>(ca) - static_cast<int>(cb);
        if (!ca)
            return 0;
        ++a;
        ++b;
    }
}

// 0x1008b7b0
int lstrlenWInternal(const WCHAR *s)
{
    int n = 0;
    while (s[n])
        ++n;
    return n;
}

// 0x1008b7d0: 128-bit intermediate, saturating on overflow and division by zero.
LONGLONG llMulDiv(LONGLONG a, LONGLONG b, LONGLONG c, LONGLONG d)
{
    ULARGE_INTEGER ua, ub;
    ua.QuadPart = static_cast<DWORDLONG>(a >= 0 ? a : -a);
    ub.QuadPart = static_cast<DWORDLONG>(b >= 0 ? b : -b);
    const DWORDLONG uc = static_cast<DWORDLONG>(c >= 0 ? c : -c);
    BOOL sign = (a < 0) ^ (b < 0);

    // Long multiplication into p[1]:p[0].
    ULARGE_INTEGER p[2];
    p[0].QuadPart = UInt32x32To64(ua.LowPart, ub.LowPart);
    ULARGE_INTEGER x;
    x.QuadPart = UInt32x32To64(ua.LowPart, ub.HighPart) + UInt32x32To64(ua.HighPart, ub.LowPart) + p[0].HighPart;
    p[0].HighPart = x.LowPart;
    p[1].QuadPart = UInt32x32To64(ua.HighPart, ub.HighPart) + x.HighPart;

    if (d != 0) {
        ULARGE_INTEGER ud[2];
        if (sign) {
            ud[0].QuadPart = static_cast<DWORDLONG>(-d);
            ud[1].QuadPart = d > 0 ? static_cast<DWORDLONG>(-1LL) : 0;
        } else {
            ud[0].QuadPart = static_cast<DWORDLONG>(d);
            ud[1].QuadPart = d < 0 ? static_cast<DWORDLONG>(-1LL) : 0;
        }
        ULARGE_INTEGER total;
        total.QuadPart = static_cast<DWORDLONG>(ud[0].LowPart) + p[0].LowPart;
        p[0].LowPart = total.LowPart;
        total.LowPart = total.HighPart;
        total.HighPart = 0;
        total.QuadPart += static_cast<DWORDLONG>(ud[0].HighPart) + p[0].HighPart;
        p[0].HighPart = total.LowPart;
        total.LowPart = total.HighPart;
        total.HighPart = 0;
        p[1].QuadPart += ud[1].QuadPart + total.QuadPart;
        if (static_cast<LONG>(p[1].HighPart) < 0) {
            sign = !sign;
            p[0].QuadPart = ~p[0].QuadPart;
            p[1].QuadPart = ~p[1].QuadPart;
            p[0].QuadPart += 1;
            p[1].QuadPart += (p[0].QuadPart == 0);
        }
    }

    if (c < 0)
        sign = !sign;
    if (uc <= p[1].QuadPart)
        return sign ? static_cast<LONGLONG>(0x8000000000000000ULL) : 0x7fffffffffffffffLL;
    if (p[1].QuadPart == 0) {
        const DWORDLONG result = p[0].QuadPart / uc;
        return sign ? -static_cast<LONGLONG>(result) : static_cast<LONGLONG>(result);
    }
    ULARGE_INTEGER ulic;
    ulic.QuadPart = uc;
    if (ulic.HighPart == 0) {
        const DWORD divisor = static_cast<DWORD>(uc);
        ULARGE_INTEGER dividend, result;
        dividend.HighPart = p[1].LowPart;
        dividend.LowPart = p[0].HighPart;
        result.HighPart = static_cast<DWORD>(dividend.QuadPart / divisor);
        p[0].HighPart = static_cast<DWORD>(dividend.QuadPart % divisor);
        result.LowPart = 0;
        result.QuadPart = p[0].QuadPart / divisor + result.QuadPart;
        return sign ? -static_cast<LONGLONG>(result.QuadPart) : static_cast<LONGLONG>(result.QuadPart);
    }
    DWORDLONG result = 0;
    for (int i = 0; i < 64; ++i) {
        result <<= 1;
        p[1].QuadPart <<= 1;
        if (p[0].HighPart & 0x80000000)
            p[1].LowPart++;
        p[0].QuadPart <<= 1;
        if (uc <= p[1].QuadPart) {
            p[1].QuadPart -= uc;
            result += 1;
        }
    }
    return sign ? -static_cast<LONGLONG>(result) : static_cast<LONGLONG>(result);
}

// 0x1008bb00: waits without blocking messages sent to the thread's windows (raising the thread's
// priority while it does, so that the queue does not build up).
DWORD WaitDispatchingMessages(HANDLE object, DWORD wait, HWND window, UINT msg, HANDLE event)
{
    BOOL peeked = FALSE;
    DWORD result;
    DWORD start = 0;
    int priority = 0;
    HANDLE objects[2] = {object, event};
    if (wait != INFINITE && wait != 0)
        start = GetTickCount();
    const DWORD count = event ? 2 : 1;
    for (;;) {
        result = WaitForMultipleObjects(count, objects, FALSE, 0);
        if (result < WAIT_OBJECT_0 + count)
            break;
        const DWORD timeout = wait > 10 ? 10 : wait;
        result = MsgWaitForMultipleObjects(count, objects, FALSE, timeout,
                                           window ? QS_SENDMESSAGE + QS_POSTMESSAGE : QS_SENDMESSAGE);
        if (!(result == WAIT_OBJECT_0 + count || (result == WAIT_TIMEOUT && timeout != wait)))
            break;
        MSG m;
        if (window) {
            while (PeekMessageA(&m, window, msg, msg, PM_REMOVE))
                DispatchMessageA(&m);
        }
        PeekMessageA(&m, nullptr, 0, 0, PM_NOREMOVE);
        if (wait != INFINITE && wait != 0) {
            const DWORD now = GetTickCount();
            const DWORD diff = now - start;
            wait = diff > wait ? 0 : wait - diff;
            start = now;
        }
        if (!peeked) {
            // (an unsigned compare: below-normal priorities are not raised)
            priority = GetThreadPriority(GetCurrentThread());
            if (static_cast<DWORD>(priority) < THREAD_PRIORITY_HIGHEST)
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            peeked = TRUE;
        }
    }
    if (peeked) {
        SetThreadPriority(GetCurrentThread(), priority);
        if (HIWORD(GetQueueStatus(QS_POSTMESSAGE)) & QS_POSTMESSAGE) {
            if (!g_unblockMessage)
                g_unblockMessage = RegisterWindowMessageA("AMUnblock");
            if (g_unblockMessage) {
                MSG m;
                while (PeekMessageA(&m, reinterpret_cast<HWND>(-1), g_unblockMessage, g_unblockMessage, PM_REMOVE)) {
                }
            }
            PostThreadMessageA(GetCurrentThreadId(), g_unblockMessage, 0, 0);
        }
    }
    return result;
}

// 0x1008bd10 (built for Windows versions without TIME_KILL_SYNCHRONOUS).
MMRESULT CompatibleTimeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user, UINT flags)
{
    return timeSetEvent(delay, resolution, callback, user, flags);
}

// -- wxlist -------------------------------------------------------------------------------------

// 0x1008d120
CPinList::CPinList() : m_pFirst(nullptr), m_pLast(nullptr), m_Count(0), m_iCacheSize(10), m_iUsed(0), m_pHead(nullptr)
{
}

// 0x1008d140
CPinList::~CPinList()
{
    RemoveAll();
    for (CNode *n = m_pHead; n;) {
        CNode *next = n->m_pNext;
        delete n;
        n = next;
    }
}

// 0x1008d1d0: frees the nodes (not into the cache).
void CPinList::RemoveAll()
{
    for (CNode *n = m_pFirst; n;) {
        CNode *next = n->m_pNext;
        delete n;
        n = next;
    }
    m_pFirst = m_pLast = nullptr;
    m_Count = 0;
}

// 0x1008d210
bool CPinList::Find(const CBasePin *pin) const
{
    for (const CNode *n = m_pFirst; n; n = n->m_pNext) {
        if (n->m_pObject == pin)
            return true;
    }
    return false;
}

// 0x1008d240: a cached node, or a new one.
bool CPinList::AddTail(CBasePin *pin)
{
    CNode *n = m_pHead;
    if (n) {
        m_pHead = n->m_pNext;
        --m_iUsed;
    } else {
        n = new (std::nothrow) CNode;
        if (!n)
            return false;
    }
    n->m_pObject = pin;
    n->m_pNext = nullptr;
    n->m_pPrev = m_pLast;
    if (m_pLast)
        m_pLast->m_pNext = n;
    else
        m_pFirst = n;
    m_pLast = n;
    ++m_Count;
    return true;
}

// 0x1008d2b0
bool CPinList::AddTail(const CPinList &list)
{
    for (const CNode *n = list.m_pFirst; n; n = n->m_pNext) {
        if (!AddTail(n->m_pObject))
            return false;
    }
    return true;
}

// -- mtype --------------------------------------------------------------------------------------

// 0x1008b420
CMediaType::CMediaType()
{
    InitMediaType();
}

// 0x1008b3e0
CMediaType::~CMediaType()
{
    FreeMediaType(*this);
}

// 0x1008b440
HRESULT CMediaType::Set(const AM_MEDIA_TYPE &type)
{
    if (&type != this) {
        FreeMediaType(*this);
        *static_cast<AM_MEDIA_TYPE *>(this) = type;
        if (type.cbFormat) {
            pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(type.cbFormat));
            if (!pbFormat) {
                cbFormat = 0;
                return E_OUTOFMEMORY;
            }
            memcpy(pbFormat, type.pbFormat, cbFormat);
        }
        if (pUnk)
            pUnk->AddRef();
    }
    return S_OK;
}

// 0x1008b4e0
void CMediaType::InitMediaType()
{
    memset(static_cast<AM_MEDIA_TYPE *>(this), 0, sizeof(AM_MEDIA_TYPE));
    lSampleSize = 1;
    bFixedSizeSamples = TRUE;
}

// 0x1008b500
BOOL CMediaType::IsPartiallySpecified() const
{
    return majortype == GUID_NULL || formattype == GUID_NULL;
}

// 0x1008b540: equal where `partial` is specified (GUID_NULL fields match anything).
BOOL CMediaType::MatchesPartial(const CMediaType *partial) const
{
    if (partial->majortype != GUID_NULL && majortype != partial->majortype)
        return FALSE;
    if (partial->subtype != GUID_NULL && subtype != partial->subtype)
        return FALSE;
    if (partial->formattype != GUID_NULL) {
        if (formattype != partial->formattype)
            return FALSE;
        if (cbFormat != partial->cbFormat)
            return FALSE;
        if (cbFormat && memcmp(pbFormat, partial->pbFormat, cbFormat) != 0)
            return FALSE;
    }
    return TRUE;
}

void FreeMediaType(AM_MEDIA_TYPE &type)
{
    if (type.cbFormat) {
        CoTaskMemFree(type.pbFormat);
        type.cbFormat = 0;
        type.pbFormat = nullptr;
    }
    if (type.pUnk) {
        type.pUnk->Release();
        type.pUnk = nullptr;
    }
}

// 0x1008b610
void DeleteMediaType(AM_MEDIA_TYPE *type)
{
    if (type) {
        FreeMediaType(*type);
        CoTaskMemFree(type);
    }
}

// 0x1008b660
HRESULT CopyMediaType(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src)
{
    *dst = *src;
    if (src->cbFormat) {
        dst->pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(src->cbFormat));
        if (!dst->pbFormat) {
            dst->cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(dst->pbFormat, src->pbFormat, dst->cbFormat);
    }
    if (dst->pUnk)
        dst->pUnk->AddRef();
    return S_OK;
}

}  // namespace ls3d::dshow
