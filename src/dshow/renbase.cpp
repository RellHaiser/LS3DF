// DirectShow base classes: CBaseRenderer, CRendererInputPin, CBaseVideoRenderer (renbase,
// 0x10086530 - 0x10088f50). The renderer holds one sample at a time, waits on the reference clock
// until it is due and hands it to DoRenderSample; the video renderer adds frame dropping and
// quality messages.
#include "dshow/baseclasses.h"

#include <mmsystem.h>
#include <new>

namespace ls3d::dshow {

namespace {
const DWORD kRenderTimeout = 10000;         // ms between checks while waiting for a sample's time
const LONG kDeliveryWait = 50;              // ms: end of stream sooner than this is sent at once
const UINT kTimerResolution = 10;
const int kAvgPeriod = 4;

// 0x100877a0
void CALLBACK EndOfStreamTimer(UINT, UINT, DWORD_PTR user, DWORD_PTR, DWORD_PTR)
{
    reinterpret_cast<CBaseRenderer *>(user)->TimerCallback();
}

// Integer square root (three Newton steps from a power of two).
int isqrt(int x)
{
    int s = 1;
    if (x > 0x40000000)
        return 0x8000;
    while (s * s < x)
        s = 2 * s;
    if (x == 0)
        return 0;
    s = (s * s + x) / (2 * s);
    if (s >= 0)
        s = (s * s + x) / (2 * s);
    if (s >= 0)
        s = (s * s + x) / (2 * s);
    return s;
}
}  // namespace

// -- CBaseRenderer ------------------------------------------------------------------------------

// 0x10086530
CBaseRenderer::CBaseRenderer(REFCLSID renderClass, const TCHAR *name, IUnknown *outer, HRESULT *)
    : CBaseFilter(name, outer, &m_InterfaceLock, renderClass), m_pPosition(nullptr), m_RenderEvent(FALSE),
      m_ThreadSignal(TRUE), m_evComplete(TRUE), m_bAbort(FALSE), m_bStreaming(FALSE), m_dwAdvise(0),
      m_pMediaSample(nullptr), m_bEOS(FALSE), m_bEOSDelivered(FALSE), m_pInputPin(nullptr), m_pQSink(nullptr),
      m_bRepaintStatus(TRUE), m_bInReceive(FALSE), m_SignalTime(0), m_EndOfStreamTimer(0)
{
    Ready();
}

// 0x10086650
CBaseRenderer::~CBaseRenderer()
{
    CBaseRenderer::StopStreaming();
    CBaseRenderer::ClearPendingSample();
    if (m_pPosition) {
        delete m_pPosition;
        m_pPosition = nullptr;
    }
    if (m_pInputPin) {
        delete m_pInputPin;
        m_pInputPin = nullptr;
    }
}

// 0x100868b0
HRESULT __stdcall CBaseRenderer::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IMediaPosition || riid == IID_IMediaSeeking)
        return GetMediaPositionInterface(riid, ppv);
    return CBaseFilter::NonDelegatingQueryInterface(riid, ppv);
}

// 0x10086780: the position pass-through, created on first use.
HRESULT CBaseRenderer::GetMediaPositionInterface(REFIID riid, void **ppv)
{
    CAutoLock lock(&m_ObjectCreationLock);
    if (m_pPosition)
        return m_pPosition->NonDelegatingQueryInterface(riid, ppv);
    HRESULT hr = NOERROR;
    m_pPosition = new (std::nothrow) CRendererPosPassThru(nullptr, CBaseFilter::GetOwner(), &hr, GetPin(0));
    if (!m_pPosition)
        return E_OUTOFMEMORY;
    if (FAILED(hr)) {
        delete m_pPosition;
        m_pPosition = nullptr;
        return E_NOINTERFACE;
    }
    return GetMediaPositionInterface(riid, ppv);
}

// 0x10086900
HRESULT CBaseRenderer::SourceThreadCanWait(BOOL canWait)
{
    if (canWait == TRUE)
        m_ThreadSignal.Reset();
    else
        m_ThreadSignal.Set();
    return NOERROR;
}

// 0x10086930: until the clock advise fires (S_OK) or the source thread is released.
HRESULT CBaseRenderer::WaitForRenderTime()
{
    HANDLE objects[] = {m_ThreadSignal, m_RenderEvent};
    DWORD result = WAIT_TIMEOUT;
    OnWaitStart();
    while (result == WAIT_TIMEOUT)
        result = WaitForMultipleObjects(2, objects, FALSE, kRenderTimeout);
    OnWaitEnd();
    if (result == WAIT_OBJECT_0)
        return VFW_E_STATE_CHANGED;
    m_dwAdvise = 0;                         // SignalTimerFired
    return NOERROR;
}

// 0x100869f0: a pause completes once a sample has arrived (or none can come).
HRESULT CBaseRenderer::CompleteStateChange(FILTER_STATE oldState)
{
    if (!m_pInputPin->IsConnected()) {
        Ready();
        return S_OK;
    }
    if (m_bEOS == TRUE) {
        Ready();
        return S_OK;
    }
    if (HaveCurrentSample() == TRUE && oldState != State_Stopped) {
        Ready();
        return S_OK;
    }
    NotReady();
    return S_FALSE;
}

// 0x100869a0
HRESULT __stdcall CBaseRenderer::GetState(DWORD msecs, FILTER_STATE *state)
{
    if (!state)
        return E_POINTER;
    if (WaitDispatchingMessages(m_evComplete, msecs) == WAIT_TIMEOUT) {
        *state = m_State;
        return VFW_S_STATE_INTERMEDIATE;
    }
    *state = m_State;
    return NOERROR;
}

void CBaseRenderer::WaitForReceiveToComplete()
{
    for (;;) {
        if (!m_bInReceive)
            break;
        MSG msg;
        PeekMessageA(&msg, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE);
        Sleep(1);
    }
    // A message posted to the thread meanwhile may have been missed by its waits.
    if (HIWORD(GetQueueStatus(QS_POSTMESSAGE)) & QS_POSTMESSAGE)
        PostThreadMessageA(GetCurrentThreadId(), WM_NULL, 0, 0);
}

void CBaseRenderer::ResetEndOfStreamTimer()
{
    if (m_EndOfStreamTimer) {
        timeKillEvent(m_EndOfStreamTimer);
        m_EndOfStreamTimer = 0;
    }
}

// 0x10086a60
HRESULT __stdcall CBaseRenderer::Stop()
{
    CAutoLock lock(&m_InterfaceLock);
    if (m_State == State_Stopped)
        return NOERROR;
    if (!m_pInputPin->IsConnected()) {
        m_State = State_Stopped;
        return NOERROR;
    }
    CBaseFilter::Stop();
    // Release a source thread waiting in GetBuffer.
    if (m_pInputPin->Allocator())
        m_pInputPin->Allocator()->Decommit();
    SetRepaintStatus(TRUE);
    StopStreaming();
    SourceThreadCanWait(FALSE);
    ResetEndOfStream();
    CancelNotification();
    Ready();
    WaitForReceiveToComplete();
    m_bAbort = FALSE;
    return NOERROR;
}

// 0x10086bd0
HRESULT __stdcall CBaseRenderer::Pause()
{
    CAutoLock lock(&m_InterfaceLock);
    const FILTER_STATE oldState = m_State;
    if (m_State == State_Paused)
        return CompleteStateChange(State_Paused);
    if (!m_pInputPin->IsConnected()) {
        m_State = State_Paused;
        return CompleteStateChange(State_Paused);
    }
    const HRESULT hr = CBaseFilter::Pause();
    if (FAILED(hr))
        return hr;
    SetRepaintStatus(TRUE);
    StopStreaming();
    SourceThreadCanWait(TRUE);
    CancelNotification();
    ResetEndOfStreamTimer();
    if (m_pInputPin->Allocator())
        m_pInputPin->Allocator()->Commit();
    if (oldState == State_Stopped) {
        m_bAbort = FALSE;
        ClearPendingSample();
    }
    return CompleteStateChange(oldState);
}

// 0x10086d20
HRESULT __stdcall CBaseRenderer::Run(REFERENCE_TIME start)
{
    CAutoLock lock(&m_InterfaceLock);
    const FILTER_STATE oldState = m_State;
    if (m_State == State_Running)
        return NOERROR;
    if (!m_pInputPin->IsConnected()) {
        NotifyEvent(EC_COMPLETE, S_OK, reinterpret_cast<LONG_PTR>(static_cast<IBaseFilter *>(this)));
        m_State = State_Running;
        return NOERROR;
    }
    Ready();
    const HRESULT hr = CBaseFilter::Run(start);
    if (FAILED(hr))
        return hr;
    SourceThreadCanWait(TRUE);
    SetRepaintStatus(FALSE);
    if (m_pInputPin->Allocator())
        m_pInputPin->Allocator()->Commit();
    if (oldState == State_Stopped) {
        m_bAbort = FALSE;
        ClearPendingSample();
    }
    return StartStreaming();
}

// 0x10086e90
CBasePin *CBaseRenderer::GetPin(int n)
{
    CAutoLock lock(&m_ObjectCreationLock);
    if (n != 0)
        return nullptr;
    if (!m_pInputPin) {
        HRESULT hr = NOERROR;
        m_pInputPin = new (std::nothrow) CRendererInputPin(this, &hr, L"In");
        if (!m_pInputPin)
            return nullptr;
        if (FAILED(hr)) {
            delete m_pInputPin;
            m_pInputPin = nullptr;
            return nullptr;
        }
    }
    return m_pInputPin;
}

// 0x10086fd0
HRESULT __stdcall CBaseRenderer::FindPin(LPCWSTR id, IPin **pin)
{
    if (!pin)
        return E_POINTER;
    if (lstrcmpWInternal(id, L"In") != 0) {
        *pin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    CBasePin *p = GetPin(0);
    *pin = p;
    // Fix: the original called AddRef through the null pointer when the pin could not be created.
    if (!p)
        return E_OUTOFMEMORY;
    p->AddRef();
    return NOERROR;
}

// 0x10087040: an end of stream with no sample left is delivered (now when streaming).
HRESULT CBaseRenderer::EndOfStream()
{
    if (m_State == State_Stopped)
        return NOERROR;
    m_bEOS = TRUE;
    if (m_pMediaSample)
        return NOERROR;
    Ready();
    if (m_bStreaming)
        SendEndOfStream();
    return NOERROR;
}

// 0x10087080
HRESULT CBaseRenderer::BeginFlush()
{
    if (m_State == State_Paused)
        NotReady();
    SourceThreadCanWait(FALSE);
    CancelNotification();
    ClearPendingSample();
    WaitForReceiveToComplete();
    return NOERROR;
}

// 0x10087110
HRESULT CBaseRenderer::EndFlush()
{
    if (m_pPosition)
        m_pPosition->ResetMediaTime();
    SourceThreadCanWait(TRUE);
    return NOERROR;
}

// 0x10087130
HRESULT CBaseRenderer::CompleteConnect(IPin *)
{
    m_bAbort = FALSE;
    if (m_State == State_Running) {
        const HRESULT hr = StartStreaming();
        if (FAILED(hr))
            return hr;
        SetRepaintStatus(FALSE);
    } else {
        SetRepaintStatus(TRUE);
    }
    return NOERROR;
}

// 0x10087180
HRESULT CBaseRenderer::Inactive()
{
    if (m_pPosition)
        m_pPosition->ResetMediaTime();
    ClearPendingSample();
    return NOERROR;
}

// 0x100871a0
HRESULT CBaseRenderer::BreakConnect()
{
    if (m_pQSink) {
        m_pQSink->Release();
        m_pQSink = nullptr;
    }
    if (!m_pInputPin->IsConnected())
        return S_FALSE;
    if (m_State != State_Stopped && !m_pInputPin->CanReconnectWhenActive())
        return VFW_E_NOT_STOPPED;
    SetRepaintStatus(FALSE);
    ResetEndOfStream();
    ClearPendingSample();
    m_bAbort = FALSE;
    if (m_State == State_Running)
        StopStreaming();
    return NOERROR;
}

// 0x10087220: S_OK draws at once (no time stamps or no clock), S_FALSE waits for the time.
HRESULT CBaseRenderer::GetSampleTimes(IMediaSample *sample, REFERENCE_TIME *start, REFERENCE_TIME *end)
{
    if (SUCCEEDED(sample->GetTime(start, end))) {
        if (*end < *start)
            return VFW_E_START_TIME_AFTER_END;
        if (m_pClock)
            return ShouldDrawSampleNow(sample, start, end);
    }
    return S_OK;
}

// 0x10087290
HRESULT CBaseRenderer::CancelNotification()
{
    const DWORD_PTR advise = m_dwAdvise;
    if (m_dwAdvise) {
        m_pClock->Unadvise(m_dwAdvise);
        m_dwAdvise = 0;
    }
    m_RenderEvent.Reset();
    return advise ? S_OK : S_FALSE;
}

// 0x100872c0: the render event at once, or a clock advise at the sample's start time.
BOOL CBaseRenderer::ScheduleSample(IMediaSample *sample)
{
    if (!sample)
        return FALSE;
    REFERENCE_TIME start, end;
    HRESULT hr = GetSampleTimes(sample, &start, &end);
    if (FAILED(hr))
        return FALSE;
    if (hr == S_OK) {
        SetEvent(m_RenderEvent);
        return TRUE;
    }
    hr = m_pClock->AdviseTime(m_tStart.m_time, start, reinterpret_cast<HEVENT>(static_cast<HANDLE>(m_RenderEvent)),
                              &m_dwAdvise);
    return SUCCEEDED(hr);
}

// 0x10087350
HRESULT CBaseRenderer::Render(IMediaSample *sample)
{
    if (!sample)
        return S_FALSE;
    if (!m_bStreaming)
        return S_FALSE;
    OnRenderStart(sample);
    DoRenderSample(sample);
    OnRenderEnd(sample);
    return NOERROR;
}

// 0x100873a0
BOOL CBaseRenderer::HaveCurrentSample()
{
    CAutoLock lock(&m_RendererLock);
    return m_pMediaSample != nullptr;
}

// 0x100873d0
IMediaSample *CBaseRenderer::GetCurrentSample()
{
    CAutoLock lock(&m_RendererLock);
    if (m_pMediaSample)
        m_pMediaSample->AddRef();
    return m_pMediaSample;
}

// 0x10087730
HRESULT CBaseRenderer::ClearPendingSample()
{
    CAutoLock lock(&m_RendererLock);
    if (m_pMediaSample) {
        m_pMediaSample->Release();
        m_pMediaSample = nullptr;
    }
    return NOERROR;
}

// 0x10087430: checks and keeps the sample, schedules it when streaming.
HRESULT CBaseRenderer::PrepareReceive(IMediaSample *sample)
{
    CAutoLock lock(&m_InterfaceLock);
    m_bInReceive = TRUE;
    HRESULT hr = m_pInputPin->CBaseInputPin::Receive(sample);
    if (hr != NOERROR) {
        m_bInReceive = FALSE;
        return E_FAIL;
    }
    if (m_pInputPin->SampleProps()->pMediaType) {
        hr = m_pInputPin->SetMediaType(static_cast<CMediaType *>(m_pInputPin->SampleProps()->pMediaType));
        if (FAILED(hr)) {
            m_bInReceive = FALSE;
            return hr;
        }
    }
    CAutoLock sampleLock(&m_RendererLock);
    if (m_pMediaSample || m_bEOS || m_bAbort) {
        Ready();
        m_bInReceive = FALSE;
        return E_UNEXPECTED;
    }
    if (m_pPosition)
        m_pPosition->RegisterMediaTime(sample);
    if (m_bStreaming == TRUE && ScheduleSample(sample) == FALSE) {
        m_bInReceive = FALSE;
        return VFW_E_SAMPLE_REJECTED;
    }
    m_SignalTime = m_pInputPin->SampleProps()->tStop;
    m_pMediaSample = sample;
    m_pMediaSample->AddRef();
    if (m_bStreaming == FALSE)
        SetRepaintStatus(TRUE);
    return NOERROR;
}

// 0x100875d0: the source thread waits here until the sample is due, then it is rendered.
HRESULT CBaseRenderer::Receive(IMediaSample *sample)
{
    HRESULT hr = PrepareReceive(sample);
    if (FAILED(hr))
        return hr == VFW_E_SAMPLE_REJECTED ? NOERROR : hr;
    if (m_State == State_Paused) {
        PrepareRender();
        m_bInReceive = FALSE;
        {
            CAutoLock lock(&m_InterfaceLock);
            if (m_State == State_Stopped)
                return NOERROR;
            m_bInReceive = TRUE;
            CAutoLock sampleLock(&m_RendererLock);
            OnReceiveFirstSample(sample);
        }
        Ready();
    }
    hr = WaitForRenderTime();
    if (FAILED(hr)) {
        m_bInReceive = FALSE;
        return NOERROR;
    }
    PrepareRender();
    m_bInReceive = FALSE;
    CAutoLock lock(&m_InterfaceLock);
    if (m_State == State_Stopped)
        return NOERROR;
    CAutoLock sampleLock(&m_RendererLock);
    Render(m_pMediaSample);
    ClearPendingSample();
    SendEndOfStream();
    CancelNotification();
    return NOERROR;
}

// 0x10087810: EC_COMPLETE at the last sample's end time (by a timer if it is 50 ms or more away).
HRESULT CBaseRenderer::SendEndOfStream()
{
    if (m_bEOS == FALSE || m_bEOSDelivered || m_EndOfStreamTimer)
        return NOERROR;
    if (!m_pClock)
        return NotifyEndOfStream();
    const REFERENCE_TIME signal = m_tStart.m_time + m_SignalTime;
    REFERENCE_TIME now;
    m_pClock->GetTime(&now);
    const LONG delay = static_cast<LONG>((signal - now) / 10000);
    if (delay < kDeliveryWait)
        return NotifyEndOfStream();
    m_EndOfStreamTimer = CompatibleTimeSetEvent(static_cast<UINT>(delay), kTimerResolution, EndOfStreamTimer,
                                                reinterpret_cast<DWORD_PTR>(this), TIME_ONESHOT);
    if (m_EndOfStreamTimer == 0)
        return NotifyEndOfStream();
    return NOERROR;
}

// 0x100877b0
void CBaseRenderer::TimerCallback()
{
    CAutoLock lock(&m_RendererLock);
    if (m_EndOfStreamTimer) {
        m_EndOfStreamTimer = 0;
        SendEndOfStream();
    }
}

// 0x100878d0
HRESULT CBaseRenderer::NotifyEndOfStream()
{
    CAutoLock lock(&m_RendererLock);
    if (m_bStreaming == FALSE)
        return NOERROR;
    m_EndOfStreamTimer = 0;
    if (m_pPosition)
        m_pPosition->EOS();
    m_bEOSDelivered = TRUE;
    return NotifyEvent(EC_COMPLETE, S_OK, reinterpret_cast<LONG_PTR>(static_cast<IBaseFilter *>(this)));
}

// 0x10087970
HRESULT CBaseRenderer::ResetEndOfStream()
{
    ResetEndOfStreamTimer();
    CAutoLock lock(&m_RendererLock);
    m_bEOS = FALSE;
    m_bEOSDelivered = FALSE;
    m_SignalTime = 0;
    return NOERROR;
}

// 0x100879c0
HRESULT CBaseRenderer::StartStreaming()
{
    CAutoLock lock(&m_RendererLock);
    if (m_bStreaming == TRUE)
        return NOERROR;
    m_bStreaming = TRUE;
    timeBeginPeriod(1);
    OnStartStreaming();
    if (!m_pMediaSample)
        return SendEndOfStream();
    if (!ScheduleSample(m_pMediaSample))
        m_RenderEvent.Set();
    return NOERROR;
}

// 0x10087a70
HRESULT CBaseRenderer::StopStreaming()
{
    CAutoLock lock(&m_RendererLock);
    m_bEOSDelivered = FALSE;
    if (m_bStreaming == TRUE) {
        m_bStreaming = FALSE;
        OnStopStreaming();
        timeEndPeriod(1);
    }
    return NOERROR;
}

// 0x10087ae0
void CBaseRenderer::SetRepaintStatus(BOOL repaint)
{
    CAutoLock lock(&m_RendererLock);
    m_bRepaintStatus = repaint;
}

// -- CRendererInputPin --------------------------------------------------------------------------

CRendererInputPin::CRendererInputPin(CBaseRenderer *renderer, HRESULT *phr, LPCWSTR name)
    : CBaseInputPin(nullptr, renderer, &renderer->m_InterfaceLock, phr, name), m_pRenderer(renderer)
{
}

// 0x10087b50
HRESULT __stdcall CRendererInputPin::EndOfStream()
{
    CAutoLock lock(&m_pRenderer->m_InterfaceLock);
    CAutoLock sampleLock(&m_pRenderer->m_RendererLock);
    HRESULT hr = CheckStreaming();
    if (hr != NOERROR)
        return hr;
    hr = m_pRenderer->EndOfStream();
    if (SUCCEEDED(hr))
        hr = CBaseInputPin::EndOfStream();
    return hr;
}

// 0x10087bf0
HRESULT __stdcall CRendererInputPin::BeginFlush()
{
    CAutoLock lock(&m_pRenderer->m_InterfaceLock);
    {
        CAutoLock sampleLock(&m_pRenderer->m_RendererLock);
        CBaseInputPin::BeginFlush();
        m_pRenderer->BeginFlush();
    }
    return m_pRenderer->ResetEndOfStream();
}

// 0x10087c90
HRESULT __stdcall CRendererInputPin::EndFlush()
{
    CAutoLock lock(&m_pRenderer->m_InterfaceLock);
    CAutoLock sampleLock(&m_pRenderer->m_RendererLock);
    HRESULT hr = m_pRenderer->EndFlush();
    if (SUCCEEDED(hr))
        hr = CBaseInputPin::EndFlush();
    return hr;
}

// 0x10087d30: an error while running (not stopping / flushing / aborting) aborts the graph.
HRESULT __stdcall CRendererInputPin::Receive(IMediaSample *sample)
{
    const HRESULT hr = m_pRenderer->Receive(sample);
    if (FAILED(hr)) {
        CAutoLock lock(&m_pRenderer->m_InterfaceLock);
        if (!IsStopped() && !IsFlushing() && !m_pRenderer->m_bAbort && !m_bRunTimeError) {
            m_pRenderer->NotifyEvent(EC_ERRORABORT, hr, 0);
            {
                CAutoLock sampleLock(&m_pRenderer->m_RendererLock);
                if (m_pRenderer->IsStreaming() && !m_pRenderer->m_bEOSDelivered)
                    m_pRenderer->NotifyEndOfStream();
            }
            m_bRunTimeError = true;
        }
    }
    return hr;
}

// 0x10087e80
HRESULT CRendererInputPin::BreakConnect()
{
    const HRESULT hr = m_pRenderer->BreakConnect();
    if (FAILED(hr))
        return hr;
    return CBaseInputPin::BreakConnect();
}

// 0x10087ea0
HRESULT CRendererInputPin::CompleteConnect(IPin *receivePin)
{
    const HRESULT hr = m_pRenderer->CompleteConnect(receivePin);
    if (FAILED(hr))
        return hr;
    return CBaseInputPin::CompleteConnect(receivePin);
}

// 0x10087ed0
HRESULT __stdcall CRendererInputPin::QueryId(LPWSTR *id)
{
    if (!id)
        return E_POINTER;
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(8));
    if (!*id)
        return E_OUTOFMEMORY;
    lstrcpyWInternal(*id, L"In");
    return NOERROR;
}

// 0x10087f10
HRESULT CRendererInputPin::CheckMediaType(const CMediaType *pmt)
{
    return m_pRenderer->CheckMediaType(pmt);
}

// 0x10087f20
HRESULT CRendererInputPin::Active()
{
    return m_pRenderer->Active();
}

// 0x10087f30
HRESULT CRendererInputPin::Inactive()
{
    m_bRunTimeError = false;
    return m_pRenderer->Inactive();
}

// 0x10087f40
HRESULT CRendererInputPin::SetMediaType(const CMediaType *pmt)
{
    const HRESULT hr = CBaseInputPin::SetMediaType(pmt);
    if (FAILED(hr))
        return hr;
    return m_pRenderer->SetMediaType(pmt);
}

// -- CBaseVideoRenderer -------------------------------------------------------------------------

// 0x10087f70
CBaseVideoRenderer::CBaseVideoRenderer(REFCLSID renderClass, const TCHAR *name, IUnknown *outer, HRESULT *phr)
    : CBaseRenderer(renderClass, name, outer, phr), m_bSupplierHandlingQuality(FALSE), m_cFramesDropped(0),
      m_cFramesDrawn(0)
{
    CBaseVideoRenderer::ResetStreamingTimes();
}

// 0x10088de0
HRESULT __stdcall CBaseVideoRenderer::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == IID_IQualProp)
        return GetInterface(static_cast<IQualProp *>(this), ppv);
    if (riid == IID_IQualityControl)
        return GetInterface(static_cast<IQualityControl *>(this), ppv);
    return CBaseRenderer::NonDelegatingQueryInterface(riid, ppv);
}

// 0x10088ea0: leaving the graph reports the window destroyed.
HRESULT __stdcall CBaseVideoRenderer::JoinFilterGraph(IFilterGraph *graph, LPCWSTR name)
{
    if (!graph && m_pGraph) {
        IBaseFilter *filter;
        static_cast<IBaseFilter *>(this)->QueryInterface(IID_IBaseFilter, reinterpret_cast<void **>(&filter));
        NotifyEvent(EC_WINDOW_DESTROYED, reinterpret_cast<LONG_PTR>(filter), 0);
        filter->Release();
    }
    return CBaseFilter::JoinFilterGraph(graph, name);
}

// 0x100880f0
HRESULT CBaseVideoRenderer::ResetStreamingTimes()
{
    m_trLastDraw = -1000;                   // as if a frame had been drawn 1 s ago
    m_tStreamingStart = static_cast<int>(timeGetTime());
    m_trRenderAvg = 0;
    m_trFrameAvg = -1;                      // "unset"
    m_trDuration = 0;
    m_trRenderLast = 0;
    m_trWaitAvg = 0;
    m_tRenderStart = 0;
    m_cFramesDrawn = 0;
    m_cFramesDropped = 0;
    m_iTotAcc = 0;
    m_iSumSqAcc = 0;
    m_iSumSqFrameTime = 0;
    m_trFrame = 0;
    m_trLate = 0;
    m_iSumFrameTime = 0;
    m_nNormal = 0;
    m_trEarliness = 0;
    m_trTarget = -300000;                   // 30 ms early
    m_trThrottle = 0;
    m_trRememberStampForPerf = 0;
    return NOERROR;
}

// 0x100881b0
HRESULT CBaseVideoRenderer::OnStartStreaming()
{
    ResetStreamingTimes();
    return NOERROR;
}

// 0x100881c0
HRESULT CBaseVideoRenderer::OnStopStreaming()
{
    m_tStreamingStart = static_cast<int>(timeGetTime()) - m_tStreamingStart;
    return NOERROR;
}

// 0x100881f0: sync offset (ms) and inter-frame time statistics.
void CBaseVideoRenderer::RecordFrameLateness(int late, int frame)
{
    int tLate = late / 10000;
    // Frames more than a second off (start-up) would spoil the statistics.
    if (tLate > 1000 || tLate < -1000) {
        if (m_cFramesDrawn <= 1)
            tLate = 0;
        else if (tLate > 0)
            tLate = 1000;
        else
            tLate = -1000;
    }
    if (m_cFramesDrawn > 1) {
        m_iTotAcc += tLate;
        m_iSumSqAcc += tLate * tLate;
    }
    if (m_cFramesDrawn > 2) {
        int tFrame = frame / 10000;
        if (tFrame > 1000 || tFrame < 0)
            tFrame = 1000;
        m_iSumSqFrameTime += tFrame * tFrame;
        m_iSumFrameTime += tFrame;
    }
    ++m_cFramesDrawn;
}

void CBaseVideoRenderer::ThrottleWait()
{
    if (m_trThrottle > 0)
        Sleep(m_trThrottle / 10000);
    else
        Sleep(0);
}

// 0x10088300
void CBaseVideoRenderer::OnDirectRender(IMediaSample *)
{
    m_trRenderAvg = 0;
    m_trRenderLast = 5000000;               // so that a mode switch does not inhibit a new average
    RecordFrameLateness(m_trLate, m_trFrame);
    ThrottleWait();
}

// 0x10088370
void CBaseVideoRenderer::OnRenderStart(IMediaSample *)
{
    RecordFrameLateness(m_trLate, m_trFrame);
    m_tRenderStart = static_cast<int>(timeGetTime());
}

// 0x100883a0: the average drawing time ignores single slow frames.
void CBaseVideoRenderer::OnRenderEnd(IMediaSample *)
{
    const int tr = (static_cast<int>(timeGetTime()) - m_tRenderStart) * 10000;
    if (tr < m_trRenderAvg * 2 || tr < 2 * m_trRenderLast)
        m_trRenderAvg = (tr + (kAvgPeriod - 1) * m_trRenderAvg) / kAvgPeriod;
    m_trRenderLast = tr;
    ThrottleWait();
}

// 0x10088420
HRESULT __stdcall CBaseVideoRenderer::SetSink(IQualityControl *sink)
{
    m_pQSink = sink;
    return NOERROR;
}

// 0x10088430: the upstream filter cannot keep up: sleep after each frame.
HRESULT __stdcall CBaseVideoRenderer::Notify(IBaseFilter *, Quality q)
{
    if (q.Proportion >= 1000)
        m_trThrottle = 0;
    else
        m_trThrottle = -330000 + 388880000 / (q.Proportion + 167);
    return NOERROR;
}

// 0x10088470
HRESULT CBaseVideoRenderer::SendQuality(REFERENCE_TIME late, REFERENCE_TIME realStream)
{
    Quality q;
    q.TimeStamp = realStream;
    if (m_trFrameAvg < 0)
        q.Type = Famine;
    else
        q.Type = m_trFrameAvg > 2 * m_trRenderAvg ? Famine : Flood;
    q.Proportion = 1000;
    if (m_trFrameAvg >= 0) {
        if (late > 0) {
            // Catch up over the next second.
            q.Proportion = 1000 - static_cast<int>(late / (UNITS / 1000));
            if (q.Proportion < 500)
                q.Proportion = 500;
        } else if (m_trWaitAvg > 20000 && late < -20000) {
            // Go faster carefully, aiming at a 2 ms wait.
            if (m_trWaitAvg >= m_trFrameAvg) {
                q.Proportion = 2000;
            } else if (m_trFrameAvg + 20000 > m_trWaitAvg) {
                // (the original's integer form: whole multiples of 1000)
                q.Proportion = m_trFrameAvg / (m_trFrameAvg + 20000 - m_trWaitAvg) * 1000;
                if (q.Proportion > 2000)
                    q.Proportion = 2000;
            } else {
                q.Proportion = 2000;
            }
        }
    }
    q.Late = late + m_trRenderAvg / 2;
    if (!m_pQSink) {
        IQualityControl *qc = nullptr;
        if (SUCCEEDED(m_pInputPin->GetConnected()->QueryInterface(IID_IQualityControl, reinterpret_cast<void **>(&qc))))
            m_pQSink = qc;
    }
    if (m_pQSink)
        return m_pQSink->Notify(static_cast<IBaseFilter *>(this), q);
    return S_FALSE;
}

// 0x100885f0: S_OK draw now, S_FALSE wait for the (adjusted) start time, E_FAIL drop the frame.
HRESULT CBaseVideoRenderer::ShouldDrawSampleNow(IMediaSample *sample, REFERENCE_TIME *start, REFERENCE_TIME *end)
{
    // About 8 ms are lost waiting for the screen refresh: draw that much earlier.
    if (*start >= 80000) {
        *start -= 80000;
        *end -= 80000;
    }
    m_trRememberStampForPerf = *start;
    REFERENCE_TIME realStream;
    m_pClock->GetTime(&realStream);
    realStream -= m_tStart.m_time;
    const int trueLate = TimeDiff(realStream - *start);
    const int late = trueLate;
    const HRESULT hr = SendQuality(late, realStream);
    m_bSupplierHandlingQuality = hr == S_OK;

    const int duration = static_cast<int>(*end - *start);
    {
        // A major change of frame rate resets the average.
        const int t = m_trDuration / 32;
        if (duration > m_trDuration + t || duration < m_trDuration - t) {
            m_trFrameAvg = duration;
            m_trDuration = duration;
        }
    }
    const BOOL justDropped = (m_bSupplierHandlingQuality && sample->IsDiscontinuity() == S_OK) || m_nNormal == -1;
    // Slide back gently from slow-machine mode (earliness is negative).
    if (late > 0)
        m_trEarliness = 0;
    else if (late >= m_trEarliness || justDropped)
        m_trEarliness = late;
    else
        m_trEarliness = m_trEarliness - m_trEarliness / 8;

    int waitAvg;
    {
        const int l = late < 0 ? -late : 0;
        waitAvg = (l + m_trWaitAvg * (kAvgPeriod - 1)) / kAvgPeriod;
    }
    int frame;
    {
        REFERENCE_TIME tr = realStream - m_trLastDraw;
        if (tr > 10000000)
            tr = 10000000;
        frame = static_cast<int>(tr);
    }

    if (3 * m_trRenderAvg <= m_trFrameAvg ||
        (m_bSupplierHandlingQuality ? late <= duration * 4 : late + late < duration) || m_trWaitAvg > 80000 ||
        realStream - m_trLastDraw > UNITS) {
        HRESULT result;
        BOOL asap = FALSE;
        if (justDropped)
            asap = TRUE;
        else if (m_trFrameAvg > duration + duration / 16 && late > -duration * 10)
            asap = TRUE;
        if (late < -9000000)
            asap = FALSE;
        if (asap) {
            m_nNormal = 0;
            m_trWaitAvg = (m_trWaitAvg * (kAvgPeriod - 1)) / kAvgPeriod;
            m_trFrameAvg = (frame + m_trFrameAvg * (kAvgPeriod - 1)) / kAvgPeriod;
            PreparePerformanceData(trueLate, frame);
            m_trLastDraw = realStream;
            if (m_trEarliness > late)
                m_trEarliness = late;
            result = S_OK;
        } else {
            ++m_nNormal;
            m_trFrameAvg = duration;
            {
                int e = m_trEarliness;
                if (e < -m_trFrameAvg)
                    e = -m_trFrameAvg;
                *start += e;
            }
            const int delay = -trueLate;
            result = delay <= 0 ? S_OK : S_FALSE;
            m_trWaitAvg = waitAvg;
            if (result == S_FALSE) {
                frame = TimeDiff(*start - m_trLastDraw);
                m_trLastDraw = *start;
            } else {
                m_trLastDraw = realStream;
            }
            const int accuracy = delay > 0 ? TimeDiff(*start - m_trRememberStampForPerf) : trueLate;
            PreparePerformanceData(accuracy, frame);
        }
        return result;
    }
    // Drop it, and draw the next one early.
    m_trWaitAvg = waitAvg;
    m_nNormal = -1;
    return E_FAIL;
}

// 0x10088a50
BOOL CBaseVideoRenderer::ScheduleSample(IMediaSample *sample)
{
    if (!CBaseRenderer::ScheduleSample(sample)) {
        ++m_cFramesDropped;
        return FALSE;
    }
    return TRUE;
}

// 0x10088ad0
HRESULT __stdcall CBaseVideoRenderer::get_FramesDroppedInRenderer(int *frames)
{
    if (!frames)
        return E_POINTER;
    CAutoLock lock(&m_InterfaceLock);
    *frames = m_cFramesDropped;
    return NOERROR;
}

// 0x10088b10
HRESULT __stdcall CBaseVideoRenderer::get_FramesDrawn(int *frames)
{
    if (!frames)
        return E_POINTER;
    CAutoLock lock(&m_InterfaceLock);
    *frames = m_cFramesDrawn;
    return NOERROR;
}

// 0x10088b50: frames per 100 s.
HRESULT __stdcall CBaseVideoRenderer::get_AvgFrameRate(int *rate)
{
    if (!rate)
        return E_POINTER;
    CAutoLock lock(&m_InterfaceLock);
    const int t = m_bStreaming ? static_cast<int>(timeGetTime()) - m_tStreamingStart : m_tStreamingStart;
    *rate = t <= 0 ? 0 : MulDiv(100000, m_cFramesDrawn, t);
    return NOERROR;
}

// 0x10088bd0
HRESULT __stdcall CBaseVideoRenderer::get_AvgSyncOffset(int *offset)
{
    if (!offset)
        return E_POINTER;
    CAutoLock lock(&m_InterfaceLock);
    if (!m_pClock) {
        *offset = 0;
        return NOERROR;
    }
    // (no statistics for the first frame)
    *offset = m_cFramesDrawn <= 1 ? 0 : static_cast<int>(m_iTotAcc / (m_cFramesDrawn - 1));
    return NOERROR;
}

// 0x10088c60: sqrt((S - T^2 / N) / (N - 1)).
HRESULT CBaseVideoRenderer::GetStdDev(int samples, int *result, LONGLONG sumSq, LONGLONG total)
{
    if (!result)
        return E_POINTER;
    CAutoLock lock(&m_InterfaceLock);
    if (!m_pClock) {
        *result = 0;
        return NOERROR;
    }
    if (samples <= 1) {
        *result = 0;
    } else {
        LONGLONG x = sumSq - llMulDiv(total, total, samples, 0);
        x = x / (samples - 1);
        *result = isqrt(static_cast<LONG>(x));
    }
    return NOERROR;
}

// 0x10088d80
HRESULT __stdcall CBaseVideoRenderer::get_DevSyncOffset(int *deviation)
{
    return GetStdDev(m_cFramesDrawn - 1, deviation, m_iSumSqAcc, m_iTotAcc);
}

// 0x10088db0
HRESULT __stdcall CBaseVideoRenderer::get_Jitter(int *jitter)
{
    return GetStdDev(m_cFramesDrawn - 2, jitter, m_iSumSqFrameTime, m_iSumFrameTime);
}

}  // namespace ls3d::dshow
