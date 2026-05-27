#pragma once

#include <cstdint>
#include <cstdio>

namespace common {

// 64-bucket log2 histogram of TSC cycle counts. One bucket per power of two:
// bucket b covers cycles in [2^b, 2^(b+1)). Bucket 0 also captures cycles == 0.
struct Histogram {
  uint64_t buckets[64]{};
  uint64_t count{0};
  uint64_t max_cycles{0};

  void record(uint64_t cycles) noexcept {
    ++count;
    if (cycles > max_cycles)
      max_cycles = cycles;
    int b = cycles == 0 ? 0 : 63 - __builtin_clzll(cycles);
    ++buckets[b];
  }

  // Upper-bound estimate (in cycles) of the bucket containing the p-th
  // percentile. p is in [0,1].
  uint64_t percentile_cycles(double p) const noexcept {
    if (count == 0)
      return 0;
    uint64_t threshold = static_cast<uint64_t>(p * static_cast<double>(count));
    uint64_t cum = 0;
    for (int b = 0; b < 64; ++b) {
      cum += buckets[b];
      if (cum > threshold)
        return b < 63 ? (uint64_t{1} << (b + 1)) : UINT64_MAX;
    }
    return max_cycles;
  }

  // One-liner periodic dump:
  // [lat] <label> p50=320ns p99=2.1us p999=18us max=44us n=100000
  void dump(std::FILE *out, const char *label,
            double tsc_per_ns) const noexcept {
    char p50[16], p99[16], p999[16], pmax[16];
    fmt_ns(p50, sizeof(p50), cycles_to_ns(percentile_cycles(0.50), tsc_per_ns));
    fmt_ns(p99, sizeof(p99), cycles_to_ns(percentile_cycles(0.99), tsc_per_ns));
    fmt_ns(p999, sizeof(p999),
           cycles_to_ns(percentile_cycles(0.999), tsc_per_ns));
    fmt_ns(pmax, sizeof(pmax), cycles_to_ns(max_cycles, tsc_per_ns));
    std::fprintf(out, "[lat] %s p50=%s p99=%s p999=%s max=%s n=%llu\n", label,
                 p50, p99, p999, pmax,
                 static_cast<unsigned long long>(count));
  }

  // Full per-bucket dump for shutdown artifacts.
  void dump_full(std::FILE *out, const char *label,
                 double tsc_per_ns) const noexcept {
    char pmax[16];
    fmt_ns(pmax, sizeof(pmax), cycles_to_ns(max_cycles, tsc_per_ns));
    std::fprintf(out, "[lat-full] %s n=%llu max=%s\n", label,
                 static_cast<unsigned long long>(count), pmax);
    uint64_t cum = 0;
    for (int b = 0; b < 64; ++b) {
      if (buckets[b] == 0)
        continue;
      cum += buckets[b];
      uint64_t lo = b == 0 ? 0 : (uint64_t{1} << b);
      uint64_t hi = b < 63 ? (uint64_t{1} << (b + 1)) : UINT64_MAX;
      char lo_s[16], hi_s[16];
      fmt_ns(lo_s, sizeof(lo_s), cycles_to_ns(lo, tsc_per_ns));
      fmt_ns(hi_s, sizeof(hi_s), cycles_to_ns(hi, tsc_per_ns));
      double pct =
          count ? 100.0 * static_cast<double>(cum) / static_cast<double>(count)
                : 0.0;
      std::fprintf(out, "  [%2d] %8s..%-8s n=%-10llu cum=%6.2f%%\n", b, lo_s,
                   hi_s, static_cast<unsigned long long>(buckets[b]), pct);
    }
  }

private:
  static double cycles_to_ns(uint64_t cycles, double tsc_per_ns) noexcept {
    return static_cast<double>(cycles) / tsc_per_ns;
  }

  static void fmt_ns(char *out, size_t n, double ns) noexcept {
    if (ns < 1000.0)
      std::snprintf(out, n, "%.0fns", ns);
    else if (ns < 1e6)
      std::snprintf(out, n, "%.1fus", ns / 1e3);
    else if (ns < 1e9)
      std::snprintf(out, n, "%.1fms", ns / 1e6);
    else
      std::snprintf(out, n, "%.2fs", ns / 1e9);
  }
};

} // namespace common
