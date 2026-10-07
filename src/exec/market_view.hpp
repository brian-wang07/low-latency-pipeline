#pragma once

#include "common/ipc/shm.hpp"
#include <cmath>
#include <cstdint>

namespace exec {

// Incrementally-updated market view, fed one MarketUpdate per event off the core
// feed. Derives the standard MM inputs without keeping an order book. Prices and
// price-derived values are in 4-decimal fixed point (x10000), like the feed.
class MarketView {
public:
  using Update = MarketUpdate<EXEC_DEPTH>;

  void on_update(const Update &u) noexcept {
    u_ = u;
    if (two_sided()) {
      const double m = mid();
      if (have_mid_) {
        const double dm = m - prev_mid_;
        var_ = (1.0 - VOL_ALPHA) * var_ + VOL_ALPHA * dm * dm;
      }
      prev_mid_ = m;
      have_mid_ = true;
    }
  }

  bool two_sided() const noexcept { return u_.nb > 0 && u_.na > 0; }

  uint32_t best_bid() const noexcept { return u_.best_bid; }
  uint32_t best_ask() const noexcept { return u_.best_ask; }
  uint32_t bid_size() const noexcept { return u_.nb > 0 ? u_.bids[0].shares : 0u; }
  uint32_t ask_size() const noexcept { return u_.na > 0 ? u_.asks[0].shares : 0u; }

  // Derived values below are valid only when two_sided().
  double mid() const noexcept {
    return 0.5 * (double(u_.best_bid) + double(u_.best_ask));
  }
  // Size-weighted mid: each price weighted by the opposite side's size, so it
  // leans toward the thinner side.
  double microprice() const noexcept {
    const double bq = u_.bids[0].shares, aq = u_.asks[0].shares;
    const double q = bq + aq;
    return q > 0.0 ? (double(u_.best_bid) * aq + double(u_.best_ask) * bq) / q
                   : mid();
  }
  // Depth imbalance over the captured levels, in [-1, 1].
  double imbalance() const noexcept {
    uint64_t bq = 0, aq = 0;
    for (int i = 0; i < u_.nb; ++i)
      bq += u_.bids[i].shares;
    for (int i = 0; i < u_.na; ++i)
      aq += u_.asks[i].shares;
    const double tot = double(bq) + double(aq);
    return tot > 0.0 ? (double(bq) - double(aq)) / tot : 0.0;
  }
  // EWMA stdev of per-update mid changes (fixed-point units).
  double realized_vol() const noexcept { return std::sqrt(var_); }

  int bid_depth() const noexcept { return u_.nb; }
  int ask_depth() const noexcept { return u_.na; }
  const Level &bid_level(int i) const noexcept { return u_.bids[i]; }
  const Level &ask_level(int i) const noexcept { return u_.asks[i]; }

  bool last_was_trade() const noexcept { return u_.trade_size != 0; }
  uint32_t trade_price() const noexcept { return u_.trade_price; }
  uint32_t trade_size() const noexcept { return u_.trade_size; }
  common::Side trade_side() const noexcept { return u_.trade_side; }

  uint64_t event_seq() const noexcept { return u_.event_seq; }
  uint64_t event_time_ns() const noexcept { return u_.event_time; }

private:
  static constexpr double VOL_ALPHA = 0.01; // EWMA weight for per-update mid vol

  Update u_{};
  double prev_mid_ = 0.0;
  double var_ = 0.0;
  bool have_mid_ = false;
};

} // namespace exec
