// ITexture: a Direct3D texture created from image files (BMP, TGA, DDS), memory or nothing, with
// the conversion options of its flags (vtable 0x1009dc34, 0x44 bytes; ITexture.cpp region
// 0x10073720 - 0x1007a100).
#pragma once

#include <cstdint>

#include "dx/d3d8.h"

// ITexture::Create / IGraph::LoadTexture flags ("ITC_*" in the original's messages).
enum : uint32_t {
    ITC_FROM_MEMORY = 0x00000001,       // the name points at a BITMAPINFOHEADER image in memory
    ITC_SOURCE_ALPHA = 0x00000002,      // use the image's own alpha (32-bit; 16-bit: its top bit)
    ITC_USE_ALPHA = 0x00000004,         // a separate opacity map (alpha file name)
    ITC_ALPHA_1BIT = 0x00000008,        // 1-bit alpha (colour key: palette entry 0 / black)
    ITC_STREAMED = 0x00000040,          // loaded on demand by the texture thread
    ITC_DITHER = 0x00000080,            // Floyd-Steinberg dithering into 16-bit formats
    ITC_MIPMAP = 0x00000100,
    ITC_COMPRESS = 0x00000200,          // DXT, when the device has it
    ITC_TRUECOLOR = 0x00000400,         // A8R8G8B8
    ITC_KEEP_PIXELS = 0x00001000,       // keep the 32-bit copy of level 0 (pixels)
    ITC_HALF = 0x00002000,              // half the image size
    ITC_CLEAR = 0x00004000,             // ITC_EMPTY: fill with 0
    ITC_EMPTY = 0x00008000,             // no image: an empty width x height texture
    ITC_DEFAULT = 0x00010000,           // IGraph's 8 x 8 white texture (LoadTexture)
    ITC_BMP_UNPADDED = 0x00040000,      // BMP rows without the 4-byte padding
    ITC_NOCACHE = 0x00200000,           // not shared through IGraph's cached texture list
    ITC_NORESIZE = 0x00400000,          // keep the image size (no power of 2, no maximum)
    ITC_PIXEL_MASK = 0x01000000,        // (internal) pixels is a 1-bit mask of the alpha
    ITC_QUEUED = 0x40000000,            // (internal) ITC_STREAMED: requested from the thread
    ITC_RESIDENT = 0x80000000,          // (internal) ITC_STREAMED: holds a staging texture
    // Options a .dds file does not take (LoadTexture clears them).
    ITC_DDS_IGNORED = ITC_DITHER | ITC_MIPMAP | ITC_COMPRESS | ITC_TRUECOLOR | ITC_HALF,
};

namespace ls3d::tex {
struct S_streamRecord;
}

class ITexture {
public:
    // 0x00 0x10073760: drops a reference; the last one closes and frees the texture.
    virtual int __stdcall Release();
    // 0x04 0x100737c0: becomes a copy of `source` (its own Direct3D texture, uncached).
    virtual HRESULT __stdcall Duplicate(const ITexture *source);
    // 0x08 0x10073a80: loads the image (and opacity map) files, or creates an empty texture.
    // `levels`: mip levels with ITC_MIPMAP (0: all).
    virtual HRESULT __stdcall Create(const char *fileName, const char *alphaFileName, uint32_t flags, int width,
                                     int height, int levels);
    // 0x0C 0x10076ae0
    virtual HRESULT __stdcall Close();
    // 0x10 0x10073a60: "WAS NOT IMPLEMENTED".
    virtual HRESULT __stdcall Reload();
    // 0x14 / 0x18 0x10076c20 / 0x10076c80: locks a level (one lock at a time); returns the bits.
    virtual void *__stdcall Lock(const RECT *rect, int *pitch, int level);
    virtual HRESULT __stdcall Unlock();
    // 0x1C 0x10076cb0: draws a point with the texture so that the driver uploads it.
    virtual HRESULT __stdcall Preload();
    // 0x20 0x10076d90: fills a level (rect 0: all) with a colour in the texture's format.
    virtual bool __stdcall Fill(const RECT *rect, D3DCOLOR color, int level);
    // 0x24 0x10077160: the texture stretched over a screen rectangle, a colour per corner.
    virtual bool __stdcall DrawRect(int left, int top, int right, int bottom, D3DCOLOR topLeft, D3DCOLOR topRight,
                                    D3DCOLOR bottomLeft, D3DCOLOR bottomRight);
    // 0x28 0x10076fe0: the texture at a screen position, unscaled.
    virtual bool __stdcall Draw(int x, int y);
    // 0x2C 0x100772e0: level 0 to a .tga file (not for compressed formats).
    virtual bool __stdcall SaveToTGA(const char *fileName);

    ITexture();                                 // 0x10073720

private:
    // The two halves of Create (one function in the original).
    HRESULT CreateFromDds(const char *fileName);
    HRESULT CreateFromImage(const char *fileName, const char *alphaFileName, int width, int height, int levels);

public:
    uint16_t id;                                // +0x04 IGraph's running number
    uint16_t pad06;
    IDirect3DTexture8 *d3dTexture;              // +0x08
    IDirect3DSurface8 *lockedSurface;           // +0x0C while locked
    bool loaded;                                // +0x10
    uint8_t pad11[3];
    int refCount;                               // +0x14
    char *fileName;                             // +0x18 file name without the directory
    char *alphaFileName;                        // +0x1C
    uint16_t width;                             // +0x20
    uint16_t height;                            // +0x22
    D3DFORMAT format;                           // +0x24
    uint32_t flags;                             // +0x28 ITC_* in effect
    uint32_t requestedFlags;                    // +0x2C ITC_* as given to Create
    uint32_t colorKey;                          // +0x30 ITC_ALPHA_1BIT: palette entry 0 in the texture format
    uint8_t *pixels;                            // +0x34 ITC_KEEP_PIXELS: 32-bit BGRA level 0 (or a 1-bit mask)
    uint16_t levels;                            // +0x38
    uint16_t pad3A;
    uint32_t memorySize;                        // +0x3C bytes of all levels
    ls3d::tex::S_streamRecord *stream;          // +0x40 ITC_STREAMED
};
static_assert(sizeof(ITexture) == 0x44, "ITexture layout");

namespace ls3d::tex {

// Image loading, pixel conversion and the compressor (image_load.cpp, image_convert.cpp, s3tc).
struct S_imageInfo;

// Reads a .bmp / .tga file (or an in-memory BITMAPINFOHEADER image) into bottom-up rows; resizes
// (nearest pixel) to width x height if both are given (0x100775f0).
bool LoadImage(const void *source, int width, int height, uint32_t flags, uint8_t **bits, uint32_t **palette,
               BITMAPINFOHEADER *info, bool fromMemory, FILETIME *fileTime, int handle);
// Writes a 32-bit BGRA image into a locked surface of the surface's format (0x10077d00).
bool WriteSurface(const ITexture *texture, const uint8_t *bgra, IDirect3DSurface8 *surface, uint32_t width,
                  uint32_t height, int kind);
// Compresses `source` (16-bit) into `dest` (DXT) with the S3TC encoder (0x100788b0).
bool CompressSurface(const char *name, int kind, IDirect3DSurface8 *source, IDirect3DSurface8 *dest);
// DXT1 / DXT3 / DXT5 blocks to 32-bit BGRA (0x100794a0, 0x10079790, 0x10079b10).
void DecodeDxt1(const uint8_t *src, uint32_t *dst, uint32_t width, uint32_t height);
void DecodeDxt3(const uint8_t *src, uint32_t *dst, uint32_t width, uint32_t height);
void DecodeDxt5(const uint8_t *src, uint32_t *dst, uint32_t width, uint32_t height);

// Texture thread bookkeeping (texture_stream.cpp).
bool StreamAdd(ITexture *texture, const char *fileName, uint32_t flags);     // 0x10079230
void StreamRemove(ITexture *texture);                                       // 0x10079340
bool StreamRequest(ITexture *texture);                                      // 0x10078e40
void StreamReleaseAll();                                                    // 0x10079440

}  // namespace ls3d::tex
