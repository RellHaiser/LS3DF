// Background texture loading (IG_TEXTURE_THREAD): staging texture pools filled by a worker thread
// (0x10078b40 - 0x10079440).
#pragma once

#include <cstdint>

#include <windows.h>

namespace ls3d::tex {

// Drops every pending request and releases the staging pools (0x10078b40).
void StreamFlush();
// Creates the staging pools for the device's formats (0x10078c40; flags = S_IGraphInit::flags).
void StreamInit(uint32_t flags);
// The worker thread (0x10078ed0).
DWORD WINAPI StreamThread(void *param);

}  // namespace ls3d::tex
