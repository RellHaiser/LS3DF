// Engine log and error reporting (0x10068cb0, 0x10069110 - 0x10069690).
//
// Memory-safety fixes: the original formats into a 1000-byte stack buffer with vsprintf and builds
// the log path with _splitpath into fixed buffers; the port uses bounded formatting and strings.
// dbgPrintf1 passed its formatted text to fprintf as a format string; the port writes it as text.
#include "core/debug.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <time.h>
#include <string>

namespace ls3d {

bool g_logDisabled = false;
ErrorCallback *g_errorCallback = nullptr;

namespace {

std::string g_logPath;      // 0x100d1430

FILE *OpenLog(const char *mode)
{
    return std::fopen(LogPath(), mode);
}

}  // namespace

// The log goes next to the executable: drive and directory of the command line (whose leading
// quote is skipped) plus "log.txt". As in the original, the directory is whatever _splitpath
// takes from the whole command line, arguments included.
const char *LogPath()
{
    if (g_logPath.empty()) {
        char drive[_MAX_DRIVE] = {}, dir[_MAX_DIR] = {};
        const char *cmd = GetCommandLineA();
        char line[_MAX_PATH * 4];
        std::snprintf(line, sizeof line, "%s", cmd);
        _splitpath_s(line, drive, sizeof drive, dir, sizeof dir, nullptr, 0, nullptr, 0);
        g_logPath = drive;
        g_logPath += dir[0] == '"' ? dir + 1 : dir;
        g_logPath += "log.txt";
    }
    return g_logPath.c_str();
}

void StartLog()
{
    if (FILE *f = OpenLog("wt")) {
        char date[16], time[16];
        _strdate_s(date, sizeof date);
        _strtime_s(time, sizeof time);
        std::fprintf(f, "Date: %s    time:%s\n", date, time);
        std::fclose(f);
        dbgPrintf("LS3D Engine V5.59.5           %s    GetVer:%d\n", "Aug 16 2004", 0x22f);
    }
}

// 0x10069400
void LogGuid(const GUID *guid)
{
    if (g_logDisabled || !guid)
        return;
    if (FILE *f = OpenLog("at")) {
        std::fprintf(f, "                GUID: %08X-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X\n",
                     static_cast<unsigned>(guid->Data1), guid->Data2, guid->Data3, guid->Data4[0], guid->Data4[1],
                     guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
        std::fclose(f);
    }
}

// 0x10069680
void ReportError(HRESULT code, int severity, const char *text, uintptr_t context)
{
    if (g_errorCallback)
        g_errorCallback(static_cast<uint32_t>(code), severity, text ? text : "", static_cast<uint32_t>(context));
    if (severity > kSevError)
        std::exit(-1);
}

}  // namespace ls3d

using ls3d::g_logDisabled;

// 0x10069110: one formatted line. Returns the number of characters formatted, -1 without a log.
extern "C" int dbgPrintf(const char *format, ...)
{
    if (g_logDisabled)
        return 0;
    FILE *f = ls3d::OpenLog("at");
    if (!f)
        return -1;
    char buf[1000];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    std::fprintf(f, "%s\n", buf);
    std::fclose(f);
    return n;
}

// 0x10069210: formatted text without a newline.
extern "C" int dbgPrintf1(const char *format, ...)
{
    if (g_logDisabled)
        return 0;
    FILE *f = ls3d::OpenLog("at");
    if (!f)
        return -1;
    char buf[1000];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    std::fputs(buf, f);
    std::fclose(f);
    return n;
}

// 0x10069310: the text unformatted (the arguments are ignored); returns fwrite's count.
extern "C" int dbgPrintf2(const char *text, ...)
{
    if (g_logDisabled)
        return 0;
    FILE *f = ls3d::OpenLog("at");
    if (!f)
        return -1;
    const int n = static_cast<int>(std::fwrite(text, std::strlen(text), 1, f));
    std::fclose(f);
    return n;
}
