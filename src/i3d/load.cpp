// The file loaders (load.cpp, 0x1005d170 - 0x1005e6c0): model / scene files (.4ds), animation
// files (.5ds), shadow files (.6ds) and the meshes in model files.
//
// A model file holds the materials, then the frames in hierarchy order (each with the index of its
// parent among the frames read before it); each frame reads its own data (slot 0x70), meshes
// through LoadMesh. The frames are kept in g_loadFrames until the model or scene takes them.
#include "i3d/loader.h"

#include <cstring>

#include "core/debug.h"
#include "external/rw_data.h"
#include "i3d/anim.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/model.h"
#include "i3d/object.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/shadow.h"
#include "i3d/target.h"
#include "i3d/visual.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "platform/platform.h"

using namespace ls3d;
using namespace ls3d::i3d;

extern "C" const char *I3DGetResultMessage(int32_t result);     // core/version.cpp

namespace ls3d::i3d {

int g_loadFile = -1;
HRESULT g_loadResult;
C_vector<I3D_target *> g_loadTargets;
const char *g_loadFileName;
I3D_material *g_loadMaterials[6000];
int g_loadNumMaterials;
I3D_frame *g_loadFrames[10000];
int g_loadNumFrames;

}  // namespace ls3d::i3d

namespace {

// The load callback: (8, progress 0 - 1, 0 start / 1 progress / 2 end, context).
typedef uint32_t __stdcall LoadCallback(uint32_t message, float progress, uint32_t state, void *context);

bool s_modelLoading;                        // 0x100d00a8 a model, scene or shadow file is being read
bool s_animLoading;                         // 0x100d00a9 an animation file is being read
LoadCallback *s_callback;                   // 0x100c0664
void *s_callbackContext;                    // 0x100d00a4
uint32_t s_loadFlags;                       // 0x100c0668 bit 1: progress messages
float s_progress;                           // 0x100c0660
float s_progressSent;                       // 0x100ca2c4
uint32_t s_hasAnimation;                    // 0x100d00a0 the file asks for its animation (.5ds)

// Waits up to 4 s for another load to end.
bool WaitFor(const bool &busy)
{
    if (!busy)
        return true;
    const DWORD start = platform::Milliseconds();
    do {
        g_pIGraph->ProcessWinMessages();
        if (platform::Milliseconds() - start > 4000)
            return false;
    } while (busy);
    return true;
}

// `fileName` with its extension (after the last '.' following the last '\') replaced by `ext`.
void ReplaceExtension(char (&out)[0x114], const char *fileName, const char *ext)
{
    size_t dot = 0, i = 0;
    for (const char *p = fileName; *p; ++p, ++i) {
        if (*p == '.')
            dot = i;
        else if (*p == '\\')
            dot = 0;
    }
    if (!dot)
        dot = i;
    std::memcpy(out, fileName, dot);
    std::strcpy(out + dot, ext);
}

// The header: the magic and version, then the file time (checked against `time` when given).
HRESULT ReadHeader(int file, const char *magic, int version, const FILETIME *time)
{
    char tag[4] = {};
    dtaRead(file, tag, 4);
    if (std::memcmp(tag, magic, 4)) {
        dtaClose(file);
        return I3DERR_FILECORRUPTED;
    }
    int v = 0;
    dtaRead(file, &v, 2);
    if (v != version) {
        dtaClose(file);
        return I3DERR_BADVERSION;
    }
    FILETIME ft;
    dtaRead(file, &ft, 8);
    if (time && CompareFileTime(time, &ft)) {
        dtaClose(file);
        return I3DERR_BADTIMESTAMP;
    }
    return I3D_OK;
}

// 0x1005d210: a progress message every 2 %.
void Progress(float progress)
{
    if (!(s_progressSent + 0.02f > progress) && (s_loadFlags & 2)) {
        s_progressSent = progress;
        s_progress = progress;
        s_callback(8, progress, 1, s_callbackContext);
    }
}

// 0x1005d970: the model file's materials and frames.
HRESULT ReadModelFile(const char *fileName)
{
    g_loadNumFrames = 0;
    g_loadNumMaterials = 0;
    s_hasAnimation = 0;
    g_loadTargets.SetCount(0);
    if (!fileName || !*fileName)
        return I3DERR_INVALIDPARAMS;
    g_loadFile = dtaOpen(fileName, false);
    if (g_loadFile < 0)
        return I3DERR_NOFILE;
    const HRESULT header = ReadHeader(g_loadFile, "4DS", 0x29, nullptr);
    if (header < 0)
        return header;
    g_loadResult = I3D_OK;
    g_loadFileName = fileName;
    dtaRead(g_loadFile, &g_loadNumMaterials, 2);
    if (g_loadNumMaterials > 6000) {
        // (The file stays open.)
        g_loadResult = I3DERR_LIMITEXCEEDED;
        return I3DERR_LIMITEXCEEDED;
    }
    int i = 0;
    for (; i < g_loadNumMaterials; ++i) {
        Progress(static_cast<float>(i / 2) / static_cast<float>(g_loadNumMaterials));
        uint32_t flags;
        dtaRead(g_loadFile, &flags, 4);
        I3D_material *m;
        if (flags & MATF_ANIMATED) {
            I3D_anim_material *am = g_driver->CreateAnimMaterial();
            if (!am) {
                g_loadResult = I3DERR_OUTOFMEM;
                break;
            }
            am->matFlags = flags;
            am->Load();
            m = am;
        } else {
            m = g_driver->CreateMaterial();
            if (!m) {
                g_loadResult = I3DERR_OUTOFMEM;
                break;
            }
            m->matFlags = flags;
            m->Load();
        }
        g_loadMaterials[i] = m;
        // (A material that failed to load is kept but not counted: it is never released.)
        if (g_loadResult)
            break;
    }
    g_loadNumMaterials = i;
    if (!g_loadResult) {
        dtaRead(g_loadFile, &g_loadNumFrames, 2);
        // (The original has no limit: more than 10000 frames overran its array.)
        if (g_loadNumFrames > 10000) {
            g_loadNumFrames = 0;
            g_loadResult = I3DERR_LIMITEXCEEDED;
        }
        int f = 0;
        for (; f < g_loadNumFrames; ++f) {
            Progress(static_cast<float>(f / 2) / static_cast<float>(g_loadNumFrames) + 0.5f);
            int type = 0;
            dtaRead(g_loadFile, &type, 1);
            I3D_frame *frame;
            if (type == FRAME_VISUAL) {
                int visualType = 0;
                dtaRead(g_loadFile, &visualType, 1);
                frame = CreateVisualObject(visualType);
                if (!frame) {
                    g_loadResult = I3DERR_GENERIC;
                    break;
                }
                auto *v = static_cast<I3D_visual *>(frame);
                dtaRead(g_loadFile, &v->visualFlags, 2);
                v->visualFlags &= ~VISF_INVMATRIX_VALID;
            } else if (type < 0x11) {
                frame = CreateFrameObject(type);
                if (!frame) {
                    g_loadResult = I3DERR_GENERIC;
                    break;
                }
            } else {
                g_loadResult = I3DERR_FILECORRUPTED;
                break;
            }
            int parentIndex = 0;
            dtaRead(g_loadFile, &parentIndex, 2);
            if (parentIndex) {
                if (parentIndex > f) {
                    g_loadResult = I3DERR_FILECORRUPTED;
                    break;
                }
                frame->LinkTo(g_loadFrames[parentIndex - 1], 0);
            }
            g_loadFrames[f] = frame;
            frame->Load();
            if (g_loadResult)
                break;
        }
        g_loadNumFrames = f;
        dtaRead(g_loadFile, &s_hasAnimation, 1);
    }
    // The targets' frame indices become frames.
    for (I3D_target *t : g_loadTargets) {
        auto *entry = reinterpret_cast<intptr_t *>(t->frames.Data());
        for (int k = t->frames.Size(); k; --k) {
            const intptr_t index = *entry;
            if (index > g_loadNumFrames) {
                g_loadResult = I3DERR_FILECORRUPTED;
                *entry = 0;
                t->RemoveFrame(nullptr);
            } else if (!index) {
                t->RemoveFrame(nullptr);
            } else {
                *entry = reinterpret_cast<intptr_t>(g_loadFrames[index - 1]);
                g_loadFrames[index - 1]->frameFlags |= FRMFLAGS_TARGETED;
                ++entry;
            }
        }
    }
    dtaClose(g_loadFile);
    g_loadFile = -1;
    for (int k = 0; k < g_loadNumMaterials; ++k)
        g_loadMaterials[k]->Release();
    for (int k = 0; k < g_loadNumFrames; ++k) {
        I3D_frame *f = g_loadFrames[k];
        if (f->type != FRAME_VISUAL || !(static_cast<I3D_visual *>(f)->visualFlags & VISF_OBJECT_PARAMS))
            continue;
        const uint32_t type = f->GetVisualType();
        if (type == FourCC("SNGM") || type == FourCC("SMMR"))
            SkinFixJoints(f);
    }
    return g_loadResult;
}

// 0x1005e580: an animation file's blob (16-byte aligned).
HRESULT ReadAnimationFile(const char *fileName, uint8_t **blob, const FILETIME *time)
{
    if (!fileName)
        return I3DERR_INVALIDPARAMS;
    const int file = dtaOpen(fileName, false);
    if (file < 0)
        return I3DERR_NOFILE;
    const HRESULT header = ReadHeader(file, "5DS", 0x7a, time);
    if (header < 0)
        return header;
    int size = 0;
    dtaRead(file, &size, 4);
    auto *data = static_cast<uint8_t *>(AlignedAlloc(static_cast<size_t>(size), 16));
    if (!data) {
        dtaClose(file);
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    dtaRead(file, data, size);
    dtaClose(file);
    *blob = data;
    return I3D_OK;
}

// 0x1005e1a0: a shadow file: its vertices, indices and parts.
HRESULT ReadShadowFile(S_shadowData *data, const char *fileName, const FILETIME *time)
{
    if (!fileName || !*fileName)
        return I3DERR_INVALIDPARAMS;
    g_loadFile = dtaOpen(fileName, false);
    if (g_loadFile < 0)
        return I3DERR_NOFILE;
    const HRESULT header = ReadHeader(g_loadFile, "6DS", 0xc, time);
    if (header < 0)
        return header;
    // The counts (0x100d00d4): vertices, indices, parts.
    static uint16_t s_counts[3];
    dtaRead(g_loadFile, s_counts, 6);
    if (s_counts[0]) {
        data->vertices.Reserve(s_counts[0]);
        dtaRead(g_loadFile, data->vertices.Data(), s_counts[0] * 16);
        data->vertices.SetCount(s_counts[0]);
    }
    if (s_counts[1]) {
        data->indices.Reserve(s_counts[1]);
        dtaRead(g_loadFile, data->indices.Data(), s_counts[1] * 2);
        data->indices.SetCount(s_counts[1]);
    }
    if (s_counts[2]) {
        data->parts.Reserve(s_counts[2]);
        data->parts.SetCount(s_counts[2]);
        S_shadowPart *p = data->parts.Data();
        for (; s_counts[2]; --s_counts[2], ++p) {
            dtaRead(g_loadFile, &p->numVertices, 8);
            int length = 0;
            dtaRead(g_loadFile, &length, 1);
            p->name = static_cast<char *>(::operator new(static_cast<size_t>(length) + 1));
            if (!p->name) {
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return I3DERR_OUTOFMEM;
            }
            if (length)
                dtaRead(g_loadFile, p->name, length);
            p->name[length] = 0;
        }
        s_counts[2] = 0xffff;
    }
    dtaClose(g_loadFile);
    g_loadFile = -1;
    return I3D_OK;
}

}  // namespace

namespace ls3d::i3d {

// 0x1005d260
I3D_RESULT OpenModelFile(const char *fileName, uint32_t flags, void *callback, void *context, I3D_frame *root)
{
    if (!root || !fileName || !*fileName)
        return I3DERR_INVALIDPARAMS;
    if (!WaitFor(s_modelLoading))
        return I3DERR_GENERIC;
    s_modelLoading = true;
    s_callbackContext = context;
    s_callback = static_cast<LoadCallback *>(callback);
    s_loadFlags = flags;
    if (!callback)
        s_loadFlags = flags & 1;
    s_progressSent = 0.0f;
    if (s_loadFlags & 2) {
        s_progress = 0.0f;
        s_callback(8, s_progress, 0, context);
    }
    s_hasAnimation = 0;
    g_loadNumFrames = 0;
    char name[0x114];
    ReplaceExtension(name, fileName, ".4ds");
    ig::g_noActivation = true;
    HRESULT result = ReadModelFile(name);
    ig::g_noActivation = false;
    // The frames to the model (or the scene of the primary sector `root`); those without a
    // parent under the root.
    if (root->type == FRAME_MODEL) {
        auto *model = static_cast<I3D_model *>(root);
        for (I3D_frame **f = g_loadFrames; g_loadNumFrames; --g_loadNumFrames, ++f) {
            if (!(*f)->parent)
                (*f)->LinkTo(root, 0);
            model->AddFrame(*f);
            (*f)->Release();
        }
    } else {
        I3D_scene *scene = static_cast<I3D_sector *>(root)->scene;
        for (I3D_frame **f = g_loadFrames; g_loadNumFrames; --g_loadNumFrames, ++f) {
            if (!(*f)->parent)
                (*f)->LinkTo(root, 0);
            scene->AddFrame(*f);
            (*f)->Release();
        }
    }
    // (The count ends at -1.)
    --g_loadNumFrames;
    if (result == I3DERR_FILESYSTEM) {
        dtaDelete(name);
    } else if (result == I3D_OK) {
        if (s_hasAnimation) {
            I3D_animation_set *set = g_driver->CreateAnimationSet();
            if (!set) {
                if (s_loadFlags & 2) {
                    s_progress = 1.0f;
                    s_callback(8, s_progress, 2, s_callbackContext);
                }
                s_modelLoading = false;
                return I3DERR_OUTOFMEM;
            }
            result = set->Open(fileName, flags);
            if (result == I3D_OK) {
                set->defaultMode = 2;
                if (root->type == FRAME_MODEL)
                    static_cast<I3D_model *>(root)->SetAnimation(set, 0, 1.0f, 0);
                else
                    static_cast<I3D_sector *>(root)->scene->SetAnimation(set, 0, 1.0f, 0);
            }
            set->Release();
        }
        root->frameFlags = (root->frameFlags & ~FRMFLAGS_BOUND_VALID) | FRMFLAGS_CHANGED;
        root->Update();
    }
    if (s_loadFlags & 2) {
        s_progress = 1.0f;
        s_callback(8, s_progress, 2, s_callbackContext);
    }
    s_modelLoading = false;
    return result;
}

// 0x1005d5b0
I3D_RESULT OpenAnimationFile(const char *fileName, I3D_animation_set *set)
{
    if (!set || !fileName || !*fileName) {
        dbgPrintf("!! INVALIDPARAMS  load.cpp  ?::Open()  %s", fileName ? fileName : "fName:NULL");
        return I3DERR_INVALIDPARAMS;
    }
    if (!WaitFor(s_animLoading))
        return I3DERR_GENERIC;
    s_animLoading = true;
    g_pIGraph->ProcessWinMessages();
    s_callback = nullptr;
    s_loadFlags = 0;
    char name[0x114];
    ReplaceExtension(name, fileName, ".5ds");
    const HRESULT result = ReadAnimationFile(name, &set->data, nullptr);
    if (result == I3DERR_FILESYSTEM) {
        dtaDelete(name);
    } else if (result != I3D_OK && result != I3DERR_USERBREAK) {
        dbgPrintf("!! %s  load.cpp  I3D_animation_set::Open()  %s", I3DGetResultMessage(result), fileName);
    }
    s_animLoading = false;
    return result;
}

// 0x1005d740
I3D_RESULT OpenShadowFile(const char *fileName, uint32_t flags, S_shadowData *data)
{
    if (!data || !fileName || !*fileName)
        return I3DERR_INVALIDPARAMS;
    if (!WaitFor(s_modelLoading))
        return I3DERR_GENERIC;
    s_modelLoading = true;
    s_callback = nullptr;
    s_loadFlags = 0;
    char name[0x114];
    ReplaceExtension(name, fileName, ".6ds");
    const HRESULT result = ReadShadowFile(data, name, nullptr);
    s_modelLoading = false;
    return result;
}

// 0x1005ddd0
I3D_mesh *LoadMesh()
{
    // A mesh shared with an earlier frame: that frame's index.
    int shared = 0;
    dtaRead(g_loadFile, &shared, 2);
    if (shared) {
        I3D_mesh *m = static_cast<I3D_object *>(g_loadFrames[shared - 1])->mesh;
        ++m->refCount;
        return m;
    }
    int numLevels = 0;
    dtaRead(g_loadFile, &numLevels, 1);
    if (!numLevels)
        return nullptr;
    if (numLevels > I3D_mesh::kMaxLevels) {
        ReportError(I3DERR_LIMITEXCEEDED, kSevError, g_loadFileName, static_cast<uintptr_t>(numLevels));
        g_loadResult = I3DERR_LIMITEXCEEDED;
        return nullptr;
    }
    I3D_mesh *mesh = g_driver->CreateMesh();
    if (!mesh) {
        dbgPrintf("!! Mesh not created  LoadMeshImage() '%s'", g_loadFileName);
        g_loadResult = I3DERR_OUTOFMEM;
        return nullptr;
    }
    for (int i = 0; i < numLevels; ++i) {
        I3D_mesh_level *level = g_driver->CreateMeshLevel();
        if (!level) {
            dbgPrintf("!! Mesh_level not created  LoadMeshImage() '%s'", g_loadFileName);
            g_loadResult = I3DERR_OUTOFMEM;
            return nullptr;
        }
        dtaRead(g_loadFile, &level->lodDistance, 4);
        dtaRead(g_loadFile, &level->extraFormat, 4);
        int numVerts = 0;
        dtaRead(g_loadFile, &numVerts, 2);
        if (numVerts == 0xffff) {
            // A texture level: the UVs of level 0's vertices, in a plain array where the vertex
            // buffer pointer goes.
            numVerts = mesh->levels[0]->numVertices;
            auto *uv = static_cast<uint8_t *>(::operator new(static_cast<size_t>(numVerts) * 8));
            level->vertices.buffer = reinterpret_cast<C_VertexBuffer *>(uv);
            if (!uv) {
                dbgPrintf("!! m_pUV not created  LoadMeshImage() '%s'", g_loadFileName);
                g_loadResult = I3DERR_OUTOFMEM;
                return nullptr;
            }
            dtaRead(g_loadFile, uv, numVerts * 8);
        } else if (numVerts) {
            void *dst = level->AllocVertices(numVerts);
            dtaRead(g_loadFile, dst, numVerts * 0x20);
            level->ComputeBounds();
        }
        if (level->extraFormat) {
            level->SetExtraFormat(level->extraFormat);
            if (!level->extraData) {
                dbgPrintf("!! ExtraData not created  LoadMeshImage() '%s'", g_loadFileName);
                g_loadResult = I3DERR_OUTOFMEM;
                return nullptr;
            }
            dtaRead(g_loadFile, level->extraData, level->ExtraStride() * numVerts);
        }
        int numGroups = 0;
        dtaRead(g_loadFile, &numGroups, 1);
        while (numGroups) {
            --numGroups;
            I3D_face_group *group = g_driver->CreateFaceGroup();
            if (!group) {
                dbgPrintf("!! Face_group not created  LoadMeshImage() '%s'", g_loadFileName);
                g_loadResult = I3DERR_OUTOFMEM;
                return nullptr;
            }
            int numFaces = 0;
            dtaRead(g_loadFile, &numFaces, 2);
            if (numFaces) {
                I3D_triface *faces = group->AllocFaces(numFaces);
                if (!faces) {
                    dbgPrintf("!! Faces not created  LoadMeshImage() '%s'", g_loadFileName);
                    g_loadResult = I3DERR_OUTOFMEM;
                    return nullptr;
                }
                dtaRead(g_loadFile, faces, numFaces * 6);
            }
            int materialId = 0;
            dtaRead(g_loadFile, &materialId, 2);
            if (materialId) {
                if (materialId > g_loadNumMaterials) {
                    // (The rest of the level's groups are not read.)
                    dbgPrintf("!! Invalid material ID:%d  materialu:%d  LoadImage() '%s'", materialId, g_loadNumMaterials,
                              g_loadFileName);
                    g_loadResult = I3DERR_FILECORRUPTED;
                    break;
                }
                group->SetMaterial(g_loadMaterials[materialId - 1]);
            }
            level->AddFaceGroup(group);
        }
        mesh->SetLevel(level, i);
        level->Release();
    }
    return mesh;
}

}  // namespace ls3d::i3d
