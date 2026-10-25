// IGraph: window, Direct3D 8 device, render states and textures (IGraph.cpp, 0x100699a0 - 0x10070eb0).
// Input and timing are in igraph_input.cpp, image files in igraph_image.cpp.
#include "igraph/igraph.h"

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <direct.h>

#include "core/cpu.h"
#include "core/debug.h"
#include "core/enhance.h"
#include "dx/dxerr8.h"
#include "i3d/i3d_links.h"
#include "i3d/vb_manager.h"
#include "igraph/igraph_state.h"
#include "igraph/ishow.h"
#include "igraph/itexture.h"
#include "igraph/texture_stream.h"
#include "isnd/isnd_voices.h"
#include "math/ls3d_math.h"

using namespace ls3d;
using namespace ls3d::ig;

IGraph *g_pIGraph = nullptr;                    // 0x100d16f4

namespace ls3d::ig {

IDirect3D8 *g_pD3D = nullptr;
IDirect3DDevice8 *g_pDevice = nullptr;
S_IGraphInit g_init;
D3DPRESENT_PARAMETERS g_presentParams;
D3DDEVTYPE g_deviceType = D3DDEVTYPE_HAL;
HWND g_mainWnd = nullptr;
HWND g_childWnd = nullptr;
bool g_initialized = false;
bool g_inScene = false;
bool g_inactive = false;
bool g_noActivation = false;

uint32_t g_caps = 0;
uint32_t g_quirks = 0;
int g_maxTextureWidth = 0;
int g_maxTextureHeight = 0;
int g_anisotropy = 0;

D3DFORMAT g_fmt16Bit = D3DFMT_UNKNOWN;
D3DFORMAT g_fmt1BitAlpha = D3DFMT_UNKNOWN;
D3DFORMAT g_fmtAlpha = D3DFMT_UNKNOWN;
D3DFORMAT g_fmtTrueColor = D3DFMT_UNKNOWN;
D3DFORMAT g_fmtDxt1 = D3DFMT_UNKNOWN;
D3DFORMAT g_fmtDxt3 = D3DFMT_UNKNOWN;
D3DFORMAT g_fmtDxt5 = D3DFMT_UNKNOWN;
D3DFORMAT g_fmtBump = D3DFMT_UNKNOWN;

DWORD g_vsNormalTex1 = 0;
DWORD g_vsNormalTex2 = 0;
DWORD g_vsColorTex1 = 0;
DWORD g_vsColorTex2 = 0;
DWORD g_vsFarPlane = 0;

D3DMATRIX g_texProjectXY;
D3DMATRIX g_texProjectXZ;

S_stateCache g_rs = {};
int g_zBufferType = D3DZB_TRUE;

C_vector<ITexture *> g_cachedTextures(0x400);
C_vector<ITexture *> g_uncachedTextures(0x400);
C_vector<IShow *> g_shows(0x20);
ITexture *g_defaultTexture = nullptr;
ITexture *g_bumpAnimTextures[17] = {};
uint16_t g_textureId = 0;

CRITICAL_SECTION g_textureLock;
volatile int g_frameCounter = 0;
HANDLE g_textureThread = nullptr;
bool g_textureThreadRun = false;

bool g_showFps = false;
bool g_showSoundStats = false;
bool g_logSoundFailures = false;
D3DVIEWPORT8 g_viewport;

}  // namespace ls3d::ig

namespace {

const char kSourceFile[] = "C:\\MyProjects\\Engine8_5\\IGraph\\IGraph.cpp";
const char kWindowClass[] = "LS3D";

// Window title, and its copy while the application is inactive.
char g_appName[0x100];                          // 0x100d18b0
char g_savedAppName[0x100];                     // 0x100d1aa0

C_vector<HWND> g_dialogs(0x20);                 // 0x100d19b0
C_vector<D3DDISPLAYMODE> g_displayModes(8);     // 0x100d2180 (refresh rates cleared)
C_vector<D3DFORMAT> g_displayFormats(0x20);     // 0x100d2138
uint8_t g_bitDepth = 0;                         // 0x100d1d40 (16 / 32; Init's request in fullscreen)

// Accessibility keys switched off while the engine runs (SPI_GET/SETSTICKYKEYS ...). Only the
// sticky keys structure gets its size: the filter and toggle keys calls fail as in the original.
STICKYKEYS g_stickyKeys;                        // 0x100d1e3c
FILTERKEYS g_filterKeys;                        // 0x100d1ddc
TOGGLEKEYS g_toggleKeys;                        // 0x100d2164

// FPS overlay counters.
int g_fpsFrames = 0;                            // 0x100d21bc
int g_fps = 0;                                  // 0x100d21c0
DWORD g_fpsTime = timeGetTime();                // 0x100d2158 (static initializer 0x10069990)

}  // namespace

namespace ls3d::ig {
D3DMATERIAL8 g_defaultMaterial;
}

namespace ls3d::ig {

// 0x1007d466 is DXGetErrorString8 (dxerr8.lib, linked into the original).
void LogDxError(HRESULT hr, int line)
{
    dbgPrintf("!! Error detected: %s, File: %s, Line: %i !!", DXGetErrorString8A(hr), kSourceFile, line);
}

}  // namespace ls3d::ig

// ---------------------------------------------------------------- construction

// 0x100699a0: the 17 frames of the animated bump map (called by the renderer on first use).
bool LoadBumpAnimTextures(IGraph *igraph)
{
    for (int i = 0; i < 17; ++i) {
        char name[100];
        std::sprintf(name, "BumpAnim_%02i.dds", i);
        if (igraph->LoadTexture(name, nullptr, 0, &g_bumpAnimTextures[i]) < 0)
            return false;
    }
    return true;
}

// 0x10069a00
IGraph::IGraph()
{
    std::memset(&g_init, 0, sizeof g_init);
    g_initialized = false;
    g_mainWnd = nullptr;
    g_childWnd = nullptr;
    g_pD3D = nullptr;
    g_pDevice = nullptr;
    std::memset(&g_viewport, 0, sizeof g_viewport);

    // 0x100d1ba0: diffuse and ambient white, no specular, emissive white, power 0.
    float *m = reinterpret_cast<float *>(&g_defaultMaterial);
    for (int i = 0; i < 8; ++i)
        m[i] = 1.0f;
    for (int i = 8; i < 12; ++i)
        m[i] = 0.0f;
    for (int i = 12; i < 16; ++i)
        m[i] = 1.0f;
    m[16] = 0.0f;

    g_maxTextureWidth = 0;
    g_maxTextureHeight = 0;
    std::strcpy(g_appName, kWindowClass);

    // Texture projection matrices. The original builds the rows from 4-float temporaries whose last
    // element it never sets (stack garbage in column 3); the port uses 0 (D3DTTFF_COUNT2 ignores it).
    std::memset(&g_texProjectXY, 0, sizeof g_texProjectXY);
    g_texProjectXY.m[0][0] = 0.5f;
    g_texProjectXY.m[1][1] = -0.5f;
    g_texProjectXY.m[3][0] = 0.5f;
    g_texProjectXY.m[3][1] = -0.5f;
    std::memset(&g_texProjectXZ, 0, sizeof g_texProjectXZ);
    g_texProjectXZ.m[0][0] = 0.5f;
    g_texProjectXZ.m[2][1] = -0.5f;
    g_texProjectXZ.m[3][0] = 0.5f;
    g_texProjectXZ.m[3][1] = -0.5f;

    std::memset(&g_presentParams, 0, sizeof g_presentParams);
    ResetInputState();
    g_rs.alphaRef = 0x20;

    g_cachedTextures.Reserve(0x1000);
    g_uncachedTextures.Reserve(0x1000);
    InitializeCriticalSection(&g_textureLock);
}

// 0x10069e50
IGraph::~IGraph()
{
    if (g_initialized)
        dbgPrintf("!! WARNING  IGraph was initialised, but not closed");
    ClipCursor(nullptr);
    DeleteCriticalSection(&g_textureLock);
}

// ---------------------------------------------------------------- object factories

// 0x10069e80
IShow *__stdcall IGraph::CreateIShow()
{
    return new IShow;
}

// 0x10069ee0
ITexture *__stdcall IGraph::CreateITexture()
{
    ITexture *texture = new ITexture;
    texture->id = ++g_textureId;
    if (g_textureId > 0x7ffe)
        g_textureId = 0;
    return texture;
}

// Finds a texture in the search directories (0x1006fbd0); defined in igraph_image.cpp.
void FindTextureFile(const char *fileName, char *path);

// 0x10069f70
HRESULT __stdcall IGraph::LoadTexture(const char *fileName, const char *alphaFileName, uint32_t flags,
                                      ITexture **out)
{
    // DDS files carry their own format and mip levels: the conversion options do not apply.
    if (fileName && *fileName) {                // (the original also reads fileName[-1] of "")
        const char last = fileName[std::strlen(fileName) - 1];
        if (last == 's' || last == 'S')
            flags &= ~ITC_DDS_IGNORED;
    }
    *out = nullptr;
    if (flags & ITC_DEFAULT) {
        ++g_defaultTexture->refCount;
        *out = g_defaultTexture;
        return 0;
    }
    if (alphaFileName && *alphaFileName)
        flags |= ITC_USE_ALPHA;

    if (!(flags & (ITC_FROM_MEMORY | ITC_NOCACHE))) {
        if (!fileName || !*fileName)
            return I3DERR_INVALIDPARAMS;
        for (int i = g_cachedTextures.Size(); i--;) {
            ITexture *texture = g_cachedTextures[i];
            if (texture->requestedFlags != flags && texture->flags != flags)
                continue;
            if (_stricmp(texture->fileName ? texture->fileName : "", fileName))
                continue;
            if ((flags & ITC_USE_ALPHA) &&
                _stricmp(texture->alphaFileName ? texture->alphaFileName : "", alphaFileName))
                continue;
            ++texture->refCount;
            *out = texture;
            return 0;
        }
    }

    ITexture *texture = CreateITexture();
    if (!texture)
        return I3DERR_OUTOFMEM;

    char path[0x100];
    if (fileName && !(flags & ITC_FROM_MEMORY)) {
        FindTextureFile(fileName, path);
        if (!path[0])
            std::sprintf(path, "Maps\\%s", fileName);
    } else {
        path[0] = 0;
    }
    char alphaPath[0x100];
    if (alphaFileName && *alphaFileName) {
        FindTextureFile(alphaFileName, alphaPath);
        if (!alphaPath[0])
            std::sprintf(alphaPath, "Maps\\%s", alphaFileName);
    } else {
        alphaPath[0] = 0;
    }

    const HRESULT hr = texture->Create((flags & ITC_FROM_MEMORY) ? fileName : path,
                                     ((flags & ITC_USE_ALPHA) && alphaPath[0]) ? alphaPath : nullptr, flags, 0,
                                     0, 0);
    if (hr < 0) {
        texture->Release();
        *out = nullptr;
        return hr;
    }
    *out = texture;
    return 0;
}

// 0x1006a1f0
bool __stdcall IGraph::SetTexture(ITexture *texture, int stage)
{
    return g_pDevice->SetTexture(stage, texture ? texture->d3dTexture : nullptr) >= 0;
}

// 0x1006a240
HWND __stdcall IGraph::GetMainHWND()
{
    return g_mainWnd;
}

// 0x1006a250
HWND __stdcall IGraph::GetChildHWND()
{
    return g_childWnd;
}

// 0x1006a260
bool __stdcall IGraph::SetAppName(const char *name)
{
    if (!name) {
        if (!g_appName[0])
            return false;
        g_appName[0] = 0;
    } else {
        if (!std::strcmp(g_appName, name))
            return false;
        std::strncpy(g_appName, name, 0x100);
        g_appName[0xff] = 0;
    }
    return SetWindowTextA(g_mainWnd, g_appName) != FALSE;
}

// 0x1006a2f0
bool __cdecl IGraph::SetAppNameF(const char *format, ...)
{
    char buf[0xfc];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof buf, format, args);  // the original's vsprintf has no limit
    va_end(args);
    return SetAppName(buf);
}

// 0x1006a330
const char *__stdcall IGraph::GetAppName()
{
    return g_appName;
}

// 0x1006a340
bool __stdcall IGraph::AddDlg(HWND dialog)
{
    if (!dialog)
        return false;
    g_dialogs.PushBack(dialog);
    return true;
}

// 0x1006a3d0
bool __stdcall IGraph::DelDlg(HWND dialog)
{
    return g_dialogs.RemoveSwap(dialog);
}

// 0x1006a470
HRESULT __stdcall IGraph::ProcessWinMessages()
{
    for (;;) {
        MSG msg;
        while (!PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE | PM_NOYIELD)) {
            if (!g_inactive || g_noActivation || !(g_init.flags & IG_WAIT_INACTIVE))
                return 0;
            WaitMessage();
        }
        int dialog = 0;
        for (; dialog < g_dialogs.Size(); ++dialog) {
            if (IsDialogMessageA(g_dialogs[dialog], &msg))
                break;
        }
        // Typed characters (not repeats of a held key) queue for ReadChar.
        if (msg.message == WM_CHAR && static_cast<LONG>(msg.lParam) >= 0)
            QueueChar(static_cast<int>(msg.wParam));
        if (KeyboardUsesMessages())
            KeyFromMessage(msg);
        if (dialog == g_dialogs.Size()) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

// ---------------------------------------------------------------- device

// 0x1006a6c0
HRESULT __stdcall IGraph::Clear(D3DCOLOR color, float z, uint32_t flags)
{
    DWORD d3dFlags = (flags & 1) ? D3DCLEAR_TARGET : 0;
    if (flags & 2)
        d3dFlags |= D3DCLEAR_ZBUFFER;
    if (g_caps & IGCAPS_STENCIL)
        d3dFlags |= D3DCLEAR_STENCIL;
    return g_pDevice->Clear(0, nullptr, d3dFlags, color, z, 0);
}

namespace ls3d::ig {

// 0x1006a710
void ResetDeviceStates()
{
    IDirect3DDevice8 *dev = g_pDevice;
    dev->SetRenderState(D3DRS_AMBIENT, 0xff00ff);
    dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    dev->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, TRUE);
    dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
    dev->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGEND, 0x43960000);  // 300.0f
    dev->SetRenderState(D3DRS_FOGSTART, 0);
    dev->SetRenderState(D3DRS_FOGCOLOR, 0xffffffff);
    dev->SetRenderState((g_caps & IGCAPS_TABLE_FOG) ? D3DRS_FOGTABLEMODE : D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, 2);
    dev->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_CURRENT);
    dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(2, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(2, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ZBIAS, 0);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetTextureStageState(1, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetTextureStageState(1, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetTransform(D3DTS_TEXTURE0, &g_texProjectXY);
    dev->SetTransform(D3DTS_TEXTURE1, &g_texProjectXY);
    dev->SetVertexShader(g_vsNormalTex1);

    g_rs.colorOp[1] = D3DTOP_DISABLE;
    g_rs.colorOp[2] = D3DTOP_DISABLE;
    g_rs.zFunc = 1;
    g_rs.zWrite = 1;
    g_rs.addressU0 = D3DTADDRESS_WRAP;
    g_rs.addressV0 = D3DTADDRESS_WRAP;
    g_rs.specular = 0;
    g_rs.fog = 0;
    g_rs.fogMode = 0;
    g_rs.fogStart = 0.0f;
    g_rs.alpha = 0;
    g_rs.blend = 0;
    g_rs.zBias = 0;
    g_rs.unknown19c0 = 0;
    g_rs.unknown1888 = 0;
    g_rs.unknown1780 = 0;
    g_rs.vertexShader = 0;
    g_rs.fogEnd = 300.0f;
    g_rs.fogColor = 0xffffffff;
    g_rs.cull = D3DCULL_CCW;
    g_rs.cullDefault = D3DCULL_CCW;
    g_rs.colorOp[0] = D3DTOP_MODULATE;
    g_rs.alphaOp[0] = D3DTOP_MODULATE;
    g_rs.alphaOp[1] = D3DTOP_MODULATE;
}

}  // namespace ls3d::ig

// 0x1006abc0
HRESULT __stdcall IGraph::SetState(int state, int value)
{
    IDirect3DDevice8 *dev = g_pDevice;
    switch (state) {
    case IGS_ALPHA:
        if (g_rs.alpha == value)
            break;
        g_rs.alpha = value;
        switch (value) {
        case 0:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            return dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        case 1:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            return dev->SetRenderState(D3DRS_ALPHAREF, 1);
        case 2:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            return dev->SetRenderState(D3DRS_ALPHAREF, 0);
        case 3:
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            return dev->SetRenderState(D3DRS_ALPHAREF, g_rs.alphaRef);
        }
        break;
    case IGS_BLEND:
        if (g_rs.blend == value)
            break;
        g_rs.blend = value;
        switch (value) {
        case 0:
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            return dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        case 1:
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            return dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        case 2:
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            return dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        case 3:
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
            return dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
        case 4:
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
            return dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        }
        break;
    case IGS_CULL:
        if (g_rs.cull == value)
            break;
        g_rs.cull = value;
        return dev->SetRenderState(D3DRS_CULLMODE, value == 3 ? g_rs.cullDefault : value);
    case IGS_COLOROP0:
    case IGS_COLOROP1:
    case IGS_COLOROP2:
        if (g_rs.colorOp[state - IGS_COLOROP0] == value)
            break;
        g_rs.colorOp[state - IGS_COLOROP0] = value;
        return dev->SetTextureStageState(state - IGS_COLOROP0, D3DTSS_COLOROP, value);
    case IGS_ALPHAOP0:
    case IGS_ALPHAOP1:
        if (g_rs.alphaOp[state - IGS_ALPHAOP0] == value)
            break;
        g_rs.alphaOp[state - IGS_ALPHAOP0] = value;
        return dev->SetTextureStageState(state - IGS_ALPHAOP0, D3DTSS_ALPHAOP, value);
    case IGS_ZBIAS:
        if (g_rs.zBias == value)
            break;
        g_rs.zBias = value;
        return dev->SetRenderState(D3DRS_ZBIAS, value);
    case IGS_ADDRESSU0:
        if (g_rs.addressU0 == value)
            break;
        g_rs.addressU0 = value;
        return dev->SetTextureStageState(0, D3DTSS_ADDRESSU, value);
    case IGS_ADDRESSV0:
        // The original sets ADDRESSU here as well (and GetState(IGS_ADDRESSV0) returns the U value).
        if (g_rs.addressV0 == value)
            break;
        g_rs.addressV0 = value;
        return dev->SetTextureStageState(0, D3DTSS_ADDRESSU, value);
    case IGS_ALPHAREF:
        g_rs.alphaRef = value;
        return dev->SetRenderState(D3DRS_ALPHAREF, value);
    case IGS_ZFUNC:
        if (g_rs.zFunc == value)
            break;
        g_rs.zFunc = value;
        return dev->SetRenderState(D3DRS_ZFUNC, value ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
    case IGS_ZWRITE:
        SetZWrite(value);
        return value;   // 0x10063720 returns nothing; this is what was in EAX in most cases
    case IGS_ZENABLE:
        return dev->SetRenderState(D3DRS_ZENABLE, value ? g_zBufferType : D3DZB_FALSE);
    case IGS_CLAMP0:
        if (value) {
            dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            return dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        return dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    case IGS_CLAMP1:
        if (value) {
            dev->SetTextureStageState(1, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            return dev->SetTextureStageState(1, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        dev->SetTextureStageState(1, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        return dev->SetTextureStageState(1, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    case IGS_FOG: {
        const HRESULT hr = dev->SetRenderState(D3DRS_FOGENABLE, value ? TRUE : FALSE);
        g_rs.fog = value ? 1 : 0;
        return hr;
    }
    case IGS_SPECULAR:
        SetSpecular(value);
        return value;
    case IGS_COLORWRITE:
        return dev->SetRenderState(D3DRS_COLORWRITEENABLE, value);
    }
    return value;
}

// 0x1006b220
int __stdcall IGraph::GetState(int state)
{
    DWORD value = 0;
    switch (state) {
    case IGS_ALPHA: return g_rs.alpha;
    case IGS_BLEND: return g_rs.blend;
    case IGS_CULL: return g_rs.cull;
    case IGS_COLOROP0: return g_rs.colorOp[0];
    case IGS_COLOROP1: return g_rs.colorOp[1];
    case IGS_COLOROP2: return g_rs.colorOp[2];
    case IGS_ALPHAOP0: return g_rs.alphaOp[0];
    case IGS_ALPHAOP1: return g_rs.alphaOp[1];
    case IGS_ZBIAS: return g_rs.zBias;
    case IGS_ADDRESSU0:
    case IGS_ADDRESSV0: return g_rs.addressU0;
    case IGS_ALPHAREF: return g_rs.alphaRef;
    case IGS_ZFUNC: return g_rs.zFunc;
    case IGS_ZWRITE: return g_rs.zWrite;
    case IGS_ZENABLE:
        g_pDevice->GetRenderState(D3DRS_ZENABLE, &value);
        return static_cast<int>(value);
    case IGS_FOG: return g_rs.fog;
    case IGS_SPECULAR: return g_rs.specular;
    case IGS_COLORWRITE:
        g_pDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &value);
        return static_cast<int>(value);
    }
    return -1;
}

// 0x1006b360
HRESULT __stdcall IGraph::SetWorldMatrix(const S_matrix *matrix)
{
    return g_pDevice->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX *>(matrix));
}

// 0x1006b380
HRESULT __stdcall IGraph::BeginScene()
{
    if (g_inScene)
        return 1;
    g_inScene = true;
    return g_pDevice->BeginScene();
}

// 0x1006b3b0
HRESULT __stdcall IGraph::EndScene()
{
    if (!g_inScene)
        return 0;
    g_inScene = false;
    return g_pDevice->EndScene();
}

// 0x1006b3e0
HRESULT __stdcall IGraph::Flip()
{
    snd::UpdateVoices();
    if (g_showFps) {
        ++g_fpsFrames;
        if (static_cast<int>(timeGetTime() - g_fpsTime) >= 1000) {
            g_fpsTime = timeGetTime();
            g_fps = g_fpsFrames;
            g_fpsFrames = 0;
        }
        g_pDevice->BeginScene();
        char text[0x34];
        std::sprintf(text, "%i", g_fps);
        i3d::DrawDebugText(0x50, 0x28, text);
        g_pDevice->EndScene();
    }
    if (g_showSoundStats) {
        g_pDevice->BeginScene();
        snd::DrawStats();
        g_pDevice->EndScene();
    }
    if (g_init.flags & IG_TEXTURE_THREAD) {
        EnterCriticalSection(&g_textureLock);
        ++g_frameCounter;
        LeaveCriticalSection(&g_textureLock);
    }
    Sleep(0);
    HRESULT hr = g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
    if (hr == D3DERR_DEVICELOST) {
        if (IsMouseInit()) {
            CloseMouse();
            dbgPrintf("DEVICE LOST - 0");
        }
        if (!g_inactive && g_pDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
            dbgPrintf("DEVICE LOST - 1");
            if (RestoreDevice(true) >= 0 && (g_init.flags & IG_TEXTURE_THREAD))
                tex::StreamInit(g_init.flags);
            ResetDeviceStates();
            dbgPrintf("DEVICE LOST - 2");
            if (MouseMode() > -1)
                InitMouse(MouseMode());
        }
    }
    return hr;
}

// 0x1006b5a0 / 0x1006b6e0: the primitive kinds and vertex formats of DrawPrimitive.
static bool PrimitiveType(int kind, D3DPRIMITIVETYPE *type)
{
    switch (kind) {
    case IGP_POINTS:
    case 6: *type = D3DPT_POINTLIST; return true;
    case IGP_LINES: *type = D3DPT_LINELIST; return true;
    case IGP_LINESTRIP: *type = D3DPT_LINESTRIP; return true;
    case IGP_TRIANGLES: *type = D3DPT_TRIANGLELIST; return true;
    case IGP_TRIANGLEFAN: *type = D3DPT_TRIANGLEFAN; return true;
    case IGP_TRIANGLESTRIP: *type = D3DPT_TRIANGLESTRIP; return true;
    }
    return false;
}

static bool SelectVertexFormat(int format, UINT *stride)
{
    switch (format) {
    case IGV_COLOR_TEX:
        if (g_rs.vertexShader != 2) {
            g_rs.vertexShader = 2;
            g_pDevice->SetVertexShader(g_vsColorTex1);
        }
        *stride = 0x18;
        return true;
    case IGV_TRANSFORMED:
        if (g_rs.vertexShader != 6) {
            g_rs.vertexShader = 6;
            g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
        }
        *stride = 0x20;
        return true;
    case IGV_TRANSFORMED_TEX2:
        if (g_rs.vertexShader != 7) {
            g_rs.vertexShader = 7;
            g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2);
        }
        *stride = 0x28;
        return true;
    }
    return false;
}

// 0x1006b5a0
HRESULT __stdcall IGraph::DrawPrimitive(int kind, int count, const void *vertices, int vertexFormat)
{
    if (!count)
        return 0;
    D3DPRIMITIVETYPE type;
    UINT stride;
    if (!PrimitiveType(kind, &type) || !SelectVertexFormat(vertexFormat, &stride))
        return I3DERR_INVALIDPARAMS;
    return g_pDevice->DrawPrimitiveUP(type, count, vertices, stride) < 0 ? I3DERR_GENERIC : 0;
}

// 0x1006b6e0
HRESULT __stdcall IGraph::DrawIndexedPrimitive(int kind, int numVertices, const uint16_t *indices, int count,
                                               const void *vertices, int vertexFormat)
{
    if (!count)
        return 0;
    D3DPRIMITIVETYPE type;
    UINT stride;
    if (!PrimitiveType(kind, &type) || !SelectVertexFormat(vertexFormat, &stride))
        return I3DERR_INVALIDPARAMS;
    return g_pDevice->DrawIndexedPrimitiveUP(type, 0, numVertices, count, indices, D3DFMT_INDEX16, vertices,
                                             stride) < 0
               ? I3DERR_GENERIC
               : 0;
}

// 0x1006b820
static LRESULT CALLBACK IGraphWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 0;
    case WM_DESTROY:
        if (!g_initialized)
            return 1;
        PostQuitMessage(0);
        break;
    case WM_CLOSE:
        if (!g_initialized)
            return 1;
        g_pIGraph->Close();
        std::exit(static_cast<int>(wParam));
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
    case WM_ACTIVATE: {
        const bool active = LOWORD(wParam) != WA_INACTIVE;
        bool deactivate = false;
        bool reactivate = false;
        if (g_noActivation) {
            reactivate = g_inactive;
        } else if (HIWORD(wParam)) {                // minimised
            deactivate = !g_inactive;
        } else if (g_inactive) {
            reactivate = active;
        } else if (!active) {
            // Losing the focus to one of our own windows does not count.
            const HWND foreground = GetForegroundWindow();
            deactivate = !IsChild(g_mainWnd, foreground) && g_mainWnd != foreground;
        }
        if (deactivate) {
            std::strcpy(g_savedAppName, g_appName);
            if (g_init.flags & IG_WAIT_INACTIVE) {
                if (std::strlen(g_appName) < 0xf6)
                    std::strcat(g_appName, " ..zzZZ");
                SetWindowTextA(g_mainWnd, g_appName);
                if (g_textureThread)
                    SuspendThread(g_textureThread);
            }
            g_inactive = true;
        } else if (reactivate) {
            g_pIGraph->SetAppName(g_savedAppName);
            g_inactive = false;
            if (g_pDevice)
                i3d::ReapplyDriverStates();
            // The players' volume is lost with the focus.
            for (int i = g_shows.Size(); i--;) {
                IShow *show = g_shows[i];
                show->SetVolume(show->GetVolume());
            }
            if (g_textureThread)
                ResumeThread(g_textureThread);
        }
        break;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// 0x1006bb80
const char *__stdcall IGraph::GetFormatName(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_UNKNOWN: return "D3DFMT_UNKNOWN";
    case D3DFMT_R8G8B8: return "D3DFMT_R8G8B8";
    case D3DFMT_A8R8G8B8: return "D3DFMT_A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "D3DFMT_X8R8G8B8";
    case D3DFMT_R5G6B5: return "D3DFMT_R5G6B5";
    case D3DFMT_X1R5G5B5: return "D3DFMT_X1R5G5B5";
    case D3DFMT_A1R5G5B5: return "D3DFMT_A1R5G5B5";
    case D3DFMT_A4R4G4B4: return "D3DFMT_A4R4G4B4";
    case D3DFMT_R3G3B2: return "D3DFMT_R3G3B2";
    case D3DFMT_A8: return "D3DFMT_A8";
    case D3DFMT_A8R3G3B2: return "D3DFMT_A8R3G3B2";
    case D3DFMT_X4R4G4B4: return "D3DFMT_X4R4G4B4";
    case D3DFMT_A8P8: return "D3DFMT_A8P8";
    case D3DFMT_P8: return "D3DFMT_P8";
    case D3DFMT_L8: return "D3DFMT_L8";
    case D3DFMT_A8L8: return "D3DFMT_A8L8";
    case D3DFMT_A4L4: return "D3DFMT_A4L4";
    case D3DFMT_V8U8: return "D3DFMT_V8U8";
    case D3DFMT_L6V5U5: return "D3DFMT_L6V5U5";
    case D3DFMT_X8L8V8U8: return "D3DFMT_X8L8V8U8";
    case D3DFMT_Q8W8V8U8: return "D3DFMT_Q8W8V8U8";
    case D3DFMT_V16U16: return "D3DFMT_V16U16";
    case D3DFMT_W11V11U10: return "D3DFMT_W11V11U10";
    case D3DFMT_D16_LOCKABLE: return "D3DFMT_D16_LOCKABLE";
    case D3DFMT_D32: return "D3DFMT_D32";
    case D3DFMT_D15S1: return "D3DFMT_D15S1";
    case D3DFMT_D24S8: return "D3DFMT_D24S8";
    case D3DFMT_D24X8: return "D3DFMT_D24X8";
    case D3DFMT_D24X4S4: return "D3DFMT_D24X4S4";
    case D3DFMT_D16: return "D3DFMT_D16";
    case D3DFMT_VERTEXDATA: return "D3DFMT_VERTEXDATA";
    case D3DFMT_INDEX16: return "D3DFMT_INDEX16";
    case D3DFMT_INDEX32: return "D3DFMT_INDEX32";
    case D3DFMT_DXT1: return "D3DFMT_DXT1";
    case D3DFMT_DXT2: return "D3DFMT_DXT2";
    case D3DFMT_DXT3: return "D3DFMT_DXT3";
    case D3DFMT_DXT4: return "D3DFMT_DXT4";
    case D3DFMT_DXT5: return "D3DFMT_DXT5";
    case D3DFMT_YUY2: return "D3DFMT_YUY2";
    case D3DFMT_UYVY: return "D3DFMT_UYVY";
    case D3DFMT_FORCE_DWORD: return "D3DFMT_FORCE_DWORD";
    default: return "UNSUPPORTED_FORMAT";
    }
}

// The depth formats tried for a 16 / 32-bit display, from the last entry to the first.
static const D3DFORMAT kDepthFormats16[4] = {D3DFMT_D15S1, D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D32};
static const D3DFORMAT kDepthFormats32[4] = {D3DFMT_D24X4S4, D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D32};

static bool Is16BitFormat(D3DFORMAT f) { return f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 || f == D3DFMT_A1R5G5B5; }
static bool Is32BitFormat(D3DFORMAT f) { return f == D3DFMT_R8G8B8 || f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8; }

// Picks the depth buffer format for the back buffer format (part of Init and RestoreDevice).
static HRESULT ChooseDepthFormat()
{
    const D3DFORMAT *list = g_bitDepth == 16 ? kDepthFormats16 : kDepthFormats32;
    const D3DFORMAT display = g_presentParams.BackBufferFormat;
    for (int i = 4; i--;) {
        if (g_pD3D->CheckDeviceFormat(g_init.adapter, g_deviceType, display, D3DUSAGE_DEPTHSTENCIL,
                                      D3DRTYPE_SURFACE, list[i]) >= 0 &&
            g_pD3D->CheckDepthStencilMatch(g_init.adapter, g_deviceType, display, display, list[i]) >= 0) {
            g_presentParams.AutoDepthStencilFormat = list[i];
            break;
        }
    }
    if (g_init.flags & IG_LOCKABLE_ZBUFFER) {
        g_presentParams.AutoDepthStencilFormat = D3DFMT_D16_LOCKABLE;
        return 0;
    }
    const D3DFORMAT depth = g_presentParams.AutoDepthStencilFormat;
    if (!depth) {
        dbgPrintf("IGraph::Init() - Error. Useful Z-Buffer format not found !!");
        return I3DERR_GENERIC;
    }
    if (depth == D3DFMT_D15S1 || depth == D3DFMT_D24X4S4 || depth == D3DFMT_D24S8)
        g_caps |= IGCAPS_STENCIL;
    return 0;
}

static void SetBitDepthFromFormat(D3DFORMAT format)
{
    if (Is16BitFormat(format))
        g_bitDepth = 16;
    if (Is32BitFormat(format))
        g_bitDepth = 32;
}

namespace ls3d::ig {

// 0x1006be50
HRESULT RestoreDevice(bool reset)
{
    g_vbManager.ReleaseDefaultPool();
    g_ibManager.ReleaseDefaultPool();
    if (g_init.flags & IG_TEXTURE_THREAD)
        tex::StreamFlush();
    if (reset) {
        if (!(g_init.flags & IG_FULLSCREEN)) {
            // The desktop format may have changed while the device was lost.
            D3DDISPLAYMODE mode;
            g_pD3D->GetAdapterDisplayMode(g_init.adapter, &mode);
            g_presentParams.BackBufferFormat = mode.Format;
            SetBitDepthFromFormat(mode.Format);
            const HRESULT hr = ChooseDepthFormat();
            if (hr < 0)
                return hr;
        }
        const HRESULT hr = g_pDevice->Reset(&g_presentParams);
        if (hr < 0) {
            LogDxError(hr, 0x7af);
            return I3DERR_GENERIC;
        }
        if (g_pDevice)
            i3d::ReapplyDriverStates();
        i3d::InvalidateLights();
        i3d::InitParticleBuffers();
    }

    struct Shader {
        const DWORD *decl;
        const DWORD *function;
        DWORD *handle;
        int line;
    };
    static const DWORD kDeclNormalTex1[] = {D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
                                            D3DVSD_REG(3, D3DVSDT_FLOAT3), D3DVSD_REG(7, D3DVSDT_FLOAT2),
                                            D3DVSD_END()};
    static const DWORD kDeclNormalTex2[] = {D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
                                            D3DVSD_REG(3, D3DVSDT_FLOAT3), D3DVSD_REG(7, D3DVSDT_FLOAT2),
                                            D3DVSD_REG(8, D3DVSDT_FLOAT2), D3DVSD_END()};
    static const DWORD kDeclColorTex1[] = {D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
                                           D3DVSD_REG(5, D3DVSDT_D3DCOLOR), D3DVSD_REG(7, D3DVSDT_FLOAT2),
                                           D3DVSD_END()};
    static const DWORD kDeclColorTex2[] = {D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
                                           D3DVSD_REG(5, D3DVSDT_D3DCOLOR), D3DVSD_REG(7, D3DVSDT_FLOAT2),
                                           D3DVSD_REG(8, D3DVSDT_FLOAT2), D3DVSD_END()};
    static const DWORD kDeclFarPlane[] = {D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3),
                                          D3DVSD_REG(1, D3DVSDT_D3DCOLOR), D3DVSD_REG(2, D3DVSDT_FLOAT2),
                                          D3DVSD_END()};
    // vs.1.0: dp4 oPos.x, v0, c0 / dp4 oPos.y, v0, c1 / dp4 oPos.z, v0, c3 / dp4 oPos.w, v0, c3 /
    // mov oD0, v1 (0x100a7810).
    static const DWORD kFarPlaneCode[] = {0xfffe0100, 0x9, 0xc0010000, 0x90e40000, 0xa0e40000, 0x9, 0xc0020000,
                                          0x90e40000, 0xa0e40001, 0x9, 0xc0040000, 0x90e40000, 0xa0e40003,
                                          0x9, 0xc0080000, 0x90e40000, 0xa0e40003, 0x1, 0xd00f0000, 0x90e40001,
                                          0xffff};
    const DWORD swUsage = (g_caps & IGCAPS_HW_TNL) ? 0 : D3DUSAGE_SOFTWAREPROCESSING;
    const Shader shaders[] = {
        {kDeclNormalTex1, nullptr, &g_vsNormalTex1, 0x7d4},
        {kDeclNormalTex2, nullptr, &g_vsNormalTex2, 0x7de},
        {kDeclColorTex1, nullptr, &g_vsColorTex1, 0x809},
        {kDeclColorTex2, nullptr, &g_vsColorTex2, 0x813},
        {kDeclFarPlane, kFarPlaneCode, &g_vsFarPlane, 0x81d},
    };
    for (const Shader &s : shaders) {
        const DWORD usage = s.function ? ((g_caps & IGCAPS_HW_VERTEX_SHADERS) ? 0 : D3DUSAGE_SOFTWAREPROCESSING)
                                       : swUsage;
        const HRESULT hr = g_pDevice->CreateVertexShader(s.decl, s.function, s.handle, usage);
        if (hr < 0) {
            LogDxError(hr, s.line);
            return I3DERR_GENERIC;
        }
    }
    return 0;
}

}  // namespace ls3d::ig

// 0x1006c270
HRESULT __stdcall IGraph::SetViewport(const S_IGraphViewport *viewport)
{
    std::memcpy(&g_viewport, viewport, sizeof g_viewport);
    return g_pDevice->SetViewport(&g_viewport) < 0 ? I3DERR_GENERIC : 0;
}

// 0x1006c2b0
const S_IGraphViewport *__stdcall IGraph::GetViewport()
{
    return reinterpret_cast<const S_IGraphViewport *>(&g_viewport);
}

// 0x1006c2c0
HRESULT __stdcall IGraph::SetFlags(uint32_t flags)
{
    g_init.flags = flags;
    return 0;
}

// The display mode list order (0x1006d9a0): width, height, then format descending.
static int __cdecl CompareModes(const void *a, const void *b)
{
    const D3DDISPLAYMODE *m = static_cast<const D3DDISPLAYMODE *>(a);
    const D3DDISPLAYMODE *n = static_cast<const D3DDISPLAYMODE *>(b);
    if (m->Width < n->Width)
        return -1;
    if (n->Width < m->Width)
        return 1;
    if (m->Height < n->Height)
        return -1;
    if (n->Height < m->Height)
        return 1;
    if (static_cast<int>(n->Format) < static_cast<int>(m->Format))
        return -1;
    return static_cast<int>(m->Format) < static_cast<int>(n->Format);
}

// The first of the 16-bit texture formats the device supports (Init looks twice).
static bool Find16BitTextureFormat(D3DFORMAT display)
{
    static const D3DFORMAT kFormats[] = {D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5};
    for (D3DFORMAT f : kFormats) {
        if (g_pD3D->CheckDeviceFormat(g_init.adapter, g_deviceType, display, 0, D3DRTYPE_TEXTURE, f) >= 0) {
            g_fmt16Bit = f;
            return true;
        }
    }
    return false;
}

using Direct3DCreate8Fn = IDirect3D8 *(__stdcall *)(UINT);

// 0x1006c2d0
HRESULT __stdcall IGraph::Init(const S_IGraphInit *init)
{
    CoInitialize(nullptr);
    // dpnhpast.dll comes with DirectX 8.1.
    if (HMODULE probe = LoadLibraryA("dpnhpast.dll")) {
        FreeLibrary(probe);
    } else {
        dbgPrintf("ERROR! DirectX version is lower than 8.1 !");
        return I3DERR_UNSUPPORTED;
    }
    if (!init)
        return I3DERR_INVALIDPARAMS;
    g_init = *init;
    // ReportError reads the callback from the init block (0x100d1d4c = S_IGraphInit::errorCallback).
    g_errorCallback = g_init.errorCallback;
    Close();

    std::memset(i3d::g_renderStats, 0, sizeof i3d::g_renderStats);
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = IGraphWndProc;
    wc.hInstance = g_init.instance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExA(&wc)) {
        dbgPrintf("IGraph::Init() - Error register window class !!");
        return I3DERR_GENERIC;
    }

    if (!(g_init.flags & IG_FULLSCREEN)) {
        const int frameX = GetSystemMetrics(SM_CXFIXEDFRAME);
        const int frameY = GetSystemMetrics(SM_CYFIXEDFRAME);
        const int caption = GetSystemMetrics(SM_CYCAPTION);
        int menu = 0;
        if (g_init.flags & IG_MENU) {
            if (!g_init.menu)
                g_init.menu = LoadMenuA(GetModuleHandleA("LS3D.DLL"), MAKEINTRESOURCEA(123));
            menu = GetSystemMetrics(SM_CYMENU);
        }
        g_mainWnd = CreateWindowExA(0, kWindowClass, "LS3D engine",
                                    WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    g_init.x - frameX, g_init.y - menu - caption - frameY,
                                    g_init.width + frameX * 2, g_init.height + frameY * 2 + menu + caption,
                                    GetDesktopWindow(), g_init.menu, g_init.instance, nullptr);
        if (!g_mainWnd) {
            dbgPrintf("IGraph::Init() - Error create m_MainHWnd !!");
            return I3DERR_GENERIC;
        }
        UpdateWindow(g_mainWnd);
        if (g_init.flags & IG_CHILD_WINDOW) {
            g_childWnd = CreateWindowExA(0, kWindowClass, "LS3D engine", WS_CHILD | WS_VISIBLE, g_init.childX,
                                         g_init.childY, g_init.childWidth, g_init.childHeight, g_mainWnd,
                                         nullptr, g_init.instance, nullptr);
            if (!g_childWnd) {
                dbgPrintf("IGraph::Init() - Error create m_ChildHWnd !!");
                return I3DERR_GENERIC;
            }
            UpdateWindow(g_childWnd);
        }
    } else {
        g_init.flags &= ~IG_CHILD_WINDOW;
        SetRect(reinterpret_cast<RECT *>(&g_init.childX), 0, 0, 0, 0);
        g_init.flags &= ~IG_MENU;
        g_init.x = 0;
        g_init.y = 0;
        g_init.menu = nullptr;
        g_mainWnd = CreateWindowExA(WS_EX_TOPMOST, kWindowClass, "LS3D engine", WS_VISIBLE, g_init.x, g_init.y,
                                    g_init.width, g_init.height, GetDesktopWindow(), nullptr, g_init.instance,
                                    nullptr);
        if (!g_mainWnd)
            return I3DERR_GENERIC;
        UpdateWindow(g_mainWnd);
    }
    SetFocus(g_mainWnd);

    // The original links d3d8.lib; the port binds Direct3DCreate8 at run time (the SDK has no
    // Direct3D 8 import library any more).
    if (HMODULE d3d8 = LoadLibraryA("d3d8.dll")) {
        if (auto create = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8, "Direct3DCreate8")))
            g_pD3D = create(D3D_SDK_VERSION);
    }
    if (!g_pD3D) {
        dbgPrintf("IGraph::Init() - Error create D3D8 object !!");
        return I3DERR_GENERIC;
    }

    // Display modes without refresh rates, newest enumeration index first; and their formats.
    for (UINT i = g_pD3D->GetAdapterModeCount(g_init.adapter); i--;) {
        D3DDISPLAYMODE mode;
        g_pD3D->EnumAdapterModes(g_init.adapter, i, &mode);
        bool known = false;
        for (int k = g_displayModes.Size(); k--;) {
            const D3DDISPLAYMODE &m = g_displayModes[k];
            if (mode.Width == m.Width && mode.Height == m.Height && mode.Format == m.Format) {
                known = true;
                break;
            }
        }
        if (!known) {
            mode.RefreshRate = 0;
            g_displayModes.PushBack(mode);
        }
        bool knownFormat = false;
        for (int k = g_displayFormats.Size(); k--;) {
            if (mode.Format == g_displayFormats[k]) {
                knownFormat = true;
                break;
            }
        }
        if (!knownFormat)
            g_displayFormats.PushBack(mode.Format);
    }
    std::qsort(g_displayModes.Data(), g_displayModes.Size(), sizeof(D3DDISPLAYMODE), CompareModes);

    S_cpuInfo cpu = {};
    DetectCpu(&cpu);
    OSVERSIONINFOA os = {};
    os.dwOSVersionInfoSize = sizeof os;
#pragma warning(suppress : 4996)  // GetVersionExA is deprecated (and lies about newer Windows)
    GetVersionExA(&os);
    const char *osName = "Windows ??";
    if (os.dwMajorVersion == 3) {
        if (os.dwMinorVersion == 0)
            osName = "Windows NT 3.0";
        else if (os.dwMinorVersion == 1)
            osName = "Windows 3.1";
    } else if (os.dwMajorVersion == 4) {
        osName = os.dwMinorVersion == 0 ? "Windows NT 4.0" : "Windows 9x/ME";
    } else if (os.dwMajorVersion == 5) {
        if (os.dwMinorVersion == 0)
            osName = "Windows 2000";
        else if (os.dwMinorVersion == 1)
            osName = "Windows XP";
    }
    dbgPrintf("\nDetected OS: %s ver. %i.%i.%i %s", osName, os.dwMajorVersion, os.dwMinorVersion,
              os.dwBuildNumber & 0xffff, os.szCSDVersion);
    const uint32_t mhz = MeasureCpuMhz();
    dbgPrintf("Detected CPU: %s, %s %iMHz, family:%i, model:%i, stepping:%i", cpu.vendor, cpu.name, mhz,
              cpu.family, cpu.model, cpu.stepping);
    if (g_init.flags & IG_VERBOSE) {
        dbgPrintf("All usable display modes:");
        for (int i = g_displayModes.Size(); i--;) {
            const D3DDISPLAYMODE &m = g_displayModes[i];
            dbgPrintf("   Width: %4i, Height: %4i, Format: %s, Refresh: %i", m.Width, m.Height,
                      GetFormatName(m.Format), m.RefreshRate);
        }
        dbgPrintf("");
    }

    D3DDISPLAYMODE desktop;
    HRESULT hr = g_pD3D->GetAdapterDisplayMode(g_init.adapter, &desktop);
    if (hr < 0) {
        LogDxError(hr, 0x9a7);
        return I3DERR_GENERIC;
    }
    D3DADAPTER_IDENTIFIER8 id;
    hr = g_pD3D->GetAdapterIdentifier(g_init.adapter, D3DENUM_NO_WHQL_LEVEL, &id);
    if (hr < 0) {
        LogDxError(hr, 0x9ab);
        return I3DERR_GENERIC;
    }
    dbgPrintf("Current display mode: %ix%i, %iHz, format: %s \n", desktop.Width, desktop.Height,
              desktop.RefreshRate, GetFormatName(desktop.Format));
    dbgPrintf("Using adapter number: %i", g_init.adapter);
    dbgPrintf("              Driver: %s", id.Driver);
    dbgPrintf("         Description: %s", id.Description);
    dbgPrintf("      Driver version: %d.%02d.%02d.%04d", HIWORD(id.DriverVersion.HighPart),
              LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart));
    dbgPrintf("           Vendor ID: %lX", id.VendorId);
    dbgPrintf("           Device ID: %lX", id.DeviceId);
    dbgPrintf("           SubSys ID: %lX", id.SubSysId);
    dbgPrintf("            Revision: %lX", id.Revision);
    LogGuid(&id.DeviceIdentifier);
    dbgPrintf("");

    D3DCAPS8 caps;
    hr = g_pD3D->GetDeviceCaps(g_init.adapter, g_deviceType, &caps);
    if (hr < 0) {
        LogDxError(hr, 0x9c5);
        return I3DERR_GENERIC;
    }
    if (caps.MaxSimultaneousTextures < 2) {
        g_quirks |= IGQ_LIMITED | IGQ_NO_DXT;
        dbgPrintf("This engine can not run without HARDWARE MULTITEXTURING.");
    }
    if (caps.MaxTextureWidth == 256)
        g_quirks |= IGQ_LIMITED | IGQ_NO_DXT;
    switch (id.VendorId) {
    case 0x121a:                                    // 3dfx Voodoo3
        if (id.DeviceId == 9)
            g_quirks |= IGQ_LIMITED | IGQ_NO_DXT;
        break;
    case 0x5333:                                    // S3 Savage
        if (id.DeviceId == 0x8a22 || id.DeviceId == 0x8c10)
            g_quirks |= IGQ_LIMITED | IGQ_NO_BLENDCURRENTALPHA | IGQ_NO_BLENDFACTORALPHA | IGQ_NO_DXT;
        break;
    case 0x102b:                                    // Matrox
        switch (id.DeviceId) {
        case 0x521: case 0x520: case 0x1000: case 0x1001: case 0x51f: case 0x51b: case 0x51a: case 0x519:
            g_quirks |= IGQ_LIMITED | IGQ_MATROX | IGQ_NO_DXT;
            break;
        }
        if (id.DeviceId == 0x525)
            g_quirks |= IGQ_LIMITED;
        break;
    case 0x1002:                                    // ATI
        g_quirks |= IGQ_ATI;
        break;
    }
    if ((g_init.flags & IG_HW_TNL) && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
        g_caps |= IGCAPS_HW_TNL;
        dbgPrintf("Using HARDWARE T&L.");
        if ((caps.VertexShaderVersion & 0xff00) >= 0x100) {
            g_caps |= IGCAPS_HW_VERTEX_SHADERS;
            dbgPrintf("Using HARDWARE VERTEX SHADERS.");
        }
    }
    if (!(caps.TextureOpCaps & D3DTEXOPCAPS_BLENDCURRENTALPHA))
        g_quirks |= IGQ_NO_BLENDCURRENTALPHA;
    if (!(caps.TextureOpCaps & D3DTEXOPCAPS_BLENDFACTORALPHA))
        g_quirks |= IGQ_NO_BLENDFACTORALPHA;
    if (!(caps.TextureOpCaps & D3DTEXOPCAPS_ADD))
        g_quirks |= IGQ_NO_TEXOP_ADD;
    g_maxTextureWidth = static_cast<int>(caps.MaxTextureWidth);
    g_maxTextureHeight = static_cast<int>(caps.MaxTextureHeight);

    std::memset(&g_presentParams, 0, sizeof g_presentParams);
    if (!(g_init.flags & IG_FULLSCREEN)) {
        g_presentParams.Windowed = TRUE;
        g_presentParams.BackBufferFormat = desktop.Format;
        SetBitDepthFromFormat(desktop.Format);
        g_presentParams.hDeviceWindow = (g_init.flags & IG_CHILD_WINDOW) ? g_childWnd : g_mainWnd;
    } else {
        g_bitDepth = g_init.bitDepth;
        g_presentParams.FullScreen_RefreshRateInHz = g_init.refreshRate;
        g_presentParams.FullScreen_PresentationInterval =
            (g_init.flags & IG_VSYNC) ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
        g_presentParams.BackBufferWidth = g_init.width;
        g_presentParams.BackBufferHeight = g_init.height;
        g_presentParams.BackBufferCount = (g_init.flags & IG_TRIPLE_BUFFER) ? 2 : 1;
        // The display mode of the requested size and colour depth, from the end of the sorted list.
        for (int i = g_displayModes.Size(); i--;) {
            const D3DDISPLAYMODE &m = g_displayModes[i];
            if (static_cast<int>(m.Width) != g_init.width || static_cast<int>(m.Height) != g_init.height)
                continue;
            const bool match = g_bitDepth == 16 ? Is16BitFormat(m.Format)
                                                : g_bitDepth == 32 && Is32BitFormat(m.Format);
            if (match) {
                g_presentParams.BackBufferFormat = m.Format;
                break;
            }
        }
        if (!g_presentParams.BackBufferFormat) {
            dbgPrintf("IGraph::Init() - Error. Useful BackBuffer format not found !!");
            return I3DERR_GENERIC;
        }
    }
    g_presentParams.EnableAutoDepthStencil = TRUE;
    hr = ChooseDepthFormat();
    if (hr < 0)
        return hr;

    D3DMULTISAMPLE_TYPE samples = D3DMULTISAMPLE_NONE;
    if (g_init.flags & (IG_FSAA2 | IG_FSAA4)) {
        if (g_init.flags & IG_FSAA2) {
            if (g_init.flags & IG_VERBOSE)
                dbgPrintf("Trying to use 2 samples Full Scene AntiAliasing ...");
            samples = D3DMULTISAMPLE_2_SAMPLES;
        } else {
            if (g_init.flags & IG_VERBOSE)
                dbgPrintf("Trying to use 4 samples Full Scene AntiAliasing ...");
            samples = D3DMULTISAMPLE_4_SAMPLES;
        }
        if (g_pD3D->CheckDeviceMultiSampleType(g_init.adapter, D3DDEVTYPE_HAL, g_presentParams.BackBufferFormat,
                                               g_presentParams.Windowed, samples) < 0) {
            if (g_init.flags & IG_VERBOSE)
                dbgPrintf("ERROR... cannot use FSAA with current backbuffer format !\n");
            g_init.flags &= ~(IG_FSAA2 | IG_FSAA4);
            samples = D3DMULTISAMPLE_NONE;
        } else if (g_init.flags & IG_VERBOSE) {
            dbgPrintf("OK... FSAA enabled.\n");
        }
    }
    g_presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_presentParams.MultiSampleType = samples;
    const char *depthName = GetFormatName(g_presentParams.AutoDepthStencilFormat);
    const char *backName = GetFormatName(g_presentParams.BackBufferFormat);
    if (g_presentParams.Windowed) {
        const bool child = (g_init.flags & IG_CHILD_WINDOW) != 0;
        dbgPrintf("Trying to create device with WINDOWED mode: %ix%i, %iHz, BackFormat: %s, DepthFormat: %s",
                  child ? g_init.childWidth : g_init.width, child ? g_init.childHeight : g_init.height,
                  desktop.RefreshRate, backName, depthName);
    } else {
        dbgPrintf("Trying to create device with FULLSCREEN mode: %ix%i, %iHz, BackFormat: %s, DepthFormat: %s",
                  g_presentParams.BackBufferWidth, g_presentParams.BackBufferHeight,
                  g_presentParams.FullScreen_RefreshRateInHz, backName, depthName);
    }
    if (g_init.flags & IG_REFERENCE) {
        dbgPrintf("\nIGraph::Init() - Warning. SLOW REFERENCE RASTERIZER used !!\n");
        g_deviceType = D3DDEVTYPE_REF;
    }
    // No D3DCREATE_FPU_PRESERVE: Direct3D switches the FPU to single precision, which the engine's
    // float results depend on. With hardware T&L the device is MIXED ((caps & 1 ? 0x60 : 0) +
    // 0x20): the engine's index and vertex buffers are all D3DUSAGE_SOFTWAREPROCESSING, which a
    // HARDWARE_VERTEXPROCESSING device refuses (D3DERR_INVALIDCALL).
    hr = g_pD3D->CreateDevice(g_init.adapter, g_deviceType, g_mainWnd,
                              ((g_caps & IGCAPS_HW_TNL) ? D3DCREATE_MIXED_VERTEXPROCESSING
                                                        : D3DCREATE_SOFTWARE_VERTEXPROCESSING) |
                                  D3DCREATE_MULTITHREADED,
                              &g_presentParams, &g_pDevice);
    if (hr < 0) {
        LogDxError(hr, 0xb28);
        return I3DERR_GENERIC;
    }
    for (DWORD stage = 0; stage < 2; ++stage) {
        g_pDevice->SetTextureStageState(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
    }
    g_pDevice->SetRenderState(D3DRS_DITHERENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    if (g_init.flags & (IG_FSAA2 | IG_FSAA4))
        g_pDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    if (!(g_caps & IGCAPS_HW_TNL))
        g_pDevice->SetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, TRUE);
    g_pDevice->SetTextureStageState(0, D3DTSS_MAXANISOTROPY, 0);
    // (Port enhancement, LS3DF.ini AnisotropicFiltering: linear minification is anisotropic, up to
    // the option's degree or the device's. I3D_driver::SetState(0) applies it after a reset.)
    g_anisotropy = 0;
    if (const int want = Enhancements().anisotropy) {
        const int most = static_cast<int>(caps.MaxAnisotropy);
        if ((caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) && most > 1)
            g_anisotropy = want < most ? want : most;
        if (g_anisotropy) {
            for (DWORD stage = 0; stage < 3; ++stage) {
                g_pDevice->SetTextureStageState(stage, D3DTSS_MAXANISOTROPY, static_cast<DWORD>(g_anisotropy));
                if (stage < 2)
                    g_pDevice->SetTextureStageState(stage, D3DTSS_MINFILTER, D3DTEXF_ANISOTROPIC);
            }
        }
    }
    g_pDevice->SetRenderState(D3DRS_ALPHAREF, g_rs.alphaRef);
    RestoreDevice(false);

    hr = g_pD3D->GetAdapterDisplayMode(g_init.adapter, &desktop);
    if (hr < 0) {
        LogDxError(hr, 0xb4c);
        return I3DERR_GENERIC;
    }
    g_viewport.X = 0;
    g_viewport.Width = ScrnX();
    g_viewport.Y = 0;
    g_viewport.Height = ScrnY();
    g_viewport.MinZ = 0.0f;
    g_viewport.MaxZ = 1.0f;

    // Texture formats, checked against the desktop format.
    const D3DFORMAT display = desktop.Format;
    auto supported = [&](D3DFORMAT f) {
        return g_pD3D->CheckDeviceFormat(g_init.adapter, g_deviceType, display, 0, D3DRTYPE_TEXTURE, f) >= 0;
    };
    if (!Find16BitTextureFormat(display))
        dbgPrintf("IGraph::Init() - Error. Useful 16bit texture format not found !!");
    if (!Find16BitTextureFormat(display)) {
        dbgPrintf("IGraph::Init() - Error. Useful 16bit texture format not found !!");
        return I3DERR_BADTEXTUREFORMAT;
    }
    if (!supported(D3DFMT_A1R5G5B5)) {
        dbgPrintf("IGraph::Init() - Error. Useful 1bit Alpha texture format not found !!");
        return I3DERR_BADTEXTUREFORMAT;
    }
    g_fmt1BitAlpha = D3DFMT_A1R5G5B5;
    if (supported(D3DFMT_A4R4G4B4)) {
        g_fmtAlpha = D3DFMT_A4R4G4B4;
    } else if (supported(D3DFMT_A8R8G8B8)) {
        g_fmtAlpha = D3DFMT_A8R8G8B8;
    } else {
        dbgPrintf("IGraph::Init() - Error. Useful Alpha texture format not found !!");
        return I3DERR_BADTEXTUREFORMAT;
    }
    if (supported(D3DFMT_A8R8G8B8))
        g_fmtTrueColor = D3DFMT_A8R8G8B8;
    if (!(g_quirks & IGQ_NO_DXT)) {
        if (supported(D3DFMT_DXT1)) {
            g_caps |= IGCAPS_DXT;
            g_fmtDxt1 = D3DFMT_DXT1;
        }
        if (supported(D3DFMT_DXT3)) {
            g_caps |= IGCAPS_DXT;
            g_fmtDxt3 = D3DFMT_DXT3;
        }
        if (supported(D3DFMT_DXT5)) {
            g_caps |= IGCAPS_DXT;
            g_fmtDxt5 = D3DFMT_DXT5;
        }
    }
    if (supported(D3DFMT_V8U8)) {
        g_caps |= IGCAPS_BUMPMAP;
        g_fmtBump = D3DFMT_V8U8;
    }
    auto nameOrNone = [&](D3DFORMAT f) { return GetFormatName(f > 0 ? f : static_cast<D3DFORMAT>(-1)); };
    dbgPrintf("\nMaximum available texture memory ( AGP + VRAM ): %i MB", g_pDevice->GetAvailableTextureMem() >> 20);
    dbgPrintf("\nUsing texture formats:");
    dbgPrintf("   16bit textures: %s", GetFormatName(g_fmt16Bit));
    dbgPrintf("   1bit Alpha textures: %s", GetFormatName(g_fmt1BitAlpha));
    dbgPrintf("   Alpha textures: %s", GetFormatName(g_fmtAlpha));
    dbgPrintf("   TrueColor textures: %s", nameOrNone(g_fmtTrueColor));
    dbgPrintf("   DXT1 textures: %s", nameOrNone(g_fmtDxt1));
    dbgPrintf("   DXT3 textures: %s", nameOrNone(g_fmtDxt3));
    dbgPrintf("   DXT5 textures: %s", nameOrNone(g_fmtDxt5));
    dbgPrintf("   Bump-maps: %s", nameOrNone(g_fmtBump));
    dbgPrintf("\n");
    {
        const S_enhancements &e = Enhancements();
        if (e.anisotropy || e.trilinear || e.mipmaps || e.trueColorTextures || e.trueColorLightmaps) {
            EnhancementLog("Port enhancements (LS3DF.ini):");
            if (e.anisotropy)
                EnhancementLog("   Anisotropic filtering: %ix asked, %ix used (device maximum %lu%s)", e.anisotropy,
                               g_anisotropy, caps.MaxAnisotropy,
                               (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) ? "" : ", not supported");
            if (e.trilinear)
                EnhancementLog("   Trilinear filtering: always");
            if (e.mipmaps)
                EnhancementLog("   Mip maps: gamma-correct, alpha-weighted, coverage kept");
            if (e.trueColorTextures)
                EnhancementLog("   Textures from image files: %s", g_fmtTrueColor ? "A8R8G8B8" : "unchanged (no A8R8G8B8)");
            if (e.trueColorLightmaps)
                EnhancementLog("   Lightmaps: %s", g_fmtTrueColor ? "32-bit" : "unchanged (no A8R8G8B8)");
            EnhancementLog("");
        }
    }

    hr = InitDirectInput(g_init.instance);
    if (hr < 0) {
        LogDxError(hr, 0xbd7);
        return I3DERR_GENERIC;
    }
    SetSwapMouseButtons(GetSystemMetrics(SM_SWAPBUTTON) != 0);

    // Sticky keys off while the engine runs (Close restores the saved state). The original switches
    // filter and toggle keys "off" only when sticky keys had flags, and never sets their structure
    // sizes, so those calls fail.
    g_stickyKeys.cbSize = sizeof g_stickyKeys;
    SystemParametersInfoA(SPI_GETSTICKYKEYS, sizeof g_stickyKeys, &g_stickyKeys, 0);
    const DWORD stickyFlags = g_stickyKeys.dwFlags;
    if (stickyFlags) {
        g_stickyKeys.dwFlags = 0;
        SystemParametersInfoA(SPI_SETSTICKYKEYS, g_stickyKeys.cbSize, &g_stickyKeys, 0);
        g_stickyKeys.dwFlags = stickyFlags;
    }
    SystemParametersInfoA(SPI_GETFILTERKEYS, g_filterKeys.cbSize, &g_filterKeys, 0);
    if (g_stickyKeys.dwFlags) {
        const DWORD saved = g_filterKeys.dwFlags;
        g_filterKeys.dwFlags = 0;
        SystemParametersInfoA(SPI_SETFILTERKEYS, g_filterKeys.cbSize, &g_filterKeys, 0);
        g_filterKeys.dwFlags = saved;
    }
    SystemParametersInfoA(SPI_GETTOGGLEKEYS, g_toggleKeys.cbSize, &g_toggleKeys, 0);
    if (g_stickyKeys.dwFlags) {
        const DWORD saved = g_toggleKeys.dwFlags;
        g_toggleKeys.dwFlags = 0;
        SystemParametersInfoA(SPI_SETTOGGLEKEYS, g_toggleKeys.cbSize, &g_toggleKeys, 0);
        g_toggleKeys.dwFlags = saved;
    }

    ResetDeviceStates();
    if (caps.Caps2 & D3DCAPS2_FULLSCREENGAMMA) {
        g_caps |= IGCAPS_GAMMA;
        D3DGAMMARAMP ramp;
        GetGammaRamp(ramp.red);
        // Drivers that report 8-bit values get them widened by GetGammaRamp.
        uint32_t caps8 = g_caps | IGCAPS_GAMMA_8BIT;
        for (int i = 256; i--;) {
            if (ramp.red[i] > 0xff || ramp.green[i] > 0xff || ramp.blue[i] > 0xff)
                caps8 &= ~IGCAPS_GAMMA_8BIT;
        }
        g_caps = caps8;
    }
    if (g_init.flags & IG_TEXTURE_THREAD) {
        tex::StreamInit(g_init.flags);
        g_textureThreadRun = true;
        DWORD threadId;
        g_textureThread = CreateThread(nullptr, 0, tex::StreamThread, nullptr, 0, &threadId);
        SetThreadPriority(g_textureThread, THREAD_PRIORITY_NORMAL);
    }
    Clear(0, 1.0f, 3);
    Flip();
    Clear(0, 1.0f, 3);
    Flip();
    g_inScene = false;
    g_initialized = true;
    if (!_mkdir("Maps"))
        dbgPrintf("Directory '\\Maps' was successfully created.\n");

    g_defaultTexture = CreateITexture();
    if (g_defaultTexture->Create(nullptr, nullptr, ITC_EMPTY, 8, 8, 0) >= 0)
        g_defaultTexture->Fill(nullptr, 0xffffffff, 0);
    return 0;
}

// 0x1006da20
bool __stdcall IGraph::IsInitialized()
{
    return g_initialized;
}

// 0x1006da30
const S_IGraphInit *__stdcall IGraph::GetInitData()
{
    return &g_init;
}

// 0x1006da40
HRESULT __stdcall IGraph::Close()
{
    if (g_defaultTexture) {
        g_defaultTexture->Release();
        g_defaultTexture = nullptr;
    }
    for (ITexture *&texture : g_bumpAnimTextures) {
        if (texture) {
            texture->Release();
            texture = nullptr;
        }
    }
    ClipCursor(nullptr);
    if ((g_init.flags & IG_TEXTURE_THREAD) && g_textureThreadRun) {
        g_textureThreadRun = false;
        WaitForSingleObject(g_textureThread, INFINITE);
        CloseHandle(g_textureThread);
    }
    SetSwapMouseButtons(false);
    SystemParametersInfoA(SPI_SETSTICKYKEYS, g_stickyKeys.cbSize, &g_stickyKeys, 0);
    SystemParametersInfoA(SPI_SETFILTERKEYS, g_filterKeys.cbSize, &g_filterKeys, 0);
    SystemParametersInfoA(SPI_SETTOGGLEKEYS, g_toggleKeys.cbSize, &g_toggleKeys, 0);
    g_vbManager.FreeUnused();
    g_ibManager.FreeUnused();
    g_vbManager.FreeAll();
    g_ibManager.FreeAll();
    DelAllDirs();

    if (g_shows.Size() > 0) {
        dbgPrintf("\nWARNING! Unreleased IShows: %i", g_shows.Size());
        for (int i = g_shows.Size(); i--;) {
            dbgPrintf("   0x%x, Refs:%d", g_shows[i], g_shows[i]->refCount);
            ReportError(I3DERR_UNRELEASED_SHOW, kSevWarning, nullptr, reinterpret_cast<uintptr_t>(g_shows[i]));
        }
        g_shows.Free();
    }
    if (g_cachedTextures.Size() > 0) {
        dbgPrintf("\nWARNING! Releasing unreleased CACHED textures: %d", g_cachedTextures.Size());
        for (int i = g_cachedTextures.Size(); i--;) {
            ITexture *texture = g_cachedTextures[i];
            dbgPrintf("   %s, %s, Refs:%d", texture->fileName, texture->alphaFileName, texture->refCount);
            ReportError(I3DERR_UNRELEASED_TEXTURE, kSevWarning, nullptr, reinterpret_cast<uintptr_t>(texture));
            // The last Release removes the texture from the list.
            while (g_cachedTextures[i]->Release()) {
            }
        }
        g_cachedTextures.Free();
    }
    if (g_uncachedTextures.Size() > 0) {
        dbgPrintf("\nWARNING! Releasing unreleased UNCACHED textures: %d", g_uncachedTextures.Size());
        for (int i = g_uncachedTextures.Size(); i--;) {
            ITexture *texture = g_uncachedTextures[i];
            ReportError(I3DERR_UNRELEASED_TEXTURE, kSevWarning, nullptr, reinterpret_cast<uintptr_t>(texture));
            dbgPrintf("   %s, %s, Refs:%d", texture->fileName, texture->alphaFileName, texture->refCount);
            while (g_uncachedTextures[i]->Release()) {
            }
        }
        g_uncachedTextures.Free();
    }
    g_fmtDxt1 = D3DFMT_UNKNOWN;
    g_fmtDxt3 = D3DFMT_UNKNOWN;
    g_fmtDxt5 = D3DFMT_UNKNOWN;
    g_fmt16Bit = D3DFMT_UNKNOWN;
    g_fmtAlpha = D3DFMT_UNKNOWN;
    g_fmt1BitAlpha = D3DFMT_UNKNOWN;
    g_fmtTrueColor = D3DFMT_UNKNOWN;
    g_displayModes.Free();
    g_displayFormats.Free();
    CloseKeyboard();
    CloseMouse();
    std::memset(&g_presentParams, 0, sizeof g_presentParams);
    std::memset(&g_viewport, 0, sizeof g_viewport);
    g_caps = 0;
    g_quirks = 0;
    // (The original also sets an otherwise unused global, 0x100d188c, to 4 here.)
    g_maxTextureWidth = 0;
    g_maxTextureHeight = 0;
    ReleaseDirectInput();
    if (g_pDevice) {
        if (g_init.flags & IG_TEXTURE_THREAD) {
            TerminateThread(g_textureThread, 0);
            tex::StreamFlush();
        }
        g_textureThread = nullptr;
        g_pDevice->Release();
        g_pDevice = nullptr;
    }
    if (g_pD3D) {
        g_pD3D->Release();
        g_pD3D = nullptr;
        CoUninitialize();
    }
    g_initialized = false;
    return 0;
}

// 0x1006de50
int __stdcall IGraph::ScrnX()
{
    return (g_init.flags & IG_CHILD_WINDOW) ? g_init.childWidth : g_init.width;
}

// 0x1006de70
int __stdcall IGraph::ScrnY()
{
    return (g_init.flags & IG_CHILD_WINDOW) ? g_init.childHeight : g_init.height;
}

// 0x1006de90
int __stdcall IGraph::GetBitDepth()
{
    return g_bitDepth;
}

// 0x1006deb0
uint32_t __stdcall IGraph::GetCaps()
{
    return g_caps;
}

// 0x1006dea0
D3DFORMAT __stdcall IGraph::GetBackBufferFormat()
{
    return g_presentParams.BackBufferFormat;
}

// 0x10057630
int __stdcall IGraph::ReservedD4()
{
    return 0;
}

// 0x1006f700
bool __stdcall IGraph::ReservedD8(int, int)
{
    return true;
}

// ---------------------------------------------------------------- texture lists, gamma

// 0x10070060
ITexture **__stdcall IGraph::GetCachedTextures()
{
    return g_cachedTextures.Data();
}

// 0x10070070
int __stdcall IGraph::NumCachedTextures()
{
    return g_cachedTextures.Size();
}

// 0x10070080
ITexture **__stdcall IGraph::GetUncachedTextures()
{
    return g_uncachedTextures.Data();
}

// 0x10070090
int __stdcall IGraph::NumUncachedTextures()
{
    return g_uncachedTextures.Size();
}

// 0x100700a0
bool __stdcall IGraph::SetGammaRamp(const uint16_t *ramp, bool calibrate)
{
    if (!(g_caps & IGCAPS_GAMMA))
        return false;
    g_pDevice->SetGammaRamp(calibrate ? D3DSGR_CALIBRATE : D3DSGR_NO_CALIBRATION,
                            reinterpret_cast<const D3DGAMMARAMP *>(ramp));
    return true;
}

// 0x100700e0
bool __stdcall IGraph::GetGammaRamp(uint16_t *ramp)
{
    if (!(g_caps & IGCAPS_GAMMA))
        return false;
    g_pDevice->GetGammaRamp(reinterpret_cast<D3DGAMMARAMP *>(ramp));
    if (g_caps & IGCAPS_GAMMA_8BIT) {
        for (int i = 0x300; i--;)
            ramp[i] = static_cast<uint16_t>(ramp[i] << 8 | ramp[i]);
    }
    return true;
}

// 0x10069e50 / 0x10069a00 run from the engine start-up (core/engine.cpp).
extern "C" IGraph *__cdecl GetIGraph()
{
    return g_pIGraph;
}
