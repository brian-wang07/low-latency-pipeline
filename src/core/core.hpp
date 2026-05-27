#pragma once

#include "common/event.hpp"
#include <climits>
#include <cstdint>
#include <memory>

namespace core::equity {

using Price = uint32_t; // 4 decimal fixed point
using Qty = uint32_t;
using OrderRef = uint64_t;

// min tick size is $0.01; prices come in at 4 decimal point accuracy, so scale by this much 
inline constexpr Price PRICE_TICK = 100;

struct OrderEntry {
  Price price;
  Qty shares;
  common::Side side;
};

// Flat open-addressed hash, robin-hood eviction. Single-writer; no alignment
// padding because there are no concurrent readers to false-share with.
template <uint32_t Capacity> class OrderMap {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be power of 2");

public:
  static constexpr uint32_t CAPACITY = Capacity;
  static constexpr uint32_t MASK = Capacity - 1;

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
  Slot slots_[Capacity]{};
};

struct PriceLevel {
  Qty total_shares;
  uint32_t order_count;
};

template <uint32_t MaxLevels> class PriceLevelArray {
  static_assert((MaxLevels & (MaxLevels - 1)) == 0,
                "MaxLevels must be power of 2");

public:
  static constexpr uint32_t MAX_LEVELS = MaxLevels;

  void init(Price base_price) noexcept;
  PriceLevel &at(Price price) noexcept;
  const PriceLevel &at(Price price) const noexcept;
  uint32_t index_of(Price price) const noexcept;
  bool in_range(Price price) const noexcept;
  Price base_price() const noexcept;

private:
  Price base_price_{0};
  PriceLevel levels_[MaxLevels]{};
};

struct TopOfBook {
  Price best_bid{0};
  Price best_ask{UINT32_MAX};
};

// is this overkill?
inline constexpr uint32_t DEFAULT_ORDER_CAPACITY = 1048576;
inline constexpr uint32_t DEFAULT_LEVEL_COUNT = 1048576;

class OrderBook {
public:
  using LevelArr = PriceLevelArray<DEFAULT_LEVEL_COUNT>;
  using Orders = OrderMap<DEFAULT_ORDER_CAPACITY>;

  struct Level {
    Price price;
    Qty shares;
  };

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
  Qty qty_at_bid(Price p) const noexcept;
  Qty qty_at_ask(Price p) const noexcept;

  // Walk top non-empty levels from TOB into out[]. Returns number written.
  int top_bids(Level *out, int max_out) const noexcept;
  int top_asks(Level *out, int max_out) const noexcept;

  uint64_t stock_id() const noexcept;
  bool initialized() const noexcept;

private:
  void rescan(common::Side side, Price price) noexcept;

  bool initialized_{false};
  TopOfBook tob_;
  uint64_t stock_id_{0};
  LevelArr bids_;
  LevelArr asks_;
  Orders orders_;
};

struct BookArray {
  std::unique_ptr<OrderBook> books_[65536];

  OrderBook &ensure(uint16_t locate, uint64_t stock_id,
                    Price base_price) noexcept;
  OrderBook *get(uint16_t locate) noexcept;
  const OrderBook *get(uint16_t locate) const noexcept;
};

} // namespace core::equity
