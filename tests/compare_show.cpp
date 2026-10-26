// Differential test of IShow (src/igraph/ishow.cpp) and the video device's Windows backend
// (src/video/win32): the original LS3DF.dll's players and the port's run the same operations,
// each side on its own fakes.
//   Ogg   the game's shortest Ogg files (the unpacked Sounds folder, test_paths.h), decoded by
//         vorbisfile.dll into fake DirectSound buffers. The stream threads are woken through
//         their notification events and waited for. The DirectSound calls, the buffers' bytes and
//         the players' fields must be the same.
//   Bink  the game's smallest movies (Gamemovie\*.bik) through binkw32.dll. Both sides' imports of
//         it point at logging wrappers: BinkWait gives both sides the same scripted answers, and
//         BinkSetSoundSystem is only logged. The Bink calls, the tiles (layout, quads and pixels
//         on the fake Direct3D device of fake_d3d8.h) and the fields must be the same.
//   AVI   uncompressed AVI files written for the test, through DirectShow and the texture
//         renderer. The graphs run without a reference clock, so every frame between the start
//         and stop positions reaches the tiles. A silent WAV goes through the plain RenderFile
//         graph. (These run in real time and the test waits for them: some seconds.)
// Not covered: PlayModal (keyboard), Draw (IGraph's render states), the external stream API
// (nothing installs one).
//
// Usage: compare_show.exe [original LS3DF.dll] [rounds] [seed]
//
// test-sources: src/igraph/*.cpp src/isnd/*.cpp src/core/debug.cpp src/core/cpu.cpp src/core/enhance.cpp src/dx/dxerr8.cpp src/i3d/ib_manager.cpp src/i3d/vb_manager.cpp src/dshow/*.cpp
// test-sources: src/gfx/d3d8/gfx_d3d8.cpp src/audio/dsound/audio_dsound.cpp src/input/dinput/input_dinput.cpp
// test-sources: src/platform/win32/platform_win32.cpp src/video/win32/*.cpp
#include <windows.h>

#include <dshow.h>
#include <float.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "../src/audio/dsound/dsound_device.h"
#include "../src/core/debug.h"
#include "../src/external/bink.h"
#include "../src/external/vorbisfile.h"
#include "../src/gfx/d3d8/d3d8_device.h"
#include "../src/i3d/driver.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/igraph_state.h"
#include "../src/igraph/ishow.h"
#include "../src/igraph/itexture.h"
#include "../src/isnd/isnd.h"
#include "fake_d3d8.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d {
namespace i3d {
I3D_driver *g_driver;
uint32_t g_renderStats[8];
void ReapplyDriverStates() {}
void DrawDebugText(int, int, const char *) {}
bool UseWBuffer() { return false; }
void InvalidateLights() {}
int InitParticleBuffers() { return 0; }
void RefreshSoundVolumes() {}
void DrawText2D(int, int, const char *, uint32_t, float) {}
}  // namespace i3d
}  // namespace ls3d

using namespace ls3d;

namespace {

uint8_t *g_base;
void *g_origIGraph;
ISND_driver *g_origDriver;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }

uint32_t Bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

std::mt19937 g_rng(1);
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<unsigned>(n)); }

int g_failures;
std::string g_step;

void Fail(const char *fmt, ...)
{
    ++g_failures;
    if (g_failures <= 30) {
        char text[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        std::printf("DIFF at %s: %s\n", g_step.c_str(), text);
    }
}

template <class T> void Check(const char *what, T a, T b)
{
    if (!(a == b))
        Fail("%s: %lld / %lld", what, static_cast<long long>(a), static_cast<long long>(b));
}

// Points `module`'s import of dll!function at `replacement`.
bool PatchImport(HMODULE module, const char *dll, const char *function, void *replacement)
{
    uint8_t *base = reinterpret_cast<uint8_t *>(module);
    const IMAGE_NT_HEADERS *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char *>(base + d->Name), dll) != 0)
            continue;
        auto *names = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                                                                : d->FirstThunk));
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto *byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(byName->Name), function) != 0)
                continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<uintptr_t>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, old, &old);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- fake DirectSound

struct World;
class FakeBuffer;

class FakeNotify : public IDirectSoundNotify {
public:
    FakeBuffer *owner = nullptr;
    HRESULT __stdcall QueryInterface(REFIID, void **out) override
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall SetNotificationPositions(DWORD count, LPCDSBPOSITIONNOTIFY positions) override;
};

class FakeBuffer : public IDirectSoundBuffer8 {
public:
    FakeBuffer(World *world, int id) : w(world), id(id) { notify.owner = this; }

    World *w;
    int id;
    std::vector<uint8_t> data;
    DWORD position = 0;
    HANDLE event = nullptr;             // the notifications' event
    FakeNotify notify;

    void Log(const char *fmt, ...);

    HRESULT __stdcall QueryInterface(REFIID iid, void **out) override
    {
        if (iid == IID_IDirectSoundBuffer8 || iid == IID_IDirectSoundBuffer || iid == IID_IUnknown) {
            Log("QI(Buffer8)");
            *out = static_cast<IDirectSoundBuffer8 *>(this);
        } else if (iid == IID_IDirectSoundNotify) {
            Log("QI(Notify)");
            *out = static_cast<IDirectSoundNotify *>(&notify);
        } else {
            Log("QI(other)");
            *out = nullptr;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
    // (References are not logged: the port releases what the original leaked, see IShow::Close.)
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall GetCaps(LPDSBCAPS) override { Log("GetCaps"); return E_NOTIMPL; }
    HRESULT __stdcall GetCurrentPosition(LPDWORD play, LPDWORD write) override;
    HRESULT __stdcall GetFormat(LPWAVEFORMATEX, DWORD, LPDWORD) override { Log("GetFormat"); return E_NOTIMPL; }
    HRESULT __stdcall GetVolume(LPLONG) override { Log("GetVolume"); return E_NOTIMPL; }
    HRESULT __stdcall GetPan(LPLONG) override { Log("GetPan"); return E_NOTIMPL; }
    HRESULT __stdcall GetFrequency(LPDWORD) override { Log("GetFrequency"); return E_NOTIMPL; }
    HRESULT __stdcall GetStatus(LPDWORD) override { Log("GetStatus"); return E_NOTIMPL; }
    HRESULT __stdcall Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) override { Log("Initialize"); return E_NOTIMPL; }
    HRESULT __stdcall Lock(DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD s1, LPVOID *p2, LPDWORD s2,
                           DWORD flags) override
    {
        Log("Lock(%lu,%lu,%d,%d,%lu)", offset, bytes, p2 != nullptr, s2 != nullptr, flags);
        const DWORD size = static_cast<DWORD>(data.size());
        if (flags & DSBLOCK_ENTIREBUFFER) {
            offset = 0;
            bytes = size;
        }
        if (offset >= size || bytes > size)
            return DSERR_INVALIDPARAM;
        *p1 = data.data() + offset;
        *s1 = offset + bytes > size ? size - offset : bytes;
        if (p2)
            *p2 = offset + bytes > size ? data.data() : nullptr;
        if (s2)
            *s2 = offset + bytes > size ? offset + bytes - size : 0;
        return DS_OK;
    }
    HRESULT __stdcall Play(DWORD reserved, DWORD priority, DWORD flags) override
    {
        Log("Play(%lu,%lu,%08lX)", reserved, priority, flags);
        return DS_OK;
    }
    HRESULT __stdcall SetCurrentPosition(DWORD p) override
    {
        Log("SetCurrentPosition(%lu)", p);
        position = p;
        return DS_OK;
    }
    HRESULT __stdcall SetFormat(LPCWAVEFORMATEX) override { Log("SetFormat"); return E_NOTIMPL; }
    HRESULT __stdcall SetVolume(LONG v) override
    {
        Log("SetVolume(%ld)", v);
        return DS_OK;
    }
    HRESULT __stdcall SetPan(LONG) override { Log("SetPan"); return E_NOTIMPL; }
    HRESULT __stdcall SetFrequency(DWORD) override { Log("SetFrequency"); return E_NOTIMPL; }
    HRESULT __stdcall Stop() override
    {
        Log("Stop");
        return DS_OK;
    }
    HRESULT __stdcall Unlock(LPVOID p1, DWORD s1, LPVOID p2, DWORD s2) override
    {
        Log("Unlock(%ld,%lu,%d,%lu)", static_cast<long>(static_cast<uint8_t *>(p1) - data.data()), s1, p2 != nullptr,
            s2);
        return DS_OK;
    }
    HRESULT __stdcall Restore() override { Log("Restore"); return E_NOTIMPL; }
    HRESULT __stdcall SetFX(DWORD, LPDSEFFECTDESC, LPDWORD) override { Log("SetFX"); return E_NOTIMPL; }
    HRESULT __stdcall AcquireResources(DWORD, DWORD, LPDWORD) override { Log("AcquireResources"); return E_NOTIMPL; }
    HRESULT __stdcall GetObjectInPath(REFGUID, DWORD, REFGUID, LPVOID *) override
    {
        Log("GetObjectInPath");
        return E_NOTIMPL;
    }
};

class FakeDSound : public IDirectSound8 {
public:
    World *w = nullptr;
    HRESULT __stdcall QueryInterface(REFIID, void **out) override
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN outer) override;
    HRESULT __stdcall GetCaps(LPDSCAPS) override { return E_NOTIMPL; }
    HRESULT __stdcall DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER *) override { return E_NOTIMPL; }
    HRESULT __stdcall SetCooperativeLevel(HWND, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall Compact() override { return E_NOTIMPL; }
    HRESULT __stdcall GetSpeakerConfig(LPDWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetSpeakerConfig(DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall Initialize(LPCGUID) override { return E_NOTIMPL; }
    HRESULT __stdcall VerifyCertification(LPDWORD) override { return E_NOTIMPL; }
};

// One side's DirectSound: its calls (from the main thread and the stream thread) and buffers.
struct World {
    std::mutex lock;
    std::vector<std::string> log;
    std::vector<FakeBuffer *> buffers;
    FakeDSound ds;
    std::atomic<int> positionReads{0};

    World() { ds.w = this; }
    void VLog(const char *prefix, const char *fmt, va_list args)
    {
        char text[512];
        std::vsnprintf(text, sizeof text, fmt, args);
        std::lock_guard<std::mutex> guard(lock);
        log.push_back(std::string(prefix) + text);
    }
    // A buffer by one of its interfaces (the players hold the sound device's handles, which are
    // the fakes' interfaces); -1 for none.
    FakeBuffer *Find(const void *p) const
    {
        for (FakeBuffer *b : buffers) {
            if (p == static_cast<const IDirectSoundBuffer8 *>(b) || p == static_cast<const IDirectSoundNotify *>(&b->notify))
                return b;
        }
        return nullptr;
    }
    int IdOf(const void *p) const
    {
        const FakeBuffer *b = Find(p);
        return b ? b->id : p ? -2 : -1;
    }
};

void FakeBuffer::Log(const char *fmt, ...)
{
    char prefix[16];
    std::snprintf(prefix, sizeof prefix, "B%d.", id);
    va_list args;
    va_start(args, fmt);
    w->VLog(prefix, fmt, args);
    va_end(args);
}

HRESULT FakeBuffer::GetCurrentPosition(LPDWORD play, LPDWORD write)
{
    Log("GetCurrentPosition(%d,%d)", play != nullptr, write != nullptr);
    if (play)
        *play = position;
    if (write)
        *write = position;
    ++w->positionReads;
    return DS_OK;
}

HRESULT FakeNotify::SetNotificationPositions(DWORD count, LPCDSBPOSITIONNOTIFY positions)
{
    std::string offsets;
    bool oneEvent = true;
    for (DWORD i = 0; i < count; ++i) {
        offsets += std::to_string(positions[i].dwOffset) + ",";
        oneEvent &= positions[i].hEventNotify == positions[0].hEventNotify && positions[i].hEventNotify;
    }
    owner->Log("Notify.SetNotificationPositions(%lu,%s%s)", count, offsets.c_str(), oneEvent ? "one event" : "events?");
    owner->event = count ? positions[0].hEventNotify : nullptr;
    return DS_OK;
}

HRESULT FakeDSound::CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN outer)
{
    const WAVEFORMATEX *f = desc->lpwfxFormat;
    char text[256];
    std::snprintf(text, sizeof text, "CreateSoundBuffer(%lu,%08lX,%lu,%d)->B%zu format(%u,%u,%lu,%lu,%u,%u)",
                  desc->dwSize, desc->dwFlags, desc->dwBufferBytes, outer != nullptr, w->buffers.size(),
                  f ? f->wFormatTag : 0, f ? f->nChannels : 0, f ? f->nSamplesPerSec : 0, f ? f->nAvgBytesPerSec : 0,
                  f ? f->nBlockAlign : 0, f ? f->wBitsPerSample : 0);
    {
        std::lock_guard<std::mutex> guard(w->lock);
        w->log.push_back(std::string("DS.") + text);
    }
    // A player makes a buffer when it opens a file, after it closed the last one: the older
    // buffers' bytes go (the original never releases a stream buffer, and a long run would run
    // out of memory).
    for (FakeBuffer *old : w->buffers)
        std::vector<uint8_t>().swap(old->data);
    FakeBuffer *b = new FakeBuffer(w, static_cast<int>(w->buffers.size()));
    b->data.assign(desc->dwBufferBytes, 0xcd);
    w->buffers.push_back(b);
    *out = b;
    return DS_OK;
}

World g_world[2];       // the original's, the port's

// ---------------------------------------------------------------- Bink, logged

// binkw32.dll's functions; both sides' imports of them point at BinkHooks<side>.
struct S_bink {
    BINK *(__stdcall *open)(const char *, uint32_t);
    void(__stdcall *close)(BINK *);
    int32_t(__stdcall *pause)(BINK *, int32_t);
    int32_t(__stdcall *copy)(BINK *, void *, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t, uint32_t);
    void(__stdcall *gotoFrame)(BINK *, uint32_t, int32_t);
    void(__stdcall *setVolume)(BINK *, uint32_t, int32_t);
    int32_t(__stdcall *doFrame)(BINK *);
    void(__stdcall *nextFrame)(BINK *);
    void *openDirectSound;
} g_bink;

struct S_binkLog {
    std::vector<std::string> lines;
    std::vector<BINK *> handles;        // by id (closed ones null)
    size_t waits = 0;                   // BinkWait calls answered

    int Id(const void *b) const
    {
        for (size_t i = handles.size(); i--;) {
            if (handles[i] == b)
                return static_cast<int>(i);
        }
        return b ? -2 : -1;
    }
    void Log(const char *fmt, ...)
    {
        char text[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        lines.push_back(text);
    }
};
S_binkLog g_binkLog[2];
std::vector<int32_t> g_waits;           // BinkWait's answers: the n-th for each side's n-th call
std::mt19937 g_waitRng(77);

template <int S> struct BinkHooks {
    static S_binkLog &L() { return g_binkLog[S]; }
    static BINK *__stdcall Open(const char *name, uint32_t flags)
    {
        BINK *b = g_bink.open(name, flags);
        if (b)
            L().handles.push_back(b);
        L().Log("BinkOpen(%s,%u)->%d", name, flags, L().Id(b));
        return b;
    }
    static void __stdcall Close(BINK *b)
    {
        const int id = L().Id(b);
        L().Log("BinkClose(%d)", id);
        if (id >= 0)
            L().handles[id] = nullptr;
        g_bink.close(b);
    }
    static int32_t __stdcall Pause(BINK *b, int32_t pause)
    {
        L().Log("BinkPause(%d,%d)", L().Id(b), pause);
        return g_bink.pause(b, pause);
    }
    static int32_t __stdcall Copy(BINK *b, void *dest, int32_t pitch, uint32_t destHeight, uint32_t destX,
                                  uint32_t destY, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags)
    {
        L().Log("BinkCopyToBufferRect(%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%08X)", L().Id(b), dest != nullptr, pitch,
                destHeight, destX, destY, x, y, w, h, flags);
        return g_bink.copy(b, dest, pitch, destHeight, destX, destY, x, y, w, h, flags);
    }
    static void __stdcall Goto(BINK *b, uint32_t frame, int32_t flags)
    {
        L().Log("BinkGoto(%d,%u,%d)", L().Id(b), frame, flags);
        g_bink.gotoFrame(b, frame, flags);
    }
    // "Not yet" one time in three, the same answers for both sides.
    static int32_t __stdcall Wait(BINK *b)
    {
        S_binkLog &l = L();
        while (g_waits.size() <= l.waits)
            g_waits.push_back(g_waitRng() % 3 == 0);
        const int32_t r = g_waits[l.waits++];
        l.Log("BinkWait(%d)->%d", l.Id(b), r);
        return r;
    }
    static void __stdcall SetVolume(BINK *b, uint32_t track, int32_t volume)
    {
        L().Log("BinkSetVolume(%d,%u,%d)", L().Id(b), track, volume);
        g_bink.setVolume(b, track, volume);
    }
    static int32_t __stdcall DoFrame(BINK *b)
    {
        L().Log("BinkDoFrame(%d)", L().Id(b));
        return g_bink.doFrame(b);
    }
    static void __stdcall NextFrame(BINK *b)
    {
        L().Log("BinkNextFrame(%d)", L().Id(b));
        g_bink.nextFrame(b);
    }
    // Only logged: Bink would play the movies' sound through the fake DirectSound.
    static int32_t __stdcall SetSoundSystem(BINKSNDSYSOPEN open, uint32_t param)
    {
        L().Log("BinkSetSoundSystem(%s,%s)", reinterpret_cast<void *>(open) == g_bink.openDirectSound ? "BinkOpenDirectSound" : "?",
                param == reinterpret_cast<uint32_t>(static_cast<IDirectSound8 *>(&g_world[S].ds)) ? "the fake DirectSound" : "?");
        return 1;
    }

    static bool Install(HMODULE module)
    {
        const struct {
            const char *name;
            void *hook;
        } hooks[] = {
            {"_BinkOpen@8", reinterpret_cast<void *>(&Open)},
            {"_BinkClose@4", reinterpret_cast<void *>(&Close)},
            {"_BinkPause@8", reinterpret_cast<void *>(&Pause)},
            {"_BinkCopyToBufferRect@44", reinterpret_cast<void *>(&Copy)},
            {"_BinkGoto@12", reinterpret_cast<void *>(&Goto)},
            {"_BinkWait@4", reinterpret_cast<void *>(&Wait)},
            {"_BinkSetVolume@12", reinterpret_cast<void *>(&SetVolume)},
            {"_BinkDoFrame@4", reinterpret_cast<void *>(&DoFrame)},
            {"_BinkNextFrame@4", reinterpret_cast<void *>(&NextFrame)},
            {"_BinkSetSoundSystem@8", reinterpret_cast<void *>(&SetSoundSystem)},
        };
        for (const auto &h : hooks) {
            if (!PatchImport(module, "binkw32.dll", h.name, h.hook)) {
                std::printf("cannot patch the import of %s\n", h.name);
                return false;
            }
        }
        return true;
    }
};

bool LoadBink()
{
    HMODULE bink = GetModuleHandleA("binkw32.dll");
    if (!bink)
        return false;
    auto get = [bink](const char *name) { return reinterpret_cast<void *>(GetProcAddress(bink, name)); };
    g_bink.open = reinterpret_cast<decltype(g_bink.open)>(get("_BinkOpen@8"));
    g_bink.close = reinterpret_cast<decltype(g_bink.close)>(get("_BinkClose@4"));
    g_bink.pause = reinterpret_cast<decltype(g_bink.pause)>(get("_BinkPause@8"));
    g_bink.copy = reinterpret_cast<decltype(g_bink.copy)>(get("_BinkCopyToBufferRect@44"));
    g_bink.gotoFrame = reinterpret_cast<decltype(g_bink.gotoFrame)>(get("_BinkGoto@12"));
    g_bink.setVolume = reinterpret_cast<decltype(g_bink.setVolume)>(get("_BinkSetVolume@12"));
    g_bink.doFrame = reinterpret_cast<decltype(g_bink.doFrame)>(get("_BinkDoFrame@4"));
    g_bink.nextFrame = reinterpret_cast<decltype(g_bink.nextFrame)>(get("_BinkNextFrame@4"));
    g_bink.openDirectSound = get("_BinkOpenDirectSound@4");
    return g_bink.open && g_bink.close && g_bink.pause && g_bink.copy && g_bink.gotoFrame && g_bink.setVolume &&
           g_bink.doFrame && g_bink.nextFrame && g_bink.openDirectSound;
}

// ---------------------------------------------------------------- the two sides

struct S_device {
    D3DFORMAT fmt16, fmt1, fmtAlpha, fmtTrue;
    int maxW, maxH;
};

// The texture formats and limits of both sides (as compare_texture's Configure).
void Configure(FakeDevice *dev, const S_device &d)
{
    Orig<IDirect3DDevice8 *>(0x100d216c) = dev;
    Orig<D3DFORMAT>(0x100d1da8) = d.fmt16;
    Orig<D3DFORMAT>(0x100d1a74) = d.fmt1;
    Orig<D3DFORMAT>(0x100d1a78) = d.fmtAlpha;
    Orig<D3DFORMAT>(0x100d1774) = d.fmtTrue;
    Orig<D3DFORMAT>(0x100d1898) = D3DFMT_UNKNOWN;
    Orig<D3DFORMAT>(0x100d1edc) = D3DFMT_UNKNOWN;
    Orig<D3DFORMAT>(0x100d1a90) = D3DFMT_UNKNOWN;
    Orig<uint32_t>(0x100d1dac) = 0;
    Orig<int>(0x100d2128) = d.maxW;
    Orig<int>(0x100d1ce8) = d.maxH;
    using namespace ls3d::ig;
    g_pDevice = dev;
    g_fmt16Bit = d.fmt16;
    g_fmt1BitAlpha = d.fmt1;
    g_fmtAlpha = d.fmtAlpha;
    g_fmtTrueColor = d.fmtTrue;
    g_fmtDxt1 = g_fmtDxt3 = g_fmtDxt5 = D3DFMT_UNKNOWN;
    g_caps = 0;
    g_maxTextureWidth = d.maxW;
    g_maxTextureHeight = d.maxH;
}

const S_device kDevices[] = {
    {D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_A8R8G8B8, 256, 256},
    {D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, 2048, 2048},
    {D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_UNKNOWN, 128, 64},
    {D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_A8R8G8B8, 64, 32},
};
const int kDeviceCount = sizeof kDevices / sizeof kDevices[0];

// The sound driver of both sides (initialized: the players use the fake DirectSound).
void SetSound(bool on)
{
    Orig<bool>(0x100d2c60) = on;
    snd::g_initialized = on;
}

IShow *OrigCreateIShow()
{
    using Fn = IShow *(__stdcall *)(void *);
    return (*reinterpret_cast<Fn **>(g_origIGraph))[0x38 / 4](g_origIGraph);
}

// A texture whose pixels go with its last reference (a long run makes thousands of tiles).
struct ShowTexture : FakeTexture {
    using FakeTexture::FakeTexture;
    void OnFinalRelease() override
    {
        for (FakeSurface *s : levels) {
            if (s->refs == 1)
                std::vector<uint8_t>().swap(s->bits);
        }
        FakeTexture::OnFinalRelease();
    }
};

struct ShowDevice : FakeDevice {
    HRESULT __stdcall CreateTexture(UINT w, UINT h, UINT levels, DWORD, D3DFORMAT fmt, D3DPOOL pool,
                                    IDirect3DTexture8 **out) override
    {
        if (fmt == D3DFMT_UNKNOWN) {
            *out = nullptr;
            return D3DERR_INVALIDCALL;
        }
        ShowTexture *t = new ShowTexture(w, h, levels, fmt, pool);
        textures.push_back(t);
        *out = t;
        return 0;
    }
};

// A player of each side doing the same things.
struct S_pair {
    IShow *a = nullptr, *b = nullptr;
    FakeDevice *device = nullptr;       // their tiles' textures

    void Create(const S_device &d)
    {
        device = new ShowDevice;
        Configure(device, d);
        a = OrigCreateIShow();
        b = g_pIGraph->CreateIShow();
    }
    // Every reference released (the last frees the objects).
    void Free()
    {
        while (a->refCount > 1 && b->refCount > 1) {
            a->Release();
            b->Release();
        }
        Check("Release", a->Release(), b->Release());
        a = b = nullptr;
    }
};

FakeTexture *Fake(const ITexture *t)
{
    return static_cast<FakeTexture *>(reinterpret_cast<IDirect3DTexture8 *>(t->deviceTexture));
}

// The vertex of a tile's quad, without the specular colour (never written: the original's is
// whatever its stack held).
bool SameVertex(const S_showVertex &x, const S_showVertex &y)
{
    return Bits(x.x) == Bits(y.x) && Bits(x.y) == Bits(y.y) && Bits(x.z) == Bits(y.z) && Bits(x.rhw) == Bits(y.rhw) &&
           x.diffuse == y.diffuse && Bits(x.u) == Bits(y.u) && Bits(x.v) == Bits(y.v);
}

uint64_t g_pixelsCompared;          // bytes of tile pixels

void CompareFields(IShow *a, IShow *b);

// Fields, tiles and movies of the two players (unless freed); the DirectSound and Bink logs.
void Compare(const S_pair &p)
{
    if (p.a && p.b)
        CompareFields(p.a, p.b);

    // DirectSound: the calls, and the bytes of the stream buffers.
    if (g_world[0].log != g_world[1].log) {
        const std::vector<std::string> &la = g_world[0].log, &lb = g_world[1].log;
        size_t k = 0;
        while (k < la.size() && k < lb.size() && la[k] == lb[k])
            ++k;
        Fail("DirectSound call %zu: %s / %s", k, k < la.size() ? la[k].c_str() : "(none)",
             k < lb.size() ? lb[k].c_str() : "(none)");
    }
    g_world[0].log.clear();
    g_world[1].log.clear();
    if (p.a && p.b) {
        const FakeBuffer *ba = g_world[0].Find(p.a->buffer), *bb = g_world[1].Find(p.b->buffer);
        if (ba && bb && ba->id == bb->id && ba->data != bb->data)
            Fail("stream buffer B%d bytes", ba->id);
    }

    // Bink: the calls.
    if (g_binkLog[0].lines != g_binkLog[1].lines) {
        const std::vector<std::string> &la = g_binkLog[0].lines, &lb = g_binkLog[1].lines;
        size_t k = 0;
        while (k < la.size() && k < lb.size() && la[k] == lb[k])
            ++k;
        Fail("Bink call %zu: %s / %s", k, k < la.size() ? la[k].c_str() : "(none)",
             k < lb.size() ? lb[k].c_str() : "(none)");
    }
    g_binkLog[0].lines.clear();
    g_binkLog[1].lines.clear();
    if (g_binkLog[0].waits != g_binkLog[1].waits) {
        Fail("BinkWait calls %zu / %zu", g_binkLog[0].waits, g_binkLog[1].waits);
        g_binkLog[1].waits = g_binkLog[0].waits;
    }
    // IGraph's list of players.
    Check("IShows", Orig<C_vector<IShow *>>(0x100d1ee0).Size(), ls3d::ig::g_shows.Size());
}

void CompareFields(IShow *a, IShow *b)
{
    std::string why;
    auto field = [&why](const char *name, long long x, long long y) {
        if (x != y) {
            char t[128];
            std::snprintf(t, sizeof t, " %s %lld / %lld", name, x, y);
            why += t;
        }
    };
    field("open", a->open, b->open);
    field("refCount", a->refCount, b->refCount);
    field("flags", a->flags, b->flags);
    field("rect.x", a->rect.x, b->rect.x);
    field("rect.y", a->rect.y, b->rect.y);
    field("rect.width", a->rect.width, b->rect.width);
    field("rect.height", a->rect.height, b->rect.height);
    field("volume", Bits(a->volume), Bits(b->volume));
    field("frameCount", a->frameCount, b->frameCount);
    field("extStream", a->extStream, b->extStream);
    field("notifyCount", a->notifyCount, b->notifyCount);
    field("writePos", a->writePos, b->writePos);
    field("bufferSize", a->bufferSize, b->bufferSize);
    field("filling", a->filling, b->filling);
    field("vorbis", a->vorbis != nullptr, b->vorbis != nullptr);
    field("thread", a->thread != nullptr, b->thread != nullptr);
    field("event", a->event != nullptr, b->event != nullptr);
    field("graph", a->graph != nullptr, b->graph != nullptr);
    // (Closed, the original keeps its file handle after a corrupt file and its stream buffer
    // interfaces; the port does not.)
    if (a->open && b->open) {
        field("file", a->file >= 0, b->file >= 0);
        field("buffer", g_world[0].IdOf(a->buffer), g_world[1].IdOf(b->buffer));
        field("notify", g_world[0].IdOf(a->notify), g_world[1].IdOf(b->notify));
    }
    field("movie", g_binkLog[0].Id(a->movie), g_binkLog[1].Id(b->movie));
    const BINK *ma = reinterpret_cast<const BINK *>(a->movie), *mb = reinterpret_cast<const BINK *>(b->movie);
    if (ma && mb) {
        field("bink.width", ma->width, mb->width);
        field("bink.height", ma->height, mb->height);
        field("bink.frames", ma->frames, mb->frames);
        field("bink.frameNum", ma->frameNum, mb->frameNum);
        field("bink.rate", ma->rate, mb->rate);
        field("bink.rateDiv", ma->rateDiv, mb->rateDiv);
    }
    field("tiles", a->tiles.Size(), b->tiles.Size());
    for (int i = 0; i < a->tiles.Size() && i < b->tiles.Size(); ++i) {
        const S_showTile &ta = a->tiles[i], &tb = b->tiles[i];
        char name[32];
        std::snprintf(name, sizeof name, "tile%d", i);
        if (ta.x != tb.x || ta.y != tb.y || ta.width != tb.width || ta.height != tb.height)
            why += std::string(" ") + name + " rectangle";
        for (int v = 0; v < 4; ++v) {
            if (!SameVertex(ta.quad[v], tb.quad[v])) {
                why += std::string(" ") + name + " quad";
                break;
            }
        }
        const ITexture *xa = ta.texture, *xb = tb.texture;
        if (xa->width != xb->width || xa->height != xb->height || xa->format != xb->format || xa->flags != xb->flags) {
            why += std::string(" ") + name + " texture";
            continue;
        }
        const std::vector<uint8_t> &pa = Fake(xa)->levels[0]->bits, &pb = Fake(xb)->levels[0]->bits;
        g_pixelsCompared += pa.size();
        if (pa != pb) {
            size_t k = 0;
            while (k < pa.size() && k < pb.size() && pa[k] == pb[k])
                ++k;
            char t[96];
            std::snprintf(t, sizeof t, " %s pixels (byte %zu of %zu)", name, k, pa.size());
            why += t;
        }
    }
    if (!why.empty())
        Fail("fields:%s", why.c_str());
}

// ---------------------------------------------------------------- operations of every player

float RandVolume()
{
    static const float kVolumes[] = {0.0f, 1e-9f, 0.001f, 0.25f, 0.5f, 1.0f, 1.5f};
    if (Rand(3))
        return kVolumes[Rand(sizeof kVolumes / sizeof kVolumes[0])];
    return static_cast<float>(Rand(100001)) / 100000.0f;
}

// One of the operations that all players have; false when `op` is none of them.
bool CommonStep(S_pair &p, int op)
{
    IShow *a = p.a, *b = p.b;
    switch (op) {
    case 0: {
        // The original returns what freeing its Ogg decoder left in EAX (1), when it had one.
        const bool decoder = a->vorbis != nullptr;
        const HRESULT ha = a->Close(), hb = b->Close();
        if (!(decoder && ha >= 0 && hb == 0))
            Check("Close", ha, hb);
        return true;
    }
    case 1: Check("Play", a->Play(), b->Play()); return true;
    case 2: Check("Stop", a->Stop(), b->Stop()); return true;
    case 3: Check("Pause", a->Pause(), b->Pause()); return true;
    case 4: {
        int ma = -7, mb = -7;
        Check("GetPosition", a->GetPosition(&ma), b->GetPosition(&mb));
        Check("GetPosition ms", ma, mb);
        return true;
    }
    case 5: Check("GetState", a->GetState(), b->GetState()); return true;
    case 6: {
        const float v = RandVolume();
        Check("SetVolume", a->SetVolume(v), b->SetVolume(v));
        Check("GetVolume", Bits(a->GetVolume()), Bits(b->GetVolume()));
        return true;
    }
    case 7: Check("IsOpen", a->IsOpen(), b->IsOpen()); return true;
    case 8: {
        S_showRect ra = {-1, -1, -1, -1}, rb = ra;
        Check("GetRect", a->GetRect(&ra), b->GetRect(&rb));
        Check("GetRect rect", std::memcmp(&ra, &rb, sizeof ra), 0);
        return true;
    }
    case 9: {
        S_showRect ra = {-1, -1, -1, -1}, rb = ra;
        Check("GetVideoSize", a->GetVideoSize(&ra), b->GetVideoSize(&rb));
        Check("GetVideoSize size", std::memcmp(&ra, &rb, sizeof ra), 0);
        return true;
    }
    case 10: {
        if (Rand(4) == 0) {
            Check("SetRect", a->SetRect(nullptr), b->SetRect(nullptr));
        } else {
            const S_showRect r = {Rand(200) - 50, Rand(200) - 50, Rand(900), Rand(700)};
            Check("SetRect", a->SetRect(&r), b->SetRect(&r));
        }
        return true;
    }
    case 11: SetSound(Rand(4) != 0); return true;
    case 12:
        if (Rand(2) || a->refCount < 2)
            Check("AddRef", a->AddRef(), b->AddRef());
        else
            Check("Release", a->Release(), b->Release());
        return true;
    case 13: {
        Check("GetTileCount", a->GetTileCount(), b->GetTileCount());
        Check("GetTiles", a->GetTiles() == a->tiles.Data(), b->GetTiles() == b->tiles.Data());
        Check("GetFrameCount", a->GetFrameCount(), b->GetFrameCount());
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------- Ogg

std::vector<std::string> g_oggs;        // the game's files, then a missing one
std::vector<int> g_oggMs;               // their lengths
std::string g_corruptOgg;
int g_corruptOpens;
long g_wakes, g_endStops;
int g_opens[2];             // files the original opened: Ogg, Bink

// Its length in milliseconds (vorbisfile's ov_time_total, which LS3DF does not import).
int OggLength(const std::string &path)
{
    using TotalFn = double(__cdecl *)(OggVorbis_File *, int);
    const auto total = reinterpret_cast<TotalFn>(GetProcAddress(GetModuleHandleA("vorbisfile.dll"), "ov_time_total"));
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f || !total) {
        if (f)
            std::fclose(f);
        return 0;
    }
    ov_callbacks callbacks = {
        [](void *p, size_t size, size_t count, void *file) { return std::fread(p, size, count, static_cast<FILE *>(file)); },
        [](void *file, ogg_int64_t offset, int whence) { return _fseeki64(static_cast<FILE *>(file), offset, whence); },
        [](void *file) { return std::fclose(static_cast<FILE *>(file)); },
        [](void *file) { return std::ftell(static_cast<FILE *>(file)); },
    };
    OggVorbis_File vf;
    if (ov_open_callbacks(f, &vf, nullptr, 0, callbacks) < 0) {
        std::fclose(f);
        return 0;
    }
    const int ms = static_cast<int>(total(&vf, -1) * 1000.0);
    ov_clear(&vf);
    return ms;
}

// Waits until both sides' new stream threads block in the wait for their events (their cycle
// counts stay put for 5 ms). Close terminates the threads: one terminated while it starts (its
// DLL attach calls, its first message wait setting up its queue) would keep a lock taken for
// good, and every later thread would hang. (The game never closes a stream that soon.)
void SettleThreads(const IShow *a, const IShow *b)
{
    HANDLE threads[2];
    int count = 0;
    for (const IShow *show : {a, b}) {
        if (show->thread)
            threads[count++] = *reinterpret_cast<const HANDLE *>(&show->thread);
    }
    ULONG64 last[2] = {};
    const ULONGLONG start = GetTickCount64();
    ULONGLONG stableSince = start;
    while (count && GetTickCount64() - start < 5000) {
        SwitchToThread();
        bool same = true;
        for (int i = 0; i < count; ++i) {
            ULONG64 cycles = 0;
            QueryThreadCycleTime(threads[i], &cycles);
            same &= cycles && cycles == last[i];
            last[i] = cycles;
        }
        if (!same)
            stableSince = GetTickCount64();
        else if (GetTickCount64() - stableSince >= 5)
            break;
    }
}

// Wakes a side's stream thread as DirectSound's notification would, and waits until it has
// refilled the buffer (its play position read, then `filling` cleared).
bool WakeStream(World &w, IShow *show)
{
    const FakeBuffer *buffer = w.Find(show->buffer);
    if (!buffer || !buffer->event)
        return false;
    const int reads = w.positionReads;
    SetEvent(buffer->event);
    const DWORD start = GetTickCount();
    while (w.positionReads == reads || show->filling) {
        if (GetTickCount() - start > 10000) {
            DWORD code = 0;
            GetExitCodeThread(*reinterpret_cast<HANDLE *>(&show->thread), &code);
            std::printf("  stream thread: reads %d -> %d, filling %d, flags %08x, notifyCount %d, event %s, "
                        "thread %s\n",
                        reads, w.positionReads.load(), show->filling, show->flags, show->notifyCount,
                        WaitForSingleObject(buffer->event, 0) == WAIT_OBJECT_0 ? "set" : "consumed",
                        code == STILL_ACTIVE ? "running" : "ended");
            return false;
        }
        SwitchToThread();
    }
    return true;
}

void OggStep(S_pair &p)
{
    IShow *a = p.a, *b = p.b;
    // (Closed or stopped players open and play more often: most operations need a playing file.)
    int op = Rand(26);
    if (!a->open && Rand(2))
        op = 14;
    else if (a->open && !(a->flags & ISHOWF_PLAYING) && Rand(3) == 0)
        op = 1;
    if (CommonStep(p, op))
        return;
    switch (op) {
    case 14:
    case 15: {
        std::string file;
        const int pick = Rand(static_cast<int>(g_oggs.size()) + 1);
        if (pick < static_cast<int>(g_oggs.size()))
            file = g_oggs[pick];
        else if (g_corruptOpens < 8)        // (the original keeps the file open: rw_data's handles leak)
            file = g_corruptOgg, ++g_corruptOpens;
        else
            file = g_oggs[0];
        const uint32_t flags = (Rand(2) ? ISHOW_LOOP : 0) | (Rand(4) == 0 ? ISHOW_TRUECOLOR : 0);
        const HRESULT ha = a->Open(file.c_str(), flags), hb = b->Open(file.c_str(), flags);
        Check("Open", ha, hb);
        g_opens[0] += ha == 0;
        SettleThreads(a, b);
        break;
    }
    case 16:
    case 17: {
        // Anywhere in the longest file, past its end now and then.
        const int ms = Rand(g_oggMs.empty() ? 20000 : *std::max_element(g_oggMs.begin(), g_oggMs.end()) + 1000);
        Check("SetPosition", a->SetPosition(ms), b->SetPosition(ms));
        break;
    }
    default: {
        // The buffer's notifications: the play cursor somewhere, then the thread refills.
        if (!(a->flags & ISHOWF_PLAYING) || !(b->flags & ISHOWF_PLAYING) || !(a->flags & ISHOWF_OGG))
            break;
        const int wakes = 1 + Rand(Rand(3) ? 3 : 14);
        for (int i = 0; i < wakes; ++i) {
            if (!(a->flags & ISHOWF_PLAYING) || !(b->flags & ISHOWF_PLAYING))
                break;
            DWORD play;
            switch (Rand(4)) {
            case 0: play = static_cast<DWORD>(a->writePos); break;
            case 1: play = 0; break;
            default: play = static_cast<DWORD>(Rand(a->bufferSize)) & ~3u; break;
            }
            g_world[0].Find(a->buffer)->position = play;
            g_world[1].Find(b->buffer)->position = play;
            const bool endStop = (a->flags & ISHOWF_END) && a->notifyCount == 10;
            const bool woke = WakeStream(g_world[0], a);
            if (WakeStream(g_world[1], b) != woke)
                Fail("stream thread woken on one side only");
            if (!woke) {
                Fail("stream thread did not answer");
                break;
            }
            ++g_wakes;
            g_endStops += endStop;
            Compare(p);
        }
        break;
    }
    }
}

void OggTest(int rounds)
{
    SetSound(true);
    S_pair p;
    p.Create(kDevices[0]);
    for (int r = 0; r < rounds && g_failures < 30; ++r) {
        char step[64];
        std::snprintf(step, sizeof step, "ogg round %d", r);
        g_step = step;
        if (Rand(400) == 0) {
            p.Free();
            Compare(p);
            p.Create(kDevices[0]);
        }
        OggStep(p);
        Compare(p);
    }
    g_step = "ogg end";
    p.Free();
}

// ---------------------------------------------------------------- Bink

std::vector<std::string> g_movies;      // the game's, then a missing one

void BinkStep(S_pair &p)
{
    IShow *a = p.a, *b = p.b;
    int op = Rand(30);
    if (!a->open && Rand(2))
        op = 14;
    else if (a->open && !(a->flags & ISHOWF_PLAYING) && Rand(3) == 0)
        op = 1;
    if (CommonStep(p, op))
        return;
    const BINK *movie = reinterpret_cast<const BINK *>(a->movie);
    switch (op) {
    case 14: {
        const std::string &file = g_movies[Rand(static_cast<int>(g_movies.size()))];
        const uint32_t flags = Rand(2) ? ISHOW_TRUECOLOR : 0;
        const HRESULT ha = a->Open(file.c_str(), flags), hb = b->Open(file.c_str(), flags);
        Check("Open", ha, hb);
        g_opens[1] += ha == 0;
        break;
    }
    case 15: {
        const uint32_t frame = static_cast<uint32_t>(Rand(movie ? static_cast<int>(movie->frames) + 3 : 50));
        Check("SetFrame", a->SetFrame(frame), b->SetFrame(frame));
        break;
    }
    case 16: {
        const int length = movie && movie->rate ? static_cast<int>(1000.0 * movie->frames * movie->rateDiv / movie->rate) : 5000;
        const int ms = Rand(length + 500);
        Check("SetPosition", a->SetPosition(ms), b->SetPosition(ms));
        break;
    }
    case 17: {
        // A new pair of players on another device.
        p.Free();
        p.Create(kDevices[Rand(kDeviceCount)]);
        break;
    }
    default: {
        const bool force = Rand(4) == 0;
        Check("UpdateVideo", a->UpdateVideo(force), b->UpdateVideo(force));
        break;
    }
    }
}

void BinkTest(int rounds)
{
    SetSound(false);
    S_pair p;
    p.Create(kDevices[0]);
    for (int r = 0; r < rounds && g_failures < 30; ++r) {
        char step[64];
        std::snprintf(step, sizeof step, "bink round %d", r);
        g_step = step;
        BinkStep(p);
        Compare(p);
    }
    g_step = "bink end";
    p.Free();
    Compare(p);
}

// ---------------------------------------------------------------- AVI (DirectShow)

struct S_riff {
    std::vector<uint8_t> v;
    void U16(int x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
    void U32(uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (i * 8))); }
    void Tag(const char *t) { v.insert(v.end(), t, t + 4); }
    // A chunk: its size is filled in by End.
    size_t Begin(const char *tag)
    {
        Tag(tag);
        U32(0);
        return v.size();
    }
    size_t List(const char *list, const char *type)
    {
        const size_t at = Begin(list);
        Tag(type);
        return at;
    }
    void End(size_t at)
    {
        const uint32_t size = static_cast<uint32_t>(v.size() - at);
        std::memcpy(&v[at - 4], &size, 4);
        if (size & 1)
            v.push_back(0);
    }
    bool Save(const std::string &path) const
    {
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        std::fwrite(v.data(), 1, v.size(), f);
        std::fclose(f);
        return true;
    }
};

// An uncompressed AVI at 10 frames a second (`bits`: 16 (RGB555), 24 or 32), every frame its own
// picture; with `audioMs`, a silent 22 kHz mono stream too.
bool WriteAvi(const std::string &path, int width, int height, int bits, int frames, int audioMs)
{
    const int rowBytes = (width * bits / 8 + 3) & ~3;
    const uint32_t frameSize = static_cast<uint32_t>(rowBytes * height);
    const uint32_t audioBytes = static_cast<uint32_t>(22050 * 2 * audioMs / 1000);
    S_riff r;
    const size_t riff = r.List("RIFF", "AVI ");
    const size_t hdrl = r.List("LIST", "hdrl");
    const size_t avih = r.Begin("avih");
    r.U32(100000);                          // microseconds a frame
    r.U32(frameSize * 10 + 44100);
    r.U32(0);
    r.U32(0x10);                            // AVIF_HASINDEX
    r.U32(static_cast<uint32_t>(frames));
    r.U32(0);
    r.U32(audioMs ? 2 : 1);
    r.U32(frameSize);
    r.U32(static_cast<uint32_t>(width));
    r.U32(static_cast<uint32_t>(height));
    for (int i = 0; i < 4; ++i)
        r.U32(0);
    r.End(avih);
    {
        const size_t strl = r.List("LIST", "strl");
        const size_t strh = r.Begin("strh");
        r.Tag("vids");
        r.Tag("DIB ");
        r.U32(0);
        r.U16(0);
        r.U16(0);
        r.U32(0);
        r.U32(1);                           // scale / rate: 10 a second
        r.U32(10);
        r.U32(0);
        r.U32(static_cast<uint32_t>(frames));
        r.U32(frameSize);
        r.U32(0xffffffff);
        r.U32(0);
        r.U16(0);
        r.U16(0);
        r.U16(width);
        r.U16(height);
        r.End(strh);
        const size_t strf = r.Begin("strf");
        r.U32(40);
        r.U32(static_cast<uint32_t>(width));
        r.U32(static_cast<uint32_t>(height));
        r.U16(1);
        r.U16(bits);
        r.U32(0);                           // BI_RGB
        r.U32(frameSize);
        for (int i = 0; i < 4; ++i)
            r.U32(0);
        r.End(strf);
        r.End(strl);
    }
    if (audioMs) {
        const size_t strl = r.List("LIST", "strl");
        const size_t strh = r.Begin("strh");
        r.Tag("auds");
        r.U32(0);
        r.U32(0);
        r.U16(0);
        r.U16(0);
        r.U32(0);
        r.U32(2);                           // scale: the block; rate: bytes a second
        r.U32(44100);
        r.U32(0);
        r.U32(audioBytes / 2);
        r.U32(audioBytes);
        r.U32(0xffffffff);
        r.U32(2);
        for (int i = 0; i < 4; ++i)
            r.U16(0);
        r.End(strh);
        const size_t strf = r.Begin("strf");
        r.U16(WAVE_FORMAT_PCM);
        r.U16(1);
        r.U32(22050);
        r.U32(44100);
        r.U16(2);
        r.U16(16);
        r.End(strf);
        r.End(strl);
    }
    r.End(hdrl);
    struct S_index {
        const char *id;
        uint32_t offset, size;
    };
    std::vector<S_index> index;
    const size_t movi = r.List("LIST", "movi");
    const size_t moviTag = movi;            // the 'movi' type: the index's offsets count from there
    if (audioMs) {
        const size_t chunk = r.Begin("01wb");
        index.push_back({"01wb", static_cast<uint32_t>(chunk - 8 - moviTag), audioBytes});
        r.v.insert(r.v.end(), audioBytes, 0);
        r.End(chunk);
    }
    for (int f = 0; f < frames; ++f) {
        const size_t chunk = r.Begin("00db");
        index.push_back({"00db", static_cast<uint32_t>(chunk - 8 - moviTag), frameSize});
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < rowBytes; ++x) {
                const int c = x % (bits / 8);
                const int px = x / (bits / 8);
                uint8_t value = static_cast<uint8_t>(px * 13 + y * 7 + f * 29 + c * 50 + (px ^ y) * 3);
                if (bits == 16 && c == 1)
                    value &= 0x7f;
                r.v.push_back(px < width ? value : 0);
            }
        }
        r.End(chunk);
    }
    r.End(movi);
    const size_t idx1 = r.Begin("idx1");
    for (const S_index &e : index) {
        r.Tag(e.id);
        r.U32(0x10);                        // AVIIF_KEYFRAME
        r.U32(e.offset);
        r.U32(e.size);
    }
    r.End(idx1);
    r.End(riff);
    return r.Save(path);
}

bool WriteWav(const std::string &path, int ms)
{
    const uint32_t bytes = static_cast<uint32_t>(22050 * 2 * ms / 1000);
    S_riff r;
    const size_t riff = r.List("RIFF", "WAVE");
    const size_t fmt = r.Begin("fmt ");
    r.U16(WAVE_FORMAT_PCM);
    r.U16(1);
    r.U32(22050);
    r.U32(44100);
    r.U16(2);
    r.U16(16);
    r.End(fmt);
    const size_t data = r.Begin("data");
    r.v.insert(r.v.end(), bytes, 0);
    r.End(data);
    r.End(riff);
    return r.Save(path);
}

// A side's filter graph: the original's member is the graph; the port's graph handle points at the
// backend's graph record, whose first member is the graph (video/win32/video_win32.cpp).
IGraphBuilder *GraphOf(int side, const IShow *show)
{
    if (!show->graph)
        return nullptr;
    return side == 0 ? reinterpret_cast<IGraphBuilder *>(show->graph) : *reinterpret_cast<IGraphBuilder *const *>(show->graph);
}

// No reference clock: the renderers draw every sample when it arrives.
void NoClock(IGraphBuilder *graph)
{
    IMediaFilter *filter = nullptr;
    if (graph && SUCCEEDED(graph->QueryInterface(IID_IMediaFilter, reinterpret_cast<void **>(&filter)))) {
        filter->SetSyncSource(nullptr);
        filter->Release();
    }
}

// The position where the graph ends its stream.
void StopAt(IGraphBuilder *graph, int ms)
{
    IMediaSeeking *seeking = nullptr;
    if (graph && SUCCEEDED(graph->QueryInterface(IID_IMediaSeeking, reinterpret_cast<void **>(&seeking)))) {
        LONGLONG stop = ms * 10000LL;
        seeking->SetPositions(nullptr, AM_SEEKING_NoPositioning, &stop, AM_SEEKING_AbsolutePositioning);
        seeking->Release();
    }
}

// Waits until both players' frame counts have settled at the same value (no new frame for 300
// ms), then `extraMs` (an audio renderer finishing its stream).
void Settle(IShow *a, IShow *b, int extraMs)
{
    const DWORD start = GetTickCount();
    DWORD since = start;
    int la = -1, lb = -1;
    for (;;) {
        Sleep(10);
        const int fa = a->GetFrameCount(), fb = b->GetFrameCount();
        if (fa != la || fb != lb) {
            la = fa;
            lb = fb;
            since = GetTickCount();
        } else if (fa == fb && fa > 0 && GetTickCount() - since > 300) {
            break;
        }
        if (GetTickCount() - start > 10000)
            break;
    }
    Sleep(static_cast<DWORD>(extraMs));
}

long g_aviFrames;
int g_aviOpens, g_aviTries;
bool g_wavOpened;

void AviTest(const std::string &dir)
{
    struct S_avi {
        const char *name;
        int width, height, bits, frames, audioMs;
    };
    static const S_avi kAvis[] = {
        {"show24.avi", 75, 41, 24, 12, 0},
        {"show32.avi", 130, 70, 32, 8, 0},
        {"show16.avi", 33, 17, 16, 6, 300},
    };
    for (const S_avi &v : kAvis) {
        if (!WriteAvi(dir + v.name, v.width, v.height, v.bits, v.frames, v.audioMs)) {
            Fail("cannot write %s", v.name);
            return;
        }
    }
    SetSound(true);
    for (int d = 0; d < kDeviceCount; ++d) {
        for (const S_avi &v : kAvis) {
            char step[64];
            std::snprintf(step, sizeof step, "avi %s device %d", v.name, d);
            g_step = step;
            S_pair p;
            p.Create(kDevices[d]);
            IShow *a = p.a, *b = p.b;
            const uint32_t flags = Rand(2) ? ISHOW_TRUECOLOR : 0;
            const std::string path = dir + v.name;
            if (d == 0)     // a failed file first: the graph keeps its renderer
                Check("Open missing", a->Open((dir + "missing.avi").c_str(), flags),
                      b->Open((dir + "missing.avi").c_str(), flags));
            const HRESULT ha = a->Open(path.c_str(), flags), hb = b->Open(path.c_str(), flags);
            Check("Open", ha, hb);
            g_aviOpens += ha == 0;
            ++g_aviTries;
            Compare(p);
            CommonStep(p, 9);       // GetVideoSize
            const S_showRect rect = {Rand(100), Rand(100), 100 + Rand(500), 100 + Rand(400)};
            Check("SetRect", a->SetRect(&rect), b->SetRect(&rect));
            Compare(p);
            NoClock(GraphOf(0, a));
            NoClock(GraphOf(1, b));
            CommonStep(p, 6);       // SetVolume
            Check("Pause", a->Pause(), b->Pause());
            Sleep(200);
            Check("GetState paused", a->GetState(), b->GetState());
            const int startMs = 100 * Rand(v.frames - 1);
            const int stopMs = startMs + 100 * (1 + Rand(v.frames - startMs / 100));
            StopAt(GraphOf(0, a), stopMs);
            StopAt(GraphOf(1, b), stopMs);
            Check("SetPosition", a->SetPosition(startMs), b->SetPosition(startMs));
            CommonStep(p, 4);       // GetPosition
            // A seek while paused pauses the graph again: its event, once there, before the run
            // (else it arrives before or after the run started, depending on the timing).
            Sleep(200);
            Check("GetState sought", a->GetState(), b->GetState());
            Check("Play", a->Play(), b->Play());
            Settle(a, b, v.audioMs ? v.audioMs + 700 : 100);
            g_aviFrames += a->GetFrameCount();
            Compare(p);             // the frames drawn and the last one's pixels
            Check("GetState ended", a->GetState(), b->GetState());
            Compare(p);
            CommonStep(p, 4);
            CommonStep(p, 2);       // Stop
            Compare(p);
            p.Free();
            Compare(p);
        }
    }
    // A plain graph (the player's own renderers): a silent WAV, and a file that is not there.
    g_step = "wav";
    if (!WriteWav(dir + "show.wav", 300)) {
        Fail("cannot write show.wav");
        return;
    }
    S_pair p;
    p.Create(kDevices[0]);
    IShow *a = p.a, *b = p.b;
    Check("Open missing", a->Open((dir + "missing.mpg").c_str(), 0), b->Open((dir + "missing.mpg").c_str(), 0));
    const HRESULT ha = a->Open((dir + "show.wav").c_str(), 0), hb = b->Open((dir + "show.wav").c_str(), 0);
    Check("Open", ha, hb);
    g_wavOpened = ha == 0;
    Compare(p);
    CommonStep(p, 9);
    CommonStep(p, 6);
    Check("SetPosition", a->SetPosition(100), b->SetPosition(100));
    CommonStep(p, 4);
    Check("Pause", a->Pause(), b->Pause());
    Sleep(300);
    Check("GetState paused", a->GetState(), b->GetState());
    Check("Play", a->Play(), b->Play());
    Sleep(1200);
    Check("GetState ended", a->GetState(), b->GetState());
    CommonStep(p, 4);
    Compare(p);
    p.Free();
    Compare(p);
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const int rounds = argc > 2 && *argv[2] ? std::atoi(argv[2]) : 3000;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 3)
        g_rng.seed(static_cast<unsigned>(std::atoi(argv[3])));
    char dirBuffer[MAX_PATH];
    std::strcpy(dirBuffer, orig);
    if (char *slash = std::strrchr(dirBuffer, '\\'))
        *slash = 0;
    const std::string gameDir = dirBuffer;
    SetDllDirectoryA(gameDir.c_str());
    HMODULE dll = LoadLibraryA(orig);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", orig, GetLastError());
        return 2;
    }
    g_base = reinterpret_cast<uint8_t *>(dll);
    if (!LoadBink() || !BinkHooks<0>::Install(dll) || !BinkHooks<1>::Install(GetModuleHandleA(nullptr))) {
        std::printf("cannot hook binkw32.dll\n");
        return 2;
    }
    CoInitialize(nullptr);

    g_origIGraph = reinterpret_cast<void *(__cdecl *)()>(GetProcAddress(dll, "GetIGraph"))();
    g_pIGraph = new IGraph;
    ls3d::g_logDisabled = true;
    Orig<uint8_t>(0x100d16f2) = 1;
    // The screen of SetRect(nullptr).
    Orig<S_IGraphInit>(0x100d1d18).width = ls3d::ig::g_init.width = 640;
    Orig<S_IGraphInit>(0x100d1d18).height = ls3d::ig::g_init.height = 480;

    g_origDriver = reinterpret_cast<ISND_driver *(__cdecl *)()>(GetProcAddress(dll, "ISndGetDriver"))();
    if (!g_origDriver || Orig<ISND_driver *>(0x100d16fc) != g_origDriver) {
        std::printf("the original has no sound driver\n");
        return 2;
    }
    g_sndDriver = new ISND_driver;
    g_origDriver->dsound = ls3d::audio::dsound::Handle(static_cast<IDirectSound8 *>(&g_world[0].ds));
    g_sndDriver->dsound = ls3d::audio::dsound::Handle(static_cast<IDirectSound8 *>(&g_world[1].ds));

    // The game's shortest Ogg files and smallest movies.
    auto smallest = [](const std::string &pattern, size_t count) {
        std::vector<std::pair<DWORD, std::string>> found;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    found.push_back({fd.nFileSizeLow, fd.cFileName});
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        for (size_t i = 0; i < found.size() && i < count; ++i)
            out.push_back(found[i].second);
        return out;
    };
    const std::string sounds = UnpackedDir(gameDir) + "\\Sounds\\";
    for (const std::string &name : smallest(sounds + "*.ogg", 3)) {
        g_oggs.push_back(sounds + name);
        g_oggMs.push_back(OggLength(g_oggs.back()));
    }
    const std::string movies = gameDir + "\\Gamemovie\\";
    for (const std::string &name : smallest(movies + "*.bik", 3))
        g_movies.push_back(movies + name);
    if (g_oggs.empty() || g_movies.empty()) {
        std::printf("no game files: %s*.ogg, %s*.bik (set HD2_UNPACKED to the unpacked data)\n", sounds.c_str(),
                    movies.c_str());
        return 2;
    }
    g_oggs.push_back(sounds + "missing.ogg");
    g_movies.push_back(movies + "missing.bik");

    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string work = std::string(tmp) + "ls3df_show\\";
    CreateDirectoryA(work.c_str(), nullptr);
    g_corruptOgg = work + "corrupt.ogg";
    {
        S_riff junk;
        for (int i = 0; i < 5000; ++i)
            junk.v.push_back(static_cast<uint8_t>(g_rng()));
        junk.Save(g_corruptOgg);
    }

    // The game runs the FPU in single precision (Direct3D's default).
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);

    OggTest(rounds);
    std::printf("ogg: %d rounds, %d files opened, %ld stream wakes (%ld end stops)\n", rounds, g_opens[0], g_wakes,
                g_endStops);
    BinkTest(rounds);
    std::printf("bink: %d rounds, %d movies opened, %zu waits, %.1f GB of tile pixels compared\n", rounds, g_opens[1],
                g_binkLog[0].waits, static_cast<double>(g_pixelsCompared) / 1e9);
    AviTest(work);
    std::printf("avi: %d of %d files opened (the one with sound needs an audio device), %ld frames drawn; wav %s\n",
                g_aviOpens, g_aviTries, g_aviFrames, g_wavOpened ? "opened" : "not opened");
    PROCESS_MEMORY_COUNTERS memory = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory);
    std::printf("peak memory %zu MB\n", memory.PeakPagefileUsage >> 20);
    std::printf("show: %d rounds, %d differences\n", rounds, g_failures);
    return g_failures ? 1 : 0;
}
