// I3D_driver: construction, Init / Close, states, time, statistics, the frame factory and the
// type names (0x100151c0 - 0x10015c40, 0x100163c0, 0x10018e40 - 0x1001a0f0).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "core/enhance.h"
#include "i3d/area.h"
#include "i3d/billboard.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/dummy.h"
#include "i3d/emitter.h"
#include "i3d/i3d_hooks.h"
#include "i3d/i3d_links.h"
#include "i3d/joint.h"
#include "i3d/lens.h"
#include "i3d/light.h"
#include "i3d/litobject.h"
#include "i3d/material.h"
#include "i3d/mirror.h"
#include "i3d/model.h"
#include "i3d/morph.h"
#include "i3d/object.h"
#include "i3d/occluder.h"
#include "i3d/projector.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/shadow.h"
#include "i3d/singlemesh.h"
#include "i3d/sound.h"
#include "i3d/target.h"
#include "i3d/user.h"
#include "i3d/vb_manager.h"
#include "i3d/visual.h"
#include "i3d/volume.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;

namespace ls3d::i3d {

uint32_t g_driverStates[DRVSTATE_COUNT];
bool g_driverInitialized;
int g_driverTime;
uint32_t g_renderStats[8];
// Unused frames by type and visuals by index, reused by the factories (0x100aded8, 0x100adb10).
I3D_frame *g_frameFreeLists[17];
I3D_frame *g_visualFreeLists[16];
// The resource texture of the driver (0x100be404: bitmap 0x7D of LS3DF.dll, the font) and a
// list Close releases that nothing fills (0x100be380).
I3D_material *g_resourceMaterial;
C_vector<I3D_material *> g_driverMaterials;
// Driver state side values: the inverse of state 0x35 (0x100adb90), states 0x36 (the projectors'
// z bias) / 0x37 as used (0x100be018 / 0x100aded4: a quarter on ATI hardware).
float g_state35Inverse = 0.02f;
int g_projectorZBias;
int g_state37Value;
// Cleared by Init (0x100aded0).
int g_driverUnknownADED0;

bool UseWBuffer() { return g_driverStates[DRVSTATE_WBUFFER] != 0; }

bool LightmapsTrueColor()
{
    if (g_driverStates[DRVSTATE_LM_TRUECOLOR])
        return true;
    if (!Enhancements().trueColorLightmaps)
        return false;
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        EnhancementLog("TrueColorLightmaps: driver state 0x22 (32-bit lightmaps) is off, kept on");
    }
    return true;
}

void ReapplyDriverStates()
{
    for (int i = 0; i < DRVSTATE_COUNT; ++i) {
        const uint32_t value = g_driverStates[i];
        g_driverStates[i] = 0xffffffffu;
        g_driver->SetState(i, value);
    }
}

void DrawDebugText(int x, int y, const char *text) { g_driver->DrawText2D(x, y, text, 0, 1.0f); }

void DrawText2D(int x, int y, const char *text, uint32_t flags, float scale)
{
    g_driver->DrawText2D(x, y, text, flags, scale);
}

}  // namespace ls3d::i3d

namespace {

// 0x1005ed90: the particle buffers released, the blended items' overlap cache freed.
void ReleaseRenderBuffers()
{
    ReleaseParticleBuffers();
    g_alphaCacheSize = 0;
    AlignedFree(g_alphaCache);
    g_alphaCache = nullptr;
}

// 0x1005ed60: the same, then the particle buffers made again.
void ResetRenderBuffers()
{
    ReleaseRenderBuffers();
    InitParticleBuffers();
}

}  // namespace

// 0x100151c0
I3D_driver::I3D_driver() : unknown04(0), mirrorStamp(0) {}

// 0x10015210
I3D_driver::~I3D_driver()
{
    if (g_driverInitialized)
        dbgPrintf("!! I3D_driver was initialised, but not closed");
    FreeSkinBuffers();
    FreeProjectionBuffers();
}

// 0x100152c0
void I3D_driver::Tick(int time) { g_driverTime += time; }

// 0x100152e0
int I3D_driver::GetTime() { return g_driverTime; }

// 0x100152f0
I3D_RESULT I3D_driver::Init(const void *init)
{
    unknown04 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(init));
    std::memset(g_driverStates, 0, sizeof g_driverStates);
    g_driverTime = 0;
    g_driverStates[DRVSTATE_CLEAR] = 1;
    g_driverStates[DRVSTATE_CLEAR_Z] = 1;
    SetState(DRVSTATE_ZBUFFER, 1);
    g_driverStates[DRVSTATE_TEX_MIPMAP] = 1;
    g_driverStates[DRVSTATE_DRAW_VISUALS] = 1;
    g_driverStates[0x20] = 1;
    g_driverStates[0x1A] = 1;
    SetState(2, 2);
    SetState(0, 1);
    g_driverStates[1] = 1;
    SetState(7, 1);
    const uint32_t one = 0x3f800000;
    g_driverStates[DRVSTATE_LOD_SCALE] = one;
    g_driverStates[DRVSTATE_NEAR_LOD_SCALE] = one;
    g_driverStates[DRVSTATE_ENV_UV_SCALE] = one;
    g_driverStates[0x22] = 1;
    g_driverStates[0x23] = 1;
    g_driverStates[0x28] = 1;
    g_driverStates[DRVSTATE_TEX_DITHER] = 1;
    g_driverStates[DRVSTATE_OCCLUSION] = 1;
    g_driverStates[DRVSTATE_FOG] = 1;
    g_driverStates[DRVSTATE_TEX_COMPRESS] = 1;
    g_driverStates[DRVSTATE_SHADOWS] = 1;
    g_driverStates[0x2D] = 1;
    g_driverStates[0x30] = 1;
    g_driverStates[DRVSTATE_FRAME_NAMES] = 1;
    g_driverStates[DRVSTATE_PROJECTORS] = 1;
    g_projectorZBias = (ig::g_quirks & ig::IGQ_ATI) ? 2 : 8;
    g_driverStates[0x3B] = 1;
    g_driverStates[0x26] = 0xffffffffu;
    g_driverStates[0x35] = 50;
    g_state35Inverse = 0.02f;
    g_driverStates[0x36] = 8;
    g_driverStates[0x37] = 3;
    g_driverStates[0x3C] = 0x3a83126f;      // 0.001f
    g_driverStates[0x3D] = 0x3a83126f;
    g_driverStates[0x40] = 0x20;
    g_state37Value = (ig::g_quirks & ig::IGQ_ATI) ? 1 : 3;
    SetState(0x42, 0x1e);
    g_driverStates[0x44] = 0xffff7e00u;
    Close();
    std::memset(g_frameFreeLists, 0, sizeof g_frameFreeLists);
    std::memset(g_visualFreeLists, 0, sizeof g_visualFreeLists);
    g_driverInitialized = true;
    ResetRenderBuffers();
    g_lightmapMaterial = CreateMaterial();
    g_lightmapMaterial->SetDiffuse(S_vector(0.0f, 1.0f, 1.0f));
    g_lightmapMaterial->SetEmissive(S_vector(0.0f, 1.0f, 1.0f));
    g_lightmapMaterial->matFlags |= MATF_COLORS;
    g_nameRoot = nullptr;
    g_driverUnknownADED0 = 0;
    g_resourceMaterial = LoadResourceMaterial(MAKEINTRESOURCEA(0x7d));
    if (!(ig::g_init.flags & IG_VERBOSE))
        g_logDisabled = true;
    return I3D_OK;
}

// 0x10015560: every unused frame and visual deleted, the render queue's storage and the unused
// device buffers freed.
void I3D_driver::FreeUnusedResources()
{
    for (I3D_frame *&head : g_frameFreeLists) {
        for (I3D_frame *f = head; f;) {
            I3D_frame *next = f->nextSibling;
            delete f;
            f = next;
        }
    }
    std::memset(g_frameFreeLists, 0, sizeof g_frameFreeLists);
    for (int i = 0; i < kVisualTypeCount; ++i) {
        for (I3D_frame *f = g_visualFreeLists[i]; f;) {
            I3D_frame *next = f->nextSibling;
            delete f;
            f = next;
        }
    }
    std::memset(g_visualFreeLists, 0, sizeof g_visualFreeLists);
    g_rc.items.Free();
    ::operator delete(g_rc.keys);
    g_rc.keys = nullptr;
    g_rc.keyCapacity = 0;
    g_rc.numKeys = 0;
    g_vbManager.FreeUnused();
    g_ibManager.FreeUnused();
    FreeProjectionBuffers();
}

// 0x10015650
void I3D_driver::Close()
{
    FreeUnusedResources();
    ReleaseRenderBuffers();
    for (int i = 0; i < g_driverMaterials.Size(); ++i) {
        if (g_driverMaterials[i]) {
            g_driverMaterials[i]->Release();
            g_driverMaterials[i] = nullptr;
        }
    }
    g_driverMaterials.Free();
    if (g_lightmapMaterial) {
        g_lightmapMaterial->Release();
        g_lightmapMaterial = nullptr;
    }
    if (g_resourceMaterial) {
        g_resourceMaterial->Release();
        g_resourceMaterial = nullptr;
    }
    g_driverInitialized = false;
}

// 0x100156f0
I3D_RESULT I3D_driver::GetStats(int index, void *out)
{
    switch (index) {
    case 0:
        if (out) {
            std::memcpy(out, g_renderStats, sizeof g_renderStats);
            return I3D_OK;
        }
        break;
    case 1:
    case 4:
    case 6:
        return out ? I3DERR_UNSUPPORTED : I3DERR_INVALIDPARAMS;
    case 2:
    case 3:
    case 5:
        return I3DERR_UNSUPPORTED;
    default:
        dbgPrintf("!! Invalid index:%d  I3D_driver::GetStats()", index);
        break;
    }
    return I3DERR_INVALIDPARAMS;
}

// 0x10015770
I3D_RESULT I3D_driver::SetState(int state, uint32_t value)
{
    if (state > 0x44)
        return I3DERR_INVALIDPARAMS;
    auto filters = [](DWORD minFilter, DWORD magFilter) {
        for (DWORD stage = 0; stage < 3; ++stage) {
            g_pGfx->SetTextureStageState(stage, D3DTSS_MINFILTER, minFilter);
            g_pGfx->SetTextureStageState(stage, D3DTSS_MAGFILTER, magFilter);
            // (Port enhancement, LS3DF.ini AnisotropicFiltering; set again here after a device
            // reset cleared it.)
            if (minFilter == D3DTEXF_ANISOTROPIC)
                g_pGfx->SetTextureStageState(stage, D3DTSS_MAXANISOTROPY, static_cast<DWORD>(ig::g_anisotropy));
        }
    };
    switch (state) {
    case 0:
        filters(value ? ig::LinearMinFilter() : D3DTEXF_POINT, value ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        break;
    case 2:
        if (value <= 2) {
            // (Port enhancement, LS3DF.ini TrilinearFiltering: the levels always blended; the
            // state keeps the value asked for.)
            DWORD mipFilter = value;
            if (Enhancements().trilinear && mipFilter != D3DTEXF_LINEAR) {
                static bool s_logged;
                if (!s_logged) {
                    s_logged = true;
                    EnhancementLog("TrilinearFiltering: mip filter %u asked for, linear used", value);
                }
                mipFilter = D3DTEXF_LINEAR;
            }
            for (DWORD stage = 0; stage < 3; ++stage)
                g_pGfx->SetTextureStageState(stage, D3DTSS_MIPFILTER, mipFilter);
        }
        break;
    case 3:
        g_pGfx->SetRenderState(D3DRS_ZENABLE, value ? ig::g_zBufferType : 0);
        break;
    case 4:
        ig::g_zBufferType = value ? D3DZB_USEW : D3DZB_TRUE;
        if (g_driverStates[DRVSTATE_ZBUFFER])
            g_pGfx->SetRenderState(D3DRS_ZENABLE, ig::g_zBufferType);
        break;
    case 7:
        g_pGfx->SetRenderState(D3DRS_DITHERENABLE, value);
        break;
    case 8:
        g_pGfx->SetTextureStageState(0, D3DTSS_MIPMAPLODBIAS, value);
        g_pGfx->SetTextureStageState(1, D3DTSS_MIPMAPLODBIAS, value);
        break;
    case 9:
        g_pGfx->SetRenderState(D3DRS_FILLMODE, value ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
        break;
    case DRVSTATE_DRAW_VISUALS:
        if (value)
            g_forceCameraChanged = true;
        break;
    case 0x35:
        g_state35Inverse = 1.0f / static_cast<float>(static_cast<int>(value));
        break;
    case 0x36:
        g_projectorZBias = (ig::g_quirks & ig::IGQ_ATI) ? (static_cast<int>(value) + 3) / 4 : static_cast<int>(value);
        break;
    case 0x37:
        g_state37Value = (ig::g_quirks & ig::IGQ_ATI) ? (static_cast<int>(value) + 3) / 4 : static_cast<int>(value);
        break;
    case DRVSTATE_ENV_UV_SCALE:
        // (A magic value: the default pool vertex buffers released, the state unchanged.)
        if (value == 666) {
            g_vbManager.ReleaseDefaultPool();
            return I3D_OK;
        }
        break;
    case 0x42: {
        // The last LOD level fades out over the last `value` percent of its range.
        const float f = 1.0f - static_cast<float>(static_cast<int>(value)) * 0.01f;
        g_lodFadeScale = 1.0f / (1.0f - f * f);
        break;
    }
    default:
        break;
    }
    g_driverStates[state] = value;
    return I3D_OK;
}

// 0x10015c20
uint32_t I3D_driver::GetState(int state) { return state > 0x44 ? 0 : g_driverStates[state]; }

// 0x100163c0: a material whose texture is the bitmap resource `id` of LS3DF.dll.
I3D_material *I3D_driver::LoadResourceMaterial(const char *id)
{
    HMODULE module = GetModuleHandleA("LS3DF.dll");
    HRSRC info = FindResourceA(module, id, MAKEINTRESOURCEA(2));
    HGLOBAL data = LoadResource(module, info);
    if (!data) {
        dbgPrintf("ERROR LOADING RESOURCES !!!!");
        return nullptr;
    }
    void *bits = LockResource(data);
    I3D_material *m = CreateMaterial();
    if (!m) {
        FreeResource(data);
        return nullptr;
    }
    ITexture *t = g_pIGraph->CreateITexture();
    t->Create(static_cast<const char *>(bits), nullptr, ITC_FROM_MEMORY | ITC_ALPHA_1BIT | ITC_COMPRESS, 0, 0, 0);
    m->SetTexture(t);
    t->Release();
    m->matFlags |= MATF_ALPHA;
    FreeResource(data);
    return m;
}

// 0x10019e10
I3D_frame *I3D_driver::CreateFrame(int type) { return CreateFrameObject(type); }

// 0x10019e30
I3D_frame *I3D_driver::CreateVisual(uint32_t fourCC)
{
    const int index = VisualIndex(fourCC);
    if (index < 0)
        return nullptr;
    return CreateVisualObject(index);
}

// 0x10019e50
I3D_RESULT I3D_driver::EnumVisualTypes(I3D_VISUALTYPEPROC *proc, bool withShadow)
{
    if (!proc)
        return I3DERR_INVALIDPARAMS;
    proc(FourCC("BBRD"), "Billboard");
    proc(FourCC("LMAP"), "Lit-object");
    proc(FourCC("MRPH"), "Morph");
    proc(FourCC("OBJ_"), "Object");
    proc(FourCC("SNGM"), "Single-mesh");
    proc(FourCC("SMMR"), "Single-morph");
    proc(FourCC("LENS"), "Lensflare/Glow");
    proc(FourCC("PROJ"), "Projector");
    proc(FourCC("MIRR"), "Mirror");
    proc(FourCC("EMTR"), "Emitor");
    if (withShadow)
        proc(FourCC("SHD"), "Shadow");
    return I3D_OK;
}

// 0x10019f00
const char *I3D_driver::GetFrameTypeName(int type)
{
    static const char *const kNames[] = {
        "FRAME_VISUAL", "FRAME_LIGHT", "FRAME_CAMERA", "FRAME_SOUND", "FRAME_SECTOR", "FRAME_DUMMY",
        "FRAME_TARGET", "FRAME_USER", "FRAME_MODEL", "FRAME_JOINT", "FRAME_VOLUME", "FRAME_OCCLUDER",
        "FRAME_SCENE", "FRAME_AREA", "FRAME_SHADOW", "FRAME_LANDSCAPE",
    };
    if (type >= 1 && type <= 16)
        return kNames[type - 1];
    // (0x100adb50; the original passes no argument for the %x.)
    static char s_unknown[0x40];
    std::sprintf(s_unknown, "Unknown type:0x%x", type);
    return s_unknown;
}

// 0x1001a000
const char *I3D_driver::GetVisualTypeName(uint32_t fourCC)
{
    switch (fourCC) {
    case FourCC("LNDP"): return "I3D_VISUAL_LAND_PATCH";
    case FourCC("PROJ"): return "I3D_VISUAL_PROJECTOR";
    case FourCC("SHD"): return "I3D_VISUAL_SHADOW";
    case FourCC("BBRD"): return "I3D_VISUAL_BILLBOARD";
    case FourCC("MRPH"): return "I3D_VISUAL_MORPH";
    case FourCC("SNGM"): return "I3D_VISUAL_SINGLEMESH";
    case FourCC("LMAP"): return "I3D_VISUAL_LIT_OBJECT";
    case FourCC("EMTR"): return "I3D_VISUAL_EMITOR";
    case FourCC("SMMR"): return "I3D_VISUAL_SINGLEMORPH";
    case FourCC("MIRR"): return "I3D_VISUAL_MIRROR";
    case FourCC("LENS"): return "I3D_VISUAL_LENSF";
    case FourCC("OBJ_"): return "I3D_VISUAL_OBJECT";
    default:
        break;
    }
    // (0x100be398; the original formats the code and an empty string at 0x100be378.)
    static char s_unknown[0x40];
    char code[5] = {};
    std::memcpy(code, &fourCC, 4);
    std::sprintf(s_unknown, "Unknown type:0x%x  %s", fourCC, code);
    return s_unknown;
}

namespace ls3d::i3d {

// 0x10018e40
I3D_frame *CreateFrameObject(int type)
{
    // (The original reads the free list of any type; only 0 - 16 have one.)
    I3D_frame *f = (type >= 0 && type < 17) ? g_frameFreeLists[type] : nullptr;
    if (f) {
        g_frameFreeLists[type] = f->nextSibling;
    } else {
        switch (type) {
        case FRAME_LIGHT: f = new I3D_light; break;
        case FRAME_CAMERA: f = new I3D_camera; break;
        case FRAME_SOUND: f = new I3D_sound; break;
        case FRAME_SECTOR: f = new I3D_sector; break;
        case FRAME_DUMMY: f = new I3D_dummy; break;
        case FRAME_TARGET: f = new I3D_target; break;
        case FRAME_USER: f = new I3D_user; break;
        case FRAME_MODEL: f = new I3D_model; break;
        case FRAME_JOINT: f = new I3D_joint; break;
        case FRAME_VOLUME: f = new I3D_volume; break;
        case FRAME_OCCLUDER: f = new I3D_occluder; break;
        case FRAME_SCENE: f = new I3D_scene; break;
        case FRAME_AREA: f = new I3D_area; break;
        case FRAME_SHADOW: f = NewShadowFrame(); break;
        default:
            return nullptr;
        }
        if (!f) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return nullptr;
        }
        // (Stored as a dword: visualIndex 0.)
        f->type = static_cast<uint16_t>(type);
        f->visualIndex = 0;
    }
    f->refCount = 1;
    f->Init();
    return f;
}

// 0x10019750
I3D_frame *CreateVisualObject(int index)
{
    // (Fix: the original reads the free list of any index, past the 16 lists.)
    if (index < 0 || index >= kVisualTypeCount)
        return nullptr;
    I3D_frame *f = g_visualFreeLists[index];
    if (f) {
        g_visualFreeLists[index] = f->nextSibling;
    } else {
        switch (index) {
        case 0: f = new I3D_object; break;
        case 1: f = new I3D_lit_object; break;
        case 2: f = new I3D_singlemesh; break;
        case 3: f = new I3D_singlemorph; break;
        case 4: f = new I3D_billboard; break;
        case 5: f = new I3D_morph; break;
        case 6: f = new I3D_lens; break;
        case 7: f = new I3D_projector; break;
        case 8: f = new I3D_mirror; break;
        case 9: f = new I3D_emitor; break;
        case 11: f = new I3D_shadow_visual; break;
        default:                            // 10: LNDP, not in this engine
            return nullptr;
        }
        if (!f) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return nullptr;
        }
        f->visualIndex = static_cast<uint16_t>(index);
        f->type = FRAME_VISUAL;
    }
    f->refCount = 1;
    f->Init();
    return f;
}

// 0x10019dc0
void FreeFrameObject(I3D_frame *frame)
{
    const uint16_t type = frame->type;
    frame->Uninit();
    if (type == FRAME_VISUAL) {
        frame->nextSibling = g_visualFreeLists[frame->visualIndex];
        g_visualFreeLists[frame->visualIndex] = frame;
        return;
    }
    frame->nextSibling = g_frameFreeLists[type];
    g_frameFreeLists[type] = frame;
}

}  // namespace ls3d::i3d
