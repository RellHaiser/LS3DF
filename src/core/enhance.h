// Port enhancements: graphics options the original engine does not have, read from LS3DF.ini next
// to the DLL. Each one is off when the file or its key is missing; the engine then works as the
// original (the compare_* tests run that way).
//
//   [Graphics]
//   AnisotropicFiltering=16    ; 2 - 16: anisotropic texture filtering (capped by the device); 0: off
//   TrilinearFiltering=1       ; mip levels always blended (driver state 2 raised to linear)
//   ImprovedMipmaps=1          ; gamma-correct, alpha-weighted mip maps; colour-keyed textures keep
//                              ; their alpha-tested coverage at every level
//   TrueColorTextures=1        ; textures from image files in A8R8G8B8 (not 16-bit or DXT)
//   TrueColorLightmaps=1       ; lightmap atlases in 32-bit whatever driver state 0x22 says
#pragma once

namespace ls3d {

struct S_enhancements {
    int anisotropy;                         // AnisotropicFiltering (0: off, else 2 - 16)
    bool trilinear;                         // TrilinearFiltering
    bool mipmaps;                           // ImprovedMipmaps
    bool trueColorTextures;                 // TrueColorTextures
    bool trueColorLightmaps;                // TrueColorLightmaps
};

// The options (LS3DF.ini is read on the first call).
const S_enhancements &Enhancements();
// Replaces the options (tests, or an application that embeds the engine).
void SetEnhancements(const S_enhancements &options);
// A line in the engine log, written even after the driver turned the log off: the options'
// messages (what they changed) reach the log whether or not the game runs verbose.
void EnhancementLog(const char *format, ...);

}  // namespace ls3d
