// The render device: everything the engine draws goes through gfx::Device (g_pGfx).
//
// The engine describes rendering in Direct3D 8's fixed-function terms (dx/d3d8_types.h): render
// states, texture stage combiners, lights, fog, transforms, vertex formats. That vocabulary stays:
// the engine's look is defined by those settings, so a backend has to reproduce exactly that
// model whatever API it draws with. What the device hides is the API itself: no engine code
// outside gfx/<backend> sees a Direct3D interface. It owns the API and the device's life too:
// IGraph asks it for the adapter's display modes, formats and capabilities (Direct3D 8's terms
// again), and creates, resets and presents the device through it.
//
// Resources (textures, surfaces, vertex and index buffers) are opaque handles that only the
// backend looks inside; the engine stores them where the original stored Direct3D pointers (the
// game reads some of those structures directly, so they keep their size and place). The Direct3D 8
// backend's handles are the Direct3D objects themselves.
//
// The methods mean what their Direct3D 8 namesakes mean, and only those the engine calls are here.
#pragma once

#include "dx/d3d8_types.h"

namespace ls3d::gfx {

// Opaque resource handles (defined only by a backend).
struct Texture;
struct Surface;
struct VertexBuffer;
struct IndexBuffer;

class Device {
public:
    virtual ~Device() = default;

    // ---- the API and its adapters (no device needed)
    // Loads the API; false when it is not available.
    virtual bool Open() = 0;
    virtual bool IsOpen() = 0;
    // Releases the API (after the device).
    virtual void Close() = 0;
    virtual UINT GetAdapterModeCount(UINT adapter) = 0;
    virtual HRESULT EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE *displayMode) = 0;
    virtual HRESULT GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *displayMode) = 0;
    virtual HRESULT GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8 *identifier) = 0;
    virtual HRESULT GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8 *caps) = 0;
    virtual HRESULT CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                      D3DRESOURCETYPE resourceType, D3DFORMAT format) = 0;
    virtual HRESULT CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                           D3DFORMAT renderTargetFormat, D3DFORMAT depthStencilFormat) = 0;
    virtual HRESULT CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT surfaceFormat,
                                               BOOL windowed, D3DMULTISAMPLE_TYPE samples) = 0;

    // ---- the device
    virtual HRESULT CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behaviorFlags,
                                 D3DPRESENT_PARAMETERS *params) = 0;
    virtual bool HasDevice() = 0;
    virtual void ReleaseDevice() = 0;
    virtual HRESULT Reset(D3DPRESENT_PARAMETERS *params) = 0;
    virtual HRESULT TestCooperativeLevel() = 0;
    // Shows the whole back buffer in the device window.
    virtual HRESULT Present() = 0;
    virtual void SetGammaRamp(DWORD flags, const D3DGAMMARAMP *ramp) = 0;
    virtual void GetGammaRamp(D3DGAMMARAMP *ramp) = 0;

    // ---- frame
    virtual HRESULT BeginScene() = 0;
    virtual HRESULT EndScene() = 0;
    virtual HRESULT Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) = 0;
    virtual UINT GetAvailableTextureMem() = 0;
    virtual HRESULT ResourceManagerDiscardBytes(DWORD bytes) = 0;

    // ---- states
    virtual HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;
    virtual HRESULT GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) = 0;
    virtual HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;
    virtual HRESULT SetTexture(DWORD stage, Texture *texture) = 0;
    virtual HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) = 0;
    virtual HRESULT SetViewport(const D3DVIEWPORT8 *viewport) = 0;
    virtual HRESULT SetMaterial(const D3DMATERIAL8 *material) = 0;
    virtual HRESULT SetLight(DWORD index, const D3DLIGHT8 *light) = 0;
    virtual HRESULT LightEnable(DWORD index, BOOL enable) = 0;
    // A vertex format: an FVF code or a handle CreateVertexShader returned.
    virtual HRESULT CreateVertexShader(const DWORD *declaration, const DWORD *function, DWORD *handle,
                                       DWORD usage) = 0;
    virtual HRESULT SetVertexShader(DWORD handle) = 0;
    virtual HRESULT SetVertexShaderConstant(DWORD reg, const void *data, DWORD count) = 0;
    virtual HRESULT SetStreamSource(UINT stream, VertexBuffer *buffer, UINT stride) = 0;
    virtual HRESULT SetIndices(IndexBuffer *buffer, UINT baseVertexIndex) = 0;

    // ---- drawing
    virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT startIndex,
                                         UINT primitiveCount) = 0;
    virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void *vertices,
                                    UINT stride) = 0;
    virtual HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices,
                                           UINT primitiveCount, const void *indices, D3DFORMAT indexFormat,
                                           const void *vertices, UINT stride) = 0;

    // ---- textures and surfaces
    virtual HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                  D3DPOOL pool, Texture **texture) = 0;
    virtual HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, Surface **surface) = 0;
    virtual HRESULT UpdateTexture(Texture *source, Texture *destination) = 0;
    virtual HRESULT CopyRects(Surface *source, const RECT *sourceRects, UINT count, Surface *destination,
                              const POINT *destinationPoints) = 0;
    virtual HRESULT GetRenderTarget(Surface **surface) = 0;
    virtual HRESULT GetDepthStencilSurface(Surface **surface) = 0;

    virtual ULONG AddRef(Texture *texture) = 0;
    virtual ULONG Release(Texture *texture) = 0;
    virtual DWORD GetLevelCount(Texture *texture) = 0;
    virtual HRESULT GetLevelDesc(Texture *texture, UINT level, D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT GetSurfaceLevel(Texture *texture, UINT level, Surface **surface) = 0;

    virtual ULONG Release(Surface *surface) = 0;
    virtual HRESULT GetDesc(Surface *surface, D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT LockRect(Surface *surface, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) = 0;
    virtual HRESULT UnlockRect(Surface *surface) = 0;

    // ---- vertex and index buffers
    virtual HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, VertexBuffer **buffer) = 0;
    virtual HRESULT CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                      IndexBuffer **buffer) = 0;
    virtual ULONG Release(VertexBuffer *buffer) = 0;
    virtual HRESULT Lock(VertexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags) = 0;
    virtual HRESULT Unlock(VertexBuffer *buffer) = 0;
    virtual ULONG Release(IndexBuffer *buffer) = 0;
    virtual HRESULT Lock(IndexBuffer *buffer, UINT offset, UINT size, BYTE **data, DWORD flags) = 0;
    virtual HRESULT Unlock(IndexBuffer *buffer) = 0;
};

// The device in use (the Direct3D 8 backend's, gfx/d3d8).
extern Device *g_pGfx;

}  // namespace ls3d::gfx
