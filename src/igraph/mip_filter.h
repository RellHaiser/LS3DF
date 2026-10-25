// Port enhancement (LS3DF.ini ImprovedMipmaps): the mip levels ITexture::Create filters, made
// gamma-correct and alpha-weighted, with colour-keyed textures keeping their alpha-tested coverage.
//
// The original's filter (Downsample, itexture.cpp) weighs a 2 x 2 block 1/8, 3/8, 3/8, 1/8 and
// truncates every term, so each level comes out darker than the one above; it averages the sRGB
// values themselves (fine bright detail fades: random noise loses a quarter of its light in one
// level); transparent texels (a colour key's) mix their colour into the edges; and the coverage of
// an alpha-tested texture drifts level by level (at the default reference 0x20 colour-keyed foliage
// grows with distance).
#pragma once

#include <cstdint>

namespace ls3d::tex {

// The fraction of the texels of a 32-bit BGRA image whose alpha is at least `alphaRef` (what an
// alpha test with D3DCMP_GREATEREQUAL lets through).
double AlphaCoverage(const uint8_t *bgra, int width, int height, int alphaRef);

// The next mip level of a 32-bit BGRA image, rows packed: max(1, width / 2) x max(1, height / 2),
// each texel from the 2 x 2 block above it (the block's samples clamped to the image: 1 x 2 or
// 2 x 1 at a side of 1; an odd last row or column is left out, as Direct3D halves sizes). Colours
// are averaged in linear light, weighted by alpha (a transparent texel adds no colour; a block
// with no alpha at all is averaged plainly) and rounded to the nearest sRGB value; the alpha is
// the block's mean. `dst` must not overlap `src`.
void DownsampleMip(const uint8_t *src, int width, int height, uint8_t *dst);

// Rescales the alpha of an image so that its AlphaCoverage at `alphaRef` (1 - 255) comes as near
// `coverage` as its texels allow; texels keep their order of alpha. For the copy of a level that
// is uploaded: the next level is made from the unscaled one, or the scaling compounds.
void ScaleAlphaToCoverage(uint8_t *bgra, int width, int height, int alphaRef, double coverage);

}  // namespace ls3d::tex
