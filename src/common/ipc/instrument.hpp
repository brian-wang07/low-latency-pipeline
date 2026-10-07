#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ref {

enum InstrumentStatus : uint8_t {
  ONLINE = 0,
  POST_ONLY = 1,
  LIMIT_ONLY = 2,
  CANCEL_ONLY = 3,
  HALTED = 4,
};

// Static reference data for one tradable instrument. Prices and quantities on the
// wire are int64 multiples of the tick and lot: price = ticks * tick_mant *
// 10^tick_exp, likewise for lots.
struct alignas(64) Instrument {
  int64_t tick_mant, lot_mant;
  int64_t min_qty_lots;
  int64_t min_notional; // quote currency, 1e-8 units
  char symbol[16];      // venue native: "BTC-USD", "BTC/USD", or the ITCH ticker
  char venue[8];
  int8_t tick_exp, lot_exp;
  uint8_t price_precision, qty_precision; // decimal places for wire strings
  uint8_t status;                         // InstrumentStatus
  uint8_t _pad[3];
};
static_assert(sizeof(Instrument) == 64);
static_assert(std::is_trivially_copyable_v<Instrument>);

inline constexpr uint32_t MAX_INSTRUMENTS = 256;
inline constexpr uint16_t INVALID_INSTRUMENT = UINT16_MAX;

// Append-only, single writer (the feed producer, at startup); readers acquire
// count and may read rows below it.
struct alignas(64) InstrumentTable {
  std::atomic<uint32_t> count{0};
  uint32_t _pad;
  Instrument rows[MAX_INSTRUMENTS];

  uint16_t add(const Instrument &row) noexcept {
    const uint32_t n = count.load(std::memory_order_relaxed);
    if (n >= MAX_INSTRUMENTS)
      return INVALID_INSTRUMENT;
    rows[n] = row;
    count.store(n + 1, std::memory_order_release);
    return static_cast<uint16_t>(n);
  }

  uint16_t find(const char *symbol, const char *venue) const noexcept {
    const uint32_t n = count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
      if (std::strncmp(rows[i].symbol, symbol, sizeof(rows[i].symbol)) == 0 &&
          std::strncmp(rows[i].venue, venue, sizeof(rows[i].venue)) == 0)
        return static_cast<uint16_t>(i);
    return INVALID_INSTRUMENT;
  }

  uint32_t size() const noexcept {
    return count.load(std::memory_order_acquire);
  }
};
static_assert(sizeof(InstrumentTable) == 64 + 64 * MAX_INSTRUMENTS);

} // namespace ref
