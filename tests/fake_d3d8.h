// A minimal software Direct3D 8 device for differential tests: textures are levels of system
// memory, everything else is accepted and ignored. Shared by the original DLL (its g_pDevice set
// to the fake) and the port.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "fake_d3d8_base.h"

struct FakeSurface;
struct FakeTexture;

inline UINT FakeBytesPerPixel(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_L8:
    case D3DFMT_A8: return 1;
    default: return 2;
    }
}

inline bool FakeIsDxt(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

struct FakeSurface : FakeDirect3DSurface8Base {
    D3DSURFACE_DESC desc = {};
    INT pitch = 0;
    std::vector<uint8_t> bits;
    int locks = 0;

    FakeSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool)
    {
        desc.Format = fmt;
        desc.Type = D3DRTYPE_SURFACE;
        desc.Pool = pool;
        desc.Width = w;
        desc.Height = h;
        if (FakeIsDxt(fmt)) {
            const UINT bw = (w + 3) / 4, bh = (h + 3) / 4;     // whole 4 x 4 blocks, as Direct3D
            pitch = static_cast<INT>(bw * (fmt == D3DFMT_DXT1 ? 8 : 16));
            desc.Size = pitch * bh;
        } else {
            pitch = static_cast<INT>(w * FakeBytesPerPixel(fmt));
            desc.Size = pitch * h;
        }
        bits.assign(desc.Size, 0xcd);
    }
    HRESULT __stdcall GetDesc(D3DSURFACE_DESC *out) override
    {
        *out = desc;
        return 0;
    }
    HRESULT __stdcall LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD) override
    {
        ++locks;
        lr->Pitch = pitch;
        uint8_t *p = bits.data();
        if (rect && !FakeIsDxt(desc.Format))
            p += rect->top * pitch + rect->left * FakeBytesPerPixel(desc.Format);
        lr->pBits = p;
        return 0;
    }
    HRESULT __stdcall UnlockRect() override
    {
        --locks;
        return 0;
    }
};

struct FakeTexture : FakeDirect3DTexture8Base {
    std::vector<FakeSurface *> levels;
    D3DFORMAT format;
    D3DPOOL pool;

    FakeTexture(UINT w, UINT h, UINT count, D3DFORMAT fmt, D3DPOOL p) : format(fmt), pool(p)
    {
        if (!count) {
            count = 1;
            for (UINT a = w, b = h; a > 1 || b > 1; a >>= 1, b >>= 1)
                ++count;
        }
        for (UINT i = 0; i < count; ++i) {
            levels.push_back(new FakeSurface(w ? w : 1, h ? h : 1, fmt, p));
            w >>= 1;
            h >>= 1;
            if (!w && !h)
                break;
            if (!w)
                w = 1;
            if (!h)
                h = 1;
        }
    }
    void OnFinalRelease() override
    {
        for (FakeSurface *s : levels)
            s->Release();
        levels.clear();
        // Kept alive (leaked) so that tests can still inspect released textures.
    }
    DWORD __stdcall GetLevelCount() override { return static_cast<DWORD>(levels.size()); }
    HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC *out) override
    {
        if (level >= levels.size())
            return D3DERR_INVALIDCALL;
        *out = levels[level]->desc;
        return 0;
    }
    HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface8 **out) override
    {
        if (level >= levels.size())
            return D3DERR_INVALIDCALL;
        levels[level]->AddRef();
        *out = levels[level];
        return 0;
    }
    HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override
    {
        return levels[level]->LockRect(lr, rect, flags);
    }
    HRESULT __stdcall UnlockRect(UINT level) override { return levels[level]->UnlockRect(); }
};

struct FakeDevice : FakeDirect3DDevice8Base {
    std::vector<FakeTexture *> textures;        // every texture created (owned by the test)
    int draws = 0;

    HRESULT __stdcall CreateTexture(UINT w, UINT h, UINT levels, DWORD, D3DFORMAT fmt, D3DPOOL pool,
                                    IDirect3DTexture8 **out) override
    {
        if (fmt == D3DFMT_UNKNOWN) {
            *out = nullptr;
            return D3DERR_INVALIDCALL;
        }
        FakeTexture *t = new FakeTexture(w, h, levels, fmt, pool);
        textures.push_back(t);
        *out = t;
        return 0;
    }
    HRESULT __stdcall CopyRects(IDirect3DSurface8 *src, const RECT *, UINT, IDirect3DSurface8 *dst, const POINT *) override
    {
        FakeSurface *s = static_cast<FakeSurface *>(src), *d = static_cast<FakeSurface *>(dst);
        std::memcpy(d->bits.data(), s->bits.data(), s->bits.size() < d->bits.size() ? s->bits.size() : d->bits.size());
        return 0;
    }
    HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8 *src, IDirect3DBaseTexture8 *dst) override
    {
        FakeTexture *s = static_cast<FakeTexture *>(src), *d = static_cast<FakeTexture *>(dst);
        for (size_t i = 0; i < s->levels.size() && i < d->levels.size(); ++i)
            CopyRects(s->levels[i], nullptr, 0, d->levels[i], nullptr);
        return 0;
    }
    HRESULT __stdcall BeginScene() override { return 0; }
    HRESULT __stdcall EndScene() override { return 0; }
    HRESULT __stdcall SetTexture(DWORD, IDirect3DBaseTexture8 *) override { return 0; }
    HRESULT __stdcall SetVertexShader(DWORD) override { return 0; }
    HRESULT __stdcall SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return 0; }
    HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE, DWORD) override { return 0; }
    HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT) override
    {
        ++draws;
        return 0;
    }
};
