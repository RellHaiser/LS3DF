// I3D_area (0x10005550 - 0x10005f80).
#include "i3d/area.h"

#include <cstring>
#include <new>

#include "core/debug.h"
#include "i3d/driver.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/sound.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

const S_vector &AsVector(const S_vector4 &v) { return reinterpret_cast<const S_vector &>(v); }

template <class T> bool Contains(const C_vector<T> &list, const T &value)
{
    for (const T &v : list) {
        if (v == value)
            return true;
    }
    return false;
}

}  // namespace

void I3D_area::Init()
{
    I3D_frame::Init();
    nextInScene = nullptr;
    prevInScene = nullptr;
    numVertices = 0;
    vertices = nullptr;
    worldVertices = nullptr;
    numFaces = 0;
    faces = nullptr;
    planes = nullptr;
    scene = nullptr;
    frameFlags |= 0x00040000;               // (FRMFLAGS_SECTOR_MASK: the hierarchy holds an area)
    testedChangeCount = 0;
    fadeTime = 500;
}

void I3D_area::Uninit()
{
    for (int n = sounds.Size(); n; --n)
        RemoveSound(sounds[0]);
    sounds.Clear();
    // (The pointers stay until Init.)
    AlignedFree(vertices);
    AlignedFree(worldVertices);
    ::operator delete(faces);
    AlignedFree(planes);
    SetScene(nullptr);
    I3D_frame::Uninit();
}

void I3D_area::SetScene(I3D_scene *newScene)
{
    if (scene && prevInScene) {
        if (scene->firstArea == this)
            scene->firstArea = nextInScene;
        if (prevInScene == this) {
            scene->firstArea = nullptr;
        } else {
            prevInScene->nextInScene = nextInScene;
            nextInScene->prevInScene = prevInScene;
        }
        --scene->areaCount;
        scene->areasChanged = true;
        scene = nullptr;
    }
    if (newScene) {
        scene = newScene;
        I3D_area *const first = newScene->firstArea;
        if (!first) {
            prevInScene = this;
            nextInScene = this;
            newScene->firstArea = this;
        } else {
            first->prevInScene->nextInScene = this;
            prevInScene = first->prevInScene;
            nextInScene = first;
            first->prevInScene = this;
        }
        ++newScene->areaCount;
        newScene->areasChanged = true;
    }
}

I3D_RESULT I3D_area::LinkTo(I3D_frame *newParent, uint32_t flags)
{
    if (newParent == parent)
        return I3D_OK;
    SetScene(nullptr);
    if (newParent) {
        I3D_frame *const owner = newParent->ownerModel;
        I3D_scene *newScene = nullptr;
        if (owner && owner->GetType() == FRAME_SCENE) {
            newScene = static_cast<I3D_scene *>(owner);
        } else {
            // The scene of the first sector up. (The original loops for ever at a sector without
            // a scene and reads through null above the last frame: both end the search here.)
            for (I3D_frame *f = newParent; f; f = f->parent) {
                if (f->GetType() == FRAME_SECTOR) {
                    newScene = static_cast<I3D_sector *>(f)->scene;
                    break;
                }
            }
        }
        SetScene(newScene);
    }
    I3D_frame::LinkTo(newParent, flags);
    return I3D_OK;
}

I3D_RESULT I3D_area::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    const I3D_RESULT result = I3D_frame::Duplicate(source);
    if (source->GetType() == FRAME_AREA) {
        const auto *src = static_cast<const I3D_area *>(source);
        Build(src->vertices, src->numVertices, src->faces, src->numFaces, sizeof(S_vector4));
    }
    return result;
}

void I3D_area::MatrixChanged()
{
    if (!numVertices)
        return;
    for (int i = 0; i < numVertices; ++i) {
        // (The original copies 16 bytes of the 12-byte product: the fourth is stack garbage.)
        const S_vector w = AsVector(vertices[i]) * GetMatrix();
        worldVertices[i].x = w.x;
        worldVertices[i].y = w.y;
        worldVertices[i].z = w.z;
    }
    for (int k = 0; k < numFaces; ++k) {
        const I3D_triface &f = faces[k];
        const S_vector &a = AsVector(worldVertices[f.v[0]]);
        const S_vector &b = AsVector(worldVertices[f.v[1]]);
        const S_vector &c = AsVector(worldVertices[f.v[2]]);
        S_vector n = (c - b).Cross(a - b);
        NormalizeWithSquare(n, SquareXZY(n));
        planes[k].d = -n.Dot(a);
        planes[k].normal = n;
    }
}

void I3D_area::Load()
{
    dbgPrintf("** nepodporovany typ:%s  LoadImage()  %s", g_driver->GetFrameTypeName(type), name ? name : "");
}

I3D_RESULT I3D_area::Build(const void *verts, int numVerts, const I3D_triface *tris, int numTris, int stride)
{
    // (The original frees its arrays first: duplicating itself read them freed. They go after
    // the copy.)
    S_vector4 *const oldVertices = vertices;
    S_vector4 *const oldWorldVertices = worldVertices;
    I3D_triface *const oldFaces = faces;
    S_plane *const oldPlanes = planes;
    vertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVerts, 16));
    worldVertices = static_cast<S_vector4 *>(AlignedAlloc(sizeof(S_vector4) * numVerts, 16));
    faces = static_cast<I3D_triface *>(::operator new(sizeof(I3D_triface) * numTris, std::nothrow));
    planes = static_cast<S_plane *>(AlignedAlloc(sizeof(S_plane) * numTris, 16));
    if (vertices && worldVertices && faces && planes) {
        numVertices = numVerts;
        const auto *src = static_cast<const uint8_t *>(verts);
        for (int i = 0; i < numVerts; ++i, src += stride)
            std::memcpy(&vertices[i], src, sizeof(S_vector4));
        numFaces = numTris;
        std::memcpy(faces, tris, sizeof(I3D_triface) * numTris);
    }
    AlignedFree(oldVertices);
    AlignedFree(oldWorldVertices);
    ::operator delete(oldFaces);
    AlignedFree(oldPlanes);
    if (vertices && worldVertices && faces && planes)
        return I3D_OK;
    dbgPrintf("!! Out of memory  I3D_area::Build()");
    ReportError(I3DERR_OUTOFMEM, kSevFatal);
    return I3DERR_OUTOFMEM;
}

void I3D_area::AddSound(I3D_sound *sound)
{
    testedChangeCount = -1;
    if (Contains(sounds, sound))
        return;
    sounds.PushBack(sound);
    if (!Contains(sound->areas, this))
        sound->areas.PushBack(this);
}

I3D_RESULT I3D_area::RemoveSound(I3D_sound *sound)
{
    testedChangeCount = -1;
    if (!Contains(sounds, sound))
        return I3DERR_OBJECTNOTFOUND;
    sound->areas.RemoveSwap(this);
    sounds.RemoveSwap(sound);
    return I3D_OK;
}

bool I3D_area::IsInside(const S_vector &pos)
{
    for (int k = 0; k < numFaces; ++k) {
        const S_plane &p = planes[k];
        if (((p.normal.z * pos.z + p.normal.y * pos.y) + p.normal.x * pos.x) + p.d > 0.0f)
            return false;
    }
    return true;
}

uint32_t I3D_area::SetFadeTime(uint32_t value)
{
    fadeTime = value > 1 ? value : 1;
    return fadeTime;
}

uint32_t I3D_area::GetFadeTime() { return fadeTime; }
