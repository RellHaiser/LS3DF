// IGraph: the engine's window, Direct3D 8 device, DirectInput and texture front end
// (IGraph.cpp in the original source tree, 0x100699a0 - 0x10070eb0, vtable 0x1009d840).
//
// One object exists (GetIGraph()); it holds nothing but its vtable, all state is file-level. Every
// method is __stdcall with `this` on the stack (the slot order is the DLL's ABI), except the
// printf-style SetAppNameF, which MSVC makes __cdecl.
#pragma once

#include <cstdint>

#include <windows.h>

#include "core/debug.h"
#include "dx/d3d8.h"

struct S_matrix;
class ITexture;
class IShow;

// IGraph::Init parameters (0x38 bytes, copied to 0x100d1d18).
struct S_IGraphInit {
    HINSTANCE instance;                 // +0x00
    int adapter;                        // +0x04 Direct3D adapter ordinal
    int x, y;                           // +0x08 window position (windowed)
    int width, height;                  // +0x10 client size / fullscreen resolution
    int childX, childY;                 // +0x18 child window rectangle (IG_CHILD_WINDOW)
    int childWidth, childHeight;        // +0x20
    uint8_t bitDepth;                   // +0x28 fullscreen colour depth: 16 or 32
    uint8_t refreshRate;                // +0x29 fullscreen refresh rate (0: default)
    uint16_t reserved2A;
    uint32_t flags;                     // +0x2C IG_* below
    HMENU menu;                         // +0x30 window menu (IG_MENU; 0: resource 123 of LS3D.DLL)
    ls3d::ErrorCallback *errorCallback; // +0x34
};
static_assert(sizeof(S_IGraphInit) == 0x38, "S_IGraphInit layout");

// S_IGraphInit::flags.
enum : uint32_t {
    IG_FULLSCREEN = 0x00000002,
    IG_TRIPLE_BUFFER = 0x00000004,      // two back buffers (fullscreen)
    IG_MENU = 0x00000008,               // window with a menu (windowed)
    IG_HW_TNL = 0x00000010,             // use hardware transform and lighting when present
    IG_WAIT_INACTIVE = 0x00000020,      // sleep in ProcessWinMessages while the window is inactive
    IG_VERBOSE = 0x00000040,            // log display modes and FSAA attempts
    IG_CHILD_WINDOW = 0x00000080,       // render into a child window of the main one
    IG_VSYNC = 0x00000100,
    IG_LOCKABLE_ZBUFFER = 0x00000400,   // D3DFMT_D16_LOCKABLE (needed by SaveShot's depth)
    IG_REFERENCE = 0x00000800,          // reference rasterizer
    IG_FSAA2 = 0x00001000,
    IG_FSAA4 = 0x00002000,
    IG_SMALL_LIGHTMAPS = 0x00004000,    // (LS3DF) lightmap atlas textures at most 256 a side (else 512)
    IG_NO_DEBUG_KEYS = 0x00010000,      // no Ctrl+Alt+Shift+F10..F12 overlays
    IG_TEXTURE_THREAD = 0x00100000,     // load textures in a background thread
};

// IGraph::GetCaps() bits (0x100d1dac).
enum : uint32_t {
    IGCAPS_HW_TNL = 0x001,
    IGCAPS_TABLE_FOG = 0x004,
    IGCAPS_DXT = 0x008,                 // at least one DXTn texture format
    IGCAPS_STENCIL = 0x010,             // the depth buffer has stencil bits
    IGCAPS_GAMMA = 0x020,               // fullscreen gamma ramp
    IGCAPS_GAMMA_8BIT = 0x040,          // the driver's ramp holds 8-bit values (GetGammaRamp widens)
    IGCAPS_HW_VERTEX_SHADERS = 0x080,
    IGCAPS_BUMPMAP = 0x100,             // D3DFMT_V8U8 textures
};

// SetState / GetState indices and their values.
enum {
    IGS_ALPHA = 0,                      // 0 opaque, 1 blend + alpha test >= 1, 2 blend + test >= 0, 3 test only
    IGS_BLEND = 1,                      // 0 alpha, 1 additive, 2 one+one, 3 modulate, 4 modulate 2x
    IGS_CULL = 3,                       // D3DCULL; 3 = the default
    IGS_COLOROP0 = 4,                   // D3DTOP of texture stages 0..2
    IGS_COLOROP1 = 5,
    IGS_COLOROP2 = 6,
    IGS_ALPHAOP0 = 7,                   // D3DTOP of the alpha of stages 0, 1
    IGS_ALPHAOP1 = 8,
    IGS_ZBIAS = 9,
    IGS_ADDRESSU0 = 10,                 // D3DTADDRESS of stage 0 (11 sets the same state)
    IGS_ADDRESSV0 = 11,
    IGS_ALPHAREF = 12,
    IGS_ZFUNC = 13,                     // 0 always, else less-equal
    IGS_ZWRITE = 14,
    IGS_ZENABLE = 15,
    IGS_CLAMP0 = 16,                    // stage 0 / 1 texture coordinates clamped (1) or wrapped
    IGS_CLAMP1 = 17,
    IGS_FOG = 18,
    IGS_SPECULAR = 19,
    IGS_COLORWRITE = 20,
};

// DrawPrimitive kinds and vertex formats.
enum {
    IGP_POINTS = 0,                     // (6 too)
    IGP_LINES = 1,
    IGP_LINESTRIP = 2,
    IGP_TRIANGLES = 3,
    IGP_TRIANGLEFAN = 4,
    IGP_TRIANGLESTRIP = 5,
};
enum {
    IGV_COLOR_TEX = 0,                  // position, colour, uv (0x18 bytes, vertex shader 0x100d21ac)
    IGV_TRANSFORMED = 1,                // D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1 (0x20 bytes)
    IGV_TRANSFORMED_TEX2 = 2,           // D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX2 (0x28 bytes)
};

struct S_IGraphViewport {               // = D3DVIEWPORT8
    int x, y, width, height;
    float minZ, maxZ;
};

class IGraph {
public:
    // 0x00 0x1006c2d0: creates the window, the Direct3D device and DirectInput.
    virtual HRESULT __stdcall Init(const S_IGraphInit *init);
    // 0x04 0x1006da30
    virtual const S_IGraphInit *__stdcall GetInitData();
    // 0x08 0x1006c2c0: replaces S_IGraphInit::flags.
    virtual HRESULT __stdcall SetFlags(uint32_t flags);
    // 0x0C 0x1006da20
    virtual bool __stdcall IsInitialized();
    // 0x10 0x1006da40: releases everything Init created (textures still referenced are forced out).
    virtual HRESULT __stdcall Close();
    // 0x14 0x1006a470: dispatches the queued window messages (waits while inactive, IG_WAIT_INACTIVE).
    virtual HRESULT __stdcall ProcessWinMessages();
    // 0x18 / 0x1C 0x1006de50 / 0x1006de70: render target size.
    virtual int __stdcall ScrnX();
    virtual int __stdcall ScrnY();
    // 0x20 0x1006de90: 16 or 32.
    virtual int __stdcall GetBitDepth();
    // 0x24 0x1006deb0: IGCAPS_* bits.
    virtual uint32_t __stdcall GetCaps();
    // 0x28 0x1006dea0
    virtual D3DFORMAT __stdcall GetBackBufferFormat();
    // 0x2C 0x1006bb80: "D3DFMT_..." text of a format.
    virtual const char *__stdcall GetFormatName(D3DFORMAT format);
    // 0x30 0x1006dec0: size and depth of a .bmp / .tga file; `handle` (if given) keeps the file open
    // at its start.
    virtual HRESULT __stdcall GetImageInfo(const char *fileName, uint16_t *width, uint16_t *height,
                                           uint8_t *bitDepth, uint32_t flags, int *handle);
    // 0x34 0x1006e210: the back buffer to a .tga file, or to memory (IGSHOT_MEMORY); with
    // IGSHOT_DEPTH the depth buffer too (an .rla file next to the .tga).
    virtual bool __stdcall SaveShot(void *fileNameOrBuffer, const RECT *rect, uint32_t flags, void *depthBuffer);
    // 0x38 0x10069e80: a DirectShow video player.
    virtual IShow *__stdcall CreateIShow();
    // 0x3C 0x10069f70: loads a texture (shared with earlier loads of the same files and flags).
    virtual HRESULT __stdcall LoadTexture(const char *fileName, const char *alphaFileName, uint32_t flags,
                                          ITexture **out);
    // 0x40 0x10069ee0: an empty texture object.
    virtual ITexture *__stdcall CreateITexture();
    // 0x44 0x1006a1f0
    virtual bool __stdcall SetTexture(ITexture *texture, int stage);
    // 0x48 / 0x4C 0x1006a240 / 0x1006a250
    virtual HWND __stdcall GetMainHWND();
    virtual HWND __stdcall GetChildHWND();
    // 0x50 0x1006a260: the window title (0: none).
    virtual bool __stdcall SetAppName(const char *name);
    // 0x54 0x1006a2f0
    virtual bool __cdecl SetAppNameF(const char *format, ...);
    // 0x58 0x1006a330
    virtual const char *__stdcall GetAppName();
    // 0x5C / 0x60 0x1006a340 / 0x1006a3d0: modeless dialogs that get their messages
    // (IsDialogMessage) in ProcessWinMessages.
    virtual bool __stdcall AddDlg(HWND dialog);
    virtual bool __stdcall DelDlg(HWND dialog);
    // 0x64 0x1006a6c0: flags 1 colour, 2 depth (and stencil when present).
    virtual HRESULT __stdcall Clear(D3DCOLOR color, float z, uint32_t flags);
    // 0x68 / 0x6C 0x1006b380 / 0x1006b3b0
    virtual HRESULT __stdcall BeginScene();
    virtual HRESULT __stdcall EndScene();
    // 0x70 0x1006b3e0: presents the frame; restores a lost device.
    virtual HRESULT __stdcall Flip();
    // 0x74 / 0x78 0x1006abc0 / 0x1006b220: cached render states (IGS_*).
    virtual HRESULT __stdcall SetState(int state, int value);
    virtual int __stdcall GetState(int state);
    // 0x7C / 0x80 0x1006c270 / 0x1006c2b0
    virtual HRESULT __stdcall SetViewport(const S_IGraphViewport *viewport);
    virtual const S_IGraphViewport *__stdcall GetViewport();
    // 0x84 0x1006b360
    virtual HRESULT __stdcall SetWorldMatrix(const S_matrix *matrix);
    // 0x88 / 0x8C 0x1006b5a0 / 0x1006b6e0: user-memory primitives (IGP_*, IGV_*).
    virtual HRESULT __stdcall DrawPrimitive(int kind, int count, const void *vertices, int vertexFormat);
    virtual HRESULT __stdcall DrawIndexedPrimitive(int kind, int numVertices, const uint16_t *indices, int count,
                                                   const void *vertices, int vertexFormat);
    // 0x90 0x1006f040: keyboard; flags IGKEY_* below.
    virtual HRESULT __stdcall InitKeyboard(uint32_t flags);
    // 0x94 0x1006f150
    virtual bool __stdcall IsKeyboardInit();
    // 0x98 0x1006f160
    virtual HRESULT __stdcall CloseKeyboard();
    // 0x9C 0x1006f190: polls the keyboard (once per frame).
    virtual HRESULT __stdcall ReadKeyboard();
    // 0xA0 0x1006f320: the key pressed in this frame (DIK_ code; repeats with IGKEY_REPEAT).
    virtual int __stdcall GetKey();
    // 0xA4 0x1006f470: the character of the last GetKey(), once.
    virtual int __stdcall GetKeyChar();
    // 0xA8 0x1006f2d0: the next typed character (WM_CHAR), 0 if none.
    virtual int __stdcall ReadChar();
    // 0xAC 0x1006f300
    virtual HRESULT __stdcall ClearChars();
    // 0xB0 0x1006f490
    virtual bool __stdcall IsKeyDown(int key);
    // 0xB4 / 0xB8 0x1006f4b0 / 0x1006f500: up to 64 keys whose state GetMappedKeys() returns as bits
    // (a 64-bit mask in EDX:EAX: the game reads both halves).
    virtual HRESULT __stdcall SetKeyMap(const uint8_t *keys, uint32_t count);
    virtual uint64_t __stdcall GetMappedKeys();
    // 0xBC / 0xC0 / 0xC4 0x1006f550..: either shift / control / alt key down.
    virtual bool __stdcall IsShiftDown();
    virtual bool __stdcall IsCtrlDown();
    virtual bool __stdcall IsAltDown();
    // 0xC8 0x1006f5b0: IGSHIFT_* bits; `read` polls the keyboard first.
    virtual uint32_t __stdcall GetShiftKeys(bool read);
    // 0xCC 0x1006f660: the character of a key (shift bit 1 of `shiftKeys`: the shifted one).
    virtual uint8_t __stdcall KeyToChar(int key, uint32_t shiftKeys);
    // 0xD0 0x1006f6a0: DirectInput's name of a key (DIPROP_KEYNAME; 0 without a keyboard).
    virtual const wchar_t *__stdcall GetKeyName(int key);
    // 0xD4 0x10057630 (an engine-wide "return 0" stub)
    virtual int __stdcall ReservedD4();
    // 0xD8 0x1006f700 (does nothing)
    virtual bool __stdcall ReservedD8(int, int);
    // 0xDC 0x1006f710: mouse; mode 0 absolute (cursor), 1 exclusive relative; -1 queries the mode.
    virtual int __stdcall InitMouse(int mode);
    // 0xE0 0x1006f840
    virtual bool __stdcall IsMouseInit();
    // 0xE4 0x1006f850
    virtual HRESULT __stdcall CloseMouse();
    // 0xE8 0x1006f8a0: polls the mouse (once per frame).
    virtual HRESULT __stdcall ReadMouse();
    // 0xEC / 0xF0 / 0xF4 0x1006fa10..: movement since the last ReadMouse (F4: wheel).
    virtual int __stdcall GetMouseDX();
    virtual int __stdcall GetMouseDY();
    virtual int __stdcall GetMouseDZ();
    // 0xF8 0x1006fa40: the position (moves the cursor in absolute mode).
    virtual int __stdcall SetMousePos(int x, int y);
    // 0xFC / 0x100 0x1006fb10 / 0x1006fb20
    virtual int __stdcall GetMouseX();
    virtual int __stdcall GetMouseY();
    // 0x104 0x1006fb30: buttons 0..4 as bits.
    virtual uint32_t __stdcall GetMouseButtons();
    // 0x108 0x1006fb70: timeGetTime(), remembered.
    virtual DWORD __stdcall ReadTimer();
    // 0x10C 0x1006fb80: milliseconds since the last ReadTimer (now re-read); sleeps up to at least
    // `minMs`, reports at most `maxMs` (0: no limit).
    virtual DWORD __stdcall GetTimer(DWORD minMs, DWORD maxMs);
    // 0x110 0x1006ff80: a texture search directory (with its subdirectories); created if missing.
    virtual HRESULT __stdcall AddDir(const char *dir, bool recursive);
    // 0x114 0x1006ffa0: removes the directories whose path contains `text`.
    virtual HRESULT __stdcall DelDir(const char *text);
    // 0x118 0x10070000
    virtual void __stdcall DelAllDirs();
    // 0x11C..0x128 0x10070060..: the texture lists (cached = shared by LoadTexture).
    virtual ITexture **__stdcall GetCachedTextures();
    virtual int __stdcall NumCachedTextures();
    virtual ITexture **__stdcall GetUncachedTextures();
    virtual int __stdcall NumUncachedTextures();
    // 0x12C / 0x130 0x100700a0 / 0x100700e0: 3 x 256 words, red, green, blue (IGCAPS_GAMMA).
    virtual bool __stdcall SetGammaRamp(const uint16_t *ramp, bool calibrate);
    virtual bool __stdcall GetGammaRamp(uint16_t *ramp);
    // 0x134 0x10070160: resamples a 24 / 32-bit image (box filter when shrinking and `filter`).
    virtual bool __stdcall ResizeImage(int srcWidth, int srcHeight, int srcPitch, const uint8_t *src, int dstWidth,
                                       int dstHeight, int dstPitch, uint8_t **dst, int bytesPerPixel, bool filter);

    IGraph();                           // 0x10069a00
    ~IGraph();                          // 0x10069e50
};

// SaveShot flags.
enum : uint32_t {
    IGSHOT_MEMORY = 0x00000001,         // copy into the caller's buffer instead of writing a file
    IGSHOT_RGB24 = 0x00020000,          // memory: convert to 24-bit BGR
    IGSHOT_DEPTH = 0x00100000,          // the depth buffer too (needs IG_LOCKABLE_ZBUFFER)
};

// InitKeyboard flags.
enum : uint32_t {
    IGKEY_EXCLUSIVE = 0x1,
    IGKEY_WINDOW_MESSAGES = 0x2,        // keys from WM_KEYDOWN / WM_CHAR instead of DirectInput
    IGKEY_REPEAT = 0x4,                 // GetKey repeats a held key (350 ms, then every 34 ms)
};

// GetShiftKeys bits.
enum : uint32_t {
    IGSHIFT_SHIFT = 0x001,
    IGSHIFT_CTRL = 0x004,
    IGSHIFT_ALT = 0x008,
    IGSHIFT_LSHIFT = 0x020,
    IGSHIFT_RSHIFT = 0x040,
    IGSHIFT_LCTRL = 0x080,
    IGSHIFT_RCTRL = 0x100,
    IGSHIFT_LALT = 0x200,
    IGSHIFT_RALT = 0x400,
};

// The engine's IGraph (0x100d16f4), created by the DLL's start-up.
extern IGraph *g_pIGraph;
extern "C" IGraph *__cdecl GetIGraph();
