// IGraph: DirectInput keyboard and mouse, typed characters, timing (0x1006f040 - 0x1006fbd0).
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstring>

#include "core/debug.h"
#include "core/i3d_result.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"

using namespace ls3d;
using namespace ls3d::ig;

namespace {

IDirectInput8A *g_dinput = nullptr;             // 0x100d1d0c
IDirectInputDevice8A *g_keyboard = nullptr;     // 0x100d19c8
IDirectInputDevice8A *g_mouse = nullptr;        // 0x100d2148
bool g_keyboardInit = false;                    // 0x100d2196
bool g_mouseInit = false;                       // 0x100d2197
uint32_t g_keyboardFlags = 0;                   // 0x100d173c IGKEY_*
int g_mouseMode = -1;                           // 0x100d1a7c

uint8_t g_keys[256];                            // 0x100d1788 DirectInput keyboard state
uint8_t g_prevKeys[256];                        // 0x100d1be8 the state of the previous ReadKeyboard
uint8_t g_keyMap[64];                           // 0x100d1d68 SetKeyMap

// Typed characters: [0] the last one ReadChar returned, then the queue.
int g_chars[0x20];                              // 0x100d1e48

// IGKEY_WINDOW_MESSAGES: the key and character of the last WM_KEYDOWN / WM_CHAR.
int g_messageKey = 0;                           // 0x100d1a8c
int g_messageChar = 0;                          // 0x100d212c
int g_lastKeyChar = 0;                          // 0x100d1a80 GetKeyChar

// IGKEY_REPEAT: the held key, when it was pressed and when it last repeated.
int g_repeatKey = -1;                           // 0x100d1d60
DWORD g_repeatStart = 0;                        // 0x100d21cc
DWORD g_repeatLast = 0;                         // 0x100d21d0

DIMOUSESTATE2 g_mouseState;                     // 0x100d1ec8
DIDEVCAPS g_mouseCaps;                          // 0x100d1db0
int g_mouseX = 0;                               // 0x100d2178
int g_mouseY = 0;                               // 0x100d217c
bool g_swapButtons = false;                     // 0x100d1890 (the system's swapped mouse buttons)

DWORD g_lastTime = 0;                           // 0x100d1738 ReadTimer
DIPROPSTRING g_keyName;                         // 0x100d1f08 GetKeyName's buffer

// Key, character, shifted character (0x100a7868).
struct S_keyChar {
    uint8_t key, normal, shifted;
};
const S_keyChar kKeyChars[] = {
    {DIK_SPACE, ' ', ' '}, {DIK_0, '0', ')'}, {DIK_1, '1', '!'}, {DIK_2, '2', '@'}, {DIK_3, '3', '#'},
    {DIK_4, '4', '$'}, {DIK_5, '5', '%'}, {DIK_6, '6', '^'}, {DIK_7, '7', '&'}, {DIK_8, '8', '*'},
    {DIK_9, '9', '('}, {DIK_A, 'a', 'A'}, {DIK_B, 'b', 'B'}, {DIK_C, 'c', 'C'}, {DIK_D, 'd', 'D'},
    {DIK_E, 'e', 'E'}, {DIK_F, 'f', 'F'}, {DIK_G, 'g', 'G'}, {DIK_H, 'h', 'H'}, {DIK_I, 'i', 'I'},
    {DIK_J, 'j', 'J'}, {DIK_K, 'k', 'K'}, {DIK_L, 'l', 'L'}, {DIK_M, 'm', 'M'}, {DIK_N, 'n', 'N'},
    {DIK_O, 'o', 'O'}, {DIK_P, 'p', 'P'}, {DIK_Q, 'q', 'Q'}, {DIK_R, 'r', 'R'}, {DIK_S, 's', 'S'},
    {DIK_T, 't', 'T'}, {DIK_U, 'u', 'U'}, {DIK_V, 'v', 'V'}, {DIK_W, 'w', 'W'}, {DIK_X, 'x', 'X'},
    {DIK_Y, 'y', 'Y'}, {DIK_Z, 'z', 'Z'}, {DIK_NUMPAD0, '0', '0'}, {DIK_NUMPAD1, '1', '1'},
    {DIK_NUMPAD2, '2', '2'}, {DIK_NUMPAD3, '3', '3'}, {DIK_NUMPAD4, '4', '4'}, {DIK_NUMPAD5, '5', '5'},
    {DIK_NUMPAD6, '6', '6'}, {DIK_NUMPAD7, '7', '7'}, {DIK_NUMPAD8, '8', '8'}, {DIK_NUMPAD9, '9', '9'},
    {DIK_MULTIPLY, '*', '*'}, {DIK_ADD, '+', '+'}, {DIK_SUBTRACT, '-', '-'}, {DIK_DECIMAL, '.', '.'},
    {DIK_DIVIDE, '/', '/'}, {DIK_SEMICOLON, ';', ':'}, {DIK_EQUALS, '=', '+'}, {DIK_COMMA, ',', '<'},
    {DIK_MINUS, '-', '_'}, {DIK_PERIOD, '.', '>'}, {DIK_SLASH, '/', '?'}, {DIK_GRAVE, '`', '~'},
    {DIK_LBRACKET, '[', '{'}, {DIK_BACKSLASH, '\\', '|'}, {DIK_RBRACKET, ']', '}'}, {DIK_APOSTROPHE, '\'', '"'},
};

bool Down(int key) { return (g_keys[key] & 0x80) != 0; }

}  // namespace

namespace ls3d::ig {

// The input part of the IGraph constructor (0x10069a00).
void ResetInputState()
{
    std::memset(g_keys, 0, sizeof g_keys);
    std::memset(g_prevKeys, 0, sizeof g_prevKeys);
    std::memset(g_keyMap, 0, sizeof g_keyMap);
    std::memset(&g_mouseState, 0, sizeof g_mouseState);
    g_dinput = nullptr;
    g_keyboard = nullptr;
    g_keyboardInit = false;
    g_mouse = nullptr;
    g_mouseInit = false;
    g_mouseX = 0;
    g_mouseY = 0;
    g_mouseMode = -1;
    g_swapButtons = false;
}

HRESULT InitDirectInput(HINSTANCE instance)
{
    return DirectInput8Create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void **>(&g_dinput),
                              nullptr);
}

void ReleaseDirectInput()
{
    if (g_dinput) {
        g_dinput->Release();
        g_dinput = nullptr;
    }
}

void SetSwapMouseButtons(bool swap)
{
    g_swapButtons = swap;
}

int MouseMode()
{
    return g_mouseMode;
}

bool KeyboardUsesMessages()
{
    return (g_keyboardFlags & IGKEY_WINDOW_MESSAGES) != 0;
}

// ProcessWinMessages: a WM_CHAR into the first free queue slot.
void QueueChar(int ch)
{
    for (int i = 1; i < 0x20; ++i) {
        if (!g_chars[i]) {
            g_chars[i] = ch;
            break;
        }
    }
}

// ProcessWinMessages with IGKEY_WINDOW_MESSAGES: arrows and delete from WM_KEYDOWN, the rest
// from WM_CHAR (DirectInput codes).
void KeyFromMessage(const MSG &msg)
{
    if (msg.message == WM_KEYDOWN) {
        switch (msg.wParam) {
        case VK_LEFT: g_messageKey = DIK_LEFT; g_messageChar = 0; break;
        case VK_UP: g_messageKey = DIK_UP; g_messageChar = 0; break;
        case VK_RIGHT: g_messageKey = DIK_RIGHT; g_messageChar = 0; break;
        case VK_DOWN: g_messageKey = DIK_DOWN; g_messageChar = 0; break;
        case VK_DELETE: g_messageKey = DIK_DELETE; g_messageChar = 0; break;
        }
    } else if (msg.message == WM_CHAR && static_cast<LONG>(msg.lParam) >= 0) {
        switch (msg.wParam) {
        case 8:
            g_messageKey = DIK_BACK;
            g_messageChar = 8;
            break;
        case 9:
            g_messageKey = DIK_TAB;
            g_messageChar = 9;
            break;
        case 0x1b:
            g_messageKey = DIK_ESCAPE;
            g_messageChar = 0x1b;
            break;
        default: {
            const SHORT vk = VkKeyScanA(static_cast<CHAR>(msg.wParam));
            if (vk == -1) {
                g_messageKey = 0;
                g_messageChar = 0;
            } else {
                g_messageKey = static_cast<int>(MapVirtualKeyA(vk & 0xff, 0));
                g_messageChar = static_cast<int>(msg.wParam);
            }
        }
        }
    }
}

}  // namespace ls3d::ig

// 0x1006f040
HRESULT __stdcall IGraph::InitKeyboard(uint32_t flags)
{
    CloseKeyboard();
    g_keyboardFlags = flags;
    HRESULT hr = g_dinput->CreateDevice(GUID_SysKeyboard, &g_keyboard, nullptr);
    if (hr < 0) {
        LogDxError(hr, 0x105a);
        return I3DERR_GENERIC;
    }
    hr = g_keyboard->SetDataFormat(&c_dfDIKeyboard);
    if (hr < 0) {
        LogDxError(hr, 0x105d);
        return I3DERR_GENERIC;
    }
    hr = g_keyboard->SetCooperativeLevel(g_mainWnd, (g_keyboardFlags & IGKEY_EXCLUSIVE)
                                                         ? DISCL_EXCLUSIVE | DISCL_FOREGROUND
                                                         : DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
    if (hr < 0) {
        LogDxError(hr, 0x1067);
        return I3DERR_GENERIC;
    }
    g_keyboard->Acquire();
    g_keyboardInit = true;
    ReadKeyboard();
    std::memcpy(g_prevKeys, g_keys, sizeof g_keys);
    g_messageKey = 0;
    g_messageChar = 0;
    g_lastKeyChar = 0;
    ClearChars();
    return 0;
}

// 0x1006f150
bool __stdcall IGraph::IsKeyboardInit()
{
    return g_keyboardInit;
}

// 0x1006f160
HRESULT __stdcall IGraph::CloseKeyboard()
{
    HRESULT result = 0;
    if (g_keyboard) {
        g_keyboard->Unacquire();
        result = static_cast<HRESULT>(g_keyboard->Release());
        g_keyboard = nullptr;
    }
    g_keyboardInit = false;
    return result;
}

// 0x1006f190
HRESULT __stdcall IGraph::ReadKeyboard()
{
    if (!g_keyboard)
        return I3DERR_NOTINITIALIZED;
    std::memcpy(g_prevKeys, g_keys, sizeof g_keys);
    std::memset(g_keys, 0, sizeof g_keys);
    if (g_keyboard->GetDeviceState(sizeof g_keys, g_keys) < 0) {
        while (g_keyboard->Acquire() == DIERR_INPUTLOST)
            ProcessWinMessages();
        if (g_keyboard->GetDeviceState(sizeof g_keys, g_keys) < 0)
            std::memset(g_keys, 0, sizeof g_keys);
        return 0;
    }
    // Ctrl+Alt+Shift+F12 / F11 / F10 toggle the debug overlays.
    if (!(g_init.flags & IG_NO_DEBUG_KEYS) && Down(DIK_LSHIFT) && Down(DIK_LMENU) && Down(DIK_LCONTROL)) {
        if (Down(DIK_F12) && !(g_prevKeys[DIK_F12] & 0x80))
            g_showFps = !g_showFps;
        if (Down(DIK_F11) && !(g_prevKeys[DIK_F11] & 0x80))
            g_showSoundStats = !g_showSoundStats;
        if (Down(DIK_F10) && !(g_prevKeys[DIK_F10] & 0x80))
            g_logSoundFailures = !g_logSoundFailures;
    }
    return 0;
}

// 0x1006f2d0
int __stdcall IGraph::ReadChar()
{
    std::memmove(&g_chars[0], &g_chars[1], sizeof g_chars - sizeof g_chars[0]);
    g_chars[0x1f] = 0;
    return g_chars[0];
}

// 0x1006f300
HRESULT __stdcall IGraph::ClearChars()
{
    std::memset(g_chars, 0, sizeof g_chars);
    return 0;
}

// 0x1006f320
int __stdcall IGraph::GetKey()
{
    if (g_keyboardFlags & IGKEY_WINDOW_MESSAGES) {
        const int key = g_messageKey;
        g_messageKey = 0;
        g_lastKeyChar = g_messageChar;
        g_messageChar = 0;
        return key;
    }
    for (int key = 0; key < 0x100; ++key) {
        const bool modifier = key == DIK_LCONTROL || key == DIK_RCONTROL || key == DIK_LSHIFT ||
                              key == DIK_RSHIFT || key == DIK_LMENU || key == DIK_RMENU;
        if (Down(key) && !g_prevKeys[key] && !modifier) {
            g_lastKeyChar = KeyToChar(key, GetShiftKeys(false) & (IGSHIFT_SHIFT | IGSHIFT_CTRL | IGSHIFT_ALT));
            if (g_keyboardFlags & IGKEY_REPEAT) {
                g_repeatKey = key;
                g_repeatStart = timeGetTime();
                g_repeatLast = 0;
            }
            return key;
        }
        // (Evaluated for every key that is not a new press, as in the original.)
        if ((g_keyboardFlags & IGKEY_REPEAT) && g_repeatKey != -1) {
            if (!g_keys[g_repeatKey]) {
                g_repeatKey = -1;
                return 0;
            }
            const DWORD now = timeGetTime();
            const bool due = g_repeatLast ? static_cast<int>(now - g_repeatLast) > 34
                                          : static_cast<int>(now - g_repeatStart) > 350;
            if (due) {
                g_repeatLast = now;
                return g_repeatKey;
            }
        }
    }
    return 0;
}

// 0x1006f470
int __stdcall IGraph::GetKeyChar()
{
    const int ch = g_lastKeyChar;
    g_lastKeyChar = 0;
    return ch;
}

// 0x1006f490
bool __stdcall IGraph::IsKeyDown(int key)
{
    return Down(key & 0xff);
}

// 0x1006f4b0
HRESULT __stdcall IGraph::SetKeyMap(const uint8_t *keys, uint32_t count)
{
    if (!keys || count > sizeof g_keyMap)
        return I3DERR_INVALIDPARAMS;
    std::memset(g_keyMap, 0, sizeof g_keyMap);
    std::memcpy(g_keyMap, keys, count);
    return 0;
}

// 0x1006f500
uint64_t __stdcall IGraph::GetMappedKeys()
{
    uint64_t bits = 0;
    for (int i = 0; i < 0x40; ++i) {
        if (g_keyMap[i] && Down(g_keyMap[i]))
            bits |= 1ull << i;
    }
    return bits;
}

// 0x1006f550
bool __stdcall IGraph::IsShiftDown()
{
    return Down(DIK_LSHIFT) || Down(DIK_RSHIFT);
}

// 0x1006f570
bool __stdcall IGraph::IsCtrlDown()
{
    return Down(DIK_LCONTROL) || Down(DIK_RCONTROL);
}

// 0x1006f590
bool __stdcall IGraph::IsAltDown()
{
    return Down(DIK_LMENU) || Down(DIK_RMENU);
}

// 0x1006f5b0
uint32_t __stdcall IGraph::GetShiftKeys(bool read)
{
    if (read)
        ReadKeyboard();
    uint32_t bits = 0;
    if (IsShiftDown()) {
        bits = Down(DIK_LSHIFT) ? IGSHIFT_SHIFT | IGSHIFT_LSHIFT : IGSHIFT_SHIFT;
        if (Down(DIK_RSHIFT))
            bits |= IGSHIFT_RSHIFT;
    }
    if (IsCtrlDown()) {
        bits |= Down(DIK_LCONTROL) ? IGSHIFT_CTRL | IGSHIFT_LCTRL : IGSHIFT_CTRL;
        if (Down(DIK_RCONTROL))
            bits |= IGSHIFT_RCTRL;
    }
    if (IsAltDown()) {
        bits |= Down(DIK_LMENU) ? IGSHIFT_ALT | IGSHIFT_LALT : IGSHIFT_ALT;
        if (Down(DIK_RMENU))
            bits |= IGSHIFT_RALT;
    }
    return bits;
}

// 0x1006f660
uint8_t __stdcall IGraph::KeyToChar(int key, uint32_t shiftKeys)
{
    for (const S_keyChar &k : kKeyChars) {
        if (k.key == static_cast<uint8_t>(key))
            return (shiftKeys & IGSHIFT_SHIFT) ? k.shifted : k.normal;
    }
    return 0;
}

// 0x1006f6a0
const wchar_t *__stdcall IGraph::GetKeyName(int key)
{
    if (!g_keyboard)
        return nullptr;
    std::memset(&g_keyName, 0, sizeof g_keyName);
    g_keyName.diph.dwSize = sizeof g_keyName;
    g_keyName.diph.dwHeaderSize = sizeof g_keyName.diph;
    g_keyName.diph.dwObj = static_cast<DWORD>(key);
    g_keyName.diph.dwHow = DIPH_BYOFFSET;
    return g_keyboard->GetProperty(DIPROP_KEYNAME, &g_keyName.diph) ? nullptr : g_keyName.wsz;
}

// 0x1006f710
int __stdcall IGraph::InitMouse(int mode)
{
    if (mode == -1)
        return g_mouseMode;
    // Switching from the cursor to exclusive mode: wait until every button is up.
    if (g_mouse && g_mouseMode == 0 && mode == 1) {
        do {
            ProcessWinMessages();
            ReadMouse();
        } while ((g_mouseState.rgbButtons[0] | g_mouseState.rgbButtons[1] | g_mouseState.rgbButtons[2] |
                  g_mouseState.rgbButtons[3] | g_mouseState.rgbButtons[4]) &
                 0x80);
    }
    CloseMouse();
    HRESULT hr = g_dinput->CreateDevice(GUID_SysMouse, &g_mouse, nullptr);
    if (hr < 0) {
        LogDxError(hr, 0x1399);
        return I3DERR_GENERIC;
    }
    g_mouseCaps.dwSize = sizeof g_mouseCaps;
    g_mouse->GetCapabilities(&g_mouseCaps);
    hr = g_mouse->SetDataFormat(&c_dfDIMouse2);
    if (hr < 0) {
        LogDxError(hr, 0x13a2);
        return I3DERR_GENERIC;
    }
    hr = g_mouse->SetCooperativeLevel(g_mainWnd, mode ? DISCL_EXCLUSIVE | DISCL_FOREGROUND
                                                      : DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
    if (hr < 0) {
        LogDxError(hr, 0x13ac);
        return I3DERR_GENERIC;
    }
    g_mouse->Acquire();
    g_mouseMode = mode;
    g_mouseInit = true;
    return 0;
}

// 0x1006f840
bool __stdcall IGraph::IsMouseInit()
{
    return g_mouseInit;
}

// 0x1006f850
HRESULT __stdcall IGraph::CloseMouse()
{
    if (g_mouse) {
        g_mouse->Unacquire();
        g_mouse->Release();
        g_mouse = nullptr;
    }
    std::memset(&g_mouseState, 0, sizeof g_mouseState);
    g_mouseInit = false;
    return 0;
}

// 0x1006f8a0
HRESULT __stdcall IGraph::ReadMouse()
{
    if (!g_mouse)
        return I3DERR_NOTINITIALIZED;
    if (g_mouse->GetDeviceState(sizeof g_mouseState, &g_mouseState) < 0) {
        g_mouse->Acquire();
        if (g_mouse->GetDeviceState(sizeof g_mouseState, &g_mouseState) < 0) {
            std::memset(&g_mouseState, 0, sizeof g_mouseState);
            return I3DERR_GENERIC;
        }
    }
    if (g_mouseCaps.dwAxes < 3)
        g_mouseState.lZ = 0;
    if (g_mouseMode == 0) {
        // The original computes the cursor position relative to the window here (GetCursorPos,
        // GetWindowRect, frame metrics) and then discards it.
    } else {
        g_mouseX += g_mouseState.lX;
        g_mouseY += g_mouseState.lY;
    }
    if (g_swapButtons) {
        const BYTE b = g_mouseState.rgbButtons[0];
        g_mouseState.rgbButtons[0] = g_mouseState.rgbButtons[1];
        g_mouseState.rgbButtons[1] = b;
    }
    return 0;
}

// 0x1006fa10
int __stdcall IGraph::GetMouseDX()
{
    return g_mouseState.lX;
}

// 0x1006fa20
int __stdcall IGraph::GetMouseDY()
{
    return g_mouseState.lY;
}

// 0x1006fa30
int __stdcall IGraph::GetMouseDZ()
{
    return g_mouseState.lZ;
}

// 0x1006fa40
int __stdcall IGraph::SetMousePos(int x, int y)
{
    g_mouseX = x;
    g_mouseY = y;
    if (g_mouseMode != 0)
        return g_mouseMode;
    if (!(g_init.flags & IG_FULLSCREEN)) {
        RECT rc;
        if (g_init.flags & IG_CHILD_WINDOW) {
            GetWindowRect(g_childWnd, &rc);
        } else {
            GetWindowRect(g_mainWnd, &rc);
            const int frameX = GetSystemMetrics(SM_CXFIXEDFRAME);
            const int frameY = GetSystemMetrics(SM_CYFIXEDFRAME);
            const int caption = GetSystemMetrics(SM_CYCAPTION);
            const int menu = (g_init.flags & IG_MENU) ? GetSystemMetrics(SM_CYMENU) : 0;
            rc.left += frameX;
            rc.top += menu + caption + frameY;
        }
        x += rc.left;
        y += rc.top;
    }
    return SetCursorPos(x, y);
}

// 0x1006fb10
int __stdcall IGraph::GetMouseX()
{
    return g_mouseX;
}

// 0x1006fb20
int __stdcall IGraph::GetMouseY()
{
    return g_mouseY;
}

// 0x1006fb30
uint32_t __stdcall IGraph::GetMouseButtons()
{
    uint32_t bits = g_mouseState.rgbButtons[0] >> 7;
    if (g_mouseState.rgbButtons[2] & 0x80)
        bits |= 4;
    if (g_mouseState.rgbButtons[1] & 0x80)
        bits |= 2;
    if (g_mouseState.rgbButtons[3] & 0x80)
        bits |= 8;
    if (g_mouseState.rgbButtons[4] & 0x80)
        bits |= 0x10;
    return bits;
}

// 0x1006fb70
DWORD __stdcall IGraph::ReadTimer()
{
    g_lastTime = timeGetTime();
    return g_lastTime;
}

// 0x1006fb80
DWORD __stdcall IGraph::GetTimer(DWORD minMs, DWORD maxMs)
{
    const DWORD before = g_lastTime;
    ReadTimer();
    const DWORD elapsed = g_lastTime - before;
    if (elapsed < minMs) {
        Sleep(minMs - elapsed);
        g_lastTime += minMs - elapsed;
        return minMs;
    }
    if (maxMs && elapsed > maxMs)
        return maxMs;
    return elapsed;
}
