// Differential test of IGraph's keyboard, mouse and timer (src/igraph/igraph_input.cpp, through the
// input device of src/input): the original LS3DF.dll's IGraph and the port's run the same random
// operations, each side on its own fake DirectInput objects with the same scripted device states.
// Every DirectInput call with its arguments, the return values, the read states and the debug
// overlay switches must be the same. Both sides' timeGetTime imports read one fake clock.
// (Not covered: DirectInput8Create itself, the window-message keyboard (IGKEY_WINDOW_MESSAGES),
// SetMousePos in absolute mode (it moves the real cursor).)
//
// Usage: compare_input.exe [original LS3DF.dll] [operations] [seed]
//
// test-sources: src/igraph/*.cpp src/core/debug.cpp src/core/cpu.cpp src/core/enhance.cpp src/dx/dxerr8.cpp src/i3d/ib_manager.cpp src/i3d/vb_manager.cpp src/dshow/*.cpp
// test-sources: src/gfx/d3d8/gfx_d3d8.cpp src/audio/dsound/audio_dsound.cpp src/input/dinput/input_dinput.cpp
// test-sources: src/platform/win32/platform_win32.cpp
// test-sources: src/video/win32/*.cpp
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <random>
#include <string>
#include <vector>

#include "../src/core/debug.h"
#include "../src/i3d/driver.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/igraph_state.h"
#include "../src/input/dinput/dinput_device.h"
#include "fake_dinput.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d {
namespace i3d {
I3D_driver *g_driver;
uint32_t g_renderStats[8];
void ReapplyDriverStates() {}
void DrawDebugText(int, int, const char *) {}
bool UseWBuffer() { return false; }
void InvalidateLights() {}
int InitParticleBuffers() { return 0; }
}  // namespace i3d
namespace snd {
void UpdateVoices() {}
void DrawStats() {}
}  // namespace snd
}  // namespace ls3d

class ISND_driver;
ISND_driver *g_sndDriver;

namespace {

// ---------------------------------------------------------------- the two sides

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }

World g_origWorld, g_portWorld;
FakeDInput g_origDInput(&g_origWorld), g_portDInput(&g_portWorld);
IGraph *g_orig;                         // the original's IGraph (its own vtable)
IGraph *g_port;

// The window both sides give SetCooperativeLevel (never used as a window).
HWND const kWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));

DWORD g_clock = 100000;
DWORD WINAPI FakeTimeGetTime() { return g_clock; }

// Points `module`'s import of dll!function at `replacement` (as compare_isnd).
bool PatchImport(HMODULE module, const char *dll, const char *function, void *replacement)
{
    uint8_t *base = reinterpret_cast<uint8_t *>(module);
    const IMAGE_NT_HEADERS *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char *>(base + d->Name), dll) != 0)
            continue;
        auto *names = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                                                                : d->FirstThunk));
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto *byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(byName->Name), function) != 0)
                continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<uintptr_t>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, old, &old);
            return true;
        }
    }
    return false;
}

// The engine state both sides keep apart from DirectInput.
uint32_t &OrigInitFlags() { return Orig<uint32_t>(0x100d1d18 + 0x2c); }

std::mt19937 g_rng;
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }

int g_failures;

// What the operations reached (the port's side; the original's equals it when nothing differs).
struct S_seen {
    int keys, overlays, names, noNames, lostReads, mapped, buttons;
} g_seen;
bool g_overlays[3];
long g_op;
const char *g_step = "";

void Fail(const std::string &what)
{
    if (g_failures++ < 10)
        std::printf("DIFFERENCE at operation %ld (%s): %s\n", g_op, g_step, what.c_str());
}

template <class T> void Check(const char *name, T o, T p)
{
    if (o != p)
        Fail(std::string(name) + ": original " + std::to_string(static_cast<long long>(o)) + ", port " +
             std::to_string(static_cast<long long>(p)));
}

void CompareState()
{
    if (g_origWorld.log != g_portWorld.log) {
        size_t i = 0;
        while (i < g_origWorld.log.size() && i < g_portWorld.log.size() && g_origWorld.log[i] == g_portWorld.log[i])
            ++i;
        Fail("DirectInput calls: original \"" + (i < g_origWorld.log.size() ? g_origWorld.log[i] : "(none)") +
             "\", port \"" + (i < g_portWorld.log.size() ? g_portWorld.log[i] : "(none)") + "\"");
    }
    g_origWorld.log.clear();
    g_portWorld.log.clear();
    Check("FPS overlay", Orig<bool>(0x100d21c4), ls3d::ig::g_showFps);
    Check("sound statistics overlay", Orig<bool>(0x100d21c5), ls3d::ig::g_showSoundStats);
    Check("sound failure log", Orig<bool>(0x100d21c6), ls3d::ig::g_logSoundFailures);
    const bool overlays[3] = {ls3d::ig::g_showFps, ls3d::ig::g_showSoundStats, ls3d::ig::g_logSoundFailures};
    for (int i = 0; i < 3; ++i) {
        g_seen.overlays += overlays[i] != g_overlays[i];
        g_overlays[i] = overlays[i];
    }
}

// The same script on both sides.
template <class F> void Script(F f)
{
    f(g_origWorld);
    f(g_portWorld);
}

uint8_t RandKeyByte()
{
    static const uint8_t kValues[] = {0, 0, 0, 0, 0, 0, 0x80, 0x80, 0x01, 0x81};
    return kValues[Rand(sizeof kValues)];
}

// A new keyboard state: a few keys down, often modifiers and the debug keys.
void NewKeys()
{
    uint8_t keys[256] = {};
    const int n = Rand(6);
    for (int i = 0; i < n; ++i)
        keys[Rand(256)] = RandKeyByte();
    static const uint8_t kSpecial[] = {DIK_LSHIFT, DIK_RSHIFT, DIK_LCONTROL, DIK_RCONTROL, DIK_LMENU, DIK_RMENU,
                                       DIK_F10,    DIK_F11,    DIK_F12,      DIK_A,        DIK_1,     DIK_SPACE};
    for (uint8_t key : kSpecial) {
        if (Rand(4) == 0)
            keys[key] = RandKeyByte();
    }
    if (Rand(8) == 0) {
        // The debug overlay chord with one of its keys.
        static const uint8_t kDebugKeys[] = {DIK_F10, DIK_F11, DIK_F12};
        keys[DIK_LSHIFT] = keys[DIK_LMENU] = keys[DIK_LCONTROL] = 0x80;
        keys[kDebugKeys[Rand(3)]] = 0x80;
    }
    Script([&](World &w) { std::memcpy(w.keys, keys, sizeof keys); });
}

void NewMouse(bool buttonsUp)
{
    DIMOUSESTATE2 m = {};
    m.lX = Rand(41) - 20;
    m.lY = Rand(41) - 20;
    m.lZ = Rand(5) ? 0 : (Rand(2) ? 120 : -120);
    for (BYTE &b : m.rgbButtons)
        b = (!buttonsUp && Rand(4) == 0) ? 0x80 : 0;
    Script([&](World &w) { w.mouse = m; });
}

// A key name as text (the fakes' names are ASCII).
std::string Wide(const wchar_t *s)
{
    if (!s)
        return "(null)";
    std::string text;
    for (; *s; ++s)
        text += static_cast<char>(*s);
    return text;
}

// The operations, each on both sides.
int g_counts[24];

void Step()
{
    const int op = Rand(24);
    ++g_counts[op];
    switch (op) {
    case 0: {
        g_step = "InitKeyboard";
        static const uint32_t kFlags[] = {0, IGKEY_EXCLUSIVE, IGKEY_REPEAT, IGKEY_EXCLUSIVE | IGKEY_REPEAT};
        const uint32_t flags = kFlags[Rand(4)];
        const int failure = Rand(12);
        Script([&](World &w) {
            w.failCreate = failure == 0;
            w.failFormat = failure == 1;
            w.failLevel = failure == 2;
        });
        NewKeys();
        Check("InitKeyboard", g_orig->InitKeyboard(flags), g_port->InitKeyboard(flags));
        Script([&](World &w) { w.failCreate = w.failFormat = w.failLevel = 0; });
        break;
    }
    case 1:
        g_step = "CloseKeyboard";
        Check("CloseKeyboard", g_orig->CloseKeyboard(), g_port->CloseKeyboard());
        break;
    case 2:
    case 3:
    case 4: {
        g_step = "ReadKeyboard";
        NewKeys();
        if (Rand(10) == 0) {
            const int fails = 1 + Rand(2), lost = Rand(3);
            Script([&](World &w) {
                w.failStates = fails;
                w.lostAcquires = lost;
            });
            ++g_seen.lostReads;
        }
        Check("ReadKeyboard", g_orig->ReadKeyboard(), g_port->ReadKeyboard());
        Script([&](World &w) { w.failStates = w.lostAcquires = 0; });
        break;
    }
    case 5:
    case 6:
        g_step = "GetKey";
        for (int i = Rand(4); i >= 0; --i) {
            const int key = g_port->GetKey();
            Check("GetKey", g_orig->GetKey(), key);
            g_seen.keys += key != 0;
            Check("GetKeyChar", g_orig->GetKeyChar(), g_port->GetKeyChar());
            g_clock += static_cast<DWORD>(Rand(60));
        }
        break;
    case 7: {
        g_step = "IsKeyDown";
        const int key = Rand(2) ? Rand(256) : Rand(0x10000);
        Check("IsKeyDown", g_orig->IsKeyDown(key), g_port->IsKeyDown(key));
        Check("IsShiftDown", g_orig->IsShiftDown(), g_port->IsShiftDown());
        Check("IsCtrlDown", g_orig->IsCtrlDown(), g_port->IsCtrlDown());
        Check("IsAltDown", g_orig->IsAltDown(), g_port->IsAltDown());
        break;
    }
    case 8: {
        g_step = "GetShiftKeys";
        const bool read = Rand(3) == 0;
        if (read)
            NewKeys();
        Check("GetShiftKeys", g_orig->GetShiftKeys(read), g_port->GetShiftKeys(read));
        break;
    }
    case 9: {
        g_step = "SetKeyMap";
        uint8_t keys[70];
        for (uint8_t &k : keys)
            k = static_cast<uint8_t>(Rand(4) ? Rand(256) : 0);
        const uint32_t count = static_cast<uint32_t>(Rand(8) ? Rand(65) : Rand(71));
        const uint8_t *p = Rand(20) ? keys : nullptr;
        Check("SetKeyMap", g_orig->SetKeyMap(p, count), g_port->SetKeyMap(p, count));
        break;
    }
    case 10: {
        g_step = "GetMappedKeys";
        const uint64_t mapped = g_port->GetMappedKeys();
        Check("GetMappedKeys", g_orig->GetMappedKeys(), mapped);
        g_seen.mapped += mapped != 0;
        break;
    }
    case 11: {
        g_step = "KeyToChar";
        const int key = Rand(2) ? Rand(256) : Rand(0x10000) - 0x8000;
        const uint32_t shift = static_cast<uint32_t>(Rand(0x800));
        Check("KeyToChar", g_orig->KeyToChar(key, shift), g_port->KeyToChar(key, shift));
        break;
    }
    case 12: {
        g_step = "GetKeyName";
        const int key = Rand(256);
        const std::string o = Wide(g_orig->GetKeyName(key)), p = Wide(g_port->GetKeyName(key));
        if (o != p)
            Fail("GetKeyName: original \"" + o + "\", port \"" + p + "\"");
        ++(p == "(null)" ? g_seen.noNames : g_seen.names);
        break;
    }
    case 13: {
        g_step = "InitMouse";
        static const int kModes[] = {-1, 0, 0, 1, 1, 2};
        const int mode = kModes[Rand(6)];
        const int failure = Rand(12);
        const DWORD axes = Rand(3) ? 3 : 2;
        // (From the cursor to exclusive mode the original waits for every button to be up.)
        NewMouse(true);
        Script([&](World &w) {
            w.failCreate = failure == 0;
            w.failFormat = failure == 1;
            w.failLevel = failure == 2;
            w.axes = axes;
        });
        Check("InitMouse", g_orig->InitMouse(mode), g_port->InitMouse(mode));
        Script([&](World &w) { w.failCreate = w.failFormat = w.failLevel = 0; });
        break;
    }
    case 14:
        g_step = "CloseMouse";
        Check("CloseMouse", g_orig->CloseMouse(), g_port->CloseMouse());
        break;
    case 15:
    case 16:
    case 17: {
        g_step = "ReadMouse";
        NewMouse(false);
        if (Rand(10) == 0) {
            const int fails = 1 + Rand(2);
            Script([&](World &w) { w.failStates = fails; });
            ++g_seen.lostReads;
        }
        Check("ReadMouse", g_orig->ReadMouse(), g_port->ReadMouse());
        Script([&](World &w) { w.failStates = 0; });
        Check("GetMouseDX", g_orig->GetMouseDX(), g_port->GetMouseDX());
        Check("GetMouseDY", g_orig->GetMouseDY(), g_port->GetMouseDY());
        Check("GetMouseDZ", g_orig->GetMouseDZ(), g_port->GetMouseDZ());
        const uint32_t buttons = g_port->GetMouseButtons();
        Check("GetMouseButtons", g_orig->GetMouseButtons(), buttons);
        g_seen.buttons += buttons != 0;
        Check("GetMouseX", g_orig->GetMouseX(), g_port->GetMouseX());
        Check("GetMouseY", g_orig->GetMouseY(), g_port->GetMouseY());
        break;
    }
    case 18: {
        g_step = "SetMousePos";
        const int mode = g_orig->InitMouse(-1);
        Check("mouse mode", mode, g_port->InitMouse(-1));
        if (mode != 0) {
            const int x = Rand(1600), y = Rand(1200);
            Check("SetMousePos", g_orig->SetMousePos(x, y), g_port->SetMousePos(x, y));
        }
        break;
    }
    case 19: {
        g_step = "swap buttons";
        const bool swap = Rand(2) != 0;
        Orig<bool>(0x100d1890) = swap;
        ls3d::ig::SetSwapMouseButtons(swap);
        break;
    }
    case 20:
        g_step = "initialised";
        Check("IsKeyboardInit", g_orig->IsKeyboardInit(), g_port->IsKeyboardInit());
        Check("IsMouseInit", g_orig->IsMouseInit(), g_port->IsMouseInit());
        break;
    case 21:
        g_step = "timer";
        g_clock += static_cast<DWORD>(Rand(500));
        if (Rand(2)) {
            Check("ReadTimer", g_orig->ReadTimer(), g_port->ReadTimer());
        } else {
            const DWORD minMs = Rand(4) ? 0 : static_cast<DWORD>(Rand(3)), maxMs = static_cast<DWORD>(Rand(300));
            Check("GetTimer", g_orig->GetTimer(minMs, maxMs), g_port->GetTimer(minMs, maxMs));
        }
        break;
    case 22: {
        g_step = "debug keys flag";
        const bool off = Rand(4) == 0;
        if (off) {
            OrigInitFlags() |= IG_NO_DEBUG_KEYS;
            ls3d::ig::g_init.flags |= IG_NO_DEBUG_KEYS;
        } else {
            OrigInitFlags() &= ~IG_NO_DEBUG_KEYS;
            ls3d::ig::g_init.flags &= ~IG_NO_DEBUG_KEYS;
        }
        break;
    }
    default:
        g_step = "clock";
        g_clock += static_cast<DWORD>(Rand(400));
        break;
    }
    CompareState();
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 100000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    g_rng.seed(seed);
    char dir[MAX_PATH];
    std::strcpy(dir, orig);
    if (char *slash = std::strrchr(dir, '\\'))
        *slash = 0;
    SetDllDirectoryA(dir);
    HMODULE dll = LoadLibraryA(orig);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", orig, GetLastError());
        return 2;
    }
    g_base = reinterpret_cast<uint8_t *>(dll);
    void *clock = reinterpret_cast<void *>(&FakeTimeGetTime);
    if (!PatchImport(dll, "WINMM.dll", "timeGetTime", clock) ||
        !PatchImport(GetModuleHandleA(nullptr), "WINMM.dll", "timeGetTime", clock)) {
        std::printf("cannot patch timeGetTime\n");
        return 2;
    }
    // The original's IGraph through the port's class: the virtual tables match slot for slot.
    g_orig = reinterpret_cast<IGraph *(__cdecl *)()>(GetProcAddress(dll, "GetIGraph"))();
    g_pIGraph = g_port = new IGraph;
    ls3d::g_logDisabled = true;
    Orig<uint8_t>(0x100d16f2) = 1;              // the original's log off too

    // Each side's DirectInput object and window.
    Orig<IDirectInput8A *>(0x100d1d0c) = &g_origDInput;
    ls3d::ig::g_dinput = ls3d::input::dinput::Handle(static_cast<IDirectInput8A *>(&g_portDInput));
    Orig<HWND>(0x100d19ec) = kWindow;
    ls3d::ig::g_mainWnd = kWindow;
    OrigInitFlags() = 0;
    ls3d::ig::g_init.flags = 0;

    for (g_op = 0; g_op < ops && g_failures == 0; ++g_op)
        Step();
    g_step = "close";
    Check("CloseKeyboard", g_orig->CloseKeyboard(), g_port->CloseKeyboard());
    Check("CloseMouse", g_orig->CloseMouse(), g_port->CloseMouse());
    CompareState();

    std::printf("operations by kind:");
    for (int c : g_counts)
        std::printf(" %d", c);
    std::printf("\ndevices created: %zu / %zu\n", g_origWorld.devices.size(), g_portWorld.devices.size());
    std::printf("keys returned %d, overlay switches %d, key names %d (missing %d), lost reads %d, mapped keys %d, "
                "mouse buttons %d\n",
                g_seen.keys, g_seen.overlays, g_seen.names, g_seen.noNames, g_seen.lostReads, g_seen.mapped,
                g_seen.buttons);
    std::printf("input: %ld operations, seed %u: %s\n", g_op, seed, g_failures ? "DIFFERENT" : "identical");
    return g_failures ? 1 : 0;
}
