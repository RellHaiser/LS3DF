// The part of RAD Game Tools' Bink video API (binkw32.dll, __stdcall exports "_BinkOpen@8" ...)
// that IShow uses, declared from the original's calls.
#pragma once

#include <cstdint>

// The fields of the BINK handle that IShow reads.
struct BINK {
    uint32_t width;                     // +0x00
    uint32_t height;                    // +0x04
    uint32_t frames;                    // +0x08
    uint32_t frameNum;                  // +0x0C current frame (1-based)
    uint8_t unknown10[0x2bc - 0x10];
    // IShow's millisecond conversions use these two as frame rate and divisor:
    // ms = rateDiv * frameNum * 1000 / rate.
    uint32_t rate;                      // +0x2BC
    uint32_t rateDiv;                   // +0x2C0
};

// BinkCopyToBufferRect destination formats and flags.
enum : uint32_t {
    BINKSURFACE32 = 3,
    BINKSURFACE555 = 9,
    BINKSURFACE565 = 10,
    BINKCOPYALL = 0x80000000,
};

typedef void *(__stdcall *BINKSNDOPEN)(uint32_t param);
typedef BINKSNDOPEN(__stdcall *BINKSNDSYSOPEN)(uint32_t param);

extern "C" {
__declspec(dllimport) BINK *__stdcall BinkOpen(const char *name, uint32_t flags);
__declspec(dllimport) void __stdcall BinkClose(BINK *bink);
__declspec(dllimport) int32_t __stdcall BinkPause(BINK *bink, int32_t pause);
__declspec(dllimport) int32_t __stdcall BinkCopyToBufferRect(BINK *bink, void *dest, int32_t destPitch,
                                                             uint32_t destHeight, uint32_t destX, uint32_t destY,
                                                             uint32_t srcX, uint32_t srcY, uint32_t srcW,
                                                             uint32_t srcH, uint32_t flags);
__declspec(dllimport) void __stdcall BinkGoto(BINK *bink, uint32_t frame, int32_t flags);
__declspec(dllimport) int32_t __stdcall BinkWait(BINK *bink);
__declspec(dllimport) void __stdcall BinkSetVolume(BINK *bink, uint32_t track, int32_t volume);
__declspec(dllimport) int32_t __stdcall BinkDoFrame(BINK *bink);
__declspec(dllimport) void __stdcall BinkNextFrame(BINK *bink);
__declspec(dllimport) BINKSNDOPEN __stdcall BinkOpenDirectSound(uint32_t param);
__declspec(dllimport) int32_t __stdcall BinkSetSoundSystem(BINKSNDSYSOPEN open, uint32_t param);
}
