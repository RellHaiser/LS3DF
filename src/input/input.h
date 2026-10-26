// The input device: IGraph reads the keyboard and the mouse through input::Device (g_pInput).
//
// As with the render and sound devices (gfx/gfx.h, audio/audio.h), the vocabulary stays the API
// the engine was written for: DirectInput 8's devices, data formats and device states
// (dx/dinput_types.h). The keyboard is 256 bytes of DIK_* keys, the game's own key numbering (its
// key bindings store those codes), and the mouse is a DIMOUSESTATE2. What the device hides is
// DirectInput itself: no code outside input/<backend> calls a DirectInput interface.
//
// The DirectInput object and its devices are opaque handles that only the backend looks inside;
// the DirectInput backend's handles are the DirectInput interfaces. The methods mean what their
// DirectInput namesakes mean, and only those the engine calls are here.
#pragma once

#include "dx/dinput_types.h"

namespace ls3d::input {

// Opaque handles (defined only by a backend).
struct System;          // the input system (the DirectInput object)
struct Peripheral;      // a keyboard or a mouse

class Device {
public:
    virtual ~Device() = default;

    // ---- the input system
    virtual HRESULT Create(HINSTANCE instance, System **system) = 0;
    virtual ULONG Release(System *system) = 0;
    // A device: GUID_SysKeyboard or GUID_SysMouse.
    virtual HRESULT CreateDevice(System *system, REFGUID guid, Peripheral **peripheral) = 0;

    // ---- keyboards and mice
    virtual ULONG Release(Peripheral *peripheral) = 0;
    // c_dfDIKeyboard (256 key bytes) or c_dfDIMouse2 (DIMOUSESTATE2).
    virtual HRESULT SetDataFormat(Peripheral *peripheral, const DIDATAFORMAT *format) = 0;
    virtual HRESULT SetCooperativeLevel(Peripheral *peripheral, HWND window, DWORD flags) = 0;
    virtual HRESULT Acquire(Peripheral *peripheral) = 0;
    virtual HRESULT Unacquire(Peripheral *peripheral) = 0;
    virtual HRESULT GetDeviceState(Peripheral *peripheral, DWORD size, void *state) = 0;
    virtual HRESULT GetCapabilities(Peripheral *peripheral, DIDEVCAPS *caps) = 0;
    // DIPROP_KEYNAME: a key's name.
    virtual HRESULT GetProperty(Peripheral *peripheral, REFGUID property, DIPROPHEADER *header) = 0;
};

// The device in use (the DirectInput backend's, input/dinput).
extern Device *g_pInput;

}  // namespace ls3d::input
