// Differential test of DXGetErrorString8A (src/dx/dxerr8.cpp) against the original's statically
// linked copy at 0x1007d466 (called through its RVA; it is not exported).
//
// Usage: compare_dxerr8.exe [original LS3DF.dll]
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "../src/dx/dxerr8.cpp"
#include "test_paths.h"

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
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
    using Fn = const char *(__stdcall *)(HRESULT);
    const Fn original = reinterpret_cast<Fn>(reinterpret_cast<uint8_t *>(dll) + 0x7d466);

    long checked = 0, bad = 0;
    auto check = [&](uint32_t code) {
        ++checked;
        const char *a = original(static_cast<HRESULT>(code));
        const char *b = DXGetErrorString8A(static_cast<HRESULT>(code));
        if (std::strcmp(a, b)) {
            if (++bad < 20)
                std::printf("0x%08X: original \"%s\", port \"%s\"\n", code, a, b);
        }
    };
    // Every code of the table, its neighbours, and random values.
    for (const S_dxError &e : kErrors) {
        check(e.code - 1);
        check(e.code);
        check(e.code + 1);
    }
    std::mt19937 rng(1);
    for (int i = 0; i < 2000000; ++i)
        check(rng());
    for (uint32_t c = 0x88760000; c < 0x88770000; ++c)  // D3D facility
        check(c);
    std::printf("dxerr8: %ld codes, %ld mismatches\n", checked, bad);
    return bad ? 1 : 0;
}
