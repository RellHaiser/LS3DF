// The Direct3D 8 backend's own declarations: its Direct3D objects and the handles' conversions. Only
// the backend (and the tests, which put fake devices in) include this.
#pragma once

#include "dx/d3d8.h"
#include "gfx/gfx.h"

namespace ls3d::ig {

extern IDirect3D8 *g_pD3D;                      // 0x100d1a94
extern IDirect3DDevice8 *g_pDevice;             // 0x100d216c

}  // namespace ls3d::ig

namespace ls3d::gfx::d3d8 {

// The backend's handles are the Direct3D objects.
inline IDirect3DTexture8 *D3D(Texture *t) { return reinterpret_cast<IDirect3DTexture8 *>(t); }
inline IDirect3DSurface8 *D3D(Surface *s) { return reinterpret_cast<IDirect3DSurface8 *>(s); }
inline IDirect3DVertexBuffer8 *D3D(VertexBuffer *b) { return reinterpret_cast<IDirect3DVertexBuffer8 *>(b); }
inline IDirect3DIndexBuffer8 *D3D(IndexBuffer *b) { return reinterpret_cast<IDirect3DIndexBuffer8 *>(b); }
inline Texture *Handle(IDirect3DTexture8 *t) { return reinterpret_cast<Texture *>(t); }
inline Surface *Handle(IDirect3DSurface8 *s) { return reinterpret_cast<Surface *>(s); }
inline VertexBuffer *Handle(IDirect3DVertexBuffer8 *b) { return reinterpret_cast<VertexBuffer *>(b); }
inline IndexBuffer *Handle(IDirect3DIndexBuffer8 *b) { return reinterpret_cast<IndexBuffer *>(b); }

}  // namespace ls3d::gfx::d3d8
