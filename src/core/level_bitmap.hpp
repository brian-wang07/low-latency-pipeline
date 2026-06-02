#pragma once

#include <cstdint>

namespace core::equity {

// Hierarchical (radix-64) occupancy bitmap over a dense, fixed range of bits.
// In the order book one bit per price level marks "this level has live orders".
// The payoff is next_set / prev_set: jump straight to the nearest occupied
// level in O(tiers) word loads, so top_bids / top_asks / on_level_emptied skip
// empty levels structurally instead of walking them one tick at a time.
//
// Each tier keeps one summary bit per 64-bit word of the tier below it, making
// the structure a tree of fan-out 64. It is defined recursively: an interior
// node owns ceil(N/64) words plus a summary OccupancyBitmap over those words;
// the leaf (N <= 64) is a single word. For N = 65536 that is three tiers
// (1024 -> 16 -> 1 words, ~8 KB); widening the level array simply grows one
// more tier at compile time.
//
// Invariant: a summary bit is set iff its word is non-empty. next_set/prev_set
// lean on it to descend straight into a non-empty word.

inline constexpr uint32_t BITMAP_NPOS = UINT32_MAX;

template <uint32_t NBits, bool IsLeaf = (NBits <= 64)> struct OccupancyBitmap;

// Leaf: a single word covers up to 64 bits, no summary below it.
template <uint32_t NBits> struct OccupancyBitmap<NBits, true> {
  uint64_t word{0};

  void reset() noexcept { word = 0; }
  void set(uint32_t i) noexcept { word |= uint64_t{1} << i; }
  void clear(uint32_t i) noexcept { word &= ~(uint64_t{1} << i); }

  // Lowest set bit >= i, or BITMAP_NPOS if none.
  uint32_t next_set(uint32_t i) const noexcept {
    if (i >= 64)
      return BITMAP_NPOS;
    uint64_t m = word & (~uint64_t{0} << i);
    return m ? static_cast<uint32_t>(__builtin_ctzll(m)) : BITMAP_NPOS;
  }

  // Highest set bit <= i, or BITMAP_NPOS if none.
  uint32_t prev_set(uint32_t i) const noexcept {
    uint64_t m = (i >= 63) ? word : word & ((uint64_t{1} << (i + 1)) - 1);
    return m ? 63u - static_cast<uint32_t>(__builtin_clzll(m)) : BITMAP_NPOS;
  }
};

// Interior: NWORDS words, summarized one-bit-per-word by the tier above.
template <uint32_t NBits> struct OccupancyBitmap<NBits, false> {
  static constexpr uint32_t NWORDS = (NBits + 63) / 64;
  uint64_t words[NWORDS]{};
  OccupancyBitmap<NWORDS> summary;

  void reset() noexcept {
    for (uint64_t &w : words)
      w = 0;
    summary.reset();
  }

  void set(uint32_t i) noexcept {
    words[i >> 6] |= uint64_t{1} << (i & 63);
    summary.set(i >> 6); // word is now non-empty
  }

  void clear(uint32_t i) noexcept {
    uint32_t w = i >> 6;
    words[w] &= ~(uint64_t{1} << (i & 63));
    if (words[w] == 0)
      summary.clear(w); // word drained: drop it from the summary
  }

  uint32_t next_set(uint32_t i) const noexcept {
    uint32_t w = i >> 6;
    if (w >= NWORDS)
      return BITMAP_NPOS;
    uint64_t m = words[w] & (~uint64_t{0} << (i & 63));
    if (m)
      return (w << 6) | static_cast<uint32_t>(__builtin_ctzll(m));
    w = summary.next_set(w + 1); // next non-empty word
    if (w == BITMAP_NPOS)
      return BITMAP_NPOS;
    return (w << 6) | static_cast<uint32_t>(__builtin_ctzll(words[w]));
  }

  uint32_t prev_set(uint32_t i) const noexcept {
    uint32_t w = i >> 6;
    uint32_t b = i & 63;
    uint64_t m =
        (b == 63) ? words[w] : words[w] & ((uint64_t{1} << (b + 1)) - 1);
    if (m)
      return (w << 6) | (63u - static_cast<uint32_t>(__builtin_clzll(m)));
    if (w == 0)
      return BITMAP_NPOS;
    w = summary.prev_set(w - 1); // previous non-empty word
    if (w == BITMAP_NPOS)
      return BITMAP_NPOS;
    return (w << 6) | (63u - static_cast<uint32_t>(__builtin_clzll(words[w])));
  }
};

} // namespace core::equity
