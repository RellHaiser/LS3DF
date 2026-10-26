// ISND: the engine's sound layer (LS3DF region 0x1007a100 - 0x1007d460), on DirectSound 8 through the
// sound device (audio/audio.h). One driver
// (ISndGetDriver()) creates sources (a .wav file loaded into a static buffer), sounds (a
// duplicate of a source's buffer that plays) and the 3D listener. Sounds that play on hardware
// channels go through a voice manager (isnd_voices.cpp) that suspends the least important ones
// when the card runs out of 3D channels.
//
// All interface methods are __stdcall with `this` on the stack, in the slot order of the
// original's virtual tables. I3D (sound frames), IShow and IGraph::Flip use the data members.
#pragma once

#include <cstdint>

#include <windows.h>

#include "audio/audio.h"
#include "core/c_str.h"
#include "core/c_vector.h"
#include "math/ls3d_math.h"

class ISND_source;
class ISND_sound;
class ISND_3Dsound;
class ISND_3Dlistener;

// ISND_driver::Init flags.
enum : uint32_t {
    ISND_INIT_NO_EAX = 0x0010,          // plain DirectSound (set by Init when EAX 3 is missing)
    ISND_INIT_SOFTWARE = 0x0020,        // software buffers only: no channel count, no EAX, no voice manager
    ISND_INIT_HEADPHONES = 0x0400,      // speaker configuration (none: the Windows setting)
    ISND_INIT_STEREO = 0x0800,
    ISND_INIT_QUAD = 0x1000,
    ISND_INIT_SURROUND_5_1 = 0x2000,
};

// ISND_source::Open / ISND_sound::Open flags (kept in the objects' `flags`).
enum : uint32_t {
    ISNDF_PAUSED = 0x0001,              // (sound, internal) SetPlayPosition(ISND_PAUSE)
    ISNDF_NO_VOLUME = 0x0002,           // the sound's volume methods fail
    ISNDF_NO_PAN = 0x0004,              // SetPan fails
    ISNDF_FREQUENCY = 0x0010,           // SetFrequency allowed
    ISNDF_SOFTWARE = 0x0040,            // a software buffer outside the voice manager
    ISNDF_3D = 0x0200,                  // a 3D buffer (cleared for sources with more than one channel)
};

// ISND_sound::Play flags.
enum : uint32_t {
    ISNDP_LOOP = 0x0100,
    ISNDP_KEEP_POSITION = 0x0400,       // continue from the current position instead of the start
};

// ISND_sound::SetPlayPosition special positions.
enum : int {
    ISND_PAUSE = -1,
    ISND_RESUME = -2,
};

// ISND_driver::InitFromDesc parameters.
struct S_ISND_init {
    int frequency;                      // +0x00 mixing rate of the primary buffer
    int channels;                       // +0x04
    int bits;                           // +0x08
    uint32_t flags;                     // +0x0C ISND_INIT_*
    HWND window;                        // +0x10
};

// ISND_source::GetFormat.
struct S_ISND_format {
    int channels;
    int bytesPerSample;
    int frequency;
    uint32_t dataSize;                  // bytes of sample data
};

// A .wav file in a static sound buffer (vtable 0x1009df00, 0x4C bytes).
class ISND_source {
public:
    ISND_source();                                                          // 0x1007ccf0
    ~ISND_source();                                                         // 0x1007cdc0

    // 0x00 0x1007cef0: drops a reference; the last one destroys the source.
    virtual int __stdcall Release();
    // 0x04 0x1007cf30: loads a PCM .wav file (rw_data); flags ISNDF_*.
    virtual HRESULT __stdcall Open(const char *fileName, uint32_t flags);
    // 0x08 0x1007d240
    virtual HRESULT __stdcall Close();
    // 0x0C 0x1007d280
    virtual bool __stdcall IsOpen();
    // 0x10 0x1007d290: length in ms.
    virtual int __stdcall GetLength();
    // 0x14 0x1007d2d0
    virtual HRESULT __stdcall GetFormat(S_ISND_format *format);
    // 0x18 / 0x1C 0x1007d300 / 0x1007d340: the whole buffer.
    virtual HRESULT __stdcall Lock(void **data);
    virtual HRESULT __stdcall Unlock();

    // The file name without its directory (0x10056150, compiled with I3D).
    const char *GetFileName() const { return fileName.c_str(); }

    int refCount;                               // +0x04
    uint32_t flags;                             // +0x08 ISNDF_*
    ls3d::C_str fileName;                       // +0x0C
    void *lockData;                             // +0x10 Lock
    void *lockData2;                            // +0x14 (unused second part)
    DWORD lockSize;                             // +0x18
    DWORD lockSize2;                            // +0x1C
    int channels;                               // +0x20
    int bytesPerSample;                         // +0x24
    int frequency;                              // +0x28
    uint32_t dataSize;                          // +0x2C
    ls3d::audio::Buffer *buffer;                // +0x30 (DirectSound: its version 8 interface)
    float bytesPerMs;                           // +0x34 nAvgBytesPerSec / 1000
    WAVEFORMATEX format;                        // +0x38
};

// A playing copy of a source (vtable 0x1009de70, 0x1C bytes).
class ISND_sound {
public:
    ISND_sound();                                                           // 0x1007bee0

    // 0x00 0x1007bfb0: drops a reference; the last one deletes the sound (slot 0x3C).
    virtual int __stdcall Release();
    // 0x04 0x1007bfe0: duplicates the source's buffer (the sound keeps a reference to the source).
    virtual HRESULT __stdcall Open(ISND_source *source, uint32_t flags);
    // 0x08 0x1007c070
    virtual HRESULT __stdcall Close();
    // 0x0C 0x1007c0e0: ISNDP_* flags; voices of lower priority give way to higher ones.
    virtual HRESULT __stdcall Play(uint32_t flags, uint32_t priority);
    // 0x10 0x1007c0b0: Open + Play(flags, 0).
    virtual HRESULT __stdcall OpenAndPlay(ISND_source *source, uint32_t flags);
    // 0x14 0x1007c660
    virtual HRESULT __stdcall Stop();
    // 0x18 0x1007c750: the play cursor in ms.
    virtual int __stdcall GetPlayPosition();
    // 0x1C 0x1007c7a0: in ms, or ISND_PAUSE / ISND_RESUME.
    virtual HRESULT __stdcall SetPlayPosition(int position);
    // 0x20 0x1007c9c0: 0 - 1, logarithmic (times the driver's volume).
    virtual HRESULT __stdcall SetVolume(float volume);
    // 0x24 0x1007caa0: DirectSound attenuation in 1/100 dB (minus the driver's volume).
    virtual HRESULT __stdcall SetVolumeDb(int millibels);
    // 0x28 0x1007ca50: 0 - 1, linear in dB.
    virtual HRESULT __stdcall SetVolumeLinear(float volume);
    // 0x2C 0x1007cb10: playback rate relative to the source's (ISNDF_FREQUENCY).
    virtual HRESULT __stdcall SetFrequency(float ratio);
    // 0x30 0x1007cba0: balance as a ratio (1: centre; < 1 left, > 1 right).
    virtual HRESULT __stdcall SetPan(float pan);
    // 0x34 0x1007cc40: also true while paused or suspended by the voice manager.
    virtual bool __stdcall IsPlaying();
    // 0x38 0x1007a790
    virtual ISND_source *__stdcall GetSource();
    // 0x3C 0x1007bf10 (scalar deleting destructor)
    virtual ~ISND_sound();

    int refCount;                               // +0x04
    ls3d::audio::Buffer *buffer;                // +0x08 the source's buffer duplicated
    ISND_source *source;                        // +0x0C
    uint32_t flags;                             // +0x10 ISNDF_*
    float frequency;                            // +0x14 ratio set by SetFrequency
    float volume;                               // +0x18 last SetVolume / SetVolumeLinear value
};

// A sound with a 3D buffer (vtable 0x1009dd40, 0x24 bytes).
class ISND_3Dsound : public ISND_sound {
public:
    ISND_3Dsound();                                                         // 0x1007a430

    // 0x04 0x1007a690: with ISNDF_3D also the 3D interface of the buffer.
    HRESULT __stdcall Open(ISND_source *source, uint32_t flags) override;
    // 0x08 0x1007a710
    HRESULT __stdcall Close() override;
    // 0x3C 0x1007a450 / 0x1007a470
    ~ISND_3Dsound() override;
    // 0x40 0x1007a4e0
    virtual bool __stdcall IsOpen();
    // 0x44 - 0x4C 0x1007a4f0 / 0x1007a530 / 0x1007a570 (without a 3D buffer they do nothing).
    virtual HRESULT __stdcall SetPosition(const S_vector &position);
    virtual HRESULT __stdcall SetConeOrientation(const S_vector &direction);
    virtual HRESULT __stdcall SetVelocity(const S_vector &velocity);
    // 0x50 0x1007a5b0
    virtual HRESULT __stdcall SetDistances(float minDistance, float maxDistance);
    // 0x54 0x1007a600: in radians.
    virtual HRESULT __stdcall SetConeAngles(float inside, float outside);
    // 0x58 0x1007a650: 0 - 1.
    virtual HRESULT __stdcall SetConeOutsideVolume(float volume);

    bool open;                                  // +0x1C
    ls3d::audio::Buffer3D *buffer3D;            // +0x20
};

// The listener (vtable 0x1009dcc0, 0x48 bytes): keeps the device's listener parameters and
// writes them with Apply; EAX 3 reverb properties go straight to the driver's property set.
class ISND_3Dlistener {
public:
    ISND_3Dlistener();                                                      // 0x1007a100

    virtual int __stdcall AddRef();                                         // 0x00 0x1007a150
    virtual int __stdcall Release();                                        // 0x04 0x1007a160
    virtual HRESULT __stdcall SetPosition(const S_vector &position);        // 0x08 0x1007a1a0
    virtual HRESULT __stdcall SetOrientation(const S_vector &front, const S_vector &top);  // 0x0C 0x1007a1c0
    virtual HRESULT __stdcall SetVelocity(const S_vector &velocity);        // 0x10 0x1007a200
    virtual HRESULT __stdcall SetDistanceFactor(float factor);              // 0x14 0x1007a220
    virtual HRESULT __stdcall SetDopplerFactor(float factor);               // 0x18 0x1007a230
    virtual HRESULT __stdcall SetRolloffFactor(float factor);               // 0x1C 0x1007a240
    // 0x20 0x1007a280: an EAX 3 listener property of one dword; for the two vector properties
    // (13 reflections pan, 16 reverb pan) `value` is the address of the vector.
    virtual HRESULT __stdcall SetEaxProperty(uint32_t property, uintptr_t value);
    // 0x24 / 0x28 0x1007a320 / 0x1007a360: any EAX 3 listener property.
    virtual HRESULT __stdcall SetEaxData(uint32_t property, void *data, uint32_t size);
    virtual HRESULT __stdcall GetEaxData(uint32_t property, void *data, uint32_t size, ULONG *returned);
    // 0x2C 0x1007a250: writes the parameters to DirectSound (immediately).
    virtual HRESULT __stdcall Apply();
    // 0x30 0x1007a3b0: commits DirectSound's deferred 3D settings.
    virtual HRESULT __stdcall Commit();

    int refCount;                               // +0x04
    DS3DLISTENER params;                        // +0x08
};

// ISND_driver::externalApi: an outside stream player's functions (__fastcall), as IShow calls
// them. Names follow those calls; the zero first arguments are the original's.
struct S_ISND_externalApi {
    typedef void(__cdecl *Callback)(int stream, int event);   // event 1: the stream ended
    void *unknown00[4];
    HRESULT(__fastcall *Startup)(int zero);                                        // +0x10
    HRESULT(__fastcall *Shutdown)(int zero);                                       // +0x14
    void *unknown18;
    HRESULT(__fastcall *Set1C)(int stream, int zero, int value);                   // +0x1C
    HRESULT(__fastcall *Set20)(int stream, int zero, int value);                   // +0x20
    HRESULT(__fastcall *OpenStream)(int zero, int *stream, Callback callback, int id, int zero2);  // +0x24
    HRESULT(__fastcall *CloseStream)(int stream);                                  // +0x28
    void *unknown2C;
    HRESULT(__fastcall *Play)(int stream, int loop);                               // +0x30
    HRESULT(__fastcall *Stop)(int stream);                                         // +0x34
    HRESULT(__fastcall *Pause)(int stream);                                        // +0x38
    HRESULT(__fastcall *Resume)(int stream);                                       // +0x3C
};

// The driver (vtable 0x1009ddf0, 0x58 bytes).
class ISND_driver {
public:
    ISND_driver();                                                          // 0x1007a7a0
    ~ISND_driver();                                                         // 0x1007a7f0

    // 0x00 0x1007b5b0: Init with the parameters of a block.
    virtual HRESULT __stdcall InitFromDesc(const S_ISND_init *init);
    // 0x04 0x1007b560: opens the default device (EAX Unified if present), sets the primary
    // buffer's format and the speaker configuration. Flags ISND_INIT_*.
    virtual HRESULT __stdcall Init(HWND window, int frequency, int channels, int bits, uint32_t flags);
    // 0x08 0x1007a850
    virtual HRESULT __stdcall Close();
    // 0x0C 0x1007a840
    virtual bool __stdcall IsInitialized();
    // 0x10 0x1007a920: PCM format of the primary buffer.
    virtual HRESULT __stdcall SetPrimaryFormat(int frequency, int channels, int bits);
    // 0x14 0x1007a9b0: master volume 0 - 1 (applied by the sounds' volume methods; updates the
    // I3D sound frames).
    virtual HRESULT __stdcall SetVolume(float volume);
    // 0x18 0x1007aa70: does nothing.
    virtual HRESULT __stdcall Reserved18(int unused);
    // 0x1C - 0x38: new objects, each with one reference (0x1C / 0x24 / 0x2C / 0x34 return
    // I3DERR_OUTOFMEM when the allocation fails).
    virtual HRESULT __stdcall CreateSource(ISND_source **source);         // 0x1C 0x1007aae0
    virtual ISND_source *__stdcall NewSource();                           // 0x20 0x1007aa80
    virtual HRESULT __stdcall CreateSound(ISND_sound **sound);            // 0x24 0x1007ab80
    virtual ISND_sound *__stdcall NewSound();                             // 0x28 0x1007ab20
    virtual HRESULT __stdcall Create3DSound(ISND_3Dsound **sound);        // 0x2C 0x1007ac20
    virtual ISND_3Dsound *__stdcall New3DSound();                         // 0x30 0x1007abc0
    virtual HRESULT __stdcall CreateListener(ISND_3Dlistener **listener); // 0x34 0x1007acf0
    virtual ISND_3Dlistener *__stdcall NewListener();                     // 0x38 0x1007ac60

private:
    bool OpenDevice(HWND window, int frequency, int channels, int bits, uint32_t flags);   // 0x1007b150
    bool InitEax();                                                         // 0x1007ad30

public:
    HWND window;                                // +0x04
    int unknown08;                              // +0x08 1, never read
    ls3d::audio::System *dsound;                // +0x0C the device
    ls3d::audio::PropertySet *eax;              // +0x10 EAX 3 property set (of eaxBuffer)
    ls3d::audio::Buffer3D *eaxBuffer;           // +0x14 a small 3D buffer that carries the property set
    HMODULE externalModule;                     // +0x18 never set in LS3DF; freed by Close
    uint32_t unknown1C;                         // +0x1C
    // +0x20 Never set in LS3DF either (deleted by Close): a table of functions that IShow's
    // sound methods call (slots 0x5C - 0x78).
    struct S_ISND_externalApi *externalApi;
    uint32_t unknown24;                         // +0x24
    ls3d::audio::Buffer *primary;               // +0x28
    ls3d::audio::Listener *listener;            // +0x2C
    uint32_t flags;                             // +0x30 ISND_INIT_*
    float volume;                               // +0x34 master volume
    uint32_t maxFrequency;                      // +0x38 secondary buffer rate limits
    uint32_t minFrequency;                      // +0x3C
    ls3d::C_vector<ISND_source *> sources;      // +0x40 sources loaded by I3D sound frames
    uint32_t unknown50;                         // +0x50
    uint32_t unknown54;                         // +0x54
};

static_assert(sizeof(ISND_source) == 0x4C, "ISND_source layout");
static_assert(sizeof(ISND_sound) == 0x1C, "ISND_sound layout");
static_assert(sizeof(ISND_3Dsound) == 0x24, "ISND_3Dsound layout");
static_assert(sizeof(ISND_3Dlistener) == 0x48, "ISND_3Dlistener layout");
static_assert(sizeof(ISND_driver) == 0x58, "ISND_driver layout");

// The driver (0x100d16fc), created and destroyed with the engine globals.
extern ISND_driver *g_sndDriver;

extern "C" ISND_driver *__cdecl ISndGetDriver();                            // 0x10068fa0

namespace ls3d::snd {

// Set by a successful ISND_driver::Init, cleared by Close (0x100d2c60).
extern bool g_initialized;

// Logs a DirectSound error code ("TraceDSErrors() - DSERR_...", 0x1007bcd0).
void TraceDSErrors(HRESULT hr);

}  // namespace ls3d::snd
