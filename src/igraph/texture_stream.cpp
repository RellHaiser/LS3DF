// Texture streaming (IG_TEXTURE_THREAD, ITC_STREAMED; 0x10078b40 - 0x100794a0).
//
// A streamed texture is a .dds file whose name ends in a size class digit ("name_0.dds": 256,
// 128 or 64 pixels square). It owns no Direct3D texture until the renderer asks for it
// (StreamRequest); the thread then reads the file's level 0 into the class's system-memory
// staging texture and uploads it into a slot of the class's pool of default-pool textures,
// taking the least recently used slot or adding one.
#include "igraph/texture_stream.h"

#include <cstring>
#include <new>

#include "core/c_vector.h"
#include "core/debug.h"
#include "external/rw_data.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"
#include "platform/platform.h"

using namespace ls3d;
using namespace ls3d::ig;

namespace ls3d::tex {

// A streamed texture (0x10 bytes).
struct S_streamRecord {
    ITexture *texture;
    char *fileName;
    int sizeClass;                              // the digit before the extension
    uint32_t lastFrame;                         // Flip count of the last request
};

namespace {

// A pool texture (0x2c bytes).
struct S_slot {
    ITexture *owner;
    gfx::Texture *texture;
    gfx::Surface *surface;                      // level 0
    D3DSURFACE_DESC desc;
};
static_assert(sizeof(S_slot) == 0x2c, "S_slot layout");

constexpr int kClasses = 3;
const UINT kClassSize[kClasses] = {0x100, 0x80, 0x40};              // 0x100a8d2c
const int kClassBudget[kClasses] = {0x80000, 0x80000, 0x40000};     // 0x100a8d20 bytes created at start

C_vector<S_slot> g_pools[kClasses];             // 0x100d2a38
S_slot g_staging[kClasses];                     // 0x100d2ad0 system-memory textures
int g_poolBytes[kClasses];                      // 0x100d2be4
C_vector<S_streamRecord *> g_records(0x400);    // 0x100d2b70

// Requests: a ring of 256 (0x100d2628, read 0x100d2bd8, write 0x100d2bdc, count 0x100d2be0).
ITexture *g_queue[0x100];
int g_queueRead = 0;
int g_queueWrite = 0;
int g_queueCount = 0;

void AddSlot(int sizeClass, D3DFORMAT format)
{
    S_slot slot = {};
    gfx::g_pGfx->CreateTexture(kClassSize[sizeClass], kClassSize[sizeClass], 1, 0, format, D3DPOOL_DEFAULT,
                               &slot.texture);
    gfx::g_pGfx->GetSurfaceLevel(slot.texture, 0, &slot.surface);
    gfx::g_pGfx->GetDesc(slot.surface, &slot.desc);
    slot.owner = nullptr;
    g_pools[sizeClass].PushBack(slot);
    g_poolBytes[sizeClass] += slot.desc.Size;
}

void Lock() { platform::Lock(g_textureLock); }
void Unlock() { platform::Unlock(g_textureLock); }

}  // namespace

// 0x10078b40
void StreamFlush()
{
    Lock();
    for (C_vector<S_slot> &pool : g_pools) {
        for (int i = pool.Size(); i--;) {
            if (ITexture *owner = pool[i].owner) {
                owner->flags &= ~ITC_RESIDENT;
                pool[i].owner = nullptr;
            }
        }
    }
    Unlock();
    for (int c = 0; c < kClasses; ++c) {
        if (g_staging[c].surface) {
            gfx::g_pGfx->Release(g_staging[c].surface);
            g_staging[c].surface = nullptr;
        }
        if (g_staging[c].texture) {
            gfx::g_pGfx->Release(g_staging[c].texture);
            g_staging[c].texture = nullptr;
        }
        C_vector<S_slot> &pool = g_pools[c];
        for (int i = pool.Size(); i--;) {
            gfx::g_pGfx->Release(pool[i].surface);
            gfx::g_pGfx->Release(pool[i].texture);
        }
        pool.Free();
        g_poolBytes[c] = 0;
    }
}

// 0x10078c40
void StreamInit(uint32_t flags)
{
    StreamFlush();
    const D3DFORMAT format = ((flags & 0x80000) && g_fmtDxt1) ? g_fmtDxt1 : g_fmt16Bit;
    for (int c = 0; c < kClasses; ++c) {
        g_pools[c].SetGrowBy(0x400);
        S_slot &stage = g_staging[c];
        gfx::g_pGfx->CreateTexture(kClassSize[c], kClassSize[c], 1, 0, format, D3DPOOL_SYSTEMMEM, &stage.texture);
        gfx::g_pGfx->GetSurfaceLevel(stage.texture, 0, &stage.surface);
        gfx::g_pGfx->GetDesc(stage.surface, &stage.desc);
        stage.owner = nullptr;
        while (g_poolBytes[c] < kClassBudget[c])
            AddSlot(c, format);
    }
    g_records.Reserve(0x1000);
}

// 0x10078e40: true if the texture can be drawn now; otherwise it is queued (once).
bool StreamRequest(ITexture *texture)
{
    if (!(texture->flags & ITC_STREAMED))
        return true;
    Lock();
    texture->stream->lastFrame = g_frameCounter;
    if (texture->flags & ITC_RESIDENT) {
        Unlock();
        return true;
    }
    if (!(texture->flags & ITC_QUEUED) && g_queueCount < 0x100) {
        g_queue[g_queueWrite] = texture;
        if (++g_queueWrite > 0xff)
            g_queueWrite = 0;
        ++g_queueCount;
        texture->flags |= ITC_QUEUED;
    }
    Unlock();
    return false;
}

// 0x10078ed0
DWORD WINAPI StreamThread(void *)
{
    while (g_textureThreadRun) {
        if (!g_queueCount) {
            platform::Pause(0);
            continue;
        }
        Lock();
        // Requests older than the last frame are dropped.
        ITexture *texture = nullptr;
        while (g_queueCount > 0) {
            texture = g_queue[g_queueRead];
            if (texture) {
                if (texture->stream->lastFrame + 1 >= static_cast<uint32_t>(g_frameCounter))
                    break;
                texture->flags &= ~ITC_QUEUED;
            }
            if (++g_queueRead >= 0x100)
                g_queueRead = 0;
            --g_queueCount;
        }
        if (!g_queueCount) {
            Unlock();
            continue;
        }
        ++texture->refCount;
        Unlock();

        S_streamRecord *record = texture->stream;
        const int c = record->sizeClass;
        Lock();
        // The least recently used slot of the class, or a free one.
        C_vector<S_slot> &pool = g_pools[c];
        uint32_t oldest = g_frameCounter + 10;
        int best = 0;
        for (int i = 0; i < pool.Size(); ++i) {
            if (!pool[i].owner) {
                oldest = 0;
                best = i;
                break;
            }
            if (pool[i].owner->stream->lastFrame < oldest) {
                oldest = pool[i].owner->stream->lastFrame;
                best = i;
            }
        }
        S_slot *slot;
        if (oldest >= static_cast<uint32_t>(g_frameCounter - 1)) {
            // Every slot was drawn in the last frames: one more (the original takes the DXT1
            // format here whatever StreamInit chose).
            AddSlot(c, g_fmtDxt1);
            slot = &pool[pool.Size() - 1];
        } else {
            slot = &pool[best];
            if (ITexture *owner = slot->owner) {
                owner->deviceTexture = nullptr;
                owner->flags &= ~(ITC_RESIDENT | ITC_QUEUED);
            }
        }
        Unlock();

        S_slot &stage = g_staging[c];
        D3DLOCKED_RECT lr;
        gfx::g_pGfx->LockRect(stage.surface, &lr, nullptr, 0);
        texture->width = static_cast<uint16_t>(slot->desc.Width);
        texture->height = static_cast<uint16_t>(slot->desc.Height);
        const int file = dtaOpen(record->fileName, false);
        dtaSeek(file, 0x80, 1);                 // past the DDS header
        dtaRead(file, lr.pBits, slot->desc.Size);
        dtaClose(file);
        gfx::g_pGfx->UnlockRect(stage.surface);
        gfx::g_pGfx->UpdateTexture(stage.texture, slot->texture);

        Lock();
        slot->owner = texture;
        texture->deviceTexture = slot->texture;
        texture->flags = (texture->flags & ~(ITC_RESIDENT | ITC_QUEUED)) | ITC_RESIDENT;
        texture->Release();
        if (++g_queueRead >= 0x100)
            g_queueRead = 0;
        --g_queueCount;
        Unlock();
        platform::Pause(0);
    }
    return 0;
}

// 0x10079230 (AddTextureToTextureManager in the original's message)
bool StreamAdd(ITexture *texture, const char *fileName, uint32_t)
{
    S_streamRecord *record = new S_streamRecord;
    record->texture = texture;
    record->lastFrame = 0;
    const size_t len = std::strlen(fileName);
    record->fileName = static_cast<char *>(::operator new(len + 1, std::nothrow));
    if (!record->fileName) {
        delete record;
        dbgPrintf("AddTextureToTextureManager() - ERROR! Memory low !!!");
        return false;
    }
    std::memcpy(record->fileName, fileName, len + 1);
    record->sizeClass = fileName[len - 5] - '0';
    texture->deviceTexture = nullptr;
    texture->stream = record;
    g_records.PushBack(record);
    return true;
}

// 0x10079340
void StreamRemove(ITexture *texture)
{
    Lock();
    for (int i = g_records.Size(); i--;) {
        if (g_records[i]->texture != texture)
            continue;
        if (g_queueCount > 0 && (texture->flags & ITC_QUEUED)) {
            for (ITexture *&queued : g_queue) {
                if (queued == texture)
                    queued = nullptr;
            }
        }
        ::operator delete(g_records[i]->fileName);
        delete g_records[i];
        g_records.RemoveAtSwap(i);
        break;
    }
    if (texture->flags & ITC_RESIDENT) {
        for (C_vector<S_slot> &pool : g_pools) {
            for (int i = pool.Size(); i--;) {
                if (pool[i].owner == texture)
                    pool[i].owner = nullptr;
            }
        }
    }
    texture->flags &= ~(ITC_RESIDENT | ITC_QUEUED);
    Unlock();
}

// 0x10079440: every pool slot freed (the scene's textures are gone).
void StreamReleaseAll()
{
    Lock();
    for (C_vector<S_slot> &pool : g_pools) {
        for (int i = pool.Size(); i--;) {
            if (ITexture *owner = pool[i].owner) {
                owner->flags &= ~ITC_RESIDENT;
                pool[i].owner = nullptr;
            }
        }
    }
    Unlock();
}

}  // namespace ls3d::tex
