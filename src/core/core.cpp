#include <cstdint>
#include <cstring>

#include "common/event.hpp"
#include "core.hpp"

using namespace core::equity;

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
  return price - base_price_;
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
  base_price_ = base_price;
  std::memset(levels_, 0, MAX_LEVELS * sizeof(PriceLevel));
}

template <uint32_t MaxLevels>
Price PriceLevelArray<MaxLevels>::base_price() const noexcept {
  return base_price_;
}

// Explicit instantiation for templated classes
template class core::equity::OrderMap<DEFAULT_ORDER_CAPACITY>;
template class core::equity::PriceLevelArray<DEFAULT_LEVEL_COUNT>;

// Orderbook
void OrderBook::rescan(common::Side side, Price price) noexcept {
  if (side == common::Side::Buy && price == tob_.best_bid) {
    while (bids_.in_range(tob_.best_bid) &&
           bids_.at(tob_.best_bid).order_count == 0)
      --tob_.best_bid;
  } else if (side == common::Side::Sell && price == tob_.best_ask) {
    while (asks_.in_range(tob_.best_ask) &&
           asks_.at(tob_.best_ask).order_count == 0)
      ++tob_.best_ask;
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
    if (!bids_.in_range(price))
      return;
    orders_.insert(ref, price, shares, side);
    PriceLevel &lvl = bids_.at(price);
    lvl.total_shares += shares;
    ++lvl.order_count;
    if (price > tob_.best_bid)
      tob_.best_bid = price;
  } else {
    if (!asks_.in_range(price))
      return;
    orders_.insert(ref, price, shares, side);
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares += shares;
    ++lvl.order_count;
    if (price < tob_.best_ask)
      tob_.best_ask = price;
  }
}

void OrderBook::on_execute(OrderRef ref, Qty executed_shares) noexcept {
  OrderEntry *entry = orders_.find(ref);
  if (!entry)
    return;
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
        rescan(common::Side::Buy, price);
    }
  } else {
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares -= executed_shares;
    entry->shares -= executed_shares;
    if (entry->shares == 0) {
      --lvl.order_count;
      orders_.erase(ref);
      if (lvl.order_count == 0)
        rescan(common::Side::Sell, price);
    }
  }
}

void OrderBook::on_execute_with_price(OrderRef ref, Qty executed_shares,
                                      Price /*execution_price*/) noexcept {
  on_execute(ref, executed_shares);
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
        rescan(common::Side::Buy, price);
    }
  } else {
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares -= cancelled_shares;
    entry->shares -= cancelled_shares;
    [[unlikely]] if (entry->shares == 0) {
      --lvl.order_count;
      orders_.erase(ref);
      if (lvl.order_count == 0)
        rescan(common::Side::Sell, price);
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
      rescan(common::Side::Buy, price);
  } else {
    PriceLevel &lvl = asks_.at(price);
    lvl.total_shares -= shares;
    --lvl.order_count;
    orders_.erase(ref);
    if (lvl.order_count == 0)
      rescan(common::Side::Sell, price);
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
      ++new_lvl.order_count;
      if (new_price > tob_.best_bid)
        tob_.best_bid = new_price;
    }
    if (old_lvl.order_count == 0)
      rescan(common::Side::Buy, old_price);
  } else {
    PriceLevel &old_lvl = asks_.at(old_price);
    old_lvl.total_shares -= old_shares;
    --old_lvl.order_count;
    orders_.erase(old_ref);
    if (asks_.in_range(new_price)) {
      orders_.insert(new_ref, new_price, new_shares, side);
      PriceLevel &new_lvl = asks_.at(new_price);
      new_lvl.total_shares += new_shares;
      ++new_lvl.order_count;
      if (new_price < tob_.best_ask)
        tob_.best_ask = new_price;
    }
    if (old_lvl.order_count == 0)
      rescan(common::Side::Sell, old_price);
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

int OrderBook::top_bids(Level *out, int max_out) const noexcept {
  int n = 0;
  Price p = tob_.best_bid;
  if (p == 0)
    return 0;
  while (n < max_out && bids_.in_range(p)) {
    const PriceLevel &lvl = bids_.at(p);
    if (lvl.order_count > 0)
      out[n++] = {p, lvl.total_shares};
    if (p == 0)
      break;
    --p;
  }
  return n;
}

int OrderBook::top_asks(Level *out, int max_out) const noexcept {
  int n = 0;
  Price p = tob_.best_ask;
  if (p == UINT32_MAX)
    return 0;
  while (n < max_out && asks_.in_range(p)) {
    const PriceLevel &lvl = asks_.at(p);
    if (lvl.order_count > 0)
      out[n++] = {p, lvl.total_shares};
    ++p;
  }
  return n;
}

uint64_t OrderBook::stock_id() const noexcept { return stock_id_; }
bool OrderBook::initialized() const noexcept { return initialized_; }

// BookArray
OrderBook &BookArray::ensure(uint16_t locate, uint64_t stock_id,
                             Price base_price) noexcept {
  auto &slot = books_[locate];
  if (!slot) {
    slot = std::make_unique<OrderBook>();
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
