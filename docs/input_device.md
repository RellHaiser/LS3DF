# The input device (input)

IGraph reads the keyboard and the mouse through one interface, `input::Device`
(`input::g_pInput`), declared in [`src/input/input.h`](../src/input/input.h). Its only
implementation so far is the DirectInput backend in [`src/input/dinput`](../src/input/dinput),
which passes every call to DirectInput 8 unchanged. This is the third device seam, after the
[render device](render_device.md) and the [sound device](sound_device.md), and is built the same
way. SDL3 is the planned successor.

## Design

**The vocabulary stays DirectInput 8's.** The keyboard is 256 bytes of `DIK_*` keys, and the mouse
is a `DIMOUSESTATE2` (relative axes, wheel, five buttons), read with cooperative levels and data
formats. The `DIK_*` numbering matters beyond the engine: IGraph's key interface (`GetKey`,
`IsKeyDown`, `SetKeyMap`, ...) speaks it, and the game stores those codes in its key bindings. A
new backend translates its own key codes (SDL scancodes, for instance) into `DIK_*`. The types come
from [`src/dx/dinput_types.h`](../src/dx/dinput_types.h), which on Windows takes them from the
SDK's `dinput.h`.

**Objects are opaque handles.** The DirectInput object is an `input::System` and a keyboard or
mouse is an `input::Peripheral`; only a backend looks inside them. They live in IGraph's
`igraph_input.cpp`, not in structures the game reads. The DirectInput object (`ig::g_dinput`,
0x100d1d0c in the original) is shared IGraph state so the test can put a fake in. The DirectInput
backend's handles are the DirectInput interfaces.

**What stays in IGraph:** everything the original decides. That covers the keyboard's previous
state and new presses, key repeat (350 ms, then every 34 ms), the modifier queries, the key map,
the character table, the debug overlay keys (Ctrl+Alt+Shift+F10 to F12), the mouse's absolute
position in exclusive mode, swapped buttons and the timer. The window-message keyboard
(`IGKEY_WINDOW_MESSAGES`, from `WM_KEYDOWN` / `WM_CHAR`) belongs to the window: the
[platform](platform.md) turns the messages into keys, and `compare_window` tests it.

**The boundary is checked.** `python build.py` (and `check`) fails when code outside `src/dx` and
`src/input/dinput` includes `dinput.h` or the backend, or names a DirectInput interface or
`DirectInput8Create`.

## Tests

`compare_input` is new; nothing tested input before. The original DLL's IGraph and the port's run
the same random operations, each side on its own fake DirectInput objects with the same scripted
key and mouse states, creation failures and lost input. Both sides' `timeGetTime` read one fake
clock. Every DirectInput call with its arguments must be the same, along with every return value,
the read states and the debug overlay switches. The operations are:

- the keyboard and mouse in every mode, opened and closed;
- reads, including the "input lost" recovery;
- `GetKey` with and without repeat, `GetKeyChar` and the modifier queries;
- the key map, `KeyToChar` and key names;
- mouse movement, position and buttons (swapped or not);
- the timer.

Result: 100,000 operations identical on each of four seeds. Each run returns about 9,000 keys,
switches a debug overlay about 800 times and recovers from about 2,500 lost reads. Two deliberate
bugs are caught: wrong cooperative-level flags in the backend (at operation 21) and two mouse
buttons swapped in IGraph (at operation 284).

The test found one real difference in the port, now fixed. The held key of `IGKEY_REPEAT` starts
as key 0 in the original (a zero-initialised variable), so its first `GetKey` in repeat mode
returns 0; the port started at -1.

Not covered: `DirectInput8Create` itself, and `SetMousePos` in absolute mode (it moves the real
cursor). The window-message keyboard is `compare_window`'s ([platform](platform.md)).
