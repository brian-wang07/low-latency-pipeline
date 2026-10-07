#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/event.hpp"
#include "core.hpp"

using namespace core::equity;

namespace core::equity {
std::atomic<uint64_t> g_drops{0};
} // namespace core::equity

// OrderMap
template <uint32_t Capacity>
OrderEntry *OrderMap<Capacity>::find(OrderRef ref) noexcept {
  // if we hit capacity, we are cooked
  for (uint32_t pos = hash(ref);; ++pos) {
    Slot &s = slots_[pos & MASK];
    if (!s.occupied)
      return nullptr;
    if (s.key == ref)
      return &s.entry;
  }
}

template <uint32_t Capacity>
void OrderMap<Capacity>::insert(OrderRef ref, Price price, Qty shares,
                                common::Side side) noexcept {
  for (uint32_t pos = hash(ref);; ++pos) {
    Slot &s = slots_[pos & MASK];
    if (!s.occupied) {
      s.key = ref;
      s.entry = OrderEntry{price, shares, side};
      s.occupied = true;
      return;
    }
  }
}

template <uint32_t Capacity>
void OrderMap<Capacity>::erase(OrderRef ref) noexcept {
  uint32_t pos = hash(ref);
  for (; slots_[pos & MASK].key != ref; ++pos)
    ;
  slots_[pos & MASK].occupied = false;

  uint32_t hole = pos++;
  for (;; ++pos) {
    Slot &s = slots_[pos & MASK];
    if (!s.occupied)
      return;
    uint32_t home = hash(s.key);
    if (((pos - home) & MASK) > ((hole - home) & MASK)) {
      slots_[hole & MASK] = s;
      s.occupied = false;
      hole = pos;
    }
  }
}

// PriceLevelArray
template <uint32_t MaxLevels>
uint32_t PriceLevelArray<MaxLevels>::index_of(Price price) const noexcept {
  return (price - base_price_) / PRICE_TICK;
}

template <uint32_t MaxLevels>
bool PriceLevelArray<MaxLevels>::in_range(Price price) const noexcept {
  return index_of(price) < MAX_LEVELS;
}

template <uint32_t MaxLevels>
PriceLevel &PriceLevelArray<MaxLevels>::at(Price price) noexcept {
  return levels_[index_of(price)];
}

template <uint32_t MaxLevels>
const PriceLevel &PriceLevelArray<MaxLevels>::at(Price price) const noexcept {
  return levels_[index_of(price)];
}

template <uint32_t MaxLevels>
void PriceLevelArray<MaxLevels>::init(Price base_price) noexcept {
  base_price_ = (base_price / PRICE_TICK) * PRICE_TICK;
  std::memset(levels_, 0, MAX_LEVELS * sizeof(PriceLevel));
  occupied_.reset();
}

template <uint32_t MaxLevels>
Price PriceLevelArray<MaxLevels>::base_price() const noexcept {
  return base_price_;
}

template <uint32_t MaxLevels>
void PriceLevelArray<MaxLevels>::mark_occupied(Price price) noexcept {
  occupied_.set(index_of(price));
}

template <uint32_t MaxLevels>
void PriceLevelArray<MaxLevels>::mark_empty(Price price) noexcept {
  occupied_.clear(index_of(price));
}

template <uint32_t MaxLevels>
Price
PriceLevelArray<MaxLevels>::next_occupied_above(Price price) const noexcept {
  uint32_t i = occupied_.next_set(index_of(price) + 1);
  return i == BITMAP_NPOS ? 0u : base_price_ + i * PRICE_TICK;
}

template <uint32_t MaxLevels>
Price
PriceLevelArray<MaxLevels>::prev_occupied_below(Price price) const noexcept {
  uint32_t idx = index_of(price);
  if (idx == 0)
    return 0u;
  uint32_t i = occupied_.prev_set(idx - 1);
  return i == BITMAP_NPOS ? 0u : base_price_ + i * PRICE_TICK;
}

// Explicit instantiation for templated classes
template class core::equity::OrderMap<DEFAULT_ORDER_CAPACITY>;
template class core::equity::PriceLevelArray<DEFAULT_LEVEL_COUNT>;

// Orderbook
// A level's order_count just hit zero. Clear its occupancy bit, and if it was
// the top of book, drop to the nearest surviving level on that side — the
// bitmap finds it in O(tiers) instead of walking empty ticks. Centralized here
// because every drain-to-zero path already funnels through this call.
void OrderBook::on_level_emptied(common::Side side, Price price) noexcept {
  if (side == common::Side::Buy) {
    bids_.mark_empty(price);
    if (price == tob_.best_bid)
      tob_.best_bid = bids_.prev_occupied_below(price); // 0 when side empties
  } else {
    asks_.mark_empty(price);
    if (price == tob_.best_ask) {
      Price next = asks_.next_occupied_above(price);
      tob_.best_ask = next ? next : UINT32_MAX;
    }
  }
}

void OrderBook::init(uint64_t stock_id, Price base_price) noexcept {
  stock_id_ = stock_id;
  tob_ = TopOfBook();
  bids_.init(base_price);
  asks_.init(base_price);
  initialized_ = true;
}

void OrderBook::on_add(OrderRef ref, common::Side side, Price price,
                       Qty shares) noexcept {
  if (side == common::Side::Buy) {
    if (!bids_.in_range(price)) {
      g_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    orders_.insert(ref, price, shares, side);
    PriceLevel &lvl = bids_.at(price);
    lvl.total_shares += shares;
    if (lvl.order_count++ == 0)
      bids_.mark_occupied(price);
    if (price > tob_.best_bid)
      tob_.best_bid = price;
  } else {
    if (!asks_.in_range(price)) {
      g_drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    orders_.insert(ref, price, shares, side);
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares += shares;
    if (lvl.order_count++ == 0)
      asks_.mark_occupied(price);
    if (price < tob_.best_ask)
      tob_.best_ask = price;
  }
}

OrderBook::Trade OrderBook::on_execute(OrderRef ref,
                                       Qty executed_shares) noexcept {
  OrderEntry *entry = orders_.find(ref);
  if (!entry)
    return {};
  Price price = entry->price;
  common::Side side = entry->side;
  if (side == common::Side::Buy) {
    PriceLevel &lvl = bids_.at(price);
    lvl.total_shares -= executed_shares;
    entry->shares -= executed_shares;
    if (entry->shares == 0) {
      --lvl.order_count;
      orders_.erase(ref);
      if (lvl.order_count == 0)
        on_level_emptied(common::Side::Buy, price);
    }
  } else {
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares -= executed_shares;
    entry->shares -= executed_shares;
    if (entry->shares == 0) {
      --lvl.order_count;
      orders_.erase(ref);
      if (lvl.order_count == 0)
        on_level_emptied(common::Side::Sell, price);
    }
  }
  return {price, executed_shares, side};
}

OrderBook::Trade OrderBook::on_execute_with_price(OrderRef ref,
                                                  Qty executed_shares,
                                                  Price execution_price) noexcept {
  Trade t = on_execute(ref, executed_shares);
  // 'C' prints at the actual execution price, which can differ from the resting
  // order's display price; the book accounting above used the display price.
  if (t.shares != 0)
    t.price = execution_price;
  return t;
}

void OrderBook::on_cancel(OrderRef ref, Qty cancelled_shares) noexcept {
  OrderEntry *entry = orders_.find(ref);
  if (!entry)
    return;
  Price price = entry->price;
  common::Side side = entry->side;
  if (side == common::Side::Buy) {
    PriceLevel &lvl = bids_.at(price);
    lvl.total_shares -= cancelled_shares;
    entry->shares -= cancelled_shares;
    // ITCH Cancel will only ever partially cancel an order; this should never
    // be dispatched, but good to check
    [[unlikely]] if (entry->shares == 0) {
      --lvl.order_count;
      orders_.erase(ref);
      if (lvl.order_count == 0)
        on_level_emptied(common::Side::Buy, price);
    }
  } else {
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares -= cancelled_shares;
    entry->shares -= cancelled_shares;
    [[unlikely]] if (entry->shares == 0) {
      --lvl.order_count;
      orders_.erase(ref);
      if (lvl.order_count == 0)
        on_level_emptied(common::Side::Sell, price);
    }
  }
}

void OrderBook::on_delete(OrderRef ref) noexcept {
  OrderEntry *entry = orders_.find(ref);
  if (!entry)
    return;
  Price price = entry->price;
  Qty shares = entry->shares;
  common::Side side = entry->side;
  if (side == common::Side::Buy) {
    PriceLevel &lvl = bids_.at(price);
    lvl.total_shares -= shares;
    --lvl.order_count;
    orders_.erase(ref);
    if (lvl.order_count == 0)
      on_level_emptied(common::Side::Buy, price);
  } else {
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares -= shares;
    --lvl.order_count;
    orders_.erase(ref);
    if (lvl.order_count == 0)
      on_level_emptied(common::Side::Sell, price);
  }
}

void OrderBook::on_replace(OrderRef old_ref, OrderRef new_ref, Price new_price,
                           Qty new_shares) noexcept {
  OrderEntry *old_entry = orders_.find(old_ref);
  if (!old_entry)
    return;
  Price old_price = old_entry->price;
  Qty old_shares = old_entry->shares;
  common::Side side = old_entry->side;

  if (side == common::Side::Buy) {
    PriceLevel &old_lvl = bids_.at(old_price);
    old_lvl.total_shares -= old_shares;
    --old_lvl.order_count;
    orders_.erase(old_ref);
    if (bids_.in_range(new_price)) {
      orders_.insert(new_ref, new_price, new_shares, side);
      PriceLevel &new_lvl = bids_.at(new_price);
      new_lvl.total_shares += new_shares;
      if (new_lvl.order_count++ == 0)
        bids_.mark_occupied(new_price);
      if (new_price > tob_.best_bid)
        tob_.best_bid = new_price;
    } else {
      g_drops.fetch_add(1, std::memory_order_relaxed);
    }
    if (old_lvl.order_count == 0)
      on_level_emptied(common::Side::Buy, old_price);
  } else {
    PriceLevel &old_lvl = asks_.at(old_price);
    old_lvl.total_shares -= old_shares;
    --old_lvl.order_count;
    orders_.erase(old_ref);
    if (asks_.in_range(new_price)) {
      orders_.insert(new_ref, new_price, new_shares, side);
      PriceLevel &new_lvl = asks_.at(new_price);
      new_lvl.total_shares += new_shares;
      if (new_lvl.order_count++ == 0)
        asks_.mark_occupied(new_price);
      if (new_price < tob_.best_ask)
        tob_.best_ask = new_price;
    } else {
      g_drops.fetch_add(1, std::memory_order_relaxed);
    }
    if (old_lvl.order_count == 0)
      on_level_emptied(common::Side::Sell, old_price);
  }
}

Price OrderBook::best_bid() const noexcept { return tob_.best_bid; }
Price OrderBook::best_ask() const noexcept { return tob_.best_ask; }

Qty OrderBook::qty_at_bid(Price p) const noexcept {
  return bids_.at(p).total_shares;
}
Qty OrderBook::qty_at_ask(Price p) const noexcept {
  return asks_.at(p).total_shares;
}

// Both walks hop straight between occupied levels via the bitmap, so every
// visited price is emitted — no per-tick scan over the empty span between them.
// best_bid/best_ask point at an occupied level (or the empty sentinel), and
// each hop lands on the next occupied level, so no order_count check is needed.
int OrderBook::top_bids(Level *out, int max_out) const noexcept {
  int n = 0;
  Price p = tob_.best_bid;
  while (n < max_out && p != 0) {
    const PriceLevel &lvl = bids_.at(p);
    out[n++] = {p, lvl.total_shares, lvl.order_count};
    p = bids_.prev_occupied_below(p); // 0 ends the walk
  }
  return n;
}

int OrderBook::top_asks(Level *out, int max_out) const noexcept {
  int n = 0;
  Price p = tob_.best_ask;
  while (n < max_out && p != UINT32_MAX) {
    const PriceLevel &lvl = asks_.at(p);
    out[n++] = {p, lvl.total_shares, lvl.order_count};
    Price next = asks_.next_occupied_above(p);
    p = next ? next : UINT32_MAX; // sentinel ends the walk
  }
  return n;
}

uint64_t OrderBook::stock_id() const noexcept { return stock_id_; }
bool OrderBook::initialized() const noexcept { return initialized_; }

// BookArray
void BookArray::prewarm() noexcept {
  if (!spare_)
    spare_ = std::make_unique<OrderBook>();
  // The default constructor already zero-initializes every Slot / PriceLevel,
  // which writes (and thus faults in) every page. Touch it once more anyway to
  // be explicit and defeat any lazy-commit cleverness.
  volatile char *bytes = reinterpret_cast<volatile char *>(spare_.get());
  for (std::size_t off = 0; off < sizeof(OrderBook); off += 4096)
    bytes[off];
}

OrderBook &BookArray::ensure(uint16_t locate, uint64_t stock_id,
                             Price base_price) noexcept {
  auto &slot = books_[locate];
  if (!slot) {
    // Adopt the prewarmed book if available; otherwise allocate inline.
    slot = spare_ ? std::move(spare_) : std::make_unique<OrderBook>();
    slot->init(stock_id, base_price);
  }
  return *slot;
}

OrderBook *BookArray::get(uint16_t locate) noexcept {
  return books_[locate].get();
}

const OrderBook *BookArray::get(uint16_t locate) const noexcept {
  return books_[locate].get();
}
