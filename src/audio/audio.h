// The sound device: everything ISND and IShow play goes through audio::Device (g_pAudio).
//
// As with the render device (gfx/gfx.h), the vocabulary stays the API the engine was written for:
// DirectSound 8's buffers, 3D buffers, listener and EAX property sets (dx/dsound_types.h). The
// engine's sound is defined in those terms (volumes in millibels, 3D modes, cone angles, EAX reverb
// properties), so a backend has to reproduce that model whatever it plays sound with. What the
// device hides is DirectSound itself: no code outside audio/<backend> calls a DirectSound interface.
//
// The objects are opaque handles that only the backend looks inside, stored where the engine's
// structures kept the DirectSound pointers (same size and place). The DirectSound backend's handles
// are the DirectSound interfaces. QueryInterface is one Query overload per interface the engine
// asks for; like QueryInterface it gives a new reference.
//
// The methods mean what their DirectSound namesakes mean, and only those the engine calls are here.
#pragma once

#include "dx/dsound_types.h"

namespace ls3d::audio {

// Opaque handles (defined only by a backend).
struct System;          // the device
struct Buffer;          // a sound buffer (the primary buffer or a secondary one)
struct Buffer3D;        // a buffer's 3D controls
struct Listener;        // the 3D listener (of the primary buffer)
struct PropertySet;     // a 3D buffer's property set (EAX)
struct Notify;          // a buffer's position notifications

class Device {
public:
    virtual ~Device() = default;

    // ---- the device
    // Lists the output devices to `callback`.
    virtual HRESULT Enumerate(LPDSENUMCALLBACKA callback, void *context) = 0;
    // A device object of a COM class (EAX Unified's DirectSound 8), not initialised yet.
    virtual HRESULT CreateInstance(REFCLSID clsid, System **system) = 0;
    virtual HRESULT Initialize(System *system, const GUID *device) = 0;
    // A device, initialised (`device` null: the default one).
    virtual HRESULT Create(const GUID *device, System **system) = 0;
    virtual ULONG Release(System *system) = 0;
    virtual HRESULT GetCaps(System *system, DSCAPS *caps) = 0;
    virtual HRESULT SetCooperativeLevel(System *system, HWND window, DWORD level) = 0;
    virtual HRESULT SetSpeakerConfig(System *system, DWORD config) = 0;
    virtual HRESULT CreateSoundBuffer(System *system, const DSBUFFERDESC *desc, Buffer **buffer) = 0;
    virtual HRESULT DuplicateSoundBuffer(System *system, Buffer *original, Buffer **duplicate) = 0;
    // The API's own device object, for a player that drives it itself (Bink plays through
    // DirectSound).
    virtual void *Native(System *system) = 0;

    // ---- buffers
    // The buffer's DirectSound 8 interface (IDirectSoundBuffer8).
    virtual HRESULT Query(Buffer *buffer, Buffer **buffer8) = 0;
    virtual HRESULT Query(Buffer *buffer, Buffer3D **buffer3D) = 0;
    virtual HRESULT Query(Buffer *buffer, Listener **listener) = 0;
    virtual HRESULT Query(Buffer *buffer, Notify **notify) = 0;
    virtual ULONG Release(Buffer *buffer) = 0;
    virtual HRESULT SetFormat(Buffer *buffer, const WAVEFORMATEX *format) = 0;
    virtual HRESULT Lock(Buffer *buffer, DWORD offset, DWORD bytes, void **data1, DWORD *bytes1, void **data2,
                         DWORD *bytes2, DWORD flags) = 0;
    virtual HRESULT Unlock(Buffer *buffer, void *data1, DWORD bytes1, void *data2, DWORD bytes2) = 0;
    virtual HRESULT Play(Buffer *buffer, DWORD reserved, DWORD priority, DWORD flags) = 0;
    virtual HRESULT Stop(Buffer *buffer) = 0;
    virtual HRESULT GetStatus(Buffer *buffer, DWORD *status) = 0;
    virtual HRESULT GetCurrentPosition(Buffer *buffer, DWORD *play, DWORD *write) = 0;
    virtual HRESULT SetCurrentPosition(Buffer *buffer, DWORD position) = 0;
    virtual HRESULT SetVolume(Buffer *buffer, LONG volume) = 0;
    virtual HRESULT SetPan(Buffer *buffer, LONG pan) = 0;
    virtual HRESULT SetFrequency(Buffer *buffer, DWORD frequency) = 0;

    // ---- 3D buffers
    virtual HRESULT Query(Buffer3D *buffer3D, PropertySet **properties) = 0;
    virtual ULONG Release(Buffer3D *buffer3D) = 0;
    virtual HRESULT SetMode(Buffer3D *buffer3D, DWORD mode, DWORD apply) = 0;
    virtual HRESULT SetPosition(Buffer3D *buffer3D, float x, float y, float z, DWORD apply) = 0;
    virtual HRESULT SetVelocity(Buffer3D *buffer3D, float x, float y, float z, DWORD apply) = 0;
    virtual HRESULT SetConeOrientation(Buffer3D *buffer3D, float x, float y, float z, DWORD apply) = 0;
    virtual HRESULT SetConeAngles(Buffer3D *buffer3D, DWORD inside, DWORD outside, DWORD apply) = 0;
    virtual HRESULT SetConeOutsideVolume(Buffer3D *buffer3D, LONG volume, DWORD apply) = 0;
    virtual HRESULT SetMinDistance(Buffer3D *buffer3D, float distance, DWORD apply) = 0;
    virtual HRESULT SetMaxDistance(Buffer3D *buffer3D, float distance, DWORD apply) = 0;

    // ---- the listener
    virtual ULONG Release(Listener *listener) = 0;
    virtual HRESULT GetAllParameters(Listener *listener, DS3DLISTENER *params) = 0;
    virtual HRESULT SetAllParameters(Listener *listener, const DS3DLISTENER *params, DWORD apply) = 0;
    virtual HRESULT CommitDeferredSettings(Listener *listener) = 0;

    // ---- property sets (EAX)
    virtual ULONG Release(PropertySet *properties) = 0;
    virtual HRESULT QuerySupport(PropertySet *properties, REFGUID set, ULONG id, ULONG *support) = 0;
    virtual HRESULT Set(PropertySet *properties, REFGUID set, ULONG id, void *instance, ULONG instanceSize,
                        void *data, ULONG size) = 0;
    virtual HRESULT Get(PropertySet *properties, REFGUID set, ULONG id, void *instance, ULONG instanceSize,
                        void *data, ULONG size, ULONG *returned) = 0;

    // ---- position notifications
    virtual ULONG Release(Notify *notify) = 0;
    virtual HRESULT SetNotificationPositions(Notify *notify, DWORD count, const DSBPOSITIONNOTIFY *positions) = 0;
};

// The device in use (the DirectSound backend's, audio/dsound).
extern Device *g_pAudio;

}  // namespace ls3d::audio
