// Differential test of ISND (src/isnd): the original LS3DF.dll's sound objects and the port's run
// the same random operations, each side on its own set of fake DirectSound objects. Every
// DirectSound call with its arguments, the return values, the objects' fields and the voice
// manager's lists must be the same. (Device creation - Init, EAX detection, the device
// enumeration - needs real hardware and is not covered.)
//
// Usage: compare_isnd.exe [original LS3DF.dll] [rounds] [seed]
//
// test-sources: src/isnd/*.cpp src/core/debug.cpp
// test-sources: src/audio/dsound/audio_dsound.cpp
#include <windows.h>

#include <float.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../src/audio/dsound/dsound_device.h"
#include "../src/core/debug.h"
#include "../src/isnd/isnd.h"
#include "../src/isnd/isnd_voices.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d {
namespace ig {
bool g_logSoundFailures = false;
}
namespace i3d {
int g_refreshCalls;
void RefreshSoundVolumes() { ++g_refreshCalls; }
void DrawText2D(int, int, const char *, uint32_t, float) {}
}  // namespace i3d
}  // namespace ls3d

using namespace ls3d;
using namespace ls3d::snd;

namespace {

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }

uint32_t Bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// ---------------------------------------------------------------- fake DirectSound

struct World;

class FakeBuffer : public IDirectSoundBuffer8, public IDirectSound3DBuffer {
public:
    FakeBuffer(World *world, int id) : w(world), id(id) {}

    World *w;
    int id;
    ULONG refs = 1;
    DWORD flags = 0;
    WAVEFORMATEX format = {};
    std::shared_ptr<std::vector<uint8_t>> data = std::make_shared<std::vector<uint8_t>>();   // shared by duplicates
    LONG volume = 0, pan = 0;
    DWORD frequency = 0, position = 0, playFlags = 0;
    bool playing = false;

    void Log(const char *fmt, ...);

    HRESULT __stdcall QueryInterface(REFIID iid, void **out) override;
    ULONG __stdcall AddRef() override
    {
        Log("AddRef");
        return ++refs;
    }
    ULONG __stdcall Release() override
    {
        Log("Release");
        return --refs;
    }
    // IDirectSoundBuffer
    HRESULT __stdcall GetCaps(LPDSBCAPS) override { Log("GetCaps"); return E_NOTIMPL; }
    HRESULT __stdcall GetCurrentPosition(LPDWORD play, LPDWORD write) override
    {
        Log("GetCurrentPosition(%d,%d)", play != nullptr, write != nullptr);
        if (play)
            *play = position;
        if (write)
            *write = position;
        return DS_OK;
    }
    HRESULT __stdcall GetFormat(LPWAVEFORMATEX, DWORD, LPDWORD) override { Log("GetFormat"); return E_NOTIMPL; }
    HRESULT __stdcall GetVolume(LPLONG) override { Log("GetVolume"); return E_NOTIMPL; }
    HRESULT __stdcall GetPan(LPLONG) override { Log("GetPan"); return E_NOTIMPL; }
    HRESULT __stdcall GetFrequency(LPDWORD) override { Log("GetFrequency"); return E_NOTIMPL; }
    HRESULT __stdcall GetStatus(LPDWORD status) override
    {
        Log("GetStatus");
        *status = playing ? DSBSTATUS_PLAYING | ((playFlags & DSBPLAY_LOOPING) ? DSBSTATUS_LOOPING : 0) : 0;
        return DS_OK;
    }
    HRESULT __stdcall Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) override { Log("Initialize"); return E_NOTIMPL; }
    HRESULT __stdcall Lock(DWORD offset, DWORD bytes, LPVOID *p1, LPDWORD s1, LPVOID *p2, LPDWORD s2,
                           DWORD lockFlags) override
    {
        Log("Lock(%lu,%lu,%d,%d,%lu)", offset, bytes, p2 != nullptr, s2 != nullptr, lockFlags);
        *p1 = data->data();
        *s1 = static_cast<DWORD>(data->size());
        if (p2)
            *p2 = nullptr;
        if (s2)
            *s2 = 0;
        return DS_OK;
    }
    HRESULT __stdcall Play(DWORD reserved, DWORD priority, DWORD playFlags_) override;
    HRESULT __stdcall SetCurrentPosition(DWORD p) override
    {
        Log("SetCurrentPosition(%lu)", p);
        position = p;
        return DS_OK;
    }
    HRESULT __stdcall SetFormat(LPCWAVEFORMATEX f) override
    {
        Log("SetFormat(%u,%u,%lu,%lu,%u,%u,%u)", f->wFormatTag, f->nChannels, f->nSamplesPerSec,
            f->nAvgBytesPerSec, f->nBlockAlign, f->wBitsPerSample, f->cbSize);
        return DS_OK;
    }
    HRESULT __stdcall SetVolume(LONG v) override
    {
        Log("SetVolume(%ld)", v);
        volume = v;
        return DS_OK;
    }
    HRESULT __stdcall SetPan(LONG p) override
    {
        Log("SetPan(%ld)", p);
        pan = p;
        return DS_OK;
    }
    HRESULT __stdcall SetFrequency(DWORD f) override
    {
        Log("SetFrequency(%lu)", f);
        frequency = f;
        return DS_OK;
    }
    HRESULT __stdcall Stop() override
    {
        Log("Stop");
        playing = false;
        return DS_OK;
    }
    HRESULT __stdcall Unlock(LPVOID p1, DWORD s1, LPVOID p2, DWORD s2) override
    {
        Log("Unlock(%d,%lu,%d,%lu)", p1 == data->data(), s1, p2 != nullptr, s2);
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
    // IDirectSound3DBuffer
    HRESULT __stdcall GetAllParameters(LPDS3DBUFFER) override { Log("3D.GetAllParameters"); return E_NOTIMPL; }
    HRESULT __stdcall GetConeAngles(LPDWORD, LPDWORD) override { Log("3D.GetConeAngles"); return E_NOTIMPL; }
    HRESULT __stdcall GetConeOrientation(D3DVECTOR *) override { Log("3D.GetConeOrientation"); return E_NOTIMPL; }
    HRESULT __stdcall GetConeOutsideVolume(LPLONG) override { Log("3D.GetConeOutsideVolume"); return E_NOTIMPL; }
    HRESULT __stdcall GetMaxDistance(D3DVALUE *) override { Log("3D.GetMaxDistance"); return E_NOTIMPL; }
    HRESULT __stdcall GetMinDistance(D3DVALUE *) override { Log("3D.GetMinDistance"); return E_NOTIMPL; }
    HRESULT __stdcall GetMode(LPDWORD) override { Log("3D.GetMode"); return E_NOTIMPL; }
    HRESULT __stdcall GetPosition(D3DVECTOR *) override { Log("3D.GetPosition"); return E_NOTIMPL; }
    HRESULT __stdcall GetVelocity(D3DVECTOR *) override { Log("3D.GetVelocity"); return E_NOTIMPL; }
    HRESULT __stdcall SetAllParameters(LPCDS3DBUFFER, DWORD) override { Log("3D.SetAllParameters"); return E_NOTIMPL; }
    HRESULT __stdcall SetConeAngles(DWORD in, DWORD out, DWORD apply) override
    {
        Log("3D.SetConeAngles(%lu,%lu,%lu)", in, out, apply);
        return DS_OK;
    }
    HRESULT __stdcall SetConeOrientation(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override
    {
        Log("3D.SetConeOrientation(%08X,%08X,%08X,%lu)", Bits(x), Bits(y), Bits(z), apply);
        return DS_OK;
    }
    HRESULT __stdcall SetConeOutsideVolume(LONG v, DWORD apply) override
    {
        Log("3D.SetConeOutsideVolume(%ld,%lu)", v, apply);
        return DS_OK;
    }
    HRESULT __stdcall SetMaxDistance(D3DVALUE d, DWORD apply) override
    {
        Log("3D.SetMaxDistance(%08X,%lu)", Bits(d), apply);
        return DS_OK;
    }
    HRESULT __stdcall SetMinDistance(D3DVALUE d, DWORD apply) override
    {
        Log("3D.SetMinDistance(%08X,%lu)", Bits(d), apply);
        return DS_OK;
    }
    HRESULT __stdcall SetMode(DWORD, DWORD) override { Log("3D.SetMode"); return E_NOTIMPL; }
    HRESULT __stdcall SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override
    {
        Log("3D.SetPosition(%08X,%08X,%08X,%lu)", Bits(x), Bits(y), Bits(z), apply);
        return DS_OK;
    }
    HRESULT __stdcall SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override
    {
        Log("3D.SetVelocity(%08X,%08X,%08X,%lu)", Bits(x), Bits(y), Bits(z), apply);
        return DS_OK;
    }
};

class FakeListener : public IDirectSound3DListener {
public:
    World *w = nullptr;
    void Log(const char *fmt, ...);
    HRESULT __stdcall QueryInterface(REFIID, void **out) override
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall GetAllParameters(LPDS3DLISTENER p) override
    {
        Log("L.GetAllParameters");
        const float values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0.5f, 0.25f, 2.0f};
        std::memcpy(&p->vPosition, values, sizeof values);
        return DS_OK;
    }
    HRESULT __stdcall GetDistanceFactor(D3DVALUE *) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDopplerFactor(D3DVALUE *) override { return E_NOTIMPL; }
    HRESULT __stdcall GetOrientation(D3DVECTOR *, D3DVECTOR *) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPosition(D3DVECTOR *) override { return E_NOTIMPL; }
    HRESULT __stdcall GetRolloffFactor(D3DVALUE *) override { return E_NOTIMPL; }
    HRESULT __stdcall GetVelocity(D3DVECTOR *) override { return E_NOTIMPL; }
    HRESULT __stdcall SetAllParameters(LPCDS3DLISTENER p, DWORD apply) override
    {
        std::string s;
        const uint32_t *u = reinterpret_cast<const uint32_t *>(p);
        for (size_t i = 0; i < sizeof(DS3DLISTENER) / 4; ++i) {
            char t[12];
            std::snprintf(t, sizeof t, "%08X,", u[i]);
            s += t;
        }
        Log("L.SetAllParameters(%s%lu)", s.c_str(), apply);
        return failNext ? (failNext = false, DSERR_INVALIDCALL) : DS_OK;
    }
    HRESULT __stdcall SetDistanceFactor(D3DVALUE, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetDopplerFactor(D3DVALUE, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetOrientation(D3DVALUE, D3DVALUE, D3DVALUE, D3DVALUE, D3DVALUE, D3DVALUE, DWORD) override
    {
        return E_NOTIMPL;
    }
    HRESULT __stdcall SetPosition(D3DVALUE, D3DVALUE, D3DVALUE, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetRolloffFactor(D3DVALUE, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetVelocity(D3DVALUE, D3DVALUE, D3DVALUE, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall CommitDeferredSettings() override
    {
        Log("L.Commit");
        return failNext ? (failNext = false, DSERR_INVALIDCALL) : DS_OK;
    }
    bool failNext = false;
};

class FakeProps : public IKsPropertySet {
public:
    World *w = nullptr;
    void Log(const char *fmt, ...);
    HRESULT __stdcall QueryInterface(REFIID, void **out) override
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall Get(REFGUID set, ULONG id, LPVOID inst, ULONG instLength, LPVOID data, ULONG length,
                          PULONG returned) override
    {
        Log("P.Get(%08lX,%lu,%d,%lu,%lu,%d)", set.Data1, id, inst != nullptr, instLength, length, returned != nullptr);
        std::memset(data, 0x5A, length);
        if (returned)
            *returned = length;
        return DS_OK;
    }
    HRESULT __stdcall Set(REFGUID set, ULONG id, LPVOID inst, ULONG instLength, LPVOID data, ULONG length) override
    {
        std::string s;
        for (ULONG i = 0; i < length && i < 0x70; ++i) {
            char t[4];
            std::snprintf(t, sizeof t, "%02X", static_cast<const uint8_t *>(data)[i]);
            s += t;
        }
        Log("P.Set(%08lX,%lu,%d,%lu,%lu,%s)", set.Data1, id, inst != nullptr, instLength, length, s.c_str());
        return DS_OK;
    }
    HRESULT __stdcall QuerySupport(REFGUID, ULONG, PULONG) override { return E_NOTIMPL; }
};

class FakeDSound : public IDirectSound8 {
public:
    World *w = nullptr;
    void Log(const char *fmt, ...);
    HRESULT __stdcall QueryInterface(REFIID, void **out) override
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN outer) override;
    HRESULT __stdcall GetCaps(LPDSCAPS) override { return E_NOTIMPL; }
    HRESULT __stdcall DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER *out) override;
    HRESULT __stdcall SetCooperativeLevel(HWND, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall Compact() override { return E_NOTIMPL; }
    HRESULT __stdcall GetSpeakerConfig(LPDWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetSpeakerConfig(DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall Initialize(LPCGUID) override { return E_NOTIMPL; }
    HRESULT __stdcall VerifyCertification(LPDWORD) override { return E_NOTIMPL; }
};

struct World {
    std::vector<std::string> log;
    std::vector<FakeBuffer *> buffers;
    FakeDSound ds;
    FakeListener listener;
    FakeProps props;
    FakeBuffer *primary = nullptr;
    int failPlays = 0;          // the next N Play calls fail
    bool failDuplicate = false;

    World()
    {
        ds.w = this;
        listener.w = this;
        props.w = this;
        primary = New();
        primary->flags = DSBCAPS_PRIMARYBUFFER;
    }
    FakeBuffer *New()
    {
        FakeBuffer *b = new FakeBuffer(this, static_cast<int>(buffers.size()));
        buffers.push_back(b);
        return b;
    }
    void VLog(const char *prefix, const char *fmt, va_list args)
    {
        char text[1024];
        std::vsnprintf(text, sizeof text, fmt, args);
        log.push_back(std::string(prefix) + text);
    }
    // (The port's objects hold the sound device's handles, which are the fakes' interfaces.)
    int IdOf(const void *p) const
    {
        for (const FakeBuffer *b : buffers) {
            if (static_cast<const void *>(static_cast<const IDirectSoundBuffer8 *>(b)) == p ||
                static_cast<const void *>(static_cast<const IDirectSound3DBuffer *>(b)) == p)
                return b->id;
        }
        return p ? -2 : -1;
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
void FakeListener::Log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    w->VLog("", fmt, args);
    va_end(args);
}
void FakeProps::Log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    w->VLog("", fmt, args);
    va_end(args);
}
void FakeDSound::Log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    w->VLog("DS.", fmt, args);
    va_end(args);
}

HRESULT FakeBuffer::QueryInterface(REFIID iid, void **out)
{
    if (iid == IID_IDirectSoundBuffer8 || iid == IID_IDirectSoundBuffer || iid == IID_IUnknown) {
        Log("QI(Buffer8)");
        *out = static_cast<IDirectSoundBuffer8 *>(this);
    } else if (iid == IID_IDirectSound3DBuffer && (flags & DSBCAPS_CTRL3D)) {
        Log("QI(3DBuffer)");
        *out = static_cast<IDirectSound3DBuffer *>(this);
    } else {
        Log("QI(other)");
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ++refs;
    return S_OK;
}

HRESULT FakeBuffer::Play(DWORD reserved, DWORD priority, DWORD flags_)
{
    Log("Play(%lu,%lu,%08lX)", reserved, priority, flags_);
    if (w->failPlays > 0) {
        --w->failPlays;
        return DSERR_INVALIDCALL;
    }
    playing = true;
    playFlags = flags_;
    return DS_OK;
}

HRESULT FakeDSound::CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN outer)
{
    const WAVEFORMATEX *f = desc->lpwfxFormat;
    Log("CreateSoundBuffer(%lu,%08lX,%lu,%lu,%08lX,%d)->B%zu", desc->dwSize, desc->dwFlags, desc->dwBufferBytes,
        desc->dwReserved, desc->guid3DAlgorithm.Data1, outer != nullptr, w->buffers.size());
    if (f)
        Log("  format(%u,%u,%lu,%lu,%u,%u)", f->wFormatTag, f->nChannels, f->nSamplesPerSec, f->nAvgBytesPerSec,
            f->nBlockAlign, f->wBitsPerSample);
    FakeBuffer *b = w->New();
    b->flags = desc->dwFlags;
    if (f)
        b->format = *f;
    b->data->assign(desc->dwBufferBytes, 0);
    *out = b;
    return DS_OK;
}

HRESULT FakeDSound::DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER *out)
{
    FakeBuffer *src = static_cast<FakeBuffer *>(static_cast<IDirectSoundBuffer8 *>(original));
    Log("Duplicate(B%d)->B%zu", src->id, w->buffers.size());
    if (w->failDuplicate) {
        w->failDuplicate = false;
        *out = nullptr;
        return DSERR_OUTOFMEMORY;
    }
    FakeBuffer *b = w->New();
    b->flags = src->flags;
    b->format = src->format;
    b->data = src->data;
    *out = b;
    return DS_OK;
}

// ---------------------------------------------------------------- the two sides

World g_origWorld, g_portWorld;
ISND_driver *g_origDriver;
std::vector<std::string> g_files;
int g_failures;
std::mt19937 g_rng(12345);
bool g_haveI3D;                 // the original's I3D driver exists (its SetVolume walks the sound frames)

int Rand(int n) { return static_cast<int>(g_rng() % static_cast<unsigned>(n)); }
float RandFloat(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g_rng() % 100000) / 100000.0f; }

void Setup(ISND_driver *d, World &w, uint32_t flags)
{
    using ls3d::audio::dsound::Handle;
    d->dsound = Handle(static_cast<IDirectSound8 *>(&w.ds));
    d->eax = Handle(static_cast<IKsPropertySet *>(&w.props));
    d->primary = Handle(static_cast<IDirectSoundBuffer8 *>(w.primary));
    d->listener = Handle(static_cast<IDirectSound3DListener *>(&w.listener));
    d->flags = flags;
    d->volume = 1.0f;
    d->maxFrequency = 100000;
    d->minFrequency = 4000;
}

struct S_side {
    World *world;
    ISND_driver *driver;
    std::vector<ISND_source *> sources;
    std::vector<ISND_sound *> sounds;
    std::vector<bool> is3D;
    C_vector<S_voice> *voices;
    C_vector<S_suspendedVoice> *suspended;
    int *maxVoices;
    bool *initialized;
    void (*update)();
};
S_side g_orig, g_port;

int IndexOf(const S_side &s, const ISND_sound *sound)
{
    for (size_t i = 0; i < s.sounds.size(); ++i) {
        if (s.sounds[i] == sound)
            return static_cast<int>(i);
    }
    return sound ? -2 : -1;
}
int SourceIndex(const S_side &s, const ISND_source *source)
{
    for (size_t i = 0; i < s.sources.size(); ++i) {
        if (s.sources[i] == source)
            return static_cast<int>(i);
    }
    return source ? -2 : -1;
}

std::string g_step;
void Fail(const char *fmt, ...)
{
    if (g_failures++ < 20) {
        char text[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        std::printf("DIFF at %s: %s\n", g_step.c_str(), text);
    }
}

// Both sides read the time from this clock: their timeGetTime imports are patched to it.
DWORD g_clock = 1000000;
DWORD WINAPI FakeTimeGetTime() { return g_clock; }

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

int g_maxVoicesSeen, g_maxSuspendedSeen, g_resumes, g_lastSuspended;
void CompareState()
{
    g_maxVoicesSeen = (std::max)(g_maxVoicesSeen, g_port.voices->Size());
    g_maxSuspendedSeen = (std::max)(g_maxSuspendedSeen, g_port.suspended->Size());
    if (g_port.suspended->Size() < g_lastSuspended)
        ++g_resumes;
    g_lastSuspended = g_port.suspended->Size();
    // DirectSound calls.
    if (g_origWorld.log != g_portWorld.log) {
        size_t i = 0;
        while (i < g_origWorld.log.size() && i < g_portWorld.log.size() && g_origWorld.log[i] == g_portWorld.log[i])
            ++i;
        Fail("call %zu: orig '%s' port '%s'", i, i < g_origWorld.log.size() ? g_origWorld.log[i].c_str() : "-",
             i < g_portWorld.log.size() ? g_portWorld.log[i].c_str() : "-");
    }
    g_origWorld.log.clear();
    g_portWorld.log.clear();
    for (size_t i = 0; i < g_origWorld.buffers.size() && i < g_portWorld.buffers.size(); ++i) {
        const FakeBuffer *a = g_origWorld.buffers[i], *b = g_portWorld.buffers[i];
        if (a->refs != b->refs || a->data->size() != b->data->size() || a->volume != b->volume || a->pan != b->pan ||
            a->frequency != b->frequency || a->position != b->position || a->playing != b->playing)
            Fail("buffer B%zu state: refs %lu/%lu vol %ld/%ld pan %ld/%ld freq %lu/%lu pos %lu/%lu playing %d/%d", i,
                 a->refs, b->refs, a->volume, b->volume, a->pan, b->pan, a->frequency, b->frequency, a->position,
                 b->position, a->playing, b->playing);
    }
    if (g_origWorld.buffers.size() != g_portWorld.buffers.size())
        Fail("buffer count %zu / %zu", g_origWorld.buffers.size(), g_portWorld.buffers.size());

    // Objects.
    for (size_t k = 0; k < g_orig.sounds.size(); ++k) {
        const ISND_sound *a = g_orig.sounds[k], *b = g_port.sounds[k];
        if (!a || !b) {
            if (a != b)
                Fail("sound %zu existence", k);
            continue;
        }
        if (a->refCount != b->refCount || g_origWorld.IdOf(a->buffer) != g_portWorld.IdOf(b->buffer) ||
            SourceIndex(g_orig, a->source) != SourceIndex(g_port, b->source) || a->flags != b->flags ||
            Bits(a->frequency) != Bits(b->frequency) || Bits(a->volume) != Bits(b->volume))
            Fail("sound %zu fields: refs %d/%d flags %X/%X freq %08X/%08X vol %08X/%08X", k, a->refCount, b->refCount,
                 a->flags, b->flags, Bits(a->frequency), Bits(b->frequency), Bits(a->volume), Bits(b->volume));
        if (g_orig.is3D[k]) {
            const ISND_3Dsound *a3 = static_cast<const ISND_3Dsound *>(a), *b3 = static_cast<const ISND_3Dsound *>(b);
            if (a3->open != b3->open || g_origWorld.IdOf(a3->buffer3D) != g_portWorld.IdOf(b3->buffer3D))
                Fail("3D sound %zu fields", k);
        }
    }
    for (size_t j = 0; j < g_orig.sources.size(); ++j) {
        const ISND_source *a = g_orig.sources[j], *b = g_port.sources[j];
        if (a->refCount != b->refCount || a->flags != b->flags)
            Fail("source %zu refs %d/%d flags %X/%X", j, a->refCount, b->refCount, a->flags, b->flags);
    }

    // Voice manager.
    if (*g_orig.maxVoices != *g_port.maxVoices)
        Fail("max voices");
    const C_vector<S_voice> &va = *g_orig.voices, &vb = *g_port.voices;
    if (va.Size() != vb.Size()) {
        Fail("voices %d / %d", va.Size(), vb.Size());
    } else {
        for (int i = 0; i < va.Size(); ++i) {
            const S_voice &a = va[i], &b = vb[i];
            if (IndexOf(g_orig, a.sound) != IndexOf(g_port, b.sound) ||
                g_origWorld.IdOf(a.buffer) != g_portWorld.IdOf(b.buffer) || a.priority != b.priority ||
                a.loop != b.loop || a.startTime != b.startTime || a.pauseTime != b.pauseTime ||
                a.pausedTotal != b.pausedTotal)
                Fail("voice %d: sound %d/%d prio %u/%u loop %u/%u start %u/%u", i, IndexOf(g_orig, a.sound),
                     IndexOf(g_port, b.sound), a.priority, b.priority, a.loop, b.loop, a.startTime, b.startTime);
        }
    }
    const C_vector<S_suspendedVoice> &sa = *g_orig.suspended, &sb = *g_port.suspended;
    if (sa.Size() != sb.Size()) {
        Fail("suspended %d / %d", sa.Size(), sb.Size());
    } else {
        for (int i = 0; i < sa.Size(); ++i) {
            const S_suspendedVoice &a = sa[i], &b = sb[i];
            if (IndexOf(g_orig, a.sound) != IndexOf(g_port, b.sound) ||
                g_origWorld.IdOf(a.buffer) != g_portWorld.IdOf(b.buffer) || a.priority != b.priority ||
                a.loop != b.loop || a.position != b.position || a.suspendTime != b.suspendTime)
                Fail("suspended %d: sound %d/%d prio %u/%u", i, IndexOf(g_orig, a.sound), IndexOf(g_port, b.sound),
                     a.priority, b.priority);
        }
    }
}

void Check(const char *what, int a, int b)
{
    if (a != b)
        Fail("%s returned %d / %d", what, a, b);
}

// ---------------------------------------------------------------- test files

void WriteWav(const std::string &path, int channels, int bits, int rate, int ms, bool extraChunk, int fmtSize)
{
    const uint32_t blockAlign = static_cast<uint32_t>(channels * bits / 8);
    const uint32_t bytes = static_cast<uint32_t>(rate) * blockAlign * static_cast<uint32_t>(ms) / 1000;
    std::vector<uint8_t> f;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto u16 = [&](uint32_t v) { for (int i = 0; i < 2; ++i) f.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto tag = [&](const char *t) { f.insert(f.end(), t, t + 4); };
    tag("RIFF");
    u32(0);
    tag("WAVE");
    tag("fmt ");
    u32(static_cast<uint32_t>(fmtSize));
    u16(1);
    u16(static_cast<uint32_t>(channels));
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * blockAlign);
    u16(blockAlign);
    u16(static_cast<uint32_t>(bits));
    for (int i = 16; i < fmtSize; ++i)
        f.push_back(0);
    if (extraChunk) {
        tag("LIST");
        u32(10);
        for (int i = 0; i < 10; ++i)
            f.push_back(static_cast<uint8_t>('a' + i));
    }
    tag("data");
    u32(bytes);
    for (uint32_t i = 0; i < bytes; ++i)
        f.push_back(static_cast<uint8_t>(g_rng()));
    FILE *out = std::fopen(path.c_str(), "wb");
    std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
}

// ---------------------------------------------------------------- operations

ISND_sound *NewSound(S_side &s, bool threeD)
{
    ISND_sound *sound = threeD ? s.driver->New3DSound() : s.driver->NewSound();
    sound->flags = 0;           // uninitialised in the original until Open
    return sound;
}

uint32_t RandomSoundFlags()
{
    static const uint32_t kBits[] = {ISNDF_NO_VOLUME, ISNDF_NO_PAN, ISNDF_FREQUENCY, ISNDF_SOFTWARE, ISNDF_3D};
    uint32_t f = 0;
    for (uint32_t b : kBits) {
        if (Rand(3) == 0)
            f |= b;
    }
    return f;
}

void SoundStep(int round)
{
    const int k = Rand(static_cast<int>(g_orig.sounds.size()));
    ISND_sound *a = g_orig.sounds[k], *b = g_port.sounds[k];
    const int op = Rand(24);
    if (Rand(4) == 0)
        g_clock += static_cast<DWORD>(Rand(Rand(8) == 0 ? 30000 : 300));
    char step[64];
    std::snprintf(step, sizeof step, "round %d sound %d op %d", round, k, op);
    g_step = step;
    switch (op) {
    case 0:
    case 1: {
        const int j = Rand(static_cast<int>(g_orig.sources.size()));
        const uint32_t flags = RandomSoundFlags();
        if (Rand(20) == 0)
            g_origWorld.failDuplicate = g_portWorld.failDuplicate = true;
        Check("Open", a->Open(g_orig.sources[j], flags), b->Open(g_port.sources[j], flags));
        break;
    }
    case 2:
        Check("Close", a->Close(), b->Close());
        break;
    case 3:
    case 4:
    case 5: {
        static const uint32_t kPlay[] = {0, ISNDP_LOOP, ISNDP_KEEP_POSITION, ISNDP_LOOP | ISNDP_KEEP_POSITION};
        const uint32_t flags = kPlay[Rand(4)];
        const uint32_t priority = static_cast<uint32_t>(Rand(4));
        if (Rand(10) == 0)
            g_origWorld.failPlays = g_portWorld.failPlays = 1 + Rand(2);
        Check("Play", a->Play(flags, priority), b->Play(flags, priority));
        g_origWorld.failPlays = g_portWorld.failPlays = 0;
        break;
    }
    case 6:
        Check("Stop", a->Stop(), b->Stop());
        break;
    case 7: {
        int position;
        const int r = Rand(4);
        if (r == 0)
            position = ISND_PAUSE;
        else if (r == 1)
            position = ISND_RESUME;
        else
            position = Rand(2500);
        Check("SetPlayPosition", a->SetPlayPosition(position), b->SetPlayPosition(position));
        break;
    }
    case 8: {
        const float v = Rand(5) == 0 ? (Rand(2) ? 0.0f : 1e-9f) : RandFloat(-0.2f, 1.3f);
        Check("SetVolume", a->SetVolume(v), b->SetVolume(v));
        break;
    }
    case 9: {
        const int mb = Rand(14000) - 12000;
        Check("SetVolumeDb", a->SetVolumeDb(mb), b->SetVolumeDb(mb));
        break;
    }
    case 10: {
        const float v = RandFloat(-0.2f, 1.3f);
        Check("SetVolumeLinear", a->SetVolumeLinear(v), b->SetVolumeLinear(v));
        break;
    }
    case 11: {
        const float r = RandFloat(0.0f, 4.0f);
        Check("SetFrequency", a->SetFrequency(r), b->SetFrequency(r));
        break;
    }
    case 12: {
        const float p = Rand(6) == 0 ? 0.0f : RandFloat(-1.0f, 3.0f);
        Check("SetPan", a->SetPan(p), b->SetPan(p));
        break;
    }
    case 13:
        Check("IsPlaying", a->IsPlaying(), b->IsPlaying());
        break;
    case 14:
        Check("GetPlayPosition", a->GetPlayPosition(), b->GetPlayPosition());
        break;
    case 15: {
        const int j = Rand(static_cast<int>(g_orig.sources.size()));
        const uint32_t flags = RandomSoundFlags() | (Rand(2) ? ISNDP_LOOP : 0);
        Check("OpenAndPlay", a->OpenAndPlay(g_orig.sources[j], flags), b->OpenAndPlay(g_port.sources[j], flags));
        break;
    }
    case 16:
        if (g_orig.is3D[k]) {
            ISND_3Dsound *a3 = static_cast<ISND_3Dsound *>(a), *b3 = static_cast<ISND_3Dsound *>(b);
            const S_vector v(RandFloat(-100, 100), RandFloat(-100, 100), RandFloat(-100, 100));
            const float x = RandFloat(-1, 7), y = RandFloat(-1, 7);
            switch (Rand(7)) {
            case 0: Check("3D.SetPosition", a3->SetPosition(v), b3->SetPosition(v)); break;
            case 1: Check("3D.SetConeOrientation", a3->SetConeOrientation(v), b3->SetConeOrientation(v)); break;
            case 2: Check("3D.SetVelocity", a3->SetVelocity(v), b3->SetVelocity(v)); break;
            case 3: Check("3D.SetDistances", a3->SetDistances(x, y), b3->SetDistances(x, y)); break;
            case 4: Check("3D.SetConeAngles", a3->SetConeAngles(x, y), b3->SetConeAngles(x, y)); break;
            case 5: Check("3D.SetConeOutsideVolume", a3->SetConeOutsideVolume(x / 6), b3->SetConeOutsideVolume(x / 6)); break;
            default: Check("3D.IsOpen", a3->IsOpen(), b3->IsOpen()); break;
            }
        }
        break;
    case 17:
    case 18:
        g_orig.update();
        g_port.update();
        break;
    case 19: {
        // Playback progress / a buffer that has finished (the same buffer on both sides).
        const int id = Rand(static_cast<int>(g_origWorld.buffers.size()));
        FakeBuffer *fa = g_origWorld.buffers[id], *fb = g_portWorld.buffers[id];
        if (Rand(2)) {
            fa->playing = fb->playing = false;
        } else {
            fa->position = fb->position = static_cast<DWORD>(Rand(static_cast<int>(fa->data->size() * 9 / 10) + 1));
        }
        break;
    }
    case 20:
        if (Rand(3) == 0) {
            // Release and replace the sound (the last reference: the destructor).
            const bool threeD = Rand(2) != 0;
            const int ra = a->Release(), rb = b->Release();
            Check("Release", ra, rb);
            if (ra == 0) {
                g_orig.sounds[k] = NewSound(g_orig, threeD);
                g_port.sounds[k] = NewSound(g_port, threeD);
                g_orig.is3D[k] = g_port.is3D[k] = threeD;
            } else {
                g_orig.sounds[k]->refCount = g_port.sounds[k]->refCount = 1;
            }
        }
        break;
    case 21: {
        if (!g_haveI3D)
            break;
        const float v = RandFloat(-0.2f, 1.2f);
        Check("driver.SetVolume", g_orig.driver->SetVolume(v), g_port.driver->SetVolume(v));
        break;
    }
    case 22: {
        const bool on = Rand(2) != 0;
        Orig<bool>(0x100d21c6) = on;
        ig::g_logSoundFailures = on;
        break;
    }
    default: {
        // Change the voice count (the voice manager on / off) or the driver flags. Only between
        // plays: entries left in the lists would dangle on both sides (the game never switches).
        if (g_orig.voices->Size() || g_orig.suspended->Size() || g_port.voices->Size() || g_port.suspended->Size())
            break;
        if (Rand(2)) {
            const int n = Rand(5);
            *g_orig.maxVoices = *g_port.maxVoices = n;
        } else {
            const uint32_t flags = Rand(4) == 0 ? ISND_INIT_SOFTWARE : 0;
            g_orig.driver->flags = g_port.driver->flags = flags;
        }
        break;
    }
    }
    CompareState();
}

void ListenerTest(int rounds)
{
    ISND_3Dlistener *a = g_orig.driver->NewListener(), *b = g_port.driver->NewListener();
    g_step = "NewListener";
    if (std::memcmp(&a->params, &b->params, sizeof a->params) != 0 || a->refCount != b->refCount)
        Fail("listener fields");
    CompareState();
    for (int r = 0; r < rounds; ++r) {
        char step[64];
        const int op = Rand(14);
        std::snprintf(step, sizeof step, "listener round %d op %d", r, op);
        g_step = step;
        const S_vector v(RandFloat(-50, 50), RandFloat(-50, 50), RandFloat(-50, 50));
        const S_vector u(RandFloat(-1, 1), RandFloat(-1, 1), RandFloat(-1, 1));
        const float f = RandFloat(0, 3);
        switch (op) {
        case 0: Check("SetPosition", a->SetPosition(v), b->SetPosition(v)); break;
        case 1: Check("SetOrientation", a->SetOrientation(v, u), b->SetOrientation(v, u)); break;
        case 2: Check("SetVelocity", a->SetVelocity(v), b->SetVelocity(v)); break;
        case 3: Check("SetDistanceFactor", a->SetDistanceFactor(f), b->SetDistanceFactor(f)); break;
        case 4: Check("SetDopplerFactor", a->SetDopplerFactor(f), b->SetDopplerFactor(f)); break;
        case 5: Check("SetRolloffFactor", a->SetRolloffFactor(f), b->SetRolloffFactor(f)); break;
        case 6: {
            const uint32_t property = static_cast<uint32_t>(Rand(28));
            static S_vector pan;
            pan = v;
            const uintptr_t value = (property == 13 || property == 16) ? reinterpret_cast<uintptr_t>(&pan)
                                                                       : static_cast<uintptr_t>(g_rng());
            Check("SetEaxProperty", a->SetEaxProperty(property, value), b->SetEaxProperty(property, value));
            break;
        }
        case 7: {
            uint8_t data[0x70];
            for (uint8_t &d : data)
                d = static_cast<uint8_t>(g_rng());
            const uint32_t property = static_cast<uint32_t>(Rand(28)), size = static_cast<uint32_t>(Rand(0x70));
            Check("SetEaxData", a->SetEaxData(property, data, size), b->SetEaxData(property, data, size));
            break;
        }
        case 8: {
            uint8_t da[0x70] = {}, db[0x70] = {};
            ULONG ra = 0, rb = 0;
            const uint32_t property = static_cast<uint32_t>(Rand(28)), size = static_cast<uint32_t>(Rand(0x70));
            Check("GetEaxData", a->GetEaxData(property, da, size, &ra), b->GetEaxData(property, db, size, &rb));
            if (ra != rb || std::memcmp(da, db, size) != 0)
                Fail("GetEaxData result");
            break;
        }
        case 9:
            g_origWorld.listener.failNext = g_portWorld.listener.failNext = Rand(4) == 0;
            Check("Apply", a->Apply(), b->Apply());
            break;
        case 10:
            g_origWorld.listener.failNext = g_portWorld.listener.failNext = Rand(4) == 0;
            Check("Commit", a->Commit(), b->Commit());
            break;
        case 11: {
            // EAX on / off.
            const uint32_t flags = Rand(2) ? ISND_INIT_NO_EAX : 0;
            g_orig.driver->flags = g_port.driver->flags = flags;
            if (Rand(4) == 0) {
                g_orig.driver->eax = nullptr;
                g_port.driver->eax = nullptr;
            } else {
                g_orig.driver->eax = ls3d::audio::dsound::Handle(&g_origWorld.props);
                g_port.driver->eax = ls3d::audio::dsound::Handle(&g_portWorld.props);
            }
            break;
        }
        case 12: Check("AddRef", a->AddRef(), b->AddRef()); break;
        default: {
            const int fr = 4000 + Rand(60000), ch = 1 + Rand(2), bits = Rand(2) ? 8 : 16;
            Check("SetPrimaryFormat", g_orig.driver->SetPrimaryFormat(fr, ch, bits),
                  g_port.driver->SetPrimaryFormat(fr, ch, bits));
            break;
        }
        }
        if (std::memcmp(&a->params, &b->params, sizeof a->params) != 0 || a->refCount != b->refCount)
            Fail("listener fields");
        CompareState();
    }
    g_step = "listener release";
    while (a->refCount > 1) {
        a->Release();
        b->Release();
    }
    Check("Release", a->Release(), b->Release());
    g_orig.driver->flags = g_port.driver->flags = 0;
    g_orig.driver->eax = ls3d::audio::dsound::Handle(&g_origWorld.props);
    g_port.driver->eax = ls3d::audio::dsound::Handle(&g_portWorld.props);
    CompareState();
}

void SourceTest()
{
    for (size_t i = 0; i < g_files.size(); ++i) {
        for (int variant = 0; variant < 3; ++variant) {
            char step[64];
            std::snprintf(step, sizeof step, "source %zu variant %d", i, variant);
            g_step = step;
            const uint32_t flags = variant == 0 ? 0 : variant == 1 ? ISNDF_3D : ISNDF_3D | ISNDF_SOFTWARE;
            g_orig.driver->flags = g_port.driver->flags = variant == 2 ? 0 : (variant ? ISND_INIT_SOFTWARE : 0);
            ISND_source *a = g_orig.driver->NewSource(), *b = g_port.driver->NewSource();
            Check("Open", a->Open(g_files[i].c_str(), flags), b->Open(g_files[i].c_str(), flags));
            Check("IsOpen", a->IsOpen(), b->IsOpen());
            Check("GetLength", a->GetLength(), b->GetLength());
            S_ISND_format fa = {}, fb = {};
            Check("GetFormat", a->GetFormat(&fa), b->GetFormat(&fb));
            if (std::memcmp(&fa, &fb, sizeof fa) != 0)
                Fail("format");
            if (a->flags != b->flags || a->channels != b->channels || a->bytesPerSample != b->bytesPerSample ||
                a->frequency != b->frequency || a->dataSize != b->dataSize || Bits(a->bytesPerMs) != Bits(b->bytesPerMs) ||
                (a->IsOpen() && std::memcmp(&a->format, &b->format, offsetof(WAVEFORMATEX, cbSize)) != 0) ||
                std::strcmp(a->GetFileName(), b->GetFileName()) != 0 ||
                g_origWorld.IdOf(a->buffer) != g_portWorld.IdOf(b->buffer))
                Fail("source fields (%s / %s)", a->GetFileName(), b->GetFileName());
            if (a->IsOpen()) {
                void *pa = nullptr, *pb = nullptr;
                Check("Lock", a->Lock(&pa), b->Lock(&pb));
                Check("Unlock", a->Unlock(), b->Unlock());
                const int ia = g_origWorld.IdOf(a->buffer), ib = g_portWorld.IdOf(b->buffer);
                if (ia >= 0 && ib >= 0 && *g_origWorld.buffers[ia]->data != *g_portWorld.buffers[ib]->data)
                    Fail("sample data");
            }
            CompareState();
            if (variant == 0 || !a->IsOpen()) {
                Check("Release", a->Release(), b->Release());
                CompareState();
            } else {
                g_orig.sources.push_back(a);
                g_port.sources.push_back(b);
            }
        }
    }
    g_orig.driver->flags = g_port.driver->flags = 0;
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 20000;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 3)
        g_rng.seed(static_cast<unsigned>(std::atoi(argv[3])));
    char dir[MAX_PATH];
    std::strcpy(dir, orig);
    if (char *slash = std::strrchr(dir, '\\'))
        *slash = 0;
    SetDllDirectoryA(dir);
    HMODULE dll = LoadLibraryA(orig);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", orig, GetLastError());
        return 2;
    }
    g_base = reinterpret_cast<uint8_t *>(dll);
    if (!PatchImport(dll, "WINMM.dll", "timeGetTime", reinterpret_cast<void *>(&FakeTimeGetTime)) ||
        !PatchImport(GetModuleHandleA(nullptr), "WINMM.dll", "timeGetTime", reinterpret_cast<void *>(&FakeTimeGetTime))) {
        std::printf("cannot patch timeGetTime\n");
        return 2;
    }
    ls3d::g_logDisabled = true;
    Orig<uint8_t>(0x100d16f2) = 1;

    g_origDriver = reinterpret_cast<ISND_driver *(__cdecl *)()>(GetProcAddress(dll, "ISndGetDriver"))();
    g_sndDriver = new ISND_driver;
    if (!g_origDriver) {
        std::printf("the original has no driver\n");
        return 2;
    }
    Setup(g_origDriver, g_origWorld, 0);
    Setup(g_sndDriver, g_portWorld, 0);
    Orig<bool>(0x100d2c60) = true;
    snd::g_initialized = true;

    g_orig = {&g_origWorld, g_origDriver, {}, {}, {}, &Orig<C_vector<S_voice>>(0x100d2ca0),
              &Orig<C_vector<S_suspendedVoice>>(0x100d2c70), &Orig<int>(0x100d2c64), &Orig<bool>(0x100d2c60),
              reinterpret_cast<void (*)()>(g_base + (0x1007b8e0 - 0x10000000))};
    g_port = {&g_portWorld, g_sndDriver, {}, {}, {}, &g_voices, &g_suspendedVoices, &g_maxVoices, &snd::g_initialized,
              &snd::UpdateVoices};
    g_haveI3D = Orig<void *>(0x100d16f8) != nullptr;

    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string folder = std::string(tmp) + "ls3df_snd\\";
    CreateDirectoryA(folder.c_str(), nullptr);
    struct S_file { int channels, bits, rate, ms; bool extra; int fmtSize; };
    static const S_file kFiles[] = {
        {1, 8, 11025, 20000, false, 16}, {2, 16, 22050, 5000, false, 16}, {1, 16, 44100, 3000, true, 18},
        {1, 8, 8000, 500, false, 16},    {2, 8, 11025, 9000, true, 16},   {1, 16, 22050, 15000, false, 16},
    };
    for (size_t i = 0; i < sizeof kFiles / sizeof kFiles[0]; ++i) {
        const S_file &f = kFiles[i];
        g_files.push_back(folder + "test" + std::to_string(i) + ".wav");
        WriteWav(g_files.back(), f.channels, f.bits, f.rate, f.ms, f.extra, f.fmtSize);
    }
    g_files.push_back(folder + "missing.wav");

    // The game runs the FPU in single precision (Direct3D's default).
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);

    SourceTest();
    std::printf("sources: %zu open\n", g_orig.sources.size());
    ListenerTest(rounds / 10);

    for (int k = 0; k < 8; ++k) {
        const bool threeD = (k & 1) != 0;
        g_orig.sounds.push_back(NewSound(g_orig, threeD));
        g_port.sounds.push_back(NewSound(g_port, threeD));
        g_orig.is3D.push_back(threeD);
        g_port.is3D.push_back(threeD);
    }
    *g_orig.maxVoices = *g_port.maxVoices = 3;
    for (int r = 0; r < rounds; ++r) {
        SoundStep(r);
        if (g_failures > 20)
            break;
    }
    std::printf("coverage: up to %d voices, %d suspended, %d suspended-list shrinks\n", g_maxVoicesSeen,
                g_maxSuspendedSeen, g_resumes);
    std::printf("isnd: %d rounds, %d differences (driver volume %s)\n", rounds, g_failures,
                g_haveI3D ? "tested" : "untested");
    return g_failures ? 1 : 0;
}
