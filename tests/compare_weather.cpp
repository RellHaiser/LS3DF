// Differential test of the scene's weather (src/i3d/scene_weather.cpp: UpdateWeather 0x1004a290,
// DrawWeather 0x1004b720; scene.cpp: SetWeather, ResetWeather, SetWeatherCount): the original
// LS3DF.dll and the port's built LS3DF.dll are loaded side by side, each on its own fake Direct3D
// device (vertex and index buffers in system memory), each makes a scene and a camera with its own
// driver, and both run the same frames: the driver time moves on (steps of 0 - 120 ms, now and then
// a long one), the render camera walks, turns, runs or jumps, and between frames the weather is
// changed through SetWeather (slot 0x11C): rain / snow, the particle count, fall speed, the
// particles' length and width, the box around the camera, the wind, the colours, the sector and
// dummy tests. Each side's CRT rand (RandFloat: the particles placed anew) is seeded alike.
//
// After each frame (UpdateWeather, then DrawWeather): the particles drawn, every byte of their
// vertices in the weather vertex buffer (positions, colours, uvs), every particle (position and
// height), the weather flags, the streaks' direction and its matrix, the four particle shapes,
// must be identical. Not compared: what the original leaves unset - the w components of the
// direction (rain: an unset local; snow: a word past a 3-float result) and of the shapes' rows.
//
// Usage: compare_weather.exe [original LS3DF.dll] [port LS3DF.dll] [frames] [seed]
//   Empty arguments take the defaults (the original as test_paths.h finds it, build\msvc\LS3DF.dll
//   with its PDB). COMPARE_WEATHER_TRACE set in the environment prints each step (for a crash).
//
// test-libs: dbghelp.lib
#include <windows.h>

#include <dbghelp.h>
#include <float.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/i3d/driver.h"
#include "fake_d3d8.h"
#include "test_paths.h"

namespace {

template <class R, class... A> R CallV(const void *object, int slot, A... args)
{
    using F = R(__stdcall *)(const void *, A...);
    return (*reinterpret_cast<F *const *>(object))[slot / 4](object, args...);
}
template <class T> T &Field(void *object, size_t offset) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset); }
uint32_t Bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// ---------------------------------------------------------------- the fake devices

struct FakeVB : FakeDirect3DVertexBuffer8Base {
    D3DVERTEXBUFFER_DESC desc = {};
    std::vector<uint8_t> data;
    HRESULT __stdcall Lock(UINT offset, UINT, BYTE **out, DWORD) override
    {
        *out = data.data() + offset;
        return S_OK;
    }
    HRESULT __stdcall Unlock() override { return S_OK; }
    HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC *out) override
    {
        *out = desc;
        return S_OK;
    }
    void OnFinalRelease() override { std::vector<uint8_t>().swap(data); }
};
struct FakeIB : FakeDirect3DIndexBuffer8Base {
    D3DINDEXBUFFER_DESC desc = {};
    std::vector<uint8_t> data;
    HRESULT __stdcall Lock(UINT offset, UINT, BYTE **out, DWORD) override
    {
        *out = data.data() + offset;
        return S_OK;
    }
    HRESULT __stdcall Unlock() override { return S_OK; }
    HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC *out) override
    {
        *out = desc;
        return S_OK;
    }
    void OnFinalRelease() override { std::vector<uint8_t>().swap(data); }
};
struct WeatherDevice : FakeDevice {
    HRESULT __stdcall ResourceManagerDiscardBytes(DWORD) override { return S_OK; }
    HRESULT __stdcall CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                         IDirect3DVertexBuffer8 **out) override
    {
        auto *b = new FakeVB;
        b->desc.Format = D3DFMT_VERTEXDATA;
        b->desc.Type = D3DRTYPE_VERTEXBUFFER;
        b->desc.Usage = usage;
        b->desc.Pool = pool;
        b->desc.Size = length;
        b->desc.FVF = fvf;
        b->data.assign(length, 0xcd);
        *out = b;
        return S_OK;
    }
    HRESULT __stdcall CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                        IDirect3DIndexBuffer8 **out) override
    {
        auto *b = new FakeIB;
        b->desc.Format = format;
        b->desc.Type = D3DRTYPE_INDEXBUFFER;
        b->desc.Usage = usage;
        b->desc.Pool = pool;
        b->desc.Size = length;
        b->data.assign(length, 0xcd);
        *out = b;
        return S_OK;
    }
};

// ---------------------------------------------------------------- the two DLLs

struct Side {
    HMODULE dll = nullptr;
    void *driver = nullptr;
    void *scene = nullptr;
    void *camera = nullptr;
    WeatherDevice device;
};
Side g_side[2];
HANDLE g_proc;

uint8_t *OrigVa(uint32_t va) { return reinterpret_cast<uint8_t *>(g_side[0].dll) + (va - 0x10000000); }

uint8_t *PortSymbol(const char *name)
{
    alignas(SYMBOL_INFO) char bytes[sizeof(SYMBOL_INFO) + 1024] = {};
    auto *s = reinterpret_cast<SYMBOL_INFO *>(bytes);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 1024;
    const std::string q = std::string("port!") + name;
    if (!SymFromName(g_proc, q.c_str(), s)) {
        std::printf("the port's symbol %s is not in its PDB (%lu)\n", name, GetLastError());
        std::exit(2);
    }
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(s->Address));
}

// A global or function of each side: the original's address, the port's name.
struct S_global {
    uint32_t va;
    const char *name;
};
uint8_t *Address(int side, const S_global &g) { return side ? PortSymbol(g.name) : OrigVa(g.va); }
template <class T> T &Global(int side, const S_global &g) { return *reinterpret_cast<T *>(Address(side, g)); }

const S_global kLogOff = {0x100d16f2, "ls3d::g_logDisabled"};
const S_global kDevice = {0x100d216c, "ls3d::ig::g_pDevice"};
const S_global kDriverTime = {0x100be394, "ls3d::i3d::g_driverTime"};
const S_global kRenderCamera = {0x100d127c, "ls3d::i3d::g_renderCamera"};
const S_global kWeatherVB = {0x100bf308, "ls3d::i3d::g_weatherVB"};
const S_global kWeatherDrawn = {0x100bf328, "ls3d::i3d::g_weatherDrawn"};
const S_global kDefaultTexture = {0x100d21b8, "ls3d::ig::g_defaultTexture"};
const S_global kUpdateWeather = {0x1004a290, "I3D_scene::UpdateWeather"};
const S_global kDrawWeather = {0x1004b720, "I3D_scene::DrawWeather"};

// The scene's weather members (I3D_scene, scene.h).
constexpr size_t kFlags = 0x2A0, kParticles = 0x2A4, kCount = 0x2C8, kWindStart = 0x2E0, kDirection = 0x2F0,
                 kShapes = 0x340, kPrimarySector = 0x1F8, kSectorFlags = 0x160;

// ---------------------------------------------------------------- the frames

std::mt19937 g_rng;
int Rand(int n) { return static_cast<int>(g_rng() % static_cast<uint32_t>(n)); }
float RandF(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g_rng() % 1000001) / 1000000.0f; }

struct S_vec {
    float x, y, z;
};
S_vec g_camPos = {0, 2, 0};
S_vec g_camDir = {0, 0, 1};

void SetWeather(int param, uint32_t value)
{
    for (Side &s : g_side)
        CallV<void>(s.scene, 0x11C, param, value);
}
void SetWeatherF(int param, float value) { SetWeather(param, Bits(value)); }

// A random change of the weather settings.
void ChangeWeather()
{
    switch (Rand(14)) {
    case 0: SetWeather(14, Rand(3) == 0 ? 1u : 0u); break;                       // snow / rain
    case 1: SetWeather(10, static_cast<uint32_t>(Rand(4) ? 50 + Rand(1500) : Rand(3) * 4000)); break;
    case 2: SetWeatherF(5, RandF(0.0f, 60.0f)); break;                            // fall speed (per s)
    case 3: SetWeatherF(6, RandF(0.05f, 3.0f)); break;                            // length
    case 4: SetWeatherF(7, Rand(4) ? RandF(0.01f, 0.2f) : RandF(0.2f, 2.0f)); break;   // width
    case 5: SetWeatherF(8, RandF(2.0f, 40.0f)); break;                            // box xz
    case 6: SetWeatherF(9, RandF(2.0f, 20.0f)); break;                            // box y
    case 7: case 8: {
        // The wind: mostly down, sometimes sideways or nothing.
        const float x = Rand(4) ? RandF(-0.5f, 0.5f) : RandF(-3, 3), z = Rand(4) ? RandF(-0.5f, 0.5f) : RandF(-3, 3);
        const float y = Rand(8) ? RandF(-2.0f, -0.2f) : RandF(-0.1f, 0.1f);
        SetWeatherF(11, Rand(10) ? x : 0.0f);
        SetWeatherF(12, Rand(10) ? y : 0.0f);
        SetWeatherF(13, Rand(10) ? z : 0.0f);
        break;
    }
    case 9: SetWeather(3, g_rng()); break;
    case 10: SetWeather(4, g_rng()); break;
    case 11: SetWeather(1, Rand(2) ? 1u : 0u); break;                            // the sector test
    case 12: SetWeather(2, Rand(2) ? 1u : 0u); break;                            // the dummies' boxes
    default: SetWeather(15, static_cast<uint32_t>(Rand(3))); break;
    }
}

// The camera's next position and direction: small steps (walking), runs, turns, jumps.
void MoveCamera()
{
    switch (Rand(10)) {
    case 0:
        g_camPos = {RandF(-100, 100), RandF(-5, 20), RandF(-100, 100)};
        break;
    case 1: case 2: {
        const float a = RandF(-3.14159f, 3.14159f), e = RandF(-1.2f, 1.2f);
        g_camDir = {std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a)};
        break;
    }
    case 3:
        break;                                                                    // standing still
    default: {
        const float step = Rand(3) ? RandF(0.0f, 0.15f) : RandF(0.2f, 2.0f);
        g_camPos.x += g_camDir.x * step;
        g_camPos.z += g_camDir.z * step;
        g_camPos.y += RandF(-0.02f, 0.02f);
        break;
    }
    }
    for (Side &s : g_side)
        CallV<void>(s.camera, 0x14, &g_camPos, &g_camDir, 0.0f);
}

// ---------------------------------------------------------------- the comparison

int g_bad = 0;
long g_frame = 0;
bool g_trace = std::getenv("COMPARE_WEATHER_TRACE") != nullptr;
void Trace(const char *what, int side = -1)
{
    if (g_trace) {
        std::printf("trace: frame %ld %s %d\n", g_frame, what, side);
        std::fflush(stdout);
    }
}

void Report(const char *what, size_t index, uint32_t a, uint32_t b)
{
    if (++g_bad <= 20)
        std::printf("frame %ld: %s [%zu]: original %08x, port %08x\n", g_frame, what, index, a, b);
}

void CompareWords(const char *what, const void *a, const void *b, size_t words, size_t skipEvery = 0, size_t skipAt = 0)
{
    const uint32_t *x = static_cast<const uint32_t *>(a), *y = static_cast<const uint32_t *>(b);
    for (size_t i = 0; i < words; ++i) {
        if (skipEvery && i % skipEvery == skipAt)
            continue;
        if (x[i] != y[i]) {
            Report(what, i, x[i], y[i]);
            return;
        }
    }
}

void Compare()
{
    void *a = g_side[0].scene, *b = g_side[1].scene;
    const int drawnA = Global<int>(0, kWeatherDrawn), drawnB = Global<int>(1, kWeatherDrawn);
    if (drawnA != drawnB)
        Report("particles drawn", 0, static_cast<uint32_t>(drawnA), static_cast<uint32_t>(drawnB));
    if (Field<uint32_t>(a, kFlags) != Field<uint32_t>(b, kFlags))
        Report("weather flags", 0, Field<uint32_t>(a, kFlags), Field<uint32_t>(b, kFlags));
    CompareWords("streak direction", &Field<float>(a, kWindStart), &Field<float>(b, kWindStart), 3);
    CompareWords("direction matrix", &Field<float>(a, kDirection), &Field<float>(b, kDirection), 16, 4, 3);
    CompareWords("particle shapes", &Field<float>(a, kShapes), &Field<float>(b, kShapes), 64, 4, 3);
    const int count = Field<int>(a, kCount);
    if (count != Field<int>(b, kCount))
        Report("particle count", 0, static_cast<uint32_t>(count), static_cast<uint32_t>(Field<int>(b, kCount)));
    else if (count)
        CompareWords("particles", Field<float *>(a, kParticles), Field<float *>(b, kParticles), static_cast<size_t>(count) * 4);
    // The drawn particles' vertices: 5 a particle, 0x18 bytes each, from the buffer's offset.
    if (drawnA == drawnB && drawnA > 0) {
        const uint8_t *v[2];
        for (int s = 0; s < 2; ++s) {
            uint8_t *alloc = Address(s, kWeatherVB);
            uint8_t *buffer = *reinterpret_cast<uint8_t **>(alloc);
            const uint32_t offset = *reinterpret_cast<uint32_t *>(alloc + 8);
            v[s] = *reinterpret_cast<uint8_t **>(buffer) + Field<uint32_t>(buffer, 0x14) * offset;
        }
        CompareWords("vertices", v[0], v[1], static_cast<size_t>(drawnA) * 30);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string exeDir = exe;
    exeDir = exeDir.substr(0, exeDir.find_last_of('\\'));
    const std::string portPath = argc > 2 && *argv[2] ? argv[2] : exeDir + "\\msvc\\LS3DF.dll";
    const long frames = argc > 3 && *argv[3] ? std::atol(argv[3]) : 20000;
    const uint32_t seed = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0)) : 1;
    g_rng.seed(seed);

    // The port loads from a copy (its PDB next to it): two modules named LS3DF.dll do not load.
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string work = std::string(tmp) + "ls3df_weather";
    CreateDirectoryA(work.c_str(), nullptr);
    const std::string portCopy = work + "\\LS3DF_port.dll";
    const std::string pdb = portPath.substr(0, portPath.size() - 4) + ".pdb";
    if (!CopyFileA(portPath.c_str(), portCopy.c_str(), FALSE) ||
        !CopyFileA(pdb.c_str(), (work + "\\LS3DF.pdb").c_str(), FALSE)) {
        std::printf("cannot copy %s and its PDB to %s (%lu)\n", portPath.c_str(), work.c_str(), GetLastError());
        return 2;
    }
    std::string gameDir = origPath;
    gameDir = gameDir.substr(0, gameDir.find_last_of('\\'));
    SetDllDirectoryA(gameDir.c_str());
    SetCurrentDirectoryA(work.c_str());
    g_side[0].dll = LoadLibraryA(origPath);
    g_side[1].dll = LoadLibraryA(portCopy.c_str());
    if (!g_side[0].dll || !g_side[1].dll) {
        std::printf("cannot load the DLLs (%lu)\n", GetLastError());
        return 2;
    }
    g_proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_EXACT_SYMBOLS);
    if (!SymInitialize(g_proc, work.c_str(), FALSE) ||
        !SymLoadModuleEx(g_proc, nullptr, portCopy.c_str(), "port", reinterpret_cast<DWORD64>(g_side[1].dll), 0,
                         nullptr, 0)) {
        std::printf("no PDB for the port (%lu)\n", GetLastError());
        return 2;
    }
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);

    using GetDriverFn = void *(__cdecl *)();
    using MemberFn = void(__fastcall *)(void *, void *);
    MemberFn update[2], draw[2];
    for (int s = 0; s < 2; ++s) {
        Side &side = g_side[s];
        Trace("setup", s);
        Global<bool>(s, kLogOff) = true;
        Global<IDirect3DDevice8 *>(s, kDevice) = &side.device;
        side.driver = reinterpret_cast<GetDriverFn>(GetProcAddress(side.dll, "I3DGetDriver"))();
        Trace("driver", s);
        side.scene = CallV<void *>(side.driver, 0x54, static_cast<int>(FRAME_SCENE));
        Trace("scene", s);
        side.camera = CallV<void *>(side.driver, 0x54, static_cast<int>(FRAME_CAMERA));
        void *primary = Field<void *>(side.scene, kPrimarySector);
        CallV<I3D_RESULT>(side.camera, 0x38, primary, 0u);
        CallV<void>(side.camera, 0x74, 1.2f);
        CallV<void>(side.camera, 0x8C, 4.0f / 3.0f);
        Field<uint32_t>(primary, kSectorFlags) |= 1;      // SCTF_VISIBLE: rendered this frame
        Global<void *>(s, kRenderCamera) = side.camera;
        // IGraph's default texture (DrawWeather's rain texture; made by IGraph::Init): an empty
        // ITexture, no Direct3D texture.
        static uint8_t s_texture[2][0x100];
        Global<void *>(s, kDefaultTexture) = s_texture[s];
        Global<int>(s, kDriverTime) = 0;
        update[s] = reinterpret_cast<MemberFn>(Address(s, kUpdateWeather));
        draw[s] = reinterpret_cast<MemberFn>(Address(s, kDrawWeather));
    }
    // Both CRTs' rand alike (the original's _getptd, the port's own srand).
    using GetPtdFn = uint8_t *(__cdecl *)();
    *reinterpret_cast<uint32_t *>(reinterpret_cast<GetPtdFn>(OrigVa(0x10090118))() + 0x14) = seed;
    reinterpret_cast<void(__cdecl *)(unsigned)>(PortSymbol("srand"))(seed);

    SetWeather(0, 1);
    MoveCamera();
    long drawnTotal = 0, snowFrames = 0;
    for (g_frame = 0; g_frame < frames && g_bad == 0; ++g_frame) {
        const int dt = Rand(40) ? Rand(121) : Rand(3000);
        for (int s = 0; s < 2; ++s)
            Global<int>(s, kDriverTime) += dt;
        MoveCamera();
        if (Rand(25) == 0) {
            ChangeWeather();
            for (int param = 0; param < 17; ++param) {
                const int a = CallV<int>(g_side[0].scene, 0x120, param), b = CallV<int>(g_side[1].scene, 0x120, param);
                if (a != b)
                    Report("GetWeather", static_cast<size_t>(param), static_cast<uint32_t>(a), static_cast<uint32_t>(b));
            }
        }
        for (int s = 0; s < 2; ++s) {
            Trace("update", s);
            update[s](g_side[s].scene, nullptr);
            Trace("draw", s);
            draw[s](g_side[s].scene, nullptr);
        }
        Trace("compare");
        drawnTotal += Global<int>(0, kWeatherDrawn);
        snowFrames += (Field<uint32_t>(g_side[0].scene, kFlags) & 0x40) != 0;
        Compare();
    }
    std::printf("compare_weather: %ld frames (%ld with snow), %ld particles drawn, seed %u: %s\n", g_frame, snowFrames,
                drawnTotal, seed, g_bad ? "DIFFER" : "identical");
    return g_bad ? 1 : 0;
}
