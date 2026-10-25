// Unit test of the port's LS3DF.ini enhancements that need no device: the INI reader
// (src/core/enhance.cpp) and the improved mip filter (src/igraph/mip_filter.cpp): no darkening
// across a mip chain, gamma-correct averages, no colour from transparent texels, alpha-tested
// coverage kept, every size down to 1 x 1 without writing past the level. The original's filter
// (Downsample in itexture.cpp, copied below) is the baseline the numbers are printed against.
// (compare_texture's enhanced / mipmaps modes check the filter inside ITexture::Create.)
//
// Usage: test_enhance.exe   (writes and deletes LS3DF.ini next to itself)
//
// test-sources: src/igraph/mip_filter.cpp src/core/enhance.cpp src/core/debug.cpp
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/core/enhance.h"
#include "../src/igraph/mip_filter.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAILED: %s\n", what);
    }
}

// The original's mip filter (itexture.cpp Downsample, inside ITexture::Create): weights 1/8 3/8
// 3/8 1/8, every term truncated; destination rows half the source row bytes apart.
void OriginalDownsample(const uint8_t *src, int width, int height, int rowBytes, uint8_t *dst)
{
    if (height - 1 <= 0)
        return;
    const int rows = ((height - 2) >> 1) + 1;
    const int cols = width - 1 > 0 ? ((width - 2) >> 1) + 1 : 0;
    const int dstRowBytes = rowBytes >> 1;
    for (int y = 0; y < rows; ++y) {
        const uint8_t *r0 = src + y * 2 * rowBytes;
        const uint8_t *r1 = r0 + rowBytes;
        uint8_t *d = dst + y * dstRowBytes;
        for (int x = 0; x < cols; ++x) {
            const uint8_t *p00 = r0 + x * 8, *p10 = p00 + 4;
            const uint8_t *p01 = r1 + x * 8, *p11 = p01 + 4;
            for (int c = 0; c < 4; ++c)
                d[x * 4 + c] = static_cast<uint8_t>((3 * p10[c] >> 3) + (3 * p01[c] >> 3) + (p11[c] >> 3) +
                                                    (p00[c] >> 3));
        }
    }
}

using Image = std::vector<uint8_t>;

struct S_level {
    int width, height;
    Image bgra;
};

// The mip chain of a square power-of-2 image: the port's filter (alphaRef > 0: the levels' alpha
// rescaled to level 0's coverage at that reference, as uploaded; each level made from the
// unscaled one above, as ITexture::Create does) or the original's.
std::vector<S_level> Chain(const Image &image, int size, bool original, int alphaRef)
{
    std::vector<S_level> out{{size, size, image}};
    const double coverage = alphaRef ? ls3d::tex::AlphaCoverage(image.data(), size, size, alphaRef) : 0.0;
    S_level unscaled = out.back();
    while (unscaled.width > 1) {
        S_level next{unscaled.width / 2, unscaled.height / 2, Image(size_t(unscaled.width / 2) * (unscaled.height / 2) * 4)};
        if (original)
            OriginalDownsample(unscaled.bgra.data(), unscaled.width, unscaled.height, unscaled.width * 4, next.bgra.data());
        else
            ls3d::tex::DownsampleMip(unscaled.bgra.data(), unscaled.width, unscaled.height, next.bgra.data());
        unscaled = next;
        if (alphaRef)
            ls3d::tex::ScaleAlphaToCoverage(next.bgra.data(), next.width, next.height, alphaRef, coverage);
        out.push_back(std::move(next));
    }
    return out;
}

double SrgbToLinear(int v)
{
    const double s = v / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

// Mean linear light of an image's colour channels.
double MeanLinear(const S_level &l)
{
    double sum = 0.0;
    for (size_t i = 0; i < l.bgra.size(); i += 4)
        sum += SrgbToLinear(l.bgra[i]) + SrgbToLinear(l.bgra[i + 1]) + SrgbToLinear(l.bgra[i + 2]);
    return sum / (l.bgra.size() / 4 * 3);
}

void TestIni()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string ini = exe;
    ini = ini.substr(0, ini.rfind('\\') + 1) + "LS3DF.ini";
    if (GetFileAttributesA(ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::printf("FAILED: %s exists already (not overwritten)\n", ini.c_str());
        ++g_failures;
        return;
    }
    {
        FILE *f = std::fopen(ini.c_str(), "wt");
        std::fprintf(f, "[Other]\nImprovedMipmaps=0\n[Graphics]\nAnisotropicFiltering=32\nTrilinearFiltering=1\n"
                        "ImprovedMipmaps=1\nTrueColorLightmaps=5\n");
        std::fclose(f);
    }
    const ls3d::S_enhancements &e = ls3d::Enhancements();
    DeleteFileA(ini.c_str());
    std::printf("ini: anisotropy %d, trilinear %d, mipmaps %d, true colour textures %d, lightmaps %d\n", e.anisotropy,
                e.trilinear, e.mipmaps, e.trueColorTextures, e.trueColorLightmaps);
    Check(e.anisotropy == 16, "AnisotropicFiltering=32 capped at 16");
    Check(e.trilinear && e.mipmaps && e.trueColorLightmaps, "keys set");
    Check(!e.trueColorTextures, "missing key off");
    ls3d::S_enhancements off = {};
    off.anisotropy = 1;
    ls3d::SetEnhancements(off);
    Check(ls3d::Enhancements().anisotropy == 1 && !ls3d::Enhancements().mipmaps, "SetEnhancements");
}

void TestUniform(std::mt19937 &rng)
{
    // A single colour stays itself at every size and alpha (also fully transparent ones).
    int bad = 0, cases = 0;
    const int sizes[][2] = {{2, 2}, {4, 4}, {1, 8}, {8, 1}, {3, 5}, {7, 7}};
    for (int n = 0; n < 2000; ++n) {
        const uint8_t b = n < 256 ? uint8_t(n) : uint8_t(rng()), g = n < 256 ? uint8_t(n) : uint8_t(rng());
        const uint8_t r = n < 256 ? uint8_t(n) : uint8_t(rng());
        const uint8_t a = n < 256 ? 255 : uint8_t(rng());
        for (const auto &s : sizes) {
            Image src(size_t(s[0]) * s[1] * 4);
            for (size_t i = 0; i < src.size(); i += 4) {
                src[i] = b;
                src[i + 1] = g;
                src[i + 2] = r;
                src[i + 3] = a;
            }
            const int w = s[0] > 1 ? s[0] / 2 : 1, h = s[1] > 1 ? s[1] / 2 : 1;
            Image dst(size_t(w) * h * 4);
            ls3d::tex::DownsampleMip(src.data(), s[0], s[1], dst.data());
            for (size_t i = 0; i < dst.size(); i += 4)
                bad += dst[i] != b || dst[i + 1] != g || dst[i + 2] != r || dst[i + 3] != a;
            ++cases;
        }
    }
    std::printf("uniform colours: %d cases, %d texels changed\n", cases, bad);
    Check(bad == 0, "uniform colours unchanged");
}

void TestGammaAndKey()
{
    // Black and white 2 x 2 checkers in both arrangements: half the light is sRGB 188.
    const uint8_t checker[2][4] = {{0, 255, 255, 0}, {255, 0, 0, 255}};
    for (const auto &c : checker) {
        Image src(16);
        for (int i = 0; i < 4; ++i) {
            src[i * 4] = src[i * 4 + 1] = src[i * 4 + 2] = c[i];
            src[i * 4 + 3] = 255;
        }
        uint8_t port[4], orig[4];
        ls3d::tex::DownsampleMip(src.data(), 2, 2, port);
        OriginalDownsample(src.data(), 2, 2, 8, orig);
        std::printf("checker %d %d / %d %d: port %d, original %d\n", c[0], c[1], c[2], c[3], port[0], orig[0]);
        Check(port[0] == 188 && port[1] == 188 && port[2] == 188 && port[3] == 255, "checker averages to sRGB 188");
    }
    // A colour-key edge: one opaque red texel, three transparent black ones.
    {
        const uint8_t src[16] = {0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t port[4], orig[4];
        ls3d::tex::DownsampleMip(src, 2, 2, port);
        OriginalDownsample(src, 2, 2, 8, orig);
        std::printf("key edge (red + 3 transparent black): port rgb %d %d %d a %d, original rgb %d %d %d a %d\n", port[2],
                    port[1], port[0], port[3], orig[2], orig[1], orig[0], orig[3]);
        Check(port[2] == 255 && port[1] == 0 && port[0] == 0 && port[3] == 64, "transparent texels add no colour");
    }
}

void TestBrightness(std::mt19937 &rng)
{
    // Random opaque noise: the mean light of each level against level 0's.
    const int size = 256;
    Image image(size_t(size) * size * 4);
    for (size_t i = 0; i < image.size(); ++i)
        image[i] = (i & 3) == 3 ? 255 : uint8_t(rng());
    const auto port = Chain(image, size, false, 0), orig = Chain(image, size, true, 0);
    const double base = MeanLinear(port[0]);
    // An 8-bit level can hold the mean only to half a code: the light between the codes about it.
    int code = 0;
    while (code < 255 && SrgbToLinear(code + 1) <= base)
        ++code;
    const double halfCode = (SrgbToLinear(code + 1) - SrgbToLinear(code)) * 0.5 / base;
    bool biased = false, offCode = false;
    std::printf("mean light of the levels against level 0 (noise %dx%d; half an 8-bit step: %.2f%%):\n"
                "  level  size     port      original\n", size, size, halfCode * 100.0);
    for (size_t l = 1; l < port.size(); ++l) {
        const double p = MeanLinear(port[l]) / base - 1.0, o = MeanLinear(orig[l]) / base - 1.0;
        std::printf("  %5zu  %3dx%-3d  %+7.2f%%  %+7.2f%%\n", l, port[l].width, port[l].height, p * 100.0, o * 100.0);
        biased |= port[l].width >= 32 && std::fabs(p) > 0.001;
        offCode |= std::fabs(p) > halfCode;
    }
    Check(!biased, "levels down to 32 x 32 within 0.1% of level 0's light");
    Check(!offCode, "every level within half an 8-bit step of level 0's light");
}

void TestCoverage(std::mt19937 &rng)
{
    // Binary alpha (a colour key's) of a few shapes; the coverage of each level at the reference
    // against level 0's: kept (the port's), plain box (the port's filter without it), original.
    const int size = 128;
    struct S_shape {
        const char *name;
        int kind;
        double density;
    };
    const S_shape shapes[] = {{"noise 10%", 0, 0.1}, {"noise 30%", 0, 0.3}, {"noise 70%", 0, 0.7},
                              {"blobs", 1, 0.0},     {"disc", 2, 0.0},      {"grass blades", 3, 0.0}};
    for (const int ref : {0x20, 0x80}) {
        std::printf("coverage at alpha reference 0x%02x, worst level error (levels down to 4 x 4):\n", ref);
        for (const S_shape &s : shapes) {
            Image image(size_t(size) * size * 4);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    bool on;
                    switch (s.kind) {
                    case 0: on = std::uniform_real_distribution<double>(0, 1)(rng) < s.density; break;
                    case 1: on = std::sin(x * 0.21) * std::cos(y * 0.17) + std::sin((x + y) * 0.07) > 0.6; break;
                    case 2: on = (x - 64) * (x - 64) + (y - 64) * (y - 64) < 40 * 40; break;
                    default: on = x % 6 == 0 && y > (x * 7) % 40; break;
                    }
                    uint8_t *p = &image[(size_t(y) * size + x) * 4];
                    p[0] = uint8_t(x * 2);
                    p[1] = 180;
                    p[2] = uint8_t(y * 2);
                    p[3] = on ? 255 : 0;
                    if (!on)
                        p[0] = p[1] = p[2] = 0;         // the key colour
                }
            }
            const double c0 = ls3d::tex::AlphaCoverage(image.data(), size, size, ref);
            const auto kept = Chain(image, size, false, ref), plain = Chain(image, size, false, 0),
                       orig = Chain(image, size, true, 0);
            double worst[3] = {}, total[2] = {};
            for (size_t l = 1; l < kept.size() && kept[l].width >= 4; ++l) {
                const auto err = [&](const S_level &v) {
                    return std::fabs(ls3d::tex::AlphaCoverage(v.bgra.data(), v.width, v.height, ref) - c0);
                };
                const double e[3] = {err(kept[l]), err(plain[l]), err(orig[l])};
                for (int i = 0; i < 3; ++i)
                    worst[i] = std::fmax(worst[i], e[i]);
                total[0] += e[0];
                total[1] += e[1];
            }
            std::printf("  %-13s level 0 %5.1f%%: kept %5.1f%%, plain %5.1f%%, original %5.1f%%\n", s.name, c0 * 100.0,
                        worst[0] * 100.0, worst[1] * 100.0, worst[2] * 100.0);
            Check(total[0] <= total[1] + 1e-12, "kept coverage no worse than the plain box over the chain");
        }
    }
}

void TestSizes(std::mt19937 &rng)
{
    // Every size: the level's size, nothing written past it, a uniform colour kept.
    const int sides[] = {1, 2, 3, 4, 5, 7, 8, 16, 33};
    int bad = 0, cases = 0;
    for (int w : sides) {
        for (int h : sides) {
            if (w == 1 && h == 1)
                continue;
            Image src(size_t(w) * h * 4);
            for (size_t i = 0; i < src.size(); ++i)
                src[i] = (i & 3) == 3 ? uint8_t(1 + rng() % 255) : uint8_t(rng());
            const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
            const size_t bytes = size_t(dw) * dh * 4;
            Image dst(bytes + 64, 0xab);
            ls3d::tex::DownsampleMip(src.data(), w, h, dst.data());
            ls3d::tex::ScaleAlphaToCoverage(dst.data(), dw, dh, 0x20, 0.5);
            for (size_t i = bytes; i < dst.size(); ++i)
                bad += dst[i] != 0xab;
            ++cases;
        }
    }
    std::printf("sizes: %d cases, %d bytes written past the level\n", cases, bad);
    Check(bad == 0, "nothing written past the level");
}

}  // namespace

int main()
{
    std::mt19937 rng(7);
    TestIni();
    TestUniform(rng);
    TestGammaAndKey();
    TestBrightness(rng);
    TestCoverage(rng);
    TestSizes(rng);
    std::printf(g_failures ? "test_enhance: %d FAILED\n" : "test_enhance: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
