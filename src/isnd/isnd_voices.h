// The hardware voice manager of ISND_sound (isnd_voices.cpp). With a sound card that reports
// free 3D channels (and without ISND_INIT_SOFTWARE), every sound that plays outside ISNDF_SOFTWARE
// holds one of g_maxVoices voices. When all are taken, Play suspends the voice of the lowest
// priority (or itself) into the "terminated channel list"; UpdateVoices retires finished voices
// and resumes the most important suspended one into a free voice.
#pragma once

#include <cstdint>

#include "core/c_vector.h"
#include "isnd/isnd.h"

namespace ls3d::snd {

// A sound holding a voice (0x1C bytes).
struct S_voice {
    ISND_sound *sound;                  // +0x00
    audio::Buffer *buffer;              // +0x04 the sound's buffer
    uint32_t priority;                  // +0x08
    uint32_t loop;                      // +0x0C ISNDP_LOOP bit of the Play flags
    uint32_t startTime;                 // +0x10 timeGetTime() at position 0
    uint32_t pauseTime;                 // +0x14 when paused (0: playing)
    uint32_t pausedTotal;               // +0x18 ms spent paused
};
static_assert(sizeof(S_voice) == 0x1C, "S_voice layout");

// A suspended sound (0x18 bytes).
struct S_suspendedVoice {
    ISND_sound *sound;                  // +0x00
    audio::Buffer *buffer;              // +0x04
    uint32_t priority;                  // +0x08
    // +0x0C The play cursor in bytes when suspended. The resume code reads it as ms (kept).
    DWORD position;
    uint32_t suspendTime;               // +0x10 timeGetTime() when suspended
    uint32_t loop;                      // +0x14
};
static_assert(sizeof(S_suspendedVoice) == 0x18, "S_suspendedVoice layout");

extern int g_maxVoices;                                 // 0x100d2c64 0: no voice management
extern C_vector<S_voice> g_voices;                      // 0x100d2ca0 (grows by 0x40)
extern C_vector<S_suspendedVoice> g_suspendedVoices;    // 0x100d2c70 (grows by 0x200)

// True when `sound`'s plays go through the voice manager.
bool IsManaged(const ISND_sound *sound);

// Index of `sound`'s entry, searched from the end (-1: none).
int FindVoice(const ISND_sound *sound);
int FindSuspendedVoice(const ISND_sound *sound);

// A play cursor in ms as an offset into a buffer of `source`: one block before the aligned
// position (as the original computes it), at least 0.
int MsToBufferOffset(const ISND_source *source, uint32_t ms);

// Per-frame update (0x1007b8e0, IGraph::Flip) and the statistics overlay (0x1007b6e0).
void UpdateVoices();
void DrawStats();

}  // namespace ls3d::snd
