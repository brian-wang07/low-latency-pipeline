#pragma once

#include "exec/market_view.hpp"
#include <cstdint>

namespace exec {

using OrderId = uint32_t;
inline constexpr OrderId INVALID_ORDER = 0;

// The surface a strategy sees each tick: read the market and account, and submit or
// cancel orders. Until the order router lands (plan Phase 2) orders are only tracked
// here: they never leave the process and never fill.
class StrategyContext {
public:
  explicit StrategyContext(const MarketView &mv) noexcept : mv_(mv) {}

  const MarketView &market() const noexcept { return mv_; }

  int64_t inventory() const noexcept { return 0; }
  double pnl() const noexcept { return 0.0; }

  // Price in ticks, qty in lots. Returns INVALID_ORDER for qty <= 0 or when all
  // slots are live.
  OrderId submit(common::Side side, int64_t price, int64_t qty) noexcept {
    (void)side;
    (void)price;
    if (qty <= 0)
      return INVALID_ORDER;
    for (OrderId &slot : live_)
      if (slot == INVALID_ORDER) {
        slot = next_id_++;
        ++orders_submitted_;
        return slot;
      }
    return INVALID_ORDER;
  }
  void cancel(OrderId id) noexcept {
    if (id == INVALID_ORDER)
      return;
    for (OrderId &slot : live_)
      if (slot == id) {
        slot = INVALID_ORDER;
        return;
      }
  }
  bool is_live(OrderId id) const noexcept {
    if (id == INVALID_ORDER)
      return false;
    for (OrderId slot : live_)
      if (slot == id)
        return true;
    return false;
  }

  uint64_t orders_submitted() const noexcept { return orders_submitted_; }

private:
  static constexpr int MAX_ORDERS = 64;

  const MarketView &mv_;
  OrderId live_[MAX_ORDERS]{};
  OrderId next_id_ = 1;
  uint64_t orders_submitted_ = 0;
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
