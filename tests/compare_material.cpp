// Differential test of I3D_material and I3D_anim_material (src/i3d/material.cpp): the original
// LS3DF.dll's materials (made by its driver, slots 0x58 / 0x5C) and the port's run the same random
// operations - the color, power and flag setters, SetAlpha, the textures and their alpha flags,
// CopyFrom between both kinds, SetEnvironment, Release and extra references, Load from generated
// model-file blocks (names, alpha names, frame sequences) under random driver texture states, the
// animated sequences' updates as the driver time moves (looped, and random: each side's CRT rand
// seeded the same) and SetFrame / SetEnvFrame. Textures are fake objects (a pool per side, the same
// flags); IGraph is a fake whose LoadTexture logs its arguments and hands out the pool's textures
// by name. After every operation the materials' fields (textures as pool indices), the textures'
// reference counts, the files' read positions and the LoadTexture logs must be identical.
//
// The original crashes where the port refuses or guards (see material.cpp): an animated CopyFrom
// from null, or from an animated material with frames but no current texture; SetEnvFrame while
// the texture sequence's frame is past the end of the environment sequence. The test avoids those.
//
// Usage: compare_material.exe [original LS3DF.dll] [operations] [seed]
//
// test-sources: src/i3d/material.cpp
#include <windows.h>

#include <float.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/external/rw_data.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/loader.h"
#include "../src/i3d/material.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/itexture.h"
#include "test_paths.h"

// ---------------------------------------------------------------- engine parts not linked

namespace ls3d::i3d {
uint32_t g_driverStates[DRVSTATE_COUNT];
int g_loadFile;
int g_driverTime;
}  // namespace ls3d::i3d
IGraph *g_pIGraph;

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

uint8_t *g_base;
template <class T> T &Orig(uint32_t va) { return *reinterpret_cast<T *>(g_base + (va - 0x10000000)); }
template <class F> F OrigFn(uint32_t va) { return reinterpret_cast<F>(g_base + (va - 0x10000000)); }
template <class T> T &Field(void *object, size_t offset) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset); }

// A virtual __stdcall method of an original object.
template <class R, class... A> R CallV(void *object, int slot, A... args)
{
    using F = R(__stdcall *)(void *, A...);
    return (*reinterpret_cast<F **>(object))[slot / 4](object, args...);
}

uint8_t *g_origDriver;
std::mt19937 g_rng;
long g_op;
const char *g_lastOp = "start";

int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }
float RandFloat(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g_rng() % 100001) / 100000.0f; }

[[noreturn]] void Fail(const char *what, long a, long b)
{
    std::printf("MISMATCH after op %ld (%s): %s: original %ld (0x%lx), port %ld (0x%lx)\n", g_op, g_lastOp, what, a,
                static_cast<unsigned long>(a), b, static_cast<unsigned long>(b));
    std::exit(1);
}

void Check(const char *what, long a, long b)
{
    if (a != b)
        Fail(what, a, b);
}

// ---------------------------------------------------------------- fake textures and IGraph

constexpr int kTex = 10;
alignas(16) uint8_t g_textures[2][kTex][sizeof(ITexture)];  // [0] original side, [1] port side
void *g_textureVtable[32];

ITexture *Tex(int side, int k) { return reinterpret_cast<ITexture *>(g_textures[side][k]); }
int &RefOf(void *t) { return Field<int>(t, 0x14); }
uint32_t &FlagsOf(void *t) { return Field<uint32_t>(t, 0x28); }

int TexIndex(const void *t, int side)
{
    if (!t)
        return -1;
    for (int k = 0; k < kTex; ++k) {
        if (t == g_textures[side][k])
            return k;
    }
    return -2;
}

int __stdcall FakeRelease(void *t) { return --RefOf(t); }

void __cdecl FakeUnexpected()
{
    std::printf("unexpected call of a fake texture or IGraph method in op %ld (%s)\n", g_op, g_lastOp);
    std::exit(3);
}

struct FakeIGraph {
    void **vtable;
    int side;
};
void *g_igraphVtable[96];
FakeIGraph g_fakeIGraph[2];
std::vector<std::string> g_log[2];

HRESULT __stdcall FakeLoadTexture(FakeIGraph *self, const char *name, const char *alphaName, uint32_t flags, ITexture **out)
{
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", flags);
    g_log[self->side].push_back(std::string(name ? name : "<null>") + "|" + (alphaName ? alphaName : "<null>") + "|" + hex);
    uint32_t h = 2166136261u;
    for (const char *s : {name, alphaName}) {
        for (; s && *s; ++s)
            h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
        h = (h ^ 0x7c) * 16777619u;
    }
    if (h % 7 == 0)
        return E_FAIL;
    ITexture *t = Tex(self->side, static_cast<int>(h / 7 % kTex));
    ++RefOf(t);
    *out = t;
    return S_OK;
}

uint32_t RandTextureFlags()
{
    static const uint32_t kBits[] = {ITC_SOURCE_ALPHA, ITC_USE_ALPHA, ITC_ALPHA_1BIT, ITC_STREAMED, ITC_MIPMAP,
                                     ITC_COMPRESS, ITC_TRUECOLOR, ITC_PIXEL_MASK, ITC_QUEUED};
    uint32_t f = 0;
    for (uint32_t b : kBits) {
        if (Rand(3) == 0)
            f |= b;
    }
    return f;
}

// ---------------------------------------------------------------- the materials

constexpr int kMats = 16;
struct Slot {
    uint8_t *orig;
    I3D_material *port;
    bool anim;
};
Slot g_mats[kMats];

constexpr uint32_t kOrigLoad = 0x1002bbe0;
constexpr uint32_t kOrigAnimLoad = 0x1002be30;
constexpr uint32_t kOrigLoadFile = 0x100d00f0;
constexpr uint32_t kOrigDriverStates = 0x100be058;
constexpr uint32_t kOrigDriverTime = 0x100be394;
constexpr uint32_t kOrigIGraph = 0x100d16f4;

I3D_anim_material *Anim(const Slot &s) { return static_cast<I3D_anim_material *>(s.port); }

void CompareSequence(const char *what, uint8_t *o, uint8_t *p)
{
    std::string w(what);
    Check((w + " data").c_str(), Field<void *>(o, 0) != nullptr, Field<void *>(p, 0) != nullptr);
    Check((w + " capacity").c_str(), Field<int>(o, 4), Field<int>(p, 4));
    const int n = Field<int>(o, 8);
    Check((w + " size").c_str(), n, Field<int>(p, 8));
    Check((w + " grow").c_str(), Field<int>(o, 0xC), Field<int>(p, 0xC));
    for (int k = 0; k < n; ++k)
        Check((w + " frame texture").c_str(), TexIndex(Field<void **>(o, 0)[k], 0), TexIndex(Field<void **>(p, 0)[k], 1));
    Check((w + " lastTime").c_str(), Field<int>(o, 0x10), Field<int>(p, 0x10));
    Check((w + " elapsed").c_str(), Field<int>(o, 0x14), Field<int>(p, 0x14));
    Check((w + " frame").c_str(), Field<int>(o, 0x18), Field<int>(p, 0x18));
}

std::string Offset(const char *what, int offset)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%s +0x%02x", what, offset);
    return text;
}

void CompareAll()
{
    for (int i = 0; i < kMats; ++i) {
        const Slot &s = g_mats[i];
        if (!s.orig)
            continue;
        uint8_t *o = s.orig;
        auto *p = reinterpret_cast<uint8_t *>(s.port);
        Check("refCount", Field<int>(o, 4), Field<int>(p, 4));
        Check("matFlags", Field<uint32_t>(o, 8), Field<uint32_t>(p, 8));
        Check("texture", TexIndex(Field<void *>(o, 0xC), 0), TexIndex(Field<void *>(p, 0xC), 1));
        Check("envTexture", TexIndex(Field<void *>(o, 0x10), 0), TexIndex(Field<void *>(p, 0x10), 1));
        for (int off = 0x14; off < 0x60; off += 4)
            Check(Offset("colors", off).c_str(), Field<uint32_t>(o, off), Field<uint32_t>(p, off));
        if (s.anim) {
            CompareSequence("textures", o + 0x60, p + 0x60);
            CompareSequence("envTextures", o + 0x7C, p + 0x7C);
            for (int off = 0x98; off < 0xA8; off += 4)
                Check(Offset("timing", off).c_str(), Field<int>(o, off), Field<int>(p, off));
        }
    }
    for (int k = 0; k < kTex; ++k)
        Check("texture reference count", RefOf(Tex(0, k)), RefOf(Tex(1, k)));
    Check("LoadTexture calls", static_cast<long>(g_log[0].size()), static_cast<long>(g_log[1].size()));
    for (size_t k = 0; k < g_log[0].size(); ++k) {
        if (g_log[0][k] != g_log[1][k]) {
            std::printf("MISMATCH after op %ld (%s): LoadTexture %zu: original %s, port %s\n", g_op, g_lastOp, k,
                        g_log[0][k].c_str(), g_log[1][k].c_str());
            std::exit(1);
        }
    }
    g_log[0].clear();
    g_log[1].clear();
}

// A texture argument: a pool index or null.
int RandTex() { return Rand(kTex + 2) - 2 < 0 ? -1 : Rand(kTex); }
ITexture *TexArg(int k, int side) { return k < 0 ? nullptr : Tex(side, k); }

float RandAlpha()
{
    switch (Rand(10)) {
    case 0: return -1.0f;
    case 1: return -0.0f;
    case 2: return 0.0f;
    case 3: return 1.0f;
    case 4: return 1.0000001f;
    case 5: return 2.0f;
    case 6: return std::nanf("");
    case 7: return HUGE_VALF;
    default: return RandFloat(-0.5f, 1.5f);
    }
}

// ---------------------------------------------------------------- the model-file blocks

std::string RandName(bool allowEmpty)
{
    const int kind = Rand(10);
    if (allowEmpty && kind < 2)
        return "";
    if (kind < 4) {
        std::string s;
        for (int n = 1 + Rand(5); n > 0; --n)
            s += static_cast<char>('a' + Rand(3));
        return s;
    }
    std::string s = "t";
    for (int n = Rand(8); n > 0; --n)
        s += static_cast<char>('a' + Rand(4));
    s += static_cast<char>('0' + Rand(2));
    s += static_cast<char>('0' + Rand(3));
    s += Rand(2) ? ".bmp" : ".tga";
    return s;
}

void PutName(std::vector<uint8_t> &b, const std::string &s)
{
    b.push_back(static_cast<uint8_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

void PutInt(std::vector<uint8_t> &b, int v)
{
    uint8_t bytes[4];
    std::memcpy(bytes, &v, 4);
    b.insert(b.end(), bytes, bytes + 4);
}

int RandInterval()
{
    static const int kIntervals[] = {0, 1, 10, 33, 100, 250, -5};
    return kIntervals[Rand(Rand(8) == 0 ? 7 : 6)];
}

uint32_t RandMaterialFlags(bool anim)
{
    static const uint32_t kBits[] = {MATF_ENV_TEXTURE, MATF_ALPHA, MATF_COLORKEY, 0x80000000u, 0x01000000u,
                                     0x02000000u, 0x00100000u, 0x00200000u, 0x00400000u, 0x00800000u, 0x00008000u,
                                     0x00000800u, 0x00010000u, MATF_TEXTURE};
    uint32_t f = 1 + static_cast<uint32_t>(Rand(4)) * 0x100;
    for (uint32_t b : kBits) {
        if (Rand(3) == 0)
            f |= b;
    }
    return anim ? f | MATF_ANIMATED : f & ~MATF_ANIMATED;
}

long g_loads, g_sequenceLoads, g_frameSteps, g_randomSteps, g_copiedSequences;

// Load on a fresh material: a generated block, read through two handles of one file.
void LoadMaterial(Slot &s)
{
    const uint32_t flags = RandMaterialFlags(s.anim);
    Field<uint32_t>(s.orig, 8) = flags;
    s.port->matFlags = flags;
    std::vector<uint8_t> b;
    const int colorBytes = flags & MATF_ENV_TEXTURE ? 0x4C : 0x48;
    for (int k = 0; k < colorBytes; k += 4) {
        float f = Rand(20) == 0 ? std::nanf("") : RandFloat(-0.5f, 2.0f);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        PutInt(b, static_cast<int>(bits));
    }
    if (flags & MATF_ENV_TEXTURE)
        PutName(b, RandName(true));
    const std::string name = RandName(true);
    PutName(b, name);
    if (!name.empty() && (flags & MATF_ALPHA) && !(flags & 0xa1000000))
        PutName(b, RandName(true));
    if (s.anim && (name.empty() ? Rand(2) : 1)) {
        static const int kCounts[] = {0, 1, 2, 3, 5, 12, 2, 4};
        const int frames = kCounts[Rand(8)], envFrames = kCounts[Rand(8)];
        b.push_back(static_cast<uint8_t>(frames));
        b.push_back(static_cast<uint8_t>(envFrames));
        PutInt(b, Rand(frames + 1) - 1);
        PutInt(b, RandInterval());
        PutInt(b, Rand(envFrames + 1) - 1);
        PutInt(b, RandInterval());
    }
    for (int k = 0; k < 16; ++k)
        b.push_back(static_cast<uint8_t>(g_rng()));

    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\compare_material_%lu.bin", std::getenv("TEMP"), GetCurrentProcessId());
    FILE *f = std::fopen(path, "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    const int ho = dtaOpen(path, false), hp = dtaOpen(path, false);
    if (ho < 0 || hp < 0) {
        std::printf("cannot open %s through rw_data\n", path);
        std::exit(2);
    }
    Orig<int>(kOrigLoadFile) = ho;
    g_loadFile = hp;
    OrigFn<void(__fastcall *)(void *, void *)>(s.anim ? kOrigAnimLoad : kOrigLoad)(s.orig, nullptr);
    if (s.anim)
        Anim(s)->Load();
    else
        s.port->Load();
    Check("read position", dtaSeek(ho, 0, 1), dtaSeek(hp, 0, 1));
    dtaClose(ho);
    dtaClose(hp);
    DeleteFileA(path);
    ++g_loads;
    if (s.anim && (Anim(s)->textures.frames.Size() > 1 || Anim(s)->envTextures.frames.Size() > 1))
        ++g_sequenceLoads;
}

void Create(Slot &s, bool anim)
{
    s.anim = anim;
    s.orig = CallV<uint8_t *>(g_origDriver, anim ? 0x5C : 0x58);
    s.port = anim ? new I3D_anim_material : new I3D_material;
    if (!s.orig || !s.port) {
        std::printf("no material\n");
        std::exit(2);
    }
}

// The original reads past the frames or calls through null in these (see material.cpp).
bool OrigCopyCrashes(const Slot &dest, const Slot *src)
{
    if (!dest.anim)
        return false;
    if (!src)
        return true;
    if (src == &dest || !src->anim)
        return false;
    const I3D_anim_material *a = Anim(*src);
    return (a->textures.frames.Size() > 0 && !a->texture) || (a->envTextures.frames.Size() > 0 && !a->envTexture);
}

void RunOp()
{
    Slot &s = g_mats[Rand(kMats)];
    if (!s.orig) {
        g_lastOp = "create";
        Create(s, Rand(2) != 0);
        if (Rand(3) != 0) {
            g_lastOp = "create + Load";
            LoadMaterial(s);
        }
        return;
    }
    uint8_t *o = s.orig;
    I3D_material *p = s.port;
    switch (Rand(24)) {
    case 0:
    case 22:
    case 23: {
        g_lastOp = "Release";
        const int before = p->refCount;
        Check("Release", static_cast<long>(CallV<uint32_t>(o, 0x00)), static_cast<long>(p->Release()));
        if (before == 1)
            s = Slot{};
        break;
    }
    case 1:
        if (Rand(3) != 0)
            break;
        g_lastOp = "reference";
        ++Field<int>(o, 4);
        ++p->refCount;
        break;
    case 2: {
        g_lastOp = "color setters";
        const S_vector c{RandFloat(-1.0f, 2.0f), RandFloat(-1.0f, 2.0f), RandFloat(-1.0f, 2.0f)};
        const int slot = 0x04 + 4 * Rand(4);
        I3D_RESULT rp = 0;
        switch (slot) {
        case 0x04: rp = p->SetAmbient(c); break;
        case 0x08: rp = p->SetDiffuse(c); break;
        case 0x0C: rp = p->SetEmissive(c); break;
        default: rp = p->SetSpecular(c); break;
        }
        Check("color setter", CallV<I3D_RESULT>(o, slot, &c), rp);
        break;
    }
    case 3: {
        g_lastOp = "SetPower / SetOpacity";
        const float v = Rand(20) == 0 ? std::nanf("") : RandFloat(-10.0f, 100.0f);
        if (Rand(2))
            Check("SetPower", CallV<I3D_RESULT>(o, 0x14, v), p->SetPower(v));
        else
            Check("SetOpacity", CallV<I3D_RESULT>(o, 0x18, v), p->SetOpacity(v));
        break;
    }
    case 4:
    case 5: {
        g_lastOp = "SetTexture";
        const int k = RandTex();
        Check("SetTexture", CallV<I3D_RESULT>(o, 0x1C, TexArg(k, 0)), p->SetTexture(TexArg(k, 1)));
        break;
    }
    case 6:
    case 7: {
        g_lastOp = "SetEnvTexture";
        const int k = RandTex();
        Check("SetEnvTexture", CallV<I3D_RESULT>(o, 0x2C, TexArg(k, 0)), p->SetEnvTexture(TexArg(k, 1)));
        break;
    }
    case 8:
    case 9: {
        g_lastOp = "CopyFrom";
        Slot *src = &g_mats[Rand(kMats)];
        if (!src->orig)
            src = nullptr;
        if (OrigCopyCrashes(s, src))
            break;
        const bool sequences = src && src->anim && s.anim &&
                               (Anim(*src)->textures.frames.Size() > 1 || Anim(*src)->envTextures.frames.Size() > 1);
        Check("CopyFrom", CallV<I3D_RESULT>(o, 0x20, src ? src->orig : nullptr), p->CopyFrom(src ? src->port : nullptr));
        if (sequences)
            ++g_copiedSequences;
        break;
    }
    case 10:
        g_lastOp = "SetTwoSided";
        Check("SetTwoSided", CallV<I3D_RESULT>(o, 0x24), p->SetTwoSided());
        break;
    case 11: {
        g_lastOp = "SetEnvironment";
        const int k = RandTex();
        const uint32_t b8 = static_cast<uint32_t>(Rand(10)), b12 = static_cast<uint32_t>(Rand(10));
        const float a = RandAlpha();
        Check("SetEnvironment", CallV<I3D_RESULT>(o, 0x28, TexArg(k, 0), b8, b12, a),
              p->SetEnvironment(TexArg(k, 1), b8, b12, a));
        break;
    }
    case 12: {
        g_lastOp = "SetBits8 / 12 / 0";
        const uint32_t v = static_cast<uint32_t>(Rand(Rand(4) == 0 ? 300 : 8));
        switch (Rand(3)) {
        case 0: Check("SetBits8", CallV<I3D_RESULT>(o, 0x30, v), p->SetBits8(v)); break;
        case 1: Check("SetBits12", CallV<I3D_RESULT>(o, 0x34, v), p->SetBits12(v)); break;
        default: Check("SetBits0", CallV<I3D_RESULT>(o, 0x38, v), p->SetBits0(v)); break;
        }
        break;
    }
    case 13: {
        g_lastOp = "SetAlpha";
        const float a = RandAlpha();
        Check("SetAlpha", CallV<I3D_RESULT>(o, 0x3C, a), p->SetAlpha(a));
        break;
    }
    case 14:
    case 15:
    case 16: {
        g_lastOp = "UpdateTexture / UpdateEnvTexture";
        const bool env = Rand(2) != 0;
        const int before = s.anim ? (env ? Anim(s)->envTextures.frame : Anim(s)->textures.frame) : 0;
        CallV<void>(o, env ? 0x48 : 0x44);
        if (env)
            p->UpdateEnvTexture();
        else
            p->UpdateTexture();
        if (s.anim) {
            const int after = env ? Anim(s)->envTextures.frame : Anim(s)->textures.frame;
            const int start = env ? Anim(s)->envLoopStart : Anim(s)->loopStart;
            if (after != before)
                ++(start < 0 ? g_randomSteps : g_frameSteps);
        }
        break;
    }
    case 17:
    case 18: {
        if (!s.anim)
            break;
        g_lastOp = "SetFrame / SetEnvFrame";
        I3D_anim_material *a = Anim(s);
        const bool env = Rand(2) != 0;
        const int n = env ? a->envTextures.frames.Size() : a->textures.frames.Size();
        const int frame = Rand(2 * n + 4) - 2;
        if (env) {
            if (n > 0 && Field<int>(o, 0x78) >= n)
                break;
            Check("SetEnvFrame", CallV<bool>(o, 0x50, frame), a->SetEnvFrame(frame));
        } else {
            Check("SetFrame", CallV<bool>(o, 0x4C, frame), a->SetFrame(frame));
        }
        break;
    }
    case 19: {
        g_lastOp = "time";
        static const int kSteps[] = {0, 1, 5, 17, 40, 100, 333, 1000};
        g_driverTime += kSteps[Rand(8)];
        Orig<int>(kOrigDriverTime) = g_driverTime;
        break;
    }
    case 20: {
        g_lastOp = "driver texture states";
        static const int kStates[] = {DRVSTATE_TEX_MIPMAP, DRVSTATE_TEX_DITHER, DRVSTATE_TEX_TRUECOLOR,
                                      DRVSTATE_TEX_HALF, DRVSTATE_TEX_COMPRESS};
        for (int st : kStates) {
            const uint32_t v = Rand(2) ? static_cast<uint32_t>(Rand(3)) : 0;
            g_driverStates[st] = v;
            Orig<uint32_t>(kOrigDriverStates + 4 * st) = v;
        }
        break;
    }
    default: {
        g_lastOp = "texture flags";
        const int k = Rand(kTex);
        const uint32_t f = RandTextureFlags();
        FlagsOf(Tex(0, k)) = f;
        FlagsOf(Tex(1, k)) = f;
        break;
    }
    }
}

// A crash names the operation it happened in.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
    std::printf("CRASH (0x%08lx at %p) in op %ld (%s)\n", info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress, g_op, g_lastOp);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const long ops = argc > 2 ? std::atol(argv[2]) : 200000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1;
    HMODULE dll = LoadLibraryA(origPath);
    if (!dll) {
        std::printf("cannot load %s (%lu)\n", origPath, GetLastError());
        return 2;
    }
    g_base = reinterpret_cast<uint8_t *>(dll);
    Orig<uint8_t>(0x100d16f2) = 1;              // no log
    g_origDriver = Orig<uint8_t *>(0x100d16f8);
    if (!g_origDriver) {
        std::printf("the original's driver does not exist\n");
        return 2;
    }
    _controlfp(_PC_24, _MCW_PC);
    g_rng.seed(seed);

    for (void *&slot : g_textureVtable)
        slot = reinterpret_cast<void *>(&FakeUnexpected);
    g_textureVtable[0] = reinterpret_cast<void *>(&FakeRelease);
    for (int k = 0; k < kTex; ++k) {
        const uint32_t f = RandTextureFlags();
        for (int side = 0; side < 2; ++side) {
            Field<void *>(Tex(side, k), 0) = g_textureVtable;
            RefOf(Tex(side, k)) = 1;
            FlagsOf(Tex(side, k)) = f;
        }
    }
    for (void *&slot : g_igraphVtable)
        slot = reinterpret_cast<void *>(&FakeUnexpected);
    g_igraphVtable[0x3C / 4] = reinterpret_cast<void *>(&FakeLoadTexture);
    for (int side = 0; side < 2; ++side)
        g_fakeIGraph[side] = FakeIGraph{g_igraphVtable, side};
    void *const origIGraph = Orig<void *>(kOrigIGraph);
    Orig<void *>(kOrigIGraph) = &g_fakeIGraph[0];
    g_pIGraph = reinterpret_cast<IGraph *>(&g_fakeIGraph[1]);

    // The driver time starts at 0 (a sequence's "not updated yet"); both CRTs' rand alike.
    Orig<int>(kOrigDriverTime) = 0;
    g_driverTime = 0;
    void *ptd = OrigFn<void *(__cdecl *)()>(0x10090118)();     // _getptd
    Field<uint32_t>(ptd, 0x14) = seed;
    std::srand(seed);

    for (g_op = 0; g_op < ops; ++g_op) {
        RunOp();
        CompareAll();
    }
    Orig<void *>(kOrigIGraph) = origIGraph;
    std::printf("coverage: loads %ld (with sequences %ld), sequence steps %ld (random %ld), copied sequences %ld\n",
                g_loads, g_sequenceLoads, g_frameSteps, g_randomSteps, g_copiedSequences);
    std::printf("compare_material: %ld operations, seed %u: identical\n", ops, seed);
    return 0;
}
