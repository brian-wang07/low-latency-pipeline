#pragma once

#include "common/event.hpp"
#include "common/ipc/instrument.hpp"
#include "common/ipc/market_update.hpp"
#include "common/ipc/stats.hpp"
#include <cstdint>
#include <cstring>

namespace exec {

// Money and balances are int64 in 1e-8 units of their asset (as ExecReport.fee and
// Instrument.min_notional). Prices are ticks and sizes lots of the instrument.
using Money = int64_t;
using i128 = __int128;

inline constexpr int MAX_TRADED = 16; // stats::PositionStats rows
inline constexpr int MAX_ASSETS = 16; // stats::BalanceStats rows
inline constexpr int16_t NO_SLOT = -1;

inline i128 pow10_i128(int e) noexcept {
  i128 v = 1;
  while (e-- > 0)
    v *= 10;
  return v;
}

// Splits a venue symbol into base and quote asset names: "BTC/USD" (Kraken),
// "BTC-USD" (Coinbase); a bare ticker (ITCH) is the base, quoted in USD.
inline void split_symbol(const char *symbol, char (&base)[8],
                         char (&quote)[8]) noexcept {
  std::memset(base, 0, sizeof(base));
  std::memset(quote, 0, sizeof(quote));
  const char *sep = std::strpbrk(symbol, "/-");
  if (!sep) {
    std::strncpy(base, symbol, sizeof(base) - 1);
    std::strcpy(quote, "USD");
    return;
  }
  const std::size_t bl = std::size_t(sep - symbol);
  std::memcpy(base, symbol, bl < sizeof(base) - 1 ? bl : sizeof(base) - 1);
  std::strncpy(quote, sep + 1, sizeof(quote) - 1);
}

// Positions, PnL and spot balances for the instruments exec trades. Single
// threaded (the exec hot thread); snapshots go to shm via stats.
class Account {
public:
  struct Position {
    int64_t qty = 0; // lots, signed
    i128 cost = 0;   // signed sum of px*qty (tick-lots) of the open position
    i128 realized = 0; // tick-lots
    Money fees = 0;
    int64_t bought = 0, sold = 0; // lots
    int64_t mark2 = 0;            // last bid+ask (twice the mid, in ticks); 0 = unmarked
    uint32_t fills = 0;
  };

  struct Balance {
    char name[8];
    Money total;
    Money reserved;
    Money available() const noexcept { return total - reserved; }
  };

  Account() noexcept {
    for (auto &s : slot_of_)
      s = NO_SLOT;
  }

  // Registers an instrument and its base/quote assets. Returns false if full.
  bool add_instrument(uint16_t inst, const ref::Instrument &row) noexcept {
    if (slot_of_[inst] != NO_SLOT)
      return true;
    if (n_inst_ == MAX_TRADED)
      return false;
    char base[8], quote[8];
    split_symbol(row.symbol, base, quote);
    const int b = asset(base), q = asset(quote);
    if (b < 0 || q < 0)
      return false;
    Inst &in = inst_[n_inst_];
    in = {};
    in.id = inst;
    in.base = int16_t(b);
    in.quote = int16_t(q);
    // notional(px, qty) = px*qty * tick_mant*lot_mant * 10^(tick_exp+lot_exp+8)
    in.tl_mul = i128(row.tick_mant) * row.lot_mant;
    in.tl_exp = row.tick_exp + row.lot_exp + 8;
    // base 1e-8 units per lot = lot_mant * 10^(lot_exp+8)
    in.base_per_lot = int64_t(i128(row.lot_mant) * pow10_i128(row.lot_exp + 8));
    slot_of_[inst] = int16_t(n_inst_++);
    return true;
  }

  // Finds or adds an asset by name; -1 when full.
  int asset(const char *name) noexcept {
    for (int i = 0; i < n_assets_; ++i)
      if (std::strncmp(bal_[i].name, name, sizeof(bal_[i].name)) == 0)
        return i;
    if (n_assets_ == MAX_ASSETS)
      return -1;
    Balance &b = bal_[n_assets_];
    std::memset(&b, 0, sizeof(b));
    std::strncpy(b.name, name, sizeof(b.name) - 1);
    return n_assets_++;
  }

  void set_balance(const char *name, Money total) noexcept {
    const int a = asset(name);
    if (a >= 0)
      bal_[a].total = total;
  }

  bool trades(uint16_t inst) const noexcept { return slot_of_[inst] != NO_SLOT; }

  // Quote-currency value of px*qty, 1e-8 units (truncated toward zero).
  Money notional(uint16_t inst, int64_t px, int64_t qty) const noexcept {
    return tl_to_money(in(inst), i128(px) * qty);
  }
  int64_t base_units(uint16_t inst, int64_t lots) const noexcept {
    return lots * in(inst).base_per_lot;
  }

  const Balance &base_balance(uint16_t inst) const noexcept {
    return bal_[in(inst).base];
  }
  const Balance &quote_balance(uint16_t inst) const noexcept {
    return bal_[in(inst).quote];
  }
  const Position &position(uint16_t inst) const noexcept {
    return in(inst).pos;
  }

  // Funds an order would lock: quote for a buy, base for a sell.
  Money reservation(uint16_t inst, common::Side side, int64_t px,
                    int64_t qty) const noexcept {
    return side == common::Side::Buy ? notional(inst, px, qty)
                                     : base_units(inst, qty);
  }
  void reserve(uint16_t inst, common::Side side, int64_t px,
               int64_t qty) noexcept {
    side_balance(inst, side).reserved += reservation(inst, side, px, qty);
  }
  void release(uint16_t inst, common::Side side, int64_t px,
               int64_t qty) noexcept {
    side_balance(inst, side).reserved -= reservation(inst, side, px, qty);
  }

  // Applies an execution of qty lots at px. order_px is the order's limit, whose
  // reservation the fill releases. fee is 1e-8 units of the quote (fee_in_base
  // false) or base asset.
  void on_fill(uint16_t inst, common::Side side, int64_t px, int64_t qty,
               int64_t order_px, Money fee, bool fee_in_base) noexcept {
    Inst &i = in(inst);
    Position &p = i.pos;
    release(inst, side, order_px, qty);

    const int64_t signed_qty = side == common::Side::Buy ? qty : -qty;
    const Money value = notional(inst, px, qty);
    Balance &base = bal_[i.base], &quote = bal_[i.quote];
    if (side == common::Side::Buy) {
      base.total += base_units(inst, qty);
      quote.total -= value;
      p.bought += qty;
    } else {
      base.total -= base_units(inst, qty);
      quote.total += value;
      p.sold += qty;
    }
    if (fee_in_base) {
      base.total -= fee;
      p.fees += i.base_per_lot ? Money(i128(fee) * value / (i128(qty) * i.base_per_lot))
                               : 0;
    } else {
      quote.total -= fee;
      p.fees += fee;
    }
    ++p.fills;

    // Average-cost position: reduce first, then open any remainder.
    int64_t rest = signed_qty;
    if (p.qty != 0 && (p.qty > 0) != (rest > 0)) {
      const int64_t open_abs = p.qty > 0 ? p.qty : -p.qty;
      const int64_t rest_abs = rest > 0 ? rest : -rest;
      const int64_t r = rest_abs < open_abs ? rest_abs : open_abs;
      const i128 removed = p.cost * r / open_abs; // same sign as the position
      const i128 sgn = p.qty > 0 ? 1 : -1;
      p.realized += sgn * i128(px) * r - removed;
      p.cost -= removed;
      p.qty += p.qty > 0 ? -r : r;
      rest += rest > 0 ? -r : r;
      if (p.qty == 0)
        p.cost = 0;
    }
    if (rest != 0) {
      p.cost += i128(px) * rest;
      p.qty += rest;
    }
    track_equity();
  }

  // Marks an instrument to its mid; ignored unless both sides are present.
  void mark(uint16_t inst, int64_t bid, int64_t ask) noexcept {
    if (slot_of_[inst] == NO_SLOT || bid == EMPTY_BID || ask == EMPTY_ASK)
      return;
    in(inst).pos.mark2 = bid + ask;
    track_equity();
  }

  Money realized(uint16_t inst) const noexcept {
    return tl_to_money(in(inst), in(inst).pos.realized);
  }
  // Unrealized PnL at the last mark: qty*mid - cost.
  Money unrealized(uint16_t inst) const noexcept {
    const Inst &i = in(inst);
    if (i.pos.mark2 == 0 || i.pos.qty == 0)
      return 0;
    return tl_to_money(i, (i128(i.pos.qty) * i.pos.mark2 - 2 * i.pos.cost) / 2);
  }
  Money equity() const noexcept {
    Money e = 0;
    for (int k = 0; k < n_inst_; ++k)
      e += realized(inst_[k].id) + unrealized(inst_[k].id) - inst_[k].pos.fees;
    return e;
  }
  Money peak_equity() const noexcept { return peak_; }
  Money max_drawdown() const noexcept { return max_dd_; }

  // Average entry price in ticks (0 when flat).
  int64_t avg_px(uint16_t inst) const noexcept {
    const Position &p = in(inst).pos;
    return p.qty ? int64_t(p.cost / p.qty) : 0;
  }

  void snapshot(stats::PositionStats &out) const noexcept {
    out.count = uint32_t(n_inst_);
    for (int k = 0; k < n_inst_; ++k) {
      const Inst &i = inst_[k];
      stats::Position &r = out.rows[k];
      r = {};
      r.instrument = i.id;
      r.qty_lots = i.pos.qty;
      r.avg_px_ticks = avg_px(i.id);
      r.realized_pnl = realized(i.id);
      r.unrealized_pnl = unrealized(i.id);
      r.fees = i.pos.fees;
      r.bought = i.pos.bought;
      r.sold = i.pos.sold;
      r.peak_equity = peak_;
      r.max_drawdown = max_dd_;
      r.fills = i.pos.fills;
    }
  }
  void snapshot(stats::BalanceStats &out) const noexcept {
    out.count = uint32_t(n_assets_);
    for (int a = 0; a < n_assets_; ++a) {
      std::memcpy(out.assets[a].name, bal_[a].name, sizeof(out.assets[a].name));
      out.assets[a].total = bal_[a].total;
      out.assets[a].reserved = bal_[a].reserved;
    }
  }

  int instrument_count() const noexcept { return n_inst_; }
  int asset_count() const noexcept { return n_assets_; }
  const Balance &balance(int a) const noexcept { return bal_[a]; }

private:
  struct Inst {
    uint16_t id;
    int16_t base, quote;
    int tl_exp;
    i128 tl_mul;
    int64_t base_per_lot;
    Position pos;
  };

  Inst &in(uint16_t inst) noexcept { return inst_[slot_of_[inst]]; }
  const Inst &in(uint16_t inst) const noexcept { return inst_[slot_of_[inst]]; }

  Balance &side_balance(uint16_t inst, common::Side side) noexcept {
    return bal_[side == common::Side::Buy ? in(inst).quote : in(inst).base];
  }

  static Money tl_to_money(const Inst &i, i128 tick_lots) noexcept {
    const i128 v = tick_lots * i.tl_mul;
    return i.tl_exp >= 0 ? Money(v * pow10_i128(i.tl_exp))
                         : Money(v / pow10_i128(-i.tl_exp));
  }

  void track_equity() noexcept {
    const Money e = equity();
    if (e > peak_)
      peak_ = e;
    if (peak_ - e > max_dd_)
      max_dd_ = peak_ - e;
  }

  int16_t slot_of_[ref::MAX_INSTRUMENTS];
  Inst inst_[MAX_TRADED]{};
  int n_inst_ = 0;
  Balance bal_[MAX_ASSETS]{};
  int n_assets_ = 0;
  Money peak_ = 0;
  Money max_dd_ = 0;
};

} // namespace exec
