// I3D_sector (0x1004c9b0 - 0x1004e530).
#include "i3d/sector.h"

#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "external/rw_data.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/light.h"
#include "i3d/loader.h"
#include "i3d/occluder.h"
#include "i3d/portal.h"
#include "i3d/scene.h"
#include "i3d/sound.h"

namespace ls3d::i3d {

int g_sectorChanges;
I3D_sector *g_envSector;
I3D_sector *g_envPrevSector;

}  // namespace ls3d::i3d

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

// Frame flags of a sector: the hierarchy holds a sector (FRMFLAGS_SECTOR_MASK), and the bit the
// camera uses as FRMFLAGS_CAMERA_PROJECTION (set by Init; the sector code never reads it).
constexpr uint32_t kFrameHasSector = 0x00080000;
constexpr uint32_t kFrameSectorInit = 0x08000000;

bool IsFogType(int type) { return type >= I3DLIGHT_FOG_FIRST && type <= I3DLIGHT_FOG_LAST; }

const S_vector &AsVector(const S_vector4 &v) { return reinterpret_cast<const S_vector &>(v); }

template <class T> bool Contains(const C_vector<T> &list, const T &value)
{
    for (const T &v : list) {
        if (v == value)
            return true;
    }
    return false;
}

uint32_t BitsOf(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

float FloatOf(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// The parameters' clamps: the int ones keep the upper limit first, the float ones keep NaNs.
uint32_t ClampInt(uint32_t value, int lo, int hi)
{
    int v = static_cast<int>(value);
    if (v > hi)
        v = hi;
    if (v < lo)
        v = lo;
    return static_cast<uint32_t>(v);
}

uint32_t ClampUnit(uint32_t value)
{
    float f = FloatOf(value);
    if (0.0f > f)
        f = 0.0f;
    if (1.0f < f)
        f = 1.0f;
    return BitsOf(f);
}

// A pan vector: shortened to length 1 when longer (the fourth dword copied as it is).
S_vector4 ClampPan(const S_vector4 &pan)
{
    S_vector4 v = pan;
    const float sq = SquareZYX(AsVector(v));
    if (!IsTinyBits(sq) && !(std::fabs(sq - 1.0f) < kEpsilon)) {
        const float length = std::sqrt(sq);
        if (length > 1.0f) {
            const float inv = 1.0f / length;
            v.x = v.x * inv;
            v.y = v.y * inv;
            v.z = v.z * inv;
        }
    }
    return v;
}

void OutOfMemory()
{
    ReportError(I3DERR_OUTOFMEM, kSevFatal);
}

}  // namespace

void I3D_environment::SetDefaults()
{
    // The EAX 3 defaults normalized to the parameters' ranges (room 0 mB, room HF -100 of
    // -10000 - 0, decay HF ratio 0.83 of 0.1 - 2, reflections -2602 of -10000 - 1000, reverb 200 of
    // -10000 - 2000, decay LF ratio 1, air absorption -5 of -100 - 0, HF / LF reference 5000 / 250
    // Hz of 1000 - 20000 / 20 - 1000).
    std::memset(this, 0, sizeof *this);
    params[I3DENV_ROOM] = BitsOf(1.0f);
    params[I3DENV_ROOM_HF] = BitsOf(0.99f);
    params[I3DENV_DECAY_TIME] = 1490;
    params[I3DENV_DECAY_HF_RATIO] = BitsOf(0.384210527f);
    params[I3DENV_REFLECTIONS] = BitsOf(0.672545433f);
    params[I3DENV_REFLECTIONS_DELAY] = 7;
    params[I3DENV_REVERB] = BitsOf(0.850000024f);
    params[I3DENV_REVERB_DELAY] = 10;
    params[I3DENV_DIFFUSION] = BitsOf(1.0f);
    params[I3DENV_FADE_TIME] = 500;
    params[I3DENV_DECAY_LF_RATIO] = BitsOf(0.473684222f);
    params[I3DENV_ECHO_TIME] = 250;
    params[I3DENV_MODULATION_TIME] = 250;
    params[I3DENV_AIR_ABSORPTION_HF] = BitsOf(0.949999988f);
    params[I3DENV_HF_REFERENCE] = BitsOf(0.210526317f);
    params[I3DENV_LF_REFERENCE] = BitsOf(0.234693885f);
}

// ---------------------------------------------------------------- the frame slots

void I3D_sector::Init()
{
    I3D_frame::Init();
    scene = nullptr;
    numVertices = 0;
    vertices = nullptr;
    numFaces = 0;
    faces = nullptr;
    sectorFlags = 0x800 | 0x1;
    numPlanes = 0;
    planes = nullptr;
    lightChanges = 0;
    fogLight = nullptr;
    fogLightCount = 0;
    ambientColor = 0;
    ambientColor2 = 0;
    prevInScene = nullptr;
    nextInScene = nullptr;
    occluders = nullptr;
    occluder = nullptr;
    frameFlags |= kFrameHasSector;
    environmentBlend = 0.0f;
    environmentTime = 0;
    environment.SetDefaults();
    frameFlags |= kFrameSectorInit;
    environmentId = 0;
    ++g_sectorChanges;
    renderVisuals = nullptr;
    invWorldMatrix.Identity();
}

void I3D_sector::Uninit()
{
    if (g_envSector == this)
        g_envSector = nullptr;
    if (g_envPrevSector == this)
        g_envPrevSector = nullptr;
    ++g_sectorChanges;
    SetScene(nullptr);
    if (occluder) {
        occluder->ownerSector = nullptr;
        occluder = nullptr;
    }
    for (I3D_light *light : lights)
        light->sectors.RemoveSwap(this);
    lights.Clear();
    for (int n = sounds.Size(); n; --n)
        RemoveSound(sounds[0]);
    sounds.Clear();
    FreeGeometry();
    I3D_frame::Uninit();
}

I3D_RESULT I3D_sector::SetName(const char *newName)
{
    if (!newName)
        return I3D_frame::SetName(nullptr);
    // (The original's static buffer, 0x100a6be0: its last bytes are never written.)
    static char buffer[0x4C] = "sector ";
    std::strncpy(buffer + 7, newName, 0x40);
    return I3D_frame::SetName(buffer);
}

I3D_RESULT I3D_sector::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    frameFlags |= kFrameHasSector;
    if (source->GetType() != FRAME_SECTOR)
        return I3D_OK;
    const auto *src = static_cast<const I3D_sector *>(source);
    FreeGeometry();
    numVertices = src->numVertices;
    if (numVertices) {
        vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVertices, 16));
        if (!vertices) {
            numVertices = 0;
            OutOfMemory();
            return I3DERR_OUTOFMEM;
        }
        std::memcpy(vertices, src->vertices, sizeof(S_vector4) * numVertices);
        numFaces = src->numFaces;
        if (numFaces) {
            faces = static_cast<I3D_triface *>(::operator new(sizeof(I3D_triface) * numFaces, std::nothrow));
            if (faces) {
                std::memcpy(faces, src->faces, sizeof(I3D_triface) * numFaces);
                // (The planes come with MatrixChanged.)
                planes = static_cast<S_plane *>(AlignedAlloc(sizeof(S_plane) * numFaces, 16));
            }
            if (!faces || !planes) {
                numFaces = 0;
                OutOfMemory();
                return I3DERR_OUTOFMEM;
            }
        }
    }
    portals.Reserve(src->portals.Size());
    for (I3D_portal *from : src->portals) {
        // (The original goes on with a null portal when out of memory.)
        auto *portal = new I3D_portal;
        if (!portal) {
            OutOfMemory();
            return I3DERR_OUTOFMEM;
        }
        portal->Duplicate(from);
        portal->owner = this;
        portals.PushBack(portal);
    }
    box = src->box;
    sectorFlags = (src->sectorFlags & SCTF_DUPLICATE_MASK) | SCTF_ENV_CHANGED;
    environmentId = src->environmentId;
    return I3D_OK;
}

void I3D_sector::MatrixChanged()
{
    ComputePlanes();
    for (I3D_portal *portal : portals)
        portal->MatrixChanged();
    if (!scene && parent) {
        for (I3D_frame *f = parent; f; f = f->parent) {
            if (f->GetType() == FRAME_SECTOR) {
                SetScene(static_cast<I3D_sector *>(f)->scene);
                break;
            }
        }
    }
    sectorFlags &= ~SCTF_INVMATRIX_VALID;
}

void I3D_sector::Load()
{
    I3D_frame::Load();
    // sectorFlags, environmentId, numVertices, numFaces and the box.
    dtaRead(g_loadFile, &sectorFlags, 0x30);
    vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVertices, 16));
    faces = static_cast<I3D_triface *>(::operator new(sizeof(I3D_triface) * numFaces, std::nothrow));
    planes = static_cast<S_plane *>(AlignedAlloc(sizeof(S_plane) * numFaces, 16));
    if (!vertices || !faces || !planes) {
        AlignedFree(vertices);
        vertices = nullptr;
        ::operator delete(faces);
        faces = nullptr;
        AlignedFree(planes);
        planes = nullptr;
        g_loadResult = I3DERR_OUTOFMEM;
        OutOfMemory();
        return;
    }
    dtaRead(g_loadFile, vertices, sizeof(S_vector4) * numVertices);
    dtaRead(g_loadFile, faces, sizeof(I3D_triface) * numFaces);
    int count = 0;
    dtaRead(g_loadFile, &count, 1);
    portals.Reserve(count);
    while (count) {
        --count;
        auto *portal = new I3D_portal;
        if (!portal) {
            g_loadResult = I3DERR_OUTOFMEM;
            OutOfMemory();
            return;
        }
        portal->owner = this;
        portal->Load();
        portals.PushBack(portal);
    }
    sectorFlags &= ~SCTF_INVMATRIX_VALID;
}

// ---------------------------------------------------------------- lights and sounds

void I3D_sector::AddLight(I3D_light *light)
{
    if (Contains(lights, light))
        return;
    if (light->lightType == I3DLIGHT_AMBIENT) {
        sectorFlags |= SCTF_AMBIENT_CHANGED;
    } else if (IsFogType(light->lightType)) {
        ++fogLightCount;
        if (light->frameFlags & FRMFLAGS_ON)
            fogLight = light;
    }
    ++lightChanges;
    lights.PushBack(light);
    if (!Contains(light->sectors, this))
        light->sectors.PushBack(this);
}

I3D_RESULT I3D_sector::RemoveLight(I3D_light *light)
{
    if (!Contains(lights, light))
        return I3DERR_OBJECTNOTFOUND;
    if (light->lightType == I3DLIGHT_AMBIENT) {
        sectorFlags |= SCTF_AMBIENT_CHANGED;
    } else if (IsFogType(light->lightType)) {
        --fogLightCount;
        fogLight = nullptr;
        if (fogLightCount) {
            // The first fog light that is on. (The light going is still in the list here: when on
            // and first, it stays the fog light. Kept.)
            for (I3D_light *l : lights) {
                if ((l->frameFlags & FRMFLAGS_ON) && IsFogType(l->lightType)) {
                    fogLight = l;
                    break;
                }
            }
        }
    }
    ++lightChanges;
    light->sectors.RemoveSwap(this);
    lights.RemoveSwap(light);
    return I3D_OK;
}

void I3D_sector::AddSound(I3D_sound *sound)
{
    if (Contains(sounds, sound))
        return;
    sounds.PushBack(sound);
    if (!Contains(sound->sectors, this))
        sound->sectors.PushBack(this);
    if (sound->frameFlags & FRMFLAGS_ON)
        sound->SetOn(true);
}

I3D_RESULT I3D_sector::RemoveSound(I3D_sound *sound)
{
    if (!Contains(sounds, sound))
        return I3DERR_OBJECTNOTFOUND;
    if (sound->sectors.RemoveSwap(this)) {
        // Leaving the camera's sector while still in others: the sound is switched off.
        if (scene && scene->activeCamera && scene->activeCamera->sector == this && sound->sectors.Size() > 0)
            sound->SetOn(false);
    }
    sounds.RemoveSwap(sound);
    return I3D_OK;
}

// ---------------------------------------------------------------- the sound environment

I3D_RESULT I3D_sector::SetEnvironmentParam(int index, uintptr_t value)
{
    // (The original's range check is signed and its jump table's unsigned: a negative index
    // wrote before the block. Refused.)
    if (index < 0 || index >= I3DENV_COUNT)
        return I3DERR_INVALIDPARAMS;
    uint32_t bits = static_cast<uint32_t>(value);
    switch (index) {
    case I3DENV_DECAY_TIME: bits = ClampInt(bits, 100, 20000); break;
    case I3DENV_REFLECTIONS_DELAY: bits = ClampInt(bits, 0, 300); break;
    case I3DENV_REVERB_DELAY: bits = ClampInt(bits, 0, 100); break;
    case I3DENV_FADE_TIME: break;
    case I3DENV_ECHO_TIME: bits = ClampInt(bits, 75, 250); break;
    case I3DENV_MODULATION_TIME: bits = ClampInt(bits, 39, 4000); break;
    case I3DENV_REFLECTIONS_PAN:
        environment.reflectionsPan = ClampPan(*reinterpret_cast<const S_vector4 *>(value));
        sectorFlags |= SCTF_ENV_CHANGED;
        return I3D_OK;
    case I3DENV_REVERB_PAN:
        environment.reverbPan = ClampPan(*reinterpret_cast<const S_vector4 *>(value));
        sectorFlags |= SCTF_ENV_CHANGED;
        return I3D_OK;
    default: bits = ClampUnit(bits); break;
    }
    environment.params[index] = bits;
    sectorFlags |= SCTF_ENV_CHANGED;
    return I3D_OK;
}

uintptr_t I3D_sector::GetEnvironmentParam(int index)
{
    if (index == I3DENV_REFLECTIONS_PAN)
        return reinterpret_cast<uintptr_t>(&environment.reflectionsPan);
    if (index == I3DENV_REVERB_PAN)
        return reinterpret_cast<uintptr_t>(&environment.reverbPan);
    // (The original reads any index as a dword of the block; outside the sector: 0.)
    const ptrdiff_t offset = offsetof(I3D_sector, environment) + static_cast<ptrdiff_t>(index) * 4;
    if (offset < 0 || offset + 4 > static_cast<ptrdiff_t>(sizeof(I3D_sector)))
        return 0;
    uint32_t bits;
    std::memcpy(&bits, reinterpret_cast<const uint8_t *>(this) + offset, sizeof bits);
    return bits;
}

void I3D_sector::SetEnvironment(int id)
{
    environmentId = id;
    ++lightChanges;
}

// ---------------------------------------------------------------- the hull

bool I3D_sector::IsInside(const S_vector &pos)
{
    const I3D_bbox &b = GetBound().bbox;
    if (!(b.min.x <= b.max.x))
        return false;
    // (NaN coordinates pass the box.)
    if (b.min.x > pos.x || b.max.x < pos.x || b.min.y > pos.y || b.max.y < pos.y || b.min.z > pos.z ||
        b.max.z < pos.z)
        return false;
    for (int i = 0; i < numPlanes; ++i) {
        const S_plane &p = planes[i];
        const float d = ((p.normal.y * pos.y + p.normal.z * pos.z) + p.normal.x * pos.x) + p.d;
        if (d < -kEpsilon)
            return false;
    }
    return true;
}

float I3D_sector::InsideDistance(const S_vector &pos)
{
    const I3D_bbox &b = GetBound().bbox;
    if (!(b.min.x <= b.max.x))
        return -1e16f;
    if (b.min.x > pos.x || b.max.x < pos.x || b.min.y > pos.y || b.max.y < pos.y || b.min.z > pos.z ||
        b.max.z < pos.z)
        return -1e16f;
    float nearest = 1e16f;
    for (int i = 0; i < numPlanes; ++i) {
        const S_plane &p = planes[i];
        const float d = ((p.normal.y * pos.y + p.normal.z * pos.z) + p.normal.x * pos.x) + p.d;
        if (d < -kEpsilon)
            return -1e16f;
        if (d < nearest)
            nearest = d;
    }
    return nearest;
}

void I3D_sector::ComputePlanes()
{
    if ((sectorFlags & SCTF_NO_PLANES) || !planes)
        return;
    numPlanes = 0;
    const S_matrix &m = GetMatrix();
    for (int i = 0; i < numFaces; ++i) {
        const I3D_triface &face = faces[i];
        const S_vector a = AsVector(vertices[face.v[0]]) * m;
        const S_vector b = AsVector(vertices[face.v[1]]) * m;
        const S_vector c = AsVector(vertices[face.v[2]]) * m;
        // (0x1000c6a0)
        S_vector n = (c - b).Cross(a - b);
        NormalizeWithSquare(n, SquareZYX(n));
        const float d = -a.Dot(n);
        bool known = false;
        for (int k = 0; k < numPlanes; ++k) {
            const S_plane &p = planes[k];
            if (std::fabs(p.d - d) < 0.001f && std::fabs(p.normal.x - n.x) < 0.001f &&
                std::fabs(p.normal.y - n.y) < 0.001f && std::fabs(p.normal.z - n.z) < 0.001f) {
                known = true;
                break;
            }
        }
        if (!known) {
            planes[numPlanes].normal = n;
            planes[numPlanes].d = d;
            ++numPlanes;
        }
    }
}

void I3D_sector::FreeGeometry()
{
    AlignedFree(planes);
    planes = nullptr;
    // (The original keeps the plane count: IsInside then walked a freed or, after Duplicate, a
    // new and maybe shorter array until the next MatrixChanged.)
    numPlanes = 0;
    for (I3D_portal *portal : portals) {
        if (I3D_sector *to = portal->connectedSector)
            to->incomingPortals.RemoveSwap(portal);
        delete portal;
    }
    portals.Clear();
    for (I3D_portal *portal : incomingPortals)
        portal->connectedSector = nullptr;
    incomingPortals.Clear();
    AlignedFree(vertices);
    vertices = nullptr;
    ::operator delete(faces);
    faces = nullptr;
    numVertices = 0;
    numFaces = 0;
}

// ---------------------------------------------------------------- scene and occluder

void I3D_sector::SetScene(I3D_scene *newScene)
{
    if (scene == newScene)
        return;
    // (The primary sector has its scene without being in the ring.)
    if (scene && prevInScene) {
        --scene->sectorCount;
        if (scene->firstSector == this)
            scene->firstSector = nextInScene;
        if (prevInScene == this) {
            scene->firstSector = nullptr;
        } else {
            prevInScene->nextInScene = nextInScene;
            nextInScene->prevInScene = prevInScene;
        }
        scene->portalChangesSeen = 0;
        scene = nullptr;
    }
    scene = newScene;
    if (newScene) {
        I3D_sector *const first = newScene->firstSector;
        if (!first) {
            prevInScene = this;
            nextInScene = this;
            newScene->firstSector = this;
        } else {
            first->prevInScene->nextInScene = this;
            prevInScene = first->prevInScene;
            nextInScene = first;
            first->prevInScene = this;
        }
        ++newScene->sectorCount;
        newScene->portalChangesSeen = 0;
        if (occluder && occluder->parent == this)
            occluder->LinkTo(newScene->primarySector, I3DLINK_UPDMATRIX);
    }
    ++g_sectorChanges;
    for (I3D_portal *portal : portals)
        portal->SetScene(scene);
}

void I3D_sector::SetBox(const I3D_bbox &newBox)
{
    box = newBox;
    frameFlags &= ~FRMFLAGS_BOUND_VALID;
}

void I3D_sector::MakeReady()
{
    if (!numVertices)
        return;
    if ((sectorFlags & SCTF_MAKE_OCCLUDER) && !occluder) {
        auto *const occ = static_cast<I3D_occluder *>(g_driver->CreateFrame(FRAME_OCCLUDER));
        if (!occ) {
            dbgPrintf(" !! Out of memory !!  I3D_sector::MakeReady() [2]");
            MatrixChanged();
            return;
        }
        occ->Build(vertices, numVertices, faces, numFaces, sizeof(S_vector4));
        if (ownerModel) {
            if (ownerModel->GetType() == FRAME_SCENE)
                SceneAddFrame(ownerModel, occ);
            else
                ModelAddFrame(ownerModel, occ);
            occ->Release();
        }
        if (scene) {
            // In the primary sector, where the sector is.
            const S_matrix &m = GetMatrix();
            occ->localMatrix = m;
            occ->scale = m.GetScale();      // (the original copies a 4th, unset value into scaleW)
            occ->frameFlags = (occ->frameFlags & ~(FRMFLAGS_ROT_QUAT_VALID | FRMFLAGS_WMATRIX_VALID)) |
                              FRMFLAGS_CHANGED | FRMFLAGS_ROT_VALID | FRMFLAGS_LMATRIX_VALID;
            occ->LinkTo(scene->primarySector, 0);
        } else {
            occ->LinkTo(this, 0);
        }
        occ->Update();
        occluder = occ;
        occ->ownerSector = this;
    }
    MatrixChanged();
}
