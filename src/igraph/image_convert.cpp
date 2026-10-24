// ITexture's 32-bit BGRA image to a texture surface (0x10077d00), with optional Floyd-Steinberg
// dithering into the 16-bit formats (ITC_DITHER).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <vector>

#include "igraph/itexture.h"

namespace ls3d::tex {

namespace {

// Error-diffusion buffer: (height + 1) rows of (width + 2) entries of red, green, blue, alpha;
// pixel (x, y) reads entry (x + 1, y) and passes its error to (x + 2, y), (x, y + 1), (x + 1, y + 1)
// and (x + 2, y + 1) with weights 7, 3, 5, 1 (/16). The error of a channel is the value modulo the
// quantisation step; the original takes 8 for every 4-bit channel (not 16) and 128 for the 1-bit
// alpha. Single-precision float operations in the original's order.
struct S_errors {
    explicit S_errors(uint32_t width, uint32_t height) : stride(width + 2), data((height + 1) * (width + 2) * 4, 0.0f) {}
    float *At(uint32_t x, uint32_t y) { return &data[(y * stride + x) * 4]; }
    uint32_t stride;
    std::vector<float> data;
};

enum { kR = 0, kG = 1, kB = 2, kA = 3 };

float Clamp255(float v)
{
    return v > 255.0f ? 255.0f : v;
}

// One channel: the dithered value (rounded), its error spread over the neighbours.
int DitherChannel(S_errors &err, uint32_t x, uint32_t y, int channel, uint8_t value, float step)
{
    const float v = static_cast<float>(value) + err.At(x + 1, y)[channel];
    const float e = std::fmod(v, step) * 0.0625f;
    float *right = err.At(x + 2, y), *below = err.At(x, y + 1);
    right[channel] = e * 7.0f + right[channel];
    below[channel] = e * 3.0f + below[channel];
    below[4 + channel] = e * 5.0f + below[4 + channel];
    below[8 + channel] = e + below[8 + channel];
    return static_cast<int>(std::lrintf(Clamp255(v)));
}

}  // namespace

// 0x10077d00. Rows go to the surface at its pitch: the original's undithered 16-bit paths step
// rows by twice (pitch - 2 * width) + 2 * width bytes and its A8R8G8B8 copy ignores the pitch,
// which only agree for pitch = row size.
bool WriteSurface(const ITexture *texture, const uint8_t *bgra, IDirect3DSurface8 *surface, uint32_t width,
                  uint32_t height, int kind)
{
    const bool dither = (texture->flags & ITC_DITHER) != 0;
    S_errors *errors = nullptr;
    S_errors errorStorage(dither && kind == 0 ? width : 0, dither && kind == 0 ? height : 0);
    if (dither && kind == 0)
        errors = &errorStorage;
    D3DSURFACE_DESC desc;
    surface->GetDesc(&desc);
    D3DLOCKED_RECT lr;
    surface->LockRect(&lr, nullptr, 0);
    uint8_t *bits = static_cast<uint8_t *>(lr.pBits);
    const uint8_t *p = bgra;
    switch (desc.Format) {
    case D3DFMT_A8R8G8B8:
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(bits + y * lr.Pitch, bgra + y * width * 4, width * 4);
        break;
    case D3DFMT_R5G6B5:
        for (uint32_t y = 0; y < height; ++y) {
            uint16_t *out = reinterpret_cast<uint16_t *>(bits + y * lr.Pitch);
            for (uint32_t x = 0; x < width; ++x, p += 4) {
                int r = p[2], g = p[1], b = p[0];
                if (dither && errors) {
                    r = DitherChannel(*errors, x, y, kR, p[2], 8.0f);
                    g = DitherChannel(*errors, x, y, kG, p[1], 4.0f);
                    b = DitherChannel(*errors, x, y, kB, p[0], 8.0f);
                    out[x] = static_cast<uint16_t>(((r & 0xfff8) << 5 | (g & 0xfc)) << 3 | ((b >> 3) & 0x1f));
                } else {
                    out[x] = static_cast<uint16_t>(((r & 0xf8) << 5 | (g & 0xfc)) << 3 | (b >> 3));
                }
            }
        }
        break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        for (uint32_t y = 0; y < height; ++y) {
            uint16_t *out = reinterpret_cast<uint16_t *>(bits + y * lr.Pitch);
            for (uint32_t x = 0; x < width; ++x, p += 4) {
                if (dither && errors) {
                    const int r = DitherChannel(*errors, x, y, kR, p[2], 8.0f);
                    const int g = DitherChannel(*errors, x, y, kG, p[1], 8.0f);
                    const int b = DitherChannel(*errors, x, y, kB, p[0], 8.0f);
                    const int a = DitherChannel(*errors, x, y, kA, p[3], 128.0f);
                    out[x] = static_cast<uint16_t>(((((a & 0xff80) << 1 | (r & 0xf8)) << 5 | (g & 0xf8)) << 2) |
                                                   ((b >> 3) & 0x1f));
                } else {
                    // (Blue in the red bits and red in the blue bits, as in the original.)
                    out[x] = static_cast<uint16_t>(((((p[3] & 0x80) << 1 | (p[0] & 0xf8)) << 5 | (p[1] & 0xf8)) << 2) |
                                                   (p[2] >> 3));
                }
            }
        }
        break;
    case D3DFMT_A4R4G4B4:
        for (uint32_t y = 0; y < height; ++y) {
            uint16_t *out = reinterpret_cast<uint16_t *>(bits + y * lr.Pitch);
            for (uint32_t x = 0; x < width; ++x, p += 4) {
                if (dither && errors) {
                    const int r = DitherChannel(*errors, x, y, kR, p[2], 8.0f);
                    const int g = DitherChannel(*errors, x, y, kG, p[1], 8.0f);
                    const int b = DitherChannel(*errors, x, y, kB, p[0], 8.0f);
                    const int a = DitherChannel(*errors, x, y, kA, p[3], 8.0f);
                    out[x] = static_cast<uint16_t>(((a & 0xfff0) << 8 | (r & 0xf0) << 4) | (g & 0xf0) | ((b >> 4) & 0xf));
                } else {
                    out[x] = static_cast<uint16_t>(((p[3] & 0xf0) << 4 | (p[2] & 0xf0)) << 4 | (p[1] & 0xf0) | (p[0] >> 4));
                }
            }
        }
        break;
    case D3DFMT_L8:
        // The original writes red, green and blue into three planes of width * height bytes, the
        // last two past the end of the 8-bit surface; the port keeps the first (red).
        for (uint32_t i = 0; i < width * height; ++i)
            bits[i] = bgra[i * 4 + 2];
        break;
    default:
        break;                                  // (X8R8G8B8 and the rest: left as they are)
    }
    surface->UnlockRect();
    return true;
}

}  // namespace ls3d::tex
