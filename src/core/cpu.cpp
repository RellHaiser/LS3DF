// Processor identification (0x100685a0 - 0x10068c90).
#include "core/cpu.h"

#include <cstring>
#include <intrin.h>

#include <windows.h>

#include "platform/platform.h"

namespace ls3d {

namespace {

// 0x10068620: the names the original knows, by vendor, family and model. Families it does not
// list leave the name empty; known families with an unknown model give "Unknown".
const char *CpuName(int family, int model, const char *vendor)
{
    if (std::strncmp("AuthenticAMD", vendor, 12) == 0) {
        if (family == 4)
            return "AMD Am486";
        if (family == 5) {
            switch (model) {
            case 0: case 1: case 2: case 3: return "AMD K5";
            case 4: case 5: return "";
            case 6: case 7: return "AMD K6";
            case 8: return "AMD K6-2";
            case 9: case 10: case 11: case 12: case 13: case 14: case 15: return "AMD K6-3";
            default: return "Unknown";
            }
        }
        if (family == 6)
            return "AMD ATHLON";
        return "";
    }
    if (std::strncmp("GenuineIntel", vendor, 12) == 0) {
        if (family == 4) {
            switch (model) {
            case 0: case 1: return "INTEL 486DX";
            case 2: return "INTEL 486SX";
            case 3: return "INTEL 486DX2";
            case 4: return "INTEL 486SL";
            case 5: return "INTEL 486SX2";
            case 7: return "INTEL 486DX2E";
            case 8: return "INTEL 486DX4";
            default: return "Unknown";
            }
        }
        if (family == 5) {
            if (model > 0 && model < 4)
                return "INTEL Pentium";
            if (model == 4)
                return "INTEL Pentium-MMX";
            return "Unknown";
        }
        if (family == 6) {
            switch (model) {
            case 1: return "INTEL Pentium-Pro";
            case 3: case 5: return "INTEL Pentium-II";
            case 6: return "INTEL Celeron";
            case 7: return "INTEL Pentium-III";
            default: return "Unknown";
            }
        }
        return "";
    }
    return "Unknown";          // Cyrix, Centaur and others
}

}  // namespace

uint32_t DetectCpu(S_cpuInfo *info)
{
    // (The original probes CPUID under a structured exception handler; every processor that can
    // run this port has it.)
    int regs[4];
    __cpuid(regs, 0);
    char vendor[13];
    std::memcpy(vendor + 0, &regs[1], 4);       // ebx
    std::memcpy(vendor + 4, &regs[3], 4);       // edx
    std::memcpy(vendor + 8, &regs[2], 4);       // ecx
    vendor[12] = 0;
    __cpuid(regs, 1);
    const uint32_t version = static_cast<uint32_t>(regs[0]);
    const uint32_t features = static_cast<uint32_t>(regs[3]);
    uint32_t extFeatures = 0;
    __cpuid(regs, 0x80000000);
    if (static_cast<uint32_t>(regs[0]) >= 0x80000000u) {
        __cpuid(regs, 0x80000001);
        extFeatures = static_cast<uint32_t>(regs[3]);
    }
    uint32_t flags = 0;
    if (features & 0x00800000)
        flags = kCpuMmx;
    if (extFeatures & 0x80000000)
        flags |= kCpu3DNow;
    if (features & 0x02000000)
        flags |= kCpuSse;
    if (features & 0x04000000)
        flags |= kCpuSse2;
    if (info) {
        std::memset(info, 0, sizeof *info);
        info->features = flags;
        info->model = (version >> 4) & 0xF;
        info->family = (version >> 8) & 0xF;
        info->stepping = version & 0xF;
        std::memcpy(info->vendor, vendor, sizeof vendor);
        std::strcpy(info->name, CpuName(info->family, info->model, info->vendor));
        info->valid = 0xF;
    }
    return flags;
}

uint32_t MeasureCpuMhz()
{
    DWORD t0 = platform::Milliseconds(), t1;
    do
        t1 = platform::Milliseconds();
    while (static_cast<int>(t1 - t0) < 2);
    int regs[4];
    __cpuid(regs, 0);                           // (serializes, as the original)
    const uint32_t c0 = static_cast<uint32_t>(__rdtsc());
    DWORD t2;
    do
        t2 = platform::Milliseconds();
    while (static_cast<int>(t2 - t1) < 1001);
    __cpuid(regs, 0);
    const uint32_t c1 = static_cast<uint32_t>(__rdtsc());
    return (c1 - c0) / 1000000;
}

}  // namespace ls3d
