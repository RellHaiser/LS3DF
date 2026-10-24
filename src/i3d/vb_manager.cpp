// C_VBManager, C_VertexBuffer (VBManager.cpp, 0x10066020 - 0x10068620).
#include "i3d/vb_manager.h"

#include "core/debug.h"
#include "core/i3d_result.h"
#include "dx/dxerr8.h"
#include "i3d/driver.h"
#include "igraph/igraph_state.h"

namespace ls3d {

using ig::g_pDevice;
using i3d::g_driver;

C_VBManager g_vbManager;

// ---------------------------------------------------------------- C_VertexBuffer

C_VertexBuffer::C_VertexBuffer()
    : lockedData(nullptr), lockFlags(0), format(2), pool(D3DPOOL_MANAGED), usage(0), stride(0), vb(nullptr),
      capacity(0), available(0), lockIndex(-1)
{
    usedBlocks.SetGrowBy(0x100);
    usedBlocks.Reserve(0x100);
}

C_VertexBuffer::~C_VertexBuffer()
{
    for (S_bufferBlock *b : usedBlocks) {
        auto *owner = static_cast<S_VBAlloc *>(b->owner);
        owner->buffer = nullptr;
        owner->vb = nullptr;
        owner->offset = 0;
        delete b;
    }
    for (auto it = freeBlocks.begin(); it != freeBlocks.end(); ++it)
        delete *it;
    if (lockedData)
        Unlock();
    if (g_driver)
        g_driver->SlotB4();
    if (vb)
        vb->Release();
    usedBlocks.Free();
}

bool C_VertexBuffer::Create(uint32_t newUsage, uint32_t newStride, uint32_t count, uint32_t newFormat)
{
    format = newFormat;
    const uint32_t minimum = 0x20000 / newStride;
    usage = newUsage;
    stride = newStride;
    const uint32_t n = minimum > count ? minimum : count;
    capacity = n;
    available = n;
    pool = !(usage & D3DUSAGE_DYNAMIC) && (usage & D3DUSAGE_SOFTWAREPROCESSING) ? D3DPOOL_MANAGED : D3DPOOL_DEFAULT;
    HRESULT hr = g_pDevice->CreateVertexBuffer(n * stride, usage, 0, static_cast<D3DPOOL>(pool), &vb);
    if (FAILED(hr)) {
        g_pDevice->ResourceManagerDiscardBytes(0);
        hr = g_pDevice->CreateVertexBuffer(capacity * stride, usage, 0, static_cast<D3DPOOL>(pool), &vb);
        if (FAILED(hr)) {
            pool = D3DPOOL_MANAGED;
            hr = g_pDevice->CreateVertexBuffer(capacity * stride, usage, 0, D3DPOOL_MANAGED, &vb);
            if (FAILED(hr)) {
                dbgPrintf("!! Error detected: %s, File: %s, Line: %i !!, Bytes allocated: %i", DXGetErrorString8A(hr),
                          "C:\\MyProjects\\Engine8_5\\I3D\\VBManager.cpp", 163, capacity * stride);
                vb = nullptr;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return false;
            }
        }
    }
    g_driver->SlotB4();
    freeBlocks.Insert(new S_bufferBlock{nullptr, 0, capacity});
    vb->Lock(0, 0, &lockedData, (usage & D3DUSAGE_DYNAMIC) << 4);      // D3DLOCK_DISCARD
    g_vbManager.AddLocked(this);
    return true;
}

int C_VertexBuffer::Allocate(uint32_t count, S_VBAlloc *owner)
{
    for (auto it = freeBlocks.begin(); it != freeBlocks.end(); ++it) {
        S_bufferBlock *b = *it;
        const int rest = static_cast<int>(b->count - count);
        if (rest < 0)
            continue;
        b->owner = owner;
        usedBlocks.PushBack(b);
        freeBlocks.Erase(it);
        b->count = count;
        available -= count;
        if (rest > 0)
            freeBlocks.Insert(new S_bufferBlock{nullptr, b->offset + count, static_cast<uint32_t>(rest)});
        return static_cast<int>(b->offset);
    }
    return -1;
}

bool C_VertexBuffer::Free(uint32_t offset)
{
    for (int i = 0; i < usedBlocks.Size(); ++i) {
        S_bufferBlock *b = usedBlocks[i];
        if (b->offset != offset)
            continue;
        available += b->count;
        usedBlocks.RemoveAtSwap(i);
        uint32_t start = b->offset, size = b->count;
        int merged = 0;
        do {
            uint32_t extra = 0;
            auto it = freeBlocks.begin();
            for (; it != freeBlocks.end(); ++it) {
                S_bufferBlock *f = *it;
                if (f->offset == start + size) {
                    extra = f->count;
                    merged |= 1;
                    break;
                }
                if (f->offset + f->count == start) {
                    start = f->offset;
                    extra = f->count;
                    merged |= 2;
                    break;
                }
            }
            if (it == freeBlocks.end())
                break;
            S_bufferBlock *f = *it;
            freeBlocks.Erase(it);
            delete f;
            size += extra;
        } while (merged != 3);
        if (merged) {
            b->offset = start;
            b->count = size;
        }
        freeBlocks.Insert(b);
        g_vbManager.FreeUnused(this);
        return true;
    }
    return false;
}

uint8_t *C_VertexBuffer::Lock(uint32_t flags)
{
    if (lockedData) {
        if (!(lockFlags & D3DLOCK_READONLY) || flags)
            return lockedData;
        vb->Unlock();
    }
    lockFlags = flags;
    vb->Lock(0, 0, &lockedData, flags);
    if (lockIndex < 0)
        g_vbManager.AddLocked(this);
    return lockedData;
}

void C_VertexBuffer::Unlock()
{
    if (!lockedData)
        return;
    vb->Unlock();
    lockedData = nullptr;
    g_vbManager.RemoveLocked(this);
}

// ---------------------------------------------------------------- C_VBManager

C_VBManager::C_VBManager()
{
    lockedBuffers.Reserve(0x100);
    lockedBuffers.SetGrowBy(0x100);
}

C_VBManager::~C_VBManager()
{
    for (auto it = buffers.begin(); it != buffers.end(); ++it)
        delete *it;
    buffers.Erase(buffers.begin(), buffers.end());
}

void C_VBManager::AddLocked(C_VertexBuffer *b)
{
    b->lockIndex = lockedBuffers.Size();
    lockedBuffers.PushBack(b);
}

void C_VBManager::RemoveLocked(C_VertexBuffer *b)
{
    lockedBuffers[lockedBuffers.Size() - 1]->lockIndex = b->lockIndex;
    lockedBuffers.PopBack();
    if (lockedBuffers.Size())
        lockedBuffers[b->lockIndex] = lockedBuffers.Data()[lockedBuffers.Size()];
    b->lockIndex = -1;
}

bool C_VBManager::Alloc(uint32_t usage, uint32_t stride, uint32_t count, S_VBAlloc *alloc, uint32_t format,
                        bool newBuffer)
{
    if (!newBuffer) {
        for (auto it = buffers.begin(); it != buffers.end(); ++it) {
            C_VertexBuffer *b = *it;
            if (b->stride != stride || b->usage != usage || count > b->available)
                continue;
            const int offset = b->Allocate(count, alloc);
            if (offset != -1) {
                alloc->offset = static_cast<uint32_t>(offset);
                alloc->buffer = b;
                alloc->vb = b->vb;
                return true;
            }
        }
    }
    auto *b = new C_VertexBuffer;
    if (!b->Create(usage, stride, count, format)) {
        delete b;
        return false;
    }
    buffers.Insert(b);
    alloc->buffer = b;
    alloc->offset = static_cast<uint32_t>(b->Allocate(count, alloc));
    alloc->vb = b->vb;
    return true;
}

void C_VBManager::FreeUnused(C_VertexBuffer *vb)
{
    if (!vb) {
        for (auto it = buffers.begin(); it != buffers.end();) {
            C_VertexBuffer *b = *it;
            if (b->available != b->capacity) {
                ++it;
                continue;
            }
            delete b;
            buffers.Erase(it);
            it = buffers.begin();
        }
        return;
    }
    if (vb->available != vb->capacity)
        return;
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        if (*it == vb) {
            buffers.Erase(it);
            break;
        }
    }
    delete vb;
}

void C_VBManager::FreeAll()
{
    while (buffers.begin() != buffers.end()) {
        auto it = buffers.begin();
        C_VertexBuffer *b = *it;
        if (b->usedBlocks.Size() != 0 || b->freeBlocks.Size() != 1)
            dbgPrintf("C_VBManager::m_FreeAllVBManager() - WARNING! Releasing unreleased VertexBuffer !!! Some of "
                      "meshes are still unreleased ?!");
        delete b;
        buffers.Erase(it);
    }
}

void C_VBManager::ReleaseDefaultPool()
{
    for (auto it = buffers.begin(); it != buffers.end();) {
        C_VertexBuffer *b = *it;
        if (b->pool != D3DPOOL_DEFAULT) {
            ++it;
            continue;
        }
        delete b;
        buffers.Erase(it);
        it = buffers.begin();
    }
}

void C_VBManager::UnlockAll()
{
    for (C_VertexBuffer *b : lockedBuffers) {
        b->vb->Unlock();
        b->lockedData = nullptr;
        b->lockIndex = -1;
    }
    lockedBuffers.Clear();
}

void FreeVB(S_VBAlloc *alloc)
{
    if (alloc->buffer)
        alloc->buffer->Free(alloc->offset);
}

}  // namespace ls3d
