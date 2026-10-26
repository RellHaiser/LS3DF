// The Direct3D 8 backend of the render device (gfx/gfx.h): each call goes to Direct3D 8 unchanged,
// so the engine makes exactly the Direct3D calls the original made.
#include "gfx/d3d8/d3d8_device.h"

// (The names are IGraph's, where the original kept them; compare_loader finds the port's device by
// this one.)
namespace ls3d::ig {

IDirect3D8 *g_pD3D = nullptr;
IDirect3DDevice8 *g_pDevice = nullptr;

}  // namespace ls3d::ig

namespace ls3d::gfx {

namespace {

using d3d8::D3D;
using ig::g_pD3D;
using ig::g_pDevice;

using Direct3DCreate8Fn = IDirect3D8 *(__stdcall *)(UINT);

// Out-parameters: `call` writes a Direct3D object, the caller gets its handle. (The object starts as
// the caller's handle, so a call that does not write it leaves the caller's variable as it was.)
template <typename H, typename Call>
HRESULT Out(H **handle, Call call)
{
    auto *object = D3D(*handle);
    const HRESULT hr = call(&object);
    *handle = d3d8::Handle(object);
    return hr;
}

class DeviceD3D8 final : public Device {
public:
    bool Open() override
    {
        // The original links d3d8.lib; the port binds Direct3DCreate8 at run time (the SDK has no
        // Direct3D 8 import library any more).
        if (HMODULE d3d8 = LoadLibraryA("d3d8.dll")) {
            if (auto create = reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8, "Direct3DCreate8")))
                g_pD3D = create(D3D_SDK_VERSION);
        }
        return g_pD3D != nullptr;
    }
    bool IsOpen() override { return g_pD3D != nullptr; }
    void Close() override
    {
        g_pD3D->Release();
        g_pD3D = nullptr;
    }
    UINT GetAdapterModeCount(UINT adapter) override { return g_pD3D->GetAdapterModeCount(adapter); }
    HRESULT EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE *displayMode) override
    {
        return g_pD3D->EnumAdapterModes(adapter, mode, displayMode);
    }
    HRESULT GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *displayMode) override
    {
        return g_pD3D->GetAdapterDisplayMode(adapter, displayMode);
    }
    HRESULT GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8 *identifier) override
    {
        return g_pD3D->GetAdapterIdentifier(adapter, flags, identifier);
    }
    HRESULT GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8 *caps) override
    {
        return g_pD3D->GetDeviceCaps(adapter, type, caps);
    }
    HRESULT CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                              D3DRESOURCETYPE resourceType, D3DFORMAT format) override
    {
        return g_pD3D->CheckDeviceFormat(adapter, type, adapterFormat, usage, resourceType, format);
    }
    HRESULT CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                   D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) override
    {
        return g_pD3D->CheckDepthStencilMatch(adapter, type, adapterFormat, renderTargetFormat, depthStencilFormat);
    }
    HRESULT CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT surfaceFormat, BOOL windowed,
                                       D3DMULTISAMPLE_TYPE samples) override
    {
        return g_pD3D->CheckDeviceMultiSampleType(adapter, type, surfaceFormat, windowed, samples);
    }

    HRESULT CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behaviorFlags,
                         D3DPRESENT_PARAMETERS *params) override
    {
        return g_pD3D->CreateDevice(adapter, type, focusWindow, behaviorFlags, params, &g_pDevice);
    }
    bool HasDevice() override { return g_pDevice != nullptr; }
    void ReleaseDevice() override
    {
        g_pDevice->Release();
        g_pDevice = nullptr;
    }
    HRESULT Reset(D3DPRESENT_PARAMETERS *params) override { return g_pDevice->Reset(params); }
    HRESULT TestCooperativeLevel() override { return g_pDevice->TestCooperativeLevel(); }
    HRESULT Present() override { return g_pDevice->Present(nullptr, nullptr, nullptr, nullptr); }
    void SetGammaRamp(DWORD flags, const D3DGAMMARAMP *ramp) override { g_pDevice->SetGammaRamp(flags, ramp); }
    void GetGammaRamp(D3DGAMMARAMP *ramp) override { g_pDevice->GetGammaRamp(ramp); }

    HRESULT BeginScene() override { return g_pDevice->BeginScene(); }
    HRESULT EndScene() override { return g_pDevice->EndScene(); }
    HRESULT Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override
    {
        return g_pDevice->Clear(count, rects, flags, color, z, stencil);
    }
    UINT GetAvailableTextureMem() override { return g_pDevice->GetAvailableTextureMem(); }
    HRESULT ResourceManagerDiscardBytes(DWORD bytes) override { return g_pDevice->ResourceManagerDiscardBytes(bytes); }

    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override
    {
        return g_pDevice->SetRenderState(state, value);
    }
    HRESULT GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) override
    {
        return g_pDevice->GetRenderState(state, value);
    }
    HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override
    {
        return g_pDevice->SetTextureStageState(stage, type, value);
    }
    HRESULT SetTexture(DWORD stage, Texture *texture) override { return g_pDevice->SetTexture(stage, D3D(texture)); }
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override
    {
        return g_pDevice->SetTransform(state, matrix);
    }
    HRESULT SetViewport(const D3DVIEWPORT8 *viewport) override { return g_pDevice->SetViewport(viewport); }
    HRESULT SetMaterial(const D3DMATERIAL8 *material) override { return g_pDevice->SetMaterial(material); }
    HRESULT SetLight(DWORD index, const D3DLIGHT8 *light) override { return g_pDevice->SetLight(index, light); }
    HRESULT LightEnable(DWORD index, BOOL enable) override { return g_pDevice->LightEnable(index, enable); }
    HRESULT CreateVertexShader(const DWORD *declaration, const DWORD *function, DWORD *handle, DWORD usage) override
    {
        return g_pDevice->CreateVertexShader(declaration, function, handle, usage);
    }
    HRESULT SetVertexShader(DWORD handle) override { return g_pDevice->SetVertexShader(handle); }
    HRESULT SetVertexShaderConstant(DWORD reg, const void *data, DWORD count) override
    {
        return g_pDevice->SetVertexShaderConstant(reg, data, count);
    }
    HRESULT SetStreamSource(UINT stream, VertexBuffer *buffer, UINT stride) override
    {
        return g_pDevice->SetStreamSource(stream, D3D(buffer), stride);
    }
    HRESULT SetIndices(IndexBuffer *buffer, UINT baseVertexIndex) override
    {
        return g_pDevice->SetIndices(D3D(buffer), baseVertexIndex);
    }

    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT startIndex,
                                 UINT primitiveCount) override
    {
        return g_pDevice->DrawIndexedPrimitive(type, minIndex, numVertices, startIndex, primitiveCount);
    }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void *vertices, UINT stride) override
    {
        return g_pDevice->DrawPrimitiveUP(type, primitiveCount, vertices, stride);
    }
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT primitiveCount,
                                   const void *indices, D3DFORMAT indexFormat, const void *vertices,
                                   UINT stride) override
    {
        return g_pDevice->DrawIndexedPrimitiveUP(type, minIndex, numVertices, primitiveCount, indices, indexFormat,
                                                 vertices, stride);
    }

    HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                          Texture **texture) override
    {
        return Out(texture, [&](auto *p) {
            return g_pDevice->CreateTexture(width, height, levels, usage, format, pool, p);
        });
    }
    HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, Surface **surface) override
    {
        return Out(surface, [&](auto *p) { return g_pDevice->CreateImageSurface(width, height, format, p); });
    }
    HRESULT UpdateTexture(Texture *source, Texture *destination) override
    {
        return g_pDevice->UpdateTexture(D3D(source), D3D(destination));
    }
    HRESULT CopyRects(Surface *source, const RECT *sourceRects, UINT count, Surface *destination,
                      const POINT *destinationPoints) override
    {
        return g_pDevice->CopyRects(D3D(source), sourceRects, count, D3D(destination), destinationPoints);
    }
    HRESULT GetRenderTarget(Surface **surface) override
    {
        return Out(surface, [&](auto *p) { return g_pDevice->GetRenderTarget(p); });
    }
    HRESULT GetDepthStencilSurface(Surface **surface) override
    {
        return Out(surface, [&](auto *p) { return g_pDevice->GetDepthStencilSurface(p); });
    }

    ULONG AddRef(Texture *texture) override { return D3D(texture)->AddRef(); }
    ULONG Release(Texture *texture) override { return D3D(texture)->Release(); }
    DWORD GetLevelCount(Texture *texture) override { return D3D(texture)->GetLevelCount(); }
    HRESULT GetLevelDesc(Texture *texture, UINT level, D3DSURFACE_DESC *desc) override
    {
        return D3D(texture)->GetLevelDesc(level, desc);
    }
    HRESULT GetSurfaceLevel(Texture *texture, UINT level, Surface **surface) override
    {
        return Out(surface, [&](auto *p) { return D3D(texture)->GetSurfaceLevel(level, p); });
    }

    ULONG Release(Surface *surface) override { return D3D(surface)->Release(); }
    HRESULT GetDesc(Surface *surface, D3DSURFACE_DESC *desc) override { return D3D(surface)->GetDesc(desc); }
    HRESULT LockRect(Surface *surface, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override
    {
        return D3D(surface)->LockRect(locked, rect, flags);
    }
    HRESULT UnlockRect(Surface *surface) override { return D3D(surface)->UnlockRect(); }

    HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, VertexBuffer **buffer) override
    {
        return Out(buffer, [&](auto *p) { return g_pDevice->CreateVertexBuffer(length, usage, fvf, pool, p); });
    }
    HRESULT CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                              IndexBuffer **buffer) override
    {
        return Out(buffer, [&](auto *p) { return g_pDevice->CreateIndexBuffer(length, usage, format, pool, p); });
    }
    ULONG Release(VertexBuffer *buffer) override { return D3D(buffer)->Release(); }
    HRESULT Lock(VertexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags) override
    {
        return D3D(buffer)->Lock(offset, size, data, flags);
    }
    HRESULT Unlock(VertexBuffer *buffer) override { return D3D(buffer)->Unlock(); }
    ULONG Release(IndexBuffer *buffer) override { return D3D(buffer)->Release(); }
    HRESULT Lock(IndexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags) override
    {
        return D3D(buffer)->Lock(offset, size, data, flags);
    }
    HRESULT Unlock(IndexBuffer *buffer) override { return D3D(buffer)->Unlock(); }
};

DeviceD3D8 s_device;

}  // namespace

Device *g_pGfx = &s_device;

}  // namespace ls3d::gfx
