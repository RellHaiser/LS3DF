// DirectInput 8 types and constants: the vocabulary of the engine's input device (input/input.h).
//
// The key codes (DIK_*, which the game also stores in its key bindings), device states
// (DIMOUSESTATE2), data formats, cooperative levels and result codes IGraph describes input in. On
// Windows they come from the SDK's dinput.h, which also declares DirectInput's COM interfaces:
// only the DirectInput backend (input/dinput) uses those (build.py checks it).
#pragma once

#include <windows.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
