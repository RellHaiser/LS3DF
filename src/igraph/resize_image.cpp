// IGraph::ResizeImage's resampler (0x10070160), without engine dependencies but the log.
#include <cmath>
#include <cstring>
#include <new>

#include "core/debug.h"
#include "igraph/igraph_state.h"

namespace ls3d::ig {

// 0x10070160 (IGraph::ResizeImage)
bool ResizeImage(int srcWidth, int srcHeight, int srcPitch, const uint8_t *src, int dstWidth, int dstHeight,
                 int dstPitch, uint8_t **dstOut, int bpp, bool filter)
{
    if (bpp != 4 && bpp != 3)
        dbgPrintf("IGraph::ResizeImage() - ERROR!! Stride must be 3 ( 24bit image ) or 4 ( 32bit image ) !!");
    if (!*dstOut) {
        // The original allocates dstWidth * dstHeight * bpp, too little for a wider pitch.
        const int pitch = dstPitch > dstWidth * bpp ? dstPitch : dstWidth * bpp;
        *dstOut = static_cast<uint8_t *>(::operator new(static_cast<size_t>(pitch) * dstHeight, std::nothrow));
        if (!*dstOut)
            return false;
    }
    uint8_t *dst = *dstOut;
    const int rowSkip = dstPitch - dstWidth * bpp;

    if (!filter || srcWidth < dstWidth || srcHeight < dstHeight) {
        // Nearest source pixel.
        const float stepX = static_cast<float>(srcWidth) / static_cast<float>(dstWidth);
        const float stepY = static_cast<float>(srcHeight) / static_cast<float>(dstHeight);
        for (int y = 0; y < dstHeight; ++y) {
            if (dstWidth > 0) {
                const int row = static_cast<int>(static_cast<float>(y) * stepY) * srcPitch;
                for (int x = 0; x < dstWidth; ++x) {
                    const int col = static_cast<int>(static_cast<float>(x) * stepX);
                    std::memcpy(dst, src + col * bpp + row, bpp);
                    dst += bpp;
                }
            }
            dst += rowSkip;
        }
        return true;
    }

    // Box filter over the source area of each destination pixel, partial pixels at the edges
    // weighted by their covered fraction. The scale maps onto srcWidth - 1 (srcHeight - 1) pixels.
    // Float operations in the original's order (it keeps the sums on the x87 stack in single
    // precision, so this computes the same values).
    const size_t srcSize = static_cast<size_t>(srcPitch) * srcHeight;
    const float scaleX = static_cast<float>(srcWidth - 1) / static_cast<float>(dstWidth);
    const float scaleY = static_cast<float>(srcHeight - 1) / static_cast<float>(dstHeight);
    constexpr float kEps = 1e-08f;
    for (int y = 0; y < dstHeight; ++y) {
        for (int x = 0; x < dstWidth; ++x) {
            const float top = static_cast<float>(y) * scaleY;
            const float bottom = static_cast<float>(y + 1) * scaleY;
            const int y0 = static_cast<int>(top) + 1;   // first whole row
            const int y1 = static_cast<int>(bottom);    // row of the bottom edge
            float wTop, wBottom;
            if (y0 > y1) {
                wTop = bottom - top;
                wBottom = 0.0f;
            } else {
                wTop = static_cast<float>(y0) - top;
                wBottom = bottom - static_cast<float>(y1);
            }
            const int rows = y1 - y0 < 0 ? 0 : y1 - y0;
            const float left = static_cast<float>(x) * scaleX;
            const float right = static_cast<float>(x + 1) * scaleX;
            const int x0 = static_cast<int>(left) + 1;
            const int x1 = static_cast<int>(right);
            float wLeft, wRight;
            if (x0 > x1) {
                wLeft = right - left;
                wRight = 0.0f;
            } else {
                wLeft = static_cast<float>(x0) - left;
                wRight = right - static_cast<float>(x1);
            }
            const int cols = x1 - x0 < 0 ? 0 : x1 - x0;

            float b = 0.0f, g = 0.0f, r = 0.0f, a = 0.0f, weight = 0.0f;
            // Adds pixel (px, py) times w.
            auto add = [&](int px, int py, float w) {
                const uint8_t *p = src + py * srcPitch + px * bpp;
                r = r + static_cast<float>(p[2]) * w;
                g = g + static_cast<float>(p[1]) * w;
                b = b + static_cast<float>(p[0]) * w;
                if (bpp == 4)
                    a = a + static_cast<float>(p[3]) * w;
            };
            auto addCorner = [&](int px, int py, float wx, float wy) {
                const uint8_t *p = src + py * srcPitch + px * bpp;
                r = r + static_cast<float>(p[2]) * wx * wy;
                g = g + static_cast<float>(p[1]) * wx * wy;
                b = b + static_cast<float>(p[0]) * wx * wy;
                if (bpp == 4)
                    a = a + static_cast<float>(p[3]) * wx * wy;
                weight = wx * wy + weight;
            };
            if (wTop > kEps) {                  // the partial top row
                if (wLeft > kEps)
                    addCorner(x0 - 1, y0 - 1, wLeft, wTop);
                if (cols > 0) {
                    for (int c = 0; c < cols; ++c)
                        add(x0 + c, y0 - 1, wTop);
                    weight = static_cast<float>(cols) * wTop + weight;
                }
                if (wRight > kEps)
                    addCorner(x0 + cols, y0 - 1, wRight, wTop);
            }
            if (rows > 0) {                     // the whole rows
                if (wLeft > kEps) {
                    for (int k = 0; k < rows; ++k)
                        add(x0 - 1, y0 + k, wLeft);
                    weight = static_cast<float>(rows) * wLeft + weight;
                }
                if (cols > 0) {
                    // Original bug kept: the inner block advances its pixel pointer twice per
                    // column, so it sums pixels x0, x0 + 2, x0 + 4 ... (and can run past the row
                    // and the image). The port reads 0 beyond the source buffer, where the
                    // original read whatever memory followed.
                    for (int k = 0; k < rows; ++k) {
                        for (int c = 0; c < cols; ++c) {
                            const size_t at = static_cast<size_t>((y0 + k) * srcPitch + (x0 + 2 * c) * bpp);
                            auto byte = [&](int i) { return at + i < srcSize ? src[at + i] : 0; };
                            r = r + static_cast<float>(byte(2));
                            g = g + static_cast<float>(byte(1));
                            b = b + static_cast<float>(byte(0));
                            if (bpp == 4)
                                a = a + static_cast<float>(byte(3));
                        }
                    }
                    weight = static_cast<float>(cols * rows) + weight;
                }
                if (wRight > kEps) {
                    for (int k = 0; k < rows; ++k)
                        add(x0 + cols, y0 + k, wRight);
                    weight = static_cast<float>(rows) * wRight + weight;
                }
            }
            if (wBottom > kEps) {               // the partial bottom row
                const int py = y0 + rows;
                if (wLeft > kEps)
                    addCorner(x0 - 1, py, wLeft, wBottom);
                if (cols > 0) {
                    for (int c = 0; c < cols; ++c)
                        add(x0 + c, py, wBottom);
                    weight = static_cast<float>(cols) * wBottom + weight;
                }
                if (wRight > kEps)
                    addCorner(x0 + cols, py, wRight, wBottom);
            }
            // No weight (or NaN-free zero): magenta.
            if (weight > kEps || std::isnan(weight)) {
                const float inv = 1.0f / weight;
                r = inv * r;
                g = inv * g;
                b = inv * b;
                a = inv * a;
            } else {
                r = 255.0f;
                g = 0.0f;
                b = 255.0f;
                a = 255.0f;
            }
            dst[2] = static_cast<uint8_t>(std::lrintf(r));
            dst[1] = static_cast<uint8_t>(std::lrintf(g));
            dst[0] = static_cast<uint8_t>(std::lrintf(b));
            if (bpp == 4)
                dst[3] = static_cast<uint8_t>(std::lrintf(a));
            dst += bpp;
        }
        dst += rowSkip;
    }
    return true;
}

}  // namespace ls3d::ig
