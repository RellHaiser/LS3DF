// ISND_driver: device creation, EAX detection, the primary buffer and the object factories
// (0x1007a7a0 - 0x1007b640).
#include "isnd/isnd.h"

#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "core/i3d_result.h"
#include "i3d/i3d_links.h"
#include "isnd/eax.h"
#include "isnd/isnd_voices.h"

ISND_driver *g_sndDriver = nullptr;

extern "C" ISND_driver *__cdecl ISndGetDriver()
{
    return g_sndDriver;
}

namespace ls3d::snd {

bool g_initialized = false;

namespace {

// DirectSoundEnumerate callback (0x1007af10): logs every device but the primary one, with the
// version of its driver file.
BOOL CALLBACK LogDevice(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID)
{
    if (!guid)
        return TRUE;
    dbgPrintf("");
    dbgPrintf("        Driver: %s", module);
    dbgPrintf("   Description: %s", description);

    bool found = false;
    char windows[MAX_PATH];
    char path[MAX_PATH];
    const UINT length = GetWindowsDirectoryA(windows, MAX_PATH);
    if (static_cast<int>(length) > 0 && std::strlen(module) + length + 0x14 < MAX_PATH) {
        DWORD handle = 0;
        std::strcpy(path, windows);
        std::strcat(path, "\\system32\\drivers\\");
        std::strcat(path, module);
        DWORD size = GetFileVersionInfoSizeA(path, &handle);
        if (!size) {
            std::strcpy(path, windows);
            std::strcat(path, "\\system\\");
            std::strcat(path, module);
            size = GetFileVersionInfoSizeA(path, &handle);
        }
        if (size) {
            void *data = ::operator new(size);
            if (GetFileVersionInfoA(path, 0, size, data)) {
                VS_FIXEDFILEINFO *info = nullptr;
                UINT infoSize = 0;
                if (VerQueryValueA(data, "\\", reinterpret_cast<void **>(&info), &infoSize)) {
                    dbgPrintf("Driver version: %d.%02d.%02d.%04d", HIWORD(info->dwFileVersionMS),
                              LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                              LOWORD(info->dwFileVersionLS));
                    found = true;
                }
            }
            ::operator delete(data);
        }
    }
    if (!found)
        dbgPrintf("Driver version: ?.??.??.????");
    return TRUE;
}

}  // namespace
}  // namespace ls3d::snd

using namespace ls3d;

ISND_driver::ISND_driver()
    : window(nullptr), unknown08(1), dsound(nullptr), eax(nullptr), eaxBuffer(nullptr), externalModule(nullptr),
      unknown1C(0), externalApi(nullptr), unknown24(0), primary(nullptr), listener(nullptr), flags(0), volume(0.0f),
      maxFrequency(0), minFrequency(0), sources(0x20), unknown50(0), unknown54(0)
{
}

ISND_driver::~ISND_driver()
{
    if (snd::g_initialized)
        dbgPrintf("!! ISND was initialised, but not closed");
    sources.Free();
}

HRESULT ISND_driver::InitFromDesc(const S_ISND_init *init)
{
    if (!init)
        return I3DERR_INVALIDPARAMS;
    return Init(init->window, init->frequency, init->channels, init->bits, init->flags);
}

HRESULT ISND_driver::Init(HWND hwnd, int frequency, int channels, int bits, uint32_t initFlags)
{
    if (!OpenDevice(hwnd, frequency, channels, bits, initFlags))
        return I3DERR_GENERIC;
    window = hwnd;
    snd::g_initialized = true;
    volume = 1.0f;
    return I3D_OK;
}

bool ISND_driver::OpenDevice(HWND hwnd, int frequency, int channels, int bits, uint32_t initFlags)
{
    flags = initFlags;
    dbgPrintf("Sound hardware enumeration:");
    if (FAILED(DirectSoundEnumerateA(snd::LogDevice, nullptr)))
        return false;
    dbgPrintf("\nTrying to use default sound device with mix freqency: %i Hz, %i channels, %i bps.", frequency,
              channels, bits);

    bool created = false;
    if (!(flags & ISND_INIT_NO_EAX)) {
        CoInitialize(nullptr);
        if (FAILED(CoCreateInstance(snd::kClsidEaxDirectSound8, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectSound8,
                                    reinterpret_cast<void **>(&dsound)))) {
            dbgPrintf("EAX Unified was not found. Trying to use normal DirectSound initialization...");
        } else if (SUCCEEDED(dsound->Initialize(nullptr))) {
            created = true;
        } else {
            // (The original overwrote the object without releasing it.)
            dsound->Release();
            dsound = nullptr;
        }
    }
    if (!created && FAILED(DirectSoundCreate8(nullptr, &dsound, nullptr))) {
        dbgPrintf("Sound initialization FAILED !");
        return false;
    }

    DSCAPS caps = {};
    caps.dwSize = sizeof caps;
    dsound->GetCaps(&caps);
    maxFrequency = caps.dwMaxSecondarySampleRate;
    minFrequency = caps.dwMinSecondarySampleRate;
    if (minFrequency == maxFrequency || minFrequency > 10000 || minFrequency == 0 || maxFrequency < 40000) {
        dbgPrintf("Your soundcard claims WRONG frequency bounds !! Minimal frequency: %iHz, Maximal frequency: %iHz",
                  minFrequency, maxFrequency);
        minFrequency = 10000;
    }
    if (minFrequency < 8000)
        minFrequency = 8000;
    if (maxFrequency < 44000)
        maxFrequency = 44000;

    if (!(flags & ISND_INIT_SOFTWARE)) {
        snd::g_maxVoices = static_cast<int>(caps.dwFreeHw3DAllBuffers);
        dbgPrintf("Sound management: %i free 3D channels claimed.", snd::g_maxVoices);
        if (snd::g_maxVoices > 35)
            snd::g_maxVoices = 30;
        InitEax();
    }

    if (FAILED(dsound->SetCooperativeLevel(hwnd, DSSCL_PRIORITY)))
        return false;
    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof desc;
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
    if (FAILED(dsound->CreateSoundBuffer(&desc, &primary, nullptr)))
        return false;
    if (SetPrimaryFormat(frequency, channels, bits) != I3D_OK)
        return false;
    if (FAILED(primary->QueryInterface(IID_IDirectSound3DListener, reinterpret_cast<void **>(&listener))))
        return false;

    if (eax && !(flags & ISND_INIT_NO_EAX)) {
        // Start without reverb (room -10000).
        snd::S_eaxListenerProperties props = {};
        props.environment = 26;
        props.environmentSize = 7.5f;
        props.environmentDiffusion = 1.0f;
        props.room = -10000;
        props.roomHF = -100;
        props.roomLF = 0;
        props.decayTime = 1.49f;
        props.decayHFRatio = 0.83f;
        props.decayLFRatio = 1.0f;
        props.reflections = -2602;
        props.reflectionsDelay = 0.007f;
        props.reverb = 200;
        props.reverbDelay = 0.011f;
        props.echoTime = 0.25f;
        props.echoDepth = 0.0f;
        props.modulationTime = 0.25f;
        props.modulationDepth = 0.0f;
        props.airAbsorptionHF = -5.0f;
        props.hfReference = 5000.0f;
        props.lfReference = 250.0f;
        props.roomRolloffFactor = 0.0f;
        props.flags = 0x3F;
        eax->Set(snd::kEaxListenerProperties, snd::kEaxAllParameters, nullptr, 0, &props, sizeof props);
    }

    const char *speakers;
    if (initFlags & ISND_INIT_HEADPHONES) {
        dsound->SetSpeakerConfig(DSSPEAKER_HEADPHONE);
        speakers = "Speaker configuration: STEREO HEADPHONES";
    } else if (initFlags & ISND_INIT_QUAD) {
        dsound->SetSpeakerConfig(DSSPEAKER_QUAD);
        speakers = "Speaker configuration: QUADRAPHONIC SPEAKERS";
    } else if (initFlags & ISND_INIT_SURROUND_5_1) {
        dsound->SetSpeakerConfig(DSSPEAKER_5POINT1);
        speakers = "Speaker configuration: 5.1 SURROUND SOUND SPEAKERS";
    } else if (initFlags & ISND_INIT_STEREO) {
        dsound->SetSpeakerConfig(DSSPEAKER_STEREO);
        speakers = "Speaker configuration: STEREO SPEAKERS";
    } else {
        speakers = "Speaker configuration: CURRENT WINDOWS SPEAKER CONFIGURATION";
    }
    dbgPrintf(speakers);
    dbgPrintf("");
    return true;
}

bool ISND_driver::InitEax()
{
    if (!dsound)
        return false;
    if (!eax) {
        if (!eaxBuffer) {
            // A tiny hardware 3D buffer: EAX Unified exposes its property set through 3D buffers.
            WAVEFORMATEX format = {};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 1;
            format.nSamplesPerSec = 22050;
            format.nAvgBytesPerSec = 44100;
            format.nBlockAlign = 2;
            format.wBitsPerSample = 16;
            DSBUFFERDESC desc = {};
            desc.dwSize = sizeof desc;
            desc.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_STATIC | DSBCAPS_LOCHARDWARE;
            desc.dwBufferBytes = 0x40;
            desc.lpwfxFormat = &format;
            IDirectSoundBuffer *buffer = nullptr;
            if (dsound->CreateSoundBuffer(&desc, &buffer, nullptr) != DS_OK)
                return false;
            IDirectSoundBuffer8 *buffer8 = nullptr;
            if (FAILED(buffer->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void **>(&buffer8)))) {
                buffer->Release();
                return false;
            }
            if (FAILED(buffer8->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void **>(&eaxBuffer)))) {
                buffer->Release();
                buffer8->Release();
                return false;
            }
            // (The original releases these two after the block below, also when eaxBuffer
            // already existed and they were never set.)
            buffer->Release();
            buffer8->Release();
        }
        if (FAILED(eaxBuffer->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void **>(&eax)))) {
            eaxBuffer->Release();
            eaxBuffer = nullptr;
            return false;
        }
    }
    if (!(flags & ISND_INIT_NO_EAX)) {
        ULONG support = 0;
        if (SUCCEEDED(eax->QuerySupport(snd::kEaxListenerProperties, snd::kEaxAllParameters, &support)) &&
            (support & KSPROPERTY_SUPPORT_GET) && (support & KSPROPERTY_SUPPORT_SET) &&
            SUCCEEDED(eax->QuerySupport(snd::kEaxBufferProperties, snd::kEaxAllParameters, &support)) &&
            (support & KSPROPERTY_SUPPORT_GET) && (support & KSPROPERTY_SUPPORT_SET)) {
            dbgPrintf("Using EAX 3.0 through EAX Unified.");
            return true;
        }
        flags |= ISND_INIT_NO_EAX;
    }
    return true;
}

HRESULT ISND_driver::Close()
{
    if (sources.Size() > 0) {
        dbgPrintf("Unreleased sound sources: %i", sources.Size());
        for (int i = 0; i < sources.Size(); ++i)
            dbgPrintf("\t\t%s", sources[i]->GetFileName());
    }
    if (dsound) {
        if (externalApi) {
            ::operator delete(externalApi);
            externalApi = nullptr;
        }
        if (externalModule) {
            FreeLibrary(externalModule);
            externalModule = nullptr;
        }
        if (listener) {
            listener->Release();
            listener = nullptr;
        }
        if (eax) {
            eax->Release();
            eax = nullptr;
        }
        if (eaxBuffer) {
            eaxBuffer->Release();
            eaxBuffer = nullptr;
        }
        if (primary) {
            primary->Release();
            primary = nullptr;
        }
        dsound->Release();
        dsound = nullptr;
    }
    snd::g_initialized = false;
    return I3D_OK;
}

bool ISND_driver::IsInitialized()
{
    return snd::g_initialized;
}

HRESULT ISND_driver::SetPrimaryFormat(int frequency, int channels, int bits)
{
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(frequency);
    format.wBitsPerSample = static_cast<WORD>(bits);
    format.nBlockAlign = static_cast<WORD>((static_cast<WORD>(bits) >> 3) * channels);
    format.nAvgBytesPerSec = format.nBlockAlign * static_cast<DWORD>(frequency);
    return SUCCEEDED(primary->SetFormat(&format)) ? I3D_OK : I3DERR_INVALIDPARAMS;
}

HRESULT ISND_driver::SetVolume(float value)
{
    if (snd::g_initialized && !(std::fabs(value - volume) < 1e-8f)) {
        if (!(value < 1.0f))
            value = 1.0f;
        else if (value <= 0.0f)
            value = 0.0f;
        volume = value;
        i3d::RefreshSoundVolumes();
    }
    return I3D_OK;
}

HRESULT ISND_driver::Reserved18(int)
{
    return I3D_OK;
}

HRESULT ISND_driver::CreateSource(ISND_source **out)
{
    if (!out)
        return I3DERR_INVALIDPARAMS;
    *out = NewSource();
    return *out ? I3D_OK : I3DERR_OUTOFMEM;
}

ISND_source *ISND_driver::NewSource()
{
    return new (std::nothrow) ISND_source;
}

HRESULT ISND_driver::CreateSound(ISND_sound **out)
{
    if (!out)
        return I3DERR_INVALIDPARAMS;
    *out = NewSound();
    return *out ? I3D_OK : I3DERR_OUTOFMEM;
}

ISND_sound *ISND_driver::NewSound()
{
    return new (std::nothrow) ISND_sound;
}

HRESULT ISND_driver::Create3DSound(ISND_3Dsound **out)
{
    if (!out)
        return I3DERR_INVALIDPARAMS;
    *out = New3DSound();
    return *out ? I3D_OK : I3DERR_OUTOFMEM;
}

ISND_3Dsound *ISND_driver::New3DSound()
{
    return new (std::nothrow) ISND_3Dsound;
}

HRESULT ISND_driver::CreateListener(ISND_3Dlistener **out)
{
    if (!out)
        return I3DERR_INVALIDPARAMS;
    *out = NewListener();
    return *out ? I3D_OK : I3DERR_OUTOFMEM;
}

ISND_3Dlistener *ISND_driver::NewListener()
{
    ISND_3Dlistener *result = new (std::nothrow) ISND_3Dlistener;
    if (!result)
        return nullptr;
    if (listener)
        listener->GetAllParameters(&result->params);
    return result;
}
