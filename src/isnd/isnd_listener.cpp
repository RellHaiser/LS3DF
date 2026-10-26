// ISND_3Dlistener (0x1007a100 - 0x1007a3e0).
#include "isnd/isnd.h"

#include "core/debug.h"
#include "core/i3d_result.h"
#include "isnd/eax.h"

using namespace ls3d;

namespace {

// The value of the last SetEaxProperty (0x100d2c00): the property set's Set reads it from memory.
uint32_t g_eaxValue;

// The driver's EAX property set, when EAX is in use.
audio::PropertySet *EaxListener()
{
    if (!g_sndDriver->eax || (g_sndDriver->flags & ISND_INIT_NO_EAX))
        return nullptr;
    return g_sndDriver->eax;
}

}  // namespace

ISND_3Dlistener::ISND_3Dlistener() : refCount(1), params()
{
    params.dwSize = sizeof params;
    params.flDistanceFactor = 1.0f;
    params.flRolloffFactor = 1.0f;
    params.flDopplerFactor = 1.0f;
}

int ISND_3Dlistener::AddRef()
{
    return ++refCount;
}

int ISND_3Dlistener::Release()
{
    const int refs = --refCount;
    if (refs < 0) {
        dbgPrintf("!! RefCount<0  this:0x%x  ISND_3Dlistener", this);
        return 0;
    }
    if (refs == 0)
        delete this;
    return refs;
}

HRESULT ISND_3Dlistener::SetPosition(const S_vector &position)
{
    params.vPosition = {position.x, position.y, position.z};
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetOrientation(const S_vector &front, const S_vector &top)
{
    params.vOrientFront = {front.x, front.y, front.z};
    params.vOrientTop = {top.x, top.y, top.z};
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetVelocity(const S_vector &velocity)
{
    params.vVelocity = {velocity.x, velocity.y, velocity.z};
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetDistanceFactor(float factor)
{
    params.flDistanceFactor = factor;
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetDopplerFactor(float factor)
{
    params.flDopplerFactor = factor;
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetRolloffFactor(float factor)
{
    params.flRolloffFactor = factor;
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetEaxProperty(uint32_t property, uintptr_t value)
{
    audio::PropertySet *eax = EaxListener();
    if (!eax)
        return I3DERR_UNSUPPORTED;
    g_eaxValue = static_cast<uint32_t>(value);
    switch (property) {
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12:
    case 14: case 15: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25:
        audio::g_pAudio->Set(eax, snd::kEaxListenerProperties, property, nullptr, 0, &g_eaxValue, sizeof g_eaxValue);
        break;
    case snd::kEaxListenerReflectionsPan:
    case snd::kEaxListenerReverbPan:
        audio::g_pAudio->Set(eax, snd::kEaxListenerProperties, property, nullptr, 0, reinterpret_cast<void *>(value),
                             sizeof(snd::S_eaxVector));
        break;
    default:
        break;
    }
    return I3D_OK;
}

HRESULT ISND_3Dlistener::SetEaxData(uint32_t property, void *data, uint32_t size)
{
    audio::PropertySet *eax = EaxListener();
    if (!eax)
        return I3DERR_UNSUPPORTED;
    audio::g_pAudio->Set(eax, snd::kEaxListenerProperties, property, nullptr, 0, data, size);
    return I3D_OK;
}

HRESULT ISND_3Dlistener::GetEaxData(uint32_t property, void *data, uint32_t size, ULONG *returned)
{
    audio::PropertySet *eax = EaxListener();
    if (!eax)
        return I3DERR_UNSUPPORTED;
    audio::g_pAudio->Get(eax, snd::kEaxListenerProperties, property, nullptr, 0, data, size, returned);
    return I3D_OK;
}

HRESULT ISND_3Dlistener::Apply()
{
    const HRESULT hr = audio::g_pAudio->SetAllParameters(g_sndDriver->listener, &params, DS3D_IMMEDIATE);
    return SUCCEEDED(hr) ? I3D_OK : I3DERR_GENERIC;
}

HRESULT ISND_3Dlistener::Commit()
{
    return SUCCEEDED(audio::g_pAudio->CommitDeferredSettings(g_sndDriver->listener)) ? I3D_OK : I3DERR_GENERIC;
}
