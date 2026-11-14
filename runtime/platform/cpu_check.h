// x86-64-v3 check for the generated game code. Use CPUID directly so it
// needs no compiler runtime (__builtin_cpu_supports pulls in libgcc's __cpu_model, which zig cc doesn't have).
#pragma once
#include <stdbool.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>

static inline bool cpu_has_x86_64_v3(void) {
    unsigned a, b, c, d;
    // The architecture levels are cumulative. The 64-bit runtime already
    // assumes the x86-64 baseline, so check the v2 and v3 additions.
    const unsigned leaf1_ecx = bit_SSE3 | bit_SSSE3 | bit_CMPXCHG16B | bit_SSE4_1 | bit_SSE4_2 |
                               bit_POPCNT | bit_FMA | bit_MOVBE | bit_XSAVE | bit_OSXSAVE | bit_AVX | bit_F16C;
    const unsigned leaf7_ebx = bit_BMI | bit_AVX2 | bit_BMI2;
    const unsigned extended_ecx = bit_LAHF_LM | bit_LZCNT;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    if ((c & leaf1_ecx) != leaf1_ecx) return false;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d) || (b & leaf7_ebx) != leaf7_ebx) return false;
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d) || (c & extended_ecx) != extended_ecx) return false;

    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;  // The OS saves both the XMM and YMM state.
}
#endif
