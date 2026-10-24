// Engine log (log.txt next to the executable) and error reporting.
#pragma once

#include <cstdint>

#include <windows.h>

// Exported debug output (cdecl, printf style).
extern "C" __declspec(dllexport) int dbgPrintf(const char *format, ...);      // line + newline
extern "C" __declspec(dllexport) int dbgPrintf1(const char *format, ...);     // no newline
extern "C" __declspec(dllexport) int dbgPrintf2(const char *text, ...);       // the text as is

namespace ls3d {

// Set while the engine is told not to write its log (0x100d16f2).
extern bool g_logDisabled;

// Path of the log (0x100d1430): the executable's directory + "log.txt", built on first use.
const char *LogPath();
// Writes the "Date / time" header (truncating the log) and the engine banner (0x10068cb0, part).
void StartLog();

// Writes a GUID line to the log (0x10069400).
void LogGuid(const GUID *guid);

// Error callback the application installs (0x100d1d4c, S_IGraphInit::errorCallback; cdecl):
// (code, severity, text, context).
using ErrorCallback = void __cdecl(uint32_t code, int severity, const char *text, uint32_t context);
extern ErrorCallback *g_errorCallback;

// Severity levels of ReportError. Errors above kSevError terminate the process.
enum ErrorSeverity : int {
    kSevInfo = 0,
    kSevWarning = 1,
    kSevError = 2,
    kSevFatal = 3,
};

// Passes an error to the application's callback; a fatal one (severity > 2) exits (0x10069680).
void ReportError(HRESULT code, int severity, const char *text = nullptr, uintptr_t context = 0);

}  // namespace ls3d
