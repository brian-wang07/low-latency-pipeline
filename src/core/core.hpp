#pragma once

#include "common/event.hpp"
#include <cstdint>

namespace core {

// stock locate hashmap, orderbook impl

using Price = uint32_t; // 4 decimal fixed point
using Qty = uint32_t;
using OrderRef = uint64_t; // order_ref_number

// Passive orderbook reconstruction. As data is pre-matched by the exchange, we
// only need to reconstruct the state.
struct alignas(32) OrderEntry {
  Price price;
  Qty shares;
  common::Side side;
  char _pad[3];
};

// flat hash map: by storing keys and values in a contiguous array, we can
// maximize cache locality.
struct OrderMap {
public:
  static constexpr uint32_t CAPACITY = 1 << 20;
  static constexpr uint32_t MASK = CAPACITY - 1;

  // pointer to order reference number, nullptr if dne
  OrderEntry *find(OrderRef ref) noexcept;

  void insert(OrderRef ref, Price price, Qty shares,
              common::Side side) noexcept;

  void erase(OrderRef ref) noexcept;

private:
  static uint32_t hash(OrderRef ref) noexcept {
    return static_cast<uint32_t>((ref * 0x9e3779b97f4a7c15ULL) >> 44);
  }

  struct Slot {
    OrderRef key;
    OrderEntry entry;
    bool occupied;
  };
  alignas(64) Slot slots_[CAPACITY];
};

struct alignas(64) PriceLevel {
  Qty total_shares;
  uint32_t order_count;
  char _pad[56];
};

class PriceLevelArray {
public:
  static constexpr uint32_t MAX_LEVELS =
      1 << 16; // 665536 levels @ $0.0001/tick -> $6.55 range; maybe widen?

  // on first order seen; set base price, zero array
  void init(Price base_price) noexcept;

  PriceLevel &at(Price price) noexcept;
  const PriceLevel &at(Price price) const noexcept;

  uint32_t index_of(Price price) const noexcept;

  Price base_price() const noexcept;

  bool in_range(Price price) const noexcept;

private:
  Price base_price_;
  alignas(64) PriceLevel levels_[MAX_LEVELS];
};

struct alignas(64) TopOfBook {
  Price best_bid{0};
  Price best_ask{UINT32_MAX};
};

class OrderBook {
public:
  OrderBook() = default;
  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;

  void init(uint64_t stock_id, Price base_price) noexcept;

  // itch A, F
  void on_add(OrderRef ref, common::Side side, Price price,
              Qty shares) noexcept;
  // itch E
  void on_execute(OrderRef ref, Qty executed_shares) noexcept;
  // itch C
  void on_execute_with_price(OrderRef ref, Qty executed_shares,
                             Price execution_price) noexcept;
  // itch X
  void on_cancel(OrderRef ref, Qty cancelled_shares) noexcept;
  // itch D
  void on_delete(OrderRef ref) noexcept;
  // itch U
  void on_replace(OrderRef old_ref, OrderRef new_ref, Price new_price,
                  Qty new_shares) noexcept;

  Price best_bid() const noexcept;
  Price best_ask() const noexcept;
  Qty qty_at_bid(const Price p) const noexcept;
  Qty qty_at_ask(const Price p) const noexcept;

  uint64_t stock_id() const noexcept;
  bool initialized() const noexcept;

private:
  void rescan(common::Side side, Price price) noexcept;
  bool initialized_{false};
  TopOfBook tob_;
  uint64_t stock_id_;
  PriceLevelArray bids_;
  PriceLevelArray asks_;
  OrderMap orders_;
};

struct BookArray {
  OrderBook books_[65536];
};

} // namespace core