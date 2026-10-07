#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dashboard {

// Prices are int64 ticks of the snapshot's instrument; a tick is 10^price_exp
// in display units (ITCH: -4).
inline double to_display(int64_t ticks, int8_t price_exp) noexcept {
  double scale = 1.0;
  for (int e = price_exp; e < 0; ++e)
    scale /= 10.0;
  for (int e = price_exp; e > 0; --e)
    scale *= 10.0;
  return static_cast<double>(ticks) * scale;
}

struct Level {
  int64_t price;
  int64_t qty;
  uint32_t order_count;
  uint32_t _pad;
};

template <std::size_t depth> struct alignas(64) Snapshot {
  uint64_t event_seq;
  int64_t best_bid; // 0 when the side is empty
  int64_t best_ask; // 0 when the side is empty
  int64_t spread;   // 0 unless both sides are present
  int64_t total_bid_qty;
  int64_t total_ask_qty;
  uint16_t instrument;
  int8_t price_exp;
  uint8_t _pad;
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
