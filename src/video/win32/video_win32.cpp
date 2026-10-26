// The Windows backend of the video device (video/video.h): movies are Bink's (binkw32), graphs
// DirectShow's with the texture renderer filter. IShow's code for both (IShow.cpp, 0x10070d30 -
// 0x10073720), moved here unchanged.
#include "video/video.h"

#include <cstring>
#include <vector>

#include <dshow.h>

#include "audio/audio.h"
#include "core/debug.h"
#include "external/bink.h"
#include "video/win32/texture_renderer.h"

namespace ls3d::video {

namespace {

// A graph: the filter graph and its control interfaces (the original's IShow members +0x04 -
// +0x18), the last texture renderer added (+0x50; owned by the graph) and the sinks it was given.
struct S_graph {
    IGraphBuilder *graph = nullptr;
    IMediaControl *control = nullptr;
    IMediaSeeking *seeking = nullptr;
    IBasicAudio *audio = nullptr;
    IMediaEvent *events = nullptr;
    CTextureRenderer *renderer = nullptr;
    std::vector<FrameSink *> sinks;
};

BINK *B(Movie *movie) { return reinterpret_cast<BINK *>(movie); }
S_graph *G(Graph *graph) { return reinterpret_cast<S_graph *>(graph); }

template <class T> void SafeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// CreateTextureRenderer's scratch state, global in the original.
char g_filterName[0x100];                   // 0x100d2468
IBaseFilter *g_splitter;                    // 0x100d25b8 the "AVI Splitter" of the graph
IEnumPins *g_enumPins;                      // 0x100d25c0
IPin *g_pins[20];                           // 0x100d2568
ULONG g_pinCount;                           // 0x100d23e8
IEnumMediaTypes *g_enumTypes;               // 0x100d25bc
AM_MEDIA_TYPE *g_mediaTypes[20];            // 0x100d23f0
ULONG g_mediaTypeCount;                     // 0x100d2460

// IShow::Open's file name for DirectShow (wide, at most 250 characters).
struct S_wideName {
    explicit S_wideName(const char *fileName) { MultiByteToWideChar(CP_ACP, 0, fileName, -1, text, 250); }
    wchar_t text[250];
};

// 0x100729f0 (IShow::m_CreateCTextureRenderer): file source -> texture renderer, plus the AVI
// splitter's audio stream into a DirectSound renderer.
HRESULT CreateTextureRenderer(S_graph *g, const wchar_t *fileName, FrameSink *sink)
{
    HRESULT hr = -1;
    CTextureRenderer *r = new CTextureRenderer(&hr);
    g->renderer = r;
    if (hr < 0) {
        dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not create texture renderer object!  hr=0x%x", hr);
        return E_FAIL;
    }
    r->sink = sink;
    IGraphBuilder *graph = g->graph;
    IBaseFilter *filter = r;
    hr = graph->AddFilter(filter, L"TEXTURERENDERER");
    if (hr < 0) {
        dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not add renderer filter to graph!  hr=0x%x", hr);
        return hr;
    }
    // (the original kept the references below on its failure paths)
    IBaseFilter *source = nullptr;
    hr = graph->AddSourceFilter(fileName, L"SOURCE", &source);
    if (hr < 0) {
        dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not create source filter to graph!  hr=0x%x", hr);
        return hr;
    }
    IPin *rendererIn = nullptr;
    hr = filter->FindPin(L"In", &rendererIn);
    if (hr < 0) {
        dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not find input pin!  hr=0x%x", hr);
        source->Release();
        return hr;
    }
    IPin *sourceOut = nullptr;
    hr = source->FindPin(L"Output", &sourceOut);
    if (hr < 0) {
        dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not find output pin!  hr=0x%x", hr);
        rendererIn->Release();
        source->Release();
        return hr;
    }
    hr = graph->Connect(sourceOut, rendererIn);
    rendererIn->Release();
    sourceOut->Release();
    source->Release();
    if (hr < 0) {
        dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not connect pins!  hr=0x%x", hr);
        return hr;
    }

    IEnumFilters *filters = nullptr;
    graph->EnumFilters(&filters);
    IBaseFilter *f;
    ULONG fetched;
    while (filters->Next(1, &f, &fetched) == S_OK) {
        FILTER_INFO info;
        f->QueryFilterInfo(&info);
        WideCharToMultiByte(CP_ACP, 0, info.achName, -1, g_filterName, sizeof(g_filterName), nullptr, nullptr);
        if (!std::memcmp(g_filterName, "AVI Splitter", 13)) {
            g_splitter = f;
            f->AddRef();
        }
        info.pGraph->Release();
        f->Release();
    }
    filters->Release();
    if (g_splitter) {
        g_splitter->EnumPins(&g_enumPins);
        g_enumPins->Next(20, g_pins, &g_pinCount);
        IPin *audioPin = nullptr;
        for (ULONG i = 0; i < g_pinCount; ++i) {
            g_pins[i]->EnumMediaTypes(&g_enumTypes);
            g_enumTypes->Next(20, g_mediaTypes, &g_mediaTypeCount);
            for (ULONG j = 0; j < g_mediaTypeCount; ++j) {
                if (!audioPin && g_mediaTypes[j]->majortype == MEDIATYPE_Audio) {
                    audioPin = g_pins[i];
                    audioPin->AddRef();
                }
                ls3d::dshow::DeleteMediaType(g_mediaTypes[j]);
            }
            g_enumTypes->Release();
            g_pins[i]->Release();
        }
        g_enumPins->Release();
        if (audioPin) {
            CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                             reinterpret_cast<void **>(&r->soundRenderer));
            if (r->soundRenderer) {
                graph->AddFilter(r->soundRenderer, L"SOUNDRENDERER");
                IPin *audioIn = nullptr;
                hr = r->soundRenderer->FindPin(L"Audio Input pin (rendered)", &audioIn);
                if (hr < 0) {
                    dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not find audio input pin!  hr=0x%x", hr);
                    audioPin->Release();
                    g_splitter->Release();
                    g_splitter = nullptr;
                    return hr;
                }
                hr = graph->Connect(audioPin, audioIn);
                audioIn->Release();
                if (hr < 0) {
                    dbgPrintf("IShow::m_CreateCTextureRenderer() - ERROR! Could not connect pins!  hr=0x%x", hr);
                    audioPin->Release();
                    g_splitter->Release();
                    g_splitter = nullptr;
                    return hr;
                }
            }
            audioPin->Release();
        }
        g_splitter->Release();
        // Fix: the original kept the released pointer, used again by the next .avi whose graph
        // has no "AVI Splitter" (possibly freed by then).
        g_splitter = nullptr;
    }
    return 0;
}

class DeviceWin32 final : public Device {
public:
    // ---- movies (Bink)
    void UseSound(audio::System *system) override
    {
        BinkSetSoundSystem(BinkOpenDirectSound, reinterpret_cast<uint32_t>(audio::g_pAudio->Native(system)));
    }
    Movie *OpenMovie(const char *fileName) override { return reinterpret_cast<Movie *>(BinkOpen(fileName, 0)); }
    void CloseMovie(Movie *movie) override { BinkClose(B(movie)); }
    S_movieInfo Info(Movie *movie) override
    {
        const BINK *b = B(movie);
        return {b->width, b->height, b->frames, b->frameNum, b->rate, b->rateDiv};
    }
    void Pause(Movie *movie, bool pause) override { BinkPause(B(movie), pause ? 1 : 0); }
    void Goto(Movie *movie, uint32_t frame) override { BinkGoto(B(movie), frame, 0); }
    bool Wait(Movie *movie) override { return BinkWait(B(movie)) != 0; }
    void DecodeFrame(Movie *movie) override { BinkDoFrame(B(movie)); }
    void NextFrame(Movie *movie) override { BinkNextFrame(B(movie)); }
    void CopyFrame(Movie *movie, void *dest, int pitch, uint32_t destHeight, uint32_t x, uint32_t y, uint32_t width,
                   uint32_t height, uint32_t format) override
    {
        BinkCopyToBufferRect(B(movie), dest, pitch, destHeight, 0, 0, x, y, width, height, format);
    }
    void SetVolume(Movie *movie, int32_t volume) override { BinkSetVolume(B(movie), 0, volume); }

    // ---- graphs (DirectShow)
    Graph *CreateGraph() override
    {
        IGraphBuilder *graph = nullptr;
        CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder,
                         reinterpret_cast<void **>(&graph));
        if (!graph)
            return nullptr;
        auto *g = new S_graph;
        g->graph = graph;
        graph->QueryInterface(IID_IMediaSeeking, reinterpret_cast<void **>(&g->seeking));
        graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void **>(&g->control));
        graph->QueryInterface(IID_IMediaEvent, reinterpret_cast<void **>(&g->events));
        graph->QueryInterface(IID_IBasicAudio, reinterpret_cast<void **>(&g->audio));
        return reinterpret_cast<Graph *>(g);
    }
    void ReleaseGraph(Graph *graph) override
    {
        S_graph *g = G(graph);
        SafeRelease(g->seeking);
        SafeRelease(g->control);
        SafeRelease(g->events);
        SafeRelease(g->audio);
        SafeRelease(g->graph);
        for (FrameSink *sink : g->sinks)
            delete sink;
        delete g;
    }
    HRESULT RenderFile(Graph *graph, const char *fileName) override
    {
        const S_wideName name(fileName);
        return G(graph)->graph->RenderFile(name.text, nullptr);
    }
    HRESULT RenderToSink(Graph *graph, const char *fileName, FrameSink *sink) override
    {
        G(graph)->sinks.push_back(sink);
        const S_wideName name(fileName);
        return CreateTextureRenderer(G(graph), name.text, sink);
    }
    // (IShow::Play runs a graph only when it has one with its control interface.)
    void Run(Graph *graph) override
    {
        if (graph && G(graph)->control)
            G(graph)->control->Run();
    }
    void Stop(Graph *graph) override { G(graph)->control->Stop(); }
    void Pause(Graph *graph) override { G(graph)->control->Pause(); }
    void SetPosition(Graph *graph, int64_t position) override
    {
        LONGLONG p = position;
        G(graph)->seeking->SetPositions(&p, AM_SEEKING_AbsolutePositioning, nullptr, AM_SEEKING_NoPositioning);
    }
    int64_t GetPosition(Graph *graph) override
    {
        LONGLONG p = 0;
        G(graph)->seeking->GetCurrentPosition(&p);
        return p;
    }
    void SetVolume(Graph *graph, long volume) override { G(graph)->audio->put_Volume(volume); }
    bool NextEvent(Graph *graph, long *code) override
    {
        IMediaEvent *events = G(graph)->events;
        LONG_PTR param1, param2;
        if (events->GetEvent(code, &param1, &param2, 0) < 0)
            return false;
        events->FreeEventParams(*code, param1, param2);
        return true;
    }
    bool VideoSize(Graph *graph, int *width, int *height) override
    {
        if (!graph || !G(graph)->renderer)
            return false;
        *width = G(graph)->renderer->videoWidth;
        *height = G(graph)->renderer->videoHeight;
        return true;
    }
};

DeviceWin32 s_device;

}  // namespace

Device *g_pVideo = &s_device;

}  // namespace ls3d::video
