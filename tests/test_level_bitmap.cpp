#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "core/level_bitmap.hpp"
#include "check.hpp"

using namespace core::equity;

// Linear-scan reference the bitmap must match exactly.
static uint32_t oracle_next(const std::vector<bool> &bits, uint32_t i) {
  for (uint32_t k = i; k < bits.size(); ++k)
    if (bits[k])
      return k;
  return BITMAP_NPOS;
}
static uint32_t oracle_prev(const std::vector<bool> &bits, uint32_t i) {
  for (uint32_t k = i + 1; k-- > 0;)
    if (bits[k])
      return k;
  return BITMAP_NPOS;
}

// Randomized set/clear, comparing next_set/prev_set to the oracle after each op.
template <uint32_t N> static void test_random(unsigned seed, int steps) {
  auto bm = std::make_unique<OccupancyBitmap<N>>();
  std::vector<bool> oracle(N, false);
  std::mt19937 rng(seed);
  std::uniform_int_distribution<uint32_t> idx(0, N - 1);

  for (int s = 0; s < steps; ++s) {
    uint32_t i = idx(rng);
    if (oracle[i]) {
      bm->clear(i);
      oracle[i] = false;
    } else {
      bm->set(i);
      oracle[i] = true;
    }
    for (int q = 0; q < 3; ++q) {
      uint32_t qi = idx(rng);
      CHECK(bm->next_set(qi) == oracle_next(oracle, qi));
      CHECK(bm->prev_set(qi) == oracle_prev(oracle, qi));
    }
  }
}

template <uint32_t N> static void test_boundaries() {
  auto bm = std::make_unique<OccupancyBitmap<N>>();

  // Empty: no set bit in either direction.
  CHECK(bm->next_set(0) == BITMAP_NPOS);
  CHECK(bm->prev_set(N - 1) == BITMAP_NPOS);

  // First bit.
  bm->set(0);
  CHECK(bm->next_set(0) == 0);
  CHECK(bm->prev_set(N - 1) == 0);
  CHECK(bm->next_set(1) == BITMAP_NPOS);
  bm->clear(0);
  CHECK(bm->next_set(0) == BITMAP_NPOS);

  // Last bit.
  bm->set(N - 1);
  CHECK(bm->next_set(0) == N - 1);
  CHECK(bm->prev_set(N - 1) == N - 1);
  if (N > 1)
    CHECK(bm->prev_set(N - 2) == BITMAP_NPOS);
  bm->clear(N - 1);

  // Adjacent bits straddling a 64-bit word boundary.
  if (N > 64) {
    bm->set(63);
    bm->set(64);
    CHECK(bm->next_set(0) == 63);
    CHECK(bm->next_set(64) == 64);
    CHECK(bm->prev_set(63) == 63);
    CHECK(bm->prev_set(N - 1) == 64);
    CHECK(bm->next_set(65) == BITMAP_NPOS);
  }
}

int main() {
  test_boundaries<64>();
  test_boundaries<128>();
  test_boundaries<4096>();
  test_boundaries<65536>();

  test_random<64>(1, 20000);
  test_random<128>(2, 20000);
  test_random<4096>(3, 20000);
  test_random<65536>(4, 3000); // fewer steps: oracle scan is O(N)

  return check_summary();
}
