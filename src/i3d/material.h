// I3D_material: the surface of a mesh face group (vtable 0x1009cc70, 0x60 bytes; 0x1002b030 -
// 0x1002b470) and I3D_anim_material, its variant whose textures cycle through frame sequences
// (vtable 0x1009ccc0, 0xA8 bytes; 0x1002b470 - 0x1002c400). I3D_driver slots 0x58 / 0x5C make
// them; the model loader fills them (Load).
//
// The colors' block (+0x14 - +0x5B) is read from the model files as it is; its order looks like
// Direct3D's D3DMATERIAL8 (diffuse, ambient, specular, emissive, power), which the names follow
// (unverified: the game's SDK header guesses other names for the setters).
#pragma once

#include "core/c_vector.h"
#include "i3d/i3d.h"

class ITexture;

// Material flags (I3D_material::matFlags).
enum : uint32_t {
    MATF_BITS0_MASK = 0x0000007F,           // SetBits0 (7 bits)
    MATF_BITS8_MASK = 0x00000700,           // SetBits8 (3 bits)
    MATF_BITS12_MASK = 0x00007000,          // SetBits12 (3 bits)
    MATF_ADDITIVE_LOW = 0x00000080,         // (file flag) drawn additively, as MATF_ADDITIVE
    MATF_CLAMP_U = 0x00010000,              // texture address clamp in u
    MATF_CLAMP_V = 0x00020000,              // ... in v
    MATF_TEXTURE = 0x00040000,              // has a texture
    MATF_ENV_TEXTURE = 0x00080000,          // has the second texture (the files' environment map)
    MATF_ANIMATED = 0x04000000,             // I3D_anim_material: the textures follow the time
    MATF_COLORS = 0x08000000,               // drawn with its own colors (else IGraph's default material)
    MATF_TWO_SIDED = 0x10000000,            // SetTwoSided: drawn without culling
    MATF_COLORKEY = 0x20000000,             // the texture's 1-bit alpha (ITC_ALPHA_1BIT)
    MATF_ALPHA = 0x40000000,                // a texture has alpha
    MATF_ADDITIVE = 0x80000000,             // (file flag) drawn additively (one + source alpha)
};

class I3D_material {
public:
    I3D_material();                         // 0x1002b030
    I3D_material(const I3D_material &) = delete;
    I3D_material &operator=(const I3D_material &) = delete;

    // 0x00 0x1002b120: the last reference deletes the material (slot 0x40).
    virtual uint32_t __stdcall Release();
    virtual I3D_RESULT __stdcall SetAmbient(const S_vector &c);        // 0x04 0x1002b140 (+0x24)
    virtual I3D_RESULT __stdcall SetDiffuse(const S_vector &c);        // 0x08 0x1002b160 (+0x14)
    virtual I3D_RESULT __stdcall SetEmissive(const S_vector &c);       // 0x0C 0x1002b180 (+0x44)
    virtual I3D_RESULT __stdcall SetSpecular(const S_vector &c);       // 0x10 0x1002b1a0 (+0x34)
    virtual I3D_RESULT __stdcall SetPower(float value);                // 0x14 0x1002b1c0 (+0x54)
    virtual I3D_RESULT __stdcall SetOpacity(float value);              // 0x18 0x1002b1d0 (+0x58)
    // 0x1C 0x1002b1e0: the texture (a reference); the alpha flags follow its format.
    virtual I3D_RESULT __stdcall SetTexture(ITexture *newTexture);
    // 0x20 0x1002b350: the colors, flags and textures of another material.
    virtual I3D_RESULT __stdcall CopyFrom(const I3D_material *source);
    virtual I3D_RESULT __stdcall SetTwoSided();                    // 0x24 0x1002b3b0
    // 0x28 0x1002b300: SetEnvTexture, SetBits8, SetBits12 and SetAlpha in turn (the first
    // failure's result; else 0).
    virtual I3D_RESULT __stdcall SetEnvironment(ITexture *env, uint32_t bits8, uint32_t bits12, float alpha);
    // 0x2C 0x1002b280: the second texture (a reference).
    virtual I3D_RESULT __stdcall SetEnvTexture(ITexture *newTexture);
    virtual I3D_RESULT __stdcall SetBits8(uint32_t value);             // 0x30 0x1002b3c0 (3 bits)
    virtual I3D_RESULT __stdcall SetBits12(uint32_t value);            // 0x34 0x1002b3e0 (3 bits)
    virtual I3D_RESULT __stdcall SetBits0(uint32_t value);             // 0x38 0x1002b400 (7 bits)
    // 0x3C 0x1002b420: 0 for a negative value; 1 when the current alpha is above 1 (the original
    // compares the old value), else the value.
    virtual I3D_RESULT __stdcall SetAlpha(float value);
    // 0x40 0x1002b0b0 (__thiscall): the scalar deleting destructor.
    virtual ~I3D_material();
    // 0x44 / 0x48 0x1002b0a0 (nothing for a plain material): the textures of the current frame
    // (I3D_anim_material).
    virtual void __stdcall UpdateTexture() {}
    virtual void __stdcall UpdateEnvTexture() {}

    // 0x1002bbe0: the colors, the second texture and the texture from the model file (the
    // texture flags from the material's flags and the driver's texture states).
    void Load();

    // Materials live in 16-byte aligned blocks.
    static void *operator new(size_t size) noexcept;
    static void operator delete(void *p);

    int refCount;                           // +0x04
    uint32_t matFlags;                      // +0x08 MATF_*
    ITexture *texture;                      // +0x0C
    ITexture *envTexture;                   // +0x10
    S_vector4 diffuse;                      // +0x14 (1, 1, 1)
    S_vector4 ambient;                      // +0x24
    S_vector4 specular;                     // +0x34 (1, 1, 1)
    S_vector4 emissive;                     // +0x44
    float power;                            // +0x54
    float opacity;                          // +0x58 (1.0) below 1 the faces are blended
    float alpha;                            // +0x5C (1.0) read with the second texture

protected:
    // The texture flags (ITC_*) the material's flags and the driver states ask for.
    uint32_t TextureFlags();
};
static_assert(sizeof(I3D_material) == 0x60, "I3D_material layout");
static_assert(offsetof(I3D_material, diffuse) == 0x14, "I3D_material layout");
static_assert(offsetof(I3D_material, alpha) == 0x5C, "I3D_material layout");

class I3D_anim_material final : public I3D_material {
public:
    I3D_anim_material();                    // 0x1002b470
    ~I3D_anim_material() override;          // 0x1002b530 (0x40 0x1002b510)

    // 0x1C / 0x2C 0x1002bba0 / 0x1002bbc0: only without frames (else I3DERR_UNSUPPORTED).
    I3D_RESULT __stdcall SetTexture(ITexture *newTexture) override;
    I3D_RESULT __stdcall SetEnvTexture(ITexture *newTexture) override;
    // 0x20 0x1002b840: also the frames and their timing.
    I3D_RESULT __stdcall CopyFrom(const I3D_material *source) override;
    // 0x44 / 0x48 0x1002b640 / 0x1002b720: the frame the driver's time reached.
    void __stdcall UpdateTexture() override;
    void __stdcall UpdateEnvTexture() override;
    // 0x4C / 0x50 0x1002bad0 / 0x1002bb30: the frame (negative: restart at the first, timed from
    // now; otherwise the given one, wrapped, timed from the start).
    virtual bool __stdcall SetFrame(int frame);
    virtual bool __stdcall SetEnvFrame(int frame);

    // 0x1002be30: as I3D_material::Load, with the frame sequences ("name00.ext" ...).
    void Load();

    // A sequence of textures switched every `interval` ms (loop from `loopStart`; negative: a
    // random frame each switch).
    struct Sequence {
        ls3d::C_vector<ITexture *> frames;  // +0x00 (grow 8)
        int lastTime;                       // +0x10 the driver time of the last update (0: none)
        int elapsed;                        // +0x14 ms into the current frame
        int frame;                          // +0x18
    };
    Sequence textures;                      // +0x60
    Sequence envTextures;                   // +0x7C
    int loopStart;                          // +0x98
    int interval;                           // +0x9C (1)
    int envLoopStart;                       // +0xA0
    int envInterval;                        // +0xA4 (1)

private:
    // The original's two copies, one per sequence.
    void UpdateSequence(Sequence &seq, ITexture *&current, int start, int period, bool alwaysStamp);
};
static_assert(sizeof(I3D_anim_material) == 0xA8, "I3D_anim_material layout");
static_assert(offsetof(I3D_anim_material, envTextures) == 0x7C, "I3D_anim_material layout");
static_assert(offsetof(I3D_anim_material, loopStart) == 0x98, "I3D_anim_material layout");

namespace ls3d::i3d {

// The driver's time in ms (0x100be394; I3D_driver::Tick advances it).
extern int g_driverTime;

}  // namespace ls3d::i3d
