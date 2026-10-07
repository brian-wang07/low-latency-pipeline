#pragma once

#include "common/ipc/shm.hpp"
#include <cmath>
#include <cstdint>

namespace exec {

// Incrementally-updated market view, fed one MarketUpdate per event off the feed.
// Derives the standard MM inputs without keeping an order book. Prices and
// price-derived values are in the instrument's ticks, sizes in its lots.
class MarketView {
public:
  using Update = MarketUpdate<EXEC_DEPTH>;

  void on_update(const Update &u) noexcept {
    // event_seq is producer-monotonic: a jump means the producer dropped frames,
    // and a step back means it restarted.
    const uint64_t expected = u_.event_seq + 1;
    bool reset = (u.flags & FEED_RESET) != 0;
    if (have_seq_ && u.event_seq != expected) {
      if (u.event_seq > expected)
        frame_gaps_ += u.event_seq - expected;
      reset = true;
    }
    feed_reset_ = reset;
    have_seq_ = true;
    if (reset)
      have_mid_ = false; // don't fold the jump across the gap into vol

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

  int64_t best_bid() const noexcept { return u_.best_bid; }
  int64_t best_ask() const noexcept { return u_.best_ask; }
  int64_t bid_size() const noexcept { return u_.nb > 0 ? u_.bids[0].qty : 0; }
  int64_t ask_size() const noexcept { return u_.na > 0 ? u_.asks[0].qty : 0; }

  // Derived values below are valid only when two_sided().
  double mid() const noexcept {
    return 0.5 * (double(u_.best_bid) + double(u_.best_ask));
  }
  // Size-weighted mid: each price weighted by the opposite side's size, so it
  // leans toward the thinner side.
  double microprice() const noexcept {
    const double bq = double(u_.bids[0].qty), aq = double(u_.asks[0].qty);
    const double q = bq + aq;
    return q > 0.0 ? (double(u_.best_bid) * aq + double(u_.best_ask) * bq) / q
                   : mid();
  }
  // Depth imbalance over the captured levels, in [-1, 1].
  double imbalance() const noexcept {
    int64_t bq = 0, aq = 0;
    for (int i = 0; i < u_.nb; ++i)
      bq += u_.bids[i].qty;
    for (int i = 0; i < u_.na; ++i)
      aq += u_.asks[i].qty;
    const double tot = double(bq) + double(aq);
    return tot > 0.0 ? (double(bq) - double(aq)) / tot : 0.0;
  }
  // EWMA stdev of per-update mid changes, in ticks.
  double realized_vol() const noexcept { return std::sqrt(var_); }

  int bid_depth() const noexcept { return u_.nb; }
  int ask_depth() const noexcept { return u_.na; }
  const Level &bid_level(int i) const noexcept { return u_.bids[i]; }
  const Level &ask_level(int i) const noexcept { return u_.asks[i]; }

  bool last_was_trade() const noexcept { return u_.trade_qty != 0; }
  int64_t trade_price() const noexcept { return u_.trade_price; }
  int64_t trade_qty() const noexcept { return u_.trade_qty; }
  common::Side trade_side() const noexcept { return u_.trade_side; }

  uint64_t event_seq() const noexcept { return u_.event_seq; }
  uint64_t event_time_ns() const noexcept { return u_.event_time; }
  uint16_t instrument() const noexcept { return u_.instrument; }
  uint8_t flags() const noexcept { return u_.flags; }

  // True when the view was interrupted just before this frame (frames dropped or
  // the producer resynced): anything derived from earlier frames is suspect.
  bool feed_reset() const noexcept { return feed_reset_; }
  // Frames lost to producer drops since start.
  uint64_t frame_gaps() const noexcept { return frame_gaps_; }

private:
  static constexpr double VOL_ALPHA = 0.01; // EWMA weight for per-update mid vol

  Update u_{};
  double prev_mid_ = 0.0;
  double var_ = 0.0;
  bool have_mid_ = false;
  bool have_seq_ = false;
  bool feed_reset_ = false;
  uint64_t frame_gaps_ = 0;
};

} // namespace exec
