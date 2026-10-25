// Port enhancements: LS3DF.ini (core/enhance.h).
#include "core/enhance.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/debug.h"

namespace ls3d {

namespace {

// LS3DF.ini next to the module this code is linked into (the DLL; a test's executable).
S_enhancements ReadIni()
{
    S_enhancements o = {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ReadIni), &module))
        return o;
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return o;
    char *slash = std::strrchr(path, '\\');
    if (!slash || static_cast<size_t>(slash + 1 - path) + sizeof "LS3DF.ini" > MAX_PATH)
        return o;
    std::strcpy(slash + 1, "LS3DF.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return o;
    auto get = [&](const char *key) { return static_cast<int>(GetPrivateProfileIntA("Graphics", key, 0, path)); };
    const int aniso = get("AnisotropicFiltering");
    o.anisotropy = aniso < 2 ? 0 : aniso > 16 ? 16 : aniso;
    o.trilinear = get("TrilinearFiltering") != 0;
    o.mipmaps = get("ImprovedMipmaps") != 0;
    o.trueColorTextures = get("TrueColorTextures") != 0;
    o.trueColorLightmaps = get("TrueColorLightmaps") != 0;
    return o;
}

S_enhancements &Options()
{
    static S_enhancements options = ReadIni();
    return options;
}

}  // namespace

const S_enhancements &Enhancements() { return Options(); }

void SetEnhancements(const S_enhancements &options) { Options() = options; }

void EnhancementLog(const char *format, ...)
{
    char buf[1000];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    const bool disabled = g_logDisabled;
    g_logDisabled = false;
    dbgPrintf("%s", buf);
    g_logDisabled = disabled;
}

}  // namespace ls3d
