// ITexture (0x10073720 - 0x100775f0): textures from .dds, .bmp / .tga files (with opacity maps,
// colour keys, mip maps, DXT compression and a compressed-file cache next to the source), memory
// images or nothing.
#include "igraph/itexture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "core/enhance.h"
#include "core/i3d_result.h"
#include "external/rw_data.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/mip_filter.h"

using namespace ls3d;
using namespace ls3d::ig;

namespace {

// Pre-transformed vertices (x, y, z, rhw, diffuse, specular, u, v) of Preload (a point), Draw and
// DrawRect (quads drawn as strips); positions and colours are filled in per call
// (0x100a8c00, 0x100a8c20, 0x100a8ca0).
struct S_tlVertex {
    float x, y, z, rhw;
    D3DCOLOR diffuse, specular;
    float u, v;
};
S_tlVertex g_preloadPoint = {0, 0, 0, 1, 0, 0, 0, 0};
S_tlVertex g_drawQuad[4] = {
    {0, 0, 0, 1, 0xffffffff, 0, 0, 0},
    {0, 0, 0, 1, 0xffffffff, 0, 1, 0},
    {0, 0, 0, 1, 0xffffffff, 0, 0, 1},
    {0, 0, 0, 1, 0xffffffff, 0, 1, 1},
};
S_tlVertex g_rectQuad[4] = {
    {0, 0, 0, 1, 0xffffffff, 0, 0, 0},
    {0, 0, 0, 1, 0xffffffff, 0, 1, 0},
    {0, 0, 0, 1, 0xffffffff, 0, 0, 1},
    {0, 0, 0, 1, 0xffffffff, 0, 1, 1},
};

// Versions of the compressed-file caches (0x1009dc30 / 0x1009dc32).
constexpr uint16_t kDxtCacheVersion = 9;
constexpr uint16_t kTexCacheVersion = 1;

// A copy of the part of `path` after its last '\'; nothing for an empty name.
bool CopyBaseName(const char *path, char **out)
{
    const char *slash = std::strrchr(path, '\\');
    const char *base = slash ? slash + 1 : path;
    const size_t len = std::strlen(base);
    if (!len)
        return true;
    char *copy = static_cast<char *>(::operator new(len + 1, std::nothrow));
    *out = copy;
    if (!copy)
        return false;
    std::memcpy(copy, base, len + 1);
    return true;
}

void AddToList(C_vector<ITexture *> &list, ITexture *texture)
{
    list.PushBack(texture);
}

// The texture size for an image: halved with ITC_HALF, scaled down to the device maximum (both
// sides by the same factor), then rounded up to powers of two (at least 4 with DXT support).
void FitTextureSize(uint16_t width, uint16_t height, uint32_t flags, uint16_t *outWidth, uint16_t *outHeight)
{
    uint16_t w = width, h = height;
    if (flags & ITC_HALF) {
        w >>= 1;
        h >>= 1;
    }
    float scaleX = 1.0f;
    if (static_cast<uint32_t>(w) > static_cast<uint32_t>(g_maxTextureWidth))
        scaleX = static_cast<float>(static_cast<uint32_t>(g_maxTextureWidth)) / static_cast<float>(w);
    float scaleY = 1.0f;
    if (static_cast<uint32_t>(h) > static_cast<uint32_t>(g_maxTextureHeight))
        scaleY = static_cast<float>(static_cast<uint32_t>(g_maxTextureHeight)) / static_cast<float>(h);
    const float scale = scaleX < scaleY ? scaleX : scaleY;
    const uint16_t fitW = static_cast<uint16_t>(static_cast<int>(static_cast<float>(w) * scale));
    const uint16_t fitH = static_cast<uint16_t>(static_cast<int>(static_cast<float>(h) * scale));
    const uint32_t minSize = (g_caps & IGCAPS_DXT) ? 4 : 1;
    uint32_t tw = minSize, th = minSize;
    while (fitW > static_cast<uint16_t>(tw))
        tw <<= 1;
    while (fitH > static_cast<uint16_t>(th))
        th <<= 1;
    *outWidth = static_cast<uint16_t>(tw);
    *outHeight = static_cast<uint16_t>(th);
}

// The name of a texture's cache file: the source path without its extension, the opacity map's
// name (DXT3), the level count, 'L' for ITC_HALF, 'G' for the port's improved mip levels and
// ".DX<kind>" / ".565" / ".555".
void CacheFileName(const ITexture *texture, const char *fileName, const char *alphaFileName, int kind,
                   bool dxt, char *out)
{
    char buf[0xff] = {};
    std::strncpy(buf, fileName, std::strlen(fileName) - 4);
    if (dxt && alphaFileName && kind == 3) {
        std::strcat(buf, "_");
        std::strncat(buf, alphaFileName, std::strlen(alphaFileName) - 4);
    }
    char levels[0x20];
    _itoa(texture->levels, levels, 10);
    std::strcat(buf, "_");
    std::strcat(buf, levels);
    if (texture->flags & ITC_HALF)
        std::strcat(buf, "L");
    // (Port enhancement: levels from tex::DownsampleMip are cached apart from the original's.)
    if (Enhancements().mipmaps && texture->levels > 1)
        std::strcat(buf, "G");
    std::strcat(buf, ".");
    const size_t n = std::strlen(buf);
    if (dxt) {
        buf[n] = 'D';
        buf[n + 1] = 'X';
        buf[n + 2] = static_cast<char>('0' + kind);
        buf[n + 3] = 0;
    } else {
        buf[n] = '5';
        buf[n + 1] = texture->format == D3DFMT_R5G6B5 ? '6' : '5';
        buf[n + 2] = '5';
        buf[n + 3] = 0;
    }
    std::strcpy(out, buf);
}

// Reads every level of the texture from an open cache file positioned at its data.
void ReadLevels(ITexture *texture, int file)
{
    texture->memorySize = 0;
    for (int level = 0; level < texture->levels; ++level) {
        IDirect3DSurface8 *surface;
        texture->d3dTexture->GetSurfaceLevel(level, &surface);
        D3DSURFACE_DESC desc;
        surface->GetDesc(&desc);
        texture->memorySize += desc.Size;
        D3DLOCKED_RECT lr;
        surface->LockRect(&lr, nullptr, 0);
        dtaRead(file, lr.pBits, desc.Size);
        surface->UnlockRect();
        surface->Release();
    }
}

// The DXT cache file of an earlier load (0x10076600). The stored source time stamps are read
// but not compared: a cache file is used as long as it has the texture's size.
bool LoadDxtCache(ITexture *texture, const char *fileName, const char *alphaFileName, int kind)
{
    char name[0x100];
    CacheFileName(texture, fileName, alphaFileName, kind, true, name);
    const int file = dtaOpen(name, false);
    if (file == -1)
        return false;
    char magic[4] = {};
    dtaRead(file, magic, 4);
    uint16_t version = 0xffff;
    uint32_t size[2] = {};
    FILETIME times[2];
    if (magic[0] == 'D' && magic[1] == 'X' && magic[2] == 'T' && (dtaRead(file, &version, 2), version >= kDxtCacheVersion)) {
        dtaRead(file, size, 8);
        if (size[0] == texture->width && size[1] == texture->height) {
            dtaRead(file, &times[0], 8);
            if (kind == 3)
                dtaRead(file, &times[1], 8);
            ReadLevels(texture, file);
            dtaClose(file);
            return true;
        }
    }
    dtaClose(file);
    return false;
}

// The 16-bit cache file of an earlier load (0x100768a0).
bool LoadTexCache(ITexture *texture, const char *fileName)
{
    char name[0x100];
    CacheFileName(texture, fileName, nullptr, 0, false, name);
    const int file = dtaOpen(name, false);
    if (file == -1)
        return false;
    char magic[4] = {};
    dtaRead(file, magic, 4);
    uint16_t version = 0xffff;
    uint32_t size[2] = {};
    FILETIME time;
    if (magic[0] == 'T' && magic[1] == 'E' && magic[2] == 'X' && (dtaRead(file, &version, 2), version == kTexCacheVersion)) {
        dtaRead(file, size, 8);
        if (size[0] == texture->width && size[1] == texture->height) {
            dtaRead(file, &time, 8);
            ReadLevels(texture, file);
            dtaClose(file);
            return true;
        }
    }
    dtaClose(file);
    return false;
}

// Writes the texture's levels to a cache file (the end of Create). Failures are only logged.
void WriteCache(ITexture *texture, const char *name, const char magic[4], uint16_t version, const FILETIME *time,
                const FILETIME *alphaTime)
{
    const int file = dtaOpenWrite(name, 0x10000);
    if (file == -1) {
        dbgPrintf("ITexture::Create() - Can't create %s !!!", name);
        return;
    }
    dtaWrite(file, magic, 4);
    dtaWrite(file, &version, 2);
    const uint32_t size[2] = {texture->width, texture->height};
    dtaWrite(file, size, 8);
    dtaWrite(file, time, 8);
    if (alphaTime)
        dtaWrite(file, alphaTime, 8);
    for (int level = 0; level < texture->levels; ++level) {
        IDirect3DSurface8 *surface;
        D3DSURFACE_DESC desc;
        D3DLOCKED_RECT lr;
        if (texture->d3dTexture->GetSurfaceLevel(level, &surface) < 0) {
            dtaClose(file);
            dbgPrintf("ITexture::Create() - Can't create %s !!!", name);
            return;
        }
        if (surface->GetDesc(&desc) < 0 || surface->LockRect(&lr, nullptr, 0) < 0) {
            surface->Release();                 // (the original leaks the surface here)
            dtaClose(file);
            dbgPrintf("ITexture::Create() - Can't create %s !!!", name);
            return;
        }
        dtaWrite(file, lr.pBits, desc.Size);
        surface->UnlockRect();
        surface->Release();
    }
    dtaClose(file);
}

// The 32-bit copy of a DDS level 0 (ITC_KEEP_PIXELS).
void DecodeDdsPixels(D3DFORMAT format, const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height)
{
    uint32_t *out = reinterpret_cast<uint32_t *>(dst);
    const uint16_t *p16 = reinterpret_cast<const uint16_t *>(src);
    switch (format) {
    case D3DFMT_DXT1:
        tex::DecodeDxt1(src, out, width, height);
        break;
    case D3DFMT_DXT3:
        tex::DecodeDxt3(src, out, width, height);
        break;
    case D3DFMT_DXT5:
        tex::DecodeDxt5(src, out, width, height);
        break;
    case D3DFMT_A8R8G8B8:
        std::memcpy(dst, src, width * height * 4);
        break;
    case D3DFMT_A1R5G5B5:
        for (uint32_t i = 0; i < width * height; ++i) {
            const uint32_t p = p16[i];
            out[i] = (((p & 0x7c00) << 3 | (p & 0x3e0)) << 3 | (p & 0x1f)) << 3 | ((p & 0x8000) ? 0xff000000u : 0);
        }
        break;
    case D3DFMT_A4R4G4B4:
        for (uint32_t i = 0; i < width * height; ++i) {
            const uint32_t p = p16[i];
            out[i] = ((((p & 0xfffff000) << 4 | (p & 0xf00)) << 4 | (p & 0xf0)) << 4 | (p & 0xf)) << 4;
        }
        break;
    default:
        break;                                  // (the copy stays as allocated)
    }
}

// A 1-bit mask of a BGRA image: bit i set where pixel i has alpha (ITC_PIXEL_MASK).
uint8_t *MakeAlphaMask(const uint8_t *bgra, int count)
{
    const int bytes = (count + 7) / 8;
    uint8_t *mask = static_cast<uint8_t *>(::operator new(bytes, std::nothrow));
    if (!mask)
        return nullptr;
    std::memset(mask, 0, bytes);
    for (int i = 0; i < count; ++i) {
        if (bgra[i * 4 + 3] >= 1)
            mask[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
    }
    return mask;
}

// The mip filter of Create: a 2x2 block to one pixel with weights 3/8 for the pixels right of and
// below the block's first one, 1/8 for the other two; every term truncated, summed in 8 bits.
// Rows of the destination are half the source row bytes apart (for even widths: its width).
void Downsample(const uint8_t *src, int width, int height, int rowBytes, uint8_t *dst)
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

// Palette entry / pixel in the bits of a 16-bit texture format, for the colour-key compare.
uint32_t KeyColor(D3DFORMAT format, uint8_t r, uint8_t g, uint8_t b, bool *known)
{
    *known = true;
    switch (format) {
    case D3DFMT_A8R8G8B8: return static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | b;
    case D3DFMT_R5G6B5: return ((static_cast<uint32_t>(r & 0xf8) << 5 | (g & 0xfc)) << 3) | (b >> 3);
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_DXT1: return ((static_cast<uint32_t>(r & 0xf8) << 5 | (g & 0xf8)) << 2) | (b >> 3);
    case D3DFMT_A4R4G4B4:
    case D3DFMT_DXT3: return (static_cast<uint32_t>(r & 0xf0) << 4 | (g & 0xf0)) | (b >> 4);
    default: *known = false; return 0;
    }
}

}  // namespace

// 0x10073720 (The original leaves the size, format, color key, level count and stream record
// unset until Create; the port starts them at 0.)
ITexture::ITexture()
    : id(0), pad06(0), d3dTexture(nullptr), lockedSurface(nullptr), loaded(false), refCount(1), fileName(nullptr),
      alphaFileName(nullptr), width(0), height(0), format(D3DFMT_UNKNOWN), flags(0), requestedFlags(0), colorKey(0),
      pixels(nullptr), levels(0), pad3A(0), memorySize(0), stream(nullptr)
{
    pad11[0] = pad11[1] = pad11[2] = 0;
}

// 0x10073760
int __stdcall ITexture::Release()
{
    const int refs = --refCount;
    if (refs < 0) {
        dbgPrintf("!! m_RefCount  ITexture this:0x%x", this);
        ReportError(I3DERR_NOTINITIALIZED, kSevError, "ITexture: m_refCount<0", reinterpret_cast<uintptr_t>(this));
        return 0;
    }
    if (!refs) {
        Close();
        ::operator delete(this);
    }
    return refs;
}

// 0x100737c0
HRESULT __stdcall ITexture::Duplicate(const ITexture *source)
{
    Close();
    if (!source || !source->loaded)
        return 0;
    flags = source->flags;
    requestedFlags = source->requestedFlags;
    if (source->fileName) {
        ::operator delete(fileName);
        const size_t len = std::strlen(source->fileName) + 1;
        fileName = static_cast<char *>(::operator new(len, std::nothrow));
        if (!fileName) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        std::memcpy(fileName, source->fileName, len);
    }
    if (source->alphaFileName) {
        ::operator delete(alphaFileName);
        const size_t len = std::strlen(source->alphaFileName) + 1;
        alphaFileName = static_cast<char *>(::operator new(len, std::nothrow));
        if (!alphaFileName) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        std::memcpy(alphaFileName, source->alphaFileName, len);
    }
    width = source->width;
    height = source->height;
    colorKey = source->colorKey;
    if (source->pixels) {
        const size_t size = (flags & ITC_PIXEL_MASK) ? (width * height + 7) / 8 : static_cast<size_t>(width) * height * 4;
        pixels = static_cast<uint8_t *>(::operator new(size, std::nothrow));
        if (!pixels)
            return I3DERR_OUTOFMEM;
        std::memcpy(pixels, source->pixels, size);
    }
    levels = source->levels;
    memorySize = source->memorySize;
    // (format is not copied, as in the original.)
    D3DSURFACE_DESC desc;
    source->d3dTexture->GetLevelDesc(0, &desc);
    g_pDevice->CreateTexture(width, height, levels, 0, desc.Format, D3DPOOL_MANAGED, &d3dTexture);
    for (int level = levels; level--;) {
        IDirect3DSurface8 *from, *to;
        source->d3dTexture->GetSurfaceLevel(level, &from);
        d3dTexture->GetSurfaceLevel(level, &to);
        g_pDevice->CopyRects(from, nullptr, 0, to, nullptr);
        from->Release();
        to->Release();
    }
    AddToList(g_uncachedTextures, this);
    loaded = true;
    Preload();
    return 0;
}

// 0x10073a60
HRESULT __stdcall ITexture::Reload()
{
    dbgPrintf("ITexture::Reload() - WAS NOT IMPLEMENTED !");
    return I3DERR_UNSUPPORTED;
}

// 0x10073a80
HRESULT __stdcall ITexture::Create(const char *name, const char *alphaName, uint32_t createFlags, int widthParam,
                                   int heightParam, int levelsParam)
{
    Close();
    flags = createFlags;
    requestedFlags = createFlags;

    // ---- streamed: only a record for the texture thread
    if (flags & ITC_STREAMED) {
        if (name && !CopyBaseName(name, &fileName))
            goto out_of_memory;
        if (alphaName && !CopyBaseName(alphaName, &alphaFileName))
            goto out_of_memory;
        if (!tex::StreamAdd(this, name, flags))
            return I3DERR_GENERIC;
        loaded = true;
        AddToList(g_cachedTextures, this);
        return 0;
    }

    // ---- .dds: the file's own format and levels
    if (name) {
        const char *ext = std::strrchr(name, '.');
        if (ext && (ext[1] == 'd' || ext[1] == 'D'))
            return CreateFromDds(name);
    }
    return CreateFromImage(name, alphaName, widthParam, heightParam, levelsParam);

out_of_memory:
    ReportError(I3DERR_OUTOFMEM, kSevFatal);
    return I3DERR_OUTOFMEM;
}

// 0x10076ae0
HRESULT __stdcall ITexture::Close()
{
    Unlock();
    if (flags & ITC_STREAMED) {
        tex::StreamRemove(this);
        stream = nullptr;
        d3dTexture = nullptr;                   // (a staging texture of the thread's pools)
    }
    ::operator delete(pixels);
    pixels = nullptr;
    width = 0;
    height = 0;
    format = D3DFMT_UNKNOWN;
    flags = 0;
    colorKey = 0;
    memorySize = 0;
    ::operator delete(fileName);
    fileName = nullptr;
    ::operator delete(alphaFileName);
    alphaFileName = nullptr;
    g_cachedTextures.RemoveSwap(this);
    g_uncachedTextures.RemoveSwap(this);
    HRESULT refs = 0;
    if (d3dTexture) {
        refs = static_cast<HRESULT>(d3dTexture->Release());
        d3dTexture = nullptr;
    }
    loaded = false;
    return refs;
}

// 0x10076c20
void *__stdcall ITexture::Lock(const RECT *rect, int *pitch, int level)
{
    if (lockedSurface)
        return nullptr;
    d3dTexture->GetSurfaceLevel(level & 0xffff, &lockedSurface);
    D3DLOCKED_RECT lr;
    lockedSurface->LockRect(&lr, rect, 0);
    *pitch = lr.Pitch;
    return lr.pBits;
}

// 0x10076c80
HRESULT __stdcall ITexture::Unlock()
{
    if (lockedSurface) {
        lockedSurface->UnlockRect();
        lockedSurface->Release();
        lockedSurface = nullptr;
    }
    return 0;
}

// 0x10076cb0
HRESULT __stdcall ITexture::Preload()
{
    const bool scene = g_inScene;
    if (!scene)
        g_pDevice->BeginScene();
    if (g_rs.vertexShader != 6) {
        g_rs.vertexShader = 6;
        g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1);
    }
    g_pDevice->SetTexture(0, d3dTexture);
    g_pDevice->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &g_preloadPoint, sizeof(S_tlVertex));
    if (!scene)
        g_pDevice->EndScene();
    return 0;
}

// 0x10076d90
bool __stdcall ITexture::Fill(const RECT *rect, D3DCOLOR color, int level)
{
    D3DSURFACE_DESC desc;
    if (d3dTexture->GetLevelDesc(level & 0xffff, &desc) < 0)
        return false;
    int pitch;
    uint8_t *bits = static_cast<uint8_t *>(Lock(rect, &pitch, level));
    if (!bits)
        return false;
    const uint32_t w = rect ? rect->right - rect->left : desc.Width;
    const uint32_t h = rect ? rect->bottom - rect->top : desc.Height;
    switch (format) {
    case D3DFMT_R8G8B8:
        // (Written R, G, B: the reverse of the format's byte order, as in the original.)
        for (uint32_t y = 0; y < h; ++y, bits += pitch) {
            for (uint32_t x = 0; x < w; ++x) {
                bits[x * 3] = static_cast<uint8_t>(color >> 16);
                bits[x * 3 + 1] = static_cast<uint8_t>(color >> 8);
                bits[x * 3 + 2] = static_cast<uint8_t>(color);
            }
        }
        break;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        for (uint32_t y = 0; y < h; ++y, bits += pitch) {
            for (uint32_t x = 0; x < w; ++x)
                reinterpret_cast<uint32_t *>(bits)[x] = color;
        }
        break;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
        // (The low 16 bits of the colour as they are.)
        for (uint32_t y = 0; y < h; ++y, bits += pitch) {
            for (uint32_t x = 0; x < w; ++x)
                reinterpret_cast<uint16_t *>(bits)[x] = static_cast<uint16_t>(color);
        }
        break;
    case D3DFMT_L8:
        // The original writes red, green and blue into three planes of desc.Width * desc.Height
        // bytes, the last two past the end of the 8-bit surface; the port writes the first only.
        for (uint32_t i = 0; i < w * h; ++i)
            bits[i] = static_cast<uint8_t>(color >> 16);
        break;
    default:
        Unlock();
        return false;
    }
    Unlock();
    return true;
}

// 0x10077160
bool __stdcall ITexture::DrawRect(int left, int top, int right, int bottom, D3DCOLOR topLeft, D3DCOLOR topRight,
                                  D3DCOLOR bottomLeft, D3DCOLOR bottomRight)
{
    const RECT screen = {0, 0, g_pIGraph->ScrnX(), g_pIGraph->ScrnY()};
    const RECT rc = {left, top, right, bottom};
    RECT visible;
    if (!IntersectRect(&visible, &rc, &screen))
        return false;
    g_rectQuad[0].x = static_cast<float>(left);
    g_rectQuad[0].y = static_cast<float>(top);
    g_rectQuad[0].diffuse = topLeft;
    g_rectQuad[1].x = static_cast<float>(right);
    g_rectQuad[1].y = g_rectQuad[0].y;
    g_rectQuad[1].diffuse = topRight;
    g_rectQuad[2].x = g_rectQuad[0].x;
    g_rectQuad[2].y = static_cast<float>(bottom);
    g_rectQuad[2].diffuse = bottomLeft;
    g_rectQuad[3].x = g_rectQuad[1].x;
    g_rectQuad[3].y = g_rectQuad[2].y;
    g_rectQuad[3].diffuse = bottomRight;
    g_pIGraph->SetTexture(this, 0);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    if (g_pIGraph->DrawPrimitive(IGP_TRIANGLESTRIP, 2, g_rectQuad, IGV_TRANSFORMED) < 0)
        return false;
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    return true;
}

// 0x10076fe0
bool __stdcall ITexture::Draw(int x, int y)
{
    const RECT rc = {x, y, x + width, y + height};
    const RECT screen = {0, 0, g_pIGraph->ScrnX(), g_pIGraph->ScrnY()};
    RECT visible;
    if (!IntersectRect(&visible, &rc, &screen))
        return false;
    g_drawQuad[0].x = static_cast<float>(x);
    g_drawQuad[0].y = static_cast<float>(y);
    g_drawQuad[1].x = static_cast<float>(width) + g_drawQuad[0].x;
    g_drawQuad[1].y = g_drawQuad[0].y;
    g_drawQuad[2].x = g_drawQuad[0].x;
    g_drawQuad[2].y = static_cast<float>(height) + g_drawQuad[0].y;
    g_drawQuad[3].x = static_cast<float>(width) + g_drawQuad[0].x;
    g_drawQuad[3].y = static_cast<float>(height) + g_drawQuad[0].y;
    g_pIGraph->SetTexture(this, 0);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    if (g_pIGraph->DrawPrimitive(IGP_TRIANGLESTRIP, 2, g_drawQuad, IGV_TRANSFORMED) < 0)
        return false;
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    return true;
}

// 0x100772e0
bool __stdcall ITexture::SaveToTGA(const char *name)
{
    if (format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 || format == D3DFMT_DXT4 ||
        format == D3DFMT_DXT5) {
        dbgPrintf("!! ITexture::SaveToTGA() - Cannot create file %s  Compresed pixel format isn't supported", name);
        return false;
    }
    uint8_t header[0x12] = {};
    header[2] = 2;
    switch (format) {
    case D3DFMT_R8G8B8:
    case D3DFMT_L8: header[0x10] = 24; break;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A4R4G4B4: header[0x10] = 32; break;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: header[0x10] = 16; break;
    default: break;
    }
    const int16_t w = static_cast<int16_t>(width), h = static_cast<int16_t>(height);
    std::memcpy(header + 0xc, &w, 2);
    std::memcpy(header + 0xe, &h, 2);
    const int file = dtaOpenWrite(name, 0x10000);
    if (file < 0) {
        dbgPrintf("!! ITexture::SaveToTGA() - Cannot create file %s", name);
        return false;
    }
    dtaWrite(file, header, sizeof header);
    int pitch;
    const uint8_t *bits = static_cast<const uint8_t *>(Lock(nullptr, &pitch, 0));
    if (!bits) {
        dbgPrintf("!! ITexture::SaveToTGA() - Cannot lock texture");
        dtaClose(file);                         // (left open by the original)
        return false;
    }
    const int rowBytes = (static_cast<int8_t>(header[0x10]) / 8) * w;
    const uint8_t *row = bits + (h - 1) * pitch;
    if (format == D3DFMT_R5G6B5) {
        uint16_t *line = static_cast<uint16_t *>(::operator new(w * 2));
        for (int y = h; y; --y, row -= pitch) {
            const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
            for (int x = w; x--;)
                line[x] = static_cast<uint16_t>((p[x] >> 1 & 0x7fe0) | (p[x] & 0x1f));
            dtaWrite(file, line, rowBytes);
        }
        ::operator delete(line);
    } else if (format == D3DFMT_A4R4G4B4) {
        uint32_t *line = static_cast<uint32_t *>(::operator new(w * 4));
        for (int y = h; y; --y, row -= pitch) {
            const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
            for (int x = w; x--;) {
                const uint32_t v = p[x];
                line[x] = ((((v & 0xfffff000) << 4 | (v & 0xf00)) << 4 | (v & 0xf0)) << 4 | (v & 0xf)) << 4;
            }
            dtaWrite(file, line, rowBytes);
        }
        ::operator delete(line);
    } else {
        for (int y = h; y; --y, row -= pitch)
            dtaWrite(file, row, rowBytes);
    }
    dtaClose(file);
    Unlock();
    return true;
}

// ---------------------------------------------------------------- Create, .dds

HRESULT ITexture::CreateFromDds(const char *name)
{
    if (!CopyBaseName(name, &fileName)) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return I3DERR_OUTOFMEM;
    }
    const int file = dtaOpen(name, false);
    if (file < 0)
        return I3DERR_NOFILE;
    // "DDS " + DDSURFACEDESC2.
    uint32_t dds[0x20];
    dtaRead(file, dds, 0x80);
    if (dds[0] != 0x20534444) {
        dtaClose(file);
        return I3DERR_FILECORRUPTED;
    }
    const uint32_t ddsFlags = dds[2], ddsHeight = dds[3], ddsWidth = dds[4], mipCount = dds[7];
    const uint32_t pfFlags = dds[20], fourCC = dds[21], bitCount = dds[22], gMask = dds[24], aMask = dds[26];
    uint32_t f = flags & 0xfffe197f;
    flags = f;
    width = static_cast<uint16_t>(ddsWidth);
    height = static_cast<uint16_t>(ddsHeight);
    if (ddsFlags & 0x20000) {                   // DDSD_MIPMAPCOUNT
        levels = static_cast<uint16_t>(mipCount);
        f |= ITC_MIPMAP;
    } else {
        levels = 1;
        f &= ~ITC_MIPMAP;
    }
    flags = f;
    colorKey = 0;
    if ((f & ITC_ALPHA_1BIT) && (f & ITC_KEEP_PIXELS))
        flags = f |= ITC_PIXEL_MASK;
    auto unsupported = [&]() {
        dtaClose(file);
        return I3DERR_BADTEXTUREFORMAT;
    };
    if (pfFlags & 0x80000) {                    // DDPF_BUMPDUDV
        if (bitCount != 16 || gMask != 0xff00)
            return unsupported();
        format = g_fmtBump;
    } else if (pfFlags & 0x40) {                // DDPF_RGB
        if (bitCount == 32) {
            flags |= ITC_TRUECOLOR;
            format = g_fmtTrueColor;
        } else if (bitCount == 16) {
            if (aMask == 0) {
                flags &= ~(ITC_SOURCE_ALPHA | ITC_USE_ALPHA | ITC_ALPHA_1BIT);
                if (gMask == 0x7e0) {
                    format = g_fmt16Bit;
                } else if (gMask == 0xff00) {
                    flags &= 0xfeffeff7;
                    format = g_fmtBump;
                } else {
                    format = g_fmt1BitAlpha;
                }
            } else if (aMask == 0x8000) {
                format = g_fmt1BitAlpha;
            } else if (aMask == 0xf000) {
                flags &= ~ITC_ALPHA_1BIT;
                format = g_fmtAlpha;
            } else {
                return unsupported();
            }
        } else {
            return unsupported();
        }
    } else if (pfFlags & 0x4) {                 // DDPF_FOURCC
        flags |= ITC_COMPRESS;
        if (fourCC == D3DFMT_DXT1) {
            format = g_fmtDxt1;
        } else if (fourCC == D3DFMT_DXT3) {
            flags &= ~ITC_ALPHA_1BIT;
            format = g_fmtDxt3;
        } else if (fourCC == D3DFMT_DXT5) {
            flags &= ~ITC_ALPHA_1BIT;
            format = g_fmtDxt5;
        } else {
            return unsupported();
        }
    } else {
        return unsupported();
    }
    if (g_pDevice->CreateTexture(width, height, levels, 0, format, D3DPOOL_MANAGED, &d3dTexture) < 0) {
        dtaClose(file);
        return I3DERR_GENERIC;
    }
    memorySize = 0;
    for (int level = 0; level < levels; ++level) {
        IDirect3DSurface8 *surface;
        D3DSURFACE_DESC desc;
        D3DLOCKED_RECT lr;
        if (d3dTexture->GetSurfaceLevel(level, &surface) < 0) {
            dtaClose(file);
            Close();
            return I3DERR_GENERIC;
        }
        if (surface->GetDesc(&desc) < 0 || surface->LockRect(&lr, nullptr, 0) < 0) {
            surface->Release();                 // (leaked by the original)
            dtaClose(file);
            Close();
            return I3DERR_GENERIC;
        }
        memorySize += desc.Size;
        dtaRead(file, lr.pBits, desc.Size);
        if (level == 0 && (flags & ITC_KEEP_PIXELS)) {
            pixels = static_cast<uint8_t *>(::operator new(static_cast<size_t>(width) * height * 4, std::nothrow));
            if (!pixels) {
                surface->UnlockRect();
                surface->Release();
                dtaClose(file);
                Close();
                return I3DERR_OUTOFMEM;
            }
            std::memset(pixels, 0, static_cast<size_t>(width) * height * 4);   // (uninitialised for other formats)
            DecodeDdsPixels(format, static_cast<const uint8_t *>(lr.pBits), pixels, width, height);
            if (flags & ITC_PIXEL_MASK) {
                uint8_t *mask = MakeAlphaMask(pixels, width * height);
                if (!mask) {
                    surface->UnlockRect();
                    surface->Release();
                    dtaClose(file);
                    Close();
                    return I3DERR_OUTOFMEM;
                }
                ::operator delete(pixels);
                pixels = mask;
            }
        }
        surface->UnlockRect();
        surface->Release();
    }
    dtaClose(file);
    AddToList((flags & ITC_NOCACHE) ? g_uncachedTextures : g_cachedTextures, this);
    loaded = true;
    Preload();
    return 0;
}

// ---------------------------------------------------------------- Create, images

HRESULT ITexture::CreateFromImage(const char *name, const char *alphaName, int widthParam, int heightParam,
                                  int levelsParam)
{
    // The loaded images (globals of the original, 0x100d2600 / 0x100d2b80 / 0x100d2bb8 ...).
    BITMAPINFOHEADER info = {};                 // .biXPelsPerMeter holds the row pitch
    BITMAPINFOHEADER alphaInfo = {};
    uint8_t *bits = nullptr;
    uint32_t *palette = nullptr;
    uint8_t *alphaBits = nullptr;
    uint32_t *alphaPalette = nullptr;
    uint8_t *gray = nullptr;                    // the opacity map, 8 bits per pixel
    int grayBack = 0;                           // bytes back from the end of a gray row to the previous one
    int rowBack = 0;                            // the same for the image rows
    FILETIME fileTime = {}, alphaTime = {};
    auto freeImages = [&]() {
        ::operator delete(bits);
        ::operator delete(palette);
        ::operator delete(alphaBits);
        ::operator delete(alphaPalette);
        ::operator delete(gray);
        bits = alphaBits = gray = nullptr;
        palette = alphaPalette = nullptr;
    };

    if (flags & ITC_FROM_MEMORY) {
        flags |= ITC_NOCACHE;
        if (!name)
            return I3DERR_INVALIDPARAMS;
        const BITMAPINFOHEADER *src = static_cast<const BITMAPINFOHEADER *>(static_cast<const void *>(name));
        width = static_cast<uint16_t>(static_cast<uint16_t>(widthParam) ? widthParam : src->biWidth);
        height = static_cast<uint16_t>(static_cast<uint16_t>(heightParam) ? heightParam : src->biHeight);
        FitTextureSize(width, height, flags, &width, &height);
        if (!tex::LoadImage(name, 0, 0, flags, &bits, &palette, &info, true, nullptr, -1))
            return I3DERR_FILECORRUPTED;
    } else if (name) {
        flags &= ~ITC_EMPTY;
        if (!CopyBaseName(name, &fileName)) {
            ReportError(I3DERR_OUTOFMEM, kSevFatal);
            return I3DERR_OUTOFMEM;
        }
        int handle = -1;
        uint8_t bitDepth = 0;
        const HRESULT hr = g_pIGraph->GetImageInfo(name, &width, &height, &bitDepth, 0, &handle);
        if (hr < 0) {
            Close();
            return hr;
        }
        if (flags & ITC_SOURCE_ALPHA) {
            if (bitDepth == 8 || bitDepth == 24)
                flags &= ~ITC_SOURCE_ALPHA;
            if (bitDepth == 16)
                flags |= ITC_ALPHA_1BIT;
        }
        if (static_cast<uint16_t>(widthParam))
            width = static_cast<uint16_t>(widthParam);
        if (static_cast<uint16_t>(heightParam))
            height = static_cast<uint16_t>(heightParam);
        if (!(flags & ITC_NORESIZE))
            FitTextureSize(width, height, flags, &width, &height);
        if (!tex::LoadImage(name, 0, 0, flags, &bits, &palette, &info, false, &fileTime, handle)) {
            Close();
            return I3DERR_FILECORRUPTED;
        }
    } else {
        if (!(flags & ITC_EMPTY)) {
            dbgPrintf("ITexture::Create() - Warning! Neither Texture nor Alpha was specified, but ITC_EMPTY wasn't used!");
            flags |= ITC_EMPTY;
        }
        width = static_cast<uint16_t>(widthParam);
        height = static_cast<uint16_t>(heightParam);
        if (!(flags & ITC_NORESIZE))
            FitTextureSize(width, height, flags, &width, &height);
    }
    rowBack = (info.biBitCount >> 3) * info.biWidth + info.biXPelsPerMeter;

    // ---- the opacity map, as 8-bit gray
    if (alphaName && *alphaName && !(flags & ITC_USE_ALPHA)) {
        dbgPrintf("ITexture::Create() - Warning! AlphaName was specified, but ITC_USE_ALPHA wasn't! (%s, %s)", name,
                  alphaName);
        flags |= ITC_USE_ALPHA;
    }
    if (flags & ITC_USE_ALPHA) {
        if (alphaName) {
            if (!CopyBaseName(alphaName, &alphaFileName)) {
                ReportError(I3DERR_OUTOFMEM, kSevFatal);
                return I3DERR_OUTOFMEM;
            }
            if (!tex::LoadImage(alphaName, info.biWidth, info.biHeight, flags, &alphaBits, &alphaPalette, &alphaInfo,
                                false, &alphaTime, -1)) {
                Close();
                freeImages();
                return I3DERR_NOADDITIONALFILE;
            }
            grayBack = alphaInfo.biXPelsPerMeter * 2;
            gray = static_cast<uint8_t *>(::operator new(alphaInfo.biHeight * alphaInfo.biXPelsPerMeter, std::nothrow));
            if (!gray) {
                Close();
                freeImages();
                return I3DERR_OUTOFMEM;
            }
            // Luminance (77 R + 154 G + 25 B) / 256, read without row padding.
            auto luma = [](uint8_t r, uint8_t g, uint8_t b) {
                return static_cast<uint8_t>(((r + g * 2) * 0x4d + b * 0x19) >> 8);
            };
            const int count = alphaInfo.biWidth * alphaInfo.biHeight;
            switch (alphaInfo.biBitCount >> 3) {
            case 1: {
                const uint8_t *pal = reinterpret_cast<const uint8_t *>(alphaPalette);
                for (int i = 0; i < count; ++i) {
                    const uint8_t *e = pal + alphaBits[i] * 4;
                    gray[i] = luma(e[2], e[1], e[0]);
                }
                break;
            }
            case 3:
                for (int i = 0; i < count; ++i)
                    gray[i] = luma(alphaBits[i * 3 + 2], alphaBits[i * 3 + 1], alphaBits[i * 3]);
                break;
            default:
                // (The message shows the colour image's numbers, as in the original.)
                dbgPrintf("IImage::Create() - Unsuppoted ALPHA source file %i bit [ %i(%i), %i ] format. Only 8,24 bit "
                          "are supported ! : %s",
                          info.biBitCount, info.biWidth, info.biBitCount * info.biWidth / 8, info.biHeight, alphaName);
                Close();
                freeImages();
                return I3DERR_BADTEXTUREFORMAT;
            }
            alphaInfo.biBitCount = 8;
            alphaInfo.biXPelsPerMeter = alphaInfo.biWidth;
            grayBack = alphaInfo.biWidth * 2;
            ::operator delete(alphaBits);
            alphaBits = nullptr;
            ::operator delete(alphaPalette);
            alphaPalette = nullptr;
        } else if (!(flags & ITC_EMPTY)) {
            flags &= ~ITC_USE_ALPHA;
            dbgPrintf("ITexture::Create() - Warning! ITC_USE_ALPHA specified, but ( AlphaName == NULL ) !!  %s", name);
        }
    }

    // (Port enhancement, LS3DF.ini TrueColorTextures: a texture from an image file in A8R8G8B8.
    // Not empty or memory textures: their owners may write pixels in the format they asked for.)
    if (Enhancements().trueColorTextures && name && !(flags & ITC_FROM_MEMORY))
        flags |= ITC_TRUECOLOR;

    // ---- the texture format; `kind`: 0 uncompressed, 1 DXT1, 2 DXT1 with 1-bit alpha, 3 DXT3
    D3DFORMAT staging = D3DFMT_UNKNOWN;         // the uncompressed format compressed from
    int kind = 0;
    D3DFORMAT fmt;
    auto alphaFormat = [&]() {
        if ((flags & ITC_COMPRESS) && g_fmtDxt3) {
            staging = g_fmtAlpha;
            kind = 3;
            return g_fmtDxt3;
        }
        return g_fmtAlpha;
    };
    if ((flags & ITC_TRUECOLOR) && g_fmtTrueColor) {
        fmt = g_fmtTrueColor;
    } else {
        flags &= ~ITC_TRUECOLOR;
        const bool oneBit = (flags & ITC_ALPHA_1BIT) != 0;
        if ((flags & (ITC_SOURCE_ALPHA | ITC_USE_ALPHA)) && !oneBit) {
            fmt = alphaFormat();
        } else if (oneBit) {
            if (flags & ITC_MIPMAP) {
                fmt = alphaFormat();            // filtered levels need more than one alpha bit
            } else if ((flags & ITC_COMPRESS) && g_fmtDxt1) {
                staging = g_fmt1BitAlpha;
                kind = 2;
                fmt = g_fmtDxt1;
            } else {
                fmt = g_fmt1BitAlpha;
            }
        } else if ((flags & ITC_COMPRESS) && g_fmtDxt1) {
            staging = g_fmt1BitAlpha;
            kind = 1;
            fmt = g_fmtDxt1;
        } else {
            fmt = g_fmt16Bit;
        }
    }
    format = fmt;
    const int wantLevels = static_cast<uint16_t>((flags & ITC_MIPMAP) ? levelsParam : 1);
    // (The original does not check this call.)
    if (g_pDevice->CreateTexture(width, height, wantLevels, 0, fmt, D3DPOOL_MANAGED, &d3dTexture) < 0 || !d3dTexture) {
        Close();
        freeImages();
        return I3DERR_GENERIC;
    }
    levels = static_cast<uint16_t>(d3dTexture->GetLevelCount());

    auto finishFromCache = [&]() {
        loaded = true;
        AddToList((flags & ITC_NOCACHE) ? g_uncachedTextures : g_cachedTextures, this);
        ::operator delete(bits);
        ::operator delete(palette);
        bits = nullptr;
        palette = nullptr;
        Preload();
        return 0;
    };
    // An earlier load's compressed / converted levels. (The DXT3 file name uses the opacity map's
    // path here and only its name below, as in the original.)
    if (!(flags & (ITC_FROM_MEMORY | ITC_KEEP_PIXELS | ITC_EMPTY))) {
        bool cached = false;
        if (kind == 1)
            cached = LoadDxtCache(this, name, nullptr, 1);
        else if (kind > 1)
            cached = LoadDxtCache(this, name, alphaName, kind);
        else if (fmt == D3DFMT_R5G6B5 || fmt == D3DFMT_X1R5G5B5 || fmt == D3DFMT_A1R5G5B5)
            cached = LoadTexCache(this, name);
        if (cached) {
            ::operator delete(gray);            // (leaked by the original)
            return finishFromCache();
        }
    }

    // ---- the image as 32-bit BGRA, top row first (the sources are bottom-up)
    if (!(flags & ITC_EMPTY)) {
        pixels = static_cast<uint8_t *>(::operator new(static_cast<size_t>(info.biHeight) * info.biWidth * 4,
                                                       std::nothrow));
        if (!pixels) {
            Close();
            freeImages();
            return I3DERR_OUTOFMEM;
        }
        colorKey = 0;
        const int w = info.biWidth, h = info.biHeight;
        const int pitch = info.biXPelsPerMeter;
        uint8_t *out = pixels;
        const uint8_t *alpha = gray ? gray + (alphaInfo.biHeight - 1) * alphaInfo.biWidth : nullptr;
        switch (info.biBitCount) {
        case 8: {
            const uint8_t *pal = reinterpret_cast<const uint8_t *>(palette);
            bool known;
            const uint32_t key = KeyColor(fmt, pal[2], pal[1], pal[0], &known);
            if (known)
                colorKey = key;
            const uint8_t *src = bits + (h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, ++src, out += 4) {
                    const uint8_t *e = pal + *src * 4;
                    out[0] = e[0];
                    out[1] = e[1];
                    out[2] = e[2];
                    if (alpha) {
                        out[3] = *alpha++;
                    } else if (flags & ITC_ALPHA_1BIT) {
                        const uint32_t c = KeyColor(fmt, e[2], e[1], e[0], &known);
                        out[3] = c == colorKey ? 0 : 0xff;
                    } else {
                        out[3] = 0xff;
                    }
                }
                src -= rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        case 16: {
            const uint8_t *src = bits + (h - 1) * pitch;
            for (int y = h; y; --y) {
                const uint16_t *p = reinterpret_cast<const uint16_t *>(src);
                for (int x = 0; x < w; ++x, out += 4) {
                    const uint16_t c = p[x];
                    out[0] = static_cast<uint8_t>(c << 3);
                    out[1] = static_cast<uint8_t>(c >> 2) & 0xf8;
                    out[2] = static_cast<uint8_t>(c >> 7) & 0xf8;
                    if (flags & ITC_SOURCE_ALPHA)
                        out[3] = (c & 0x8000) ? 0xff : 0;
                    else if (alpha)
                        out[3] = *alpha++;
                    else if (flags & ITC_ALPHA_1BIT)
                        out[3] = c == colorKey ? 0 : 0xff;
                    else
                        out[3] = 0xff;
                }
                src += w * 2 - rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        case 24: {
            const uint8_t *src = bits + (h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, src += 3, out += 4) {
                    out[0] = src[0];
                    out[1] = src[1];
                    out[2] = src[2];
                    if (alpha)
                        out[3] = *alpha++;
                    else if (flags & ITC_ALPHA_1BIT)
                        out[3] = src[0] == colorKey ? 0 : 0xff;   // (the blue byte against the key)
                    else
                        out[3] = 0xff;
                }
                src -= rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        case 32: {
            const uint8_t *src = bits + (h - 1) * pitch;
            for (int y = h; y; --y) {
                for (int x = 0; x < w; ++x, src += 4, out += 4) {
                    out[0] = src[0];
                    out[1] = src[1];
                    out[2] = src[2];
                    if (flags & ITC_SOURCE_ALPHA)
                        out[3] = src[3];
                    else if (alpha)
                        out[3] = *alpha++;
                    else if (flags & ITC_ALPHA_1BIT)
                        out[3] = src[0] == colorKey ? 0 : 0xff;
                    else
                        out[3] = 0xff;
                }
                src -= rowBack;
                if (alpha)
                    alpha -= grayBack;
            }
            break;
        }
        default:
            dbgPrintf("IImage::Create() - Unsuppoted source file %i bit [ %i(%i), %i ] format. Only 8,16,24,32 bit are "
                      "supported ! : %s",
                      info.biBitCount, w, info.biBitCount * w / 8, h, name);
            Close();
            freeImages();
            return I3DERR_BADTEXTUREFORMAT;
        }
        freeImages();
        // Scaled to the texture size (box filter).
        if (width != info.biWidth || height != info.biHeight) {
            uint8_t *resized = nullptr;
            g_pIGraph->ResizeImage(info.biWidth, info.biHeight, info.biWidth * 4, pixels, width, height, width * 4,
                                   &resized, 4, true);
            ::operator delete(pixels);
            pixels = resized;
        }
    }

    // A compressed file of an earlier load, now with the opacity map's name. (For an empty
    // texture the original would take strlen of the missing name.)
    if (!(flags & ITC_FROM_MEMORY) && kind >= 1 && name && LoadDxtCache(this, name, alphaFileName, kind)) {
        loaded = true;
        AddToList((flags & ITC_NOCACHE) ? g_uncachedTextures : g_cachedTextures, this);
        Preload();
        return 0;
    }

    // ---- the levels: converted (and compressed through a staging texture), mip maps filtered
    int rowBytes = width * 4;
    int levelW = width, levelH = height;
    uint8_t *cur = nullptr, *next = nullptr;
    if (!(flags & ITC_EMPTY) && levels > 1) {
        cur = static_cast<uint8_t *>(::operator new(static_cast<size_t>(width) * height * 4, std::nothrow));
        if (!cur) {
            Close();
            return I3DERR_GENERIC;
        }
        std::memcpy(cur, pixels, static_cast<size_t>(width) * height * 4);
        // (Uninitialised in the original: the levels it does not compute.)
        next = static_cast<uint8_t *>(::operator new(static_cast<size_t>(width) * height * 2, std::nothrow));
        if (!next) {
            Close();
            ::operator delete(cur);
            return I3DERR_OUTOFMEM;
        }
        std::memset(next, 0, static_cast<size_t>(width) * height * 2);
    }
    // (Port enhancement, LS3DF.ini ImprovedMipmaps: the levels from tex::DownsampleMip; those of
    // a colour-keyed texture, drawn alpha-tested, uploaded with their alpha rescaled to level 0's
    // coverage at IGraph's reference, from a copy: the next level is made from the unscaled one.)
    const bool improvedMips = Enhancements().mipmaps && cur;
    const int coverageRef = improvedMips && (flags & ITC_ALPHA_1BIT) && g_rs.alphaRef > 0
                                ? (g_rs.alphaRef < 255 ? g_rs.alphaRef : 255)
                                : 0;
    const double coverage = coverageRef ? tex::AlphaCoverage(pixels, width, height, coverageRef) : 0.0;
    uint8_t *scaled = nullptr;
    if (coverageRef) {
        scaled = static_cast<uint8_t *>(::operator new(static_cast<size_t>(width) * height * 2, std::nothrow));
        if (!scaled) {
            Close();
            ::operator delete(cur);
            ::operator delete(next);
            return I3DERR_OUTOFMEM;
        }
    }
    IDirect3DTexture8 *stage = nullptr;
    auto fail = [&](HRESULT hr) {
        if (stage)
            stage->Release();
        Close();
        ::operator delete(cur);
        ::operator delete(next);
        ::operator delete(scaled);
        return hr;
    };
    if (!(flags & ITC_EMPTY) && staging) {
        if (g_pDevice->CreateTexture(width, height, wantLevels, 0, staging, D3DPOOL_MANAGED, &stage) < 0)
            return fail(I3DERR_GENERIC);
    }
    for (int level = 0; level < levels; ++level) {
        IDirect3DSurface8 *surface;
        if (d3dTexture->GetSurfaceLevel(level, &surface) < 0)
            return fail(I3DERR_GENERIC);
        D3DSURFACE_DESC desc;
        if (surface->GetDesc(&desc) < 0) {
            surface->Release();
            return fail(I3DERR_GENERIC);
        }
        memorySize += desc.Size;
        if (!(flags & ITC_EMPTY)) {
            const uint8_t *image = pixels;
            if (level) {
                if (improvedMips)
                    tex::DownsampleMip(cur, levelW, levelH, next);
                else
                    Downsample(cur, levelW, levelH, rowBytes, next);
                levelW = levelW >> 1 ? levelW >> 1 : 1;
                levelH = levelH >> 1 ? levelH >> 1 : 1;
                rowBytes >>= 1;
                image = next;
                if (scaled) {
                    std::memcpy(scaled, next, static_cast<size_t>(levelW) * levelH * 4);
                    tex::ScaleAlphaToCoverage(scaled, levelW, levelH, coverageRef, coverage);
                    image = scaled;
                }
            }
            bool ok;
            if (stage) {
                IDirect3DSurface8 *stageSurface;
                if (stage->GetSurfaceLevel(level, &stageSurface) < 0) {
                    surface->Release();
                    return fail(I3DERR_GENERIC);
                }
                ok = tex::WriteSurface(this, image, stageSurface, levelW, levelH, 0);
                tex::CompressSurface(name, kind, stageSurface, surface);
                stageSurface->Release();
            } else {
                ok = tex::WriteSurface(this, image, surface, levelW, levelH, kind);
            }
            if (level) {
                uint8_t *t = cur;
                cur = next;
                next = t;
            }
            if (!ok) {
                surface->Release();
                return fail(I3DERR_GENERIC);
            }
        }
        surface->Release();
    }

    if (flags & ITC_EMPTY) {
        if (flags & ITC_CLEAR)
            Fill(nullptr, 0, 0);
        AddToList(g_uncachedTextures, this);
        loaded = true;
        return 0;
    }
    if (!(flags & ITC_KEEP_PIXELS)) {
        ::operator delete(pixels);
        pixels = nullptr;
    }
    if (stage)
        stage->Release();
    ::operator delete(cur);
    ::operator delete(next);
    ::operator delete(scaled);
    AddToList((flags & ITC_NOCACHE) ? g_uncachedTextures : g_cachedTextures, this);
    loaded = true;
    Preload();

    // Cache files for the next load.
    if (!(flags & ITC_FROM_MEMORY)) {
        char cacheName[0x100];
        if (kind >= 1) {
            CacheFileName(this, name, alphaFileName, kind, true, cacheName);
            WriteCache(this, cacheName, "DXT", kDxtCacheVersion, &fileTime, fmt == D3DFMT_DXT3 ? &alphaTime : nullptr);
        } else if (fmt == D3DFMT_R5G6B5 || fmt == D3DFMT_X1R5G5B5 || fmt == D3DFMT_A1R5G5B5) {
            CacheFileName(this, name, nullptr, 0, false, cacheName);
            WriteCache(this, cacheName, "TEX", kTexCacheVersion, &fileTime, nullptr);
        }
    }
    return 0;
}
