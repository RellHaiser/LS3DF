// ISND voice manager: the per-frame update and the statistics overlay (0x1007b640 - 0x1007bcd0).
#include "isnd/isnd_voices.h"

#include <cstdio>

#include <mmsystem.h>

#include "core/fpu.h"
#include "i3d/i3d_links.h"

namespace ls3d::snd {

int g_maxVoices = 0;
C_vector<S_voice> g_voices(0x40);                       // static initializer 0x1007b640
C_vector<S_suspendedVoice> g_suspendedVoices(0x200);    // static initializer 0x1007b690

bool IsManaged(const ISND_sound *sound)
{
    return g_maxVoices > 0 && !(g_sndDriver->flags & ISND_INIT_SOFTWARE) && !(sound->flags & ISNDF_SOFTWARE);
}

int FindVoice(const ISND_sound *sound)
{
    for (int i = g_voices.Size(); i != 0;) {
        --i;
        if (g_voices[i].sound == sound)
            return i;
    }
    return -1;
}

int FindSuspendedVoice(const ISND_sound *sound)
{
    for (int i = g_suspendedVoices.Size(); i != 0;) {
        --i;
        if (g_suspendedVoices[i].sound == sound)
            return i;
    }
    return -1;
}

// (The original divides by nBlockAlign unchecked; a zero no longer faults.)
int MsToBufferOffset(const ISND_source *source, uint32_t ms)
{
    const int blockAlign = source->format.nBlockAlign;
    const int bytes = Ftol(static_cast<float>(static_cast<double>(ms) * source->bytesPerMs));
    if (blockAlign == 0)
        return 0;
    const int offset = (bytes / blockAlign - 1) * blockAlign;
    return offset < 0 ? 0 : offset;
}

void UpdateVoices()
{
    if (g_maxVoices == 0)
        return;
    const uint32_t now = timeGetTime();

    // Voices of one-shot sounds that have played to their end are released.
    for (int i = g_voices.Size(); i != 0;) {
        --i;
        S_voice &voice = g_voices[i];
        if (voice.loop == 0 && voice.pauseTime == 0) {
            const int length = voice.sound->GetSource()->GetLength();
            if (static_cast<int>(now - voice.pausedTotal - voice.startTime) >= length) {
                audio::g_pAudio->Stop(voice.buffer);
                g_voices.RemoveAtSwap(i);
            }
        }
    }

    if (g_suspendedVoices.Size() == 0)
        return;
    // Suspended one-shot sounds whose time has run out are dropped. (The loop starts one entry
    // before the last, as the original's: the last one is never checked.)
    for (int i = g_suspendedVoices.Size() - 1; i != 0;) {
        --i;
        S_suspendedVoice &suspended = g_suspendedVoices[i];
        if (suspended.loop == 0) {
            ISND_source *source = suspended.sound->GetSource();
            const float ratio = suspended.sound->frequency;
            const float elapsed = static_cast<float>(suspended.position - suspended.suspendTime + now);
            const uint32_t length = static_cast<uint32_t>(source->GetLength());
            if (static_cast<float>(static_cast<double>(length) / ratio) <= elapsed)
                g_suspendedVoices.RemoveAtSwap(i);
        }
    }

    if (g_maxVoices <= g_voices.Size())
        return;
    // A voice is free: the suspended sound of the highest priority (then the loudest) resumes.
    int best = g_suspendedVoices.Size() - 1;
    for (int i = best; i != 0;) {
        --i;
        const S_suspendedVoice &candidate = g_suspendedVoices[i];
        const S_suspendedVoice &current = g_suspendedVoices[best];
        if (candidate.priority > current.priority ||
            (candidate.priority == current.priority && current.sound->volume < candidate.sound->volume))
            best = i;
    }

    S_suspendedVoice &resumed = g_suspendedVoices[best];
    S_voice voice;
    voice.sound = resumed.sound;
    voice.buffer = resumed.buffer;
    voice.priority = resumed.priority;
    voice.loop = resumed.loop;
    voice.startTime = now;
    voice.pauseTime = 0;
    voice.pausedTotal = 0;
    S_voice &added = g_voices.PushBack(voice);

    ISND_source *source = resumed.sound->GetSource();
    uint32_t elapsed = resumed.position - resumed.suspendTime + now;
    DWORD playFlags = 0;
    if (resumed.loop) {
        const uint32_t length = static_cast<uint32_t>(
            Ftol(static_cast<float>(static_cast<double>(static_cast<uint32_t>(source->GetLength())) /
                                    resumed.sound->frequency)));
        // (The original divides unchecked; a zero length no longer faults.)
        elapsed = length ? elapsed % length : 0;
        playFlags = DSBPLAY_LOOPING;
    }
    added.startTime = now - elapsed;
    audio::g_pAudio->SetCurrentPosition(resumed.buffer, static_cast<DWORD>(MsToBufferOffset(source, elapsed)));
    audio::g_pAudio->Play(resumed.buffer, 0, 0, playFlags);
    g_suspendedVoices.RemoveAtSwap(best);
}

void DrawStats()
{
    char text[256];
    int y = 200;
    for (int i = 0; i < g_voices.Size(); ++i) {
        const S_voice &voice = g_voices[i];
        // (The pause time is measured with GetTickCount here, timeGetTime everywhere else.)
        const uint32_t paused = voice.pauseTime ? GetTickCount() - voice.pauseTime : 0;
        ISND_sound *sound = voice.sound;
        const float frequency = sound->frequency;
        const float volume = sound->volume;
        const char *name = sound->GetSource()->GetFileName();
        std::snprintf(text, sizeof text, "%02i:[%c;P% 3lu;V%.02f;F%.02f;T% 5lu;PT:% 5lu]0x%08X,%s", i,
                      voice.loop ? 'L' : ' ', static_cast<unsigned long>(voice.priority), volume, frequency,
                      static_cast<unsigned long>(sound->GetPlayPosition()),
                      static_cast<unsigned long>(voice.pausedTotal + paused),
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(sound)), name);
        i3d::DrawText2D(0, y, text, 0, 0.65f);
        y += 11;
    }

    std::snprintf(text, sizeof text, "***** TerminatedChanelList size: %i *****", g_suspendedVoices.Size());
    i3d::DrawText2D(0, g_voices.Size() * 11 + 215, text, 0, 0.65f);
    for (int i = 0; i < g_suspendedVoices.Size(); ++i) {
        const S_suspendedVoice &suspended = g_suspendedVoices[i];
        const float volume = suspended.sound->volume;
        const char *name = suspended.sound->GetSource()->GetFileName();
        std::snprintf(text, sizeof text, "%02i:[%c;% 3lu;%.02f]0x%08X,%s", i, suspended.loop ? 'L' : ' ',
                      static_cast<unsigned long>(suspended.priority), volume,
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(suspended.sound)), name);
        i3d::DrawText2D(0, (g_voices.Size() + i) * 11 + 230, text, 0x3C, 0.65f);
    }
}

}  // namespace ls3d::snd
