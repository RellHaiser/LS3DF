// The Win32 platform (platform/platform.h): the window class and procedure, the message loop and
// the other Windows calls IGraph made itself (IGraph.cpp), moved here unchanged.
#include "platform/platform.h"

#include <mmsystem.h>

#include "core/c_vector.h"
#include "dx/dinput_types.h"

namespace ls3d::platform {

namespace {

const char kWindowClass[] = "LS3D";
const char kWindowTitle[] = "LS3D engine";

WindowEvents *s_events = nullptr;
C_vector<HWND> s_dialogs(0x20);                 // 0x100d19b0

// Accessibility keys switched off while the engine runs (SPI_GET/SETSTICKYKEYS ...). Only the
// sticky keys structure gets its size: the filter and toggle keys calls fail as in the original.
STICKYKEYS s_stickyKeys;                        // 0x100d1e3c
FILTERKEYS s_filterKeys;                        // 0x100d1ddc
TOGGLEKEYS s_toggleKeys;                        // 0x100d2164

// 0x1006b820: IGraph's window procedure.
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 0;
    case WM_DESTROY:
        if (!s_events->OnDestroy())
            return 1;
        PostQuitMessage(0);
        break;
    case WM_CLOSE:
        // (When the engine is initialised, OnClose closes it and exits.)
        if (!s_events->OnClose(static_cast<int>(wParam)))
            return 1;
        break;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        // F10 and Alt combinations would open the window menu.
        if (wParam == VK_F10 || (lParam & 0x20000000))
            return 1;
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_SCREENSAVE || (wParam & 0xfff0) == SC_MONITORPOWER)
            return 1;
        break;
    case WM_ACTIVATE:
        s_events->OnActivate(LOWORD(wParam) != WA_INACTIVE, HIWORD(wParam) != 0);
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// The window-message keyboard (IGKEY_WINDOW_MESSAGES): arrows and delete from WM_KEYDOWN, the rest
// from WM_CHAR, as DirectInput codes.
void KeyFromMessage(const MSG &msg)
{
    if (msg.message == WM_KEYDOWN) {
        switch (msg.wParam) {
        case VK_LEFT: s_events->OnKeyMessage(DIK_LEFT, 0); break;
        case VK_UP: s_events->OnKeyMessage(DIK_UP, 0); break;
        case VK_RIGHT: s_events->OnKeyMessage(DIK_RIGHT, 0); break;
        case VK_DOWN: s_events->OnKeyMessage(DIK_DOWN, 0); break;
        case VK_DELETE: s_events->OnKeyMessage(DIK_DELETE, 0); break;
        }
    } else if (msg.message == WM_CHAR && static_cast<LONG>(msg.lParam) >= 0) {
        switch (msg.wParam) {
        case 8:
            s_events->OnKeyMessage(DIK_BACK, 8);
            break;
        case 9:
            s_events->OnKeyMessage(DIK_TAB, 9);
            break;
        case 0x1b:
            s_events->OnKeyMessage(DIK_ESCAPE, 0x1b);
            break;
        default: {
            const SHORT vk = VkKeyScanA(static_cast<CHAR>(msg.wParam));
            if (vk == -1)
                s_events->OnKeyMessage(0, 0);
            else
                s_events->OnKeyMessage(static_cast<int>(MapVirtualKeyA(vk & 0xff, 0)), static_cast<int>(msg.wParam));
        }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- time

uint32_t Milliseconds() { return timeGetTime(); }
uint32_t TickCount() { return GetTickCount(); }
void Pause(uint32_t ms) { Sleep(ms); }

// ---------------------------------------------------------------- threads and locks

// The handles are the Windows thread handles and critical sections.
Thread *StartThread(ThreadProc proc, void *param)
{
    DWORD threadId;
    return reinterpret_cast<Thread *>(CreateThread(nullptr, 0, proc, param, 0, &threadId));
}
void SetPriority(Thread *thread, int priority) { SetThreadPriority(reinterpret_cast<HANDLE>(thread), priority); }
void Suspend(Thread *thread) { SuspendThread(reinterpret_cast<HANDLE>(thread)); }
void Resume(Thread *thread) { ResumeThread(reinterpret_cast<HANDLE>(thread)); }
void Terminate(Thread *thread, uint32_t exitCode) { TerminateThread(reinterpret_cast<HANDLE>(thread), exitCode); }
void Join(Thread *thread) { WaitForSingleObject(reinterpret_cast<HANDLE>(thread), INFINITE); }
void Release(Thread *thread) { CloseHandle(reinterpret_cast<HANDLE>(thread)); }

Mutex *NewMutex()
{
    auto *section = new CRITICAL_SECTION;
    InitializeCriticalSection(section);
    return reinterpret_cast<Mutex *>(section);
}
void DeleteMutex(Mutex *mutex)
{
    auto *section = reinterpret_cast<CRITICAL_SECTION *>(mutex);
    DeleteCriticalSection(section);
    delete section;
}
void Lock(Mutex *mutex) { EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(mutex)); }
void Unlock(Mutex *mutex) { LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(mutex)); }

// ---------------------------------------------------------------- the window

void SetWindowEvents(WindowEvents *events) { s_events = events; }

bool RegisterWindowClass(HINSTANCE instance)
{
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    return RegisterClassExA(&wc) != 0;
}

HWND CreateMainWindow(HINSTANCE instance, bool fullscreen, int x, int y, int width, int height, bool withMenu,
                      HMENU menu)
{
    if (fullscreen)
        return CreateWindowExA(WS_EX_TOPMOST, kWindowClass, kWindowTitle, WS_VISIBLE, x, y, width, height,
                               GetDesktopWindow(), menu, instance, nullptr);
    const int frameX = GetSystemMetrics(SM_CXFIXEDFRAME);
    const int frameY = GetSystemMetrics(SM_CYFIXEDFRAME);
    const int caption = GetSystemMetrics(SM_CYCAPTION);
    const int menuHeight = withMenu ? GetSystemMetrics(SM_CYMENU) : 0;
    const DWORD style = WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    return CreateWindowExA(0, kWindowClass, kWindowTitle, style, x - frameX, y - menuHeight - caption - frameY,
                           width + frameX * 2, height + frameY * 2 + menuHeight + caption, GetDesktopWindow(), menu,
                           instance, nullptr);
}

HWND CreateChildWindow(HINSTANCE instance, HWND parent, int x, int y, int width, int height)
{
    return CreateWindowExA(0, kWindowClass, kWindowTitle, WS_CHILD | WS_VISIBLE, x, y, width, height, parent, nullptr,
                           instance, nullptr);
}

HMENU LoadDefaultMenu()
{
    return LoadMenuA(GetModuleHandleA("LS3D.DLL"), MAKEINTRESOURCEA(123));
}

void Refresh(HWND window) { UpdateWindow(window); }
void Focus(HWND window) { SetFocus(window); }
bool SetTitle(HWND window, const char *title) { return SetWindowTextA(window, title) != FALSE; }

bool ForegroundIsOwn(HWND window)
{
    const HWND foreground = GetForegroundWindow();
    return IsChild(window, foreground) || window == foreground;
}

POINT ClientOrigin(HWND window, bool framed, bool withMenu)
{
    RECT rc;
    GetWindowRect(window, &rc);
    if (framed) {
        const int frameX = GetSystemMetrics(SM_CXFIXEDFRAME);
        const int frameY = GetSystemMetrics(SM_CYFIXEDFRAME);
        const int caption = GetSystemMetrics(SM_CYCAPTION);
        const int menu = withMenu ? GetSystemMetrics(SM_CYMENU) : 0;
        rc.left += frameX;
        rc.top += menu + caption + frameY;
    }
    return {rc.left, rc.top};
}

bool AddDialog(HWND dialog)
{
    if (!dialog)
        return false;
    s_dialogs.PushBack(dialog);
    return true;
}

bool RemoveDialog(HWND dialog)
{
    return s_dialogs.RemoveSwap(dialog);
}

// 0x1006a470 (IGraph::ProcessWinMessages)
void PumpMessages()
{
    for (;;) {
        MSG msg;
        while (!PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE | PM_NOYIELD)) {
            if (!s_events->WaitWhileIdle())
                return;
            WaitMessage();
        }
        int dialog = 0;
        for (; dialog < s_dialogs.Size(); ++dialog) {
            if (IsDialogMessageA(s_dialogs[dialog], &msg))
                break;
        }
        // Typed characters (not repeats of a held key) for ReadChar.
        if (msg.message == WM_CHAR && static_cast<LONG>(msg.lParam) >= 0)
            s_events->OnChar(static_cast<int>(msg.wParam));
        if (s_events->KeyboardFromMessages())
            KeyFromMessage(msg);
        if (dialog == s_dialogs.Size()) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

// ---------------------------------------------------------------- the mouse and keyboard

bool SetCursorPosition(int x, int y) { return SetCursorPos(x, y) != FALSE; }
void ReleaseCursorClip() { ClipCursor(nullptr); }
bool MouseButtonsSwapped() { return GetSystemMetrics(SM_SWAPBUTTON) != 0; }

void DisableAccessibilityKeys()
{
    // The original switches filter and toggle keys "off" only when sticky keys had flags, and never
    // sets their structure sizes, so those calls fail.
    s_stickyKeys.cbSize = sizeof s_stickyKeys;
    SystemParametersInfoA(SPI_GETSTICKYKEYS, sizeof s_stickyKeys, &s_stickyKeys, 0);
    const DWORD stickyFlags = s_stickyKeys.dwFlags;
    if (stickyFlags) {
        s_stickyKeys.dwFlags = 0;
        SystemParametersInfoA(SPI_SETSTICKYKEYS, s_stickyKeys.cbSize, &s_stickyKeys, 0);
        s_stickyKeys.dwFlags = stickyFlags;
    }
    SystemParametersInfoA(SPI_GETFILTERKEYS, s_filterKeys.cbSize, &s_filterKeys, 0);
    if (s_stickyKeys.dwFlags) {
        const DWORD saved = s_filterKeys.dwFlags;
        s_filterKeys.dwFlags = 0;
        SystemParametersInfoA(SPI_SETFILTERKEYS, s_filterKeys.cbSize, &s_filterKeys, 0);
        s_filterKeys.dwFlags = saved;
    }
    SystemParametersInfoA(SPI_GETTOGGLEKEYS, s_toggleKeys.cbSize, &s_toggleKeys, 0);
    if (s_stickyKeys.dwFlags) {
        const DWORD saved = s_toggleKeys.dwFlags;
        s_toggleKeys.dwFlags = 0;
        SystemParametersInfoA(SPI_SETTOGGLEKEYS, s_toggleKeys.cbSize, &s_toggleKeys, 0);
        s_toggleKeys.dwFlags = saved;
    }
}

void RestoreAccessibilityKeys()
{
    SystemParametersInfoA(SPI_SETSTICKYKEYS, s_stickyKeys.cbSize, &s_stickyKeys, 0);
    SystemParametersInfoA(SPI_SETFILTERKEYS, s_filterKeys.cbSize, &s_filterKeys, 0);
    SystemParametersInfoA(SPI_SETTOGGLEKEYS, s_toggleKeys.cbSize, &s_toggleKeys, 0);
}

}  // namespace ls3d::platform
