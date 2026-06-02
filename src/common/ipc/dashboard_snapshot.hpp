#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dashboard {

// Prices arrive as uint32 4-decimal fixed point (scaled by 10000)
inline constexpr double PRICE_SCALE = 10000.0;

inline constexpr double to_display(uint32_t fixed) noexcept {
  return static_cast<double>(fixed) / PRICE_SCALE;
}

struct Level {
  uint32_t price;
  uint32_t shares;
  uint32_t order_count;
};

template <std::size_t depth> struct alignas(64) Snapshot {
  uint64_t event_seq;
  char stock_id[8];
  uint32_t best_bid;
  uint32_t best_ask;
  uint32_t spread;
  uint64_t total_bid_qty;
  uint64_t total_ask_qty;
  int32_t nb;
  int32_t na;
  Level bids[depth];
  Level asks[depth];

  double vwmid;
  double imbalance;
  double ema;
  double tick_rate;

  double latency_p99_ns;
  double latency_p999_ns;
  double ring_occupancy;
};

static_assert(std::is_trivially_copyable_v<Snapshot<1>>,
              "dashboard::Snapshot must stay trivially copyable for the ring");

} // namespace dashboard
