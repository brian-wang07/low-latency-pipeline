#pragma once

#include "common/ipc/instrument.hpp"
#include <cstdint>

namespace exec {

enum class RateOp : uint8_t { Add, Amend, Cancel };

// Kraken's per-pair rate counter: every add/amend/cancel adds a cost, the counter
// decays continuously, and a request that would push it past the tier's max is
// rejected by the venue. Costs follow plan.md ("Verified venue facts"); the amend
// penalty is read as +1 plus the age term, the conservative reading (re-check
// against Kraken's docs before Phase 5). Shared vectors:
// tests/fixtures/kraken_rate_vectors.csv.
struct KrakenRateParams {
  double max = 60;         // Starter 60, Intermediate 125, Pro 180
  double decay_per_s = 1;  // Starter 1, Intermediate 2.34, Pro 3.75
  uint32_t max_open = 60;  // open orders per pair: 60 / 80 / 225
  double margin = 5;       // headroom kept below max
};

inline constexpr int64_t SEC_NS = 1'000'000'000;

inline int kraken_rate_cost(RateOp op, int64_t age_ns) noexcept {
  switch (op) {
  case RateOp::Add:
    return 1;
  case RateOp::Amend:
    return 1 + (age_ns < 5 * SEC_NS    ? 3
                : age_ns < 10 * SEC_NS ? 2
                : age_ns < 15 * SEC_NS ? 1
                                       : 0);
  case RateOp::Cancel:
    return age_ns < 5 * SEC_NS     ? 8
           : age_ns < 10 * SEC_NS  ? 6
           : age_ns < 15 * SEC_NS  ? 5
           : age_ns < 45 * SEC_NS  ? 4
           : age_ns < 90 * SEC_NS  ? 2
           : age_ns < 300 * SEC_NS ? 1
                                   : 0;
  }
  return 0;
}

// One counter per instrument, on the exec clock (event time in ITCH replay).
class RateModel {
public:
  void configure(bool enabled, const KrakenRateParams &p) noexcept {
    enabled_ = enabled;
    p_ = p;
  }
  bool enabled() const noexcept { return enabled_; }
  const KrakenRateParams &params() const noexcept { return p_; }

  double count(uint16_t inst, int64_t now_ns) const noexcept {
    const Counter &c = c_[inst];
    if (now_ns <= c.last_ns)
      return c.count;
    const double v = c.count - p_.decay_per_s * double(now_ns - c.last_ns) / 1e9;
    return v > 0 ? v : 0;
  }

  // Headroom left under max - margin.
  double budget(uint16_t inst, int64_t now_ns) const noexcept {
    return enabled_ ? p_.max - p_.margin - count(inst, now_ns) : 1e18;
  }

  bool would_allow(uint16_t inst, RateOp op, int64_t age_ns,
                   int64_t now_ns) const noexcept {
    return !enabled_ ||
           count(inst, now_ns) + kraken_rate_cost(op, age_ns) <= p_.max - p_.margin;
  }

  void charge(uint16_t inst, RateOp op, int64_t age_ns, int64_t now_ns) noexcept {
    if (!enabled_)
      return;
    Counter &c = c_[inst];
    c.count = count(inst, now_ns) + kraken_rate_cost(op, age_ns);
    if (now_ns > c.last_ns)
      c.last_ns = now_ns;
  }

private:
  struct Counter {
    double count = 0;
    int64_t last_ns = 0;
  };
  bool enabled_ = false;
  KrakenRateParams p_{};
  Counter c_[ref::MAX_INSTRUMENTS]{};
};

} // namespace exec
