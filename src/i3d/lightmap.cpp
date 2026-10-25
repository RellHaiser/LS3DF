// The lightmap atlas (0x10059e00 - 0x1005d1e0). 0x10059e00, 0x10059e30 / 0x10059e60 and 0x1005ca50 /
// 0x1005ca70 are the static initialisers and atexit destructors of this file's globals (a header's
// (0, 0, 0, 1) and the list of atlas textures); 0x1005a850 / 0x1005a870 (an atlas texture's
// constructor / destructor), 0x1005cab0, 0x1005cd50, 0x1005cdb0, 0x1005ceb0, 0x1005cf70 and
// 0x1005d170 the MSVC 6 vector code of that list and of the placements (std::vector here).
#include "i3d/lightmap.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "core/debug.h"
#include "core/fpu.h"
#include "i3d/driver.h"
#include "i3d/frame.h"
#include "i3d/i3d.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

// A free rectangle of an atlas texture (0x1C bytes).
struct S_freeRect {
    int area;                               // +0x00
    int x, y;                               // +0x04 (a RECT made with SetRect(x, y, width, height))
    int width, height;                      // +0x0C
    S_freeRect *prev;                       // +0x14
    S_freeRect *next;                       // +0x18
};

// An atlas texture (0x28 bytes in the original). Copies share the rectangles, as the original's
// records do.
struct S_lmTexture {
    uint16_t width = 0;
    uint16_t height = 0;
    int freeArea = 0;                       // in the rectangles of at least 3 x 3
    int smallArea = 0;                      // in the thinner ones (not kept)
    S_freeRect *head = nullptr;             // by area, the smallest first
    S_freeRect *tail = nullptr;
    std::vector<S_lmPlacement> placements;  // the patches being placed (none in the list)
    ITexture *texture = nullptr;            // (the C_lightmaps' references)
};

// The atlas textures, the least free area first (0x100c0630).
std::vector<S_lmTexture> s_textures;

// The texture's rectangles freed (their area taken off the free area).
void FreeRects(S_lmTexture &t)
{
    for (S_freeRect *n = t.head; n;) {
        S_freeRect *next = n->next;
        t.freeArea -= n->area;
        delete n;
        n = next;
    }
    // (The original leaves the list pointing at them; those records are not used again.)
    t.head = t.tail = nullptr;
}

void InsertBefore(S_lmTexture &t, S_freeRect *n, S_freeRect *at)
{
    n->prev = at->prev;
    n->next = at;
    if (at->prev)
        at->prev->next = n;
    else
        t.head = n;
    at->prev = n;
}

// 0x1005a890: a rectangle into the list by area (one thinner than 3 only counted).
bool AddFreeRect(S_lmTexture &t, int x, int y, int width, int height)
{
    if (width < 3 || height < 3) {
        t.smallArea += height * width;
        return true;
    }
    const int area = height * width;
    t.freeArea += area;
    auto *n = new (std::nothrow) S_freeRect{area, x, y, width, height, nullptr, nullptr};
    if (!n)
        return false;
    if (!t.head) {
        t.head = t.tail = n;
        return true;
    }
    for (S_freeRect *at = t.head; at; at = at->next) {
        if (at->area >= area) {
            InsertBefore(t, n, at);
            return true;
        }
    }
    n->prev = t.tail;
    t.tail->next = n;
    t.tail = n;
    return true;
}

// 0x1005a9b0: a rectangle whose size changed moved to its place by area (one thinner than 3
// dropped, its area counted).
void ReinsertRect(S_lmTexture &t, S_freeRect *n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        t.head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        t.tail = n->prev;
    if (n->width < 3 || n->height < 3) {
        t.smallArea += n->area;
        delete n;
        return;
    }
    t.freeArea += n->area;
    for (S_freeRect *at = t.head; at; at = at->next) {
        if (at->area >= n->area) {
            InsertBefore(t, n, at);
            return;
        }
    }
    n->next = nullptr;
    if (!t.head) {
        n->prev = nullptr;
        t.head = t.tail = n;
        return;
    }
    n->prev = t.tail;
    t.tail->next = n;
    t.tail = n;
}

// A rectangle the patch fills exactly taken out.
void RemoveRect(S_lmTexture &t, S_freeRect *n)
{
    t.freeArea -= n->area;
    if (n->prev)
        n->prev->next = n->next;
    else
        t.head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        t.tail = n->prev;
    delete n;
}

// 0x1005aaa0: `src`'s rectangles appended to `dst`'s list (the areas as they are).
bool CopyRects(const S_lmTexture &src, S_lmTexture &dst)
{
    for (const S_freeRect *s = src.head; s; s = s->next) {
        auto *n = new (std::nothrow) S_freeRect;
        if (!n)
            return false;
        *n = *s;
        n->next = nullptr;
        n->prev = dst.tail;
        if (dst.tail)
            dst.tail->next = n;
        else
            dst.head = n;
        dst.tail = n;
    }
    return true;
}

// The patch turned (`h` wide, `w` high) into rectangle `n`, which the rest splits into.
bool PlaceTurned(S_lmTexture &t, S_freeRect *n, int w, int h)
{
    t.placements.push_back({n->x, n->y, 1, {}});
    if (n->width == h && n->height == w) {
        RemoveRect(t, n);
        return true;
    }
    t.freeArea -= n->area;
    const int nw = n->width;
    const int nh = n->height;
    if (nw == h) {
        n->area = (nh - w) * nw;
        n->y += w;
        n->height = nh - w;
        ReinsertRect(t, n);
        return true;
    }
    if (nh == w) {
        n->x += h;
        n->width = nw - h;
        n->area = (nw - h) * nh;
        ReinsertRect(t, n);
        return true;
    }
    // Two rectangles: below the patch and beside it; the larger one keeps the full length.
    const int below = (nh - w) * nw;
    const int besideWidth = nw - h;
    const int beside = besideWidth * nh;
    const int larger = below > beside ? below : beside;
    S_freeRect r = *n;
    r.y += w;
    r.height -= w;
    n->x += h;
    n->width = besideWidth;
    if (larger == below)
        r.width = h;
    else
        n->height = w;
    n->area = n->height * besideWidth;
    ReinsertRect(t, n);
    AddFreeRect(t, r.x, r.y, r.width, r.height);
    return true;
}

// 0x1005ab40: the patch into the first (smallest) free rectangle it fits, turned when only that
// fits (or turned it fills a side); false when none has room.
bool Place(S_lmTexture &t, const S_lightmapPart &p)
{
    const int w = p.width;
    const int h = p.height;
    if (!t.head)
        AddFreeRect(t, 0, 0, static_cast<int16_t>(t.width), static_cast<int16_t>(t.height));
    S_freeRect *n = t.head;
    for (; n; n = n->next) {
        if (n->width >= w && n->height >= h)
            break;
        if (n->height >= w && n->width >= h)
            return PlaceTurned(t, n, w, h);
    }
    if (!n)
        return false;
    bool turnable;
    if (n->height < w)
        turnable = false;
    else if (n->width == h)
        return PlaceTurned(t, n, w, h);
    else if (n->height == w && n->width >= h)
        return PlaceTurned(t, n, w, h);
    else
        turnable = n->width >= h;
    const int x0 = n->x;
    const int y0 = n->y;
    t.placements.push_back({x0, y0, 0, {}});
    if (n->width == w && n->height == h) {
        RemoveRect(t, n);
        return true;
    }
    t.freeArea -= n->area;
    const int nw = n->width;
    const int nh = n->height;
    if (nw == w) {
        n->area = (nh - h) * nw;
        n->y += h;
        n->height = nh - h;
        ReinsertRect(t, n);
        return true;
    }
    if (nh == h) {
        n->x += w;
        n->width = nw - w;
        n->area = (nw - w) * nh;
        ReinsertRect(t, n);
        return true;
    }
    // The split that leaves the largest rectangle: the patch as it is (beside it at full height,
    // or below it at full width) or turned.
    const int besideWidth = nw - w;
    const int beside = besideWidth * nh;
    const int below = (nh - h) * nw;
    const int belowTurned = (nh - w) * nw;
    const int besideTurnedWidth = nw - h;
    const int besideTurned = besideTurnedWidth * nh;
    int largest = belowTurned > besideTurned ? belowTurned : besideTurned;
    if (below > largest)
        largest = below;
    if (beside > largest)
        largest = beside;
    S_freeRect r = *n;
    if (!turnable || largest == beside || largest == below) {
        r.y += h;
        r.height -= h;
        n->x += w;
        if (largest == beside) {
            r.width = w;
            n->width = besideWidth;
        } else {
            n->height = h;
            n->width = besideWidth;
        }
    } else {
        r.y += w;
        r.height -= w;
        n->x += h;
        if (largest == belowTurned) {
            r.width = h;
            n->width = besideTurnedWidth;
        } else {
            n->height = w;
            n->width = besideTurnedWidth;
        }
        t.placements.pop_back();
        t.placements.push_back({x0, y0, 1, {}});
    }
    n->area = n->height * n->width;
    ReinsertRect(t, n);
    AddFreeRect(t, r.x, r.y, r.width, r.height);
    return true;
}

// The record into the list before the first with as much free area.
void InsertSorted(const S_lmTexture &rec)
{
    size_t k = 0;
    while (k < s_textures.size() && s_textures[k].freeArea < rec.freeArea)
        ++k;
    s_textures.insert(s_textures.begin() + static_cast<std::ptrdiff_t>(k), rec);
}

// A record's placements and rectangles freed (a candidate not taken).
void Discard(S_lmTexture &t)
{
    t.placements.clear();
    FreeRects(t);
}

// The texels of patch `p` written into the locked atlas texture at `pl`; its u / v set.
void WritePart(ITexture *texture, uint8_t *bits, int pitch, const S_lmTexture &atlas, const S_lmPlacement &pl,
               S_lightmapPart &p)
{
    const D3DFORMAT format = texture->format;
    if (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8 && format != D3DFMT_R5G6B5 &&
        format != D3DFMT_X1R5G5B5 && format != D3DFMT_A1R5G5B5 && format != D3DFMT_L8)
        return;                             // (R8G8B8 atlases get nothing written, u / v unset)
    const float u = static_cast<float>(pl.x) / static_cast<float>(static_cast<int16_t>(atlas.width));
    p.u = pl.rotated && !p.IsUniform() ? u + 2.0f : u;
    p.v = static_cast<float>(pl.y) / static_cast<float>(static_cast<int16_t>(atlas.height));
    const int w = p.width;
    const int h = p.height;
    const uint32_t c = p.color;
    const uint8_t *src = static_cast<const uint8_t *>(p.texels);
    // (Fix: the original reads null texels when the format had none to convert to.)
    if (!p.IsUniform() && !src)
        return;
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8: {
        uint8_t *d = bits + pl.y * pitch + pl.x * 4;
        if (p.IsUniform()) {
            for (int r = 0; r < 3; ++r, d += pitch - 12) {
                for (int i = 0; i < 3; ++i, d += 4) {
                    d[0] = static_cast<uint8_t>(c);
                    d[1] = static_cast<uint8_t>(c >> 8);
                    d[2] = static_cast<uint8_t>(c >> 16);
                }
            }
        } else if (!pl.rotated) {
            for (int r = 0; r < h; ++r, d += pitch - w * 4) {
                for (int i = 0; i < w; ++i, d += 4, src += 3) {
                    d[0] = src[0];
                    d[1] = src[1];
                    d[2] = src[2];
                }
            }
        } else {
            // Turned: each row into a column.
            for (int r = 0; r < h; ++r) {
                uint8_t *q = d + r * 4;
                for (int i = 0; i < w; ++i, q += pitch, src += 3) {
                    q[0] = src[0];
                    q[1] = src[1];
                    q[2] = src[2];
                }
            }
        }
        break;
    }
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: {
        const uint32_t pitch16 = static_cast<uint32_t>(pitch) >> 1;
        uint16_t *d = reinterpret_cast<uint16_t *>(bits) + (pitch16 * pl.y + pl.x);
        const uint16_t *s16 = static_cast<const uint16_t *>(p.texels);
        if (p.IsUniform()) {
            const uint32_t rr = (c >> 16) & 0xff, gg = (c >> 8) & 0xff, bb = c & 0xff;
            uint16_t texel;
            if (format == D3DFMT_R5G6B5)
                texel = static_cast<uint16_t>((((rr & 0xf8) << 5 | (gg & 0xfc)) << 3) | (bb >> 3));
            else
                texel = static_cast<uint16_t>(((((rr & 0xf8) | 0xff00) << 5 | (gg & 0xf8)) << 2) | (bb >> 3));
            for (int r = 0; r < 3; ++r, d += pitch16) {
                d[0] = texel;
                d[1] = texel;
                d[2] = texel;
            }
        } else if (!pl.rotated) {
            for (int r = 0; r < h; ++r, d += pitch16 - w) {
                for (int i = 0; i < w; ++i)
                    *d++ = *s16++;
            }
        } else {
            for (int r = 0; r < h; ++r) {
                uint16_t *q = d + r;
                for (int i = 0; i < w; ++i, q += pitch16)
                    *q = *s16++;
            }
        }
        break;
    }
    default: {
        // L8: three planes of the texture's size, red, green and blue.
        const uint32_t texW = texture->width;
        const uint32_t planeSize = texW * texture->height;
        uint8_t *p0 = bits + texW * pl.y + pl.x;
        uint8_t *p1 = p0 + planeSize;
        uint8_t *p2 = p0 + planeSize * 2;
        const int atlasWidth = static_cast<int16_t>(atlas.width);
        if (p.IsUniform()) {
            const int skip = atlasWidth - 3;
            for (int r = 0; r < 3; ++r, p0 += skip, p1 += skip, p2 += skip) {
                for (int i = 0; i < 3; ++i) {
                    *p0++ = static_cast<uint8_t>(c >> 16);
                    *p1++ = static_cast<uint8_t>(c >> 8);
                    *p2++ = static_cast<uint8_t>(c);
                }
            }
        } else if (!pl.rotated) {
            const int skip = atlasWidth - w;
            for (int r = 0; r < h; ++r, p0 += skip, p1 += skip, p2 += skip) {
                for (int i = 0; i < w; ++i, src += 3) {
                    *p0++ = src[2];
                    *p1++ = src[1];
                    *p2++ = src[0];
                }
            }
        } else {
            for (int r = 0; r < h; ++r, ++p0, ++p1, ++p2) {
                uint32_t q = 0;
                for (int i = 0; i < w; ++i, q += texW, src += 3) {
                    p0[q] = src[2];
                    p1[q] = src[1];
                    p2[q] = src[0];
                }
            }
        }
        break;
    }
    }
}

}  // namespace

namespace ls3d::i3d {

// 0x10059eb0
bool ConvertTexels(uint16_t width, uint16_t height, const uint8_t *bgr, void **out)
{
    if (width == 3 && height == 3)
        return true;
    D3DFORMAT format = ig::g_fmt1BitAlpha;
    if (LightmapsTrueColor() && ig::g_fmtTrueColor)
        format = ig::g_fmtTrueColor;
    // Floyd-Steinberg errors (b, g, r of each texel; a column each side and a row below).
    const bool dither = (format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5) && g_driverStates[DRVSTATE_LM_DITHER];
    float *err = nullptr;
    const uint32_t errStride = static_cast<uint32_t>(width) + 2;
    if (dither) {
        const size_t bytes = static_cast<size_t>(errStride) * (static_cast<uint32_t>(height) + 1) * 16;
        err = static_cast<float *>(AlignedAlloc(bytes, 16));
        if (err)                            // (the original clears the null block)
            std::memset(err, 0, bytes);
    }
    switch (format) {
    case D3DFMT_R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_L8: {
        const size_t bytes = static_cast<size_t>(width) * height * 3;
        auto *copy = static_cast<uint8_t *>(::operator new(bytes));
        *out = copy;
        std::memcpy(copy, bgr, bytes);
        break;
    }
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: {
        auto *dst = static_cast<uint16_t *>(::operator new(static_cast<size_t>(width) * height * 2));
        *out = dst;
        auto pack = [](uint32_t r, uint32_t g, uint32_t b) {
            return static_cast<uint16_t>(((((r & 0xfff8) | 0xff00) << 5 | (g & 0xf8)) << 2) | ((b >> 3) & 0x1f));
        };
        if (!err) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x, bgr += 3)
                    *dst++ = pack(bgr[2], bgr[1], bgr[0]);
            }
            break;
        }
        for (int y = 0; y < height; ++y) {
            // The texel's error entry (one column in), the row below's from the left neighbour.
            float *e = err + (static_cast<size_t>(y) * errStride + 1) * 4;
            float *below = err + (static_cast<size_t>(y) + 1) * errStride * 4;
            for (int x = 0; x < width; ++x, bgr += 3, e += 4, below += 4) {
                const float b = static_cast<float>(bgr[0]) + e[2];
                float g = static_cast<float>(bgr[1]) + e[1];
                float r = static_cast<float>(bgr[2]) + e[0];
                // The parts lost to 5 bits (the remainder of 8), sixteenths of them pushed on.
                const float er = static_cast<float>(std::fmod(static_cast<double>(r), 8.0) * 0.0625);
                const float eg = static_cast<float>(std::fmod(static_cast<double>(g), 8.0) * 0.0625);
                const float eb = static_cast<float>(std::fmod(static_cast<double>(b), 8.0) * 0.0625);
                e[4] = er * 7.0f + e[4];
                e[5] = eg * 7.0f + e[5];
                e[6] = 7.0f * eb + e[6];
                below[0] = er * 3.0f + below[0];
                below[1] = eg * 3.0f + below[1];
                below[2] = 3.0f * eb + below[2];
                below[4] = er * 5.0f + below[4];
                below[5] = eg * 5.0f + below[5];
                below[6] = 5.0f * eb + below[6];
                below[8] = er + below[8];
                below[9] = eg + below[9];
                below[10] = eb + below[10];
                if (r > 255.0f)
                    r = 255.0f;
                if (g > 255.0f)
                    g = 255.0f;
                float bc = b;
                if (bc > 255.0f)
                    bc = 255.0f;
                const uint32_t ir = static_cast<uint32_t>(Fistp(r));
                const uint32_t igr = static_cast<uint32_t>(Fistp(g)) & 0xff;
                const int ib = Fistp(bc);
                *dst++ = static_cast<uint16_t>(((((ir & 0xfff8) | 0xff00) << 5 | (igr & 0xf8)) << 2) |
                                               ((static_cast<uint32_t>(ib >> 3)) & 0x1f));
            }
        }
        break;
    }
    default:
        AlignedFree(err);
        return false;
    }
    AlignedFree(err);
    return true;
}

C_lightmap::C_lightmap() : texture(nullptr), placements(10), parts(nullptr), order(0x40), totalArea(0) {}

C_lightmap::~C_lightmap()
{
    if (texture) {
        if (texture->Release() <= 0) {
            // The texture is gone: out of the driver's list and the atlas.
            g_driver->lightmapTextures.RemoveSwap(texture);
            g_driver->SlotB4();
            for (size_t i = 0; i < s_textures.size(); ++i) {
                if (s_textures[i].texture == texture) {
                    Discard(s_textures[i]);
                    s_textures.erase(s_textures.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        } else {
            // The patches' rectangles back to the texture's record (from the end of the list).
            int k = static_cast<int>(s_textures.size()) - 1;
            while (k >= 0 && s_textures[k].texture != texture)
                --k;
            if (k >= 0) {
                S_lmTexture rec = s_textures[k];
                s_textures.erase(s_textures.begin() + k);
                for (int i = 0; i < order.Size(); ++i) {
                    const S_lmPlacement &pl = placements[i];
                    const S_lightmapPart &p = parts[order[i]];
                    int w = p.width;
                    int h = p.height;
                    if (pl.rotated)
                        std::swap(w, h);
                    AddFreeRect(rec, pl.x, pl.y, w, h);
                    const RECT r = {pl.x, pl.y, pl.x + w, pl.y + h};
                    texture->Fill(&r, 0xffffffff, 0);
                }
                InsertSorted(rec);
            }
        }
    }
    order.Free();
    placements.Free();
}

bool C_lightmap::Create(const uint16_t *indices, S_lightmapPart *partArray, int numParts, int *textureBytes,
                        const I3D_frame *owner)
{
    order.Clear();
    totalArea = 0;
    parts = partArray;
    // The patches by area, the largest first (the taken ones marked with u = 666).
    for (;;) {
        int best = 0;
        uint16_t bestIndex = 0;
        for (int k = 0; k < numParts; ++k) {
            const uint16_t i = indices[k];
            if (parts[i].u == 0.0f && parts[i].area >= best) {
                best = parts[i].area;
                bestIndex = i;
            }
        }
        if (best <= 0)
            break;
        parts[bestIndex].u = 666.0f;
        order.PushBack(bestIndex);
    }
    for (int i = 0; i < order.Size(); ++i)
        totalArea += parts[order[i]].area;
    for (int i = 0; i < order.Size(); ++i) {
        S_lightmapPart &p = parts[order[i]];
        ConvertTexels(static_cast<uint16_t>(p.width), static_cast<uint16_t>(p.height), p.pixels, &p.texels);
    }

    // The first atlas texture they all fit in (it leaves the list; it goes back by its new free
    // area at the end).
    S_lmTexture found;
    S_lmTexture *best = nullptr;
    for (size_t i = 0; i < s_textures.size(); ++i) {
        const S_lmTexture &rec = s_textures[i];
        if (totalArea > rec.freeArea)
            continue;
        found = rec;
        found.head = found.tail = nullptr;
        if (!CopyRects(rec, found)) {
            Discard(found);                 // (fix: the original drops the copied rectangles)
            return false;
        }
        bool fits = true;
        for (int k = 0; k < order.Size(); ++k) {
            const S_lightmapPart &p = parts[order[k]];
            if (found.freeArea < p.area || !Place(found, p)) {
                Discard(found);
                fits = false;
                break;
            }
        }
        if (!fits)
            continue;
        Discard(s_textures[i]);
        s_textures.erase(s_textures.begin() + static_cast<std::ptrdiff_t>(i));
        best = &found;
        break;
    }

    // Else a new texture: of the sizes the patches fit in, the one left with the least free
    // area; the patches halved while none.
    const uint32_t limit = (ig::g_init.flags & IG_SMALL_LIGHTMAPS) ? 0x100u : 0x200u;
    uint32_t maxHeight = static_cast<uint32_t>(ig::g_maxTextureHeight);
    if (maxHeight >= limit)
        maxHeight = limit;
    uint32_t maxWidth = static_cast<uint32_t>(ig::g_maxTextureWidth);
    if (maxWidth >= limit)
        maxWidth = limit;
    std::vector<S_lmTexture> candidates;
    while (!best) {
        for (int height = 0x80; height <= static_cast<int>(maxHeight); height *= 2) {
            for (int width = 0x80; width <= static_cast<int>(maxWidth); width *= 2) {
                if (totalArea > width * height)
                    continue;
                S_lmTexture rec;
                rec.width = static_cast<uint16_t>(width);
                rec.height = static_cast<uint16_t>(height);
                bool fits = true;
                for (int k = 0; k < order.Size(); ++k) {
                    if (!Place(rec, parts[order[k]])) {
                        Discard(rec);
                        fits = false;
                        break;
                    }
                }
                if (fits)
                    candidates.push_back(rec);
            }
        }
        for (S_lmTexture &c : candidates) {
            if (!best) {
                best = &c;
            } else if (c.freeArea < best->freeArea) {
                Discard(*best);
                best = &c;
            } else {
                Discard(c);
            }
        }
        if (best)
            break;
        dbgPrintf("m_CreateLM() - LM pro %s se nevesly do textury => zmensuji vsech %i LM na ctvrtinu.",
                  owner->name ? owner->name : "", order.Size());
        const int before = totalArea;
        for (int k = 0; k < order.Size(); ++k) {
            S_lightmapPart &p = parts[order[k]];
            if (p.width < 4 || p.height < 4)
                continue;
            totalArea -= p.area;
            const int16_t w = (p.width & ~1) < 8 ? 4 : static_cast<int16_t>(p.width >> 1);
            const int16_t h = (p.height & ~1) < 8 ? 4 : static_cast<int16_t>(p.height >> 1);
            uint8_t *resized = nullptr;
            g_pIGraph->ResizeImage(p.sourceWidth, p.sourceHeight, p.sourceWidth * 3, p.pixels, w, h, w * 3,
                                   &resized, 3, true);
            p.area = h * w;
            p.width = w;
            p.height = h;
            totalArea += p.area;
            ::operator delete(p.texels);
            p.texels = nullptr;
            ConvertTexels(static_cast<uint16_t>(w), static_cast<uint16_t>(h), resized, &p.texels);
            ::operator delete(resized);
        }
        if (totalArea == before) {
            dbgPrintf("LightMap::m_CreateLM() - ERROR!! There are no LMs for RESIZE !!");
            return false;
        }
    }

    const bool newTexture = !best->texture;
    if (newTexture) {
        texture = g_pIGraph->CreateITexture();
        if (!texture) {
            Discard(*best);
            return false;
        }
        const uint32_t flags = (LightmapsTrueColor() && ig::g_fmtTrueColor)
                                   ? (ITC_EMPTY | ITC_TRUECOLOR | 0x80000)
                                   : (ITC_EMPTY | ITC_ALPHA_1BIT | 0x80000);
        if (FAILED(texture->Create(nullptr, nullptr, flags, best->width, best->height, 0))) {
            Discard(*best);
            // (Fix: the original keeps the reference of the texture that failed.)
            texture->Release();
            texture = nullptr;
            return false;
        }
    } else {
        texture = best->texture;
        ++texture->refCount;
    }
    switch (texture->format) {
    case D3DFMT_R8G8B8:
    case D3DFMT_L8:
        *textureBytes += totalArea * 3;
        break;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        *textureBytes += totalArea * 4;
        break;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        *textureBytes += totalArea * 2;
        break;
    default:
        break;
    }
    if (newTexture)
        texture->Fill(nullptr, 0xffffffff, 0);
    int pitch = 0;
    auto *bits = static_cast<uint8_t *>(texture->Lock(nullptr, &pitch, 0));
    if (!bits) {
        Discard(*best);
        return false;
    }
    for (int i = 0; i < order.Size(); ++i)
        WritePart(texture, bits, pitch, *best, best->placements[i], parts[order[i]]);
    texture->Unlock();
    if (newTexture) {
        g_driver->lightmapTextures.PushBack(texture);
        g_driver->SlotB4();
        best->texture = texture;
    }
    for (const S_lmPlacement &pl : best->placements)
        placements.PushBack(pl);
    best->placements.clear();
    InsertSorted(*best);
    // The pixels are no longer needed (but a small patch's).
    for (int k = 0; k < order.Size(); ++k) {
        S_lightmapPart &p = parts[order[k]];
        if (p.area > 9) {
            ::operator delete(p.pixels);
            p.pixels = nullptr;
            ::operator delete(p.texels);
            p.texels = nullptr;
        }
    }
    return true;
}

}  // namespace ls3d::i3d
