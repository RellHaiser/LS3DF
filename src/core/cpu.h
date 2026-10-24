// Processor identification for the IGraph start-up log (0x10068620 - 0x10068c90).
#pragma once

#include <cstdint>

namespace ls3d {

// Feature bits returned by DetectCpu.
enum : uint32_t {
    kCpuMmx = 0x1,
    kCpuSse = 0x2,
    kCpuSse2 = 0x4,
    kCpu3DNow = 0x8,
};

struct S_cpuInfo {              // 0x40 bytes
    char vendor[13];            // +0x00 CPUID vendor string ("GenuineIntel")
    char name[31];              // +0x0D model name ("INTEL Pentium-III"), "" when not known
    int family;                 // +0x2C
    int model;                  // +0x30
    int stepping;               // +0x34
    uint32_t features;          // +0x38 kCpu* bits
    uint32_t valid;             // +0x3C 0x0F once filled
};
static_assert(sizeof(S_cpuInfo) == 0x40, "S_cpuInfo layout");

// Fills `info` (if given) and returns the feature bits; 0 without CPUID (0x10068b00).
uint32_t DetectCpu(S_cpuInfo *info);
// Clock rate in MHz, measured with the time-stamp counter over one second (0x10068c10).
uint32_t MeasureCpuMhz();

}  // namespace ls3d
