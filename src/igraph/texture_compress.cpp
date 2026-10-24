// ITexture's DXT compression: a 16-bit staging surface through the S3TC encoder (0x100788b0).
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"
#include "igraph/s3tc.h"

namespace ls3d::tex {

// `kind` 1: X1R5G5B5 to DXT1, 2: A1R5G5B5 to DXT1 with transparency, 3: A4R4G4B4 to DXT3.
bool CompressSurface(const char *name, int kind, IDirect3DSurface8 *source, IDirect3DSurface8 *dest)
{
    // Error weights of red, green, blue (luminance).
    static const float kWeights[3] = {0.3086f, 0.6094f, 0.082f};
    D3DSURFACE_DESC desc;
    if (FAILED(source->GetDesc(&desc)))
        return false;
    D3DLOCKED_RECT srcLock, dstLock;
    if (FAILED(source->LockRect(&srcLock, nullptr, D3DLOCK_READONLY)))
        return false;
    if (FAILED(dest->LockRect(&dstLock, nullptr, 0))) {
        source->UnlockRect();           // (left locked by the original)
        return false;
    }

    s3tc::S_surfaceDesc src = {};
    src.size = sizeof src;
    src.flags = 0x180e;
    src.height = desc.Height;
    src.width = desc.Width;
    src.pitch = srcLock.Pitch;
    src.surface = srcLock.pBits;
    src.pfSize = 0x20;
    uint32_t type = s3tc::S3TC_OPAQUE;
    switch (kind) {
    case 2:
        type = s3tc::S3TC_ALPHA_KEY;
        [[fallthrough]];
    case 1:
        src.mask[0] = 0x7c00;
        src.mask[1] = 0x3e0;
        src.mask[2] = 0x1f;
        src.mask[3] = 0x8000;
        src.pfFlags = 0x41;             // DDPF_RGB | DDPF_ALPHAPIXELS
        src.bitCount = 16;
        break;
    case 3:
        type = s3tc::S3TC_ALPHA_EXPLICIT;
        src.mask[0] = 0xf00;
        src.mask[1] = 0xf0;
        src.mask[2] = 0xf;
        src.mask[3] = 0xf000;
        src.pfFlags = 0x41;
        src.bitCount = 16;
        break;
    default:
        break;
    }
    s3tc::S_surfaceDesc dst = {};
    dst.size = sizeof dst;

    const bool verbose = (ig::g_init.flags & IG_VERBOSE) != 0;
    if (verbose)
        g_pIGraph->SetAppNameF("Compressing texture %s [%ix%i]", name, desc.Width, desc.Height);
    s3tc::Encode(&src, nullptr, &dst, dstLock.pBits, type, kWeights);
    if (verbose)
        g_pIGraph->SetAppNameF("");
    source->UnlockRect();
    dest->UnlockRect();
    return true;
}

}  // namespace ls3d::tex
