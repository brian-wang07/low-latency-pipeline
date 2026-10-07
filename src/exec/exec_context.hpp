#pragma once

#include "common/config_file.hpp"
#include "common/ipc/shm.hpp"
#include "common/platform/spin_pause.hpp"
#include "common/platform/tsc.hpp"
#include "exec/account.hpp"
#include "exec/market_view.hpp"
#include "exec/order_table.hpp"
#include "exec/rate_model.hpp"
#include "exec/risk.hpp"
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace exec {

// Everything exec's hot loop owns: market view, account, orders, risk and the
// counters it publishes. One instance in static storage; it is also the state a
// strategy module runs against, so its layout is part of the module ABI.
struct ExecContext {
  // Wiring into shm (set by setup()).
  ipc::PipelineShm *shm = nullptr;
  FeedRing *feed = nullptr;
  const ref::InstrumentTable *instruments = nullptr;
  const volatile std::sig_atomic_t *shutdown = nullptr;

  MarketView view;
  Account account;
  OrderTable orders;
  RiskGate risk;
  RateModel rate;

  // Clock: ITCH replay runs on event time so rate limits and order ages follow the
  // market's pace; live runs on the TSC.
  bool event_clock = true;
  double tsc_per_ns = 1.0;
  int64_t event_ns = 0;
  uint64_t frame_tsc_in = 0; // tsc_in of the frame being handled
  bool in_tick = false;      // tick_to_order is only meaningful inside on_tick
  uint32_t control_flags = 0;
  RejectReason last_reject = RejectReason::None;
  uint64_t timer_cycles = 0;

  stats::ExecStats st{}; // live counters and histograms, published as-is

  int64_t now_ns() const noexcept {
    return event_clock ? event_ns : int64_t(double(read_tsc()) / tsc_per_ns);
  }

  void count_reject(RejectReason r) noexcept {
    last_reject = r;
    ++st.rejects[size_t(r)];
    if (r == RejectReason::RingFull)
      ++st.ring_full_rejects;
    else
      ++st.risk_rejects;
  }

  // Applies a venue (or NullRouter) report to the order table and account.
  void apply_report(const oe::ExecReport &rep) noexcept {
    OrderRow *row = orders.find(rep.cl_ord_id);
    if (!row) {
      ++st.unsolicited;
      return;
    }
    switch (rep.exec_type) {
    case oe::ExecType::Ack:
      row->status = oe::OrdStatus::New;
      row->t_ack_ns = now_ns();
      ++st.acks;
      break;
    case oe::ExecType::PartialFill:
    case oe::ExecType::Fill: {
      const int64_t q = rep.last_qty;
      account.on_fill(row->instrument, row->side, rep.last_px, q, row->px,
                      rep.fee, rep.fee_ccy == 1);
      row->cum += q;
      orders.set_leaves(*row, row->leaves > q ? row->leaves - q : 0);
      row->status = rep.exec_type == oe::ExecType::Fill
                        ? oe::OrdStatus::Filled
                        : oe::OrdStatus::PartiallyFilled;
      ++st.fills;
      if (row->leaves == 0)
        finish(*row);
      break;
    }
    case oe::ExecType::Replaced: {
      account.release(row->instrument, row->side, row->px, row->leaves);
      row->px = row->new_px;
      row->qty = row->new_qty;
      orders.set_leaves(*row, row->qty - row->cum);
      account.reserve(row->instrument, row->side, row->px, row->leaves);
      row->pending &= uint8_t(~PENDING_REPLACE);
      ++st.replaced;
      break;
    }
    case oe::ExecType::Canceled:
      ++st.canceled;
      finish(*row);
      break;
    case oe::ExecType::Expired:
      ++st.expired;
      finish(*row);
      break;
    case oe::ExecType::Reject:
      ++st.venue_rejects;
      finish(*row);
      break;
    case oe::ExecType::CancelReject:
      row->pending &= uint8_t(~(PENDING_CANCEL | PENDING_REPLACE));
      break;
    case oe::ExecType::Restated:
      break; // exec restart handoff, Phase 4
    }
  }

  void publish() noexcept {
    st.heartbeat_tsc = read_tsc();
    st.frame_gaps = view.frame_gaps();
    st.open_orders = orders.size();
    const int64_t now = now_ns();
    for (int k = 0; k < 4; ++k)
      st.rate_budget[k] = k < account.instrument_count()
                              ? rate.budget(traded_[k], now)
                              : 0.0;
    shm->exec_stats.store(st);
    stats::PositionStats ps;
    account.snapshot(ps);
    shm->positions.store(ps);
    stats::BalanceStats bs{};
    account.snapshot(bs);
    shm->balances.store(bs);
    stats::OpenOrders oo;
    orders.snapshot(oo);
    shm->open_orders.store(oo);
  }

  // Registers every instrument the feed producer has published (waiting for at
  // least one), and reads risk, rate and null-mode balances from config.
  bool setup(ipc::PipelineShm *p, const common::Config &cfg,
             const volatile std::sig_atomic_t *shutdown_flag) noexcept {
    shm = p;
    feed = &p->feed_to_exec;
    instruments = &p->instruments;
    shutdown = shutdown_flag;

    calibrate_tsc(tsc_per_ns);
    timer_cycles = uint64_t(cfg.get_f64("exec.timer_ms", 10.0) * 1e6 * tsc_per_ns);
    event_clock = std::strcmp(cfg.get_str("exec.clock", "event"), "wall") != 0;

    risk.configure(cfg);
    KrakenRateParams rp;
    rp.max = cfg.get_f64("kraken.rate_max", rp.max);
    rp.decay_per_s = cfg.get_f64("kraken.rate_decay", rp.decay_per_s);
    rp.max_open = uint32_t(cfg.get_i64("kraken.max_open_per_pair", rp.max_open));
    rp.margin = cfg.get_f64("rate.margin", rp.margin);
    rate.configure(std::strcmp(cfg.get_str("rate.model", "kraken"), "none") != 0,
                   rp);

    while (instruments->size() == 0) {
      if (*shutdown)
        return false;
      SPIN_PAUSE();
    }
    const uint32_t n = instruments->size();
    for (uint32_t i = 0; i < n; ++i) {
      if (!account.add_instrument(uint16_t(i), instruments->rows[i]))
        break;
      risk.register_instrument(uint16_t(i), instruments->rows[i]);
      if (i < 4)
        traded_[i] = uint16_t(i);
    }
    bool ok = true;
    cfg.for_prefix("null.balance.", [&](const char *asset, const char *v) {
      char *end = nullptr;
      const double amount = std::strtod(v, &end);
      if (end == v || *end != '\0') {
        std::fprintf(stderr, "exec: bad balance null.balance.%s=%s\n", asset, v);
        ok = false;
        return;
      }
      account.set_balance(asset, Money(std::llround(amount * 1e8)));
    });

    // Continue the generation sequence so cl_ord_ids never repeat across restarts.
    stats::ExecStats prev{};
    for (int tries = 0; tries < 1000 && !p->exec_stats.try_load(prev); ++tries) {
    }
    st.exec_generation = prev.exec_generation + 1;
    orders.set_generation(st.exec_generation);
    st.limit_max_position = risk.limits().max_position;
    st.limit_max_position_notional = risk.limits().max_position_notional;
    return ok;
  }

private:
  // Terminal state: give back the reservation and drop the row.
  void finish(OrderRow &row) noexcept {
    account.release(row.instrument, row.side, row.px, row.leaves);
    orders.erase(row.id);
  }

  uint16_t traded_[4] = {};
};

} // namespace exec
