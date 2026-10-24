// Differential test of IGraph::ResizeImage (src/igraph/resize_image.cpp) against the original's
// slot 0x134, called on the original DLL's IGraph object (the resampler needs no device).
//
// Usage: compare_resize.exe [original LS3DF.dll] [rounds]
#include <windows.h>

#include <float.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../src/igraph/resize_image.cpp"
#include "test_paths.h"

extern "C" __declspec(dllexport) int dbgPrintf(const char *, ...)
{
    return 0;
}

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 3000;
    char dir[MAX_PATH];
    std::strcpy(dir, orig);
    if (char *slash = std::strrchr(dir, '\\'))
        *slash = 0;
    SetDllDirectoryA(dir);
    HMODULE dll = LoadLibraryA(orig);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", orig, GetLastError());
        return 2;
    }
    using GetIGraphFn = void *(__cdecl *)();
    void *igraph = reinterpret_cast<GetIGraphFn>(GetProcAddress(dll, "GetIGraph"))();
    if (!igraph) {
        std::printf("GetIGraph() returned 0\n");
        return 2;
    }
    using ResizeFn = bool(__stdcall *)(void *, int, int, int, const uint8_t *, int, int, int, uint8_t **, int, int);
    const ResizeFn original = (*reinterpret_cast<ResizeFn **>(igraph))[0x134 / 4];

    // The game runs with the FPU in single precision (Direct3D's default).
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);

    std::mt19937 rng(7);
    long images = 0, bad = 0;
    for (int round = 0; round < rounds; ++round) {
        const int bpp = (rng() & 1) ? 4 : 3;
        const int sw = 1 + rng() % 200, sh = 1 + rng() % 200;
        int dw, dh;
        if (rng() % 4) {            // mostly shrinking (the box filter)
            dw = 1 + rng() % sw;
            dh = 1 + rng() % sh;
        } else {
            dw = 1 + rng() % 300;
            dh = 1 + rng() % 300;
        }
        const int srcPitch = sw * bpp + (rng() % 3) * 4;
        const int dstPitch = dw * bpp + (rng() % 3) * 4;
        const bool filter = (rng() % 5) != 0;
        std::vector<uint8_t> src(static_cast<size_t>(srcPitch) * sh);
        const int pattern = rng() % 3;
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = pattern == 0 ? static_cast<uint8_t>(rng()) : pattern == 1 ? static_cast<uint8_t>(i * 7 / 5)
                                                                              : static_cast<uint8_t>((rng() & 1) * 255);
        std::vector<uint8_t> a(static_cast<size_t>(dstPitch) * dh, 0xcd), b = a;
        uint8_t *pa = a.data(), *pb = b.data();
        const bool ra = original(igraph, sw, sh, srcPitch, src.data(), dw, dh, dstPitch, &pa, bpp, filter);
        const bool rb = ls3d::ig::ResizeImage(sw, sh, srcPitch, src.data(), dw, dh, dstPitch, &pb, bpp, filter);
        ++images;
        if (ra != rb || a != b) {
            size_t first = 0;
            while (first < a.size() && a[first] == b[first])
                ++first;
            if (++bad < 15)
                std::printf("%dx%d -> %dx%d bpp %d filter %d: byte %zu orig %d port %d\n", sw, sh, dw, dh, bpp,
                            filter, first, first < a.size() ? a[first] : -1, first < b.size() ? b[first] : -1);
        }
    }
    std::printf("resize: %ld images, %ld differ\n", images, bad);
    return bad ? 1 : 0;
}
