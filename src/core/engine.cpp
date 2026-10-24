// The engine's start-up and shut-down (0x10068c90 - 0x10068fb0). In the original a static object
// (0x100d16f1), made by 0x10068c90 while the CRT initialises the DLL, starts the log and makes the
// three singletons, IGraph, the I3D driver and the sound driver; 0x10068eb0 (FreeEngineGlobals,
// registered with atexit) deletes them. The port does both in DllMain (the original's, 0x10094655,
// only returns TRUE): after every other global is initialised, whatever the link order, and before
// the CRT's exit list runs, as the original's last-registered destructor runs first.
#include <windows.h>

#include <cstdint>

#include "core/debug.h"
#include "i3d/driver.h"
#include "igraph/igraph.h"
#include "isnd/isnd.h"

namespace ls3d::i3d {

I3D_driver *g_driver = nullptr;             // 0x100d16f8

}  // namespace ls3d::i3d

namespace {

// Performance counter ticks per 16 us (0x100d1590; at least 1; nothing in the engine reads it).
uint64_t g_perfTicksPer16Us;

// 0x10068cb0
void StartEngine()
{
    ls3d::StartLog();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_perfTicksPer16Us = (static_cast<uint64_t>(frequency.QuadPart) << 4) / 1000000;
    if (!g_perfTicksPer16Us)
        g_perfTicksPer16Us = 1;
    g_pIGraph = new IGraph;
    ls3d::i3d::g_driver = new I3D_driver;
    g_sndDriver = new ISND_driver;
}

// 0x10068eb0 (FreeEngineGlobals)
void FreeEngine()
{
    delete ls3d::i3d::g_driver;
    ls3d::i3d::g_driver = nullptr;
    delete g_pIGraph;
    g_pIGraph = nullptr;
    delete g_sndDriver;
    g_sndDriver = nullptr;
}

}  // namespace

// 0x10068fb0
extern "C" I3D_driver *__cdecl I3DGetDriver()
{
    return ls3d::i3d::g_driver;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        StartEngine();
    else if (reason == DLL_PROCESS_DETACH)
        FreeEngine();
    return TRUE;
}
