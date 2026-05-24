#include <cassert>
#include <cstdint>
#include <cstring>

#include "common/event.hpp"
#include "core.hpp"

using namespace core;

OrderEntry *OrderMap::find(OrderRef ref) noexcept {
  // if we hit capacity, we are cooked
  for (uint32_t pos = hash(ref);; ++pos) {
    Slot &s = slots_[pos & MASK];
    if (!s.occupied)
      return nullptr;
    if (s.key == ref)
      return &s.entry;
  }
}

void OrderMap::insert(OrderRef ref, Price price, Qty shares,
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

void OrderMap::erase(OrderRef ref) noexcept {
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

uint32_t PriceLevelArray::index_of(Price price) const noexcept {
  return price - base_price_;
}

bool PriceLevelArray::in_range(Price price) const noexcept {
  return index_of(price) < MAX_LEVELS;
}

PriceLevel &PriceLevelArray::at(Price price) noexcept {
  return levels_[index_of(price)];
}

const PriceLevel &PriceLevelArray::at(Price price) const noexcept {
  return levels_[index_of(price)];
}

void PriceLevelArray::init(Price base_price) noexcept {
  base_price_ = base_price;
  std::memset(levels_, 0, MAX_LEVELS * sizeof(PriceLevel));
}

Price PriceLevelArray::base_price() const noexcept { return base_price_; }

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
  orders_.insert(ref, price, shares, side);
  if (side == common::Side::Buy) {
    bids_.at(price).total_shares += shares;
    bids_.at(price).order_count++;
    if (price > tob_.best_bid)
      tob_.best_bid = price;
  } else {
    asks_.at(price).total_shares += shares;
    asks_.at(price).order_count++;
    if (price < tob_.best_ask)
      tob_.best_ask = price;
  }
}

void OrderBook::on_execute(OrderRef ref, Qty executed_shares) noexcept {
  OrderEntry *entry = orders_.find(ref);
  assert(entry != nullptr && "Execute message on a missing order!");
  Price price = entry->price;
  common::Side side = entry->side;
  // TODO: Maybe change interface to be able to select bid/ask, so i dont have
  // to write the same thing twice everytime?
  if (side == common::Side::Buy) {
    PriceLevel &level = bids_.at(price);
    level.total_shares -= executed_shares;
    entry->shares -= executed_shares;
    if (entry->shares == 0) {
      --level.order_count;
      orders_.erase(ref);
      if (level.order_count == 0)
        rescan(common::Side::Buy, price);
    }
  } else {
    PriceLevel &level = asks_.at(price);
    level.total_shares -= executed_shares;
    entry->shares -= executed_shares;
    if (entry->shares == 0) {
      --level.order_count;
      orders_.erase(ref);
      if (level.order_count == 0)
        rescan(common::Side::Sell, price);
    }
  }
}

void OrderBook::on_execute_with_price(OrderRef ref, Qty executed_shares,
                                      Price execution_price) noexcept {
  on_execute(ref, executed_shares);
}

void OrderBook::on_cancel(OrderRef ref, Qty cancelled_shares) noexcept {
  OrderEntry *entry = orders_.find(ref);
  Price price = entry->price;
  common::Side side = entry->side;
  if (side == common::Side::Buy) {
    PriceLevel &level = bids_.at(price);
    level.total_shares -= cancelled_shares;
    entry->shares -= cancelled_shares;
    // ITCH Cancel will only ever partially cancel an order; this should never
    // be dispatched, but good to check
    [[unlikely]]
    if (entry->shares == 0) {
      --level.order_count;
      orders_.erase(ref);
      if (level.order_count == 0)
        rescan(common::Side::Buy, price);
    }
  } else {
    PriceLevel &level = asks_.at(price);
    level.total_shares -= cancelled_shares;
    entry->shares -= cancelled_shares;
    [[unlikely]]
    if (entry->shares == 0) {
      --level.order_count;
      orders_.erase(ref);
      if (level.order_count == 0)
        rescan(common::Side::Sell, price);
    }
  }
}

void OrderBook::on_delete(OrderRef ref) noexcept {
  OrderEntry *entry = orders_.find(ref);
  Price price = entry->price;
  Qty shares = entry->shares;
  common::Side side = entry->side;
  if (side == common::Side::Buy) {
    PriceLevel &level = bids_.at(price);
    level.total_shares -= shares;
    --level.order_count;
    orders_.erase(ref);
    if (level.order_count == 0)
      rescan(common::Side::Buy, price);
  } else {
    PriceLevel &level = asks_.at(price);
    level.total_shares -= shares;
    --level.order_count;
    orders_.erase(ref);
    if (level.order_count == 0)
      rescan(common::Side::Sell, price);
  }
}

void OrderBook::on_replace(OrderRef old_ref, OrderRef new_ref, Price new_price,
                           Qty new_shares) noexcept {
  OrderEntry *old_entry = orders_.find(old_ref);
  Price old_price = old_entry->price;
  Qty old_shares = old_entry->shares;
  common::Side side = old_entry->side;
  if (side == common::Side::Buy) {
    PriceLevel &old_level = bids_.at(old_price);
    old_level.total_shares -= old_shares;
    --old_level.order_count;
    orders_.erase(old_ref);
    orders_.insert(new_ref, new_price, new_shares, side);
    PriceLevel &new_level = bids_.at(new_price);
    new_level.total_shares += new_shares;
    ++new_level.order_count;
    if (old_level.order_count == 0)
      rescan(common::Side::Buy, old_price);
    if (new_price > tob_.best_bid)
      tob_.best_bid = new_price;
  } else {
    PriceLevel &old_level = asks_.at(old_price);
    old_level.total_shares -= old_shares;
    --old_level.order_count;
    orders_.erase(old_ref);
    orders_.insert(new_ref, new_price, new_shares, side);
    PriceLevel &new_level = asks_.at(new_price);
    new_level.total_shares += new_shares;
    ++new_level.order_count;
    if (old_level.order_count == 0)
      rescan(common::Side::Sell, old_price);
    if (new_price < tob_.best_ask)
      tob_.best_ask = new_price;
  }
}

Price OrderBook::best_bid() const noexcept { return tob_.best_bid; }

Price OrderBook::best_ask() const noexcept { return tob_.best_ask; }

Qty OrderBook::qty_at_bid(const Price p) const noexcept {
  return bids_.at(p).total_shares;
}

Qty OrderBook::qty_at_ask(const Price p) const noexcept {
  return asks_.at(p).total_shares;
}

uint64_t OrderBook::stock_id() const noexcept { return stock_id_; }

bool OrderBook::initialized() const noexcept { return initialized_; }