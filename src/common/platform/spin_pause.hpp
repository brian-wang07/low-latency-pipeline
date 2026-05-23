#pragma once

#if defined (__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define SPIN_PAUSE() _mm_pause();
#elif defined (__aarch64__) || defined (_M_ARM64) || defined (__arm__) || defined(_M_ARM)
    #define SPIN_PAUSE() __asm__ volatile("yield")
#else
    #define SPIN_PAUSE() ((void)0)
#endif