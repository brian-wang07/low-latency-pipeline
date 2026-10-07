#pragma once

#include "exec/strategy.hpp"
#include <cstdint>
#include <cstdio>

namespace exec {

// post after a fill, and stops quoting a side once inventory hits a cap.
// Exercises the full submit -> fill -> inventory/PnL loop, and logs whenever a
// fill moves the position.
class DummyQuoter {
public:
  void on_tick(StrategyContext &ctx) noexcept {
    const MarketView &m = ctx.market();
    ++ticks_;
    if (!m.two_sided())
      return;

    const int64_t inv = ctx.inventory();
    if (inv < POS_CAP && !ctx.is_live(bid_))
      bid_ = ctx.submit(common::Side::Buy, m.best_bid(), LOT);
    if (inv > -POS_CAP && !ctx.is_live(ask_))
      ask_ = ctx.submit(common::Side::Sell, m.best_ask(), LOT);

    if (inv != last_inv_ || (ticks_ & HEARTBEAT_MASK) == 0) {
      std::fprintf(stderr, "[exec] ticks=%llu inv=%lld pnl=$%.2f\n",
                   (unsigned long long)ticks_, (long long)inv,
                   ctx.pnl() / 10000.0);
      last_inv_ = inv;
    }
  }

private:
  static constexpr int64_t LOT = 100;
  static constexpr int64_t POS_CAP = 1000;
  static constexpr uint64_t HEARTBEAT_MASK = (1ull << 20) - 1;

  OrderId bid_ = INVALID_ORDER;
  OrderId ask_ = INVALID_ORDER;
  uint64_t ticks_ = 0;
  int64_t last_inv_ = 0;
};

} // namespace exec
