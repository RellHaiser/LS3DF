// The platform: the operating system services the engine uses besides drawing, sound and input -
// the window and its messages, the mouse cursor, time, threads and locks.
//
// Unlike the devices (gfx, audio, input), the platform is chosen when the DLL is built: one backend
// source is compiled in (platform/win32 so far). Nothing needs two platforms in one process, and
// the engine reads the clock from static initialisers, before any object could be set up.
//
// The engine keeps its decisions: IGraph decides what activating, deactivating or closing the
// window does, when the message loop waits, what a typed character or a key message means
// (WindowEvents). The backend does the operating system's part: the window class and procedure,
// creating windows, the message loop, dialogs, the cursor. Window, menu and instance handles are
// the native ones (HWND, HMENU, HINSTANCE), as IGraph's interface hands them to the game.
#pragma once

#include <cstdint>

#include <windows.h>

namespace ls3d::platform {

// ---------------------------------------------------------------- time

// A millisecond clock (wraps after 49.7 days; timeGetTime's).
uint32_t Milliseconds();
// The system's coarser millisecond count (GetTickCount's; the voice manager reads it once).
uint32_t TickCount();
// Waits `ms` milliseconds; 0 gives up the rest of the time slice.
void Pause(uint32_t ms);

// ---------------------------------------------------------------- threads and locks

struct Thread;                  // opaque
struct Mutex;                   // opaque

using ThreadProc = DWORD(WINAPI *)(void *param);

Thread *StartThread(ThreadProc proc, void *param);
// THREAD_PRIORITY_* (only THREAD_PRIORITY_NORMAL is used).
void SetPriority(Thread *thread, int priority);
void Suspend(Thread *thread);
void Resume(Thread *thread);
void Terminate(Thread *thread, uint32_t exitCode);
// Waits until the thread has ended.
void Join(Thread *thread);
// Forgets the thread (after Join or Terminate).
void Release(Thread *thread);

Mutex *NewMutex();
void DeleteMutex(Mutex *mutex);
void Lock(Mutex *mutex);
void Unlock(Mutex *mutex);

struct Event;                   // opaque: an auto-reset event, initially not set

Event *NewEvent();
void DeleteEvent(Event *event);
void SetEvent(Event *event);
// Waits until the event is set (true), or wakes up early for the thread's window messages (false).
bool WaitEvent(Event *event);
// The native handle, for an API that sets the event itself (DirectSound's position notifications).
HANDLE NativeEvent(Event *event);

// ---------------------------------------------------------------- the window

// What the engine decides about its window. The backend's window procedure and message loop ask
// IGraph's implementation.
class WindowEvents {
public:
    // The window was asked to close (WM_CLOSE): false while the engine is not initialised (the
    // request is then ignored); otherwise the engine closes and the process exits with `code`
    // (does not return).
    virtual bool OnClose(int code) = 0;
    // The window is being destroyed (WM_DESTROY): false while the engine is not initialised;
    // otherwise the backend ends the message loop.
    virtual bool OnDestroy() = 0;
    // The window was activated or deactivated (WM_ACTIVATE): `minimized` when it was minimised.
    virtual void OnActivate(bool active, bool minimized) = 0;
    // True while the message loop should wait for messages instead of returning (the game is
    // in the background).
    virtual bool WaitWhileIdle() = 0;
    // A typed character (WM_CHAR).
    virtual void OnChar(int ch) = 0;
    // True when the keyboard reads window messages (IGKEY_WINDOW_MESSAGES) ...
    virtual bool KeyboardFromMessages() = 0;
    // ... and a key from them: its DIK_* code (0: none) and character.
    virtual void OnKeyMessage(int key, int ch) = 0;
};

// The engine's decisions (IGraph's, set when it is constructed: the game may pump messages before
// it initialises IGraph).
void SetWindowEvents(WindowEvents *events);
// Registers the engine's window class. False on failure.
bool RegisterWindowClass(HINSTANCE instance);
// The main window. Windowed: a captioned window whose client area of width x height is at (x, y)
// (with `menu`, which is drawn when `withMenu`). Fullscreen: a topmost popup of that size at
// (x, y). Null on failure.
HWND CreateMainWindow(HINSTANCE instance, bool fullscreen, int x, int y, int width, int height, bool withMenu,
                      HMENU menu);
// A child window of `parent` (IG_CHILD_WINDOW: the editors' view).
HWND CreateChildWindow(HINSTANCE instance, HWND parent, int x, int y, int width, int height);
// The engine's own window menu (IG_MENU without one of the game's).
HMENU LoadDefaultMenu();
// Paints the window now (what needs painting).
void Refresh(HWND window);
void Focus(HWND window);
bool SetTitle(HWND window, const char *title);
// True when the foreground window is `window` or one of its children (the focus stays in the
// engine's windows).
bool ForegroundIsOwn(HWND window);
// The screen position of the top left of `window`'s client area. `framed`: a main window, whose
// frame and caption (and `withMenu`, its menu) come before the client area; otherwise the
// window's own top left.
POINT ClientOrigin(HWND window, bool framed, bool withMenu);
// Modeless dialogs (the editors'): their messages go to them first.
bool AddDialog(HWND dialog);
bool RemoveDialog(HWND dialog);
// Handles the waiting messages; while events->WaitWhileIdle() it waits for more instead of
// returning.
void PumpMessages();

// ---------------------------------------------------------------- the mouse and keyboard

// Moves the cursor to a screen position (false on failure).
bool SetCursorPosition(int x, int y);
// Lets the cursor leave any rectangle it was confined to.
void ReleaseCursorClip();
// The user swapped the left and right mouse buttons.
bool MouseButtonsSwapped();
// Sticky, filter and toggle keys (their hotkeys interrupt a game) off while the engine runs, and
// back as they were.
void DisableAccessibilityKeys();
void RestoreAccessibilityKeys();

}  // namespace ls3d::platform
