#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#elif !defined(__aarch64__) && !defined(_M_ARM64)
#error "Unsupported architecture: no TSC implementation"
#endif

inline uint64_t read_tsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  _mm_lfence();
  uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#elif defined(__aarch64__) || defined(_M_ARM64)
  uint64_t val;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#endif
}

inline void calibrate_tsc(double &tsc_per_ns) {
  struct timespec a, b;
  uint64_t tsc_a = read_tsc();
  clock_gettime(CLOCK_MONOTONIC, &a);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  clock_gettime(CLOCK_MONOTONIC, &b);
  uint64_t tsc_b = read_tsc();
  uint64_t elapsed_ns =
      (b.tv_sec - a.tv_sec) * 1'000'000'000ULL + (b.tv_nsec - a.tv_nsec);
  tsc_per_ns = static_cast<double>(tsc_b - tsc_a) / elapsed_ns;
}
