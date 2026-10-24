// I3D_lens: the lens flare / glow visual (LENS; vtable 0x1009c2c0, 0x250 bytes; 0x10007dd0 -
// 0x10009810).
//
// A lens seen in a frame's pass joins the scene's lensFlares; Render draws the list after
// everything else with DrawFlare and drops those faded out. One element is a glow: a sprite at the
// frame, sized by the distance; more are a flare: the frame's position is a direction (a sun),
// the elements are sprites strung along the line from it through the screen's center. A ray from
// the camera (through the transmit filter, also through the near sector) decides how much of the
// lens shows; brightness and color follow at their fade speeds.
#pragma once

#include "core/c_vector.h"
#include "i3d/visual.h"

class I3D_material;

enum : uint32_t {
    LENSF_SEEN = 0x00000001,                // (renderer) seen in this frame's passes
};

// A sprite of the lens (0x0C bytes; from the model file).
struct S_lensElement {
    float position;                         // +0x00 flares: along the axis from the screen's center
    float size;                             // +0x04 times the texture's size
    I3D_material *material;                 // +0x08 (a reference)
};

class I3D_lens : public I3D_visual {
public:
    // 0x4C 0x10007ef0: the frame; of another lens its parameters and elements.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x10007dd0
    void Uninit() override;                                                    // 0x64 0x10007ea0
    // 0x70 0x10009610: the frame, then the elements (with their materials).
    void Load() override;
    // 0x9C 0x100080d0: draws the lens (pre-transformed sprites, additive, no z); `seen`: it was seen
    // this frame (the visibility ray runs every testInterval ms), else it fades out.
    virtual void __stdcall DrawFlare(bool seen);

    float intensity;                        // +0x1E0 (1) the sprites' alpha
    float sizeScale;                        // +0x1E4 (1)
    uint32_t lensFlags;                     // +0x1E8 LENSF_*
    uint32_t testInterval;                  // +0x1EC (150) ms between visibility rays
    uint32_t testTime;                      // +0x1F0 timeGetTime of the next ray (0 when it joins the list)
    uint32_t updateTime;                    // +0x1F4 timeGetTime of the last fade step
    float brightness;                       // +0x1F8 fades toward target (the list drops it near 0)
    float target;                           // +0x1FC 1 visible, 0 hidden
    S_vector4 color;                        // +0x200 (1, 1, 1) fades toward transmit
    S_vector4 transmit;                     // +0x210 (1, 1, 1) what the ray let through
    float fadeIn;                           // +0x220 (15) per second
    float fadeOut;                          // +0x224 (8)
    ls3d::C_vector<S_lensElement> elements{10}; // +0x228 (grow 10: the driver's factory)
    float nearRange;                        // +0x238 glows: closer, they grow by nearScale
    float farStart;                         // +0x23C glows: farther, they shrink to 0 at range
    float range;                            // +0x240 (300) glows: not drawn beyond
    float nearScale;                        // +0x244 (1)
    uint32_t padding248[2];
};
static_assert(sizeof(I3D_lens) == 0x250, "I3D_lens layout");
static_assert(offsetof(I3D_lens, lensFlags) == 0x1E8, "I3D_lens layout");
static_assert(offsetof(I3D_lens, brightness) == 0x1F8, "I3D_lens layout");
static_assert(offsetof(I3D_lens, elements) == 0x228, "I3D_lens layout");
static_assert(offsetof(I3D_lens, nearScale) == 0x244, "I3D_lens layout");
