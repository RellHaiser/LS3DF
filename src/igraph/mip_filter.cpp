// Port enhancement: gamma-correct, alpha-weighted mip levels with kept coverage (mip_filter.h).
#include "igraph/mip_filter.h"

#include <cmath>
#include <cstdlib>

namespace ls3d::tex {

namespace {

double SrgbToLinear(double s)
{
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

struct S_srgbTables {
    float toLinear[256];
    // The linear values half way (in sRGB) between the codes i and i + 1.
    float midpoints[255];
};

const S_srgbTables &Tables()
{
    static const S_srgbTables tables = [] {
        S_srgbTables t;
        for (int i = 0; i < 256; ++i)
            t.toLinear[i] = static_cast<float>(SrgbToLinear(i / 255.0));
        for (int i = 0; i < 255; ++i)
            t.midpoints[i] = static_cast<float>(SrgbToLinear((i + 0.5) / 255.0));
        return t;
    }();
    return tables;
}

// The nearest sRGB code of a linear value: the number of midpoints at or below it.
uint8_t ToSrgb(const S_srgbTables &t, float linear)
{
    int lo = 0, hi = 255;
    while (lo < hi) {
        const int m = (lo + hi) >> 1;
        if (linear < t.midpoints[m])
            hi = m;
        else
            lo = m + 1;
    }
    return static_cast<uint8_t>(lo);
}

}  // namespace

double AlphaCoverage(const uint8_t *bgra, int width, int height, int alphaRef)
{
    const int n = width * height;
    if (n <= 0)
        return 0.0;
    int passed = 0;
    for (int i = 0; i < n; ++i)
        passed += bgra[i * 4 + 3] >= alphaRef;
    return static_cast<double>(passed) / n;
}

void DownsampleMip(const uint8_t *src, int width, int height, uint8_t *dst)
{
    const S_srgbTables &t = Tables();
    const int w = width >> 1 ? width >> 1 : 1;
    const int h = height >> 1 ? height >> 1 : 1;
    const int lastX = width - 1, lastY = height - 1;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row0 = src + (2 * y < lastY ? 2 * y : lastY) * width * 4;
        const uint8_t *row1 = src + (2 * y + 1 < lastY ? 2 * y + 1 : lastY) * width * 4;
        uint8_t *d = dst + y * w * 4;
        for (int x = 0; x < w; ++x, d += 4) {
            const int x0 = (2 * x < lastX ? 2 * x : lastX) * 4;
            const int x1 = (2 * x + 1 < lastX ? 2 * x + 1 : lastX) * 4;
            const uint8_t *p[4] = {row0 + x0, row0 + x1, row1 + x0, row1 + x1};
            float plain[3] = {}, weighted[3] = {};
            int alpha = 0;
            for (const uint8_t *s : p) {
                const float a = s[3];
                alpha += s[3];
                for (int c = 0; c < 3; ++c) {
                    const float v = t.toLinear[s[c]];
                    plain[c] += v;
                    weighted[c] += v * a;
                }
            }
            for (int c = 0; c < 3; ++c)
                d[c] = ToSrgb(t, alpha ? weighted[c] / static_cast<float>(alpha) : plain[c] * 0.25f);
            d[3] = static_cast<uint8_t>((alpha + 2) >> 2);
        }
    }
}

// The threshold t whose count of alpha >= t is nearest the target (of equally near ones the
// closest to `ref`) is mapped onto `ref`: the alpha scaled by ref / t, texels on each side of t
// kept on their side of ref.
void ScaleAlphaToCoverage(uint8_t *bgra, int width, int height, int ref, double coverage)
{
    const int n = width * height;
    int histogram[256] = {};
    for (int i = 0; i < n; ++i)
        ++histogram[bgra[i * 4 + 3]];
    const double target = coverage * n;
    int best = ref;
    double bestError = 0.0;
    bool found = false;
    int above = 0;
    for (int t = 255; t >= 1; --t) {
        above += histogram[t];
        const double error = std::fabs(above - target);
        if (!found || error < bestError || (error == bestError && std::abs(t - ref) < std::abs(best - ref))) {
            found = true;
            best = t;
            bestError = error;
        }
    }
    if (best == ref)
        return;
    const double scale = static_cast<double>(ref) / best;
    for (int i = 0; i < n; ++i) {
        uint8_t &a = bgra[i * 4 + 3];
        long v = std::lround(a * scale);
        if (v > 255)
            v = 255;
        if (a >= best)
            v = v < ref ? ref : v;
        else if (v >= ref)
            v = ref - 1;
        a = static_cast<uint8_t>(v);
    }
}

}  // namespace ls3d::tex
