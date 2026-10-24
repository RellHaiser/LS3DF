// The parts of Creative's EAX 3.0 interface (eax.h) that ISND uses: the EAX Unified DirectSound
// object and the listener / buffer property sets of IKsPropertySet.
#pragma once

#include <windows.h>

namespace ls3d::snd {

// CLSID_EAXDirectSound8 {ca503b60-b176-11d4-a094-d0c0bf3a560c} (0x100a1da8).
constexpr GUID kClsidEaxDirectSound8 = {0xca503b60, 0xb176, 0x11d4, {0xa0, 0x94, 0xd0, 0xc0, 0xbf, 0x3a, 0x56, 0x0c}};
// DSPROPSETID_EAX30_ListenerProperties {a8fa6882-b476-11d3-bdb9-00c0f02ddf87} (0x100a1d48).
constexpr GUID kEaxListenerProperties = {0xa8fa6882, 0xb476, 0x11d3, {0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87}};
// DSPROPSETID_EAX30_BufferProperties {a8fa6881-b476-11d3-bdb9-00c0f02ddf87} (0x100a1d58).
constexpr GUID kEaxBufferProperties = {0xa8fa6881, 0xb476, 0x11d3, {0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87}};

// DSPROPERTY_EAXLISTENER_ALLPARAMETERS / DSPROPERTY_EAXBUFFER_ALLPARAMETERS.
constexpr ULONG kEaxAllParameters = 1;
// The two vector properties of the listener set (reflections pan, reverb pan).
constexpr ULONG kEaxListenerReflectionsPan = 13;
constexpr ULONG kEaxListenerReverbPan = 16;

struct S_eaxVector {
    float x, y, z;
};

// EAXLISTENERPROPERTIES (0x70 bytes).
struct S_eaxListenerProperties {
    unsigned long environment;          // +0x00
    float environmentSize;              // +0x04
    float environmentDiffusion;         // +0x08
    long room;                          // +0x0C
    long roomHF;                        // +0x10
    long roomLF;                        // +0x14
    float decayTime;                    // +0x18
    float decayHFRatio;                 // +0x1C
    float decayLFRatio;                 // +0x20
    long reflections;                   // +0x24
    float reflectionsDelay;             // +0x28
    S_eaxVector reflectionsPan;         // +0x2C
    long reverb;                        // +0x38
    float reverbDelay;                  // +0x3C
    S_eaxVector reverbPan;              // +0x40
    float echoTime;                     // +0x4C
    float echoDepth;                    // +0x50
    float modulationTime;               // +0x54
    float modulationDepth;              // +0x58
    float airAbsorptionHF;              // +0x5C
    float hfReference;                  // +0x60
    float lfReference;                  // +0x64
    float roomRolloffFactor;            // +0x68
    unsigned long flags;                // +0x6C
};
static_assert(sizeof(S_eaxListenerProperties) == 0x70, "EAXLISTENERPROPERTIES layout");

}  // namespace ls3d::snd
