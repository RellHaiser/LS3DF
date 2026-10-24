// DXGetErrorString8 of dxerr8.lib, linked statically into the original (0x1007d466): the name of a
// DirectX result code ("D3DERR_DEVICELOST", ...), "Unknown" if none.
#pragma once

#include <windows.h>

const char *__stdcall DXGetErrorString8A(HRESULT hr);
