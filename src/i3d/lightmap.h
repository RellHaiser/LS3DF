// The lightmap atlas, the original's "LightMap" (0x10059e00 - 0x1005d1e0): the lightmapped
// visuals' patches packed into shared lightmap textures.
//
// Every atlas texture (S_lmTexture, in a global list sorted by free area) keeps its free
// rectangles in a list sorted by area; rectangles thinner than 3 pixels are only counted. A
// C_lightmap places the patches (S_lightmapPart) of one mesh level, largest first: into the first
// existing texture they all fit in, else into the new texture of 128 - 512 pixels a side that
// leaves the least free area (every patch halved while none does), turning a patch by 90 degrees
// where that leaves more room; then it writes them into the texture. Freed, it gives the
// rectangles back (filled white) and releases the texture.
#pragma once

#include <cstdint>

#include "core/c_vector.h"

class I3D_frame;
class ITexture;

namespace ls3d::i3d {

// A lightmap patch (0x1C bytes). A uniform patch is a 3 x 3 marker with its color instead of
// pixels.
struct S_lightmapPart {
    int16_t sourceWidth;                    // +0x00 the image's size
    int16_t sourceHeight;                   // +0x02
    int16_t width;                          // +0x04 in the atlas (halved while it is full)
    int16_t height;                         // +0x06
    int area;                               // +0x08 width x height
    float u;                                // +0x0C its place in the atlas texture (+ 2: turned)
    float v;                                // +0x10
    union {
        uint8_t *pixels;                    // +0x14 BGR, sourceWidth x sourceHeight
        uint32_t color;                     // ... a uniform patch's 0xAARRGGBB (Construct: alpha 0xFF)
    };
    void *texels;                           // +0x18 width x height in the atlas texture's format

    bool IsUniform() const { return width == 3 && height == 3; }
};
static_assert(sizeof(S_lightmapPart) == 0x1C, "S_lightmapPart layout");

// A patch's place in an atlas texture (12 bytes).
struct S_lmPlacement {
    int x;
    int y;
    uint8_t rotated;                        // turned by 90 degrees (width along y)
    uint8_t padding[3];
};

class C_lightmap {
public:
    C_lightmap();                           // 0x1005a340
    // 0x1005a370: the rectangles back to the atlas texture, filled white; the texture released
    // (the last reference takes it out of the atlas and the driver's list).
    ~C_lightmap();
    C_lightmap(const C_lightmap &) = delete;
    C_lightmap &operator=(const C_lightmap &) = delete;

    // 0x1005b1d0 (m_CreateLM): the patches `indices` of `parts` placed and written into an atlas
    // texture; `textureBytes` grows by the texture memory they take. The parts' pixels are freed
    // (but small ones'), their u / v set. `owner` names the visual in the messages.
    bool Create(const uint16_t *indices, S_lightmapPart *parts, int numParts, int *textureBytes,
                const I3D_frame *owner);

    ITexture *texture;                      // +0x00 (a reference)
    C_vector<S_lmPlacement> placements;     // +0x04 (grow 10) in `order`
    S_lightmapPart *parts;                  // +0x14 (the level's)
    C_vector<uint16_t> order;               // +0x18 (grow 0x40) the placed parts, largest first
    int totalArea;                          // +0x28 their area
};
static_assert(sizeof(C_lightmap) == 0x2C, "C_lightmap layout");

// The atlas texture format's texels of a patch (0x10059eb0): BGR copied (24 / 32-bit and L8
// textures), or 1-5-5-5 (16-bit; dithered with driver state DRVSTATE_LM_DITHER); `out` a new
// block. False for other formats; a uniform patch (3 x 3) has none and gives true.
bool ConvertTexels(uint16_t width, uint16_t height, const uint8_t *bgr, void **out);

}  // namespace ls3d::i3d
