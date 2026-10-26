// The DirectSound backend of the sound device (audio/audio.h): each call goes to DirectSound
// unchanged, so the engine makes exactly the DirectSound calls the original made.
#include "audio/dsound/dsound_device.h"

namespace ls3d::audio {

namespace {

using dsound::DS;

// Out-parameters: `call` writes a DirectSound interface, the caller gets its handle. (The interface
// starts as the caller's handle, so a call that does not write it leaves the caller's variable as
// it was.)
template <typename H, typename Call>
HRESULT Out(H **handle, Call call)
{
    auto *object = DS(*handle);
    const HRESULT hr = call(reinterpret_cast<void **>(&object));
    *handle = dsound::Handle(object);
    return hr;
}

class DeviceDSound final : public Device {
public:
    HRESULT Enumerate(LPDSENUMCALLBACKA callback, void *context) override
    {
        return DirectSoundEnumerateA(callback, context);
    }
    HRESULT CreateInstance(REFCLSID clsid, System **system) override
    {
        return Out(system, [&](void **p) {
            return CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectSound8, p);
        });
    }
    HRESULT Initialize(System *system, const GUID *device) override { return DS(system)->Initialize(device); }
    HRESULT Create(const GUID *device, System **system) override
    {
        return Out(system, [&](void **p) {
            return DirectSoundCreate8(device, reinterpret_cast<IDirectSound8 **>(p), nullptr);
        });
    }
    ULONG Release(System *system) override { return DS(system)->Release(); }
    HRESULT GetCaps(System *system, DSCAPS *caps) override { return DS(system)->GetCaps(caps); }
    HRESULT SetCooperativeLevel(System *system, HWND window, DWORD level) override
    {
        return DS(system)->SetCooperativeLevel(window, level);
    }
    HRESULT SetSpeakerConfig(System *system, DWORD config) override { return DS(system)->SetSpeakerConfig(config); }
    HRESULT CreateSoundBuffer(System *system, const DSBUFFERDESC *desc, Buffer **buffer) override
    {
        return Out(buffer, [&](void **p) {
            return DS(system)->CreateSoundBuffer(desc, reinterpret_cast<IDirectSoundBuffer **>(p), nullptr);
        });
    }
    HRESULT DuplicateSoundBuffer(System *system, Buffer *original, Buffer **duplicate) override
    {
        return Out(duplicate, [&](void **p) {
            return DS(system)->DuplicateSoundBuffer(DS(original), reinterpret_cast<IDirectSoundBuffer **>(p));
        });
    }
    void *Native(System *system) override { return DS(system); }

    HRESULT Query(Buffer *buffer, Buffer **buffer8) override
    {
        return Out(buffer8, [&](void **p) { return DS(buffer)->QueryInterface(IID_IDirectSoundBuffer8, p); });
    }
    HRESULT Query(Buffer *buffer, Buffer3D **buffer3D) override
    {
        return Out(buffer3D, [&](void **p) { return DS(buffer)->QueryInterface(IID_IDirectSound3DBuffer, p); });
    }
    HRESULT Query(Buffer *buffer, Listener **listener) override
    {
        return Out(listener, [&](void **p) { return DS(buffer)->QueryInterface(IID_IDirectSound3DListener, p); });
    }
    HRESULT Query(Buffer *buffer, Notify **notify) override
    {
        return Out(notify, [&](void **p) { return DS(buffer)->QueryInterface(IID_IDirectSoundNotify, p); });
    }
    ULONG Release(Buffer *buffer) override { return DS(buffer)->Release(); }
    HRESULT SetFormat(Buffer *buffer, const WAVEFORMATEX *format) override { return DS(buffer)->SetFormat(format); }
    HRESULT Lock(Buffer *buffer, DWORD offset, DWORD bytes, void **data1, DWORD *bytes1, void **data2, DWORD *bytes2,
                 DWORD flags) override
    {
        return DS(buffer)->Lock(offset, bytes, data1, bytes1, data2, bytes2, flags);
    }
    HRESULT Unlock(Buffer *buffer, void *data1, DWORD bytes1, void *data2, DWORD bytes2) override
    {
        return DS(buffer)->Unlock(data1, bytes1, data2, bytes2);
    }
    HRESULT Play(Buffer *buffer, DWORD reserved, DWORD priority, DWORD flags) override
    {
        return DS(buffer)->Play(reserved, priority, flags);
    }
    HRESULT Stop(Buffer *buffer) override { return DS(buffer)->Stop(); }
    HRESULT GetStatus(Buffer *buffer, DWORD *status) override { return DS(buffer)->GetStatus(status); }
    HRESULT GetCurrentPosition(Buffer *buffer, DWORD *play, DWORD *write) override
    {
        return DS(buffer)->GetCurrentPosition(play, write);
    }
    HRESULT SetCurrentPosition(Buffer *buffer, DWORD position) override
    {
        return DS(buffer)->SetCurrentPosition(position);
    }
    HRESULT SetVolume(Buffer *buffer, LONG volume) override { return DS(buffer)->SetVolume(volume); }
    HRESULT SetPan(Buffer *buffer, LONG pan) override { return DS(buffer)->SetPan(pan); }
    HRESULT SetFrequency(Buffer *buffer, DWORD frequency) override { return DS(buffer)->SetFrequency(frequency); }

    HRESULT Query(Buffer3D *buffer3D, PropertySet **properties) override
    {
        return Out(properties, [&](void **p) { return DS(buffer3D)->QueryInterface(IID_IKsPropertySet, p); });
    }
    ULONG Release(Buffer3D *buffer3D) override { return DS(buffer3D)->Release(); }
    HRESULT SetMode(Buffer3D *buffer3D, DWORD mode, DWORD apply) override { return DS(buffer3D)->SetMode(mode, apply); }
    HRESULT SetPosition(Buffer3D *buffer3D, float x, float y, float z, DWORD apply) override
    {
        return DS(buffer3D)->SetPosition(x, y, z, apply);
    }
    HRESULT SetVelocity(Buffer3D *buffer3D, float x, float y, float z, DWORD apply) override
    {
        return DS(buffer3D)->SetVelocity(x, y, z, apply);
    }
    HRESULT SetConeOrientation(Buffer3D *buffer3D, float x, float y, float z, DWORD apply) override
    {
        return DS(buffer3D)->SetConeOrientation(x, y, z, apply);
    }
    HRESULT SetConeAngles(Buffer3D *buffer3D, DWORD inside, DWORD outside, DWORD apply) override
    {
        return DS(buffer3D)->SetConeAngles(inside, outside, apply);
    }
    HRESULT SetConeOutsideVolume(Buffer3D *buffer3D, LONG volume, DWORD apply) override
    {
        return DS(buffer3D)->SetConeOutsideVolume(volume, apply);
    }
    HRESULT SetMinDistance(Buffer3D *buffer3D, float distance, DWORD apply) override
    {
        return DS(buffer3D)->SetMinDistance(distance, apply);
    }
    HRESULT SetMaxDistance(Buffer3D *buffer3D, float distance, DWORD apply) override
    {
        return DS(buffer3D)->SetMaxDistance(distance, apply);
    }

    ULONG Release(Listener *listener) override { return DS(listener)->Release(); }
    HRESULT GetAllParameters(Listener *listener, DS3DLISTENER *params) override
    {
        return DS(listener)->GetAllParameters(params);
    }
    HRESULT SetAllParameters(Listener *listener, const DS3DLISTENER *params, DWORD apply) override
    {
        return DS(listener)->SetAllParameters(params, apply);
    }
    HRESULT CommitDeferredSettings(Listener *listener) override { return DS(listener)->CommitDeferredSettings(); }

    ULONG Release(PropertySet *properties) override { return DS(properties)->Release(); }
    HRESULT QuerySupport(PropertySet *properties, REFGUID set, ULONG id, ULONG *support) override
    {
        return DS(properties)->QuerySupport(set, id, support);
    }
    HRESULT Set(PropertySet *properties, REFGUID set, ULONG id, void *instance, ULONG instanceSize, void *data,
                ULONG size) override
    {
        return DS(properties)->Set(set, id, instance, instanceSize, data, size);
    }
    HRESULT Get(PropertySet *properties, REFGUID set, ULONG id, void *instance, ULONG instanceSize, void *data,
                ULONG size, ULONG *returned) override
    {
        return DS(properties)->Get(set, id, instance, instanceSize, data, size, returned);
    }

    ULONG Release(Notify *notify) override { return DS(notify)->Release(); }
    HRESULT SetNotificationPositions(Notify *notify, DWORD count, const DSBPOSITIONNOTIFY *positions) override
    {
        return DS(notify)->SetNotificationPositions(count, positions);
    }
};

DeviceDSound s_device;

}  // namespace

Device *g_pAudio = &s_device;

}  // namespace ls3d::audio
