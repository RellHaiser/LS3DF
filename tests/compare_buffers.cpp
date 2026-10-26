// Differential test of the vertex / index buffer managers (src/i3d/vb_manager.cpp,
// ib_manager.cpp, core/multiset.h): the original LS3DF.dll's g_ibManager / g_vbManager and the
// port's run the same random operations - allocations of every usage, stride and size (into the
// existing buffers or new ones, some creations failing to exercise the retries), frees with the
// merging of neighbouring blocks, FreeUnused (all or one buffer), FreeAll and ReleaseDefaultPool
// with allocations still alive, UnlockAll - each side on its own fake Direct3D device whose buffers
// log every call. After every operation the managers must be identical: both trees node for node
// (shape, colours, the head's links), every buffer's fields, free tree and used list, the locked
// lists, the allocation records, and the device logs.
//
// No allocation is empty: an empty block shares its offset with the next allocation's, Free by
// offset may then free that one instead, and a record can be left pointing at a released buffer
// (in both; vb_manager.h).
//
// Usage: compare_buffers.exe [original LS3DF.dll] [operations] [seed]
//
// test-sources: src/i3d/ib_manager.cpp src/i3d/vb_manager.cpp src/core/debug.cpp src/dx/dxerr8.cpp
// test-sources: src/gfx/d3d8/gfx_d3d8.cpp
#include "../src/core/debug.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/vb_manager.h"
#include "../src/igraph/igraph_state.h"
#include "buffer_check.h"

#include <algorithm>
#include "test_paths.h"

namespace ls3d::i3d {
I3D_driver *g_driver;
}

namespace {

// ---------------------------------------------------------------- allocations

constexpr int kAllocs = 400;
S_IBAlloc g_ibAlloc[2][kAllocs];
S_VBAlloc g_vbAlloc[2][kAllocs];

int AllocIndex(const void *owner, int side, bool vb)
{
    if (!owner)
        return -1;
    for (int k = 0; k < kAllocs; ++k) {
        if (vb ? owner == &g_vbAlloc[side][k] : owner == &g_ibAlloc[side][k])
            return k;
    }
    return -2;
}

void CompareAll()
{
    CompareManagers();
    for (int k = 0; k < kAllocs; ++k) {
        CompareAlloc("IB alloc", g_ibAlloc[0][k], g_ibAlloc[1][k]);
        CompareAlloc("VB alloc", g_vbAlloc[0][k], g_vbAlloc[1][k]);
    }
    CompareLogs();
}

// ---------------------------------------------------------------- operations

using ThisFn = void(__fastcall *)(void *, void *);

uint32_t RandUsage()
{
    static const uint32_t kUsages[] = {0x10, 0x18, 0x8, 0x208, 0x210, 0x0};
    return kUsages[Rand(Rand(4) == 0 ? 6 : 2)];
}

void SetFailures()
{
    const int f = Rand(12) == 0 ? 1 + Rand(2) : 0;
    g_failCreates[0] = g_failCreates[1] = f;
}

long g_allocs, g_newBuffers, g_merges;
int g_mostBuffers;


void RunOp()
{
    void *const ibm = &Orig<uint8_t>(0x100c05f0);
    void *const vbm = &Orig<uint8_t>(0x100d1400);
    const int k = Rand(kAllocs);
    switch (Rand(24)) {
    case 0:
    case 1:
    case 2:
    case 3: {
        if (g_ibAlloc[1][k].buffer)
            break;
        g_lastOp = "IB Alloc";
        const uint32_t usage = RandUsage();
        const uint32_t count = Rand(16) == 0 ? 0x8000 + Rand(0x10000) : Rand(8) == 0 ? 1 + Rand(3) : 1 + Rand(3000);
        SetFailures();
        const int before = g_ibManager.buffers.Size();
        using AllocFn = bool(__fastcall *)(void *, void *, uint32_t, uint32_t, S_IBAlloc *);
        const bool ro = OrigFn<AllocFn>(0x10058550)(ibm, nullptr, usage, count, &g_ibAlloc[0][k]);
        const bool rp = g_ibManager.Alloc(usage, count, &g_ibAlloc[1][k]);
        Check("IB Alloc", ro, rp);
        ++g_allocs;
        g_newBuffers += g_ibManager.buffers.Size() != before;
        break;
    }
    case 4:
    case 5:
    case 6: {
        if (!g_ibAlloc[1][k].buffer && Rand(4) != 0)
            break;
        g_lastOp = "IB free";
        OrigFn<void(__stdcall *)(S_IBAlloc *)>(0x100587d0)(&g_ibAlloc[0][k]);
        FreeIB(&g_ibAlloc[1][k]);
        g_ibAlloc[0][k] = S_IBAlloc{};
        g_ibAlloc[1][k] = S_IBAlloc{};
        break;
    }
    case 7:
        if (Rand(6) != 0)
            break;
        g_lastOp = "IB FreeUnused";
        OrigFn<ThisFn>(0x10058860)(ibm, nullptr);
        g_ibManager.FreeUnused();
        break;
    case 8:
        if (Rand(40) != 0)
            break;
        g_lastOp = "IB FreeAll";
        OrigFn<ThisFn>(0x100588c0)(ibm, nullptr);
        g_ibManager.FreeAll();
        break;
    case 9:
        if (Rand(20) != 0)
            break;
        g_lastOp = "IB ReleaseDefaultPool";
        OrigFn<ThisFn>(0x10058930)(ibm, nullptr);
        g_ibManager.ReleaseDefaultPool();
        break;
    case 10:
        g_lastOp = "IB UnlockAll";
        OrigFn<ThisFn>(0x100587f0)(ibm, nullptr);
        g_ibManager.UnlockAll();
        break;
    case 11:
    case 12:
    case 13:
    case 14: {
        if (g_vbAlloc[1][k].buffer)
            break;
        g_lastOp = "VB Alloc";
        static const uint32_t kStrides[] = {0x20, 0x18, 0x28, 0x10, 0x30, 0x1C};
        const uint32_t usage = RandUsage(), stride = kStrides[Rand(Rand(3) == 0 ? 6 : 2)];
        const uint32_t count = Rand(16) == 0 ? 5000 + Rand(9000) : Rand(8) == 0 ? 1 + Rand(3) : 1 + Rand(1500);
        const uint32_t format = static_cast<uint32_t>(Rand(4));
        const bool newBuffer = Rand(16) == 0;
        SetFailures();
        const int before = g_vbManager.buffers.Size();
        using AllocFn = bool(__fastcall *)(void *, void *, uint32_t, uint32_t, uint32_t, S_VBAlloc *, uint32_t, char);
        const bool ro = OrigFn<AllocFn>(0x10066bf0)(vbm, nullptr, usage, stride, count, &g_vbAlloc[0][k], format,
                                                    newBuffer ? 1 : 0);
        const bool rp = g_vbManager.Alloc(usage, stride, count, &g_vbAlloc[1][k], format, newBuffer);
        Check("VB Alloc", ro, rp);
        ++g_allocs;
        g_newBuffers += g_vbManager.buffers.Size() != before;
        break;
    }
    case 15:
    case 16:
    case 17: {
        if (!g_vbAlloc[1][k].buffer && Rand(4) != 0)
            break;
        g_lastOp = "VB free";
        OrigFn<void(__stdcall *)(S_VBAlloc *)>(0x10066e90)(&g_vbAlloc[0][k]);
        FreeVB(&g_vbAlloc[1][k]);
        g_vbAlloc[0][k] = S_VBAlloc{};
        g_vbAlloc[1][k] = S_VBAlloc{};
        break;
    }
    case 18:
        if (Rand(6) != 0)
            break;
        g_lastOp = "VB FreeUnused";
        OrigFn<void(__fastcall *)(void *, void *, void *)>(0x10066ef0)(vbm, nullptr, nullptr);
        g_vbManager.FreeUnused();
        break;
    case 19: {
        // One buffer (the n-th in both trees).
        const int n = g_vbManager.buffers.Size();
        if (!n)
            break;
        g_lastOp = "VB FreeUnused(buffer)";
        const int pick = Rand(n);
        std::vector<C_VertexBuffer *> o, p;
        InOrder(OrigVBM().buffers.Head()->parent, Orig<void *>(0x100d142c), o);
        InOrder(g_vbManager.buffers.Head()->parent, g_vbManager.buffers.Nil(), p);
        OrigFn<void(__fastcall *)(void *, void *, void *)>(0x10066ef0)(vbm, nullptr, o[pick]);
        g_vbManager.FreeUnused(p[pick]);
        break;
    }
    case 20:
        if (Rand(40) != 0)
            break;
        g_lastOp = "VB FreeAll";
        OrigFn<ThisFn>(0x10066fb0)(vbm, nullptr);
        g_vbManager.FreeAll();
        break;
    case 21:
        if (Rand(20) != 0)
            break;
        g_lastOp = "VB ReleaseDefaultPool";
        OrigFn<ThisFn>(0x10067020)(vbm, nullptr);
        g_vbManager.ReleaseDefaultPool();
        break;
    default:
        g_lastOp = "VB UnlockAll";
        OrigFn<ThisFn>(0x10066eb0)(vbm, nullptr);
        g_vbManager.UnlockAll();
        break;
    }
}

// The index buffers of both sides in tree order (debugging).
void DumpIB()
{
    std::vector<C_IndexBuffer *> o, p;
    InOrder(OrigIBM().buffers.Head()->parent, Orig<void *>(0x100c062c), o);
    InOrder(g_ibManager.buffers.Head()->parent, g_ibManager.buffers.Nil(), p);
    for (int side = 0; side < 2; ++side) {
        std::printf("%s:\n", side ? "port" : "original");
        for (C_IndexBuffer *b : side ? p : o) {
            std::printf("  IB %d usage %x cap %u avail %u free:", FakeIndex(b->ib, side ? g_ibs[1] : g_ibs[0]), b->usage,
                        b->capacity, b->available);
            std::vector<S_bufferBlock *> f;
            InOrder(b->freeBlocks.Head()->parent, side ? static_cast<const void *>(b->freeBlocks.Nil()) : Orig<void *>(0x100c0624), f);
            for (S_bufferBlock *x : f)
                std::printf(" %u+%u", x->offset, x->count);
            std::printf("\n");
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 200000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    if (!SetUpBuffers(origPath))
        return 2;
    g_logDisabled = true;
    g_rng.seed(seed);
    g_ownerIndex = &AllocIndex;
    i3d::g_driver = reinterpret_cast<I3D_driver *>(&g_fakeDriver);

    CompareAll();
    const long dumpOp = std::getenv("CB_DUMP_OP") ? std::atol(std::getenv("CB_DUMP_OP")) : -1;
    for (g_op = 0; g_op < ops; ++g_op) {
        if (g_op == dumpOp)
            DumpIB();
        RunOp();
        CompareAll();
        g_mostBuffers = std::max(g_mostBuffers, std::max(g_ibManager.buffers.Size(), g_vbManager.buffers.Size()));
    }
    std::printf("coverage: allocations %ld (new buffers %ld); buffers now IB %d, VB %d; most at once %d\n", g_allocs,
                g_newBuffers, g_ibManager.buffers.Size(), g_vbManager.buffers.Size(), g_mostBuffers);
    std::printf("compare_buffers: %ld operations, seed %u: identical\n", ops, seed);
    // (Both managers keep their buffers: the fake devices outlive them.)
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
