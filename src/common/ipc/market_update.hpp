#pragma once

#include "common/event.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exec {

struct Level {
  uint32_t price;
  uint32_t shares;
  uint32_t order_count;
};

// Resolved market-data frame published by core (the feed handler) to the
// exec process
template <std::size_t depth> struct alignas(64) MarketUpdate {
  uint64_t event_seq;
  uint64_t event_time;
  uint64_t tsc_in;
  char stock_id[8];

  uint32_t best_bid;
  uint32_t best_ask;

  // Resolved trade carried by this update, if any. trade_size == 0 means there
  // was no trade on this update
  uint32_t trade_price;
  uint32_t trade_size;
  common::Side trade_side;

  int32_t nb;
  int32_t na;
  Level bids[depth];
  Level asks[depth];
};

static_assert(std::is_trivially_copyable_v<MarketUpdate<1>>,
              "exec::MarketUpdate must stay trivially copyable for the ring");

} // namespace exec
