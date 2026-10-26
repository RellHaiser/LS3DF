// I3D_object (0x10035fc0 - 0x10038410) and the mesh visuals' level selection (0x1005eb90).
#include "i3d/object.h"

#include <cmath>
#include <cstring>

#include "core/debug.h"
#include "core/fpu.h"
#include "i3d/billboard.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/i3d_hooks.h"
#include "i3d/material.h"
#include "i3d/loader.h"
#include "i3d/mesh.h"
#include "i3d/render_helpers.h"
#include "i3d/render_state.h"
#include "i3d/scene.h"
#include "i3d/sector.h"
#include "i3d/singlemesh.h"
#include "i3d/vb_manager.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;
using ig::g_rs;

namespace ls3d::i3d {

I3D_material *g_lightmapMaterial;
float g_lodFadeScale;

void ObjectSetLightLevel(I3D_frame *visual, float level) { static_cast<I3D_object *>(visual)->SetLightLevel(level); }
void ObjectSetAlpha(I3D_frame *visual, float value) { static_cast<I3D_object *>(visual)->SetAlpha(value); }

}  // namespace ls3d::i3d

namespace {

// The identity matrix the world-space meshes are drawn with (0x1009cf40).
const S_matrix kIdentity = { { { 1.0f, 0.0f, 0.0f, 0.0f },
                               { 0.0f, 1.0f, 0.0f, 0.0f },
                               { 0.0f, 0.0f, 1.0f, 0.0f },
                               { 0.0f, 0.0f, 0.0f, 1.0f } } };

// The matrix a mesh visual's vertices are drawn with.
const S_matrix &DrawMatrix(I3D_visual *v)
{
    return (v->visualFlags & VISF_WORLD_SPACE) ? kIdentity : v->GetMatrix();
}

// A level's vertices transformed (position, normal: the 0x20-byte vertex) by `m`.
void TransformLevel(I3D_mesh_level *level, const S_matrix &m)
{
    uint8_t *v = static_cast<uint8_t *>(level->LockVertices(0));
    for (int n = level->numVertices; n != 0; --n, v += 0x20) {
        S_vector &pos = *reinterpret_cast<S_vector *>(v);
        S_vector &normal = *reinterpret_cast<S_vector *>(v + 0xC);
        pos = pos * m;
        S_vector nn = normal.RotateByMatrix(m);
        NormalizeWithSquare(nn, SquareZYX(nn));
        normal = nn;
    }
    level->FillRenderVB(true, nullptr, nullptr);
    level->ComputeBounds();
}

}  // namespace

// 0x10035fc0
void I3D_object::Init()
{
    I3D_visual::Init();
    visualFlags |= VISF_OBJECT_PARAMS;
    lightLevel = 1.0f;
    alpha = 1.0f;
    lodFade = 1.0f;
    mesh = nullptr;
    textureLevel = 0;
    std::memset(lights.indices, 0, sizeof lights.indices);
    lights.count = 0;
    lights.stamp = -1;
}

// 0x10036020 (the mesh pointer is kept: Init clears it)
void I3D_object::Uninit()
{
    if (mesh)
        mesh->Release();
    I3D_frame::Uninit();
}

// 0x10038410
void I3D_object::Load()
{
    I3D_frame::Load();
    I3D_mesh *loaded = LoadMesh();
    SetMesh(loaded, 0);
    if (loaded)
        loaded->Release();
}

// 0x10036040
I3D_RESULT I3D_object::SetMesh(I3D_mesh *newMesh, uint32_t)
{
    auto takeBound = [this](const I3D_mesh *m) {
        std::memcpy(&localBound.bbox, &m->bboxMin, 0x20);
        localBound.center = m->center;
        localBound.radius = m->radius;
    };
    if (newMesh == mesh) {
        if (newMesh && !(visualFlags & VISF_WORLD_SPACE))
            takeBound(newMesh);
        return I3D_OK;
    }
    if (mesh)
        mesh->Release();
    mesh = newMesh;
    if (!newMesh) {
        localBound.bbox.min = S_vector(1e16f, 1e16f, 1e16f);
        localBound.bbox.max = S_vector(-1e16f, -1e16f, -1e16f);
        localBound.center = S_vector(0.0f, 0.0f, 0.0f);
        localBound.radius = 0.0f;
        return I3D_OK;
    }
    ++newMesh->refCount;
    if (!(visualFlags & VISF_WORLD_SPACE))
        takeBound(newMesh);
    textureLevel = 0;
    return I3D_OK;
}

// 0x10036160
I3D_RESULT I3D_object::DuplicateMesh()
{
    if (!mesh || mesh->refCount <= 1)
        return I3D_OK;
    I3D_mesh *copy = g_driver->CreateMesh();
    if (!copy) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    const I3D_RESULT r = copy->Duplicate(mesh);
    if (r < 0) {
        copy->Release();
        return r;
    }
    mesh->Release();
    mesh = copy;
    return I3D_OK;
}

// 0x100368d0 / 0x100368f0
void I3D_object::SetLightLevel(float level) { lightLevel = level; }
void I3D_object::SetAlpha(float value) { alpha = value; }

// 0x10036960
void I3D_object::RefreshLevel(int level)
{
    if (mesh && mesh->levels[level])
        mesh->levels[level]->FillRenderVB(true, nullptr, nullptr);
}

// 0x10036910
void I3D_object::RefreshLevels()
{
    if (!mesh)
        return;
    for (uint32_t i = mesh->numLevels; i != 0; --i) {
        if (mesh->levels[i - 1])
            mesh->levels[i - 1]->FillRenderVB(true, nullptr, nullptr);
    }
}

// 0x100361e0
I3D_RESULT I3D_object::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    I3D_object *src = static_cast<I3D_object *>(source);
    switch (src->GetVisualType()) {
    case FourCC("LMAP"):
    case FourCC("BBRD"):
    case FourCC("MRPH"):
    case FourCC("SNGM"):
    case FourCC("SMMR"):
    case FourCC("OBJ_"):
        SetMesh(src->mesh, 0);
        lightLevel = src->lightLevel;
        alpha = src->alpha;
        break;
    case FourCC("LENS"):
        SetMesh(nullptr, 0);
        break;
    default:
        return I3DERR_GENERIC;
    }
    visualFlags = src->visualFlags & 0x2ff52;
    localBound = src->localBound;
    if (!(src->visualFlags & VISF_WORLD_SPACE)) {
        visualFlags &= ~VISF_WBOUND_VALID;
    } else {
        if (!(src->visualFlags & VISF_WBOUND_VALID))
            src->UpdateWBoundProc();
        worldBound = src->worldBound;
    }
    visualFlags |= VISF_OBJECT_PARAMS;
    return I3D_OK;
}

// 0x10036350
void I3D_object::SetWorldSpace(bool on)
{
    if (visualFlags & VISF_WORLD_SPACE) {
        if (on)
            return;
        visualFlags &= ~VISF_WORLD_SPACE;
        if (!mesh)
            return;
        DuplicateMesh();
        const S_matrix *m;
        if (visualFlags & VISF_WORLD_SPACE) {
            m = &kIdentity;
        } else {
            if (!(visualFlags & VISF_INVMATRIX_VALID)) {
                visualFlags |= VISF_INVMATRIX_VALID;
                invWorldMatrix.Inverse(GetMatrix());
            }
            m = &invWorldMatrix;
        }
        for (uint32_t i = 0; i < mesh->numLevels; ++i) {
            if (I3D_mesh_level *level = mesh->GetLevel(static_cast<int>(i)))
                TransformLevel(level, *m);
        }
        mesh->ComputeBounds();
        RefreshLevels();
        return;
    }
    if (!on)
        return;
    if (!(visualFlags & VISF_WBOUND_VALID))
        UpdateWBoundProc();
    if (!mesh) {
        visualFlags = (visualFlags & ~VISF_WBOUND_VALID) | VISF_WORLD_SPACE;
        return;
    }
    DuplicateMesh();
    const S_matrix &m = DrawMatrix(this);
    for (uint32_t i = 0; i < mesh->numLevels; ++i) {
        if (I3D_mesh_level *level = mesh->GetLevel(static_cast<int>(i)))
            TransformLevel(level, m);
    }
    mesh->ComputeBounds();
    visualFlags = (visualFlags & ~VISF_WBOUND_VALID) | VISF_WORLD_SPACE;
    RefreshLevels();
}

// 0x1005eb90
int I3D_object::SelectLevel(const float &distance2)
{
    const int n = static_cast<int>(mesh->numLevels);
    int level;
    if (n == 1) {
        const I3D_mesh_level *l0 = mesh->levels[0];
        if (!l0)
            return 0;
        if (l0->lodDistance < 0.0f || l0->lodDistance == 0.0f)
            return 0;
        const float s = g_lodScale * worldScale;
        level = distance2 / (s * s) > l0->lodDistance ? 1 : 0;
    } else {
        const float s = g_lodScale * worldScale;
        const float q = distance2 / (s * s);
        level = 0;
        for (; level < n - 1; ++level) {
            const I3D_mesh_level *l = mesh->levels[level];
            if (l && (q < l->lodDistance || q == l->lodDistance))
                goto chosen;
        }
        {
            const I3D_mesh_level *l = mesh->levels[level];
            if (l && l->lodDistance > 0.0f && q > l->lodDistance)
                level = n;
        }
    }
chosen:
    if (!(visualFlags & VISF_TEXTURE_LOD)) {
        textureLevel = level;
        return level;
    }
    const int last = n - 1;
    if (level > last)
        return level;
    // Texture LODs: the chosen level's textures, or (while they stream in) the first other level
    // whose texture is loaded, else the last.
    auto firstMaterial = [this](int i) {
        I3D_material *m = mesh->levels[i]->faceGroups->material;
        if (m->matFlags & MATF_ANIMATED)
            m->UpdateTexture();
        return m;
    };
    int use = level;
    I3D_material *m = firstMaterial(level);
    if (m->texture && !tex::StreamRequest(m->texture)) {
        use = last;
        for (int j = 0; j < last; ++j) {
            if (j == level)
                continue;
            I3D_material *mj = firstMaterial(j);
            if (!mj->texture)
                continue;
            if (mj->matFlags & MATF_ANIMATED)
                mj->UpdateTexture();
            if (mj->texture->flags & ITC_RESIDENT) {
                use = j;
                break;
            }
        }
    }
    if (textureLevel != use) {
        textureLevel = use;
        visualFlags |= VISF_UV_STALE;
    }
    return 0;
}

// 0x10036990
void I3D_object::Prepare()
{
    if (!mesh)
        return;
    if (PrepareMirrored())
        return;
    const float distance2 = LodDistance2();
    const int levelIndex = SelectLevel(distance2);
    I3D_mesh_level *level = mesh->levels[levelIndex];
    if (!level)
        return;
    const int numVertices = level->numVertices;
    if (numVertices == 0)
        return;
    if (!level->renderVertices.vb) {
        level->CreateRenderVB(1, &GetMatrix(), nullptr);
        if (visualFlags & VISF_TEXTURE_LOD)
            visualFlags |= VISF_UV_STALE;
    } else if (level->flags & MLF_RENDER_STALE) {
        level->FillRenderVB(false, &GetMatrix(), nullptr);
    }
    SetLodFade(levelIndex, level, distance2);
    if (visualFlags & VISF_UV_STALE) {
        // The texture level's UVs into the render vertices.
        visualFlags &= ~VISF_UV_STALE;
        C_VertexBuffer *rb = level->renderVertices.buffer;
        uint8_t *dst = rb->Lock(0) + rb->stride * level->renderVertices.offset;
        const uint32_t dstStride = level->renderVertices.buffer->stride;
        const uint8_t *src;
        uint32_t srcStride;
        if (textureLevel > 0) {
            // (A texture level keeps a plain array of its UVs in its vertex buffer pointer.)
            src = reinterpret_cast<const uint8_t *>(mesh->levels[textureLevel]->vertices.buffer);
            srcStride = 8;
        } else {
            C_VertexBuffer *vb = level->vertices.buffer;
            srcStride = 0x20;
            src = vb->Lock(D3DLOCK_READONLY) + vb->stride * level->vertices.offset + 0x18;
        }
        for (int i = numVertices; i != 0; --i, dst += dstStride, src += srcStride) {
            std::memcpy(dst + 0x18, src, 8);
        }
    }
    SetLightSector(g_rc.sector);
    SelectLights(this, lights, 1);
    QueueFaceGroups(level, (visualFlags & VISF_TEXTURE_LOD) ? mesh->levels[textureLevel]->faceGroups : nullptr);
}

bool I3D_object::PrepareMirrored()
{
    if (!(visualFlags & VISF_MIRRORED))
        return false;
    // A mirror's copy: the reflected visual's items, in its sector, with mirrored lights.
    // (g_rc.sector is left at that sector.)
    I3D_object *original = static_cast<I3D_object *>(data);
    I3D_frame *f = original->parent;
    while (f && f->type != FRAME_SECTOR)
        f = f->parent;
    g_rc.sector = f ? static_cast<I3D_sector *>(f) : g_renderScene->primarySector;
    const int first = g_rc.items.Size();
    original->Prepare();
    MirrorLights(original->lights, lights);
    for (int i = first; i < g_rc.items.Size(); ++i)
        g_rc.items[i].visual = this;
    return true;
}

float I3D_object::LodDistance2()
{
    S_vector p;
    if (visualFlags & VISF_LOD_AT_POSITION) {
        p = GetMatrix()(3);
    } else {
        if (!(visualFlags & VISF_WBOUND_VALID))
            UpdateWBoundProc();
        p = worldBound.center;
    }
    const S_vector d(p.x - g_rc.lodPosition.x, p.y - g_rc.lodPosition.y, p.z - g_rc.lodPosition.z);
    return SquareZYX(d);
}

void I3D_object::SetLodFade(int levelIndex, const I3D_mesh_level *level, float distance2)
{
    // The last level fades out over g_lodFadeScale of its range.
    const uint32_t n = mesh->numLevels;
    if (levelIndex == static_cast<int>(n - 1) && level->lodDistance > 0.0f) {
        float range = level->lodDistance;
        const float s = g_lodScale * worldScale;
        float q = distance2 / (s * s);
        if (n > 1) {
            const float prev = mesh->levels[levelIndex - 1]->lodDistance;
            q = q - prev;
            range = range - prev;
        }
        float f = (1.0f - q / range) * g_lodFadeScale;
        if (f > 0.99f)
            f = 1.0f;
        lodFade = f;
    } else {
        lodFade = 1.0f;
    }
}

void I3D_object::QueueFaceGroups(I3D_mesh_level *level, I3D_face_group *uvGroup)
{
    for (I3D_face_group *fg = level->faceGroups; fg; fg = fg->next) {
        S_renderItem &item = g_rc.items.PushBackUninit();
        item.visual = this;
        item.sector = g_rc.sector;
        item.data0C = fg;
        I3D_material *mat = fg->material;
        if (!mat || !(mat->matFlags & MATF_TEXTURE)) {
            item.texture = nullptr;
        } else if (!(visualFlags & VISF_TEXTURE_LOD) || !uvGroup) {
            // (The original reads the texture level's group without a null check.)
            if (mat->matFlags & MATF_ANIMATED)
                mat->UpdateTexture();
            item.texture = mat->texture;
        } else {
            I3D_material *m2 = uvGroup->material;
            if (m2->matFlags & MATF_ANIMATED)
                m2->UpdateTexture();
            item.texture = m2->texture;
        }
        if (!mat || !(mat->matFlags & MATF_ENV_TEXTURE)) {
            item.texture2 = nullptr;
        } else if (!(visualFlags & VISF_TEXTURE_LOD) || !uvGroup) {
            if (mat->matFlags & MATF_ANIMATED)
                mat->UpdateEnvTexture();
            item.texture2 = mat->envTexture;
        } else {
            // (The texture level's groups advance only with environment textures: the original's.)
            I3D_material *m2 = uvGroup->material;
            if (m2->matFlags & MATF_ANIMATED)
                m2->UpdateEnvTexture();
            item.texture2 = m2->envTexture;
            uvGroup = uvGroup->next;
        }
        if (!mat || (!item.texture && (mat->opacity * alpha) * lodFade >= 1.0f)) {
            item.sortKey = 0;
            ++g_rc.numOpaque;
        } else if (!(mat->matFlags & (MATF_ALPHA | MATF_ADDITIVE)) && !((mat->opacity * alpha) * lodFade < 1.0f)) {
            const uint32_t id = item.texture ? item.texture->id : 0;
            item.sortKey = (mat->matFlags & MATF_COLORKEY) ? ((id & RIKEY_TEXTURE_MASK) | RIKEY_COLORKEY)
                                                           : (id & RIKEY_TEXTURE_MASK);
            ++g_rc.numOpaque;
        } else {
            item.sortKey = RIKEY_BLENDED | (static_cast<uint32_t>(Fistp(depth)) & RIKEY_DEPTH_MASK);
            ++g_rc.numBlended;
        }
    }
}

// 0x10037010
void I3D_object::Draw(const S_renderItem *item) { DrawMeshItem(this, item); }

namespace ls3d::i3d {

void DrawMeshItem(I3D_object *v, const S_renderItem *item)
{
    I3D_face_group *fg = static_cast<I3D_face_group *>(item->data0C);
    I3D_material *mat = fg->material;
    const S_VBAlloc *vb = nullptr;
    int numVertices = 0;
    const S_matrix *matrix = nullptr;
    // The level drawn: a mirror's copy draws its original's level (vertices of the original).
    auto levelOf = [](I3D_object *o, int i) { return o->mesh->levels[i]; };
    const bool mirrored = (v->visualFlags & VISF_MIRRORED) != 0;
    I3D_object *original = mirrored ? static_cast<I3D_object *>(v->data) : v;
    switch (v->GetVisualType()) {
    case FourCC("SNGM"):
    case FourCC("SMMR"): {
        // The skinned visuals' level buffers.
        matrix = &DrawMatrix(v);
        const int lvl = original->textureLevel;
        vb = &static_cast<I3D_singlemesh *>(original)->levels[lvl]->vb;
        numVertices = levelOf(v, lvl)->numVertices;
        break;
    }
    case FourCC("MRPH"): {
        matrix = &DrawMatrix(v);
        const int lvl = original->textureLevel;
        vb = &levelOf(original, lvl)->renderVertices;
        numVertices = levelOf(v, lvl)->numVertices;
        break;
    }
    case FourCC("BBRD"): {
        matrix = &static_cast<I3D_billboard *>(v)->matrix;
        const int lvl = original->textureLevel;
        vb = &levelOf(original, lvl)->renderVertices;
        numVertices = levelOf(v, lvl)->numVertices;
        break;
    }
    case FourCC("LMAP"): {
        // Lightmaps draw with the shared material, given the face group material's settings.
        float opacity = 1.0f, matAlpha = 1.0f;
        uint32_t bits12 = 0, bits8 = 0;
        if (mat) {
            opacity = mat->opacity;
            bits12 = mat->matFlags >> 12 & 7;
            bits8 = mat->matFlags >> 8 & 7;
            matAlpha = mat->alpha;
        }
        mat = g_lightmapMaterial;
        mat->SetOpacity(opacity);
        mat->SetBits12(bits12);
        mat->SetBits8(bits8);
        mat->SetAlpha(matAlpha);
        matrix = &DrawMatrix(v);
        const int lvl = original->textureLevel;
        vb = &levelOf(original, lvl)->renderVertices;
        numVertices = levelOf(v, lvl)->numVertices;
        break;
    }
    case FourCC("OBJ_"): {
        matrix = &DrawMatrix(v);
        const int lvl = (v->visualFlags & VISF_TEXTURE_LOD) ? 0 : original->textureLevel;
        vb = &levelOf(original, lvl)->renderVertices;
        numVertices = levelOf(v, lvl)->numVertices;
        break;
    }
    default:
        break;
    }

    SetAlphaOp(0, D3DTOP_MODULATE);
    int colorOp0 = D3DTOP_MODULATE2X;
    uint32_t restore = 0;                   // the stage states to put back after drawing
    enum : uint32_t {
        RESTORE_TCI0 = 0x01, RESTORE_CLAMP_U = 0x02, RESTORE_CLAMP_V = 0x04, RESTORE_TEXGEN1 = 0x08,
        RESTORE_TEXGEN0 = 0x10, RESTORE_STAGE2 = 0x20, RESTORE_BUMP = 0x40,
    };
    float fade = 0.0f;
    if (!mat) {
        g_pGfx->SetMaterial(&ig::g_defaultMaterial);
        SetColorOp(0, D3DTOP_MODULATE2X);
        g_pGfx->SetTexture(1, nullptr);
        SetColorOp(1, D3DTOP_DISABLE);
        if (g_rs.alpha != 0) {
            g_rs.alpha = 0;
            g_pGfx->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_pGfx->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        }
        g_pGfx->SetTexture(0, ig::g_defaultTexture->deviceTexture);
        ig::SetSpecular(0);
    } else {
        fade = (v->lodFade * v->alpha) * mat->opacity;
        if (mat->matFlags & MATF_COLORS) {
            SetMaterial(*reinterpret_cast<D3DMATERIAL8 *>(&mat->diffuse), fade, v->lightLevel);
            ig::SetSpecular(mat->power > 1.0f ? 1 : 0);
        } else {
            SetMaterial(ig::g_defaultMaterial, fade, v->lightLevel);
            ig::SetSpecular(0);
        }
        const uint32_t bits12 = mat->matFlags >> 12 & 7;
        auto texGen = [](int stage, uint32_t source, int &cache, D3DTRANSFORMSTATETYPE transform) {
            if (static_cast<uint32_t>(cache) != source) {
                cache = static_cast<int>(source);
                if (source == 0)
                    g_pGfx->SetTransform(transform, &ig::g_texProjectXY);
                else if (source == 1)
                    g_pGfx->SetTransform(transform, &ig::g_texProjectXZ);
            }
            g_pGfx->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
            g_pGfx->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
            g_pGfx->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        };
        auto factor = [mat]() {
            return (static_cast<uint32_t>(Ftol(static_cast<double>(mat->alpha * 255.0f))) & 0xff) << 24;
        };
        if (!item->texture2) {
            g_pGfx->SetTexture(1, nullptr);
            SetColorOp(1, D3DTOP_DISABLE);
        } else if (!item->texture) {
            // The environment texture alone on stage 0.
            switch (mat->matFlags >> 8 & 7) {
            case 1:
                SetColorOp(0, D3DTOP_BLENDFACTORALPHA);
                g_pGfx->SetRenderState(D3DRS_TEXTUREFACTOR, factor());
                break;
            case 2:
                SetColorOp(0, D3DTOP_BLENDCURRENTALPHA);
                break;
            case 3:
            case 7:
                SetColorOp(0, D3DTOP_MODULATE);
                break;
            case 4:
            case 6:
                SetColorOp(0, D3DTOP_MODULATE2X);
                break;
            case 5:
                SetColorOp(0, D3DTOP_ADD);
                break;
            default:
                break;
            }
            if (bits12 < 2) {
                texGen(0, bits12, g_rs.unknown19c0, D3DTS_TEXTURE0);
                restore = RESTORE_TEXGEN0;
            } else {
                g_pGfx->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1);
                restore = RESTORE_TCI0;
            }
            g_pGfx->SetTexture(0, item->texture2->deviceTexture);
            g_pGfx->SetTexture(1, nullptr);
            SetColorOp(1, D3DTOP_DISABLE);
        } else {
            // The texture on stage 0 and the environment texture on stage 1 (bump: 1 and 2).
            uint32_t source = bits12;
            switch (mat->matFlags >> 8 & 7) {
            case 1:
                g_pGfx->SetRenderState(D3DRS_TEXTUREFACTOR, factor());
                colorOp0 = D3DTOP_SELECTARG1;
                SetColorOp(1, D3DTOP_BLENDFACTORALPHA);
                SetAlphaOp(1, D3DTOP_DISABLE);
                SetColorOp(2, D3DTOP_MODULATE2X);
                g_pGfx->SetTexture(1, item->texture2->deviceTexture);
                restore = RESTORE_STAGE2;
                break;
            case 2:
            case 3:
            case 4:
            case 5: {
                static const int kOps[] = { 0, 0, D3DTOP_BLENDCURRENTALPHA, D3DTOP_MODULATE, D3DTOP_MODULATE2X, D3DTOP_ADD };
                SetColorOp(1, kOps[mat->matFlags >> 8 & 7]);
                SetAlphaOp(1, D3DTOP_MODULATE);
                g_pGfx->SetTexture(1, item->texture2->deviceTexture);
                break;
            }
            case 6:
                if (!(ig::g_quirks & ig::IGQ_LIMITED)) {
                    colorOp0 = D3DTOP_SELECTARG1;
                    SetAlphaOp(1, D3DTOP_DISABLE);
                    SetColorOp(1, D3DTOP_BLENDTEXTUREALPHA);
                    SetColorOp(2, D3DTOP_MODULATE2X);
                    restore = RESTORE_STAGE2;
                } else {
                    SetColorOp(1, D3DTOP_DISABLE);
                }
                g_pGfx->SetTexture(1, item->texture2->deviceTexture);
                break;
            case 7:
                if (!(ig::g_caps & 0x100)) {
                    SetColorOp(1, D3DTOP_MODULATE);
                    SetAlphaOp(1, D3DTOP_MODULATE);
                    g_pGfx->SetTexture(1, item->texture2->deviceTexture);
                    source = 1;
                    break;
                }
                {
                    // An animated bump map on stage 1 (its frame by the driver time, its matrix
                    // turned with the camera's heading), the environment texture on stage 2.
                    if (!ig::g_bumpAnimTextures[0] && !LoadBumpAnimTextures(g_pIGraph))
                        ig::g_caps &= ~0x100u;
                    S_matrix bump;
                    bump.Identity();
                    const float a = mat->alpha;
                    bump.m[2][2] = a;
                    bump.m[1][1] = a;
                    bump.m[0][0] = a;
                    const S_vector &fwd = g_renderCamera->GetMatrix()(2);
                    float x = fwd.x, z = fwd.z;
                    const float sq = (z * z + x * x) + fwd.y * fwd.y;
                    if (!(std::fabs(sq - 1.0f) < kEpsilon)) {
                        if (!(sq < kEpsilon)) {
                            const float inv = 1.0f / std::sqrt(sq);
                            x = x * inv;
                            z = z * inv;
                        } else if (x != 0.0f) {
                            x = x < 0.0f ? -1.0f : 1.0f;
                        } else if (z != 0.0f) {
                            z = z < 0.0f ? -1.0f : 1.0f;
                        }
                    }
                    const float angle = static_cast<float>(
                        std::atan2(static_cast<double>(z), static_cast<double>(x)) - static_cast<double>(1.57079637f));
                    bump *= RotateZMatrix(angle);
                    const int frame = (ls3d::i3d::g_driverTime % 0x1fe) / 30;
                    colorOp0 = D3DTOP_MODULATE2X;
                    SetColorOp(1, D3DTOP_BUMPENVMAP);
                    SetAlphaOp(1, D3DTOP_DISABLE);
                    if (ig::g_bumpAnimTextures[frame])
                        g_pGfx->SetTexture(1, ig::g_bumpAnimTextures[frame]->deviceTexture);
                    auto bits = [](float f) {
                        DWORD d;
                        std::memcpy(&d, &f, 4);
                        return d;
                    };
                    g_pGfx->SetTextureStageState(1, D3DTSS_BUMPENVMAT00, bits(bump.m[0][0]));
                    g_pGfx->SetTextureStageState(1, D3DTSS_BUMPENVMAT01, bits(bump.m[0][1]));
                    g_pGfx->SetTextureStageState(1, D3DTSS_BUMPENVMAT10, bits(bump.m[1][0]));
                    g_pGfx->SetTextureStageState(1, D3DTSS_BUMPENVMAT11, bits(bump.m[1][1]));
                    g_pGfx->SetTexture(2, item->texture2->deviceTexture);
                    if (g_rs.unknown1780 != 1) {
                        g_rs.unknown1780 = 1;
                        g_pGfx->SetTransform(D3DTS_TEXTURE2, &ig::g_texProjectXZ);
                    }
                    g_pGfx->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    SetColorOp(2, D3DTOP_MODULATE);
                    g_pGfx->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
                    g_pGfx->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
                    g_pGfx->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
                    restore = RESTORE_BUMP;
                }
                break;
            default:
                break;
            }
            if (source < 2) {
                texGen(1, source, g_rs.unknown1888, D3DTS_TEXTURE1);
                restore |= RESTORE_TEXGEN1;
            }
        }
        if (mat->matFlags & MATF_CLAMP_U) {
            g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            restore |= RESTORE_CLAMP_U;
        }
        if (mat->matFlags & MATF_CLAMP_V) {
            g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
            restore |= RESTORE_CLAMP_V;
        }
        // Blending: additive; blended (alpha or faded); color keyed (alpha test); opaque.
        auto alphaState = [](int state) {
            if (g_rs.alpha == state)
                return;
            g_rs.alpha = state;
            switch (state) {
            case 0:
                g_pGfx->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                g_pGfx->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                break;
            case 1:
                g_pGfx->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_pGfx->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                g_pGfx->SetRenderState(D3DRS_ALPHAREF, 1);
                break;
            case 3:
                g_pGfx->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                g_pGfx->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                g_pGfx->SetRenderState(D3DRS_ALPHAREF, g_rs.alphaRef);
                break;
            }
        };
        auto blendState = [](int state) {
            if (g_rs.blend == state)
                return;
            g_rs.blend = state;
            g_pGfx->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_pGfx->SetRenderState(D3DRS_DESTBLEND, state == 1 ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
        };
        const uint32_t f = mat->matFlags;
        if (f & (MATF_ADDITIVE | MATF_ADDITIVE_LOW)) {
            alphaState(1);
            blendState(1);
        } else if ((f & MATF_ALPHA) || fade < 1.0f) {
            alphaState(1);
            blendState(0);
        } else if (f & MATF_COLORKEY) {
            alphaState(3);
            blendState(0);
        } else {
            alphaState(0);
        }
        const int cull = static_cast<int>((~f & MATF_TWO_SIDED | 0x8000000) >> 27);
        if (g_rs.cull != cull) {
            g_rs.cull = cull;
            g_pGfx->SetRenderState(D3DRS_CULLMODE, cull == 3 ? g_rs.cullDefault : cull);
        }
        if (item->texture) {
            SetColorOp(0, colorOp0);
            g_pGfx->SetTexture(0, item->texture->deviceTexture);
        } else if (!item->texture2) {
            SetColorOp(0, D3DTOP_MODULATE2X);
            g_pGfx->SetTexture(0, ig::g_defaultTexture->deviceTexture);
        }
    }

    EnableLights(item->sector->ambientColor, v->lights.count, v->lights.indices);
    g_pGfx->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX *>(matrix));
    SetVertexFormat(static_cast<int>(vb->buffer->format));
    g_pGfx->SetStreamSource(0, vb->vb, vb->buffer->stride);
    g_pGfx->SetIndices(fg->faces.ib, vb->offset);
    g_pGfx->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, numVertices, fg->faces.offset, fg->numFaces);

    if (restore & RESTORE_CLAMP_U)
        g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    if (restore & RESTORE_CLAMP_V)
        g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    if (restore & RESTORE_BUMP) {
        g_pGfx->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pGfx->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pGfx->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, 2);
        g_pGfx->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
        SetColorOp(2, D3DTOP_DISABLE);
        g_pGfx->SetTexture(2, nullptr);
    }
    if (restore & RESTORE_STAGE2)
        SetColorOp(2, D3DTOP_DISABLE);
    if (restore & RESTORE_TEXGEN1) {
        g_pGfx->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pGfx->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
        g_pGfx->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
    } else if (restore & RESTORE_TCI0) {
        g_pGfx->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    } else if (restore & RESTORE_TEXGEN0) {
        g_pGfx->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pGfx->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pGfx->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
    }
}

}  // namespace ls3d::i3d
