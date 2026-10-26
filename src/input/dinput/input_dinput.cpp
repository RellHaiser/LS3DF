// The DirectInput backend of the input device (input/input.h): each call goes to DirectInput
// unchanged, so the engine makes exactly the DirectInput calls the original made.
#include "input/dinput/dinput_device.h"

namespace ls3d::input {

namespace {

using dinput::DI;

// Out-parameters: `call` writes a DirectInput interface, the caller gets its handle. (The interface
// starts as the caller's handle, so a call that does not write it leaves the caller's variable as
// it was.)
template <typename H, typename Call>
HRESULT Out(H **handle, Call call)
{
    auto *object = DI(*handle);
    const HRESULT hr = call(&object);
    *handle = dinput::Handle(object);
    return hr;
}

class DeviceDInput final : public Device {
public:
    HRESULT Create(HINSTANCE instance, System **system) override
    {
        return Out(system, [&](IDirectInput8A **p) {
            return DirectInput8Create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void **>(p),
                                      nullptr);
        });
    }
    ULONG Release(System *system) override { return DI(system)->Release(); }
    HRESULT CreateDevice(System *system, REFGUID guid, Peripheral **peripheral) override
    {
        return Out(peripheral, [&](IDirectInputDevice8A **p) { return DI(system)->CreateDevice(guid, p, nullptr); });
    }

    ULONG Release(Peripheral *peripheral) override { return DI(peripheral)->Release(); }
    HRESULT SetDataFormat(Peripheral *peripheral, const DIDATAFORMAT *format) override
    {
        return DI(peripheral)->SetDataFormat(format);
    }
    HRESULT SetCooperativeLevel(Peripheral *peripheral, HWND window, DWORD flags) override
    {
        return DI(peripheral)->SetCooperativeLevel(window, flags);
    }
    HRESULT Acquire(Peripheral *peripheral) override { return DI(peripheral)->Acquire(); }
    HRESULT Unacquire(Peripheral *peripheral) override { return DI(peripheral)->Unacquire(); }
    HRESULT GetDeviceState(Peripheral *peripheral, DWORD size, void *state) override
    {
        return DI(peripheral)->GetDeviceState(size, state);
    }
    HRESULT GetCapabilities(Peripheral *peripheral, DIDEVCAPS *caps) override
    {
        return DI(peripheral)->GetCapabilities(caps);
    }
    HRESULT GetProperty(Peripheral *peripheral, REFGUID property, DIPROPHEADER *header) override
    {
        return DI(peripheral)->GetProperty(property, header);
    }
};

DeviceDInput s_device;

}  // namespace

Device *g_pInput = &s_device;

}  // namespace ls3d::input
