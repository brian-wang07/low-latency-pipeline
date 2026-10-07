#pragma once

#include "common/platform/tsc.hpp"
#include "exec/exec_context.hpp"
#include "exec/market_view.hpp"
#include "exec/order_table.hpp"
#include "exec/router.hpp"
#include <cstdint>

namespace exec {

struct PositionView {
  int64_t qty;          // lots
  int64_t avg_px;       // ticks
  Money realized;       // 1e-8 quote units
  Money unrealized;     // at the last mid
};

// The surface a strategy sees: read the market and account, and submit, amend or
// cancel orders. Every request passes the risk gate before it is recorded and
// routed; a rejected one returns INVALID_ORDER (or false) and last_reject() says
// why. Templated on the router so the whole path inlines into the hot loop.
template <class R> class StrategyContext {
public:
  StrategyContext(ExecContext &x, R &router) noexcept : x_(x), router_(router) {}

  const MarketView &market() const noexcept { return x_.view; }
  const ref::Instrument &instrument(uint16_t i) const noexcept {
    return x_.instruments->rows[i];
  }
  int64_t now_ns() const noexcept { return x_.now_ns(); }
  uint64_t now_tsc() const noexcept { return read_tsc(); }

  PositionView position(uint16_t i) const noexcept {
    return {x_.account.position(i).qty, x_.account.avg_px(i),
            x_.account.realized(i), x_.account.unrealized(i)};
  }
  // Position in the current frame's instrument, in lots.
  int64_t inventory() const noexcept {
    return x_.account.position(x_.view.instrument()).qty;
  }
  // Account equity (realized + unrealized - fees), quote currency units.
  double pnl() const noexcept { return double(x_.account.equity()) / 1e8; }

  uint32_t open_orders(uint16_t i) const noexcept { return x_.orders.live_count(i); }
  const OrderRow *order(OrderId id) const noexcept { return x_.orders.find(id); }
  bool is_live(OrderId id) const noexcept { return x_.orders.find(id) != nullptr; }
  double rate_budget(uint16_t i) const noexcept {
    return x_.rate.budget(i, x_.now_ns());
  }
  RejectReason last_reject() const noexcept { return x_.last_reject; }

  // A new order on the current frame's instrument. px in ticks (0 for market),
  // qty in lots.
  OrderId submit(common::Side side, int64_t px, int64_t qty,
                 oe::Tif tif = oe::Tif::GTC,
                 oe::OrdType type = oe::OrdType::Limit) noexcept {
    const uint16_t inst = x_.view.instrument();
    const int64_t now = x_.now_ns();
    const OrderCheck oc{inst, side, type, tif, px, qty, x_.view.mid2(), now};
    RejectReason r = x_.risk.check_new(oc, x_.instruments->rows[inst], x_.account,
                                       x_.orders, x_.rate, x_.control_flags);
    if (r == RejectReason::None && x_.orders.full())
      r = RejectReason::TableFull;
    if (r != RejectReason::None) {
      x_.count_reject(r);
      return INVALID_ORDER;
    }
    OrderRow row{};
    row.id = x_.orders.next_id();
    row.tsc_decision = read_tsc();
    row.t_submit_ns = now;
    // A market order reserves (and later releases) at mid; the request stays px 0.
    row.px = px > 0 ? px : x_.view.mid2() / 2;
    row.qty = row.leaves = qty;
    row.instrument = inst;
    row.side = side;
    row.tif = tif;
    row.ord_type = type;
    row.status = oe::OrdStatus::PendingNew;
    const oe::OrderRequest req{row.id, 0,   row.tsc_decision, 0,    px,  qty,
                               inst,   oe::ReqType::New,      side, type, tif,
                               0,      0};
    x_.orders.insert(row);
    if (!router_.submit(req)) {
      x_.orders.erase(row.id);
      x_.count_reject(RejectReason::RingFull);
      return INVALID_ORDER;
    }
    x_.account.reserve(inst, side, row.px, qty);
    x_.rate.charge(inst, RateOp::Add, 0, now);
    ++x_.st.orders_new;
    record_tick_to_order();
    return row.id;
  }

  // Amends a live order to px and a new total qty (amend keeps queue priority
  // where the venue allows). False if rejected, unknown or already pending.
  bool replace(OrderId id, int64_t px, int64_t qty) noexcept {
    OrderRow *row = x_.orders.find(id);
    if (!row || row->pending) {
      x_.count_reject(RejectReason::UnknownOrder);
      return false;
    }
    const int64_t now = x_.now_ns();
    const RejectReason r = x_.risk.check_replace(
        *row, px, qty, x_.view.mid2(), now, x_.instruments->rows[row->instrument],
        x_.account, x_.orders, x_.rate, x_.control_flags);
    if (r != RejectReason::None) {
      x_.count_reject(r);
      return false;
    }
    const oe::OrderRequest req{id,  id,  read_tsc(),       row->strategy_tag,
                               px,  qty, row->instrument,  oe::ReqType::Replace,
                               row->side, row->ord_type,   row->tif, 0, 0};
    if (!router_.submit(req)) {
      x_.count_reject(RejectReason::RingFull);
      return false;
    }
    row->pending |= PENDING_REPLACE;
    row->new_px = px;
    row->new_qty = qty;
    x_.rate.charge(row->instrument, RateOp::Amend,
                   row->t_ack_ns ? now - row->t_ack_ns : 0, now);
    ++x_.st.orders_replace;
    record_tick_to_order();
    return true;
  }

  // Cancels are never risk-rejected; they still cost rate budget.
  bool cancel(OrderId id) noexcept {
    OrderRow *row = x_.orders.find(id);
    if (!row)
      return false;
    if (row->pending & PENDING_CANCEL)
      return true;
    const int64_t now = x_.now_ns();
    const oe::OrderRequest req{id,  id,  read_tsc(),      row->strategy_tag,
                               row->px, row->leaves, row->instrument,
                               oe::ReqType::Cancel, row->side, row->ord_type,
                               row->tif, 0, 0};
    if (!router_.submit(req)) {
      x_.count_reject(RejectReason::RingFull);
      return false;
    }
    row->pending |= PENDING_CANCEL;
    x_.rate.charge(row->instrument, RateOp::Cancel,
                   row->t_ack_ns ? now - row->t_ack_ns : 0, now);
    ++x_.st.orders_cancel;
    return true;
  }

  void cancel_all() noexcept {
    for (uint32_t i = 0; i < x_.orders.size(); ++i)
      cancel(x_.orders.row(i).id);
  }

private:
  void record_tick_to_order() noexcept {
    if (x_.in_tick)
      x_.st.tick_to_order.record(read_tsc() - x_.frame_tsc_in);
  }

  ExecContext &x_;
  R &router_;
};

// Static-dispatch contract: on_tick(Ctx&) noexcept, usually a template over the
// context so one strategy serves every router. Optional hooks, called only if
// present: on_start(ctx), on_stop(ctx), on_exec(ctx, const oe::ExecReport&),
// on_timer(ctx, int64_t now_ns).
template <class S, class Ctx>
concept Strategy = requires(S s, Ctx &ctx) {
  { s.on_tick(ctx) } noexcept;
};

} // namespace exec
