// I3D_material, I3D_anim_material (0x1002b030 - 0x1002c400).
#include "i3d/material.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "external/rw_data.h"
#include "igraph/igraph.h"
#include "igraph/itexture.h"
#include "i3d/driver.h"
#include "i3d/loader.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

// A name from the model file: a length byte, then the characters.
void ReadName(char (&buffer)[256], int handle)
{
    int length = 0;
    dtaRead(handle, &length, 1);
    if (length)
        dtaRead(handle, buffer, length);
    buffer[length] = 0;
}

}  // namespace

// ---------------------------------------------------------------- I3D_material

void *I3D_material::operator new(size_t size) noexcept { return AlignedAlloc(size, 16); }
void I3D_material::operator delete(void *p) { AlignedFree(p); }

I3D_material::I3D_material()
    : refCount(1), matFlags(1), texture(nullptr), envTexture(nullptr), diffuse{1.0f, 1.0f, 1.0f, 0.0f}, ambient{},
      specular{1.0f, 1.0f, 1.0f, 0.0f}, emissive{}, power(0.0f), opacity(1.0f), alpha(1.0f)
{
}

I3D_material::~I3D_material()
{
    if (texture)
        texture->Release();
    if (envTexture)
        envTexture->Release();
}

uint32_t I3D_material::Release()
{
    if (--refCount < 0)
        return 0;
    if (refCount)
        return refCount;
    delete this;
    return 0;
}

I3D_RESULT I3D_material::SetAmbient(const S_vector &c)
{
    ambient.x = c.x;
    ambient.y = c.y;
    ambient.z = c.z;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetDiffuse(const S_vector &c)
{
    diffuse.x = c.x;
    diffuse.y = c.y;
    diffuse.z = c.z;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetEmissive(const S_vector &c)
{
    emissive.x = c.x;
    emissive.y = c.y;
    emissive.z = c.z;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetSpecular(const S_vector &c)
{
    specular.x = c.x;
    specular.y = c.y;
    specular.z = c.z;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetPower(float value)
{
    power = value;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetOpacity(float value)
{
    opacity = value;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetTexture(ITexture *newTexture)
{
    if (newTexture == texture)
        return I3D_OK;
    if (texture)
        texture->Release();
    // The alpha flags as the second texture alone leaves them.
    const uint32_t old = matFlags;
    matFlags = old & 0x7ffbffff;
    if (!envTexture) {
        matFlags = old & 0x1ffbffff;
    } else {
        if (!(envTexture->flags & ITC_SOURCE_ALPHA))
            matFlags = old & 0x3ffbffff;
        if (!(envTexture->flags & ITC_ALPHA_1BIT))
            matFlags &= ~MATF_COLORKEY;
    }
    texture = newTexture;
    if (newTexture) {
        ++newTexture->refCount;
        if (newTexture->flags & (ITC_SOURCE_ALPHA | ITC_USE_ALPHA))
            matFlags |= MATF_ALPHA;
        if (newTexture->flags & ITC_ALPHA_1BIT)
            matFlags |= newTexture->flags & ITC_MIPMAP ? (MATF_COLORKEY | MATF_ALPHA) : MATF_COLORKEY;
        matFlags |= MATF_TEXTURE;
    }
    return I3D_OK;
}

I3D_RESULT I3D_material::SetEnvTexture(ITexture *newTexture)
{
    if (newTexture == envTexture)
        return I3D_OK;
    if (envTexture)
        envTexture->Release();
    envTexture = newTexture;
    if (!texture) {
        matFlags &= ~(MATF_ALPHA | MATF_COLORKEY);
    } else {
        if (!(texture->flags & (ITC_SOURCE_ALPHA | ITC_USE_ALPHA)))
            matFlags &= ~MATF_ALPHA;
        if (!(texture->flags & ITC_ALPHA_1BIT))
            matFlags &= ~MATF_COLORKEY;
    }
    if (!newTexture) {
        matFlags &= ~MATF_ENV_TEXTURE;
        return I3D_OK;
    }
    ++newTexture->refCount;
    if (newTexture->flags & ITC_SOURCE_ALPHA)
        matFlags |= MATF_ALPHA;
    matFlags |= MATF_ENV_TEXTURE;
    return I3D_OK;
}

I3D_RESULT I3D_material::CopyFrom(const I3D_material *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    matFlags = source->matFlags & ~MATF_ANIMATED;
    std::memcpy(&diffuse, &source->diffuse, 0x44);
    opacity = source->opacity;
    alpha = source->alpha;
    SetTexture(source->texture);
    SetEnvTexture(source->envTexture);
    return I3D_OK;
}

I3D_RESULT I3D_material::SetTwoSided()
{
    matFlags |= MATF_TWO_SIDED;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetEnvironment(ITexture *env, uint32_t bits8, uint32_t bits12, float value)
{
    I3D_RESULT result = SetEnvTexture(env);
    if (result < 0)
        return result;
    if ((result = SetBits8(bits8)) < 0)
        return result;
    if ((result = SetBits12(bits12)) < 0)
        return result;
    result = SetAlpha(value);
    return result < 0 ? result : I3D_OK;
}

I3D_RESULT I3D_material::SetBits8(uint32_t value)
{
    matFlags = (matFlags & ~MATF_BITS8_MASK) | value << 8;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetBits12(uint32_t value)
{
    matFlags = (matFlags & ~MATF_BITS12_MASK) | value << 12;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetBits0(uint32_t value)
{
    matFlags = (matFlags & ~MATF_BITS0_MASK) | value;
    return I3D_OK;
}

I3D_RESULT I3D_material::SetAlpha(float value)
{
    if (value < 0.0f)
        alpha = 0.0f;
    else if (alpha > 1.0f)
        alpha = 1.0f;
    else
        alpha = value;
    return I3D_OK;
}

uint32_t I3D_material::TextureFlags()
{
    const uint32_t flags = matFlags;
    uint32_t tf = flags >> 5 & ITC_STREAMED;
    if (g_driverStates[DRVSTATE_TEX_DITHER])
        tf |= ITC_DITHER;
    if (g_driverStates[DRVSTATE_TEX_TRUECOLOR] || (flags & 0x100000))
        tf |= ITC_TRUECOLOR;
    else if (g_driverStates[DRVSTATE_TEX_COMPRESS] && !(flags & 0x200000))
        tf |= ITC_COMPRESS;
    if ((flags & 0x800000) && g_driverStates[DRVSTATE_TEX_MIPMAP])
        tf |= ITC_MIPMAP;
    if (g_driverStates[DRVSTATE_TEX_HALF] && !(flags & 0x400000))
        tf |= ITC_HALF;
    if (flags & 0x8000)
        tf |= ITC_KEEP_PIXELS;
    if (flags & MATF_COLORKEY)
        tf |= flags & 0x1000000 ? (ITC_ALPHA_1BIT | ITC_SOURCE_ALPHA) : ITC_ALPHA_1BIT;
    else if (flags & MATF_ALPHA)
        tf |= flags & 0x1000000 ? ITC_SOURCE_ALPHA : ITC_USE_ALPHA;
    // (A 1-bit alpha with mip maps blends.)
    if ((tf & (ITC_MIPMAP | ITC_ALPHA_1BIT)) == (ITC_MIPMAP | ITC_ALPHA_1BIT))
        matFlags |= MATF_ALPHA;
    return tf;
}

void I3D_material::Load()
{
    uint32_t tf = TextureFlags();
    char name[256], alphaName[256];
    if (!(matFlags & MATF_ENV_TEXTURE)) {
        dtaRead(g_loadFile, &diffuse, 0x48);
    } else {
        dtaRead(g_loadFile, &diffuse, 0x4C);
        ReadName(name, g_loadFile);
        if (*name) {
            const uint32_t envFlags = (g_driverStates[DRVSTATE_TEX_MIPMAP] ? ITC_MIPMAP : 0) | (tf & 0xffffef31) | 0x80000;
            g_pIGraph->LoadTexture(name, nullptr, envFlags, &envTexture);
        }
    }
    int length = 0;
    dtaRead(g_loadFile, &length, 1);
    if (!length)
        return;
    dtaRead(g_loadFile, name, length);
    name[length] = 0;
    alphaName[0] = 0;
    if ((matFlags & MATF_ALPHA) && !(matFlags & 0xa1000000)) {
        ReadName(alphaName, g_loadFile);
        if (!*alphaName)
            tf &= ~ITC_USE_ALPHA;
    }
    g_pIGraph->LoadTexture(name, alphaName, tf, &texture);
}

// ---------------------------------------------------------------- I3D_anim_material

I3D_anim_material::I3D_anim_material() : loopStart(0), interval(1), envLoopStart(0), envInterval(1)
{
    textures.frames.SetGrowBy(8);
    textures.lastTime = g_driverTime;
    textures.elapsed = 0;
    textures.frame = 0;
    envTextures.frames.SetGrowBy(8);
    envTextures.lastTime = g_driverTime;
    envTextures.elapsed = 0;
    envTextures.frame = 0;
    matFlags |= MATF_ANIMATED;
}

I3D_anim_material::~I3D_anim_material()
{
    // The frames own their textures; the current ones are theirs.
    if (textures.frames.Size()) {
        for (int i = textures.frames.Size(); i > 0; --i) {
            if (ITexture *t = textures.frames[i - 1])
                t->Release();
        }
        textures.frames.Free();
        texture = nullptr;
    }
    if (envTextures.frames.Size()) {
        for (int i = envTextures.frames.Size(); i > 0; --i) {
            if (ITexture *t = envTextures.frames[i - 1])
                t->Release();
        }
        envTextures.frames.Free();
        envTexture = nullptr;
    }
}

I3D_RESULT I3D_anim_material::SetTexture(ITexture *newTexture)
{
    if (textures.frames.Size())
        return I3DERR_UNSUPPORTED;
    return I3D_material::SetTexture(newTexture);
}

I3D_RESULT I3D_anim_material::SetEnvTexture(ITexture *newTexture)
{
    if (envTextures.frames.Size())
        return I3DERR_UNSUPPORTED;
    return I3D_material::SetEnvTexture(newTexture);
}

I3D_RESULT I3D_anim_material::CopyFrom(const I3D_material *source)
{
    for (int i = textures.frames.Size(); i > 0; --i) {
        if (ITexture *t = textures.frames[i - 1])
            t->Release();
    }
    textures.frames.Clear();
    for (int i = envTextures.frames.Size(); i > 0; --i) {
        if (ITexture *t = envTextures.frames[i - 1])
            t->Release();
    }
    envTextures.frames.Clear();
    // (The current textures are dropped without a release: a reference of a material without
    // frames leaks, as in the original.)
    textures.elapsed = 0;
    textures.lastTime = g_driverTime;
    textures.frame = 0;
    texture = nullptr;
    envTextures.elapsed = 0;
    envTextures.lastTime = g_driverTime;
    envTextures.frame = 0;
    envTexture = nullptr;
    // (The original reads a null source's flags below. Refused here.)
    if (!source)
        return I3D_OK;
    matFlags = source->matFlags & ~MATF_ANIMATED;
    std::memcpy(&diffuse, &source->diffuse, 0x44);
    opacity = source->opacity;
    alpha = source->alpha;
    SetTexture(source->texture);
    SetEnvTexture(source->envTexture);
    if (!(source->matFlags & MATF_ANIMATED))
        return I3D_OK;
    const auto *src = static_cast<const I3D_anim_material *>(source);
    matFlags |= MATF_ANIMATED;
    interval = src->interval;
    loopStart = src->loopStart;
    envInterval = src->envInterval;
    envLoopStart = src->envLoopStart;
    if (src->textures.frames.Size() > 0) {
        if (texture)
            texture->Release();       // (the original releases a null one through null)
        for (ITexture *t : src->textures.frames) {
            textures.frames.PushBack(t);
            if (t)
                ++t->refCount;
        }
        texture = textures.frames[0];
    }
    if (src->envTextures.frames.Size() > 0) {
        if (envTexture)
            envTexture->Release();       // (the original releases a null one through null)
        for (ITexture *t : src->envTextures.frames) {
            envTextures.frames.PushBack(t);
            if (t)
                ++t->refCount;
        }
        envTexture = envTextures.frames[0];
    }
    return I3D_OK;
}

void I3D_anim_material::UpdateSequence(Sequence &seq, ITexture *&current, int start, int period, bool alwaysStamp)
{
    const int n = seq.frames.Size();
    if (!n)
        return;
    if (n > 1 && period) {
        if (!seq.lastTime) {
            current = seq.frames[seq.frame];
            seq.lastTime = g_driverTime;
            seq.elapsed = 0;
            return;
        }
        const int dt = g_driverTime - seq.lastTime;
        if (dt) {
            const uint32_t steps = static_cast<uint32_t>(seq.elapsed + dt) / static_cast<uint32_t>(period);
            if (!steps) {
                seq.elapsed += dt;
            } else {
                seq.elapsed += dt - period * static_cast<int>(steps);
                if (start < 0) {
                    seq.frame = std::rand() % n;
                } else {
                    seq.frame += static_cast<int>(steps);
                    if (n <= seq.frame)
                        seq.frame = start + (seq.frame - start) % (n - start);
                }
            }
            seq.lastTime = g_driverTime;
        }
        current = seq.frames[seq.frame];
        return;
    }
    current = seq.frames[0];
    if (alwaysStamp)
        seq.lastTime = g_driverTime;
}

void I3D_anim_material::UpdateTexture() { UpdateSequence(textures, texture, loopStart, interval, false); }

void I3D_anim_material::UpdateEnvTexture() { UpdateSequence(envTextures, envTexture, envLoopStart, envInterval, true); }

bool I3D_anim_material::SetFrame(int frame)
{
    const int n = textures.frames.Size();
    if (!n)
        return false;
    if (frame < 0) {
        textures.lastTime = g_driverTime;
        textures.frame = 0;
    } else {
        if (n <= frame)
            frame %= n;
        textures.frame = frame;
        textures.lastTime = 0;
    }
    texture = textures.frames[textures.frame];
    return true;
}

bool I3D_anim_material::SetEnvFrame(int frame)
{
    const int n = envTextures.frames.Size();
    if (!n)
        return false;
    if (frame < 0) {
        envTextures.lastTime = g_driverTime;
        envTextures.frame = 0;
    } else {
        if (n <= frame)
            frame %= n;
        envTextures.frame = frame;
        envTextures.lastTime = 0;
    }
    // (The original takes the frame of the other sequence here; past the end of this one it read
    // beyond the array: then this sequence's own frame.)
    const int k = textures.frame < n ? textures.frame : envTextures.frame;
    envTexture = envTextures.frames[k];
    return true;
}

void I3D_anim_material::Load()
{
    char envName[256], name[256], alphaName[256];
    if (!(matFlags & MATF_ENV_TEXTURE)) {
        dtaRead(g_loadFile, &diffuse, 0x48);
    } else {
        dtaRead(g_loadFile, &diffuse, 0x4C);
        ReadName(envName, g_loadFile);
    }
    int length = 0;
    dtaRead(g_loadFile, &length, 1);
    name[0] = 0;
    alphaName[0] = 0;
    if (length) {
        dtaRead(g_loadFile, name, length);
        name[length] = 0;
        if ((matFlags & MATF_ALPHA) && !(matFlags & 0xa1000000))
            ReadName(alphaName, g_loadFile);
    }
    int frames = 0, envFrames = 0;
    dtaRead(g_loadFile, &frames, 1);
    dtaRead(g_loadFile, &envFrames, 1);
    dtaRead(g_loadFile, &loopStart, 0x10);      // loopStart, interval, envLoopStart, envInterval
    const uint32_t tf = TextureFlags();

    // The sequences: "name00.ext", "name01.ext" ... (names of under 6 characters: one empty frame).
    if (matFlags & MATF_ENV_TEXTURE) {
        const uint32_t envFlags = (g_driverStates[DRVSTATE_TEX_MIPMAP] ? ITC_MIPMAP : 0) | (tf & 0xffffef31) | 0x80000;
        if (envFrames < 2) {
            g_pIGraph->LoadTexture(envName, nullptr, envFlags, &envTexture);
        } else {
            const int at = static_cast<int>(std::strlen(envName)) - 6;
            if (at < 0) {
                envTextures.frames.PushBack(nullptr);
            } else {
                for (int i = 0; i < envFrames; ++i) {
                    std::sprintf(envName + at, "%02i", i);
                    envName[at + 2] = '.';
                    ITexture *t = nullptr;
                    g_pIGraph->LoadTexture(envName, nullptr, envFlags, &t);
                    envTextures.frames.PushBack(t);
                }
            }
        }
    }
    if (*name || *alphaName) {
        if (frames < 2) {
            g_pIGraph->LoadTexture(name, alphaName, tf, &texture);
        } else {
            const int at = static_cast<int>(std::strlen(name)) - 6;
            if (at < 0) {
                textures.frames.PushBack(nullptr);
            } else {
                char *alphaAt = nullptr;
                if ((matFlags & 0x2000000) && !(matFlags & 0xa1000000)) {
                    const int a = static_cast<int>(std::strlen(alphaName)) - 6;
                    if (a >= 0)
                        alphaAt = alphaName + a;
                }
                for (int i = 0; i < frames; ++i) {
                    std::sprintf(name + at, "%02i", i);
                    name[at + 2] = '.';
                    if (alphaAt) {
                        std::sprintf(alphaAt, "%02i", i);
                        alphaAt[2] = '.';
                    }
                    ITexture *t = nullptr;
                    g_pIGraph->LoadTexture(name, alphaName, tf, &t);
                    textures.frames.PushBack(t);
                }
            }
        }
    }
    SetFrame(-1);
    SetEnvFrame(-1);
}
