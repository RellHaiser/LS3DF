// Differential test of IGraph's window procedure and message loop (the platform layer,
// src/platform, with IGraph's window decisions in src/igraph/igraph.cpp): the original LS3DF.dll's
// window procedure (0x1006b820) and the port's (the platform's "LS3D" window class) get the same
// messages for a hidden test window that both sides take as their main window; the same posted
// messages go through each side's ProcessWinMessages. Return values, the activation state, the
// application name, the titles each side gives the window, the quit messages, the typed characters
// (ReadChar), the window-message keyboard (GetKey / GetKeyChar with IGKEY_WINDOW_MESSAGES) and the
// dialog list must be the same.
// (Not covered: creating the windows (Init), WM_CLOSE while initialised (the process exits), the
// message loop's waiting while inactive, the cursor and the accessibility keys.)
//
// Usage: compare_window.exe [original LS3DF.dll] [operations] [seed]
//
// test-sources: src/igraph/*.cpp src/core/debug.cpp src/core/cpu.cpp src/core/enhance.cpp src/dx/dxerr8.cpp src/i3d/ib_manager.cpp src/i3d/vb_manager.cpp src/dshow/*.cpp
// test-sources: src/gfx/d3d8/gfx_d3d8.cpp src/audio/dsound/audio_dsound.cpp src/input/dinput/input_dinput.cpp
// test-sources: src/platform/win32/platform_win32.cpp
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/core/debug.h"
#include "../src/i3d/driver.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/igraph_state.h"
#include "../src/input/dinput/dinput_device.h"
#include "../src/platform/platform.h"
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

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }

IGraph *g_orig;                         // the original's IGraph (its own vtable)
IGraph *g_port;
WNDPROC g_origProc;                     // 0x1006b820
WNDPROC g_portProc;                     // the platform's
World g_origWorld, g_portWorld;
FakeDInput g_origDInput(&g_origWorld), g_portDInput(&g_portWorld);

HWND g_window;                          // both sides' main window (hidden)
HWND g_dialog;                          // a window both sides may take as a dialog

// The engine state the window code reads, on both sides.
bool &OrigInitialized() { return Orig<bool>(0x100d2195); }
bool &OrigInactive() { return Orig<bool>(0x100d2190); }
bool &OrigNoActivation() { return Orig<bool>(0x100d2191); }
uint32_t &OrigInitFlags() { return Orig<uint32_t>(0x100d1d18 + 0x2c); }

std::mt19937 g_rng;
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }

int g_failures;

// What the operations reached (the port's side; the original's equals it when nothing differs).
struct S_seen {
    int deactivations, reactivations, swallowed, quits, chars, keys;
} g_seen;
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

void CheckText(const char *name, const std::string &o, const std::string &p)
{
    if (o != p)
        Fail(std::string(name) + ": original \"" + o + "\", port \"" + p + "\"");
}

std::string Title()
{
    char text[0x200] = {};
    GetWindowTextA(g_window, text, sizeof text);
    return text;
}

// Empties the thread's message queue (the quit message too). (Windows hands out a quit message only
// when no other message waits, so each side handles a message on an empty queue.)
void Drain()
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    }
}

// A quit message waiting (WM_DESTROY posts one), removed.
bool TakeQuit()
{
    MSG msg;
    return PeekMessageA(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE;
}

// The result of one side's handling of a message.
struct S_result {
    LRESULT value;
    bool inactive;
    std::string appName, title;
    bool quit;
};

S_result Send(bool orig, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Drain();
    S_result r;
    r.value = (orig ? g_origProc : g_portProc)(g_window, msg, wParam, lParam);
    r.inactive = orig ? OrigInactive() : ls3d::ig::g_inactive;
    r.appName = (orig ? g_orig : g_port)->GetAppName();
    r.title = Title();
    r.quit = TakeQuit();
    return r;
}

void SendBoth(UINT msg, WPARAM wParam, LPARAM lParam)
{
    const bool wasInactive = ls3d::ig::g_inactive;
    const S_result o = Send(true, msg, wParam, lParam);
    const S_result p = Send(false, msg, wParam, lParam);
    g_seen.deactivations += !wasInactive && p.inactive;
    g_seen.reactivations += wasInactive && !p.inactive;
    g_seen.swallowed += p.value == 1;
    g_seen.quits += p.quit;
    Check("window procedure", o.value, p.value);
    Check("inactive", o.inactive, p.inactive);
    CheckText("application name", o.appName, p.appName);
    CheckText("window title", o.title, p.title);
    Check("quit message", o.quit, p.quit);
}

// One message to post: to the main window or the dialog.
struct S_post {
    bool dialog;
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
};

std::vector<S_post> RandomMessages()
{
    static const WPARAM kKeys[] = {VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_DELETE, 'A', 'Q', '5', VK_F1, VK_SPACE};
    static const WPARAM kChars[] = {'a', 'Z', '7', ' ', '-', '/', 8, 9, 0x1b, 0x0d, 0xe9, '~'};
    std::vector<S_post> out;
    for (int n = Rand(6); n >= 0; --n) {
        S_post m;
        m.dialog = Rand(5) == 0;
        if (Rand(2)) {
            m.msg = WM_CHAR;
            m.wParam = kChars[Rand(sizeof kChars / sizeof kChars[0])];
            m.lParam = Rand(4) ? 1 : static_cast<LPARAM>(0x80000001u);   // bit 31: a released key
        } else {
            m.msg = WM_KEYDOWN;
            m.wParam = kKeys[Rand(sizeof kKeys / sizeof kKeys[0])];
            m.lParam = 1 | (static_cast<LPARAM>(MapVirtualKeyA(static_cast<UINT>(m.wParam), 0)) << 16);
        }
        out.push_back(m);
    }
    return out;
}

// What one side's ProcessWinMessages made of the messages.
struct S_pumped {
    std::vector<int> chars;
    int key, keyChar;
};

S_pumped Pump(bool orig, const std::vector<S_post> &messages)
{
    Drain();
    for (const S_post &m : messages)
        PostMessageA(m.dialog ? g_dialog : g_window, m.msg, m.wParam, m.lParam);
    IGraph *ig = orig ? g_orig : g_port;
    ig->ProcessWinMessages();
    S_pumped r;
    for (int i = 0; i < 0x20; ++i) {
        const int ch = ig->ReadChar();
        if (!ch)
            break;
        r.chars.push_back(ch);
    }
    r.key = ig->GetKey();
    r.keyChar = ig->GetKeyChar();
    return r;
}

std::string Chars(const std::vector<int> &chars)
{
    std::string text;
    for (int c : chars)
        text += std::to_string(c) + " ";
    return text;
}

int g_counts[12];

void Step()
{
    const int op = Rand(12);
    ++g_counts[op];
    switch (op) {
    case 0:
    case 1:
    case 2: {
        g_step = "WM_ACTIVATE";
        static const WORD kStates[] = {WA_INACTIVE, WA_ACTIVE, WA_CLICKACTIVE};
        const WPARAM wParam = MAKEWPARAM(kStates[Rand(3)], Rand(4) == 0 ? 1 : 0);
        SendBoth(WM_ACTIVATE, wParam, 0);
        break;
    }
    case 3: {
        g_step = "engine state";
        const bool noActivation = Rand(5) == 0, wait = Rand(2) != 0, initialized = Rand(2) != 0;
        OrigNoActivation() = ls3d::ig::g_noActivation = noActivation;
        OrigInitialized() = ls3d::ig::g_initialized = initialized;
        if (wait) {
            OrigInitFlags() |= IG_WAIT_INACTIVE;
            ls3d::ig::g_init.flags |= IG_WAIT_INACTIVE;
        } else {
            OrigInitFlags() &= ~IG_WAIT_INACTIVE;
            ls3d::ig::g_init.flags &= ~IG_WAIT_INACTIVE;
        }
        break;
    }
    case 4: {
        g_step = "SetAppName";
        static const char *const kNames[] = {"HD2", "Hidden & Dangerous 2", "", "LS3D", nullptr};
        std::string longName(300, 'x');
        const char *name = Rand(8) == 0 ? longName.c_str() : kNames[Rand(5)];
        const bool o = g_orig->SetAppName(name);
        const std::string oTitle = Title();
        const bool p = g_port->SetAppName(name);
        Check("SetAppName", o, p);
        CheckText("window title", oTitle, Title());
        CheckText("application name", g_orig->GetAppName(), g_port->GetAppName());
        break;
    }
    case 5: {
        g_step = "system keys";
        const bool up = Rand(2) != 0;
        if (Rand(2))
            SendBoth(up ? WM_SYSKEYUP : WM_SYSKEYDOWN, VK_F10, 1);
        else
            SendBoth(up ? WM_SYSKEYUP : WM_SYSKEYDOWN, 'A' + Rand(26), 0x20000001);   // with Alt
        break;
    }
    case 6:
        g_step = "WM_SYSCOMMAND";
        SendBoth(WM_SYSCOMMAND, (Rand(2) ? SC_SCREENSAVE : SC_MONITORPOWER) | Rand(16), 0);
        break;
    case 7:
        g_step = "WM_ERASEBKGND / WM_DESTROY / WM_CLOSE";
        switch (Rand(3)) {
        case 0: SendBoth(WM_ERASEBKGND, 0, 0); break;
        case 1: SendBoth(WM_DESTROY, 0, 0); break;
        default:
            // (Only while not initialised: an initialised engine closes and exits.)
            if (!OrigInitialized())
                SendBoth(WM_CLOSE, Rand(3), 0);
            break;
        }
        break;
    case 8:
    case 9: {
        g_step = "ProcessWinMessages";
        // (The loop waits for messages while the game is inactive: not here.)
        if (OrigInactive() && !OrigNoActivation() && (OrigInitFlags() & IG_WAIT_INACTIVE)) {
            OrigInitFlags() &= ~IG_WAIT_INACTIVE;
            ls3d::ig::g_init.flags &= ~IG_WAIT_INACTIVE;
        }
        const std::vector<S_post> messages = RandomMessages();
        const S_pumped o = Pump(true, messages);
        const S_pumped p = Pump(false, messages);
        CheckText("typed characters", Chars(o.chars), Chars(p.chars));
        Check("GetKey", o.key, p.key);
        Check("GetKeyChar", o.keyChar, p.keyChar);
        g_seen.chars += static_cast<int>(p.chars.size());
        g_seen.keys += p.key != 0;
        break;
    }
    case 10: {
        g_step = "keyboard mode";
        const uint32_t flags = Rand(3) ? IGKEY_WINDOW_MESSAGES : 0;
        Check("InitKeyboard", g_orig->InitKeyboard(flags), g_port->InitKeyboard(flags));
        if (g_origWorld.log != g_portWorld.log)
            Fail("DirectInput calls");
        g_origWorld.log.clear();
        g_portWorld.log.clear();
        break;
    }
    default: {
        g_step = "dialogs";
        static HWND const kOther = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x5678));
        HWND const kDialogs[] = {g_dialog, nullptr, kOther};
        HWND const dialog = kDialogs[Rand(3)];
        if (Rand(2))
            Check("AddDlg", g_orig->AddDlg(dialog), g_port->AddDlg(dialog));
        else
            Check("DelDlg", g_orig->DelDlg(dialog), g_port->DelDlg(dialog));
        break;
    }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 20000;
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
    // The original's IGraph through the port's class: the virtual tables match slot for slot.
    g_orig = reinterpret_cast<IGraph *(__cdecl *)()>(GetProcAddress(dll, "GetIGraph"))();
    g_pIGraph = g_port = new IGraph;
    ls3d::g_logDisabled = true;
    Orig<uint8_t>(0x100d16f2) = 1;              // the original's log off too

    // The test's windows live on a desktop of their own: the window procedure's activation gives
    // them the keyboard focus, and on the user's desktop real typing would then reach the queue.
    HDESK desktop = CreateDesktopA("LS3DFTestDesktop", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        std::printf("cannot use a desktop of the test's own (%lu)\n", GetLastError());
        return 2;
    }

    // The window procedures: the original's by address, the port's from its window class.
    g_origProc = reinterpret_cast<WNDPROC>(g_base + (0x1006b820 - 0x10000000));
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    if (!ls3d::platform::RegisterWindowClass(instance) || !GetClassInfoExA(instance, "LS3D", &wc)) {
        std::printf("cannot register the platform's window class\n");
        return 2;
    }
    g_portProc = wc.lpfnWndProc;

    // The test's own hidden windows.
    WNDCLASSEXA test = {};
    test.cbSize = sizeof test;
    test.lpfnWndProc = DefWindowProcA;
    test.hInstance = instance;
    test.lpszClassName = "LS3DTestWindow";
    RegisterClassExA(&test);
    g_window = CreateWindowExA(0, "LS3DTestWindow", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, instance,
                               nullptr);
    g_dialog = CreateWindowExA(0, "LS3DTestWindow", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, instance,
                               nullptr);
    if (!g_window || !g_dialog) {
        std::printf("cannot create the test windows\n");
        return 2;
    }
    Orig<HWND>(0x100d19ec) = g_window;
    ls3d::ig::g_mainWnd = g_window;
    OrigInitFlags() = 0;
    ls3d::ig::g_init.flags = 0;
    Orig<IDirectInput8A *>(0x100d1d0c) = &g_origDInput;
    ls3d::ig::g_dinput = ls3d::input::dinput::Handle(static_cast<IDirectInput8A *>(&g_portDInput));

    for (g_op = 0; g_op < ops && g_failures == 0; ++g_op)
        Step();

    std::printf("operations by kind:");
    for (int c : g_counts)
        std::printf(" %d", c);
    std::printf("\ndeactivations %d, reactivations %d, messages swallowed %d, quit messages %d, typed characters %d, "
                "window-message keys %d\n",
                g_seen.deactivations, g_seen.reactivations, g_seen.swallowed, g_seen.quits, g_seen.chars, g_seen.keys);
    std::printf("window: %ld operations, seed %u: %s\n", g_op, seed, g_failures ? "DIFFERENT" : "identical");
    return g_failures ? 1 : 0;
}
