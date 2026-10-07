#pragma once

#include "common/event.hpp"
#include "common/ipc/instrument.hpp"
#include "common/ipc/order_msgs.hpp"
#include "common/ipc/stats.hpp"
#include <algorithm>
#include <cstdint>

namespace exec {

using OrderId = uint64_t; // the order's cl_ord_id; stable across replaces
inline constexpr OrderId INVALID_ORDER = 0;

enum PendingBits : uint8_t { PENDING_CANCEL = 1, PENDING_REPLACE = 2 };

struct OrderRow {
  OrderId id;
  uint64_t strategy_tag;
  uint64_t tsc_decision;
  int64_t t_submit_ns, t_ack_ns; // exec clock; t_ack_ns 0 until acked
  int64_t px, qty, leaves, cum;
  int64_t new_px, new_qty; // replace in flight
  uint16_t instrument;
  common::Side side;
  oe::Tif tif;
  oe::OrdType ord_type;
  oe::OrdStatus status;
  uint8_t pending; // PendingBits
};

// Exec's live orders: terminal orders are erased. Rows sit in a dense array (swap-remove on erase) so iteration
// touches only live orders; an open-addressed index (same hash and backward-shift
// erase as core's OrderMap) maps cl_ord_id to row.
class OrderTable {
public:
  static constexpr uint32_t MAX_ROWS = 2048;
  static constexpr uint32_t INDEX_SLOTS = 4096; // load factor <= 0.5
  static constexpr uint32_t MASK = INDEX_SLOTS - 1;

  // cl_ord_id = (generation << 40) | counter: unique across exec restarts.
  void set_generation(uint64_t gen) noexcept { gen_ = gen & 0xFFFFFF; }
  OrderId next_id() noexcept { return (gen_ << 40) | (++counter_ & ((1ull << 40) - 1)); }

  uint32_t size() const noexcept { return n_; }
  bool full() const noexcept { return n_ == MAX_ROWS; }
  OrderRow &row(uint32_t i) noexcept { return rows_[i]; }
  const OrderRow &row(uint32_t i) const noexcept { return rows_[i]; }

  OrderRow *find(OrderId id) noexcept {
    const uint32_t s = slot_of(id);
    return s == NONE ? nullptr : &rows_[index_[s].pos];
  }
  const OrderRow *find(OrderId id) const noexcept {
    return const_cast<OrderTable *>(this)->find(id);
  }

  // Returns nullptr when full or the id is already present.
  OrderRow *insert(const OrderRow &r) noexcept {
    if (n_ == MAX_ROWS || r.id == INVALID_ORDER || slot_of(r.id) != NONE)
      return nullptr;
    uint32_t pos = hash(r.id);
    while (index_[pos & MASK].key != 0)
      ++pos;
    index_[pos & MASK] = {r.id, n_};
    rows_[n_] = r;
    add_open(r, r.leaves);
    ++live_[r.instrument];
    return &rows_[n_++];
  }

  void erase(OrderId id) noexcept {
    uint32_t s = slot_of(id);
    if (s == NONE)
      return;
    const uint32_t pos = index_[s].pos;
    add_open(rows_[pos], -rows_[pos].leaves);
    --live_[rows_[pos].instrument];
    // Swap-remove the dense row and repoint the moved row's index entry.
    const uint32_t last = --n_;
    if (pos != last) {
      rows_[pos] = rows_[last];
      index_[slot_of(rows_[pos].id)].pos = pos;
    }
    // Backward-shift deletion keeps probe chains intact without tombstones.
    uint32_t hole = s;
    for (uint32_t i = (s + 1) & MASK; index_[i].key != 0; i = (i + 1) & MASK) {
      const uint32_t home = hash(index_[i].key) & MASK;
      if (((i - home) & MASK) >= ((i - hole) & MASK)) {
        index_[hole] = index_[i];
        hole = i;
      }
    }
    index_[hole] = {};
  }

  // Changes a row's leaves, keeping the per-instrument open lots in step.
  void set_leaves(OrderRow &r, int64_t leaves) noexcept {
    add_open(r, leaves - r.leaves);
    r.leaves = leaves;
  }

  uint32_t live_count(uint16_t inst) const noexcept { return live_[inst]; }
  int64_t open_buy_lots(uint16_t inst) const noexcept { return open_buy_[inst]; }
  int64_t open_sell_lots(uint16_t inst) const noexcept { return open_sell_[inst]; }

  // Oldest orders first, at most the 64 that fit.
  void snapshot(stats::OpenOrders &out) const noexcept {
    uint16_t order[MAX_ROWS];
    for (uint32_t i = 0; i < n_; ++i)
      order[i] = uint16_t(i);
    const uint32_t k = n_ < 64 ? n_ : 64;
    std::partial_sort(order, order + k, order + n_, [&](uint16_t a, uint16_t b) {
      return rows_[a].t_submit_ns < rows_[b].t_submit_ns;
    });
    out.count = k;
    for (uint32_t i = 0; i < k; ++i) {
      const OrderRow &r = rows_[order[i]];
      out.rows[i] = {r.id, r.tsc_decision, r.px, r.qty, r.leaves,
                     r.instrument, r.side, r.status};
    }
  }

private:
  static constexpr uint32_t NONE = UINT32_MAX;

  struct Slot {
    uint64_t key; // 0 = empty (INVALID_ORDER is never inserted)
    uint32_t pos;
  };

  static uint32_t hash(uint64_t id) noexcept {
    return static_cast<uint32_t>((id * 0x9e3779b97f4a7c15ULL) >> 52);
  }

  uint32_t slot_of(OrderId id) const noexcept {
    for (uint32_t pos = hash(id);; ++pos) {
      const Slot &s = index_[pos & MASK];
      if (s.key == 0) // empty first: INVALID_ORDER (0) must never match
        return NONE;
      if (s.key == id)
        return pos & MASK;
    }
  }

  void add_open(const OrderRow &r, int64_t delta_lots) noexcept {
    (r.side == common::Side::Buy ? open_buy_ : open_sell_)[r.instrument] +=
        delta_lots;
  }

  Slot index_[INDEX_SLOTS]{};
  OrderRow rows_[MAX_ROWS];
  uint32_t n_ = 0;
  uint64_t gen_ = 0;
  uint64_t counter_ = 0;
  uint32_t live_[ref::MAX_INSTRUMENTS]{};
  int64_t open_buy_[ref::MAX_INSTRUMENTS]{};
  int64_t open_sell_[ref::MAX_INSTRUMENTS]{};
};

} // namespace exec
