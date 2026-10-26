// Differential test of ITexture::Create (BMP / TGA images: conversion, colour keys, opacity maps,
// dithering, scaling, mip maps, the 16-bit cache files): the original LS3DF.dll and the port
// (src/igraph) create textures on the same software Direct3D device (fake_d3d8.h); the levels and
// the texture objects' fields must be identical.
//
// Usage: compare_texture.exe [original LS3DF.dll] [rounds] [enhanced | mipmaps]
//
// The port's LS3DF.ini enhancements (src/core/enhance.h), off by default, are checked in two more
// modes. enhanced (ImprovedMipmaps + TrueColorTextures): the original creates image files' textures
// with ITC_TRUECOLOR; the objects and level 0 must match its, each further level of an A8R8G8B8
// texture must be tex::DownsampleMip of the level above. mipmaps (ImprovedMipmaps alone): the same
// for A8R8G8B8 textures; 16-bit levels made from a kept 32-bit image (ITC_KEEP_PIXELS) must be the
// DownsampleMip chain converted by WriteSurface, and the second pass's levels, loaded from the
// port's own cache file (named apart from the original's), those of the first pass.
//
// test-sources: src/igraph/*.cpp src/core/debug.cpp src/core/cpu.cpp src/core/enhance.cpp src/dx/dxerr8.cpp src/i3d/ib_manager.cpp src/i3d/vb_manager.cpp src/dshow/*.cpp
// test-sources: src/gfx/d3d8/gfx_d3d8.cpp
#include <windows.h>

#include <float.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/core/enhance.h"
#include "../src/gfx/d3d8/d3d8_device.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/i3d_links.h"
#include "../src/i3d/vb_manager.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/igraph_state.h"
#include "../src/igraph/ishow.h"
#include "../src/igraph/itexture.h"
#include "../src/igraph/mip_filter.h"
#include "fake_d3d8.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not ported yet

namespace ls3d {
namespace i3d {
I3D_driver *g_driver;
uint32_t g_renderStats[8];
void ReapplyDriverStates() {}
void DrawDebugText(int, int, const char *) {}
bool UseWBuffer() { return false; }
void InvalidateLights() {}
int InitParticleBuffers() { return 0; }
}  // namespace i3d
namespace snd {
void UpdateVoices() {}
void DrawStats() {}
}  // namespace snd
}  // namespace ls3d

class ISND_driver;
ISND_driver *g_sndDriver;

// ---------------------------------------------------------------- the original DLL

namespace {

uint8_t *g_base;
void *g_origIGraph;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }

using CreateITextureFn = void *(__stdcall *)(void *);
using CreateFn = HRESULT(__stdcall *)(void *, const char *, const char *, uint32_t, int, int, int);
using ReleaseFn = int(__stdcall *)(void *);

void *OrigCreateITexture()
{
    return (*reinterpret_cast<CreateITextureFn **>(g_origIGraph))[0x40 / 4](g_origIGraph);
}

struct S_device {
    D3DFORMAT fmt16, fmt1, fmtAlpha, fmtTrue;
    uint32_t caps;
    int maxW, maxH;
};

void Configure(FakeDevice *dev, const S_device &d)
{
    Orig<IDirect3DDevice8 *>(0x100d216c) = dev;
    Orig<D3DFORMAT>(0x100d1da8) = d.fmt16;
    Orig<D3DFORMAT>(0x100d1a74) = d.fmt1;
    Orig<D3DFORMAT>(0x100d1a78) = d.fmtAlpha;
    Orig<D3DFORMAT>(0x100d1774) = d.fmtTrue;
    Orig<D3DFORMAT>(0x100d1898) = D3DFMT_UNKNOWN;
    Orig<D3DFORMAT>(0x100d1edc) = D3DFMT_UNKNOWN;
    Orig<D3DFORMAT>(0x100d1a90) = D3DFMT_UNKNOWN;
    Orig<uint32_t>(0x100d1dac) = d.caps;
    Orig<int>(0x100d2128) = d.maxW;
    Orig<int>(0x100d1ce8) = d.maxH;
    using namespace ls3d::ig;
    g_pDevice = dev;
    g_fmt16Bit = d.fmt16;
    g_fmt1BitAlpha = d.fmt1;
    g_fmtAlpha = d.fmtAlpha;
    g_fmtTrueColor = d.fmtTrue;
    g_fmtDxt1 = g_fmtDxt3 = g_fmtDxt5 = D3DFMT_UNKNOWN;
    g_caps = d.caps;
    g_maxTextureWidth = d.maxW;
    g_maxTextureHeight = d.maxH;
}

// ---------------------------------------------------------------- test images

std::mt19937 g_rng(5);
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }

std::string g_dir;

void Put16(std::vector<uint8_t> &v, int x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void Put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (i * 8))); }

uint8_t Pixel(int pattern, int x, int y, int c)
{
    switch (pattern) {
    case 0: return uint8_t(g_rng());
    case 1: return uint8_t(x * 7 + y * 3 + c * 50);
    case 2: return uint8_t((x / 3 + y / 5 + c) & 1 ? 255 : 0);
    default: return uint8_t(c == 3 ? (x * 16) : (y * 9 + c * 30));
    }
}

std::string WriteBmp(const char *name, int w, int h, int bpp, int pattern, bool fullPalette)
{
    const int rowBytes = (bpp * w / 8 + 3) / 4 * 4;
    const int colors = bpp == 8 ? (fullPalette ? 0 : 256) : 0;
    std::vector<uint8_t> f;
    f.push_back('B');
    f.push_back('M');
    Put32(f, 0);
    Put32(f, 0);
    Put32(f, 0);
    Put32(f, 40);
    Put32(f, uint32_t(w));
    Put32(f, uint32_t(h));
    Put16(f, 1);
    Put16(f, bpp);
    Put32(f, 0);
    Put32(f, rowBytes * h);
    Put32(f, 0);
    Put32(f, 0);
    Put32(f, uint32_t(colors));
    Put32(f, 0);
    if (bpp == 8) {
        for (int i = 0; i < 256; ++i) {
            f.push_back(uint8_t(i * 3));
            f.push_back(uint8_t(255 - i));
            f.push_back(uint8_t(i * 7));
            f.push_back(0);
        }
    }
    for (int y = 0; y < h; ++y) {
        std::vector<uint8_t> row;
        for (int x = 0; x < w; ++x) {
            if (bpp == 8)
                row.push_back(Pixel(pattern, x, y, 0));
            else if (bpp == 16)
                Put16(row, Pixel(pattern, x, y, 0) | Pixel(pattern, x, y, 1) << 8);
            else
                for (int c = 0; c < bpp / 8; ++c)
                    row.push_back(Pixel(pattern, x, y, c));
        }
        row.resize(rowBytes, 0);
        f.insert(f.end(), row.begin(), row.end());
    }
    const std::string path = g_dir + name;
    FILE *out = std::fopen(path.c_str(), "wb");
    std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
    return path;
}

std::string WriteTga(const char *name, int w, int h, int bpp, int pattern, bool topDown)
{
    std::vector<uint8_t> f(18, 0);
    f[2] = 2;
    f[12] = uint8_t(w);
    f[13] = uint8_t(w >> 8);
    f[14] = uint8_t(h);
    f[15] = uint8_t(h >> 8);
    f[16] = uint8_t(bpp);
    f[17] = topDown ? 0x20 : 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < bpp / 8; ++c)
                f.push_back(Pixel(pattern, x, y, c));
    const std::string path = g_dir + name;
    FILE *out = std::fopen(path.c_str(), "wb");
    std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
    return path;
}

// A .dds file: uncompressed (bits/masks) or DXT1/3/5, random level data.
std::string WriteDds(const char *name, int w, int h, int type, bool mips)
{
    // type: 0 A8R8G8B8, 1 A1R5G5B5, 2 A4R4G4B4, 3 R5G6B5, 4 DXT1, 5 DXT3, 6 DXT5, 7 V8U8
    static const uint32_t kBits[] = {32, 16, 16, 16, 0, 0, 0, 16};
    static const uint32_t kMasks[][4] = {{0xff0000, 0xff00, 0xff, 0xff000000}, {0x7c00, 0x3e0, 0x1f, 0x8000},
                                         {0xf00, 0xf0, 0xf, 0xf000},           {0xf800, 0x7e0, 0x1f, 0},
                                         {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0xff, 0xff00, 0, 0}};
    int levels = 1;
    if (mips)
        for (int a = w, b = h; a > 1 || b > 1; a >>= 1, b >>= 1)
            ++levels;
    std::vector<uint8_t> f;
    Put32(f, 0x20534444);
    Put32(f, 124);
    Put32(f, 0x1007 | (mips ? 0x20000 : 0));
    Put32(f, uint32_t(h));
    Put32(f, uint32_t(w));
    Put32(f, 0);
    Put32(f, 0);
    Put32(f, uint32_t(levels));
    for (int i = 0; i < 11; ++i)
        Put32(f, 0);
    Put32(f, 32);
    if (type >= 4 && type <= 6) {
        Put32(f, 4);
        Put32(f, type == 4 ? D3DFMT_DXT1 : type == 5 ? D3DFMT_DXT3 : D3DFMT_DXT5);
        for (int i = 0; i < 5; ++i)
            Put32(f, 0);
    } else {
        Put32(f, type == 7 ? 0x80000 : (0x40 | (kMasks[type][3] ? 1 : 0)));
        Put32(f, 0);
        Put32(f, kBits[type]);
        for (int i = 0; i < 4; ++i)
            Put32(f, kMasks[type][i]);
    }
    for (int i = 0; i < 5; ++i)
        Put32(f, 0);
    for (int l = 0, a = w, b = h; l < levels; ++l) {
        size_t size;
        if (type >= 4 && type <= 6)
            size = size_t(a < 4 ? 1 : a / 4) * (b < 4 ? 1 : b / 4) * (type == 4 ? 8 : 16);
        else
            size = size_t(a) * b * kBits[type] / 8;
        for (size_t i = 0; i < size; ++i)
            f.push_back(uint8_t(g_rng()));
        a = a > 1 ? a >> 1 : 1;
        b = b > 1 ? b >> 1 : 1;
    }
    const std::string path = g_dir + name;
    FILE *out = std::fopen(path.c_str(), "wb");
    std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);
    return path;
}

// Deletes the cache files Create writes next to the images.
void DeleteCaches()
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *ext = std::strrchr(fd.cFileName, '.');
        if (ext && _stricmp(ext, ".bmp") && _stricmp(ext, ".tga") && _stricmp(ext, ".dds"))
            DeleteFileA((g_dir + fd.cFileName).c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// The names of the cache files in the test folder.
std::vector<std::string> CacheFiles()
{
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        const char *ext = std::strrchr(fd.cFileName, '.');
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ext && _stricmp(ext, ".bmp") &&
            _stricmp(ext, ".tga") && _stricmp(ext, ".dds"))
            out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

// The state of a created texture.
struct S_result {
    HRESULT hr;
    uint16_t width, height;
    uint32_t format, flags, requestedFlags, colorKey, memorySize;
    uint16_t levels;
    std::vector<uint8_t> pixels;
    std::vector<std::vector<uint8_t>> data;
};

S_result Capture(HRESULT hr, const uint8_t *object)
{
    S_result r = {};
    r.hr = hr;
    std::memcpy(&r.width, object + 0x20, 2);
    std::memcpy(&r.height, object + 0x22, 2);
    std::memcpy(&r.format, object + 0x24, 4);
    std::memcpy(&r.flags, object + 0x28, 4);
    std::memcpy(&r.requestedFlags, object + 0x2c, 4);
    std::memcpy(&r.colorKey, object + 0x30, 4);
    std::memcpy(&r.levels, object + 0x38, 2);
    std::memcpy(&r.memorySize, object + 0x3c, 4);
    const uint8_t *pixels;
    std::memcpy(&pixels, object + 0x34, 4);
    if (pixels && hr >= 0)
        r.pixels.assign(pixels, pixels + ((r.flags & ITC_PIXEL_MASK) ? (r.width * r.height + 7) / 8 : r.width * r.height * 4));
    FakeTexture *tex;
    std::memcpy(&tex, object + 8, 4);
    if (tex && hr >= 0)
        for (FakeSurface *s : tex->levels)
            r.data.push_back(s->bits);
    return r;
}

// The levels below level 0 that the port's ImprovedMipmaps makes of a 32-bit image: the
// DownsampleMip chain (a colour-keyed texture's uploaded with the alpha rescaled to the image's
// coverage at IGraph's alpha reference), in `format` as Create writes them (the texture's
// ITC_DITHER).
std::vector<std::vector<uint8_t>> ExpectedMips(const uint8_t *bgra, int width, int height, size_t levels,
                                               uint32_t flags, D3DFORMAT format, const ITexture *texture)
{
    std::vector<std::vector<uint8_t>> out;
    const int ref = (flags & ITC_ALPHA_1BIT) ? ls3d::ig::g_rs.alphaRef : 0;
    const double coverage = ref ? ls3d::tex::AlphaCoverage(bgra, width, height, ref) : 0.0;
    std::vector<uint8_t> cur(bgra, bgra + size_t(width) * height * 4), next;
    for (size_t l = 1; l < levels; ++l) {
        const int w = width > 1 ? width >> 1 : 1, h = height > 1 ? height >> 1 : 1;
        next.assign(size_t(w) * h * 4, 0);
        ls3d::tex::DownsampleMip(cur.data(), width, height, next.data());
        cur.swap(next);
        width = w;
        height = h;
        std::vector<uint8_t> level = cur;
        if (ref)
            ls3d::tex::ScaleAlphaToCoverage(level.data(), w, h, ref, coverage);
        if (format == D3DFMT_A8R8G8B8) {
            out.push_back(level);
        } else {
            FakeSurface s(UINT(w), UINT(h), format, D3DPOOL_MANAGED);
            ls3d::gfx::Surface *surface = ls3d::gfx::d3d8::Handle(static_cast<IDirect3DSurface8 *>(&s));
            ls3d::tex::WriteSurface(texture, level.data(), surface, UINT(w), UINT(h), 0);
            out.push_back(s.bits);
        }
    }
    return out;
}

std::string Describe(const S_result &r)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "hr %08x %dx%d fmt %d flags %08x key %x levels %d mem %u pixels %zu",
                  static_cast<unsigned>(r.hr), r.width, r.height, r.format, r.flags, r.colorKey, r.levels, r.memorySize,
                  r.pixels.size());
    return buf;
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 400;
    const std::string mode = argc > 3 ? argv[3] : "";
    const bool trueColorMode = mode == "enhanced", mipsMode = mode == "mipmaps";
    const bool enhanced = trueColorMode || mipsMode;
    if (!mode.empty() && !enhanced) {
        std::printf("unknown mode %s (enhanced, mipmaps)\n", mode.c_str());
        return 2;
    }
    {
        ls3d::S_enhancements e = {};
        e.mipmaps = enhanced;
        e.trueColorTextures = trueColorMode;
        ls3d::SetEnhancements(e);
    }
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
    g_origIGraph = reinterpret_cast<void *(__cdecl *)()>(GetProcAddress(dll, "GetIGraph"))();
    g_pIGraph = new IGraph;
    // The constructors' default material (the renderer's, for meshes without colors of their own).
    if (std::memcmp(&ls3d::ig::g_defaultMaterial, &Orig<D3DMATERIAL8>(0x100d1ba0), sizeof(D3DMATERIAL8))) {
        std::printf("IGraph's default material differs from the original's\n");
        return 1;
    }
    ls3d::g_logDisabled = true;
    Orig<uint8_t>(0x100d16f2) = 1;              // the original's log off too

    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    g_dir = std::string(tmp) + "ls3df_tex\\";
    CreateDirectoryA(g_dir.c_str(), nullptr);

    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);

    static const S_device kDevices[] = {
        {D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_A8R8G8B8, 0, 256, 256},
        {D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, 0, 2048, 2048},
        {D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_UNKNOWN, 0, 128, 64},
        {D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_A8R8G8B8, IGCAPS_DXT, 512, 512},
    };
    static const uint32_t kFlagBits[] = {ITC_SOURCE_ALPHA, ITC_ALPHA_1BIT, ITC_DITHER, ITC_MIPMAP, ITC_TRUECOLOR,
                                         ITC_KEEP_PIXELS, ITC_HALF, ITC_NORESIZE, ITC_NOCACHE, ITC_COMPRESS};
    long cases = 0, bad = 0, compressed = 0, checkedMips = 0, cachedMips = 0;
    for (int round = 0; round < rounds; ++round) {
        FakeDevice dev;
        const int device = Rand(4);
        Configure(&dev, kDevices[device]);
        if (device == 3) {                      // DXT formats: .dds files and ITC_COMPRESS (the S3TC encoder)
            Orig<D3DFORMAT>(0x100d1898) = ls3d::ig::g_fmtDxt1 = D3DFMT_DXT1;
            Orig<D3DFORMAT>(0x100d1edc) = ls3d::ig::g_fmtDxt3 = D3DFMT_DXT3;
            Orig<D3DFORMAT>(0x100d1a90) = ls3d::ig::g_fmtDxt5 = D3DFMT_DXT5;
            Orig<D3DFORMAT>(0x100d1d10) = ls3d::ig::g_fmtBump = D3DFMT_V8U8;
        }
        const int kind = Rand(device == 3 ? 9 : 7);
        const int w = 1 + Rand(kind < 4 ? 160 : 70), h = 1 + Rand(kind < 4 ? 160 : 70), pattern = Rand(4);
        std::string image;
        switch (kind) {
        case 0: image = WriteBmp("c.bmp", w, h, 8, pattern, Rand(2) != 0); break;
        case 1: image = WriteBmp("c.bmp", w, h, 16, pattern, false); break;
        case 2: image = WriteBmp("c.bmp", w, h, 24, pattern, false); break;
        case 3: image = WriteBmp("c.bmp", w, h, 32, pattern, false); break;
        case 4: image = WriteTga("c.tga", w, h, 16, pattern, Rand(2) != 0); break;
        case 5: image = WriteTga("c.tga", w, h, 24, pattern, Rand(2) != 0); break;
        case 6: image = WriteTga("c.tga", w, h, 32, pattern, Rand(2) != 0); break;
        default: {
            const int side = 1 << (2 + Rand(6)), side2 = 1 << (2 + Rand(6));
            image = WriteDds("c.dds", side, side2, Rand(8), Rand(2) != 0);
            break;
        }
        }
        std::string alpha;
        uint32_t flags = 0;
        for (uint32_t bit : kFlagBits)
            if (Rand(4) == 0)
                flags |= bit;
        if (kind >= 7 && Rand(2))
            flags |= ITC_KEEP_PIXELS;           // exercises the DXT decoders
        if (Rand(4) == 0) {
            alpha = Rand(2) ? WriteBmp("a.bmp", 1 + Rand(100), 1 + Rand(100), 8, Rand(4), false)
                            : WriteBmp("a.bmp", 1 + Rand(100), 1 + Rand(100), 24, Rand(4), false);
            flags |= ITC_USE_ALPHA;
        }
        int wParam = Rand(6) == 0 ? 1 + Rand(100) : 0, hParam = Rand(6) == 0 ? 1 + Rand(100) : 0;
        // Mip maps of a texture with an odd width read bytes the original's filter never writes
        // (its rows are half the source row apart): only even power-of-2 sizes then. (The
        // enhanced modes compare no level of the original's below level 0.)
        if (!enhanced && (flags & ITC_NORESIZE) && (flags & ITC_MIPMAP)) {
            const int size = 1 << (1 + Rand(6));
            wParam = size;
            hParam = 1 << (1 + Rand(6));
        }
        const int levels = Rand(3) == 0 ? Rand(4) : 0;
        // TrueColorTextures: what the original makes when asked for ITC_TRUECOLOR (image files).
        const uint32_t origFlags = flags | (trueColorMode && kind < 7 ? ITC_TRUECOLOR : 0);
        S_result portFirst;
        // Pass 0 converts the image (no cache files) and writes the cache; pass 1 loads both
        // textures from the cache file the port wrote.
        for (int pass = 0; pass < 2; ++pass) {
            if (!pass)
                DeleteCaches();
            void *o = OrigCreateITexture();
            const HRESULT ho = (*reinterpret_cast<CreateFn **>(o))[2](o, image.c_str(), alpha.empty() ? nullptr : alpha.c_str(),
                                                                    origFlags, wParam, hParam, levels);
            const S_result a = Capture(ho, static_cast<uint8_t *>(o));
            if (!pass)
                DeleteCaches();
            ITexture *p = g_pIGraph->CreateITexture();
            const HRESULT hp = p->Create(image.c_str(), alpha.empty() ? nullptr : alpha.c_str(), flags, wParam, hParam, levels);
            const S_result b = Capture(hp, reinterpret_cast<uint8_t *>(p));
            ++cases;
            // The port's cache file of improved mip levels is named apart from the original's (a
            // 'G' before the extension), and only that one.
            bool cacheNamed = true;
            if (!pass) {
                for (const std::string &file : CacheFiles()) {
                    const size_t dot = file.rfind('.');
                    const bool tagged = dot && file[dot - 1] == 'G';
                    cacheNamed &= tagged == (enhanced && b.levels > 1);
                }
            }
            if (kind < 7 && (a.format == D3DFMT_DXT1 || a.format == D3DFMT_DXT3))
                ++compressed;                   // through the S3TC encoder
            // Levels after one whose width or height is 1 are not computed by the original (it
            // uploads whatever its buffer held): compared up to there.
            S_result a2 = a, b2 = b;
            {
                size_t valid = 0;
                for (int lw = a.width, lh = a.height; valid < a.data.size(); ++valid) {
                    if (valid && (lw <= 1 || lh <= 1))
                        break;
                    if (valid) {
                        lw = lw > 1 ? lw >> 1 : 1;
                        lh = lh > 1 ? lh >> 1 : 1;
                    }
                }
                // (valid counts levels whose source level had both sides > 1, plus level 0)
                if (a2.data.size() > valid)
                    a2.data.resize(valid);
                if (b2.data.size() > valid)
                    b2.data.resize(valid);
            }
            // A .dds level 0 copy in a format the original does not decode stays uninitialised.
            if (kind >= 7 && a.format != D3DFMT_DXT1 && a.format != D3DFMT_DXT3 && a.format != D3DFMT_DXT5 &&
                a.format != D3DFMT_A8R8G8B8 && a.format != D3DFMT_A1R5G5B5 && a.format != D3DFMT_A4R4G4B4) {
                a2.pixels.clear();
                b2.pixels.clear();
            } else {
                a2.pixels = a.pixels;
                b2.pixels = b.pixels;
            }
            // (Enhanced modes, second pass: a mip-mapped 16-bit / DXT texture of the port's comes from
            // its own cache file while the original converts the image again. A cache load leaves
            // the colour key 0, as the original's does, and the DXT3 cache of an opacity-mapped
            // image, found only after the image's conversion, keeps the converted image.)
            const bool ownCache = enhanced && pass == 1 && kind < 7 && b.levels > 1 && b.format != D3DFMT_A8R8G8B8;
            bool same = a.hr == b.hr && a.width == b.width && a.height == b.height && a.format == b.format &&
                        a.flags == b.flags && (a.colorKey == b.colorKey || (ownCache && b.colorKey == 0)) &&
                        a.levels == b.levels && a.memorySize == b.memorySize &&
                        (a2.pixels == b2.pixels || (ownCache && !(b.flags & ITC_KEEP_PIXELS)));
            if (!enhanced || kind >= 7) {
                same = same && a2.data == b2.data;
            } else if (same) {
                // Level 0 as the original's; the levels below by the port's filter.
                same = a.data.size() == b.data.size() && (b.data.empty() || a.data[0] == b.data[0]);
                if (same && b.data.size() > 1) {
                    std::vector<std::vector<uint8_t>> expected;
                    const bool kept = !(b.flags & ITC_PIXEL_MASK) && b.pixels.size() == size_t(b.width) * b.height * 4;
                    if (b.format == D3DFMT_A8R8G8B8)
                        expected = ExpectedMips(b.data[0].data(), b.width, b.height, b.data.size(), b.flags,
                                                D3DFMT_A8R8G8B8, p);
                    else if (kept && !FakeIsDxt(D3DFORMAT(b.format)))
                        expected = ExpectedMips(b.pixels.data(), b.width, b.height, b.data.size(), b.flags,
                                                D3DFORMAT(b.format), p);
                    if (!expected.empty()) {
                        ++checkedMips;
                        same = std::equal(expected.begin(), expected.end(), b.data.begin() + 1);
                    }
                    if (pass == 1) {
                        cachedMips += b.format != D3DFMT_A8R8G8B8;     // (32-bit textures have no cache)
                        same = same && b.data == portFirst.data;
                    }
                }
            }
            if (!cacheNamed) {
                same = false;
                std::printf("case %d.%d: cache file name (levels %d):", round, pass, b.levels);
                for (const std::string &file : CacheFiles())
                    std::printf(" %s", file.c_str());
                std::printf("\n");
            }
            if (!pass)
                portFirst = b;
            if (!same) {
                ++bad;
                if (bad <= 12) {
                    std::printf("case %d.%d: kind %d %dx%d pattern %d flags %08x alpha %d params %d,%d,%d\n", round, pass,
                                kind, w, h, pattern, flags, !alpha.empty(), wParam, hParam, levels);
                    std::printf("   orig %s\n   port %s\n", Describe(a).c_str(), Describe(b).c_str());
                    for (size_t l = 0; l < a.data.size() && l < b.data.size(); ++l) {
                        if (a.data[l] != b.data[l]) {
                            size_t i = 0;
                            while (a.data[l][i] == b.data[l][i])
                                ++i;
                            std::printf("   level %zu differs at byte %zu (of %zu): %02x vs %02x\n", l, i, a.data[l].size(),
                                        a.data[l][i], b.data[l][i]);
                            break;
                        }
                    }
                }
            }
            (*reinterpret_cast<ReleaseFn **>(o))[0](o);
            p->Release();
        }
    }
    DeleteCaches();
    if (enhanced)
        std::printf("%s: %ld mip chains checked against DownsampleMip, %ld against the port's cache\n", mode.c_str(),
                    checkedMips, cachedMips);
    std::printf("texture: %ld cases (%ld compressed), %ld differ\n", cases, compressed, bad);
    return bad ? 1 : 0;
}
