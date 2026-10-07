#pragma once

#include "common/event.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exec {

// Prices and quantities are int64 ticks and lots of the frame's instrument (see
// ref::InstrumentTable).
struct Level {
  int64_t price;
  int64_t qty;
  uint32_t order_count;
  uint32_t _pad;
};
static_assert(sizeof(Level) == 24);

enum FrameFlags : uint8_t {
  TWO_SIDED = 1,
  SNAPSHOT = 2,   // first frame after a full book rebuild
  FEED_RESET = 4, // frames were dropped or the feed resynced before this one
  LAST_IN_BATCH = 8,
  CROSS = 16, // the trade is an auction cross print: trade_side is meaningless
};

inline constexpr int64_t EMPTY_BID = 0;
inline constexpr int64_t EMPTY_ASK = INT64_MAX;

// Resolved market-data frame published by a feed producer (core_main or the
// gateway MD thread) to the exec process.
template <std::size_t depth> struct alignas(64) MarketUpdate {
  uint64_t event_seq;  // producer-monotonic; a gap means frames were dropped
  uint64_t event_time; // ITCH ns since midnight, or venue ns since epoch
  uint64_t tsc_in;     // producer read_tsc() at ingest
  int64_t best_bid;    // EMPTY_BID / EMPTY_ASK when a side is empty
  int64_t best_ask;
  int64_t trade_price; // trade carried by this frame; trade_qty == 0 means none
  int64_t trade_qty;
  uint16_t instrument;
  common::Side trade_side;
  uint8_t flags; // FrameFlags
  int16_t nb;
  int16_t na;
  uint16_t _pad;
  Level bids[depth];
  Level asks[depth];
};

static_assert(std::is_trivially_copyable_v<MarketUpdate<1>>,
              "exec::MarketUpdate must stay trivially copyable for the ring");
static_assert(sizeof(MarketUpdate<10>) == 576);
static_assert(sizeof(MarketUpdate<5>) == 320);

} // namespace exec
