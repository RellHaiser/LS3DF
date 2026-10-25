// I3D_driver's debug drawing (slots 0x24 - 0x4C, 0x10016480 - 0x10018ae0): points, lines,
// triangles, circles, text in the resource font, sprites, boxes and spheres, drawn at once with
// the device (between the caller's BeginScene / EndScene).
#include <cmath>
#include <cstring>

#include "core/fpu.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/material.h"
#include "i3d/render_helpers.h"
#include "i3d/render_state.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using ig::g_pDevice;

namespace ls3d::i3d {
extern I3D_material *g_resourceMaterial;
}

namespace {

// Vertex format 2: position, color, one texture coordinate pair.
struct S_colorVertex {
    S_vector pos;
    uint32_t color;
    float u, v;
};
static_assert(sizeof(S_colorVertex) == 0x18, "S_colorVertex layout");

// Vertex format 6: transformed position, color, specular, one texture coordinate pair.
struct S_screenVertex {
    float x, y, z, rhw;
    uint32_t color;
    uint32_t specular;
    float u, v;
};
static_assert(sizeof(S_screenVertex) == 0x20, "S_screenVertex layout");

// The vertices of the lists drawn (0x100adea0 / 0x100be01c, static C_vector objects).
C_vector<S_colorVertex> s_vertices;
C_vector<S_screenVertex> s_screenVertices;

// Three unit circles of 16 points around z, y and x (0x100adba0, static initializer 0x10018600)
// and the lines joining each circle's points (0x100a6368).
const S_vector4 kSphereVertices[48] = {
    {1.0f, 0.0f, 0.0f, 0.0f}, {0.92f, 0.39f, 0.0f, 0.0f}, {0.71f, 0.71f, 0.0f, 0.0f}, {0.39f, 0.92f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.0f}, {-0.39f, 0.92f, 0.0f, 0.0f}, {-0.71f, 0.71f, 0.0f, 0.0f}, {-0.92f, 0.39f, 0.0f, 0.0f},
    {-1.0f, 0.0f, 0.0f, 0.0f}, {-0.92f, -0.39f, 0.0f, 0.0f}, {-0.71f, -0.71f, 0.0f, 0.0f}, {-0.39f, -0.92f, 0.0f, 0.0f},
    {0.0f, -1.0f, 0.0f, 0.0f}, {0.39f, -0.92f, 0.0f, 0.0f}, {0.71f, -0.71f, 0.0f, 0.0f}, {0.92f, -0.39f, 0.0f, 0.0f},
    {1.0f, 0.0f, 0.0f, 0.0f}, {0.92f, 0.0f, 0.39f, 0.0f}, {0.71f, 0.0f, 0.71f, 0.0f}, {0.39f, 0.0f, 0.92f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f}, {-0.39f, 0.0f, 0.92f, 0.0f}, {-0.71f, 0.0f, 0.71f, 0.0f}, {-0.92f, 0.0f, 0.39f, 0.0f},
    {-1.0f, 0.0f, 0.0f, 0.0f}, {-0.92f, 0.0f, -0.39f, 0.0f}, {-0.71f, 0.0f, -0.71f, 0.0f}, {-0.39f, 0.0f, -0.92f, 0.0f},
    {0.0f, 0.0f, -1.0f, 0.0f}, {0.39f, 0.0f, -0.92f, 0.0f}, {0.71f, 0.0f, -0.71f, 0.0f}, {0.92f, 0.0f, -0.39f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.39f, 0.92f, 0.0f}, {0.0f, 0.71f, 0.71f, 0.0f}, {0.0f, 0.92f, 0.39f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.92f, -0.39f, 0.0f}, {0.0f, 0.71f, -0.71f, 0.0f}, {0.0f, 0.39f, -0.92f, 0.0f},
    {0.0f, 0.0f, -1.0f, 0.0f}, {0.0f, -0.39f, -0.92f, 0.0f}, {0.0f, -0.71f, -0.71f, 0.0f}, {0.0f, -0.92f, -0.39f, 0.0f},
    {0.0f, -1.0f, 0.0f, 0.0f}, {0.0f, -0.92f, 0.39f, 0.0f}, {0.0f, -0.71f, 0.71f, 0.0f}, {0.0f, -0.39f, 0.92f, 0.0f},
};
const uint16_t kSphereIndices[96] = {
    0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 0,
    16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30, 30, 31, 31, 16,
    32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 32,
};
// A box's edges and faces by corner (BoxCorners' order).
const uint16_t kBoxLines[24] = {0, 1, 0, 2, 0, 4, 1, 3, 1, 5, 2, 3, 2, 6, 3, 7, 4, 5, 4, 6, 5, 7, 6, 7};
const uint16_t kBoxFaces[36] = {
    0, 1, 2, 2, 1, 3, 4, 6, 5, 5, 6, 7, 0, 2, 4, 2, 6, 4, 1, 5, 3, 5, 7, 3, 0, 4, 1, 1, 4, 5, 2, 3, 6, 3, 7, 6,
};
// The sphere's matrix (0x100adf40).
S_matrix s_sphereMatrix;

// The opacity of the flags' transparency byte (stored as a float).
float FlagsAlpha(uint32_t flags)
{
    return static_cast<float>(static_cast<int>(0xff - (flags & 0xff))) * (1.0f / 255.0f);
}

// A D3DCOLOR of 0 - 1 components.
uint32_t PackColor(float alpha, float r, float g, float b)
{
    const int ia = Fistp(alpha * 255.0f);
    const int ir = Fistp(r * 255.0f);
    const int ig = Fistp(g * 255.0f);
    const int ib = Fistp(b * 255.0f);
    return static_cast<uint32_t>(((ia << 8 | ir) << 8 | ig) << 8 | ib);
}

// The states every helper sets: modulated color and alpha, blending unless opaque.
void SetDrawStates(float alpha)
{
    SetColorOp(0, D3DTOP_MODULATE);
    SetAlphaOp(0, D3DTOP_MODULATE);
    SetAlphaState(IsTinyBits(std::fabs(alpha - 1.0f)) ? 0 : 1);
}

// The material's texture (null without one).
IDirect3DBaseTexture8 *D3DTexture(const I3D_material *m) { return m->texture ? m->texture->d3dTexture : nullptr; }

// The render camera's world matrix moved to `pos` set as the world matrix (text and sprites face
// the camera); false without a render camera.
bool SetFacingMatrix(const S_vector &pos)
{
    if (!g_renderCamera)
        return false;
    S_matrix m = g_renderCamera->GetMatrix();
    m(3) = pos;
    g_pIGraph->SetWorldMatrix(&m);
    return true;
}

}  // namespace

// 0x10016480
I3D_RESULT I3D_driver::DrawPoint(const S_vector &pos, const S_vector &color, uint32_t flags)
{
    DisableFog();
    const float alpha = FlagsAlpha(flags);
    S_colorVertex v = {pos, PackColor(alpha, color.x, color.y, color.z), 0.0f, 0.0f};
    SetDrawStates(alpha);
    SetBlendState(0);
    g_pDevice->SetTexture(0, nullptr);
    SetVertexFormat(2);
    return g_pDevice->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &v, sizeof v);
}

// 0x100166a0
I3D_RESULT I3D_driver::DrawPoint2D(int x, int y, const S_vector &color, uint32_t flags)
{
    DisableFog();
    const float alpha = FlagsAlpha(flags);
    S_screenVertex v = {static_cast<float>(x), static_cast<float>(y), 0.0f, 1.0f,
                        PackColor(alpha, color.x, color.y, color.z), 0, 0.0f, 0.0f};
    SetDrawStates(alpha);
    SetBlendState(0);
    g_pDevice->SetTexture(0, nullptr);
    SetVertexFormat(6);
    return g_pDevice->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &v, sizeof v);
}

// 0x100168c0
I3D_RESULT I3D_driver::DrawLine(const S_vector &a, const S_vector &b, const S_vector &color, uint32_t flags)
{
    DisableFog();
    const float alpha = FlagsAlpha(flags);
    const uint32_t c = PackColor(alpha, color.x, color.y, color.z);
    S_colorVertex v[2] = {{a, c, 0.0f, 0.0f}, {b, c, 0.0f, 0.0f}};
    SetDrawStates(alpha);
    SetBlendState(0);
    g_pDevice->SetTexture(0, nullptr);
    SetVertexFormat(2);
    return g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof v[0]);
}

// 0x10016b00
I3D_RESULT I3D_driver::DrawLines(const void *positions, int numVerts, const uint16_t *indices, int numIndices,
                                 const S_vector &color, int stride, uint32_t flags)
{
    DisableFog();
    const float alpha = FlagsAlpha(flags);
    const uint32_t c = PackColor(alpha, color.x, color.y, color.z);
    SetDrawStates(alpha);
    SetBlendState(0);
    g_pDevice->SetTexture(0, nullptr);
    SetVertexFormat(2);
    s_vertices.ResizeDiscard(numVerts);
    const auto *src = static_cast<const uint8_t *>(positions);
    for (int i = 0; i < numVerts; ++i, src += stride) {
        std::memcpy(&s_vertices[i].pos, src, sizeof(S_vector));
        s_vertices[i].color = c;
    }
    return g_pDevice->DrawIndexedPrimitiveUP(D3DPT_LINELIST, 0, numVerts, static_cast<UINT>(numIndices) >> 1, indices,
                                             D3DFMT_INDEX16, s_vertices.Data(), sizeof(S_colorVertex));
}

// 0x10016d50
I3D_RESULT I3D_driver::DrawTriangles(const void *positions, int numVerts, const uint16_t *indices, int numIndices,
                                     const S_vector &color, int stride, uint32_t flags, const float *uvs)
{
    DisableFog();
    const float alpha = FlagsAlpha(flags);
    const uint32_t c = PackColor(alpha, color.x, color.y, color.z);
    SetDrawStates(alpha);
    SetBlendState(0);
    if (flags & DRAWF_NO_CULL)
        SetCullState(1);
    else if (flags & DRAWF_CULL_CW)
        SetCullState(2);
    else
        SetCullState(3);
    const auto *src = static_cast<const uint8_t *>(positions);
    const auto *uv = reinterpret_cast<const uint8_t *>(uvs);
    const UINT numFaces = static_cast<UINT>(numIndices) / 3;
    if (!(flags & DRAWF_SCREEN)) {
        s_vertices.ResizeDiscard(numVerts);
        for (int i = 0; i < numVerts; ++i, src += stride) {
            S_colorVertex &v = s_vertices[i];
            std::memcpy(&v.pos, src, sizeof(S_vector));
            v.color = c;
            if (uvs) {
                std::memcpy(&v.u, uv, 8);
                uv += stride;
            }
        }
        if (!uvs)
            g_pDevice->SetTexture(0, nullptr);
        SetVertexFormat(2);
        g_pDevice->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, numVerts, numFaces, indices, D3DFMT_INDEX16,
                                          s_vertices.Data(), sizeof(S_colorVertex));
    } else {
        s_screenVertices.ResizeDiscard(numVerts);
        for (int i = 0; i < numVerts; ++i, src += stride) {
            S_screenVertex &v = s_screenVertices[i];
            std::memcpy(&v.x, src, 16);
            v.color = c;
            v.specular = 0;
            if (uvs) {
                std::memcpy(&v.u, uv, 8);
                uv += stride;
            }
        }
        if (!uvs)
            g_pDevice->SetTexture(0, nullptr);
        SetVertexFormat(6);
        g_pDevice->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, numVerts, numFaces, indices, D3DFMT_INDEX16,
                                          s_screenVertices.Data(), sizeof(S_screenVertex));
    }
    if (flags & (DRAWF_NO_CULL | DRAWF_CULL_CW))
        SetCullState(3);
    return I3D_OK;
}

// 0x100172d0
I3D_RESULT I3D_driver::DrawCircle2D(const float *center, float radius, const S_vector &color, int segments,
                                    uint32_t flags)
{
    DisableFog();
    const float alpha = FlagsAlpha(flags);
    const uint32_t c = PackColor(alpha, color.x, color.y, color.z);
    SetDrawStates(alpha);
    SetBlendState(0);
    const S_matrix step = RotateZMatrix(6.28318548f / static_cast<float>(segments));
    s_screenVertices.ResizeDiscard(segments + 1);
    S_vector offset(0.0f, -radius, 0.0f);
    for (int i = 0; i <= segments; ++i) {
        S_screenVertex &v = s_screenVertices[i];
        v.x = offset.x + center[0];
        v.y = offset.y + center[1];
        v.z = 0.0f;
        v.specular = 0;
        v.rhw = 1.0f;
        v.color = c;
        offset = offset.RotateByMatrix(step);
    }
    // (The last point is the first again.)
    s_screenVertices[segments] = s_screenVertices[0];
    g_pDevice->SetTexture(0, nullptr);
    SetVertexFormat(6);
    return g_pDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, segments, s_screenVertices.Data(), sizeof(S_screenVertex));
}

// 0x100175c0
I3D_RESULT I3D_driver::DrawText3D(const S_vector &pos, const char *text, uint32_t flags, float scale)
{
    DisableFog();
    // (Without a render camera the original returns D3DRS_CULLMODE's result, or 1 when the cache
    // already held it.)
    I3D_RESULT result = 1;
    if (ig::g_rs.cull != 1) {
        ig::g_rs.cull = 1;
        result = g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (!SetFacingMatrix(pos))
        return result;
    I3D_material *m = g_resourceMaterial;
    // (The original reads the material without testing it; the font failing to load crashed here.)
    if (!m)
        return result;
    const float alpha = FlagsAlpha(flags);
    const uint32_t c = PackColor(alpha, m->diffuse.x, m->diffuse.y, m->diffuse.z);
    SetDrawStates(alpha);
    SetBlendState(0);
    g_pDevice->SetTexture(0, D3DTexture(m));
    SetVertexFormat(2);
    const float size = scale * 16.0f;
    if (m->matFlags & MATF_ANIMATED)
        m->UpdateTexture();
    const float du = 16.0f / static_cast<float>(m->texture->width);
    if (m->matFlags & MATF_ANIMATED)
        m->UpdateTexture();
    const float dv = 16.0f / static_cast<float>(m->texture->height);
    const int length = static_cast<int>(std::strlen(text));
    s_vertices.ResizeDiscard(length * 6);
    S_colorVertex *v = s_vertices.Data();
    for (int i = 0; i < length; ++i, v += 6) {
        // The font is a 16 x 16 grid of the 256 character codes.
        const uint8_t ch = static_cast<uint8_t>(text[i]);
        const float u0 = static_cast<float>(ch & 15) * du;
        const float v0 = static_cast<float>(ch >> 4) * dv;
        const float fi = static_cast<float>(i);
        const float x0 = fi * size;
        const float x1 = (fi + 1.0f) * size;
        const float u1 = u0 + du;
        const float v1 = v0 + dv;
        v[0] = {S_vector(x0, 0.0f, 0.0f), c, u0, v1};
        v[1] = {S_vector(x1, size, 0.0f), c, u1, v0};
        v[2] = {S_vector(x0, size, 0.0f), c, u0, v0};
        v[3] = {S_vector(x1, 0.0f, 0.0f), c, u1, v1};
        v[4] = v[1];
        v[5] = v[0];
    }
    g_pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, length * 2, s_vertices.Data(), sizeof(S_colorVertex));
    if (g_driverStates[9])
        return g_pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    return I3D_OK;
}

// 0x10017ae0
I3D_RESULT I3D_driver::DrawText2D(int x, int y, const char *text, uint32_t flags, float scale)
{
    DisableFog();
    SetCullState(1);
    I3D_material *m = g_resourceMaterial;
    if (!m)
        return I3D_OK;
    const float alpha = FlagsAlpha(flags);
    const uint32_t c = PackColor(alpha, m->diffuse.x, m->diffuse.y, m->diffuse.z);
    SetDrawStates(alpha);
    SetBlendState(0);
    g_pDevice->SetTexture(0, D3DTexture(m));
    SetVertexFormat(6);
    const float size = scale * 16.0f;
    if (m->matFlags & MATF_ANIMATED)
        m->UpdateTexture();
    const float du = 16.0f / static_cast<float>(m->texture->width);
    if (m->matFlags & MATF_ANIMATED)
        m->UpdateTexture();
    const float dv = 16.0f / static_cast<float>(m->texture->height);
    const int length = static_cast<int>(std::strlen(text));
    s_screenVertices.ResizeDiscard(length * 6);
    const float left = static_cast<float>(x);
    const float top = static_cast<float>(y);
    const float bottom = size + top;
    S_screenVertex *v = s_screenVertices.Data();
    for (int i = 0; i < length; ++i, v += 6) {
        const uint8_t ch = static_cast<uint8_t>(text[i]);
        const float u0 = static_cast<float>(ch & 15) * du;
        const float v0 = static_cast<float>(ch >> 4) * dv;
        const float fi = static_cast<float>(i);
        const float x0 = fi * size + left;
        const float x1 = (fi + 1.0f) * size + left;
        const float u1 = u0 + du;
        const float v1 = v0 + dv;
        v[0] = {x0, top, 1.0f, 1.0f, c, 0, u0, v0};
        v[1] = {x1, top, 1.0f, 1.0f, c, 0, u1, v0};
        v[2] = {x0, bottom, 1.0f, 1.0f, c, 0, u0, v1};
        v[3] = {x1, bottom, 1.0f, 1.0f, c, 0, u1, v1};
        v[4] = v[2];
        v[5] = v[1];
    }
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, length * 2, s_screenVertices.Data(), sizeof(S_screenVertex));
    if (!g_driverStates[0]) {
        g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    } else if (ig::g_anisotropy) {
        // (Port enhancement, LS3DF.ini AnisotropicFiltering: back from the text's linear.)
        g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_ANISOTROPIC);
    }
    if (g_driverStates[9])
        g_pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    return g_pDevice->SetRenderState(D3DRS_ZENABLE, ig::g_zBufferType);
}

// 0x10018070
I3D_RESULT I3D_driver::DrawSprite(const S_vector &pos, I3D_material *material, uint32_t flags, float size)
{
    DisableFog();
    if (!SetFacingMatrix(pos))
        return I3D_OK;
    const float alpha = FlagsAlpha(flags);
    const S_vector4 &d = material->diffuse;
    const uint32_t c = PackColor(alpha, d.x, d.y, d.z);
    SetColorOp(0, D3DTOP_MODULATE);
    SetAlphaOp(0, D3DTOP_MODULATE);
    const float lo = size * -0.5f;
    const float hi = size * 0.5f;
    // (The texture coordinates stay 0.001 inside the edges.)
    S_colorVertex v[4] = {
        {S_vector(lo, lo, 0.0f), c, 0.001f, 0.999f},
        {S_vector(lo, hi, 0.0f), c, 0.001f, 0.001f},
        {S_vector(hi, lo, 0.0f), c, 0.999f, 0.999f},
        {S_vector(hi, hi, 0.0f), c, 0.999f, 0.001f},
    };
    const uint32_t mf = material->matFlags;
    if (mf & (MATF_ADDITIVE | MATF_ADDITIVE_LOW)) {
        SetAlphaState(1);
        SetBlendState(1);
    } else if (mf & MATF_ALPHA) {
        SetAlphaState(1);
        SetBlendState(0);
    } else if (mf & MATF_COLORKEY) {
        SetAlphaState(3);
        SetBlendState(0);
    } else {
        SetAlphaState(0);
    }
    g_pDevice->SetTexture(0, D3DTexture(material));
    SetVertexFormat(2);
    return g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}

// 0x10018400
I3D_RESULT I3D_driver::DrawBox(const I3D_bbox &box, const S_vector *edgeColor, const S_vector *faceColor,
                               uint32_t flags)
{
    // Empty boxes (min above max in x, or NaN) are not drawn.
    if (!(box.min.x < box.max.x) && !(box.min.x == box.max.x))
        return I3D_OK;
    g_pDevice->SetTexture(0, nullptr);
    const S_vector4 *corners = BoxCorners(box);
    // (The faces get all 16 bits of `flags`, so the edges' transparency byte reads as DRAWF_*.)
    if (faceColor)
        DrawTriangles(corners, 8, kBoxFaces, 36, *faceColor, sizeof(S_vector4), flags & 0xffff, nullptr);
    if (edgeColor)
        DrawLines(corners, 8, kBoxLines, 24, *edgeColor, sizeof(S_vector4), (flags & 0xffff) >> 8);
    return I3D_OK;
}

// 0x10018a20
I3D_RESULT I3D_driver::DrawSphere(const S_matrix &matrix, const I3D_bsphere &sphere, const S_vector &color,
                                  uint32_t flags)
{
    // (Spheres of radius 0 or below are not drawn; the original then returns the FPU status.)
    if (sphere.radius < 0.0f || sphere.radius == 0.0f)
        return I3D_OK;
    s_sphereMatrix.Identity();
    s_sphereMatrix(3) = sphere.center;
    s_sphereMatrix.m[2][2] = sphere.radius;
    s_sphereMatrix.m[1][1] = sphere.radius;
    s_sphereMatrix.m[0][0] = sphere.radius;
    s_sphereMatrix = s_sphereMatrix * matrix;
    g_pIGraph->SetWorldMatrix(&s_sphereMatrix);
    return DrawLines(kSphereVertices, 48, kSphereIndices, 96, color, sizeof(S_vector4), (flags & 0xffff) >> 8);
}
