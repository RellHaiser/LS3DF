// Generated from src/dx/d3d8.h: default implementations of the
// Direct3D 8 interfaces for test harnesses (every method fails or returns 0).
#pragma once

#include "../src/dx/d3d8.h"

#define FAKE_IUNKNOWN                                                                         \
    long refs = 1;                                                                            \
    HRESULT __stdcall QueryInterface(REFIID, void **out) override                            \
    {                                                                                         \
        *out = nullptr;                                                                       \
        return E_NOINTERFACE;                                                                 \
    }                                                                                         \
    ULONG __stdcall AddRef() override { return ++refs; }                                      \
    ULONG __stdcall Release() override                                                        \
    {                                                                                         \
        const long r = --refs;                                                                \
        if (!r)                                                                               \
            OnFinalRelease();                                                                 \
        return r;                                                                             \
    }                                                                                         \
    virtual void OnFinalRelease() {}

struct FakeDirect3D8Base : public IDirect3D8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall RegisterSoftwareDevice(void *pInitializeFunction) override { return E_NOTIMPL; }
    UINT __stdcall GetAdapterCount() override { return {}; }
    HRESULT __stdcall GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *pIdentifier) override { return E_NOTIMPL; }
    UINT __stdcall GetAdapterModeCount(UINT Adapter) override { return {}; }
    HRESULT __stdcall EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE *pMode) override { return E_NOTIMPL; }
    HRESULT __stdcall GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override { return E_NOTIMPL; }
    HRESULT __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) override { return E_NOTIMPL; }
    HRESULT __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return E_NOTIMPL; }
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) override { return E_NOTIMPL; }
    HRESULT __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 *pCaps) override { return E_NOTIMPL; }
    HMONITOR __stdcall GetAdapterMonitor(UINT Adapter) override { return {}; }
    HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice8 **ppReturnedDeviceInterface) override { return E_NOTIMPL; }
};

struct FakeDirect3DDevice8Base : public IDirect3DDevice8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall TestCooperativeLevel() override { return E_NOTIMPL; }
    UINT __stdcall GetAvailableTextureMem() override { return {}; }
    HRESULT __stdcall ResourceManagerDiscardBytes(DWORD Bytes) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDirect3D(IDirect3D8 **ppD3D8) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDeviceCaps(D3DCAPS8 *pCaps) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE *pMode) override { return E_NOTIMPL; }
    HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { return E_NOTIMPL; }
    HRESULT __stdcall SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) override { return E_NOTIMPL; }
    void __stdcall SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags) override { }
    BOOL __stdcall ShowCursor(BOOL bShow) override { return {}; }
    HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain8 **pSwapChain) override { return E_NOTIMPL; }
    HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) override { return E_NOTIMPL; }
    HRESULT __stdcall Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) override { return E_NOTIMPL; }
    HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer) override { return E_NOTIMPL; }
    HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override { return E_NOTIMPL; }
    void __stdcall SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp) override { }
    void __stdcall GetGammaRamp(D3DGAMMARAMP *pRamp) override { }
    HRESULT __stdcall CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface) override { return E_NOTIMPL; }
    HRESULT __stdcall CopyRects(IDirect3DSurface8 *pSourceSurface, const RECT *pSourceRectsArray, UINT cRects, IDirect3DSurface8 *pDestinationSurface, const POINT *pDestPointsArray) override { return E_NOTIMPL; }
    HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture) override { return E_NOTIMPL; }
    HRESULT __stdcall GetFrontBuffer(IDirect3DSurface8 *pDestSurface) override { return E_NOTIMPL; }
    HRESULT __stdcall SetRenderTarget(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil) override { return E_NOTIMPL; }
    HRESULT __stdcall GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) override { return E_NOTIMPL; }
    HRESULT __stdcall BeginScene() override { return E_NOTIMPL; }
    HRESULT __stdcall EndScene() override { return E_NOTIMPL; }
    HRESULT __stdcall Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { return E_NOTIMPL; }
    HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) override { return E_NOTIMPL; }
    HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override { return E_NOTIMPL; }
    HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) override { return E_NOTIMPL; }
    HRESULT __stdcall SetViewport(const D3DVIEWPORT8 *pViewport) override { return E_NOTIMPL; }
    HRESULT __stdcall GetViewport(D3DVIEWPORT8 *pViewport) override { return E_NOTIMPL; }
    HRESULT __stdcall SetMaterial(const D3DMATERIAL8 *pMaterial) override { return E_NOTIMPL; }
    HRESULT __stdcall GetMaterial(D3DMATERIAL8 *pMaterial) override { return E_NOTIMPL; }
    HRESULT __stdcall SetLight(DWORD Index, const D3DLIGHT8 *pLight) override { return E_NOTIMPL; }
    HRESULT __stdcall GetLight(DWORD Index, D3DLIGHT8 *pLight) override { return E_NOTIMPL; }
    HRESULT __stdcall LightEnable(DWORD Index, BOOL Enable) override { return E_NOTIMPL; }
    HRESULT __stdcall GetLightEnable(DWORD Index, BOOL *pEnable) override { return E_NOTIMPL; }
    HRESULT __stdcall SetClipPlane(DWORD Index, const float *pPlane) override { return E_NOTIMPL; }
    HRESULT __stdcall GetClipPlane(DWORD Index, float *pPlane) override { return E_NOTIMPL; }
    HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { return E_NOTIMPL; }
    HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override { return E_NOTIMPL; }
    HRESULT __stdcall BeginStateBlock() override { return E_NOTIMPL; }
    HRESULT __stdcall EndStateBlock(DWORD *pToken) override { return E_NOTIMPL; }
    HRESULT __stdcall ApplyStateBlock(DWORD Token) override { return E_NOTIMPL; }
    HRESULT __stdcall CaptureStateBlock(DWORD Token) override { return E_NOTIMPL; }
    HRESULT __stdcall DeleteStateBlock(DWORD Token) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD *pToken) override { return E_NOTIMPL; }
    HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS8 *pClipStatus) override { return E_NOTIMPL; }
    HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS8 *pClipStatus) override { return E_NOTIMPL; }
    HRESULT __stdcall GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture) override { return E_NOTIMPL; }
    HRESULT __stdcall SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture) override { return E_NOTIMPL; }
    HRESULT __stdcall GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override { return E_NOTIMPL; }
    HRESULT __stdcall SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { return E_NOTIMPL; }
    HRESULT __stdcall ValidateDevice(DWORD *pNumPasses) override { return E_NOTIMPL; }
    HRESULT __stdcall GetInfo(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override { return E_NOTIMPL; }
    HRESULT __stdcall SetCurrentTexturePalette(UINT PaletteNumber) override { return E_NOTIMPL; }
    HRESULT __stdcall GetCurrentTexturePalette(UINT *PaletteNumber) override { return E_NOTIMPL; }
    HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override { return E_NOTIMPL; }
    HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices, UINT startIndex, UINT primCount) override { return E_NOTIMPL; }
    HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return E_NOTIMPL; }
    HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return E_NOTIMPL; }
    HRESULT __stdcall ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall CreateVertexShader(const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage) override { return E_NOTIMPL; }
    HRESULT __stdcall SetVertexShader(DWORD Handle) override { return E_NOTIMPL; }
    HRESULT __stdcall GetVertexShader(DWORD *pHandle) override { return E_NOTIMPL; }
    HRESULT __stdcall DeleteVertexShader(DWORD Handle) override { return E_NOTIMPL; }
    HRESULT __stdcall SetVertexShaderConstant(DWORD Register, const void *pConstantData, DWORD ConstantCount) override { return E_NOTIMPL; }
    HRESULT __stdcall GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) override { return E_NOTIMPL; }
    HRESULT __stdcall GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) override { return E_NOTIMPL; }
    HRESULT __stdcall GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride) override { return E_NOTIMPL; }
    HRESULT __stdcall SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) override { return E_NOTIMPL; }
    HRESULT __stdcall GetIndices(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) override { return E_NOTIMPL; }
    HRESULT __stdcall CreatePixelShader(const DWORD *pFunction, DWORD *pHandle) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPixelShader(DWORD Handle) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPixelShader(DWORD *pHandle) override { return E_NOTIMPL; }
    HRESULT __stdcall DeletePixelShader(DWORD Handle) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPixelShaderConstant(DWORD Register, const void *pConstantData, DWORD ConstantCount) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo) override { return E_NOTIMPL; }
    HRESULT __stdcall DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) override { return E_NOTIMPL; }
    HRESULT __stdcall DeletePatch(UINT Handle) override { return E_NOTIMPL; }
};

struct FakeDirect3DSwapChain8Base : public IDirect3DSwapChain8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) override { return E_NOTIMPL; }
    HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer) override { return E_NOTIMPL; }
};

struct FakeDirect3DResource8Base : public IDirect3DResource8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
};

struct FakeDirect3DBaseTexture8Base : public IDirect3DBaseTexture8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
    DWORD __stdcall SetLOD(DWORD LODNew) override { return {}; }
    DWORD __stdcall GetLOD() override { return {}; }
    DWORD __stdcall GetLevelCount() override { return {}; }
};

struct FakeDirect3DTexture8Base : public IDirect3DTexture8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
    DWORD __stdcall SetLOD(DWORD LODNew) override { return {}; }
    DWORD __stdcall GetLOD() override { return {}; }
    DWORD __stdcall GetLevelCount() override { return {}; }
    HRESULT __stdcall GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override { return E_NOTIMPL; }
    HRESULT __stdcall GetSurfaceLevel(UINT Level, IDirect3DSurface8 **ppSurfaceLevel) override { return E_NOTIMPL; }
    HRESULT __stdcall LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall UnlockRect(UINT Level) override { return E_NOTIMPL; }
    HRESULT __stdcall AddDirtyRect(const RECT *pDirtyRect) override { return E_NOTIMPL; }
};

struct FakeDirect3DVolumeTexture8Base : public IDirect3DVolumeTexture8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
    DWORD __stdcall SetLOD(DWORD LODNew) override { return {}; }
    DWORD __stdcall GetLOD() override { return {}; }
    DWORD __stdcall GetLevelCount() override { return {}; }
    HRESULT __stdcall GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) override { return E_NOTIMPL; }
    HRESULT __stdcall GetVolumeLevel(UINT Level, IDirect3DVolume8 **ppVolumeLevel) override { return E_NOTIMPL; }
    HRESULT __stdcall LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall UnlockBox(UINT Level) override { return E_NOTIMPL; }
    HRESULT __stdcall AddDirtyBox(const D3DBOX *pDirtyBox) override { return E_NOTIMPL; }
};

struct FakeDirect3DCubeTexture8Base : public IDirect3DCubeTexture8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
    DWORD __stdcall SetLOD(DWORD LODNew) override { return {}; }
    DWORD __stdcall GetLOD() override { return {}; }
    DWORD __stdcall GetLevelCount() override { return {}; }
    HRESULT __stdcall GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override { return E_NOTIMPL; }
    HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface8 **ppCubeMapSurface) override { return E_NOTIMPL; }
    HRESULT __stdcall LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override { return E_NOTIMPL; }
    HRESULT __stdcall AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) override { return E_NOTIMPL; }
};

struct FakeDirect3DVertexBuffer8Base : public IDirect3DVertexBuffer8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
    HRESULT __stdcall Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall Unlock() override { return E_NOTIMPL; }
    HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override { return E_NOTIMPL; }
};

struct FakeDirect3DIndexBuffer8Base : public IDirect3DIndexBuffer8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    DWORD __stdcall SetPriority(DWORD PriorityNew) override { return {}; }
    DWORD __stdcall GetPriority() override { return {}; }
    void __stdcall PreLoad() override { }
    D3DRESOURCETYPE __stdcall GetType() override { return {}; }
    HRESULT __stdcall Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall Unlock() override { return E_NOTIMPL; }
    HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC *pDesc) override { return E_NOTIMPL; }
};

struct FakeDirect3DSurface8Base : public IDirect3DSurface8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    HRESULT __stdcall GetContainer(REFIID riid, void **ppContainer) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDesc(D3DSURFACE_DESC *pDesc) override { return E_NOTIMPL; }
    HRESULT __stdcall LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall UnlockRect() override { return E_NOTIMPL; }
};

struct FakeDirect3DVolume8Base : public IDirect3DVolume8 {
    FAKE_IUNKNOWN
    HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) override { return E_NOTIMPL; }
    HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return E_NOTIMPL; }
    HRESULT __stdcall FreePrivateData(REFGUID refguid) override { return E_NOTIMPL; }
    HRESULT __stdcall GetContainer(REFIID riid, void **ppContainer) override { return E_NOTIMPL; }
    HRESULT __stdcall GetDesc(D3DVOLUME_DESC *pDesc) override { return E_NOTIMPL; }
    HRESULT __stdcall LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) override { return E_NOTIMPL; }
    HRESULT __stdcall UnlockBox() override { return E_NOTIMPL; }
};

