#pragma once

#include "common/ipc/shm.hpp"
#include <cstdint>

namespace exec {

using OrderId = uint32_t;
inline constexpr OrderId INVALID_ORDER = 0;

// Shadow-fill model. Holds the strategy's non-displayed orders and fills them from the
// observed trade flow by price-time priority, behind a latency gate; tracks inventory
// and cash for PnL. Orders never enter any book and never perturb the feed (see
// plan.md "Why shadow fills"). Passive quoting only -- marketable shadow orders are
// not modelled.
class FillModel {
public:
  explicit FillModel(uint64_t latency_budget_ns) noexcept
      : latency_ns_(latency_budget_ns) {}

  // Register a shadow order resting at `price` (feed units). now_ns is the event-time
  // of the update the strategy is reacting to. Returns INVALID_ORDER if full.
  OrderId submit(common::Side side, uint32_t price, uint32_t qty,
                 uint64_t now_ns) noexcept {
    if (qty == 0)
      return INVALID_ORDER;
    for (Order &o : orders_) {
      if (o.active)
        continue;
      o = {next_id_, side, price, qty, now_ns + latency_ns_, QUEUE_UNSET, true};
      return next_id_++;
    }
    return INVALID_ORDER; // registry full
  }

  void cancel(OrderId id) noexcept {
    for (Order &o : orders_)
      if (o.active && o.id == id) {
        o.active = false;
        return;
      }
  }

  bool is_live(OrderId id) const noexcept {
    for (const Order &o : orders_)
      if (o.active && o.id == id)
        return true;
    return false;
  }

  // Apply one feed update: refresh the mark, and fill resting orders against its trade
  // (if any). Call before the strategy's on_tick so it sees the fills.
  void on_update(const MarketUpdate<EXEC_DEPTH> &u) noexcept {
    if (u.nb > 0 && u.na > 0)
      last_mid_ = 0.5 * (double(u.best_bid) + double(u.best_ask));
    for (Order &o : orders_) {
      if (!o.active || u.event_time < o.t_live)
        continue; // inactive, or not live yet (latency gate)
      if (o.queue == QUEUE_UNSET) {
        // Go-live: our place in line is the size resting at our price right now. Don't
        // also compete for this update's trade -- we arrive just after it.
        o.queue = level_size(u, o.side, o.price);
        continue;
      }
      if (u.trade_size != 0)
        fill(o, u);
    }
  }

  int64_t inventory() const noexcept { return inventory_; }
  double cash() const noexcept { return cash_; }
  double pnl() const noexcept { return cash_ + double(inventory_) * last_mid_; }
  uint64_t fill_count() const noexcept { return fill_count_; }
  uint64_t filled_shares() const noexcept { return filled_shares_; }

private:
  static constexpr int MAX_ORDERS = 64;
  static constexpr uint64_t QUEUE_UNSET = UINT64_MAX;

  struct Order {
    OrderId id;
    common::Side side;
    uint32_t price;
    uint32_t qty;    // remaining
    uint64_t t_live; // event-time when eligible to fill (post + latency)
    uint64_t queue;  // shares resting ahead of us; QUEUE_UNSET until go-live
    bool active;
  };

  // Aggregate size resting at `price` on `side`, from the depth ladder; 0 if the price
  // isn't a visible level (e.g. an inside-spread improvement -> no queue ahead of us).
  static uint32_t level_size(const MarketUpdate<EXEC_DEPTH> &u, common::Side side,
                             uint32_t price) noexcept {
    if (side == common::Side::Buy) {
      for (int i = 0; i < u.nb; ++i)
        if (u.bids[i].price == price)
          return u.bids[i].shares;
    } else {
      for (int i = 0; i < u.na; ++i)
        if (u.asks[i].price == price)
          return u.asks[i].shares;
    }
    return 0;
  }

  void fill(Order &o, const MarketUpdate<EXEC_DEPTH> &u) noexcept {
    if (u.trade_side != o.side)
      return; // flow must hit our side (a bid is hit when an aggressor sells)

    const uint32_t P = u.trade_price;
    const uint32_t V = u.trade_size;
    const bool buy = (o.side == common::Side::Buy);

    // A bid is reached by trades at or below its price; an ask at or above. A trade at
    // a better price was absorbed ahead of us and never reached our level.
    if (buy ? (P > o.price) : (P < o.price))
      return;

    uint32_t f;
    if (buy ? (P < o.price) : (P > o.price)) {
      o.queue = 0; // the sweep traded through us: our level was passed
      f = o.qty;
    } else {
      // Trade at our level: pay down the queue ahead, then take the excess.
      const uint64_t paid = o.queue < V ? o.queue : V;
      o.queue -= paid;
      const uint64_t avail = V - paid;
      f = avail < o.qty ? uint32_t(avail) : o.qty;
    }

    if (f == 0)
      return;
    const int64_t signed_f = buy ? int64_t(f) : -int64_t(f);
    inventory_ += signed_f;                       // long on bid fills, short on asks
    cash_ -= double(signed_f) * double(o.price);  // pay to buy, receive to sell
    o.qty -= f;
    ++fill_count_;
    filled_shares_ += f;
    if (o.qty == 0)
      o.active = false;
  }

  uint64_t latency_ns_;
  Order orders_[MAX_ORDERS]{};
  int64_t inventory_ = 0;
  double cash_ = 0.0;
  double last_mid_ = 0.0;
  uint64_t fill_count_ = 0;
  uint64_t filled_shares_ = 0;
  OrderId next_id_ = 1; // 0 is reserved as INVALID_ORDER
};

} // namespace exec
