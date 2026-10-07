#pragma once

#include "exec/fill_model.hpp"
#include "exec/market_view.hpp"

namespace exec {

// The surface a strategy sees each tick: read the market and account, and submit or
// cancel shadow orders. A read/write wrapper over the MarketView and the FillModel.
class StrategyContext {
public:
  StrategyContext(const MarketView &mv, FillModel &fills) noexcept
      : mv_(mv), fills_(fills) {}

  const MarketView &market() const noexcept { return mv_; }

  int64_t inventory() const noexcept { return fills_.inventory(); }
  double pnl() const noexcept { return fills_.pnl(); }

  // Orders rest at the current event-time; the model applies the latency budget.
  OrderId submit(common::Side side, uint32_t price, uint32_t qty) noexcept {
    return fills_.submit(side, price, qty, mv_.event_time_ns());
  }
  void cancel(OrderId id) noexcept { fills_.cancel(id); }
  bool is_live(OrderId id) const noexcept { return fills_.is_live(id); }

private:
  const MarketView &mv_;
  FillModel &fills_;
};

// Optional authoring base. The hot loop dispatches through the concept below, so a
// strategy need not inherit this.
struct IStrategy {
  virtual void on_tick(StrategyContext &ctx) noexcept = 0;
  virtual ~IStrategy() = default;
};

// Static-dispatch contract: any type with a noexcept on_tick(StrategyContext&).
template <class S>
concept Strategy = requires(S s, StrategyContext &ctx) {
  { s.on_tick(ctx) } noexcept;
};

} // namespace exec
