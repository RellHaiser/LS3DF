// Differential test of the S3TC encoder (src/igraph/s3tc.cpp): the original's S3TCencode
// (0x10080ed0) and the port compress the same random images (16 / 32-bit pixel formats, 8-bit
// palettes, every encode type, odd sizes, few-colour and single-colour blocks); the output blocks
// and the destination descriptions must be identical. The FPU runs in single precision as in the
// game (Direct3D's default).
//
// Usage: compare_s3tc.exe [original LS3DF.dll] [images] [seed]
//
// test-sources: src/igraph/s3tc.cpp
#include <windows.h>

#include <float.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "../src/igraph/s3tc.h"
#include "test_paths.h"

using namespace ls3d::s3tc;

namespace {

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }
using EncodeFn = void(__cdecl *)(const S_surfaceDesc *, const uint8_t *, S_surfaceDesc *, void *, uint32_t,
                                 const float *);

std::mt19937 g_rng(1);
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<unsigned>(n)); }

struct S_format {
    uint32_t bits;
    uint32_t mask[4];
};
const S_format kFormats[] = {
    {16, {0x7c00, 0x3e0, 0x1f, 0x8000}},        // A1R5G5B5
    {16, {0xf00, 0xf0, 0xf, 0xf000}},           // A4R4G4B4
    {16, {0xf800, 0x7e0, 0x1f, 0}},             // R5G6B5 (no alpha: a zero mask)
    {32, {0xff0000, 0xff00, 0xff, 0xff000000}}, // A8R8G8B8
    {24, {0xff0000, 0xff00, 0xff, 0}},          // R8G8B8 (no alpha)
};

// A random pixel value: noise, a small palette of colours, a gradient or one colour.
uint32_t Pixel(int style, uint32_t x, uint32_t y, const uint32_t *few, uint32_t full)
{
    switch (style) {
    case 0: return g_rng() & full;
    case 1: return few[Rand(2)] & full;
    case 2: return few[Rand(4)] & full;
    case 3: return few[0] & full;
    case 4: return ((x * 0x01030507u + y * 0x0b0d1113u) ^ few[0]) & full;
    default: return (Rand(8) ? few[Rand(3)] : g_rng()) & full;
    }
}

// The work area's fields (offsets from 0x100d2cd8) for the state diff.
struct S_field {
    uint32_t va;
    const char *name;
};
const S_field kFields[] = {
    {0x100d2cd8, "alphaSk"}, {0x100d2ce0, "unique"}, {0x100d2e60, "invCount"}, {0x100d2e68, "alphaErrDen"},
    {0x100d2e70, "lo"}, {0x100d2e88, "hi"}, {0x100d2ea0, "kindErr"}, {0x100d2eb8, "alphaMean"},
    {0x100d2ec0, "quantTmp"}, {0x100d2ec8, "maxDist2"}, {0x100d2ed0, "cov2"}, {0x100d2f18, "alphaN"},
    {0x100d2f20, "alphaErr"}, {0x100d2f28, "alphaSetErr"}, {0x100d2f50, "ordSum"}, {0x100d30d0, "fitC"},
    {0x100d30e8, "xformEp"}, {0x100d3118, "bestQuantErr"}, {0x100d3120, "proj"}, {0x100d31a0, "scaled"},
    {0x100d3320, "alphaErrNum"}, {0x100d3328, "uniqueSum"}, {0x100d34a8, "bfitS"}, {0x100d34f0, "errTab"},
    {0x100d4cf0, "ordW2"}, {0x100d4d70, "alphaCentered"}, {0x100d4df0, "candErr"}, {0x100d4e00, "alphaSets"},
    {0x100d5080, "ordSum2"}, {0x100d5200, "invNorm"}, {0x100d5208, "mean"}, {0x100d5220, "single"},
    {0x100d6d20, "alphaSkc"}, {0x100d6d28, "passEp"}, {0x100d6d88, "centered"}, {0x100d6f08, "alphaInvN"},
    {0x100d6f10, "palette"}, {0x100d7090, "uniqueWeight"}, {0x100d7110, "quantErr"}, {0x100d7120, "alphaSq"},
    {0x100d7128, "trace"}, {0x100d7130, "alphaLo"}, {0x100d7138, "alphaHi"}, {0x100d7140, "bfitC"},
    {0x100d7158, "fitQ"}, {0x100d7170, "axis"}, {0x100d7188, "bfitQ"}, {0x100d71a0, "alphaThresh"},
    {0x100d71d8, "passErr"}, {0x100d71e8, "dist2"}, {0x100d71f0, "ordW"}, {0x100d7270, "weights"},
    {0x100d7288, "cov"}, {0x100d72d0, "fitS"}, {0x100d7318, "alphaPrevLo"}, {0x100d7320, "alphaPrevHi"},
    {0x100d7328, "fitBestNum"}, {0x100d7330, "bfitBestNum"}, {0x100d7338, "singleReady"}, {0x100d7340, "end"},
};

// The interpolated-alpha fit (0x10085a80) reads its caller's uninitialised index array on its
// first pass (only the first entry is cleared), so its state and, rarely, the alpha block depend
// on stack contents; the port starts from zeros. Its fields are left out of the state diff, and
// DXT5 alpha blocks are counted apart (LS3DF itself never encodes DXT5).
bool g_skipAlphaState;

// Prints the work-area fields that differ (up to `limit`); returns how many differ.
int CompareState(int limit)
{
    size_t size;
    const double *dens[2];
    const uint8_t *port = static_cast<const uint8_t *>(DebugWorkArea(&size, dens));
    const uint8_t *orig = &Orig<uint8_t>(0x100d2cd8);
    int diffs = 0;
    for (size_t f = 0; f + 1 < sizeof kFields / sizeof kFields[0]; ++f) {
        if (g_skipAlphaState && std::strncmp(kFields[f].name, "alpha", 5) == 0)
            continue;
        const size_t lo = kFields[f].va - 0x100d2cd8, hi = kFields[f + 1].va - 0x100d2cd8;
        for (size_t o = lo; o < hi; o += 8) {
            const size_t n = hi - o < 8 ? hi - o : 8;
            if (std::memcmp(port + o, orig + o, n) != 0) {
                if (diffs++ < limit) {
                    double a = 0, b = 0;
                    std::memcpy(&a, orig + o, n);
                    std::memcpy(&b, port + o, n);
                    std::printf("  state %s[+0x%zx]: %.9g / %.9g\n", kFields[f].name, o - lo, a, b);
                }
                break;
            }
        }
    }
    if (Orig<double>(0x100a99a0) != *dens[0] || Orig<double>(0x100a99a8) != *dens[1]) {
        if (diffs++ < limit)
            std::printf("  state bestDen: %.9g %.9g / %.9g %.9g\n", Orig<double>(0x100a99a0), Orig<double>(0x100a99a8),
                        *dens[0], *dens[1]);
    }
    return diffs;
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const int images = argc > 2 ? std::atoi(argv[2]) : 2000;
    if (argc > 3)
        g_rng.seed(static_cast<unsigned>(std::atoi(argv[3])));
    // "blocks": encode block by block and compare the encoders' work areas too.
    const bool perBlock = argc > 4 && std::strcmp(argv[4], "blocks") == 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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
    g_base = reinterpret_cast<uint8_t *>(dll);
    Orig<uint8_t>(0x100d16f2) = 1;              // the original's log off
    const EncodeFn origEncode = reinterpret_cast<EncodeFn>(g_base + (0x10080ed0 - 0x10000000));

    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    // "fitalpha": the alpha block's 1-D fit alone (0x10085a80) on random alpha sets.
    if (argc > 4 && std::strcmp(argv[4], "fitalpha") == 0) {
        using FitFn = double(__cdecl *)(int, int, const double *, int *, int *);
        const FitFn origFit = reinterpret_cast<FitFn>(g_base + (0x10085a80 - 0x10000000));
        unsigned cw0;
        _controlfp_s(&cw0, _PC_24, _MCW_PC);
        int bad = 0;
        for (int t = 0; t < images && bad < 5; ++t) {
            const int levels = Rand(2) ? 8 : 6;
            const int n = 1 + Rand(16);
            double v[16];
            for (int i = 0; i < n; ++i)
                v[i] = static_cast<float>(static_cast<double>(Rand(256)) / 255.0);
            if (t == 0) {
                v[0] = static_cast<float>(44.0 / 255.0);
                v[1] = static_cast<float>(196.0 / 255.0);
            }
            const int nn = t == 0 ? 2 : n;
            const int ll = t == 0 ? 6 : levels;
            int ea[2] = {}, eb[2] = {}, ia[16] = {}, ib[16] = {};
            std::printf("t %d\n", t);
            const double ra = origFit(ll, nn, v, ea, ia);
            std::printf("orig ok\n");
            const double rb = DebugFitAlpha(ll, nn, v, eb, ib);
            const int sd = CompareState(0);
            if (std::memcmp(&ra, &rb, 8) || std::memcmp(ea, eb, sizeof ea) || std::memcmp(ia, ib, sizeof ia) || sd) {
                ++bad;
                std::printf("fit %d levels %d n %d: err %.9g / %.9g ends %d %d / %d %d\n  values:", t, ll, nn, ra, rb,
                            ea[0], ea[1], eb[0], eb[1]);
                for (int i = 0; i < nn; ++i)
                    std::printf(" %.0f", v[i] * 255.0);
                std::printf("\n");
                CompareState(12);
            }
        }
        std::printf("fitalpha: %d sets, %d differ\n", images, bad);
        return bad ? 1 : 0;
    }

    static const uint32_t kTypes[] = {0x000, 0x001, 0x002, 0x100, 0x101, 0x102, 0x200, 0x201, 0x202};
    int failures = 0;
    int alphaDiffs = 0;
    long long blocks = 0;
    for (int img = 0; img < images && failures < 10; ++img) {
        const bool paletted = Rand(8) == 0;
        const S_format &f = kFormats[Rand(5)];
        const uint32_t w = 1 + static_cast<uint32_t>(Rand(Rand(4) ? 16 : 64));
        const uint32_t h = 1 + static_cast<uint32_t>(Rand(Rand(4) ? 16 : 64));
        const uint32_t bpp = paletted ? 1 : (f.bits + 7) / 8;
        const int pitch = static_cast<int>(w * bpp + static_cast<uint32_t>(Rand(3)) * 4);
        std::vector<uint8_t> pixels(static_cast<size_t>(pitch) * h + 8, 0);
        uint8_t palette[256 * 4];
        for (uint8_t &b : palette)
            b = static_cast<uint8_t>(g_rng());
        const uint32_t full = paletted ? 0xff : (f.bits == 32 ? 0xffffffffu : (1u << f.bits) - 1);
        const int style = Rand(6);
        uint32_t few[4];
        for (uint32_t &c : few)
            c = g_rng();
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                if (Rand(64) == 0) {
                    for (uint32_t &c : few)
                        c = g_rng();
                }
                uint32_t v = Pixel(style, x, y, few, full);
                if (!paletted && f.mask[3] && Rand(3) == 0)
                    v = (v & ~f.mask[3]) | (Rand(2) ? f.mask[3] : 0);       // fully opaque / clear
                std::memcpy(&pixels[static_cast<size_t>(y) * pitch + x * bpp], &v, bpp);
            }
        }

        S_surfaceDesc src = {};
        src.size = 0x6c;
        src.flags = 0x180e;
        src.width = w;
        src.height = h;
        src.pitch = pitch;
        src.surface = pixels.data();
        src.pfSize = 0x20;
        src.pfFlags = 0x41;
        src.bitCount = paletted ? 8 : f.bits;
        for (int c = 0; c < 4; ++c)
            src.mask[c] = paletted ? (c < 3 ? 0xffu << (8 * c) : 0xff000000u) : f.mask[c];
        if (!src.mask[3])
            src.mask[3] = paletted ? 0 : f.mask[0];         // (a zero mask hangs the original)
        src.keyLow = few[1] & 0x00ffffff & full;
        src.keyHigh = src.keyLow + static_cast<uint32_t>(Rand(4) == 0 ? 0 : Rand(0x10000));
        uint32_t type = kTypes[Rand(9)];
        if (paletted)
            type &= 0x001;                                   // palettes: opaque / colour key only
        const uint32_t threshold = static_cast<uint32_t>(Rand(256));
        Orig<uint32_t>(0x100d2cd0) = threshold;
        g_alphaThreshold = threshold;
        float weights[3];
        for (float &x : weights)
            x = Rand(10) == 0 ? 0.0f : static_cast<float>(Rand(1000)) / 1000.0f;
        if (Rand(4) == 0) {
            weights[0] = 0.3086f;
            weights[1] = 0.6094f;
            weights[2] = 0.082f;
        }

        const size_t blockBytes = (type & 0xff00) ? 16 : 8;
        if (perBlock) {
            // Each 4 x 4 block on its own (the encoder's state carries over the same way), with
            // the work areas compared after every block.
            for (uint32_t by = 0; by < h && failures < 10; by += 4) {
                for (uint32_t bx = 0; bx < w && failures < 10; bx += 4) {
                    S_surfaceDesc sub = src;
                    sub.width = w - bx < 4 ? w - bx : 4;
                    sub.height = h - by < 4 ? h - by : 4;
                    sub.surface = pixels.data() + static_cast<size_t>(by) * pitch + bx * bpp;
                    uint8_t a[16], b[16];
                    S_surfaceDesc da = {}, db = {};
                    origEncode(&sub, paletted ? palette : nullptr, &da, a, type, weights);
                    Encode(&sub, paletted ? palette : nullptr, &db, b, type, weights);
                    ++blocks;
                    const bool dxt5 = (type & 0xff00) == S3TC_ALPHA_INTERPOLATED;
                    g_skipAlphaState = true;
                    if (dxt5 && std::memcmp(a, b, 8) != 0)
                        ++alphaDiffs;
                    const bool same = dxt5 ? std::memcmp(a + 8, b + 8, 8) == 0 : std::memcmp(a, b, blockBytes) == 0;
                    const int stateDiffs = CompareState(0);
                    if (!same || stateDiffs) {
                        ++failures;
                        std::printf("image %d block (%u,%u) %ux%u bpp %u%s type %03X style %d weights %g %g %g: %s\n",
                                    img, bx, by, sub.width, sub.height, sub.bitCount, paletted ? " palette" : "", type,
                                    style, weights[0], weights[1], weights[2], same ? "same output" : "output differs");
                        for (size_t i = 0; i < blockBytes; ++i)
                            std::printf("%02X", a[i]);
                        std::printf(" / ");
                        for (size_t i = 0; i < blockBytes; ++i)
                            std::printf("%02X", b[i]);
                        std::printf("\n  pixels:");
                        for (uint32_t y = 0; y < sub.height; ++y) {
                            for (uint32_t x = 0; x < sub.width; ++x) {
                                uint32_t v = 0;
                                std::memcpy(&v, static_cast<uint8_t *>(sub.surface) + y * pitch + x * bpp, bpp);
                                std::printf(" %0*X", static_cast<int>(bpp * 2), v);
                            }
                            std::printf(" |");
                        }
                        std::printf("\n");
                        CompareState(12);
                        return 1;
                    }
                }
            }
            continue;
        }
        const size_t outSize = ((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
        std::vector<uint8_t> outA(outSize + 16, 0xcd), outB(outSize + 16, 0xcd);
        S_surfaceDesc dstA = {}, dstB = {};
        dstA.size = dstB.size = 0x6c;
        origEncode(&src, paletted ? palette : nullptr, &dstA, outA.data(), type, weights);
        Encode(&src, paletted ? palette : nullptr, &dstB, outB.data(), type, weights);
        blocks += static_cast<long long>(outSize / blockBytes);
        if (dstA.surface != outA.data() || dstB.surface != outB.data())
            std::printf("image %d: destination surface pointer\n", img);
        dstA.surface = dstB.surface = nullptr;      // (each side's own buffer)
        if (std::memcmp(&dstA, &dstB, sizeof dstA) != 0) {
            for (int k = 0; k < 0x6c / 4; ++k) {
                const uint32_t x = reinterpret_cast<const uint32_t *>(&dstA)[k];
                const uint32_t y = reinterpret_cast<const uint32_t *>(&dstB)[k];
                if (x != y)
                    std::printf("image %d: destination +0x%02X %08X / %08X\n", img, k * 4, x, y);
            }
            ++failures;
        }
        const bool dxt5 = (type & 0xff00) == S3TC_ALPHA_INTERPOLATED;
        for (size_t i = 0; i < outSize; ++i) {
            if (outA[i] == outB[i])
                continue;
            if (dxt5 && i % blockBytes < 8) {
                ++alphaDiffs;                       // (see g_skipAlphaState)
                i = (i / blockBytes) * blockBytes + 7;
                continue;
            }
            std::printf("image %d (%ux%u bpp %u%s type %03X style %d): block %zu byte %zu: %02X / %02X\n", img, w, h,
                        src.bitCount, paletted ? " palette" : "", type, style, i / blockBytes, i % blockBytes, outA[i],
                        outB[i]);
            ++failures;
            break;
        }
    }
    std::printf("s3tc: %d images, %lld blocks, %d differ (DXT5 alpha blocks apart: %d)\n", images, blocks, failures,
                alphaDiffs);
    return failures ? 1 : 0;
}
