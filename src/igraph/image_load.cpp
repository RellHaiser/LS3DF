// Image files for ITexture: .bmp / .tga / memory images (0x100775f0) and the DXT decoders of the
// DirectX SDK's texture tool sample, linked into the original (0x100794a0 - 0x1007a0f7).
#include <cstdint>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "external/rw_data.h"
#include "igraph/itexture.h"

namespace ls3d::tex {

// 0x100775f0. `info` gets the image's BITMAPINFOHEADER with biXPelsPerMeter replaced by the row
// pitch; rows are bottom-up (top-down TGA files are flipped). A palette (8-bit images, or as many
// entries as biClrUsed says) comes as 256 BGRA entries.
bool LoadImage(const void *source, int width, int height, uint32_t flags, uint8_t **bitsOut, uint32_t **paletteOut,
               BITMAPINFOHEADER *infoOut, bool fromMemory, FILETIME *fileTime, int handle)
{
    const char *name = static_cast<const char *>(source);
    BITMAPINFOHEADER info;
    const uint8_t *memory = nullptr;
    bool tga = false;
    bool topDown = false;
    int file = handle;
    if (fromMemory) {
        std::memcpy(&info, source, sizeof info);
        memory = static_cast<const uint8_t *>(source) + sizeof info;
    } else {
        const char *ext = std::strrchr(name, '.');
        if (!ext)
            return false;
        if (ext[1] == 't' || ext[1] == 'T')
            tga = true;
        else if (ext[1] != 'b' && ext[1] != 'B')
            return false;
        if (file == -1) {
            file = dtaOpen(name, false);
            if (file == -1) {
                dbgPrintf("LoadImage() - (%s) File not found !!", name);
                return false;
            }
        }
        if (fileTime)
            dtaGetTime(file, fileTime, false);
        if (tga) {
            uint8_t header[0x12];
            if (dtaRead(file, header, 0x12) <= 0) {
                dtaClose(file);
                return false;
            }
            if (static_cast<int8_t>(header[0]) > 0)
                dtaSeek(file, static_cast<int8_t>(header[0]), 1);       // skip the image id
            if (header[0x11] & 0x10)
                dbgPrintf("!! Error. Right-to-left TGA detected, but not reversed !");
            topDown = (header[0x11] & 0x20) != 0;
            int16_t w, h;
            std::memcpy(&w, header + 0xc, 2);
            std::memcpy(&h, header + 0xe, 2);
            const int bpp = static_cast<int8_t>(header[0x10]);
            std::memset(&info, 0, sizeof info);
            info.biSize = 0x28;
            info.biWidth = w;
            info.biHeight = h;
            info.biPlanes = 1;
            info.biBitCount = static_cast<WORD>(bpp);
            info.biXPelsPerMeter = (bpp / 8) * w;
            info.biSizeImage = info.biXPelsPerMeter * h;
        } else {
            BITMAPFILEHEADER fileHeader;
            if (dtaRead(file, &fileHeader, 0xe) <= 0 || dtaRead(file, &info, 0x28) <= 0) {
                dtaClose(file);
                return false;
            }
        }
    }
    if (!tga) {
        if (info.biCompression) {
            dbgPrintf("LoadImage() - (%s) Unsupported compresion :%lu!!", name, info.biCompression);
            dtaClose(file);
            return false;
        }
        // BMP rows are padded to 4 bytes.
        const int rowBytes = info.biBitCount * info.biWidth / 8;
        const int padded = rowBytes / 4 * 4;
        info.biXPelsPerMeter = (!(flags & ITC_BMP_UNPADDED) && padded != rowBytes) ? padded + 4 : rowBytes;
    }
    const int pitch = info.biXPelsPerMeter;

    uint32_t *palette = nullptr;
    if (!info.biClrUsed && info.biBitCount <= 8)
        info.biClrUsed = 1u << info.biBitCount;
    if (info.biClrUsed) {
        palette = static_cast<uint32_t *>(::operator new(0x400, std::nothrow));
        if (!palette) {
            dtaClose(file);
            return false;
        }
        std::memset(palette, 0, 0x400);
        if (fromMemory) {
            std::memcpy(palette, memory, info.biClrUsed * 4);
            memory += info.biClrUsed * 4;
        } else if (!tga) {
            if (dtaRead(file, palette, info.biClrUsed * 4) != static_cast<int>(info.biClrUsed * 4))
                goto palette_error;
        } else {
            for (uint32_t i = 0; i < info.biClrUsed; ++i) {
                if (dtaRead(file, &palette[i], 3) != 3)
                    goto palette_error;
            }
        }
    }
    *paletteOut = palette;

    {
        const int size = pitch * info.biHeight;
        uint8_t *bits = static_cast<uint8_t *>(::operator new(size, std::nothrow));
        if (!bits) {
            ::operator delete(palette);
            dtaClose(file);
            return false;
        }
        if (fromMemory) {
            std::memcpy(bits, memory, size);
        } else {
            if (dtaRead(file, bits, size) != size) {
                dbgPrintf("LoadImage() - (%s) Error reading bitmap !!", name);
                ::operator delete(bits);
                ::operator delete(palette);
                dtaClose(file);
                return false;
            }
            dtaClose(file);
        }
        if (topDown) {
            uint8_t *row = static_cast<uint8_t *>(::operator new(pitch, std::nothrow));
            if (!row) {
                ::operator delete(bits);
                ::operator delete(palette);
                return false;
            }
            uint8_t *a = bits, *b = bits + (info.biHeight - 1) * pitch;
            for (int i = info.biHeight / 2; i; --i, a += pitch, b -= pitch) {
                std::memcpy(row, a, pitch);
                std::memcpy(a, b, pitch);
                std::memcpy(b, row, pitch);
            }
            ::operator delete(row);
        }
        if (!width || !height || (width == info.biWidth && height == info.biHeight)) {
            if (infoOut)
                *infoOut = info;
            *bitsOut = bits;
            return true;
        }
        // Resized to width x height (nearest pixel), rows without padding.
        const int bpp = info.biBitCount;
        uint8_t *resized = static_cast<uint8_t *>(::operator new(bpp * width * height >> 3, std::nothrow));
        if (!resized) {
            ::operator delete(bits);
            ::operator delete(palette);
            return false;
        }
        const float stepX = static_cast<float>(info.biWidth) / static_cast<float>(width);
        const float stepY = static_cast<float>(info.biHeight) / static_cast<float>(height);
        uint8_t *out = resized;
        for (int y = 0; y < height; ++y) {
            const int row = static_cast<int>(static_cast<float>(y) * stepY) * pitch;
            for (int x = 0; x < width; ++x) {
                const int col = static_cast<int>(static_cast<float>(x) * stepX);
                std::memcpy(out, bits + row + (col * bpp >> 3), bpp >> 3);
                out += bpp >> 3;
            }
        }
        ::operator delete(bits);
        *bitsOut = resized;
        if (infoOut) {
            info.biWidth = width;
            info.biHeight = height;
            info.biXPelsPerMeter = bpp * width >> 3;
            info.biSizeImage = bpp * width * height >> 3;
            *infoOut = info;
        }
        return true;
    }

palette_error:
    dbgPrintf("LoadImage() - (%s) Error reading palette !!", name);
    ::operator delete(palette);
    dtaClose(file);
    return false;
}

namespace {

// The four colours of a DXT colour block as BGRA (the 565 endpoints expanded by shifts). Without
// the 4-colour order the fourth is the transparent 0x0000ffff, as in the sample.
void DecodeColors(const uint8_t *block, uint32_t colors[4])
{
    uint16_t c0, c1;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    const uint32_t r0 = static_cast<uint8_t>((c0 >> 11) << 3), g0 = static_cast<uint8_t>((c0 >> 5) << 2),
                   b0 = static_cast<uint8_t>(c0 << 3);
    const uint32_t r1 = static_cast<uint8_t>((c1 >> 11) << 3), g1 = static_cast<uint8_t>((c1 >> 5) << 2),
                   b1 = static_cast<uint8_t>(c1 << 3);
    auto pack = [](uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | (r & 0xff) << 16 | (g & 0xff) << 8 | (b & 0xff); };
    colors[0] = pack(r0, g0, b0);
    colors[1] = pack(r1, g1, b1);
    if (c1 < c0) {
        colors[2] = pack((r1 + r0 * 2) / 3, (g1 + g0 * 2) / 3, (b1 + b0 * 2) / 3);
        colors[3] = pack((r0 + r1 * 2) / 3, (g0 + g1 * 2) / 3, (b0 + b1 * 2) / 3);
    } else {
        colors[2] = pack((r1 + r0) / 2, (g1 + g0) / 2, (b1 + b0) / 2);
        colors[3] = 0x0000ffff;
    }
}

// A block's 4 x 4 colour indices into the image at `dst`.
void WriteColorBlock(const uint8_t *indices, const uint32_t colors[4], uint32_t *dst, uint32_t width)
{
    for (int y = 0; y < 4; ++y, dst += width) {
        for (int x = 0; x < 4; ++x)
            dst[x] = colors[(indices[y] >> (x * 2)) & 3];
    }
}

}  // namespace

// 0x100794a0
void DecodeDxt1(const uint8_t *src, uint32_t *dst, uint32_t width, uint32_t height)
{
    const int blocksX = static_cast<int>(width) / 4, blocksY = static_cast<int>(height) / 4;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const uint8_t *block = src + (by * blocksX + bx) * 8;
            uint32_t colors[4];
            DecodeColors(block, colors);
            WriteColorBlock(block + 4, colors, dst + by * 4 * width + bx * 4, width);
        }
    }
}

// 0x10079790
void DecodeDxt3(const uint8_t *src, uint32_t *dst, uint32_t width, uint32_t height)
{
    const int blocksX = static_cast<int>(width) / 4, blocksY = static_cast<int>(height) / 4;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const uint8_t *block = src + (by * blocksX + bx) * 16;
            uint32_t *out = dst + by * 4 * width + bx * 4;
            uint32_t colors[4];
            DecodeColors(block + 8, colors);
            WriteColorBlock(block + 12, colors, out, width);
            // Explicit 4-bit alpha, a row per 16-bit word.
            for (int y = 0; y < 4; ++y) {
                uint16_t row;
                std::memcpy(&row, block + y * 2, 2);
                for (int x = 0; x < 4; ++x, row >>= 4) {
                    const uint8_t a = static_cast<uint8_t>((row & 0xf) | (row & 0xf) << 4);
                    out[y * width + x] = (out[y * width + x] & 0xffffff) | static_cast<uint32_t>(a) << 24;
                }
            }
        }
    }
}

// 0x10079e30: a DXT5 alpha block (two endpoints, 3-bit indices) into the alpha bytes.
static void DecodeAlpha3Bit(uint32_t *dst, const uint8_t *block, uint32_t width)
{
    const uint32_t a0 = block[0], a1 = block[1];
    uint16_t alphas[8];
    alphas[0] = static_cast<uint16_t>(a0);
    alphas[1] = static_cast<uint16_t>(a1);
    if (a0 <= a1) {
        alphas[2] = static_cast<uint16_t>((a1 + a0 * 4) / 5);
        alphas[3] = static_cast<uint16_t>((a0 * 3 + a1 * 2) / 5);
        alphas[4] = static_cast<uint16_t>((a1 * 3 + a0 * 2) / 5);
        alphas[5] = static_cast<uint16_t>((a0 + a1 * 4) / 5);
        alphas[6] = 0;
        alphas[7] = 0xff;
    } else {
        alphas[2] = static_cast<uint16_t>((a1 + a0 * 6) / 7);
        alphas[3] = static_cast<uint16_t>((a0 * 5 + a1 * 2) / 7);
        alphas[4] = static_cast<uint16_t>((a1 * 3 + a0 * 4) / 7);
        alphas[5] = static_cast<uint16_t>((a0 * 3 + a1 * 4) / 7);
        alphas[6] = static_cast<uint16_t>((a1 * 5 + a0 * 2) / 7);
        alphas[7] = static_cast<uint16_t>((a0 + a1 * 6) / 7);
    }
    uint8_t index[16];
    uint32_t bits;
    std::memcpy(&bits, block + 2, 4);
    for (int i = 0; i < 8; ++i)
        index[i] = static_cast<uint8_t>((bits >> (i * 3)) & 7);
    std::memcpy(&bits, block + 5, 4);
    for (int i = 0; i < 8; ++i)
        index[8 + i] = static_cast<uint8_t>((bits >> (i * 3)) & 7);
    for (int y = 0; y < 4; ++y, dst += width) {
        for (int x = 0; x < 4; ++x)
            dst[x] = static_cast<uint32_t>(static_cast<uint8_t>(alphas[index[y * 4 + x]])) << 24 | (dst[x] & 0xffffff);
    }
}

// 0x10079b10
void DecodeDxt5(const uint8_t *src, uint32_t *dst, uint32_t width, uint32_t height)
{
    const int blocksX = static_cast<int>(width) / 4, blocksY = static_cast<int>(height) / 4;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const uint8_t *block = src + (by * blocksX + bx) * 16;
            uint32_t *out = dst + by * 4 * width + bx * 4;
            uint32_t colors[4];
            DecodeColors(block + 8, colors);
            WriteColorBlock(block + 12, colors, out, width);
            DecodeAlpha3Bit(out, block, width);
        }
    }
}

}  // namespace ls3d::tex
