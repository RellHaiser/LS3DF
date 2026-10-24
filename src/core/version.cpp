// Engine version and result-code exports (0x10068f20 - 0x1006909c).
#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace {

constexpr int kEngineVersion = 0x22F;       // 5.59
char g_unknownResult[256];                  // 0x100d1530

}  // namespace

// 0x10068f20
extern "C" __declspec(dllexport) uint32_t GetEngineVersion()
{
    return kEngineVersion;
}

// 0x10068f30: checks the version the client was built for. The original returns 0x201 on a match
// and otherwise the message box result with its low byte cleared (callers test AL).
extern "C" __declspec(dllexport) uint32_t TEV(const char *who, int version)
{
    if (version == kEngineVersion)
        return 0x201;
    char text[128];
    std::snprintf(text, sizeof text, "Bad version - engine:%d  client:%d", kEngineVersion, version);
    return static_cast<uint32_t>(MessageBoxA(nullptr, text, who, MB_ICONHAND | MB_SYSTEMMODAL)) & 0xFFFFFF00u;
}

// 0x10068fc0: text of an I3D result code.
extern "C" __declspec(dllexport) const char *I3DGetResultMessage(int32_t result)
{
    static const char *const kErrors[] = {
        "File not found",                       // 0x80000001
        "Out of memory",
        "Bad texture format",
        "Bad texture",
        "Generic error",                        // 0x80000005
        "Object not found",
        "Invalid parameter",
        "Not loaded",
        "Not initialized",
        "File corrupted",                       // 0x8000000A
        "No additional file (opacity map)",
        "Cyclic link of hierarchy",
        "Enumeration canceled",
        "Unsupported",
        "No 3D hardware",                       // 0x8000000F
        "Sound buffer lost",
        "User break",
        "File system error",
        "Bad version of file",
        "Bad timestamp of file",                // 0x80000014
        "Some limit of engine is exceeded",
        "Unreleased frame",
        "Unreleased anim",
        "Unreleased ITexture",
        "Unreleased IShow",                     // 0x80000019
    };
    if (result == 0)
        return "OK";
    const uint32_t index = static_cast<uint32_t>(result) - 0x80000001u;
    if (result < 0 && index < sizeof kErrors / sizeof kErrors[0])
        return kErrors[index];
    std::snprintf(g_unknownResult, sizeof g_unknownResult, "Unknown result:0x%x", static_cast<unsigned>(result));
    return g_unknownResult;
}
