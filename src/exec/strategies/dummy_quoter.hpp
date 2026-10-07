#pragma once

#include "exec/strategy.hpp"
#include <cstdint>
#include <cstdio>

namespace exec {

// Quotes one lot of LOT at the touch on each side and amends a resting quote to
// follow the touch, stopping a side once inventory hits the cap. Exercises submit,
// amend, cancel, the risk gate and the rate model; logs a heartbeat line.
class DummyQuoter {
public:
  template <class Ctx> void on_tick(Ctx &ctx) noexcept {
    const MarketView &m = ctx.market();
    ++ticks_;
    if (!m.two_sided())
      return;

    const int64_t inv = ctx.inventory();
    quote(ctx, bid_, common::Side::Buy, m.best_bid(), inv < POS_CAP);
    quote(ctx, ask_, common::Side::Sell, m.best_ask(), inv > -POS_CAP);

    if ((ticks_ & HEARTBEAT_MASK) == 0)
      std::fprintf(stderr, "[exec] ticks=%llu inv=%lld pnl=$%.2f\n",
                   (unsigned long long)ticks_, (long long)inv, ctx.pnl());
  }

private:
  static constexpr int64_t LOT = 100;
  static constexpr int64_t POS_CAP = 1000;
  static constexpr uint64_t HEARTBEAT_MASK = (1ull << 20) - 1;

  template <class Ctx>
  void quote(Ctx &ctx, OrderId &id, common::Side side, int64_t px,
             bool want) noexcept {
    const OrderRow *o = ctx.order(id);
    if (!want) {
      if (o)
        ctx.cancel(id);
      return;
    }
    if (!o)
      id = ctx.submit(side, px, LOT);
    else if (o->px != px && !o->pending)
      ctx.replace(id, px, o->qty);
  }

  OrderId bid_ = INVALID_ORDER;
  OrderId ask_ = INVALID_ORDER;
  uint64_t ticks_ = 0;
};

} // namespace exec
