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

// ---------------------------------------------------------------- the fake DirectInput objects

// One side's fake DirectInput: its devices, the scripted states and the log of every call.
struct World {
    std::vector<std::string> log;
    std::vector<IDirectInputDevice8A *> devices;
    uint8_t keys[256] = {};
    DIMOUSESTATE2 mouse = {};
    DWORD axes = 3;
    int failStates = 0;                 // GetDeviceState calls to fail with DIERR_INPUTLOST
    int lostAcquires = 0;               // Acquire calls to fail with DIERR_INPUTLOST
    int failCreate = 0;                 // CreateDevice calls to fail
    int failFormat = 0;                 // SetDataFormat calls to fail
    int failLevel = 0;                  // SetCooperativeLevel calls to fail

    void Log(const char *fmt, ...)
    {
        char text[160];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        log.push_back(text);
    }
};

class FakeDevice : public IDirectInputDevice8A {
public:
    FakeDevice(World *world, bool mouse, int id) : w(world), isMouse(mouse), id(id) {}

    HRESULT __stdcall QueryInterface(REFIID, LPVOID *) override { return Unexpected("QueryInterface"); }
    ULONG __stdcall AddRef() override
    {
        Log("AddRef");
        return ++refs;
    }
    ULONG __stdcall Release() override
    {
        Log("Release");
        return --refs;
    }
    HRESULT __stdcall GetCapabilities(LPDIDEVCAPS caps) override
    {
        Log("GetCapabilities %lu", caps->dwSize);
        const DWORD size = caps->dwSize;
        std::memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwAxes = isMouse ? w->axes : 0;
        caps->dwButtons = isMouse ? 5 : 128;
        return DI_OK;
    }
    HRESULT __stdcall EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) override
    {
        return Unexpected("EnumObjects");
    }
    HRESULT __stdcall GetProperty(REFGUID property, LPDIPROPHEADER header) override
    {
        // (DIPROP_KEYNAME is a small integer posing as a GUID reference: logged as that number.)
        Log("GetProperty %u %lu %lu %lu %lu", static_cast<unsigned>(reinterpret_cast<uintptr_t>(&property)),
            header->dwSize, header->dwHeaderSize, header->dwObj, header->dwHow);
        if (header->dwObj % 7 == 0)
            return DIERR_NOTFOUND;
        auto *name = reinterpret_cast<DIPROPSTRING *>(header);
        std::swprintf(name->wsz, MAX_PATH, L"Key %lu", header->dwObj);
        return DI_OK;
    }
    HRESULT __stdcall SetProperty(REFGUID, LPCDIPROPHEADER) override { return Unexpected("SetProperty"); }
    HRESULT __stdcall Acquire() override
    {
        Log("Acquire");
        if (w->lostAcquires > 0) {
            --w->lostAcquires;
            return DIERR_INPUTLOST;
        }
        return DI_OK;
    }
    HRESULT __stdcall Unacquire() override
    {
        Log("Unacquire");
        return DI_OK;
    }
    HRESULT __stdcall GetDeviceState(DWORD size, LPVOID data) override
    {
        Log("GetDeviceState %lu", size);
        if (w->failStates > 0) {
            --w->failStates;
            return DIERR_INPUTLOST;
        }
        if (isMouse)
            std::memcpy(data, &w->mouse, size < sizeof w->mouse ? size : sizeof w->mouse);
        else
            std::memcpy(data, w->keys, size < sizeof w->keys ? size : sizeof w->keys);
        return DI_OK;
    }
    HRESULT __stdcall GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD) override
    {
        return Unexpected("GetDeviceData");
    }
    HRESULT __stdcall SetDataFormat(LPCDIDATAFORMAT format) override
    {
        // (Each side passes its own copy of c_dfDIKeyboard / c_dfDIMouse2: compared by content.)
        Log("SetDataFormat %lu %lu %lu %lu %lu", format->dwSize, format->dwObjSize, format->dwFlags, format->dwDataSize,
            format->dwNumObjs);
        if (w->failFormat > 0) {
            --w->failFormat;
            return DIERR_INVALIDPARAM;
        }
        return DI_OK;
    }
    HRESULT __stdcall SetEventNotification(HANDLE) override { return Unexpected("SetEventNotification"); }
    HRESULT __stdcall SetCooperativeLevel(HWND window, DWORD flags) override
    {
        Log("SetCooperativeLevel %p %lx", static_cast<void *>(window), flags);
        if (w->failLevel > 0) {
            --w->failLevel;
            return DIERR_INVALIDPARAM;
        }
        return DI_OK;
    }
    HRESULT __stdcall GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override
    {
        return Unexpected("GetObjectInfo");
    }
    HRESULT __stdcall GetDeviceInfo(LPDIDEVICEINSTANCEA) override { return Unexpected("GetDeviceInfo"); }
    HRESULT __stdcall RunControlPanel(HWND, DWORD) override { return Unexpected("RunControlPanel"); }
    HRESULT __stdcall Initialize(HINSTANCE, DWORD, REFGUID) override { return Unexpected("Initialize"); }
    HRESULT __stdcall CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT *, LPUNKNOWN) override
    {
        return Unexpected("CreateEffect");
    }
    HRESULT __stdcall EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override
    {
        return Unexpected("EnumEffects");
    }
    HRESULT __stdcall GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return Unexpected("GetEffectInfo"); }
    HRESULT __stdcall GetForceFeedbackState(LPDWORD) override { return Unexpected("GetForceFeedbackState"); }
    HRESULT __stdcall SendForceFeedbackCommand(DWORD) override { return Unexpected("SendForceFeedbackCommand"); }
    HRESULT __stdcall EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override
    {
        return Unexpected("EnumCreatedEffectObjects");
    }
    HRESULT __stdcall Escape(LPDIEFFESCAPE) override { return Unexpected("Escape"); }
    HRESULT __stdcall Poll() override { return Unexpected("Poll"); }
    HRESULT __stdcall SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override
    {
        return Unexpected("SendDeviceData");
    }
    HRESULT __stdcall EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override
    {
        return Unexpected("EnumEffectsInFile");
    }
    HRESULT __stdcall WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override
    {
        return Unexpected("WriteEffectToFile");
    }
    HRESULT __stdcall BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return Unexpected("BuildActionMap"); }
    HRESULT __stdcall SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return Unexpected("SetActionMap"); }
    HRESULT __stdcall GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return Unexpected("GetImageInfo"); }

private:
    void Log(const char *fmt, ...)
    {
        char text[160];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        w->Log("%c%d.%s", isMouse ? 'M' : 'K', id, text);
    }
    HRESULT Unexpected(const char *name)
    {
        Log("UNEXPECTED %s", name);
        return E_NOTIMPL;
    }

    World *w;
    bool isMouse;
    int id;
    ULONG refs = 1;
};

class FakeDInput : public IDirectInput8A {
public:
    explicit FakeDInput(World *world) : w(world) {}

    HRESULT __stdcall QueryInterface(REFIID, LPVOID *) override { return Unexpected("QueryInterface"); }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override
    {
        w->Log("DI.Release");
        return 0;
    }
    HRESULT __stdcall CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A *device, LPUNKNOWN outer) override
    {
        const bool mouse = IsEqualGUID(guid, GUID_SysMouse) != 0;
        const bool keyboard = IsEqualGUID(guid, GUID_SysKeyboard) != 0;
        w->Log("DI.CreateDevice %s %d", mouse ? "mouse" : keyboard ? "keyboard" : "?", outer != nullptr);
        if (w->failCreate > 0) {
            --w->failCreate;
            return DIERR_DEVICENOTREG;
        }
        auto *d = new FakeDevice(w, mouse, static_cast<int>(w->devices.size()));
        w->devices.push_back(d);
        *device = d;
        return DI_OK;
    }
    HRESULT __stdcall EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) override
    {
        return Unexpected("EnumDevices");
    }
    HRESULT __stdcall GetDeviceStatus(REFGUID) override { return Unexpected("GetDeviceStatus"); }
    HRESULT __stdcall RunControlPanel(HWND, DWORD) override { return Unexpected("RunControlPanel"); }
    HRESULT __stdcall Initialize(HINSTANCE, DWORD) override { return Unexpected("Initialize"); }
    HRESULT __stdcall FindDevice(REFGUID, LPCSTR, LPGUID) override { return Unexpected("FindDevice"); }
    HRESULT __stdcall EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID,
                                             DWORD) override
    {
        return Unexpected("EnumDevicesBySemantics");
    }
    HRESULT __stdcall ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD,
                                       LPVOID) override
    {
        return Unexpected("ConfigureDevices");
    }

private:
    HRESULT Unexpected(const char *name)
    {
        w->Log("DI.UNEXPECTED %s", name);
        return E_NOTIMPL;
    }

    World *w;
};

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
