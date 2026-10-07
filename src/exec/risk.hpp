#pragma once

#include "common/config_file.hpp"
#include "common/ipc/control.hpp"
#include "common/ipc/instrument.hpp"
#include "common/ipc/order_msgs.hpp"
#include "exec/account.hpp"
#include "exec/order_table.hpp"
#include "exec/rate_model.hpp"
#include <cstdint>
#include <cstring>

namespace exec {

enum class RejectReason : uint8_t {
  None = 0,
  Kill,
  Pause,
  NotTraded,        // instrument not registered with the account
  NotAllowed,       // base asset not in risk.allowed_assets
  InstrumentStatus, // halted, cancel-only, post-only or limit-only conflict
  BadOrder,         // qty <= 0, or a limit without a price
  MinQty,
  MinNotional,
  MaxOrderQty,
  MaxOrderNotional,
  MaxPosition,
  MaxPositionNotional,
  MaxOpenOrders,
  PriceBand,
  InsufficientBase,
  InsufficientQuote,
  RateLimit,
  OpenOrderLimit, // venue's per-pair open order limit
  UnknownOrder,
  TableFull,
  RingFull,
  Count
};
static_assert(int(RejectReason::Count) <= 32, "ExecStats.rejects has 32 slots");

inline const char *reject_name(RejectReason r) noexcept {
  static const char *const names[] = {
      "none",          "kill",          "pause",
      "not_traded",    "not_allowed",   "instrument_status",
      "bad_order",     "min_qty",       "min_notional",
      "max_order_qty", "max_order_notional", "max_position",
      "max_position_notional", "max_open_orders", "price_band",
      "insufficient_base", "insufficient_quote", "rate_limit",
      "open_order_limit", "unknown_order", "table_full",
      "ring_full"};
  static_assert(sizeof(names) / sizeof(names[0]) == size_t(RejectReason::Count));
  return names[size_t(r)];
}

// Limits in lots and 1e-8 quote units; 0 disables a check.
struct RiskLimits {
  int64_t max_order_qty = 0;
  Money max_order_notional = 0;
  int64_t max_position = 0;
  Money max_position_notional = 0;
  uint32_t max_open_orders = 0;
  double price_band_bp = 0;
  bool spot_only = true;
};

struct OrderCheck {
  uint16_t instrument;
  common::Side side;
  oe::OrdType ord_type;
  oe::Tif tif;
  int64_t px, qty;   // ticks, lots; px 0 for a market order
  int64_t mid2;      // bid + ask in ticks, 0 when the book isn't two-sided
  int64_t now_ns;
};

// Pre-trade checks for exec's own orders. Called on the hot thread before an
// order is recorded or routed; cheap, branchy, no allocation.
class RiskGate {
public:
  void configure(const common::Config &cfg) noexcept {
    auto money = [&](const char *k) {
      return Money(cfg.get_f64(k, 0) * 1e8);
    };
    lim_.max_order_qty = cfg.get_i64("risk.max_order_qty", 0);
    lim_.max_order_notional = money("risk.max_order_notional");
    lim_.max_position = cfg.get_i64("risk.max_position", 0);
    lim_.max_position_notional = money("risk.max_position_notional");
    lim_.max_open_orders = uint32_t(cfg.get_i64("risk.max_open_orders", 0));
    lim_.price_band_bp = cfg.get_f64("risk.price_band_bp", 0);
    lim_.spot_only = cfg.get_bool("risk.spot_only", true);
    set_allowed_assets(cfg.get_str("risk.allowed_assets", "BTC,ETH"));
  }
  void set_limits(const RiskLimits &l) noexcept { lim_ = l; }
  const RiskLimits &limits() const noexcept { return lim_; }

  // Comma-separated base assets that may be traded.
  void set_allowed_assets(const char *list) noexcept {
    std::strncpy(allowed_, list, sizeof(allowed_) - 1);
    allowed_[sizeof(allowed_) - 1] = '\0';
  }
  // Resolves the allowlist for an instrument once, off the hot path; call again
  // after set_allowed_assets.
  void register_instrument(uint16_t inst, const ref::Instrument &row) noexcept {
    char base[8], quote[8];
    split_symbol(row.symbol, base, quote);
    allowed_inst_[inst] = asset_allowed(base);
  }

  bool asset_allowed(const char *asset) const noexcept {
    const std::size_t n = std::strlen(asset);
    for (const char *p = allowed_; *p;) {
      const char *comma = std::strchr(p, ',');
      const std::size_t len = comma ? std::size_t(comma - p) : std::strlen(p);
      if (len == n && std::strncmp(p, asset, n) == 0)
        return true;
      if (!comma)
        break;
      p = comma + 1;
    }
    return false;
  }

  RejectReason check_new(const OrderCheck &o, const ref::Instrument &inst,
                         const Account &acct, const OrderTable &orders,
                         const RateModel &rate, uint32_t control) const noexcept {
    if (control & ipc::KILL)
      return RejectReason::Kill;
    if (control & ipc::PAUSE)
      return RejectReason::Pause;
    if (!acct.trades(o.instrument))
      return RejectReason::NotTraded;
    if (!allowed_inst_[o.instrument])
      return RejectReason::NotAllowed;
    if (RejectReason r = status_check(o, inst); r != RejectReason::None)
      return r;
    if (o.qty <= 0 || (o.ord_type == oe::OrdType::Limit && o.px <= 0))
      return RejectReason::BadOrder;
    const int64_t ref_px = o.px > 0 ? o.px : o.mid2 / 2;
    if (inst.min_qty_lots > 0 && o.qty < inst.min_qty_lots)
      return RejectReason::MinQty;
    const Money notional = acct.notional(o.instrument, ref_px, o.qty);
    if (inst.min_notional > 0 && notional < inst.min_notional)
      return RejectReason::MinNotional;
    if (lim_.max_order_qty && o.qty > lim_.max_order_qty)
      return RejectReason::MaxOrderQty;
    if (lim_.max_order_notional && notional > lim_.max_order_notional)
      return RejectReason::MaxOrderNotional;
    if (RejectReason r = position_check(o.instrument, o.side, o.qty, ref_px,
                                        acct, orders);
        r != RejectReason::None)
      return r;
    if (lim_.max_open_orders && orders.size() >= lim_.max_open_orders)
      return RejectReason::MaxOpenOrders;
    if (rate.enabled() && orders.live_count(o.instrument) >= rate.params().max_open)
      return RejectReason::OpenOrderLimit;
    if (RejectReason r = band_check(o.px, o.mid2); r != RejectReason::None)
      return r;
    if (lim_.spot_only) {
      const Money need = acct.reservation(o.instrument, o.side, ref_px, o.qty);
      if (o.side == common::Side::Buy
              ? need > acct.quote_balance(o.instrument).available()
              : need > acct.base_balance(o.instrument).available())
        return o.side == common::Side::Buy ? RejectReason::InsufficientQuote
                                           : RejectReason::InsufficientBase;
    }
    if (!rate.would_allow(o.instrument, RateOp::Add, 0, o.now_ns))
      return RejectReason::RateLimit;
    return RejectReason::None;
  }

  // An amend of a live order to px/qty (qty is the new total, leaves follow).
  RejectReason check_replace(const OrderRow &row, int64_t px, int64_t qty,
                             int64_t mid2, int64_t now_ns,
                             const ref::Instrument &inst, const Account &acct,
                             const OrderTable &orders, const RateModel &rate,
                             uint32_t control) const noexcept {
    if (control & ipc::KILL)
      return RejectReason::Kill;
    if (control & ipc::PAUSE)
      return RejectReason::Pause;
    if (inst.status == ref::HALTED || inst.status == ref::CANCEL_ONLY)
      return RejectReason::InstrumentStatus;
    if (qty <= row.cum || px <= 0)
      return RejectReason::BadOrder;
    if (inst.min_qty_lots > 0 && qty < inst.min_qty_lots)
      return RejectReason::MinQty;
    const Money notional = acct.notional(row.instrument, px, qty);
    if (lim_.max_order_qty && qty > lim_.max_order_qty)
      return RejectReason::MaxOrderQty;
    if (lim_.max_order_notional && notional > lim_.max_order_notional)
      return RejectReason::MaxOrderNotional;
    const int64_t new_leaves = qty - row.cum;
    if (new_leaves > row.leaves) {
      if (RejectReason r = position_check(row.instrument, row.side,
                                          new_leaves - row.leaves, px, acct,
                                          orders);
          r != RejectReason::None)
        return r;
    }
    if (RejectReason r = band_check(px, mid2); r != RejectReason::None)
      return r;
    if (lim_.spot_only) {
      const Money held = acct.reservation(row.instrument, row.side, row.px, row.leaves);
      const Money need = acct.reservation(row.instrument, row.side, px, new_leaves);
      const Money avail = row.side == common::Side::Buy
                              ? acct.quote_balance(row.instrument).available()
                              : acct.base_balance(row.instrument).available();
      if (need - held > avail)
        return row.side == common::Side::Buy ? RejectReason::InsufficientQuote
                                             : RejectReason::InsufficientBase;
    }
    const int64_t age = row.t_ack_ns ? now_ns - row.t_ack_ns : 0;
    if (!rate.would_allow(row.instrument, RateOp::Amend, age, now_ns))
      return RejectReason::RateLimit;
    return RejectReason::None;
  }

private:
  RejectReason status_check(const OrderCheck &o,
                            const ref::Instrument &inst) const noexcept {
    switch (inst.status) {
    case ref::ONLINE:
      return RejectReason::None;
    case ref::POST_ONLY:
      return o.tif == oe::Tif::PostOnly ? RejectReason::None
                                        : RejectReason::InstrumentStatus;
    case ref::LIMIT_ONLY:
      return o.ord_type == oe::OrdType::Limit ? RejectReason::None
                                              : RejectReason::InstrumentStatus;
    default:
      return RejectReason::InstrumentStatus;
    }
  }

  // Worst case if every open order on the order's side filled, plus this one.
  RejectReason position_check(uint16_t inst, common::Side side, int64_t add_qty,
                              int64_t px, const Account &acct,
                              const OrderTable &orders) const noexcept {
    if (!lim_.max_position && !lim_.max_position_notional)
      return RejectReason::None;
    const int64_t pos = acct.position(inst).qty;
    const int64_t worst = side == common::Side::Buy
                              ? pos + orders.open_buy_lots(inst) + add_qty
                              : pos - orders.open_sell_lots(inst) - add_qty;
    const int64_t abs_worst = worst < 0 ? -worst : worst;
    if (lim_.max_position && abs_worst > lim_.max_position)
      return RejectReason::MaxPosition;
    if (lim_.max_position_notional &&
        acct.notional(inst, px, abs_worst) > lim_.max_position_notional)
      return RejectReason::MaxPositionNotional;
    return RejectReason::None;
  }

  RejectReason band_check(int64_t px, int64_t mid2) const noexcept {
    if (lim_.price_band_bp <= 0 || px <= 0 || mid2 <= 0)
      return RejectReason::None;
    const double mid = double(mid2) / 2;
    const double dev_bp = (double(px) - mid) / mid * 1e4;
    return (dev_bp > lim_.price_band_bp || dev_bp < -lim_.price_band_bp)
               ? RejectReason::PriceBand
               : RejectReason::None;
  }

  RiskLimits lim_{};
  char allowed_[256] = "BTC,ETH";
  bool allowed_inst_[ref::MAX_INSTRUMENTS] = {};
};

} // namespace exec
