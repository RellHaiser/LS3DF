// DirectSound 8 types and constants: the vocabulary of the engine's sound device (audio/audio.h).
//
// The structures, flags and result codes ISND and IShow describe sound in (DSBUFFERDESC,
// WAVEFORMATEX, DS3DLISTENER, DSBCAPS_*, DSERR_* ...). On Windows they come from the SDK's
// dsound.h, which also declares DirectSound's COM interfaces: only the DirectSound backend
// (audio/dsound) uses those (build.py checks it).
#pragma once

#include <windows.h>
#include <mmreg.h>
#ifndef DIRECTSOUND_VERSION
#define DIRECTSOUND_VERSION 0x0800
#endif
#include <dsound.h>
