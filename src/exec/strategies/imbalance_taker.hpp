#pragma once

#include "exec/strategy.hpp"
#include <cstdint>

namespace exec {

// Takes liquidity when the visible depth is strongly one-sided: an IOC buy at the
// ask when bids dominate, an IOC sell at the bid when asks dominate. A side
// re-arms only after imbalance falls back inside RESET (hysteresis); a cooldown on
// the exec clock, a position cap and a rate-budget floor bound its activity.
// Exercises the taker path (IOC -> ack -> expired in null mode).
class ImbalanceTaker {
public:
  template <class Ctx> void on_tick(Ctx &ctx) noexcept {
    const MarketView &m = ctx.market();
    if (!m.two_sided())
      return;
    const double imb = m.imbalance();
    if (imb < RESET)
      buy_armed_ = true;
    if (imb > -RESET)
      sell_armed_ = true;

    const int64_t now = ctx.now_ns();
    if (now - last_ns_ < COOLDOWN_NS ||
        ctx.rate_budget(m.instrument()) < MIN_BUDGET)
      return;
    const int64_t inv = ctx.inventory();
    if (imb > ENTER && buy_armed_ && inv + LOT <= POS_CAP) {
      ctx.submit(common::Side::Buy, m.best_ask(), LOT, oe::Tif::IOC);
      buy_armed_ = false;
      last_ns_ = now;
    } else if (imb < -ENTER && sell_armed_ && inv - LOT >= -POS_CAP) {
      ctx.submit(common::Side::Sell, m.best_bid(), LOT, oe::Tif::IOC);
      sell_armed_ = false;
      last_ns_ = now;
    }
  }

private:
  static constexpr double ENTER = 0.6;
  static constexpr double RESET = 0.2;
  static constexpr int64_t LOT = 100;
  static constexpr int64_t POS_CAP = 500;
  static constexpr int64_t COOLDOWN_NS = 1'000'000'000;
  static constexpr double MIN_BUDGET = 10;

  bool buy_armed_ = true;
  bool sell_armed_ = true;
  int64_t last_ns_ = INT64_MIN / 2;
};

} // namespace exec
