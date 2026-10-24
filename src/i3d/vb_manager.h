// C_VBManager / C_IBManager: pools of Direct3D vertex and index buffers shared by the meshes
// (VBManager.cpp 0x10066020 - 0x10068620, IBManager.cpp 0x10057960 - 0x10058ee0).
//
// A manager keeps its buffers in a tree ordered by free space (MSVC 6's multiset, see
// core/multiset.h: the key changes while a buffer is in the tree, the order follows the tree's
// shape) and hands out pieces of them first fit. A buffer splits its space into blocks: the free
// ones in a tree by size, the used ones in a list; a freed block merges with its free neighbours.
// New buffers are locked at once (the meshes fill them); the renderer unlocks every locked buffer
// before it draws (UnlockAll).
//
// Quirk kept: an empty allocation (count 0, e.g. a face group without faces) gets a used block at
// the offset the next allocation also gets; Free finds blocks by offset, so freeing either may
// free the other's block, and when the buffer goes the wrong record is cleared.
#pragma once

#include <cstdint>

#include "core/c_vector.h"
#include "core/multiset.h"
#include "dx/d3d8.h"

namespace ls3d {

class C_IndexBuffer;
class C_VertexBuffer;

// A mesh's piece of a shared buffer (what the managers' Alloc fills in; all zero when none).
struct S_IBAlloc {
    C_IndexBuffer *buffer;                  // +0x00
    IDirect3DIndexBuffer8 *ib;              // +0x04
    uint32_t offset;                        // +0x08 in indices
};
struct S_VBAlloc {
    C_VertexBuffer *buffer;                 // +0x00
    IDirect3DVertexBuffer8 *vb;             // +0x04
    uint32_t offset;                        // +0x08 in vertices
};

// A run of a buffer's indices / vertices.
struct S_bufferBlock {
    void *owner;                            // +0x00 the S_IBAlloc / S_VBAlloc it went to (kept when freed)
    uint32_t offset;                        // +0x04
    uint32_t count;                         // +0x08
};
struct BlockBySize {
    bool operator()(const S_bufferBlock *a, const S_bufferBlock *b) const { return a->count < b->count; }
};

// One Direct3D index buffer (16-bit indices) and its blocks (0x40 bytes).
class C_IndexBuffer {
public:
    C_IndexBuffer();
    // 0x10057a80: clears the allocations still pointing here, unlocks, releases the buffer.
    ~C_IndexBuffer();
    C_IndexBuffer(const C_IndexBuffer &) = delete;
    C_IndexBuffer &operator=(const C_IndexBuffer &) = delete;

    // 0x10057c90: the Direct3D buffer for at least `count` indices (64K at least; on failure
    // the resource manager's memory is discarded, then the managed pool tried), left locked.
    bool Create(uint32_t usage, uint32_t count);
    // 0x10057e80: the first free block (by size) that holds `count`; returns its offset or -1.
    int Allocate(uint32_t count, S_IBAlloc *owner);
    // 0x10058040: the used block at `offset` back to the free blocks, merged with its neighbours.
    bool Free(uint32_t offset);

    // The indices from the buffer's lock (inlined by the meshes, 0x10030b40): a read-only lock
    // (D3DLOCK_READONLY) is turned into a writable one when needed.
    uint16_t *Lock(uint32_t flags);
    // Unlocks (0x10030c40, inlined): after a writable lock the original locks and unlocks once
    // more without flags.
    void Unlock();

    uint16_t *lockedData;                   // +0x00 while locked
    uint32_t lockFlags;                     // +0x04 of the current lock
    uint32_t usage;                         // +0x08 D3DUSAGE_*
    uint32_t pool;                          // +0x0C D3DPOOL
    IDirect3DIndexBuffer8 *ib;              // +0x10
    Multiset<S_bufferBlock *, BlockBySize> freeBlocks;  // +0x14
    C_vector<S_bufferBlock *> usedBlocks;   // +0x24
    uint32_t capacity;                      // +0x34 indices
    uint32_t available;                     // +0x38 indices in free blocks
    int lockIndex;                          // +0x3C in the manager's locked list (-1: not in it)
};
static_assert(sizeof(C_IndexBuffer) == 0x40, "C_IndexBuffer layout");

// One Direct3D vertex buffer and its blocks (0x48 bytes).
class C_VertexBuffer {
public:
    C_VertexBuffer();
    // 0x10066110: as ~C_IndexBuffer (unlocked without the extra lock).
    ~C_VertexBuffer();
    C_VertexBuffer(const C_VertexBuffer &) = delete;
    C_VertexBuffer &operator=(const C_VertexBuffer &) = delete;

    // 0x10066310: room for at least `count` vertices of `stride` bytes (128 KB at least).
    bool Create(uint32_t usage, uint32_t stride, uint32_t count, uint32_t format);
    int Allocate(uint32_t count, S_VBAlloc *owner);         // 0x10066520
    // 0x100666e0: as C_IndexBuffer::Free; a buffer left empty is released (FreeUnused).
    bool Free(uint32_t offset);

    // The vertices from the buffer's lock (inlined by the meshes, 0x10031380).
    uint8_t *Lock(uint32_t flags);
    void Unlock();                          // 0x10031480 (inlined)

    uint8_t *lockedData;                    // +0x00 while locked
    uint32_t lockFlags;                     // +0x04 of the current lock
    uint32_t format;                        // +0x08 the vertex layout the mesh asked for (0 - 3; 2 before Create)
    uint32_t pool;                          // +0x0C D3DPOOL
    uint32_t usage;                         // +0x10 D3DUSAGE_*
    uint32_t stride;                        // +0x14 bytes per vertex
    IDirect3DVertexBuffer8 *vb;             // +0x18
    Multiset<S_bufferBlock *, BlockBySize> freeBlocks;  // +0x1C
    C_vector<S_bufferBlock *> usedBlocks;   // +0x2C
    uint32_t capacity;                      // +0x3C vertices
    uint32_t available;                     // +0x40 vertices in free blocks
    int lockIndex;                          // +0x44 in the manager's locked list (-1: not in it)
};
static_assert(sizeof(C_VertexBuffer) == 0x48, "C_VertexBuffer layout");

template <class B> struct BufferByAvailable {
    bool operator()(const B *a, const B *b) const { return a->available < b->available; }
};

class C_IBManager {
public:
    C_IBManager();                          // 0x10058220
    ~C_IBManager();                         // 0x10058450 (at exit)
    C_IBManager(const C_IBManager &) = delete;
    C_IBManager &operator=(const C_IBManager &) = delete;

    // 0x10058550: `count` indices from the first buffer of the same usage with room, else from a
    // new buffer; fills `alloc`.
    bool Alloc(uint32_t usage, uint32_t count, S_IBAlloc *alloc);
    void FreeUnused();                      // 0x10058860 buffers with nothing allocated
    void FreeAll();                         // 0x100588c0 warns about buffers still in use
    void ReleaseDefaultPool();              // 0x10058930 before a device reset
    void UnlockAll();                       // 0x100587f0 (the renderer, before drawing)

    // The locked buffers: added by Create / Lock, taken out by Unlock.
    void AddLocked(C_IndexBuffer *b);
    void RemoveLocked(C_IndexBuffer *b);

    C_vector<C_IndexBuffer *> lockedBuffers;    // +0x00
    Multiset<C_IndexBuffer *, BufferByAvailable<C_IndexBuffer>> buffers;    // +0x10
};
static_assert(sizeof(C_IBManager) == 0x20, "C_IBManager layout");

class C_VBManager {
public:
    C_VBManager();                          // 0x100668c0
    ~C_VBManager();                         // 0x10066af0 (at exit)
    C_VBManager(const C_VBManager &) = delete;
    C_VBManager &operator=(const C_VBManager &) = delete;

    // 0x10066bf0: `count` vertices from the first buffer of the same usage and stride with room
    // (unless `newBuffer`), else from a new buffer; fills `alloc`.
    bool Alloc(uint32_t usage, uint32_t stride, uint32_t count, S_VBAlloc *alloc, uint32_t format, bool newBuffer);
    // 0x10066ef0: releases the buffers that hold nothing (all, or just `vb`: then also when it
    // is not in the tree).
    void FreeUnused(C_VertexBuffer *vb = nullptr);
    void FreeAll();                         // 0x10066fb0
    void ReleaseDefaultPool();              // 0x10067020
    void UnlockAll();                       // 0x10066eb0

    void AddLocked(C_VertexBuffer *b);
    void RemoveLocked(C_VertexBuffer *b);

    C_vector<C_VertexBuffer *> lockedBuffers;   // +0x00
    Multiset<C_VertexBuffer *, BufferByAvailable<C_VertexBuffer>> buffers;  // +0x10
};
static_assert(sizeof(C_VBManager) == 0x20, "C_VBManager layout");

extern C_VBManager g_vbManager;                 // 0x100d1400
extern C_IBManager g_ibManager;                 // 0x100c05f0

// An allocation back to its buffer (0x100587d0 / 0x10066e90; the caller clears it).
void FreeIB(S_IBAlloc *alloc);
void FreeVB(S_VBAlloc *alloc);

}  // namespace ls3d
