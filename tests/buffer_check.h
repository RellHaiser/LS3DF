// Test support shared by compare_buffers and compare_mesh: the original DLL's address helpers,
// mismatch reports, a fake Direct3D device per side whose vertex / index buffers keep their bytes
// and log every call, and the comparison of both sides' buffer managers (trees node for node,
// buffers, locked lists, device logs).
//
// The including test sets g_ownerIndex: an allocation record's identity (the S_IBAlloc /
// S_VBAlloc a block points back to) on its side.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../src/i3d/vb_manager.h"
#include "../src/igraph/igraph_state.h"
#include "fake_d3d8_base.h"

namespace {

using namespace ls3d;

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }
template <class F> F OrigFn(uint32_t va) { return reinterpret_cast<F>(g_base + (va - 0x10000000)); }

std::mt19937 g_rng;
long g_op;
std::string g_lastOp = "start";
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }

[[noreturn]] void Fail(const std::string &what, long a, long b)
{
    std::printf("MISMATCH after op %ld (%s): %s: original %ld (0x%lx), port %ld (0x%lx)\n", g_op, g_lastOp.c_str(),
                what.c_str(), a, static_cast<unsigned long>(a), b, static_cast<unsigned long>(b));
    std::exit(1);
}
void Check(const std::string &what, long a, long b)
{
    if (a != b)
        Fail(what, a, b);
}

// ---------------------------------------------------------------- the fake devices

std::vector<std::string> g_log[2];          // [0] the original's device, [1] the port's
int g_failCreates[2];

void Log(int side, const char *format, ...)
{
    char text[160];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_log[side].push_back(text);
}

struct FakeIB : FakeDirect3DIndexBuffer8Base {
    int side = 0, id = 0;
    std::vector<uint8_t> data;
    HRESULT __stdcall Lock(UINT offset, UINT size, BYTE **out, DWORD flags) override
    {
        Log(side, "IB %d lock %u %u %lx", id, offset, size, flags);
        *out = data.data();
        return S_OK;
    }
    HRESULT __stdcall Unlock() override
    {
        Log(side, "IB %d unlock", id);
        return S_OK;
    }
    // (The object stays, for its identity; its bytes go: thousands of freed buffers would fill
    // the 32-bit address space.)
    void OnFinalRelease() override
    {
        Log(side, "IB %d freed", id);
        std::vector<uint8_t>().swap(data);
    }
};
struct FakeVB : FakeDirect3DVertexBuffer8Base {
    int side = 0, id = 0;
    std::vector<uint8_t> data;
    HRESULT __stdcall Lock(UINT offset, UINT size, BYTE **out, DWORD flags) override
    {
        Log(side, "VB %d lock %u %u %lx", id, offset, size, flags);
        *out = data.data();
        return S_OK;
    }
    HRESULT __stdcall Unlock() override
    {
        Log(side, "VB %d unlock", id);
        return S_OK;
    }
    // (The object stays, for its identity; its bytes go: thousands of freed buffers would fill
    // the 32-bit address space.)
    void OnFinalRelease() override
    {
        Log(side, "VB %d freed", id);
        std::vector<uint8_t>().swap(data);
    }
};

std::vector<FakeIB *> g_ibs[2];
std::vector<FakeVB *> g_vbs[2];

struct BufferDevice : FakeDirect3DDevice8Base {
    int side = 0;
    HRESULT __stdcall ResourceManagerDiscardBytes(DWORD bytes) override
    {
        Log(side, "discard %lu", bytes);
        return S_OK;
    }
    HRESULT __stdcall CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                        IDirect3DIndexBuffer8 **out) override
    {
        Log(side, "create IB %u %lx %d %d", length, usage, format, pool);
        if (g_failCreates[side] > 0) {
            --g_failCreates[side];
            return E_OUTOFMEMORY;
        }
        auto *b = new FakeIB;
        b->side = side;
        b->id = static_cast<int>(g_ibs[side].size());
        b->data.resize(length + 0x10000);      // (slack: some fills write past their allocation)
        g_ibs[side].push_back(b);
        *out = b;
        return S_OK;
    }
    HRESULT __stdcall CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                         IDirect3DVertexBuffer8 **out) override
    {
        Log(side, "create VB %u %lx %lx %d", length, usage, fvf, pool);
        if (g_failCreates[side] > 0) {
            --g_failCreates[side];
            return E_OUTOFMEMORY;
        }
        auto *b = new FakeVB;
        b->side = side;
        b->id = static_cast<int>(g_vbs[side].size());
        b->data.resize(length + 0x10000);      // (slack: some fills write past their allocation)
        g_vbs[side].push_back(b);
        *out = b;
        return S_OK;
    }
};
BufferDevice g_device[2];

template <class F> int FakeIndex(const void *b, const std::vector<F *> &list)
{
    if (!b)
        return -1;
    for (size_t k = 0; k < list.size(); ++k) {
        if (b == static_cast<const void *>(list[k]))
            return static_cast<int>(k);
    }
    return -2;
}

// A locked pointer as (buffer, byte offset).
template <class F> long LockedAt(const void *p, const std::vector<F *> &list)
{
    if (!p)
        return -1;
    for (size_t k = 0; k < list.size(); ++k) {
        const uint8_t *d = list[k]->data.data();
        if (p >= d && p <= d + list[k]->data.size())
            return static_cast<long>(k << 24 | static_cast<size_t>(static_cast<const uint8_t *>(p) - d));
    }
    return -2;
}

// Both sides' device logs, then cleared.
void CompareLogs()
{
    Check("device calls", static_cast<long>(g_log[0].size()), static_cast<long>(g_log[1].size()));
    for (size_t k = 0; k < g_log[0].size(); ++k) {
        if (g_log[0][k] != g_log[1][k]) {
            std::printf("MISMATCH after op %ld (%s): device call %zu: original \"%s\", port \"%s\"\n", g_op,
                        g_lastOp.c_str(), k, g_log[0][k].c_str(), g_log[1][k].c_str());
            std::exit(1);
        }
    }
    g_log[0].clear();
    g_log[1].clear();
}

// Every fake buffer's bytes (slow: now and then).
void CompareBufferContents()
{
    for (int kind = 0; kind < 2; ++kind) {
        const size_t n = kind ? g_vbs[0].size() : g_ibs[0].size();
        for (size_t k = 0; k < n; ++k) {
            const std::vector<uint8_t> &a = kind ? g_vbs[0][k]->data : g_ibs[0][k]->data;
            const std::vector<uint8_t> &b = kind ? g_vbs[1][k]->data : g_ibs[1][k]->data;
            if (a != b) {
                size_t at = 0;
                while (at < a.size() && at < b.size() && a[at] == b[at])
                    ++at;
                Fail(std::string(kind ? "VB" : "IB") + " " + std::to_string(k) + " contents at byte", static_cast<long>(at),
                     static_cast<long>(at));
            }
        }
    }
}

// ---------------------------------------------------------------- the managers

int (*g_ownerIndex)(const void *owner, int side, bool vb);

template <class T, class L> using Node = typename Multiset<T, L>::Node;

// A tree's values in order (the original's tree has its own nil node: no iterators).
template <class N, class T> void InOrder(const N *n, const void *nil, std::vector<T> &out)
{
    if (n == nil)
        return;
    InOrder(n->left, nil, out);
    out.push_back(n->value);
    InOrder(n->right, nil, out);
}

// Both trees node for node; `same` compares two values.
template <class T, class L, class F>
void CompareTree(const std::string &what, const Multiset<T, L> &o, void *origNil, const Multiset<T, L> &p, F same)
{
    using N = Node<T, L>;
    Check(what + " size", o.Size(), p.Size());
    std::map<const N *, const N *> pairs;
    const N *pNil = Multiset<T, L>::Nil();
    struct Walk {
        static void Go(const std::string &w, const N *a, const N *b, const void *aNil, const N *bNil,
                       std::map<const N *, const N *> &pairs, F &same, int depth)
        {
            Check(w + " node", a == aNil, b == bNil);
            if (a == aNil)
                return;
            if (depth > 64)
                Fail(w + " too deep", depth, depth);
            Check(w + " colour", a->color, b->color);
            same(a->value, b->value);
            pairs[a] = b;
            Go(w, a->left, b->left, aNil, bNil, pairs, same, depth + 1);
            Go(w, a->right, b->right, aNil, bNil, pairs, same, depth + 1);
        }
    };
    const N *oh = o.Head(), *ph = p.Head();
    pairs[oh] = ph;
    Walk::Go(what, oh->parent, ph->parent, origNil, pNil, pairs, same, 0);
    Check(what + " leftmost", pairs.count(oh->left) && pairs[oh->left] == ph->left, 1);
    Check(what + " rightmost", pairs.count(oh->right) && pairs[oh->right] == ph->right, 1);
    Check(what + " head colour", oh->color, ph->color);
    // The parents follow from the shape unless a link is broken.
    for (auto &[a, b] : pairs) {
        if (a == oh)
            continue;
        const N *ap = a->parent;
        Check(what + " parent", pairs.count(ap) && pairs[ap] == b->parent, 1);
    }
}

// (A free block keeps the owner it had: compared for used blocks only.)
void CompareBlock(const std::string &what, const S_bufferBlock *o, const S_bufferBlock *p, bool vb, bool used)
{
    if (used)
        Check(what + " owner", g_ownerIndex(o->owner, 0, vb), g_ownerIndex(p->owner, 1, vb));
    Check(what + " offset", o->offset, p->offset);
    Check(what + " count", o->count, p->count);
}

template <class V> void CompareVector(const std::string &what, const C_vector<V> &o, const C_vector<V> &p)
{
    Check(what + " size", o.Size(), p.Size());
    Check(what + " capacity", o.Capacity(), p.Capacity());
    Check(what + " grow", reinterpret_cast<const int *>(&o)[3], reinterpret_cast<const int *>(&p)[3]);
    Check(what + " data", o.Data() != nullptr, p.Data() != nullptr);
}

void CompareIB(const C_IndexBuffer *o, const C_IndexBuffer *p)
{
    Check("IB", FakeIndex(o->ib, g_ibs[0]), FakeIndex(p->ib, g_ibs[1]));
    Check("IB locked", LockedAt(o->lockedData, g_ibs[0]), LockedAt(p->lockedData, g_ibs[1]));
    Check("IB lockFlags", o->lockFlags, p->lockFlags);
    Check("IB usage", o->usage, p->usage);
    Check("IB pool", o->pool, p->pool);
    Check("IB capacity", o->capacity, p->capacity);
    Check("IB available", o->available, p->available);
    Check("IB lockIndex", o->lockIndex, p->lockIndex);
    CompareTree("IB free blocks", o->freeBlocks, Orig<void *>(0x100c0624), p->freeBlocks,
                [](S_bufferBlock *a, S_bufferBlock *b) { CompareBlock("IB free block", a, b, false, false); });
    CompareVector("IB used", o->usedBlocks, p->usedBlocks);
    for (int k = 0; k < o->usedBlocks.Size(); ++k)
        CompareBlock("IB used block", o->usedBlocks[k], p->usedBlocks[k], false, true);
}

void CompareVBuf(const C_VertexBuffer *o, const C_VertexBuffer *p)
{
    Check("VB", FakeIndex(o->vb, g_vbs[0]), FakeIndex(p->vb, g_vbs[1]));
    Check("VB locked", LockedAt(o->lockedData, g_vbs[0]), LockedAt(p->lockedData, g_vbs[1]));
    Check("VB lockFlags", o->lockFlags, p->lockFlags);
    Check("VB format", o->format, p->format);
    Check("VB pool", o->pool, p->pool);
    Check("VB usage", o->usage, p->usage);
    Check("VB stride", o->stride, p->stride);
    Check("VB capacity", o->capacity, p->capacity);
    Check("VB available", o->available, p->available);
    Check("VB lockIndex", o->lockIndex, p->lockIndex);
    CompareTree("VB free blocks", o->freeBlocks, Orig<void *>(0x100d1424), p->freeBlocks,
                [](S_bufferBlock *a, S_bufferBlock *b) { CompareBlock("VB free block", a, b, true, false); });
    CompareVector("VB used", o->usedBlocks, p->usedBlocks);
    for (int k = 0; k < o->usedBlocks.Size(); ++k)
        CompareBlock("VB used block", o->usedBlocks[k], p->usedBlocks[k], true, true);
}

const C_IBManager &OrigIBM() { return Orig<C_IBManager>(0x100c05f0); }
const C_VBManager &OrigVBM() { return Orig<C_VBManager>(0x100d1400); }

// An allocation record's buffer by its fake Direct3D buffer.
long BufferOf(const C_IndexBuffer *b, int side) { return b ? FakeIndex(b->ib, g_ibs[side]) : -1; }
long BufferOf(const C_VertexBuffer *b, int side) { return b ? FakeIndex(b->vb, g_vbs[side]) : -1; }

void CompareAlloc(const std::string &what, const S_IBAlloc &a, const S_IBAlloc &b)
{
    Check(what + " buffer", BufferOf(a.buffer, 0), BufferOf(b.buffer, 1));
    Check(what + " ib", FakeIndex(a.ib, g_ibs[0]), FakeIndex(b.ib, g_ibs[1]));
    Check(what + " offset", a.offset, b.offset);
}
void CompareAlloc(const std::string &what, const S_VBAlloc &a, const S_VBAlloc &b)
{
    Check(what + " buffer", BufferOf(a.buffer, 0), BufferOf(b.buffer, 1));
    Check(what + " vb", FakeIndex(a.vb, g_vbs[0]), FakeIndex(b.vb, g_vbs[1]));
    Check(what + " offset", a.offset, b.offset);
}

void CompareManagers()
{
    CompareTree("IB buffers", OrigIBM().buffers, Orig<void *>(0x100c062c), g_ibManager.buffers, CompareIB);
    CompareVector("IB locked list", OrigIBM().lockedBuffers, g_ibManager.lockedBuffers);
    for (int k = 0; k < OrigIBM().lockedBuffers.Size(); ++k)
        Check("IB locked list entry", FakeIndex(OrigIBM().lockedBuffers[k]->ib, g_ibs[0]),
              FakeIndex(g_ibManager.lockedBuffers[k]->ib, g_ibs[1]));
    CompareTree("VB buffers", OrigVBM().buffers, Orig<void *>(0x100d142c), g_vbManager.buffers, CompareVBuf);
    CompareVector("VB locked list", OrigVBM().lockedBuffers, g_vbManager.lockedBuffers);
    for (int k = 0; k < OrigVBM().lockedBuffers.Size(); ++k)
        Check("VB locked list entry", FakeIndex(OrigVBM().lockedBuffers[k]->vb, g_vbs[0]),
              FakeIndex(g_vbManager.lockedBuffers[k]->vb, g_vbs[1]));
}

// The port's driver for the managers alone: SlotB4 (a no-op) is all they call.
void __stdcall NoOp(void *) {}
void *g_driverVtable[64];
void *g_fakeDriver = g_driverVtable;

// Loads the original, points each side at its fake device. False when it cannot.
bool SetUpBuffers(const char *origPath)
{
    HMODULE dll = LoadLibraryA(origPath);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", origPath, GetLastError());
        return false;
    }
    g_base = reinterpret_cast<uint8_t *>(dll);
    Orig<uint8_t>(0x100d16f2) = 1;              // no log
    if (!Orig<void *>(0x100d16f8)) {
        std::printf("the original's driver does not exist\n");
        return false;
    }
    g_device[0].side = 0;
    g_device[1].side = 1;
    Orig<IDirect3DDevice8 *>(0x100d216c) = &g_device[0];
    ig::g_pDevice = &g_device[1];
    for (void *&slot : g_driverVtable)
        slot = reinterpret_cast<void *>(&NoOp);
    return true;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
    std::printf("CRASH (0x%08lx at %p) in op %ld (%s)\n", info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress, g_op, g_lastOp.c_str());
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace
