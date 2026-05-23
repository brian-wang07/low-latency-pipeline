#pragma once

#if defined (__x86_64__) || defined (_M_X64) || defined (__i386__) || defined (_M_IX86)
    #ifdef _MSC_VER
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
    #define READ_TSC() __rdtsc()
#elif defined (__aarch64__) || defined (_M_ARM64)
    static inline uint64_t read_cntvct() {
        uint64_t val;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
    }
    #define READ_TSC() read_cntvct()