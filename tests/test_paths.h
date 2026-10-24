// Where the tests find the original game. The original LS3DF.dll: the test's first argument, else
// %HD2_DIR%\LS3DF.dll, else the LS3DF.dll on the DLL search path (the game folder has to be on PATH
// anyway: the original imports rw_data.dll, binkw32.dll, vorbisfile.dll and ogg.dll from it).
// compare_loader also lists the game's files from the unpacked archives: %HD2_UNPACKED%, else
// HD2_Unpacked next to the game folder.
#pragma once

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// The linker version in a DLL's header (the original is an MSVC 6 build: 6), -1 if unreadable.
inline int LinkerVersion(const char *path)
{
    int version = -1;
    if (FILE *f = std::fopen(path, "rb")) {
        IMAGE_DOS_HEADER dos = {};
        IMAGE_NT_HEADERS32 nt = {};
        if (std::fread(&dos, sizeof dos, 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            !std::fseek(f, dos.e_lfanew, SEEK_SET) && std::fread(&nt, sizeof nt, 1, f) == 1 &&
            nt.Signature == IMAGE_NT_SIGNATURE)
            version = nt.OptionalHeader.MajorLinkerVersion;
        std::fclose(f);
    }
    return version;
}

// The original LS3DF.dll (`arg` if given and not empty); exits when there is none.
inline const char *OriginalDllPath(const char *arg = nullptr)
{
    static std::string path;
    if (arg && *arg) {
        path = arg;
    } else if (const char *dir = std::getenv("HD2_DIR"); dir && *dir) {
        path = std::string(dir) + "\\LS3DF.dll";
    } else {
        char found[MAX_PATH];
        if (SearchPathA(nullptr, "LS3DF.dll", nullptr, MAX_PATH, found, nullptr))
            path = found;
    }
    if (path.empty()) {
        std::printf("no original LS3DF.dll: pass its path, set HD2_DIR to the game folder or put the folder on PATH\n");
        std::exit(2);
    }
    const int version = LinkerVersion(path.c_str());
    if (version != 6) {
        std::printf("%s is not the original LS3DF.dll (linker version %d, the original's is 6)\n", path.c_str(),
                    version);
        std::exit(2);
    }
    return path.c_str();
}

// The unpacked game data (MODELS, MISSIONS, ...) next to `gameDir`, or %HD2_UNPACKED%.
inline std::string UnpackedDir(const std::string &gameDir)
{
    if (const char *dir = std::getenv("HD2_UNPACKED"); dir && *dir)
        return dir;
    return gameDir.substr(0, gameDir.find_last_of('\\')) + "\\HD2_Unpacked";
}
