# The platform (platform)

The operating system services the engine uses besides drawing, sound and input are in
[`src/platform/platform.h`](../src/platform/platform.h). They are the window and its messages,
the mouse cursor, time, threads and locks. The Win32 implementation in
[`src/platform/win32`](../src/platform/win32) is IGraph's own Windows code, moved there unchanged.
SDL3 is the planned successor (window, events, clock, threads).

## Design

**Chosen when the DLL is built.** The devices ([render](render_device.md), [sound](sound_device.md),
[input](input_device.md)) are interfaces with a backend object, so a second backend can sit beside
the first in one process. The platform is a set of functions instead, and one backend source is
compiled in. Nothing needs two platforms in one process, and the engine reads the clock from
static initialisers (IGraph's FPS counter), before any backend object could be constructed.

**IGraph keeps its decisions.** The backend does the operating system's part: it registers the
window class, creates the main and child windows with their frames, runs the window procedure and
the message loop, passes dialog messages to the editors' dialogs, and handles the cursor and the
accessibility keys. Whenever the original decided something, the backend asks IGraph through
`platform::WindowEvents`:

| Event | IGraph's decision |
|---|---|
| `OnClose` (WM_CLOSE) | ignored before Init; otherwise IGraph closes and the process exits |
| `OnDestroy` (WM_DESTROY) | ignored before Init; otherwise the message loop ends |
| `OnActivate` (WM_ACTIVATE) | deactivation: the "..zzZZ" title, suspending the texture thread; reactivation: the title back, device states reapplied, video volumes restored, the thread resumed |
| `WaitWhileIdle` | the message loop waits while the game is in the background (IG_WAIT_INACTIVE) |
| `OnChar` | typed characters for ReadChar |
| `KeyboardFromMessages`, `OnKeyMessage` | the window-message keyboard (IGKEY_WINDOW_MESSAGES) |

The backend's own policy stays with it. It swallows background erases, the F10 and Alt keys that
would open the window menu, and the screen saver and monitor power-off.

**Handles.** Windows, menus and instances are the native handles (`HWND`, `HMENU`, `HINSTANCE`),
because IGraph's interface hands them to the game. Threads, locks and events are opaque
(`platform::Thread`, `platform::Mutex`, `platform::Event`); with Win32 they are thread handles,
critical sections and auto-reset events. IShow's Ogg stream thread waits on an event that
DirectSound sets, so `NativeEvent` gives the sound device the handle.

**Time.** `Milliseconds()` is the engine's clock (`timeGetTime`), and every module reads it through
the platform now. `TickCount()` is `GetTickCount`, which the voice manager reads in one place, as
the original does.

**The boundary is checked.** `python build.py` (and `check`) fails when code outside
`src/platform/win32` calls one of the Windows functions the platform covers: the window, message,
cursor, thread, lock, event and time functions, `Sleep`, and `<mmsystem.h>`. Only the DirectShow
base classes keep theirs, as part of the Windows video backend ([the video device](video_device.md)).

## Not covered yet

Some Windows calls are not part of this seam, because they are small and only matter for another
platform:

- the DirectX 8.1 check (`dpnhpast.dll`) and the OS name in the log;
- COM start-up (DirectShow and EAX need it);
- `RECT` helpers;
- the texture directories' enumeration (`FindFirstFile`);
- the engine's resource lookups, `MessageBox` for a bad engine version, the INI's path from the
  module name, and the command line.

They will move as the other platforms need them.

## Tests

`compare_window` is new. The original DLL's window procedure (0x1006b820) and the port's (the
platform's "LS3D" window class) get the same messages for a hidden test window, which both sides
take as their main window. The same posted messages go through each side's `ProcessWinMessages`.
Everything must match:

- return values, the activation state and the application name;
- the titles each side gives the window and the quit messages;
- the typed characters (`ReadChar`) and the window-message keyboard (`GetKey`, `GetKeyChar`);
- the dialog list.

The test's windows live on a desktop of their own. The activation gives them the keyboard focus,
and on the user's desktop real typing would reach the queue; an early run caught exactly that.

Result: 100,000 operations identical on each of four seeds. Each run covers about 5,200
deactivations and reactivations, 19,000 messages swallowed, 1,400 quit messages, 33,000 typed
characters and 10,000 window-message keys. Four deliberate bugs are caught: two in the backend's
system-key filter and key mapping, one in its typed-character condition, and one in IGraph's
activation decision.

The rest of the suite runs through the platform's clock. `compare_isnd` and `compare_input` patch
`timeGetTime`, and the backend compiled into them calls it through their own import table. It
gives the same results as before.

Not covered: creating the windows (that needs IGraph::Init, and with it a Direct3D device), WM_CLOSE
while initialised (the process exits), the message loop waiting while inactive, the cursor, the
accessibility keys and the texture thread.
