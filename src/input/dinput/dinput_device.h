// The DirectInput backend's own declarations: the handles' conversions. Only the backend (and the
// tests, which put fake DirectInput objects in) include this.
#pragma once

#include "input/input.h"

namespace ls3d::input::dinput {

// The backend's handles are the DirectInput interfaces.
inline IDirectInput8A *DI(System *s) { return reinterpret_cast<IDirectInput8A *>(s); }
inline IDirectInputDevice8A *DI(Peripheral *p) { return reinterpret_cast<IDirectInputDevice8A *>(p); }
inline System *Handle(IDirectInput8A *s) { return reinterpret_cast<System *>(s); }
inline Peripheral *Handle(IDirectInputDevice8A *p) { return reinterpret_cast<Peripheral *>(p); }

}  // namespace ls3d::input::dinput
