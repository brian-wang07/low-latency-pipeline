#pragma once

#include "exec/exec_context.hpp"
#include "exec/risk.hpp"
#include <cstdio>

namespace exec {

// Writes the run summary as key=value lines (read by scripts/loopback_test.sh).
inline bool write_stats_log(const ExecContext &x, const char *path,
                            const char *strategy) noexcept {
  std::FILE *f = std::fopen(path, "w");
  if (!f) {
    std::perror("exec: strategy_stats.log");
    return false;
  }
  const stats::ExecStats &s = x.st;
  auto u = [&](const char *k, uint64_t v) {
    std::fprintf(f, "%s=%llu\n", k, (unsigned long long)v);
  };
  auto hist = [&](const char *name, const common::Histogram &h) {
    auto ns = [&](uint64_t cycles) {
      return (unsigned long long)(double(cycles) / x.tsc_per_ns);
    };
    std::fprintf(f, "%s_n=%llu\n%s_p50_ns=%llu\n%s_p99_ns=%llu\n%s_p999_ns=%llu\n"
                    "%s_max_ns=%llu\n",
                 name, (unsigned long long)h.count, name, ns(h.percentile_cycles(0.5)),
                 name, ns(h.percentile_cycles(0.99)), name,
                 ns(h.percentile_cycles(0.999)), name, ns(h.max_cycles));
    // Full log2 distribution: <name>_le_<upper bound ns>=count
    for (int b = 0; b < 64; ++b)
      if (h.buckets[b])
        std::fprintf(f, "%s_le_%lluns=%llu\n", name,
                     ns(b < 63 ? uint64_t{1} << (b + 1) : UINT64_MAX),
                     (unsigned long long)h.buckets[b]);
  };
  std::fprintf(f, "strategy=%s\n", strategy);
  u("exec_generation", s.exec_generation);
  u("frames", s.frames);
  u("frame_gaps", x.view.frame_gaps());
  u("orders_new", s.orders_new);
  u("orders_replace", s.orders_replace);
  u("orders_cancel", s.orders_cancel);
  u("acks", s.acks);
  u("replaced", s.replaced);
  u("canceled", s.canceled);
  u("expired", s.expired);
  u("fills", s.fills);
  u("venue_rejects", s.venue_rejects);
  u("unsolicited", s.unsolicited);
  u("risk_rejects", s.risk_rejects);
  u("ring_full_rejects", s.ring_full_rejects);
  for (int r = 1; r < int(RejectReason::Count); ++r)
    if (s.rejects[r])
      std::fprintf(f, "reject.%s=%llu\n", reject_name(RejectReason(r)),
                   (unsigned long long)s.rejects[r]);
  u("open_orders_at_exit", x.orders.size());
  std::fprintf(f, "equity=%.8f\n", double(x.account.equity()) / 1e8);
  hist("tick_to_order", s.tick_to_order);
  hist("tick", s.tick);
  hist("feed_transit", s.feed_transit);
  std::fclose(f);
  return true;
}

} // namespace exec
