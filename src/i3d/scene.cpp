// I3D_scene (0x100462b0 - 0x1004c730; the destructor 0x100195d0).
#include "i3d/scene.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <new>

#include "core/debug.h"
#include "i3d/anim.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/light.h"
#include "i3d/portal.h"
#include "i3d/render_state.h"
#include "i3d/shadow.h"
#include "i3d/vb_manager.h"
#include "i3d/visual.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"
#include "isnd/eax.h"
#include "isnd/isnd.h"
#include "platform/platform.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;
using ig::g_rs;

namespace ls3d::i3d {

int g_sceneCount;
ITexture *g_snowTexture;
S_VBAlloc g_weatherVB;
S_IBAlloc g_weatherIB;
int g_weatherCapacity;
int g_weatherDrawn;
I3D_environment g_defaultEnvironment;

}  // namespace ls3d::i3d

namespace {

// The special sectors' space (0x100bf1f0; the original's w is never set).
const I3D_bbox kSpecialSectorBox = {S_vector(-10000.0f, -10000.0f, -10000.0f), 0.0f,
                                    S_vector(10000.0f, 10000.0f, 10000.0f), 0.0f};

constexpr uint32_t kFourCC_EMTR = 0x52544D45;   // 'EMTR'
constexpr uint32_t kFourCC_OBJ_ = 0x5F4A424F;   // 'OBJ_'
constexpr uint32_t kFourCC_LMAP = 0x50414D4C;   // 'LMAP'

// The EnumFrames callback that gets a sector ready (0x10048320).
uint32_t __stdcall MakeSectorReady(I3D_frame *frame, void *)
{
    static_cast<I3D_sector *>(frame)->MakeReady();
    return 0;
}

I3D_sector *MakeSpecialSector(I3D_scene *scene, const char *name, uint32_t kind)
{
    auto *s = static_cast<I3D_sector *>(CreateFrameObject(FRAME_SECTOR));
    s->I3D_frame::SetName(name);
    s->scene = scene;
    s->sectorFlags |= kind;
    s->frameFlags |= FRMFLAGS_POS_LOCKED;
    s->SetBox(kSpecialSectorBox);
    return s;
}

// The sound listener's updates come at most every 100 ms (0x100bf280).
DWORD g_lastEnvironmentTime;
bool g_lastEnvironmentTimeSet;

// The value SetWorldSpace passes on (0x100bf258).
bool g_worldSpace;

void SetVisualWorldSpace(I3D_frame *f)
{
    if ((f->frameFlags & 0x10004000) || f->type == FRAME_VOLUME || f->type == FRAME_MODEL)
        return;
    if ((f->type == FRAME_VISUAL && f->GetVisualType() == kFourCC_OBJ_) || f->GetVisualType() == kFourCC_LMAP)
        static_cast<I3D_visual *>(f)->SetWorldSpace(g_worldSpace);
    for (I3D_frame *c = f->firstChild; c; c = c->nextSibling)
        SetVisualWorldSpace(c);
}

template <class T> T Clamp(T v, T lo, T hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

int FloatToInt(float f) { return static_cast<int>(f); }   // __ftol: truncation

}  // namespace

// ---------------------------------------------------------------- frame slots

I3D_scene::~I3D_scene() {}

void I3D_scene::Init()
{
    I3D_frame::Init();
    backgroundColor = S_vector(0.0f, 0.5f, 0.4f);
    animStack = nullptr;
    renderCallbackFlags = 0;
    renderCallback = nullptr;
    backdropNear = 1.0f;
    backdropFar = 10000.0f;
    activeCamera = nullptr;
    portalCount = 0;
    firstPortal = nullptr;
    portalChangesSeen = 0;
    sectorCount = 0;
    firstSector = nullptr;
    sectorChangesSeen = 0;
    occluderCount = 0;
    firstOccluder = nullptr;
    occludersChanged = false;
    occluderListsStale = false;
    areaCount = 0;
    firstArea = nullptr;
    areasChanged = false;
    listener = nullptr;
    frameFlags |= FRMFLAGS_POS_LOCKED;
    primarySector = MakeSpecialSector(this, "Primary sector", SCTF_PRIMARY);
    backdropSector = MakeSpecialSector(this, "Backdrop sector", SCTF_BACKDROP);
    omniSector = MakeSpecialSector(this, "Omni sector", SCTF_OMNI);
    nearSector = MakeSpecialSector(this, "Near sector", SCTF_NEAR);
    ++g_sceneCount;
    weatherFlags = WEATHER_RESHAPE | WEATHER_RECOLOR;
    ResetWeather();
    if (!g_snowTexture) {
        if (FAILED(g_pIGraph->LoadTexture("Snow.dds", nullptr, ITC_SOURCE_ALPHA, &g_snowTexture)))
            g_pIGraph->LoadTexture("Snow.tga", nullptr, ITC_SOURCE_ALPHA, &g_snowTexture);
    }
    g_defaultEnvironment.SetDefaults();
    trackedCameraSector = nullptr;
    trackedModels.SetGrowBy(25);
    const int height = g_pIGraph->ScrnY();
    SetViewport(0, 0, g_pIGraph->ScrnX(), height);
    if (g_sndDriver && g_sndDriver->IsInitialized())
        g_sndDriver->CreateListener(&listener);
    g_driver->scenes.PushBack(this);
}

void I3D_scene::Uninit()
{
    Close();
    SetActiveCamera(nullptr);
    // (The pointers stay: Init sets them again.)
    for (I3D_sector *s : {primarySector, backdropSector, omniSector, nearSector}) {
        if (s)
            s->Release();
    }
    delete listener;
    if (--g_sceneCount < 1) {
        if (g_snowTexture) {
            g_snowTexture->Release();
            g_snowTexture = nullptr;
        }
        if (g_weatherVB.vb) {
            FreeVB(&g_weatherVB);
            g_weatherVB = S_VBAlloc{};
        }
        if (g_weatherIB.ib) {
            FreeIB(&g_weatherIB);
            g_weatherIB = S_IBAlloc{};
        }
    }
    g_driver->scenes.RemoveSwap(this);
    I3D_frame::Uninit();
}

void I3D_scene::Tick(int time)
{
    for (int i = 0; i < tickFrames.Size(); ++i) {
        I3D_frame *f = tickFrames[i];
        f->Tick(time);
        if (f != tickFrames[i])
            --i;                            // (it took itself out)
    }
    if (animStack) {
        animStack->Tick(time);
        primarySector->Update();
        backdropSector->Update();
        omniSector->Update();
        nearSector->Update();
    }
}

I3D_RESULT I3D_scene::EnumFrames(I3D_ENUMPROC *proc, void *context, uint32_t flags, const char *mask)
{
    I3D_RESULT r = primarySector->EnumFrames(proc, context, flags, mask);
    if (r < 0)
        return r;
    if ((flags & ENUMF_BACKDROP) && (r = backdropSector->EnumFrames(proc, context, flags, mask)) < 0)
        return r;
    if ((flags & ENUMF_OMNI) && (r = omniSector->EnumFrames(proc, context, flags, mask)) < 0)
        return r;
    if (flags & ENUMF_NEAR)
        return nearSector->EnumFrames(proc, context, flags, mask);
    return I3DERR_OBJECTNOTFOUND;           // (without the near sector, as the original)
}

// ---------------------------------------------------------------- frames

void *I3D_scene::SetRenderCallback(void *function, uint32_t flags)
{
    void *old = renderCallback;
    renderCallbackFlags = function ? flags : 0;
    renderCallback = function;
    return old;
}

I3D_RESULT I3D_scene::Render(uint32_t flags) { return g_driver->Render(this, flags); }

I3D_frame *I3D_scene::FindFrame(const char *frameName, uint32_t flags)
{
    if (I3D_frame *f = primarySector->FindChild(frameName, flags))
        return f;
    I3D_frame *f;
    if ((flags & ENUMF_BACKDROP) && (f = backdropSector->FindChild(frameName, flags)))
        return f;
    if ((flags & ENUMF_OMNI) && (f = omniSector->FindChild(frameName, flags)))
        return f;
    if ((flags & ENUMF_NEAR) && (f = nearSector->FindChild(frameName, flags)))
        return f;
    return nullptr;
}

void I3D_scene::AddFrame(I3D_frame *frame)
{
    if (frame->ownerModel == frame)
        return;
    ++frame->refCount;
    frame->NotifyOwnerRemoved();
    frame->ownerModel = this;
    frames.PushBack(frame);
    if (!(frame->frameFlags & FRMFLAGS_SCENE_SOUND) && frame->type == FRAME_VISUAL &&
        frame->GetVisualType() == kFourCC_EMTR) {
        for (I3D_frame *f : tickFrames) {
            if (f == frame)
                return;
        }
        tickFrames.PushBack(frame);
        frame->frameFlags |= FRMFLAGS_SCENE_SOUND;
    }
}

I3D_RESULT I3D_scene::RemoveFrame(I3D_frame *frame)
{
    if (!frames.RemoveSwap(frame))
        return I3DERR_OBJECTNOTFOUND;
    frame->ownerModel = nullptr;
    if (frame->frameFlags & FRMFLAGS_SCENE_SOUND) {
        frame->frameFlags &= ~FRMFLAGS_SCENE_SOUND;
        tickFrames.RemoveSwap(frame);
    }
    if (frame->refCount > 0)
        frame->Release();
    return I3D_OK;
}

void I3D_scene::AddTickFrame(I3D_frame *frame)
{
    if (frame->frameFlags & FRMFLAGS_SCENE_SOUND)
        return;
    tickFrames.PushBack(frame);
    frame->frameFlags |= FRMFLAGS_SCENE_SOUND;
}

I3D_RESULT I3D_scene::RemoveTickFrame(I3D_frame *frame)
{
    if (!tickFrames.RemoveSwap(frame))
        return I3DERR_OBJECTNOTFOUND;
    frame->frameFlags &= ~FRMFLAGS_SCENE_SOUND;
    return I3D_OK;
}

void I3D_scene::SetLightsColor(const S_vector &color)
{
    C_vector<I3D_light *> &lights = primarySector->lights;
    for (int i = lights.Size(); i > 0; --i)
        lights[i - 1]->SetColor(color);
}

float I3D_scene::GetLightIntensity(const S_vector &pos, I3D_sector *sector, uint32_t flags, I3D_frame *excluded)
{
    if (!sector && !(sector = GetSector(pos)))
        return 0.0f;
    float ambient = 0.0f, total = 0.0f;
    C_vector<I3D_light *> &lights = sector->lights;
    for (int i = lights.Size(); i > 0; --i) {
        I3D_light *l = lights[i - 1];
        if (!(l->frameFlags & FRMFLAGS_ON) || !(l->mode & 0x40))
            continue;
        const S_vector &c = l->effectiveColor;
        float b = ((c.z + c.z) * 0.1f + (c.y + c.y) * 0.6f) + (c.x + c.x) * 0.3f;
        const S_vector lightPos(l->d3dLight.Position.x, l->d3dLight.Position.y, l->d3dLight.Position.z);
        const S_vector lightDir(l->d3dLight.Direction.x, l->d3dLight.Direction.y, l->d3dLight.Direction.z);
        switch (l->lightType) {
        case I3DLIGHT_AMBIENT:
            ambient = b + ambient;
            continue;
        case I3DLIGHT_POINT:
        case I3DLIGHT_SPOT: {
            const S_vector d(pos.x - lightPos.x, pos.y - lightPos.y, pos.z - lightPos.z);
            if (l->lightType == I3DLIGHT_SPOT) {
                S_vector n = d;
                NormalizeWithSquare(n, SquareZYX(d));
                const float cosine = (lightDir.z * n.z + lightDir.y * n.y) + n.x * lightDir.x;
                if (cosine < l->coneOuterCos || cosine == l->coneOuterCos)
                    continue;
                if (cosine < l->coneInnerCos || cosine == l->coneInnerCos)
                    b = ((cosine - l->coneOuterCos) * l->coneFalloff) * b;
            }
            const float dist = MagnitudeOfSquare(SquareZYX(d));
            if (dist >= l->range)
                continue;
            b = b / ((dist * dist * l->d3dLight.Attenuation2 + dist * l->d3dLight.Attenuation1) +
                     l->d3dLight.Attenuation0);
            break;
        }
        case I3DLIGHT_DIRECTIONAL:
            break;
        default:
            if (l->lightType >= 0 && l->lightType <= 7)
                continue;                   // (none, fog lights)
            break;
        }
        if (flags & 1) {
            // Geometry between the point and the light hides it.
            uint32_t rf = flags & 0x40 ? 0x25000000 : 0x21000000;
            S_vector toLight;
            if (l->lightType == I3DLIGHT_DIRECTIONAL) {
                toLight = S_vector(-lightDir.x, -lightDir.y, -lightDir.z);
                rf |= 0x40000;
            } else {
                toLight = S_vector(lightPos.x - pos.x, lightPos.y - pos.y, lightPos.z - pos.z);
            }
            if (TestRay(pos, toLight, rf, nullptr, nullptr, nullptr, nullptr, nullptr, excluded, 0))
                continue;
        }
        total = b + total;
    }
    const float r = total + ambient;
    if (r < 0.0f)
        return 0.0f;
    if (r < 1.0f)
        return r;
    return 1.0f;
}

void I3D_scene::SetActiveCamera(I3D_camera *camera)
{
    if (activeCamera == camera)
        return;
    if (activeCamera) {
        activeCamera->scene = nullptr;
        activeCamera->sector = nullptr;
        activeCamera->Release();
    }
    activeCamera = camera;
    if (!camera)
        return;
    ++camera->refCount;
    camera->scene = this;
    const S_matrix &m = camera->GetMatrix();
    camera->sector = GetSector(*reinterpret_cast<const S_vector *>(m.m[3]));
    if (!camera->ortho)
        camera->BuildProjection();
}

I3D_RESULT I3D_scene::UnmapScreenPoint(int x, int y, S_vector4 &origin, S_vector4 &dir)
{
    if (!activeCamera)
        return I3DERR_NOTINITIALIZED;
    const float hw = static_cast<float>(g_pIGraph->ScrnX()) * 0.5f;
    const float hh = static_cast<float>(g_pIGraph->ScrnY()) * 0.5f;
    I3D_camera *cam = activeCamera;
    const float fx = (static_cast<float>(x) - hw) / hw;
    const float fy = (static_cast<float>(y) - hh) / hh;
    if (cam->ortho) {
        const float s = 1.0f / cam->orthoScale;
        const S_matrix &m = cam->GetMatrix();
        const S_vector v(s * fx, -((fy / cam->aspect) * s), 0.0f);
        const S_vector o = v * m;
        origin.x = o.x;
        origin.y = o.y;
        origin.z = o.z;
        origin.w = m.m[3][3];               // (the original copies the result's fourth float)
        const S_matrix &w = cam->GetMatrix();
        S_vector d(w.m[2][0], w.m[2][1], w.m[2][2]);
        NormalizeWithSquare(d, SquareYZX(d));
        dir.x = d.x;
        dir.y = d.y;
        dir.z = d.z;
        dir.w = w.m[2][3];
        return I3D_OK;
    }
    const S_matrix &m = cam->GetMatrix();
    origin.x = m.m[3][0];
    origin.y = m.m[3][1];
    origin.z = m.m[3][2];
    origin.w = m.m[3][3];
    const S_vector v(fx * cam->nearHalfWidth, -(fy * cam->nearHalfHeight), cam->nearRange);
    const S_matrix inv = cam->invViewMatrix;
    S_vector d = v.RotateByMatrix(inv);
    NormalizeWithSquare(d, SquareXYZ(d));
    dir.x = d.x;
    dir.y = d.y;
    dir.z = d.z;
    dir.w = 0.0f;                           // (the original: the rotation's fourth float)
    return I3D_OK;
}

I3D_RESULT I3D_scene::TransformPoints(const S_vector4 *points, S_vector4 *out, int count)
{
    if (!activeCamera)
        return I3DERR_NOTINITIALIZED;
    const S_matrix &m = activeCamera->viewProjection;
    const float hw = static_cast<float>(g_pIGraph->ScrnX()) * 0.5f;
    const float hh = static_cast<float>(g_pIGraph->ScrnY()) * 0.5f;
    for (int n = count; n != 0; --n, ++points, ++out) {
        const S_vector4 &p = *points;
        const float w = ((p.z * m.m[2][3] + p.y * m.m[1][3]) + m.m[0][3] * p.x) + m.m[3][3];
        out->w = IsTinyBits(std::fabs(w)) ? 1e16f : 1.0f / w;
        const S_vector r = *reinterpret_cast<const S_vector *>(&p) * m;
        out->x = (out->w * r.x + 1.0f) * hw;
        out->y = (1.0f - out->w * r.y) * hh;
        out->z = r.z;
    }
    return I3D_OK;
}

bool I3D_scene::IsFrameVisible(I3D_frame *frame, I3D_camera *camera)
{
    if (!frame || !camera)
        return false;
    if (frame->type == FRAME_VISUAL) {
        auto *v = static_cast<I3D_visual *>(frame);
        v->GetWorldBound();
        return camera->IsSphereVisible(*reinterpret_cast<const S_vector4 *>(&v->worldBound.center));
    }
    if (!(frame->frameFlags & FRMFLAGS_BOUND_VALID))
        frame->UpdateBound();
    return camera->IsSphereVisible(*reinterpret_cast<const S_vector4 *>(&frame->bound.center));
}

// ---------------------------------------------------------------- animation

I3D_RESULT I3D_scene::SetAnimation(I3D_animation_set *set, int stage, float weight, int mode)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!animStack) {
        animStack = new (std::nothrow) I3D_anim_stack(this);
        if (!animStack) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
    }
    return animStack->SetAnimation(set, stage, weight, mode);
}

I3D_animation_set *I3D_scene::GetAnimation(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return nullptr;
    return animStack->stages[stage].set;
}

void I3D_scene::StopAnimation(int stage)
{
    if (static_cast<uint32_t>(stage) < 8 && animStack)
        animStack->stages[stage].active = false;
}

I3D_RESULT I3D_scene::SetAnimWeight(int stage, float weight)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    if (weight < 0.0f) {
        animStack->stages[stage].weight = 0.0f;
    } else {
        if (1.0f < weight)
            weight = 1.0f;
        animStack->stages[stage].weight = weight;
    }
    animStack->dirty = true;
    return I3D_OK;
}

float I3D_scene::GetAnimWeight(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return 0.0f;
    return animStack->stages[stage].weight;
}

I3D_RESULT I3D_scene::SetAnimTime(int stage, int time)
{
    if (static_cast<uint32_t>(stage) > 7)
        return I3DERR_INVALIDPARAMS;
    if (!animStack)
        return I3DERR_OBJECTNOTFOUND;
    animStack->stages[stage].time = time;
    animStack->stages[stage].prevTime = time;
    animStack->dirty = true;
    return I3D_OK;
}

int I3D_scene::GetAnimTime(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return 0;
    return animStack->stages[stage].time;
}

bool I3D_scene::IsAnimActive(int stage)
{
    if (static_cast<uint32_t>(stage) > 7 || !animStack)
        return false;
    return animStack->stages[stage].active;
}

int I3D_scene::NumTracks() { return animStack ? animStack->tracks.Size() : 0; }

void *I3D_scene::GetTracks() { return animStack ? animStack->tracks.Data() : nullptr; }

// ---------------------------------------------------------------- sectors

I3D_sector *I3D_scene::CreateSector()
{
    // (The original's sector skips the frame's Init: the caller gives it its state.)
    auto *s = new I3D_sector;
    s->SetScene(this);
    return s;
}

I3D_sector *I3D_scene::LocateSectorIn(const S_vector &pos, I3D_sector **sectors, int count)
{
    for (int i = count; i != 0; --i) {
        if (sectors[i - 1]->IsInside(pos))
            return sectors[i - 1];
    }
    return primarySector;
}

I3D_sector *I3D_scene::LocateSectorDepth(const S_vector &pos, I3D_sector *hint, float *depth)
{
    if (hint && !(hint->sectorFlags & SCTF_NO_PLANES)) {
        const float d = hint->InsideDistance(pos);
        if (d >= 0.0f) {
            *depth = d;
            return hint;
        }
    }
    I3D_sector *s = firstSector;
    for (int n = sectorCount; n != 0; --n, s = s->nextInScene) {
        if (s == hint)
            continue;
        const float d = s->InsideDistance(pos);
        if (d >= 0.0f) {
            *depth = d;
            return s;
        }
    }
    *depth = -1e16f;
    return primarySector;
}

I3D_sector *I3D_scene::LocateSector(const S_vector &pos, I3D_sector *hint)
{
    if (hint && !(hint->sectorFlags & SCTF_NO_PLANES) && hint->IsInside(pos))
        return hint;
    I3D_sector *s = firstSector;
    for (int n = sectorCount; n != 0; --n, s = s->nextInScene) {
        if (s != hint && s->IsInside(pos))
            return s;
    }
    return primarySector;
}

I3D_sector *I3D_scene::GetSector(const S_vector &pos)
{
    I3D_sector *s = firstSector;
    for (int n = sectorCount; n != 0; --n, s = s->nextInScene) {
        if (s->IsInside(pos))
            return s;
    }
    return primarySector;
}

void I3D_scene::SetBackdropRange(float nearRange, float farRange)
{
    if (!(std::fabs(farRange - nearRange) >= 0.01f) || !(nearRange >= 0.001f) || !(farRange >= 0.001f))
        return;
    backdropNear = nearRange;
    backdropFar = farRange;
    if (activeCamera && !activeCamera->ortho)
        activeCamera->BuildProjection();
}

void I3D_scene::UpdateFrameSectorIdle(I3D_frame *frame, const S_vector &pos, int)
{
    if (!activeCamera)
        UpdateFrameSector(frame, pos);
}

void I3D_scene::UpdateFrameSector(I3D_frame *frame, const S_vector &pos)
{
    I3D_frame *p = frame->parent;
    for (; p; p = p->parent) {
        if (p->type != FRAME_SECTOR)
            continue;
        auto *s = static_cast<I3D_sector *>(p);
        if (s->sectorFlags & (SCTF_BACKDROP | SCTF_OMNI | SCTF_NEAR))
            return;
        if (!(s->sectorFlags & SCTF_PRIMARY) && s->IsInside(pos))
            return;
        break;
    }
    I3D_sector *s = GetSector(pos);
    if (p != s)
        frame->LinkTo(s, I3DLINK_UPDMATRIX);
}

void I3D_scene::ReadySectors()
{
    EnumFrames(&MakeSectorReady, nullptr, ENUMF_SECTOR, nullptr);
    I3D_portal *p = firstPortal;
    for (int n = portalCount; n != 0; --n, p = p->next)
        p->MatrixChanged();
}

void I3D_scene::UpdateSectors()
{
    if (sectorChangesSeen != g_sectorChanges) {
        sectorChangesSeen = g_sectorChanges;
        ReadySectors();
    }
    if (portalChangesSeen != g_portalChanges) {
        portalChangesSeen = g_portalChanges;
        BuildPortals(this);
    }
}

I3D_RESULT I3D_scene::Open(const char *fileName, uint32_t flags, void *loadCallback, void *context)
{
    Close();
    if (!fileName)
        return I3D_OK;
    const I3D_RESULT r = OpenModelFile(fileName, flags, loadCallback, context, primarySector);
    if (activeCamera)
        activeCamera->Update();
    EnumFrames(&MakeSectorReady, nullptr, ENUMF_SECTOR, nullptr);
    I3D_portal *p = firstPortal;
    for (int n = portalCount; n != 0; --n, p = p->next)
        p->MatrixChanged();
    return r;
}

void I3D_scene::Close()
{
    ClearTrackedModels();
    tex::StreamReleaseAll();
    primarySector->renderVisuals = nullptr;
    backdropSector->renderVisuals = nullptr;
    omniSector->renderVisuals = nullptr;
    nearSector->renderVisuals = nullptr;
    g_activeAreas.Clear();
    for (int n = primarySector->sounds.Size(); n != 0; --n)
        primarySector->RemoveSound(primarySector->sounds[0]);
    for (int i = lensFlares.Size(); i != 0; --i)
        lensFlares[i - 1]->Release();
    lensFlares.Clear();
    g_driver->envSectors.Clear();
    tickFrames.Clear();
    delete animStack;
    animStack = nullptr;
    for (I3D_frame *f : frames) {
        f->ownerModel = nullptr;
        f->Release();
    }
    frames.Clear();
    if (activeCamera)
        activeCamera->sector = nullptr;
    g_renderCamera = nullptr;
    primarySector->fogLight = nullptr;
    backdropSector->fogLight = nullptr;
    primarySector->sectorFlags |= SCTF_ENV_CHANGED;
    primarySector->environmentBlend = 0.0f;
    primarySector->environmentTime = 0;
    primarySector->environment.SetDefaults();
    g_pGfx->ResourceManagerDiscardBytes(0);
    g_vbManager.FreeUnused();
    g_ibManager.FreeUnused();
    for (I3D_shadow *s : shadows)
        s->scene = nullptr;
    shadows.Clear();
    mirrors.Clear();                        // (their +0x2E0 keeps the scene)
    ResetRenderLists();
}

// ---------------------------------------------------------------- weather and visuals

void I3D_scene::SetWorldSpace(bool value)
{
    g_worldSpace = value;
    I3D_frame *f = primarySector;
    if ((f->frameFlags & 0x10004000) || f->type == FRAME_VOLUME || f->type == FRAME_MODEL)
        return;
    if ((f->type == FRAME_VISUAL && f->GetVisualType() == kFourCC_OBJ_) || f->GetVisualType() == kFourCC_LMAP)
        static_cast<I3D_visual *>(f)->SetWorldSpace(g_worldSpace);
    for (I3D_frame *c = f->firstChild; c; c = c->nextSibling)
        SetVisualWorldSpace(c);
}

void I3D_scene::ResetWeather()
{
    const uint32_t old = weatherFlags;
    if (!(old & WEATHER_SNOW)) {
        weatherFlags = 0;
        weatherFallSpeed = 0.025f;
        weatherParam6 = 1.5f;
        weatherParam7 = 0.03f;
    } else {
        weatherFlags = WEATHER_SNOW;
        weatherFallSpeed = 0.002f;
        weatherParam6 = 0.3f;
        weatherParam7 = 0.15f;
    }
    weatherFlags |= (old & WEATHER_ON) | 0x186;
    weatherRangeXZ = 25.0f;
    weatherRangeY = 12.0f;
    weatherCount = 1000;
    weatherParam2CC = 3000;
    // Straight down, tilted 0.2 rad (the original copies a fourth float from past the result).
    const S_vector down = S_vector(0.0f, -1.0f, 0.0f) * RotateZMatrix(0.2f);
    weatherWind = S_vector4{down.x, down.y, down.z, 0.0f};
    weatherWindStart = weatherWind;
    weatherColor1 = 0x80606060;
    weatherColor2 = 0x00606060;
    weatherDirection.SetDir(*reinterpret_cast<const S_vector *>(&weatherWindStart));
    weatherRadius = std::sqrt(weatherRangeY * weatherRangeY + weatherRangeXZ * weatherRangeXZ);
    weatherParticles.SetGrowBy(1000);
    weatherParticles.Clear();
}

void I3D_scene::SetWeather(int param, uint32_t value)
{
    float f;
    std::memcpy(&f, &value, 4);
    switch (param) {
    case 0:
        weatherFlags = value ? weatherFlags | WEATHER_ON : weatherFlags & ~WEATHER_ON;
        return;
    case 1:
        weatherFlags = value ? weatherFlags | WEATHER_PARAM1 : weatherFlags & ~WEATHER_PARAM1;
        return;
    case 2:
        weatherFlags = value ? weatherFlags | WEATHER_PARAM2 : weatherFlags & ~WEATHER_PARAM2;
        return;
    case 3:
        weatherFlags |= WEATHER_RECOLOR;
        weatherColor1 = value;
        return;
    case 4:
        weatherColor2 = value;
        weatherFlags |= WEATHER_RECOLOR;
        return;
    case 5:
        weatherFallSpeed = static_cast<float>(static_cast<int>(value)) * 0.001f;
        return;
    case 6:
        weatherFlags |= WEATHER_RESHAPE;
        weatherParam6 = f;
        return;
    case 7:
        weatherFlags |= WEATHER_RESHAPE;
        weatherParam7 = static_cast<float>(static_cast<int>(value)) * 0.5f;
        return;
    case 8:
        weatherRangeXZ = f;
        weatherRadius = std::sqrt(weatherRangeY * weatherRangeY + f * f);
        return;
    case 9:
        weatherRangeY = f;
        weatherRadius = std::sqrt(weatherRangeXZ * weatherRangeXZ + f * f);
        return;
    case 10:
        SetWeatherCount(static_cast<int>(value));
        return;
    case 11:
        weatherWind.x = f;
        return;
    case 12:
        weatherWind.y = f;
        return;
    case 13:
        weatherWind.z = f;
        return;
    case 14:
        if (value != 1)
            weatherFlags = (weatherFlags & ~WEATHER_SNOW) | WEATHER_RESHAPE;
        else
            weatherFlags |= WEATHER_SNOW | WEATHER_RESHAPE;
        return;
    case 15:
        if (value == 1)
            weatherFlags |= WEATHER_MODE1;
        else if (value == 2)
            weatherFlags |= WEATHER_MODE2;
        else
            weatherFlags &= ~(WEATHER_MODE1 | WEATHER_MODE2);
        return;
    default:
        return;
    }
}

int I3D_scene::GetWeather(int param)
{
    switch (param) {
    case 0: return weatherFlags & WEATHER_ON;
    case 1: return weatherFlags & WEATHER_PARAM1;
    case 2: return weatherFlags & WEATHER_PARAM2;
    case 3: return static_cast<int>(weatherColor1);
    case 4: return static_cast<int>(weatherColor2);
    case 5: return static_cast<int>(weatherFallSpeed * 1000.0f);
    case 6: {
        int bits;
        std::memcpy(&bits, &weatherParam6, 4);
        return bits;
    }
    case 7: return static_cast<int>(weatherParam7 + weatherParam7);
    case 8: {
        int bits;
        std::memcpy(&bits, &weatherRangeXZ, 4);
        return bits;
    }
    case 9: {
        int bits;
        std::memcpy(&bits, &weatherRangeY, 4);
        return bits;
    }
    case 10: return weatherCount;
    case 11: case 12: case 13: {
        int bits;
        std::memcpy(&bits, &(&weatherWind.x)[param - 11], 4);
        return bits;
    }
    case 14: return weatherFlags >> 6 & 1;
    case 15: return weatherFlags & WEATHER_MODE1 ? 1 : weatherFlags >> 9 & 2;
    default: return -1;
    }
}

void I3D_scene::SetWeatherCount(int count)
{
    weatherCount = count;
    if (count * 5 > 0x10000)
        weatherCount = 0x3333;
    if (!g_weatherVB.vb || g_weatherCapacity < weatherCount) {
        g_weatherCapacity = weatherCount;
        FreeVB(&g_weatherVB);
        g_weatherVB = S_VBAlloc{};
        FreeIB(&g_weatherIB);
        g_weatherIB = S_IBAlloc{};
        // Five vertices a particle (position, colour, uv; 0x18 bytes): four corners and the middle.
        if (!g_vbManager.Alloc(D3DUSAGE_WRITEONLY, 0x18, static_cast<uint32_t>(weatherCount * 5), &g_weatherVB, 2,
                               false)) {
            dbgPrintf("WeatherSystem ERROR !!! VB creating !");
            return;
        }
        C_VertexBuffer *vb = g_weatherVB.buffer;
        auto *v = reinterpret_cast<float *>(vb->Lock(0) + vb->stride * g_weatherVB.offset);
        static const float kUV[5][2] = {{1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}, {0.5f, 0.5f}};
        for (int n = weatherCount; n != 0; --n, v += 30) {
            for (int k = 0; k < 5; ++k) {
                v[6 * k + 4] = kUV[k][0];
                v[6 * k + 5] = kUV[k][1];
            }
        }
        weatherFlags |= WEATHER_RESHAPE | WEATHER_RECOLOR;
        if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, static_cast<uint32_t>(weatherCount * 12), &g_weatherIB)) {
            dbgPrintf("WeatherSystem ERROR !!! VB creating !");
            return;
        }
        // Four triangles around the middle (0x100a6b84).
        static const uint16_t kFan[12] = {1, 0, 4, 0, 2, 4, 2, 3, 4, 3, 1, 4};
        uint16_t *idx = g_weatherIB.buffer->Lock(0) + g_weatherIB.offset;
        for (int i = 0; i < weatherCount; ++i) {
            for (uint16_t f : kFan)
                *idx++ = static_cast<uint16_t>(f + i * 5);
        }
    }
    const int have = weatherParticles.Size();
    if (weatherCount <= have) {
        weatherParticles.SetCount(weatherCount);
        return;
    }
    S_vector cam(0.0f, 0.0f, 0.0f);
    if (activeCamera) {
        const S_matrix &m = activeCamera->GetMatrix();
        cam = S_vector(m.m[3][0], m.m[3][1], m.m[3][2]);
    }
    weatherParticles.Reserve(weatherCount > have ? weatherCount : have);
    for (int n = weatherCount - have; n != 0; --n) {
        S_vector4 p;
        const float rx = RandFloat() * weatherRangeXZ;
        p.x = ((cam.x - weatherRangeXZ) + rx) + rx;
        const float ry = RandFloat() * weatherRangeY;
        p.y = ((cam.y - weatherRangeY) + ry) + ry;
        const float rz = RandFloat() * weatherRangeXZ;
        p.z = ((cam.z - weatherRangeXZ) + rz) + rz;
        p.w = cam.y - weatherRangeY;
        weatherParticles.PushBack(p);
    }
}

// The box of a drawn dummy (0x10049000): three of its faces as planes, its centre, the squared
// half diagonal and the edge lengths, in world space.
void I3D_scene::MakeDummyBox(S_dummyBox &out, I3D_frame *frame)
{
    const S_matrix m = frame->GetMatrix();
    const I3D_bbox &box = *reinterpret_cast<const I3D_bbox *>(reinterpret_cast<const uint8_t *>(frame) + 0x120);
    out.frame = frame;
    out.changeCount = frame->changeCount;
    const S_vector hi = box.max * m;
    const S_vector lo = box.min * m;
    const S_vector a = S_vector(box.min.x, box.max.y, box.min.z) * m;
    const S_vector b = S_vector(box.max.x, box.max.y, box.min.z) * m;
    const S_vector c = S_vector(box.max.x, box.min.y, box.min.z) * m;
    const S_vector half((hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f);
    out.center = S_vector(half.x + lo.x, half.y + lo.y, half.z + lo.z);
    out.radiusSquare = SquareZYX(half);
    // The normal of the plane through three points: (r - q) x (p - q) (0x1000c6a0).
    auto plane = [](S_plane &pl, const S_vector &p, const S_vector &q, const S_vector &r, const S_vector &on) {
        S_vector n = (r - q).Cross(p - q);
        NormalizeWithSquare(n, SquareZYX(n));
        pl.normal = n;
        pl.d = -n.Dot(on);
    };
    plane(out.planes[0], c, b, a, c);
    plane(out.planes[1], c, hi, b, c);
    plane(out.planes[2], b, hi, a, b);
    out.extents[0] = (b - hi).Magnitude();
    out.extents[1] = (a - b).Magnitude();
    out.extents[2] = (b - c).Magnitude();
}

// ---------------------------------------------------------------- drawn dummies

void I3D_scene::AddDrawnDummy(I3D_frame *dummy)
{
    if (dummy->frameFlags & FRMFLAGS_SCENE_DUMMY)
        return;
    for (int i = drawnDummies.Size(); i != 0; --i) {
        if (drawnDummies[i - 1].frame == dummy)
            return;
    }
    S_dummyBox box;
    MakeDummyBox(box, dummy);
    drawnDummies.PushBack(box);
    dummy->frameFlags |= FRMFLAGS_SCENE_DUMMY;
}

void I3D_scene::RemoveDrawnDummy(I3D_frame *dummy)
{
    for (int i = drawnDummies.Size(); i != 0; --i) {
        if (drawnDummies[i - 1].frame != dummy)
            continue;
        drawnDummies.RemoveAtSwap(i - 1);
        break;
    }
    dummy->frameFlags &= ~FRMFLAGS_SCENE_DUMMY;
}

int I3D_scene::NumDrawnDummies() { return drawnDummies.Size(); }

S_dummyBox *I3D_scene::GetDrawnDummies() { return drawnDummies.Data(); }

// ---------------------------------------------------------------- tracked models

I3D_RESULT I3D_scene::AddTrackedModel(I3D_frame *model, I3D_frame *refA, I3D_frame *refB)
{
    if (!model || !refA || !refB || refA == refB)
        return I3DERR_INVALIDPARAMS;
    if (model->frameFlags & 0x7000)
        return I3DERR_UNSUPPORTED;
    model->frameFlags |= FRMFLAGS_TRACKED_STALE;
    for (S_trackedModel &t : trackedModels) {
        if (t.frame != model)
            continue;
        if (t.refA != refA) {
            t.distanceA = -1e16f;
            if (t.sectorA) {
                t.sectorA->Release();
                t.sectorA = nullptr;
            }
            if (t.refA != t.frame)
                t.refA->Release();
            if (refA != t.frame)
                ++refA->refCount;
            t.refA = refA;
        }
        if (t.refB != refB) {
            t.distanceB = -1e16f;
            if (t.sectorB) {
                t.sectorB->Release();
                t.sectorB = nullptr;
            }
            if (t.refB != t.frame)
                t.refB->Release();
            if (refB != t.frame)
                ++refB->refCount;
            t.refB = refB;
        }
        return I3D_OK;
    }
    S_trackedModel t{};
    t.distanceA = -1e16f;
    t.distanceB = -1e16f;
    t.frame = model;
    ++model->refCount;
    t.refA = refA;
    if (refA != model)
        ++refA->refCount;
    t.refB = refB;
    if (refB != model)
        ++refB->refCount;
    trackedModels.PushBack(t);
    return I3D_OK;
}

I3D_RESULT I3D_scene::RemoveTrackedModel(I3D_frame *model)
{
    for (int i = 0; i < trackedModels.Size(); ++i) {
        S_trackedModel &t = trackedModels[i];
        if (t.frame != model)
            continue;
        t.frame->frameFlags &= ~FRMFLAGS_TRACKED_STALE;
        t.frame->Release();
        if (t.refA != t.frame)
            t.refA->Release();
        if (t.refB != t.frame)
            t.refB->Release();
        if (t.sectorA)
            t.sectorA->Release();
        if (t.sectorB)
            t.sectorB->Release();
        trackedModels.RemoveAtSwap(i);
        return I3D_OK;
    }
    return I3DERR_OBJECTNOTFOUND;
}

void I3D_scene::ClearTrackedModels()
{
    for (S_trackedModel &t : trackedModels) {
        t.frame->Release();
        if (t.refA != t.frame)
            t.refA->Release();
        if (t.refB != t.frame)
            t.refB->Release();
        if (t.sectorA)
            t.sectorA->Release();
        if (t.sectorB)
            t.sectorB->Release();
    }
    trackedModels.Clear();
}

void I3D_scene::MarkTrackedModel(I3D_frame *model) { model->frameFlags |= FRMFLAGS_TRACKED_STALE; }

void I3D_scene::UpdateTrackedModels(I3D_sector *cameraSector)
{
    const bool cameraMoved = cameraSector != trackedCameraSector;
    if (cameraMoved)
        trackedCameraSector = cameraSector;
    for (S_trackedModel &t : trackedModels) {
        if (t.frame->frameFlags & FRMFLAGS_TRACKED_STALE) {
            I3D_sector *old = t.sectorA;
            const S_matrix &ma = t.refA->GetMatrix();
            t.sectorA = LocateSectorDepth(*reinterpret_cast<const S_vector *>(ma.m[3]), old, &t.distanceA);
            ++t.sectorA->refCount;
            if (old)
                old->Release();
            old = t.sectorB;
            const S_matrix &mb = t.refB->GetMatrix();
            t.sectorB = LocateSectorDepth(*reinterpret_cast<const S_vector *>(mb.m[3]), old, &t.distanceB);
            ++t.sectorB->refCount;
            if (old)
                old->Release();
        }
        if (!cameraMoved && !(t.frame->frameFlags & FRMFLAGS_TRACKED_STALE))
            continue;
        t.frame->frameFlags &= ~FRMFLAGS_TRACKED_STALE;
        if (t.sectorA == cameraSector || t.sectorB == cameraSector)
            t.frame->LinkTo(cameraSector, I3DLINK_UPDMATRIX);
        else if (t.distanceA <= t.distanceB)
            t.frame->LinkTo(t.sectorB, I3DLINK_UPDMATRIX);
        else
            t.frame->LinkTo(t.sectorA, I3DLINK_UPDMATRIX);
    }
}

// ---------------------------------------------------------------- viewport and sound

void I3D_scene::SetViewport(int left, int top, int right, int bottom)
{
    viewportRight = right;
    viewportTop = top;
    const float hw = static_cast<float>(static_cast<uint32_t>(right - left)) * 0.5f;
    viewportBottom = bottom;
    viewportHalfWidth = hw;
    viewportLeft = left;
    const float hh = static_cast<float>(top - bottom) * 0.5f;
    viewportHalfHeight = hh;
    viewportCenterX = static_cast<float>(left) + hw;
    viewportCenterY = static_cast<float>(top) - hh;
}

void I3D_scene::SetScreenProjection(const S_matrix &m)
{
    screenProjection = m;
    for (int r = 0; r < 4; ++r) {
        screenProjection.m[r][0] = viewportHalfWidth * m.m[r][0];
        screenProjection.m[r][1] = m.m[r][1] * viewportHalfHeight;
    }
}

void I3D_scene::SetSoundEnvironment(I3D_sector *sector, I3D_sector *previous, float t)
{
    if (!listener)
        return;
    I3D_environment env;
    bool now = false;
    if (IsTinyBits(std::fabs(1.0f - t))) {
        env = sector->environment;
        now = true;
    } else {
        LerpEnvironment(env, previous ? previous->environment : g_defaultEnvironment, sector->environment, t, true);
    }
    if (!(g_sndDriver->flags & 0x10)) {
        if (!g_lastEnvironmentTimeSet) {
            g_lastEnvironmentTimeSet = true;
            g_lastEnvironmentTime = platform::Milliseconds();
        }
        const DWORD time = platform::Milliseconds();
        if (now || time - g_lastEnvironmentTime > 100) {
            auto f = [&](int k) {
                float v;
                std::memcpy(&v, &env.params[k], 4);
                return v;
            };
            auto i = [&](int k) { return static_cast<int>(env.params[k]); };
            snd::S_eaxListenerProperties p{};
            p.environment = 0x1a;
            p.environmentSize = 7.5f;
            p.roomLF = Clamp(FloatToInt(f(0) * 10000.0f - 10000.0f), -10000, 0);
            p.roomHF = Clamp(FloatToInt(f(1) * 10000.0f - 10000.0f), -10000, 0);
            p.decayTime = Clamp(static_cast<float>(i(2)) * 0.001f, 0.1f, 20.0f);
            p.decayHFRatio = Clamp(f(3) * 1.9f + 0.1f, 0.1f, 2.0f);
            p.reflections = Clamp(FloatToInt(f(4) * 11000.0f - 10000.0f), -10000, 1000);
            p.reflectionsDelay = Clamp(static_cast<float>(i(5)) * 0.001f, 0.0f, 0.3f);
            p.reverb = Clamp(FloatToInt(f(6) * 12000.0f - 10000.0f), -10000, 2000);
            p.reverbDelay = Clamp(static_cast<float>(i(7)) * 0.001f, 0.0f, 0.1f);
            p.environmentDiffusion = Clamp(f(8), 0.0f, 1.0f);
            p.decayLFRatio = Clamp(f(10) * 1.9f + 0.1f, 0.1f, 2.0f);
            p.room = Clamp(FloatToInt(f(11) * 10000.0f - 10000.0f), -10000, 0);
            p.echoTime = Clamp(static_cast<float>(i(12)) * 0.001f, 0.075f, 0.25f);
            p.echoDepth = Clamp(f(13), 0.0f, 1.0f);
            p.modulationTime = Clamp(static_cast<float>(i(14)) * 0.001f, 0.04f, 4.0f);
            p.modulationDepth = Clamp(f(15), 0.0f, 1.0f);
            p.roomRolloffFactor = Clamp(f(16) * 10.0f, 0.0f, 10.0f);
            p.airAbsorptionHF = Clamp(f(17) * 100.0f - 100.0f, -100.0f, 0.0f);
            p.hfReference = Clamp(f(18) * 19000.0f + 1000.0f, 1000.0f, 20000.0f);
            p.lfReference = Clamp(f(19) * 980.0f + 20.0f, 20.0f, 1000.0f);
            p.reflectionsPan = snd::S_eaxVector{env.reflectionsPan.x, env.reflectionsPan.y, env.reflectionsPan.z};
            p.reverbPan = snd::S_eaxVector{env.reverbPan.x, env.reverbPan.y, env.reverbPan.z};
            p.flags = 0x3f;
            listener->SetEaxData(snd::kEaxAllParameters, &p, sizeof(p));
            g_lastEnvironmentTime = time;
        }
    }
    if (previous)
        previous->sectorFlags &= ~SCTF_ENV_CHANGED;
    sector->sectorFlags &= ~SCTF_ENV_CHANGED;
}

// ---------------------------------------------------------------- free functions

namespace ls3d::i3d {

void SceneAddFrame(I3D_frame *scene, I3D_frame *frame) { static_cast<I3D_scene *>(scene)->AddFrame(frame); }
void SceneFrameReleased(I3D_frame *scene, I3D_frame *frame) { static_cast<I3D_scene *>(scene)->RemoveFrame(frame); }
I3D_RESULT SceneRemoveSoundFrame(I3D_scene *scene, I3D_frame *frame) { return scene->RemoveTickFrame(frame); }
I3D_sector *SceneLocateSector(I3D_scene *scene, const S_vector &pos, I3D_sector *hint)
{
    return scene->LocateSector(pos, hint);
}
void SceneRemoveDummy(I3D_scene *scene, I3D_frame *dummy) { scene->RemoveDrawnDummy(dummy); }

}  // namespace ls3d::i3d

namespace ls3d::i3d {

void LerpEnvironment(I3D_environment &out, const I3D_environment &a, const I3D_environment &b, float t, bool all)
{
    const float u = 1.0f - t;
    auto lf = [&](int k) {
        float x, y;
        std::memcpy(&x, &a.params[k], 4);
        std::memcpy(&y, &b.params[k], 4);
        const float r = u * x + t * y;
        std::memcpy(&out.params[k], &r, 4);
    };
    auto li = [&](int k) {
        const float r = static_cast<float>(static_cast<int>(a.params[k])) * u +
                        static_cast<float>(static_cast<int>(b.params[k])) * t;
        out.params[k] = static_cast<uint32_t>(static_cast<int>(r));
    };
    if (all) {
        lf(0);
        lf(1);
        li(2);
        lf(3);
        lf(4);
        li(5);
        lf(6);
        li(7);
        lf(8);
        lf(10);
        lf(11);
        out.reflectionsPan.x = u * a.reflectionsPan.x + t * b.reflectionsPan.x;
        out.reflectionsPan.y = u * a.reflectionsPan.y + t * b.reflectionsPan.y;
        out.reflectionsPan.z = u * a.reflectionsPan.z + t * b.reflectionsPan.z;
        out.reverbPan.x = u * a.reverbPan.x + t * b.reverbPan.x;
        out.reverbPan.y = u * a.reverbPan.y + t * b.reverbPan.y;
        out.reverbPan.z = u * a.reverbPan.z + t * b.reverbPan.z;
        li(12);
        lf(13);
        li(14);
        for (int k = 15; k < 20; ++k)
            lf(k);
    }
    li(9);
}

void SetFog(int on, int mode, float start, float end, uint32_t color)
{
    if (g_rs.fog == on) {
        if (!on)
            return;
    } else {
        g_rs.fog = on;
        if (on != 1) {
            g_pGfx->SetRenderState(D3DRS_FOGENABLE, FALSE);
            return;
        }
        g_pGfx->SetRenderState(D3DRS_FOGENABLE, TRUE);
    }
    const D3DRENDERSTATETYPE modeState = ig::g_caps & IGCAPS_TABLE_FOG ? D3DRS_FOGTABLEMODE : D3DRS_FOGVERTEXMODE;
    if (g_rs.fogMode != mode) {
        g_rs.fogMode = mode;
        if (mode == 0)
            g_pGfx->SetRenderState(modeState, D3DFOG_LINEAR);
        else if (mode == 1)
            g_pGfx->SetRenderState(modeState, D3DFOG_EXP);
        else if (mode == 2)
            g_pGfx->SetRenderState(modeState, D3DFOG_EXP2);
    }
    auto bits = [](float f) {
        DWORD d;
        std::memcpy(&d, &f, 4);
        return d;
    };
    if (g_rs.fogMode == 0) {
        if (!IsTinyBits(std::fabs(g_rs.fogEnd - end))) {
            g_rs.fogEnd = end;
            g_pGfx->SetRenderState(D3DRS_FOGEND, bits(end));
        }
        if (!IsTinyBits(std::fabs(g_rs.fogStart - start))) {
            g_rs.fogStart = start;
            g_pGfx->SetRenderState(D3DRS_FOGSTART, bits(start));
        }
    } else if (g_rs.fogMode >= 1 && g_rs.fogMode <= 2 && !IsTinyBits(std::fabs(g_rs.fogStart - start))) {
        g_rs.fogStart = start;
        g_pGfx->SetRenderState(D3DRS_FOGDENSITY, bits(start));
    }
    if (g_rs.fogColor != color) {
        g_rs.fogColor = color;
        g_pGfx->SetRenderState(D3DRS_FOGCOLOR, color);
    }
}

}  // namespace ls3d::i3d

// 0x10047fd0
void I3D_scene::AddShadow(I3D_shadow *shadow)
{
    if (shadow->scene == this)
        return;
    shadows.PushBack(shadow);
    shadow->scene = this;
}

// 0x10048070
I3D_RESULT I3D_scene::RemoveShadow(I3D_shadow *shadow)
{
    if (!shadows.RemoveSwap(shadow))
        return I3DERR_OBJECTNOTFOUND;
    shadow->scene = nullptr;
    return I3D_OK;
}
