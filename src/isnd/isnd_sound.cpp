// ISND_sound and ISND_3Dsound (0x1007a430 - 0x1007a7a0, 0x1007bcd0 - 0x1007ccf0).
#include "isnd/isnd.h"

#include <cmath>

#include <mmsystem.h>

#include "core/debug.h"
#include "core/fpu.h"
#include "core/i3d_result.h"
#include "igraph/igraph_state.h"
#include "isnd/isnd_voices.h"

using namespace ls3d;
using namespace ls3d::snd;

namespace ls3d::snd {

void TraceDSErrors(HRESULT hr)
{
    const char *text = nullptr;
    switch (hr) {
    case DSERR_CONTROLUNAVAIL: text = "TraceDSErrors() - DSERR_CONTROLUNAVAIL"; break;
    case DSERR_ACCESSDENIED: text = "TraceDSErrors() - DSERR_ACCESSDENIED"; break;
    case DSERR_GENERIC: text = "TraceDSErrors() - DSERR_GENERIC"; break;
    case DSERR_UNSUPPORTED: text = "TraceDSErrors() - DSERR_UNSUPPORTED"; break;
    case DSERR_NOINTERFACE: text = "TraceDSErrors() - DSERR_NOINTERFACE"; break;
    case DSERR_NOAGGREGATION: text = "TraceDSErrors() - DSERR_NOAGGREGATION"; break;
    case DSERR_OUTOFMEMORY: text = "TraceDSErrors() - DSERR_OUTOFMEMORY"; break;
    case DSERR_INVALIDPARAM: text = "TraceDSErrors() - DSERR_INVALIDPARAM"; break;
    case DSERR_ALLOCATED: text = "TraceDSErrors() - DSERR_ALLOCATED"; break;
    case DSERR_INVALIDCALL: text = "TraceDSErrors() - DSERR_INVALIDCALL"; break;
    case DSERR_PRIOLEVELNEEDED: text = "TraceDSErrors() - DSERR_PRIORLEVELNEEDED"; break;
    case DSERR_BADFORMAT: text = "TraceDSErrors() - DSERR_BADFORMAT"; break;
    case DSERR_NODRIVER: text = "TraceDSErrors() - DSERR_NODRIVER"; break;
    case DSERR_ALREADYINITIALIZED: text = "TraceDSErrors() - DSERR_ALREADYINITIALIZED"; break;
    case DSERR_BUFFERLOST: text = "TraceDSErrors() - DSERR_BUFFERLOST"; break;
    case DSERR_OTHERAPPHASPRIO: text = "TraceDSErrors() - DSERR_OTHERAPPHASPRIO"; break;
    case DSERR_UNINITIALIZED: text = "TraceDSErrors() - DSERR_UNINITIALIZED"; break;
    default: break;
    }
    if (text)
        dbgPrintf(text);
}

}  // namespace ls3d::snd

// ISND_sound ----------------------------------------------------------------------------------

ISND_sound::ISND_sound()
    : refCount(1), buffer(nullptr), source(nullptr), flags(0), frequency(1.0f), volume(1.0f)
{
    // (The original leaves `flags` uninitialised until Open.)
}

ISND_sound::~ISND_sound()
{
    Stop();
    if (buffer) {
        audio::g_pAudio->Release(buffer);
        buffer = nullptr;
    }
    if (source) {
        source->Release();
        source = nullptr;
    }
    flags = 0;
}

int ISND_sound::Release()
{
    const int refs = --refCount;
    if (refs < 0) {
        dbgPrintf("!! RefCount<0  this:0x%x  ISND_sound", this);
        return 0;
    }
    if (refs == 0) {
        delete this;
        return 0;
    }
    return refs;
}

HRESULT ISND_sound::Open(ISND_source *newSource, uint32_t openFlags)
{
    Stop();
    if (buffer) {
        audio::g_pAudio->Release(buffer);
        buffer = nullptr;
    }
    if (source) {
        source->Release();
        source = nullptr;
    }
    flags = 0;
    if (!newSource->buffer)
        return I3DERR_GENERIC;
    const HRESULT hr = audio::g_pAudio->DuplicateSoundBuffer(g_sndDriver->dsound, newSource->buffer, &buffer);
    if (FAILED(hr)) {
        TraceDSErrors(hr);
        return I3DERR_GENERIC;
    }
    source = newSource;
    ++newSource->refCount;
    flags = openFlags;
    return I3D_OK;
}

HRESULT ISND_sound::Close()
{
    Stop();
    if (buffer) {
        audio::g_pAudio->Release(buffer);
        buffer = nullptr;
    }
    if (source) {
        source->Release();
        source = nullptr;
    }
    flags = 0;
    return I3D_OK;
}

HRESULT ISND_sound::Play(uint32_t playFlags, uint32_t priority)
{
    if (!buffer)
        return I3DERR_GENERIC;
    const bool keepPosition = (playFlags & ISNDP_KEEP_POSITION) != 0;
    if (!keepPosition)
        audio::g_pAudio->SetCurrentPosition(buffer, 0);

    DWORD dsFlags = 0;
    DWORD dsPriority = 0;
    if (!(g_sndDriver->flags & ISND_INIT_SOFTWARE) && !(flags & ISNDF_SOFTWARE)) {
        dsFlags = DSBPLAY_TERMINATEBY_TIME | DSBPLAY_TERMINATEBY_PRIORITY;
        dsPriority = priority;
        if (g_maxVoices > 0) {
            int index = FindVoice(this);
            if (index >= 0) {
                // Already holds a voice: restart it.
                S_voice &voice = g_voices[index];
                voice.priority = priority;
                voice.loop = playFlags & ISNDP_LOOP;
                voice.startTime = timeGetTime();
                voice.pauseTime = 0;
                voice.pausedTotal = 0;
                if (keepPosition)
                    voice.startTime -= GetPlayPosition();
            } else {
                index = FindSuspendedVoice(this);
                if (index >= 0)
                    g_suspendedVoices.RemoveAtSwap(index);

                if (g_maxVoices > g_voices.Size()) {
                    S_voice voice;
                    voice.sound = this;
                    voice.buffer = buffer;
                    voice.priority = priority;
                    voice.loop = playFlags & ISNDP_LOOP;
                    voice.startTime = timeGetTime();
                    voice.pauseTime = 0;
                    voice.pausedTotal = 0;
                    S_voice &added = g_voices.PushBack(voice);
                    if (keepPosition)
                        added.startTime -= GetPlayPosition();
                } else {
                    // All voices taken: a stopped one, else the last of lower priority (equal
                    // priority: the louder one, as the original compares) gives way.
                    S_voice *victim = nullptr;
                    uint32_t lowest = priority;
                    for (int i = g_voices.Size(); i != 0;) {
                        --i;
                        S_voice &voice = g_voices[i];
                        DWORD status = 0;
                        audio::g_pAudio->GetStatus(voice.buffer, &status);
                        if (!(status & DSBSTATUS_PLAYING)) {
                            victim = &voice;
                            break;
                        }
                        if (voice.priority < lowest ||
                            (voice.priority == lowest && victim && victim->sound->volume < voice.sound->volume)) {
                            lowest = voice.priority;
                            victim = &voice;
                        }
                    }

                    S_suspendedVoice suspended;
                    if (!victim) {
                        // Nothing to take over: this sound waits for a voice.
                        suspended.sound = this;
                        suspended.buffer = buffer;
                        suspended.priority = priority;
                        suspended.loop = playFlags & ISNDP_LOOP;
                        audio::g_pAudio->GetCurrentPosition(buffer, &suspended.position, nullptr);
                        suspended.suspendTime = timeGetTime();
                        g_suspendedVoices.PushBack(suspended);
                        return I3D_OK;
                    }
                    suspended.sound = victim->sound;
                    suspended.buffer = victim->buffer;
                    suspended.priority = victim->priority;
                    suspended.loop = victim->loop;
                    // (The original reads the play cursor of this sound's buffer, not the
                    // suspended one's; kept.)
                    audio::g_pAudio->GetCurrentPosition(buffer, &suspended.position, nullptr);
                    suspended.suspendTime = timeGetTime();
                    g_suspendedVoices.PushBack(suspended);
                    audio::g_pAudio->Stop(victim->buffer);

                    victim->sound = this;
                    victim->priority = priority;
                    victim->buffer = buffer;
                    victim->loop = playFlags & ISNDP_LOOP;
                    victim->startTime = suspended.suspendTime;
                    victim->pauseTime = 0;
                    victim->pausedTotal = 0;
                    if (keepPosition)
                        victim->startTime -= GetPlayPosition();
                }
            }
        }
    }

    const bool loop = (playFlags & ISNDP_LOOP) != 0;
    if (SUCCEEDED(audio::g_pAudio->Play(buffer, 0, dsPriority, loop ? dsFlags | DSBPLAY_LOOPING : dsFlags)))
        return I3D_OK;
    if (ig::g_logSoundFailures) {
        ISND_source *playing = GetSource();
        const DWORD now = timeGetTime();
        const char *name = GetSource()->GetFileName();
        dbgPrintf("[%i,%08X]CANT PLAY channel: P:%i, L:%i, CPos:%i/%i, %s", now,
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(this)), dsPriority,
                  loop ? ISNDP_LOOP : 0, GetPlayPosition(), playing->GetLength(), name);
    }
    // Again in software.
    const DWORD retryFlags = loop ? DSBPLAY_LOOPING | DSBPLAY_LOCSOFTWARE : DSBPLAY_LOCSOFTWARE;
    return SUCCEEDED(audio::g_pAudio->Play(buffer, 0, 0, retryFlags)) ? I3D_OK : I3DERR_GENERIC;
}

HRESULT ISND_sound::OpenAndPlay(ISND_source *newSource, uint32_t openFlags)
{
    HRESULT hr = Open(newSource, openFlags);
    if (hr == I3D_OK)
        hr = Play(openFlags, 0);
    return hr;
}

HRESULT ISND_sound::Stop()
{
    flags &= ~ISNDF_PAUSED;
    if (!buffer)
        return I3DERR_GENERIC;
    audio::g_pAudio->Stop(buffer);
    if (IsManaged(this)) {
        const int index = FindVoice(this);
        if (index >= 0)
            g_voices.RemoveAtSwap(index);
        const int suspended = FindSuspendedVoice(this);
        if (suspended >= 0)
            g_suspendedVoices.RemoveAtSwap(suspended);
    }
    return I3D_OK;
}

int ISND_sound::GetPlayPosition()
{
    if (!buffer)
        return 0;
    DWORD position = 0;
    audio::g_pAudio->GetCurrentPosition(buffer, &position, nullptr);
    return Ftol(static_cast<float>(static_cast<double>(position) / source->bytesPerMs));
}

HRESULT ISND_sound::SetPlayPosition(int position)
{
    if (!buffer)
        return I3DERR_GENERIC;
    if (position == ISND_PAUSE) {
        audio::g_pAudio->Stop(buffer);
        flags |= ISNDF_PAUSED;
        if (IsManaged(this)) {
            const int index = FindVoice(this);
            if (index >= 0)
                g_voices[index].pauseTime = timeGetTime();
        }
        return I3D_OK;
    }
    if (position == ISND_RESUME) {
        if (flags & ISNDF_PAUSED) {
            // (Resumes without DSBPLAY_LOOPING: a paused looping sound plays to its end once; kept.)
            audio::g_pAudio->Play(buffer, 0, 0, 0);
            flags &= ~ISNDF_PAUSED;
            const int index = FindVoice(this);
            if (index >= 0) {
                S_voice &voice = g_voices[index];
                voice.pausedTotal += timeGetTime() - voice.pauseTime;
                voice.pauseTime = 0;
            }
        }
        return I3D_OK;
    }

    audio::g_pAudio->SetCurrentPosition(buffer,
                                        static_cast<DWORD>(MsToBufferOffset(source, static_cast<uint32_t>(position))));
    if (IsManaged(this)) {
        const int index = FindVoice(this);
        if (index >= 0)
            g_voices[index].startTime = timeGetTime() - position;
        const int suspended = FindSuspendedVoice(this);
        if (suspended >= 0)
            g_suspendedVoices[suspended].suspendTime = timeGetTime() - position;
    }
    return I3D_OK;
}

HRESULT ISND_sound::SetVolume(float value)
{
    if ((flags & ISNDF_NO_VOLUME) || !buffer)
        return I3DERR_GENERIC;
    volume = value;
    const float level = value * g_sndDriver->volume;
    LONG millibels;
    if (level < 1e-8f) {
        millibels = DSBVOLUME_MIN;
    } else if (!(level < 1.0f)) {
        audio::g_pAudio->SetVolume(buffer, DSBVOLUME_MAX);
        return I3D_OK;
    } else {
        millibels = Ftol(static_cast<float>(std::log(static_cast<double>(level)) * 866.0));
        if (millibels < DSBVOLUME_MIN)
            millibels = DSBVOLUME_MIN;
    }
    audio::g_pAudio->SetVolume(buffer, millibels);
    return I3D_OK;
}

HRESULT ISND_sound::SetVolumeDb(int millibels)
{
    if ((flags & ISNDF_NO_VOLUME) || !buffer)
        return I3DERR_GENERIC;
    const int master = Ftol(static_cast<float>(std::log(static_cast<double>(g_sndDriver->volume)) * -866.0));
    int level = millibels - master;
    if (level < DSBVOLUME_MIN) {
        audio::g_pAudio->SetVolume(buffer, DSBVOLUME_MIN);
        return I3D_OK;
    }
    if (level > 0)
        level = 0;
    audio::g_pAudio->SetVolume(buffer, level);
    return I3D_OK;
}

HRESULT ISND_sound::SetVolumeLinear(float value)
{
    volume = value;
    if ((flags & ISNDF_NO_VOLUME) || !buffer)
        return I3DERR_GENERIC;
    audio::g_pAudio->SetVolume(buffer, Ftol((1.0f - value * g_sndDriver->volume) * -10000.0f));
    return I3D_OK;
}

HRESULT ISND_sound::SetFrequency(float ratio)
{
    if (!(flags & ISNDF_FREQUENCY) || !buffer)
        return I3DERR_GENERIC;
    const float rate = static_cast<float>(static_cast<uint32_t>(source->frequency));
    uint32_t hz = static_cast<uint32_t>(Ftol(ratio * rate));
    if (hz > g_sndDriver->maxFrequency)
        hz = g_sndDriver->maxFrequency;
    if (hz < g_sndDriver->minFrequency)
        hz = g_sndDriver->minFrequency;
    frequency = static_cast<float>(hz) / rate;
    audio::g_pAudio->SetFrequency(buffer, hz);
    return I3D_OK;
}

HRESULT ISND_sound::SetPan(float pan)
{
    if ((flags & ISNDF_NO_PAN) || !buffer)
        return I3DERR_GENERIC;
    if (std::fabs(pan) < 1e-8f) {
        audio::g_pAudio->SetPan(buffer, DSBPAN_CENTER);
        return I3D_OK;
    }
    // (A negative pan gives NaN, converted to 0: centre.)
    int value = Ftol(static_cast<float>(std::log(static_cast<double>(pan)) * 866.0));
    if (value > DSBPAN_RIGHT) {
        audio::g_pAudio->SetPan(buffer, DSBPAN_RIGHT);
        return I3D_OK;
    }
    if (value < DSBPAN_LEFT)
        value = DSBPAN_LEFT;
    audio::g_pAudio->SetPan(buffer, value);
    return I3D_OK;
}

bool ISND_sound::IsPlaying()
{
    if (!buffer)
        return false;
    if (flags & ISNDF_PAUSED)
        return true;
    if (FindSuspendedVoice(this) >= 0)
        return true;
    DWORD status = 0;
    audio::g_pAudio->GetStatus(buffer, &status);
    return (status & DSBSTATUS_PLAYING) != 0;
}

ISND_source *ISND_sound::GetSource()
{
    return source;
}

// ISND_3Dsound --------------------------------------------------------------------------------

ISND_3Dsound::ISND_3Dsound() : open(false), buffer3D(nullptr)
{
}

ISND_3Dsound::~ISND_3Dsound()
{
    Close();
}

HRESULT ISND_3Dsound::Open(ISND_source *newSource, uint32_t openFlags)
{
    if (buffer3D) {
        audio::g_pAudio->Release(buffer3D);
        buffer3D = nullptr;
    }
    const HRESULT hr = ISND_sound::Open(newSource, openFlags);
    if (hr != I3D_OK)
        return hr;
    if (openFlags & ISNDF_3D) {
        if (FAILED(audio::g_pAudio->Query(buffer, &buffer3D)))
            return I3DERR_GENERIC;
    } else if (buffer3D) {
        audio::g_pAudio->Release(buffer3D);
        buffer3D = nullptr;
    }
    open = true;
    return I3D_OK;
}

HRESULT ISND_3Dsound::Close()
{
    open = false;
    if (buffer3D) {
        audio::g_pAudio->Release(buffer3D);
        buffer3D = nullptr;
    }
    ISND_sound::Close();
    return I3D_OK;
}

bool ISND_3Dsound::IsOpen()
{
    return open;
}

HRESULT ISND_3Dsound::SetPosition(const S_vector &position)
{
    if (buffer3D && FAILED(audio::g_pAudio->SetPosition(buffer3D, position.x, position.y, position.z, DS3D_IMMEDIATE)))
        return I3DERR_GENERIC;
    return I3D_OK;
}

HRESULT ISND_3Dsound::SetConeOrientation(const S_vector &direction)
{
    if (buffer3D &&
        FAILED(audio::g_pAudio->SetConeOrientation(buffer3D, direction.x, direction.y, direction.z, DS3D_IMMEDIATE)))
        return I3DERR_GENERIC;
    return I3D_OK;
}

HRESULT ISND_3Dsound::SetVelocity(const S_vector &velocity)
{
    if (buffer3D && FAILED(audio::g_pAudio->SetVelocity(buffer3D, velocity.x, velocity.y, velocity.z, DS3D_IMMEDIATE)))
        return I3DERR_GENERIC;
    return I3D_OK;
}

HRESULT ISND_3Dsound::SetDistances(float minDistance, float maxDistance)
{
    if (buffer3D) {
        if (FAILED(audio::g_pAudio->SetMinDistance(buffer3D, minDistance, DS3D_IMMEDIATE)))
            return I3DERR_GENERIC;
        if (FAILED(audio::g_pAudio->SetMaxDistance(buffer3D, maxDistance, DS3D_IMMEDIATE)))
            return I3DERR_GENERIC;
    }
    return I3D_OK;
}

HRESULT ISND_3Dsound::SetConeAngles(float inside, float outside)
{
    // 180 / pi as the original stores it (0x1009dd9c).
    constexpr float kDegreesPerRadian = 0x1.ca5dc4p+5f;
    if (buffer3D) {
        const DWORD outsideDegrees = static_cast<DWORD>(Ftol(outside * kDegreesPerRadian));
        const DWORD insideDegrees = static_cast<DWORD>(Ftol(inside * kDegreesPerRadian));
        if (FAILED(audio::g_pAudio->SetConeAngles(buffer3D, insideDegrees, outsideDegrees, DS3D_IMMEDIATE)))
            return I3DERR_GENERIC;
    }
    return I3D_OK;
}

HRESULT ISND_3Dsound::SetConeOutsideVolume(float value)
{
    if (buffer3D &&
        FAILED(audio::g_pAudio->SetConeOutsideVolume(buffer3D, Ftol((1.0f - value) * -10000.0f), DS3D_IMMEDIATE)))
        return I3DERR_GENERIC;
    return I3D_OK;
}
