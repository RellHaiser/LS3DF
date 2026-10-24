// IGraph: texture search directories, image file headers, screenshots and image resampling
// (0x1006dec0 - 0x1006f040, 0x1006fbd0 - 0x10070d30).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <direct.h>

#include "core/c_vector.h"
#include "core/debug.h"
#include "core/i3d_result.h"
#include "external/rw_data.h"
#include "i3d/i3d_links.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"

using namespace ls3d;
using namespace ls3d::ig;

namespace {

C_vector<char *> g_textureDirs(0x20);           // 0x100d1ef4, each ending with '\'
char g_lastDir[0x100];                          // 0x100d21d8 where the last texture was found
bool g_lastDirValid = false;                    // 0x100d22d8

bool FileExists(const char *name)
{
    const int h = dtaOpen(name, false);
    if (h < 0)
        return false;
    dtaClose(h);
    return true;
}

// 0x1006fd60 (I3D_driver::m_AddNameToList in the original's message)
HRESULT AddNameToList(C_vector<char *> &list, const char *dir, bool recursive)
{
    const size_t len = std::strlen(dir);
    char *copy = static_cast<char *>(::operator new(len + 2, std::nothrow));
    if (!copy) {
        dbgPrintf(" !! Out of memory !! \tI3D_driver::m_AddNameToList()");
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    std::strcpy(copy, dir);
    // (The original reads dir[-1] for an empty name.)
    if (!len || dir[len - 1] != '\\') {
        copy[len] = '\\';
        copy[len + 1] = 0;
    }
    list.PushBack(copy);
    if (recursive) {
        char pattern[0x100];
        std::snprintf(pattern, sizeof pattern, "%s\\*.*", dir);
        WIN32_FIND_DATAA fd;
        const HANDLE find = FindFirstFileA(pattern, &fd);
        if (find == INVALID_HANDLE_VALUE) {
            _mkdir(copy);
            return 0;
        }
        do {
            if (fd.cFileName[0] != '.' && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char sub[0x100];
                std::snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
                AddNameToList(list, sub, true);
            }
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    return 0;
}

}  // namespace

// 0x1006fbd0: the path of a texture file: as given when it has a directory and exists, else in the
// directory of the previous find, else in the search directories (last added first); "" if none.
void FindTextureFile(const char *fileName, char *path)
{
    if (std::strchr(fileName, '\\') && FileExists(fileName)) {
        std::strcpy(path, fileName);
        return;
    }
    if (g_lastDirValid) {
        std::strcpy(path, g_lastDir);
        std::strcat(path, fileName);
        if (FileExists(path))
            return;
    }
    for (int i = g_textureDirs.Size(); i--;) {
        std::strcpy(path, g_textureDirs[i]);
        std::strcat(path, fileName);
        if (FileExists(path)) {
            std::strcpy(g_lastDir, g_textureDirs[i]);
            g_lastDirValid = true;
            return;
        }
    }
    g_lastDirValid = false;
    path[0] = 0;
}

// 0x1006ff80
HRESULT __stdcall IGraph::AddDir(const char *dir, bool recursive)
{
    return AddNameToList(g_textureDirs, dir, recursive);
}

// 0x1006ffa0
HRESULT __stdcall IGraph::DelDir(const char *text)
{
    for (int i = g_textureDirs.Size(); i--;) {
        if (std::strstr(g_textureDirs[i], text)) {
            ::operator delete(g_textureDirs[i]);
            g_textureDirs.RemoveAtSwap(i);
        }
    }
    return 0;
}

// 0x10070000
void __stdcall IGraph::DelAllDirs()
{
    for (int i = g_textureDirs.Size(); i; --i)
        ::operator delete(g_textureDirs[i - 1]);
    g_textureDirs.Free();
}

// 0x1006dec0
HRESULT __stdcall IGraph::GetImageInfo(const char *fileName, uint16_t *width, uint16_t *height, uint8_t *bitDepth,
                                       uint32_t flags, int *handle)
{
    if (flags & 1)
        return 0;
    const char *ext = std::strrchr(fileName, '.');
    if (!ext)
        return I3DERR_BADTEXTURE;
    int kind;
    if (ext[1] == 'b' || ext[1] == 'B')
        kind = 0;                               // .bmp
    else if (ext[1] == 't' || ext[1] == 'T')
        kind = 1;                               // .tga
    else
        return I3DERR_BADTEXTUREFORMAT;
    const int h = dtaOpen(fileName, false);
    if (h == -1)
        return I3DERR_NOFILE;
    uint8_t header[0x12];
    switch (kind) {
    case 1:
        if (dtaRead(h, header, 0x12) <= 0) {
            dtaClose(h);
            return I3DERR_FILESYSTEM;
        }
        std::memcpy(width, header + 0xc, 2);
        std::memcpy(height, header + 0xe, 2);
        *bitDepth = header[0x10];
        break;
    case 0: {
        BITMAPINFOHEADER info;
        if (dtaRead(h, header, 0xe) <= 0 || dtaRead(h, &info, 0x28) <= 0) {
            dtaClose(h);
            return I3DERR_FILESYSTEM;
        }
        *width = static_cast<uint16_t>(info.biWidth);
        *height = static_cast<uint16_t>(info.biHeight);
        *bitDepth = static_cast<uint8_t>(info.biBitCount);
        break;
    }
    default:
        dbgPrintf("IGraph::GetImageInfo() - Unsupported file format %s !", fileName);
        dtaClose(h);
        return I3DERR_BADTEXTUREFORMAT;
    }
    if (handle) {
        *handle = h;
        dtaSeek(h, 0, 0);
        return 0;
    }
    dtaClose(h);
    return 0;
}

namespace {

// One channel of a scan line, run-length encoded as in Wavefront RLA files: a count byte n >= 0 for
// n + 1 repeats of the next byte, -n for n literal bytes (0x1006e050).
int EncodeRlaChannel(const uint32_t *src, uint8_t *out, int count, uint32_t mask, int shift)
{
    auto value = [&](int i) { return static_cast<uint8_t>((src[i] & mask) >> shift); };
    int len = 0;
    while (count > 0) {
        if (count > 2 && value(1) == value(0) && value(1) == value(2)) {
            int n = 2;
            while (n < count && value(n) == value(n - 1) && n < 0x7f)
                ++n;
            out[len++] = static_cast<uint8_t>(n - 1);
            out[len++] = value(0);
            src += n;
            count -= n;
        } else {
            int n = 1;
            while (n < count) {
                if (count - n > 2 && value(n) == value(n + 1) && value(n + 1) == value(n + 2))
                    break;
                if (n >= 0x7f)
                    break;
                ++n;
            }
            out[len++] = static_cast<uint8_t>(-n);
            for (int i = 0; i < n; ++i)
                out[len++] = value(i);
            src += n;
            count -= n;
        }
    }
    return static_cast<int16_t>(len);
}

void WriteBigEndian16(FILE *f, int value)
{
    const uint8_t bytes[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    std::fwrite(bytes, 2, 1, f);
}

void WriteRlaChannel(FILE *f, const uint32_t *src, uint8_t *buf, int count, uint32_t mask, int shift)
{
    const int len = EncodeRlaChannel(src, buf, count, mask, shift);
    WriteBigEndian16(f, len);
    std::fwrite(buf, static_cast<int16_t>(len), 1, f);
}

// Wavefront RLA header (740 bytes, big-endian shorts).
#pragma pack(push, 1)
struct S_rlaHeader {
    int16_t window[4];                          // left, right, bottom, top
    int16_t active[4];
    int16_t frame, storageType, numChannels, numMatte, numAux, revision;
    char gamma[16], redPri[24], greenPri[24], bluePri[24], whitePt[24];
    int32_t jobNumber;
    char name[128], desc[128], program[64], machine[32], user[32], date[20], aspect[24], aspectRatio[8], chan[32];
    int16_t field;
    char time[12], filter[32];
    int16_t chanBits, matteType, matteBits, auxType, auxBits;
    char aux[32], space[36];
    int32_t next;
};
#pragma pack(pop)
static_assert(sizeof(S_rlaHeader) == 0x2e4, "RLA header");

int16_t SwapBytes16(int v)
{
    return static_cast<int16_t>(((v & 0xff) << 8) | ((v >> 8) & 0xff));
}

}  // namespace

// 0x1006e210
bool __stdcall IGraph::SaveShot(void *fileNameOrBuffer, const RECT *rect, uint32_t flags, void *depthBuffer)
{
    const bool toMemory = (flags & IGSHOT_MEMORY) != 0;
    const char *fileName = static_cast<const char *>(fileNameOrBuffer);
    if (!toMemory && !std::strstr(fileName, ".TGA") && !std::strstr(fileName, ".tga"))
        dbgPrintf("!!IGraph::SaveShot() - WARNING! File extension isn't .TGA !! %s", fileName);

    IDirect3DSurface8 *target = nullptr;
    IDirect3DSurface8 *image = nullptr;
    if (g_pDevice->GetRenderTarget(&target) < 0)
        return false;
    D3DSURFACE_DESC desc;
    if (target->GetDesc(&desc) < 0) {
        target->Release();
        return false;
    }
    if (!rect) {
        g_pDevice->CreateImageSurface(desc.Width, desc.Height, desc.Format, &image);
        if (g_pDevice->CopyRects(target, nullptr, 0, image, nullptr) < 0) {
            target->Release();
            return false;
        }
    } else {
        // The copy gets the rectangle's right x bottom size; the rectangle lands at its top left.
        desc.Width = rect->right;
        desc.Height = rect->bottom;
        desc.Size = 0;
        g_pDevice->CreateImageSurface(desc.Width, desc.Height, desc.Format, &image);
        const POINT origin = {0, 0};
        if (g_pDevice->CopyRects(target, rect, 1, image, &origin) < 0) {
            target->Release();
            return false;
        }
    }
    D3DLOCKED_RECT lr;
    if (image->LockRect(&lr, nullptr, D3DLOCK_READONLY) < 0) {
        target->Release();
        image->Release();
        return false;
    }
    auto finish = [&](bool result) {
        image->UnlockRect();
        image->Release();
        target->Release();
        return result;
    };

    if ((flags & IGSHOT_DEPTH) && !toMemory) {
        // Colour plus depth into an .rla file next to the .tga.
        if (!(g_init.flags & IG_LOCKABLE_ZBUFFER)) {
            dbgPrintf("!!IGraph::SaveShot() - ERROR! Z-buffer pixel format isn't LOCKABLE !!");
            return finish(false);
        }
        IDirect3DSurface8 *depth = nullptr;
        g_pDevice->GetDepthStencilSurface(&depth);
        D3DLOCKED_RECT dlr;
        if (depth->LockRect(&dlr, rect, D3DLOCK_READONLY) < 0) {
            dbgPrintf("!!IGraph::SaveShot() - ERROR! Z-buffer pixel format isn't LOCKABLE !!");
            depth->Release();
            return finish(false);
        }
        char rlaName[0x100] = {};               // (uninitialised in the original without a '.')
        for (int i = static_cast<int>(std::strlen(fileName)) - 1; i >= 0; --i) {
            if (fileName[i] == '.') {
                std::strncpy(rlaName, fileName, i + 1);
                std::strcpy(rlaName + i + 1, "rla");
                break;
            }
        }
        int width, height;
        if (!rect) {
            width = ScrnX();
            height = ScrnY();
        } else {
            width = rect->right;
            height = rect->bottom;
        }
        FILE *f = std::fopen(rlaName, "wb");
        if (!f) {
            depth->UnlockRect();
            depth->Release();
            dbgPrintf("IGraph::SaveShot() - Cannot create file %s !!!", rlaName);
            return finish(false);
        }
        S_rlaHeader header = {};
        header.window[1] = SwapBytes16(width - 1);
        header.active[1] = header.window[1];
        header.window[3] = SwapBytes16(height - 1);
        header.active[3] = header.window[3];
        header.frame = SwapBytes16(1);
        header.numChannels = SwapBytes16(3);
        header.revision = SwapBytes16(0xfffe);
        header.chanBits = SwapBytes16(8);
        std::strcpy(header.gamma, "2.2");
        std::strcpy(header.redPri, "0.670 0.330");
        std::strcpy(header.greenPri, "0.210 0.710");
        std::strcpy(header.bluePri, "0.140 0.080");
        std::strcpy(header.whitePt, "0.310 0.316");
        std::strcpy(header.program, "LS3D engine ( Z )");
        std::strncpy(header.name, rlaName, 0x7f);
        std::strcpy(header.desc, "24bit RGB + 16bit Z-channel");
        std::strcpy(header.machine, "unknown");
        std::strcpy(header.user, "unknown");
        char date[16], time[16];
        _strdate(date);
        _strtime(time);
        std::sprintf(header.date, "%s  %s", date, time);
        // fild / fidiv in single precision.
        std::sprintf(header.aspect, "%0.3f", static_cast<double>(static_cast<float>(width) / static_cast<float>(height)));
        std::strcpy(header.chan, "rgb");
        std::fwrite(&header, sizeof header, 1, f);

        uint32_t *offsets = static_cast<uint32_t *>(::operator new(sizeof(uint32_t) * height, std::nothrow));
        if (!offsets) {
            depth->UnlockRect();
            depth->Release();
            dbgPrintf("IGraph::SaveShot() - Error saving depth !");
            std::fclose(f);
            return finish(false);
        }
        std::fwrite(offsets, 4, height, f);     // placeholder, rewritten at the end
        uint8_t *line = static_cast<uint8_t *>(::operator new(width * 3));
        uint32_t *z = static_cast<uint32_t *>(::operator new(width * 4));
        uint32_t *rgb = static_cast<uint32_t *>(::operator new(width * 4));
        const uint8_t *colorRow = static_cast<const uint8_t *>(lr.pBits) + (height - 1) * lr.Pitch;
        const uint8_t *depthRow = static_cast<const uint8_t *>(dlr.pBits) + (height - 1) * dlr.Pitch;
        const uint32_t *pixels = rgb;
        if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
            dbgPrintf("!!IGraph::SaveShot() - WARNING! Converting poor quality 16bit frame buffer to 32bit RLA !");
        const bool wBuffer = i3d::UseWBuffer();
        for (int y = 0; y < height; ++y) {
            // Scan lines bottom up, each with its offset in the table.
            const uint32_t pos = static_cast<uint32_t>(std::ftell(f));
            offsets[y] = _byteswap_ulong(pos);
            const uint16_t *p16 = reinterpret_cast<const uint16_t *>(colorRow);
            uint8_t *dst = reinterpret_cast<uint8_t *>(rgb);
            if (desc.Format == D3DFMT_R5G6B5) {
                for (int x = 0; x < width; ++x, dst += 4) {
                    const uint16_t p = p16[x];
                    dst[2] = static_cast<uint8_t>(p >> 8) & 0xf8;
                    dst[1] = static_cast<uint8_t>(p >> 3) & 0xfc;
                    dst[0] = static_cast<uint8_t>(p << 3);
                }
            } else if (desc.Format == D3DFMT_X1R5G5B5 || desc.Format == D3DFMT_A1R5G5B5) {
                for (int x = 0; x < width; ++x, dst += 4) {
                    const uint16_t p = p16[x];
                    dst[2] = static_cast<uint8_t>(p >> 7) & 0xf8;
                    dst[1] = static_cast<uint8_t>(p >> 2) & 0xf8;
                    dst[0] = static_cast<uint8_t>(p << 3);
                }
            } else {
                pixels = reinterpret_cast<const uint32_t *>(colorRow);  // (read as 32-bit pixels)
            }
            WriteRlaChannel(f, pixels, line, width, 0xff0000, 0x10);
            WriteRlaChannel(f, pixels, line, width, 0xff00, 8);
            WriteRlaChannel(f, pixels, line, width, 0xff, 0);
            // Depth as floats: 10000 * (1 - z), or its reciprocal for a w-buffer.
            const uint16_t *zRow = reinterpret_cast<const uint16_t *>(depthRow);
            for (int x = 0; x < width; ++x) {
                float v = 1.0f - static_cast<float>(zRow[x]) * 1.5259022e-05f;
                if (wBuffer)
                    v = 1.0f / v;
                v *= 10000.0f;
                std::memcpy(&z[x], &v, 4);
            }
            WriteRlaChannel(f, z, line, width, 0xff000000, 0x18);
            WriteRlaChannel(f, z, line, width, 0xff0000, 0x10);
            WriteRlaChannel(f, z, line, width, 0xff00, 8);
            WriteRlaChannel(f, z, line, width, 0xff, 0);
            colorRow -= lr.Pitch;
            depthRow -= dlr.Pitch;
        }
        std::fseek(f, 0x2e4, SEEK_SET);
        std::fwrite(offsets, 4, height, f);
        std::fclose(f);
        ::operator delete(offsets);
        ::operator delete(line);
        ::operator delete(z);
        ::operator delete(rgb);
        depth->UnlockRect();
        depth->Release();
        return finish(true);
    }

    // TGA (uncompressed, bottom-up) header.
    uint8_t tga[0x12] = {};
    tga[2] = 2;
    switch (desc.Format) {
    case D3DFMT_R8G8B8: tga[0x10] = 24; break;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8: tga[0x10] = 32; break;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: tga[0x10] = 16; break;
    default: break;
    }
    const int16_t width = static_cast<int16_t>(desc.Width);
    const int16_t height = static_cast<int16_t>(desc.Height);
    std::memcpy(tga + 0xc, &width, 2);
    std::memcpy(tga + 0xe, &height, 2);
    const int rowBytes = (static_cast<int8_t>(tga[0x10]) / 8) * width;

    if (toMemory) {
        if ((flags & IGSHOT_DEPTH) && depthBuffer) {
            IDirect3DSurface8 *depth = nullptr;
            g_pDevice->GetDepthStencilSurface(&depth);
            D3DLOCKED_RECT dlr;
            if (depth->LockRect(&dlr, rect, D3DLOCK_READONLY) < 0) {
                dbgPrintf("!!IGraph::SaveShot() - ERROR! Z-buffer pixel format isn't LOCKABLE !!");
                depth->Release();
                return finish(false);
            }
            const uint8_t *row = static_cast<const uint8_t *>(dlr.pBits) + (height - 1) * dlr.Pitch;
            uint8_t *dst = static_cast<uint8_t *>(depthBuffer);
            for (int y = height; y; --y) {
                std::memcpy(dst, row, width * 2);
                dst += width * 2;
                row -= dlr.Pitch;
            }
            depth->UnlockRect();
            depth->Release();
        }
        const uint8_t *row = static_cast<const uint8_t *>(lr.pBits) + (height - 1) * lr.Pitch;
        uint8_t *dst = static_cast<uint8_t *>(fileNameOrBuffer);
        if ((flags & IGSHOT_RGB24) && desc.Format != D3DFMT_R8G8B8) {
            for (int y = height; y; --y) {
                const uint16_t *p16 = reinterpret_cast<const uint16_t *>(row);
                switch (desc.Format) {
                case D3DFMT_A8R8G8B8:
                case D3DFMT_X8R8G8B8:
                    for (int x = 0; x < width; ++x) {
                        dst[x * 3] = row[x * 4];
                        dst[x * 3 + 1] = row[x * 4 + 1];
                        dst[x * 3 + 2] = row[x * 4 + 2];
                    }
                    break;
                case D3DFMT_R5G6B5:
                    for (int x = 0; x < width; ++x) {
                        const uint16_t p = p16[x];
                        dst[x * 3 + 2] = static_cast<uint8_t>(p >> 8) & 0xf8;
                        dst[x * 3] = static_cast<uint8_t>(p << 3);
                        dst[x * 3 + 1] = static_cast<uint8_t>(p >> 3) & 0xfc;
                    }
                    break;
                case D3DFMT_X1R5G5B5:
                case D3DFMT_A1R5G5B5:
                    for (int x = 0; x < width; ++x) {
                        const uint16_t p = p16[x];
                        dst[x * 3 + 2] = static_cast<uint8_t>(p >> 7) & 0xf8;
                        dst[x * 3] = static_cast<uint8_t>(p << 3);
                        dst[x * 3 + 1] = static_cast<uint8_t>(p >> 2) & 0xf8;
                    }
                    break;
                default:
                    break;
                }
                dst += width * 3;
                row -= lr.Pitch;
            }
        } else {
            for (int y = height; y; --y) {
                std::memcpy(dst, row, rowBytes);
                dst += rowBytes;
                row -= lr.Pitch;
            }
        }
        return finish(true);
    }

    FILE *f = std::fopen(fileName, "wb");
    if (!f) {
        dbgPrintf("IGraph::SaveShot() - Cannot create file %s !!!", fileName);
        return finish(false);
    }
    std::fwrite(tga, sizeof tga, 1, f);
    uint8_t *row = static_cast<uint8_t *>(lr.pBits) + (height - 1) * lr.Pitch;
    for (int y = height; y; --y) {
        if (desc.Format == D3DFMT_R5G6B5) {
            // TGA has no 565: converted to 555 in the locked copy.
            uint16_t *p16 = reinterpret_cast<uint16_t *>(row);
            for (int x = width; x--;)
                p16[x] = static_cast<uint16_t>((p16[x] >> 1 & 0x7fe0) | (p16[x] & 0x1f));
        }
        std::fwrite(row, 1, rowBytes, f);
        row -= lr.Pitch;
    }
    std::fclose(f);
    return finish(true);
}

// 0x10070160 (the body is ls3d::ig::ResizeImage, igraph/resize_image.cpp)
bool __stdcall IGraph::ResizeImage(int srcWidth, int srcHeight, int srcPitch, const uint8_t *src, int dstWidth,
                                   int dstHeight, int dstPitch, uint8_t **dst, int bpp, bool filter)
{
    return ig::ResizeImage(srcWidth, srcHeight, srcPitch, src, dstWidth, dstHeight, dstPitch, dst, bpp, filter);
}
