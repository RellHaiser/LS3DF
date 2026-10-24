// C_IBManager, C_IndexBuffer (IBManager.cpp, 0x10057960 - 0x10058ee0).
#include "i3d/vb_manager.h"

#include "core/debug.h"
#include "core/i3d_result.h"
#include "dx/dxerr8.h"
#include "i3d/driver.h"
#include "igraph/igraph_state.h"

namespace ls3d {

using ig::g_pDevice;
using i3d::g_driver;

C_IBManager g_ibManager;

// ---------------------------------------------------------------- C_IndexBuffer

C_IndexBuffer::C_IndexBuffer()
    : lockedData(nullptr), lockFlags(0), usage(0), pool(D3DPOOL_MANAGED), ib(nullptr), capacity(0), available(0),
      lockIndex(-1)
{
    usedBlocks.SetGrowBy(0x100);
    usedBlocks.Reserve(0x100);
}

C_IndexBuffer::~C_IndexBuffer()
{
    for (S_bufferBlock *b : usedBlocks) {
        auto *owner = static_cast<S_IBAlloc *>(b->owner);
        owner->buffer = nullptr;
        owner->ib = nullptr;
        owner->offset = 0;
        delete b;
    }
    for (auto it = freeBlocks.begin(); it != freeBlocks.end(); ++it)
        delete *it;
    if (lockedData)
        Unlock();
    if (ib)
        ib->Release();
    if (g_driver)
        g_driver->SlotB4();
    usedBlocks.Free();
}

bool C_IndexBuffer::Create(uint32_t newUsage, uint32_t count)
{
    usage = newUsage;
    const uint32_t n = count < 0x10000 ? 0x10000 : count;
    capacity = n;
    available = n;
    pool = !(usage & D3DUSAGE_DYNAMIC) && (usage & D3DUSAGE_SOFTWAREPROCESSING) ? D3DPOOL_MANAGED : D3DPOOL_DEFAULT;
    HRESULT hr = g_pDevice->CreateIndexBuffer(n * 2, usage, D3DFMT_INDEX16, static_cast<D3DPOOL>(pool), &ib);
    if (FAILED(hr)) {
        g_pDevice->ResourceManagerDiscardBytes(0);
        hr = g_pDevice->CreateIndexBuffer(capacity * 2, usage, D3DFMT_INDEX16, static_cast<D3DPOOL>(pool), &ib);
        if (FAILED(hr)) {
            pool = D3DPOOL_MANAGED;
            hr = g_pDevice->CreateIndexBuffer(capacity * 2, usage, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
            if (FAILED(hr)) {
                dbgPrintf("!! Error detected: %s, File: %s, Line: %i !!", DXGetErrorString8A(hr),
                          "C:\\MyProjects\\Engine8_5\\I3D\\IBManager.cpp", 129);
                ib = nullptr;
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return false;
            }
        }
    }
    freeBlocks.Insert(new S_bufferBlock{nullptr, 0, capacity});
    g_driver->SlotB4();
    ib->Lock(0, 0, reinterpret_cast<BYTE **>(&lockedData), (usage & D3DUSAGE_DYNAMIC) << 4);   // D3DLOCK_DISCARD
    g_ibManager.AddLocked(this);
    return true;
}

int C_IndexBuffer::Allocate(uint32_t count, S_IBAlloc *owner)
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

bool C_IndexBuffer::Free(uint32_t offset)
{
    for (int i = 0; i < usedBlocks.Size(); ++i) {
        S_bufferBlock *b = usedBlocks[i];
        if (b->offset != offset)
            continue;
        available += b->count;
        usedBlocks.RemoveAtSwap(i);
        // Merge with the free blocks right after and right before (searched again after each).
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
        return true;
    }
    return false;
}

uint16_t *C_IndexBuffer::Lock(uint32_t flags)
{
    if (lockedData) {
        if (!(lockFlags & D3DLOCK_READONLY) || flags)
            return lockedData;
        ib->Unlock();
    }
    lockFlags = flags;
    ib->Lock(0, 0, reinterpret_cast<BYTE **>(&lockedData), flags);
    if (lockIndex < 0)
        g_ibManager.AddLocked(this);
    return lockedData;
}

void C_IndexBuffer::Unlock()
{
    if (!lockedData)
        return;
    ib->Unlock();
    if (!(lockFlags & D3DLOCK_READONLY)) {
        ib->Lock(0, 0, reinterpret_cast<BYTE **>(&lockedData), 0);
        ib->Unlock();
    }
    lockedData = nullptr;
    g_ibManager.RemoveLocked(this);
}

// ---------------------------------------------------------------- C_IBManager

C_IBManager::C_IBManager()
{
    lockedBuffers.Reserve(0x100);
    lockedBuffers.SetGrowBy(0x100);
}

C_IBManager::~C_IBManager()
{
    for (auto it = buffers.begin(); it != buffers.end(); ++it)
        delete *it;
    buffers.Erase(buffers.begin(), buffers.end());
}

void C_IBManager::AddLocked(C_IndexBuffer *b)
{
    b->lockIndex = lockedBuffers.Size();
    lockedBuffers.PushBack(b);
}

void C_IBManager::RemoveLocked(C_IndexBuffer *b)
{
    lockedBuffers[lockedBuffers.Size() - 1]->lockIndex = b->lockIndex;
    lockedBuffers.PopBack();
    if (lockedBuffers.Size())
        lockedBuffers[b->lockIndex] = lockedBuffers.Data()[lockedBuffers.Size()];
    b->lockIndex = -1;
}

bool C_IBManager::Alloc(uint32_t usage, uint32_t count, S_IBAlloc *alloc)
{
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        C_IndexBuffer *b = *it;
        if (b->usage != usage || count > b->available)
            continue;
        const int offset = b->Allocate(count, alloc);
        if (offset != -1) {
            alloc->buffer = b;
            alloc->offset = static_cast<uint32_t>(offset);
            alloc->ib = b->ib;
            return true;
        }
    }
    auto *b = new C_IndexBuffer;
    if (!b->Create(usage, count)) {
        delete b;
        return false;
    }
    buffers.Insert(b);
    alloc->buffer = b;
    alloc->offset = static_cast<uint32_t>(b->Allocate(count, alloc));
    alloc->ib = b->ib;
    return true;
}

void C_IBManager::FreeUnused()
{
    for (auto it = buffers.begin(); it != buffers.end();) {
        C_IndexBuffer *b = *it;
        if (b->available != b->capacity) {
            ++it;
            continue;
        }
        delete b;
        buffers.Erase(it);
        it = buffers.begin();
    }
}

void C_IBManager::FreeAll()
{
    while (buffers.begin() != buffers.end()) {
        auto it = buffers.begin();
        C_IndexBuffer *b = *it;
        if (b->usedBlocks.Size() != 0 || b->freeBlocks.Size() != 1)
            dbgPrintf("C_IBManager::m_FreeAllIBManager() - WARNING! Releasing unreleased IndexBuffer !!! Some of "
                      "meshes are still unreleased ?!");
        delete b;
        buffers.Erase(it);
    }
}

void C_IBManager::ReleaseDefaultPool()
{
    for (auto it = buffers.begin(); it != buffers.end();) {
        C_IndexBuffer *b = *it;
        if (b->pool != D3DPOOL_DEFAULT) {
            ++it;
            continue;
        }
        delete b;
        buffers.Erase(it);
        it = buffers.begin();
    }
}

void C_IBManager::UnlockAll()
{
    for (C_IndexBuffer *b : lockedBuffers) {
        b->ib->Unlock();
        if (!(b->lockFlags & D3DLOCK_READONLY)) {
            b->ib->Lock(0, 0, reinterpret_cast<BYTE **>(&b->lockedData), 0);
            b->ib->Unlock();
        }
        b->lockedData = nullptr;
        b->lockIndex = -1;
    }
    lockedBuffers.Clear();
}

void FreeIB(S_IBAlloc *alloc)
{
    if (alloc->buffer)
        alloc->buffer->Free(alloc->offset);
}

}  // namespace ls3d
