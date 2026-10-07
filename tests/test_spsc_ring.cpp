#include <cstdint>
#include <memory>
#include <thread>

#include "check.hpp"
#include "common/spsc_ring.hpp"

struct Item {
  uint64_t seq;
  uint64_t payload;
};

using Ring = common::SpscRing<Item, 8>;

static void test_fill_and_drain() {
  auto r = std::make_unique<Ring>();
  for (uint64_t i = 0; i < 8; ++i)
    CHECK(r->try_push({i, i * 3}));
  CHECK(!r->try_push({99, 0}));
  CHECK(r->try_claim() == nullptr);
  CHECK(r->size() == 8);

  Item out{};
  for (uint64_t i = 0; i < 8; ++i) {
    CHECK(r->try_pop(out));
    CHECK(out.seq == i && out.payload == i * 3);
  }
  CHECK(!r->try_pop(out));
  CHECK(r->size() == 0);
}

static void test_claim_publish() {
  auto r = std::make_unique<Ring>();
  Item *a = r->try_claim();
  CHECK(a != nullptr);
  CHECK(r->try_claim() == a); // unpublished claim is idempotent
  Item out{};
  CHECK(!r->try_pop(out)); // not visible before publish
  a->seq = 7;
  a->payload = 11;
  r->publish();
  CHECK(r->try_pop(out));
  CHECK(out.seq == 7 && out.payload == 11);
}

// head/tail are uint32 and wrap; full/empty must stay correct across the wrap.
static void test_wraparound() {
  auto r = std::make_unique<Ring>();
  const uint32_t start = UINT32_MAX - 3;
  r->head.store(start);
  r->tail.store(start);
  Item out{};
  for (uint64_t round = 0; round < 4; ++round) {
    for (uint64_t i = 0; i < 8; ++i)
      CHECK(r->try_push({round * 8 + i, 0}));
    CHECK(!r->try_push({0, 0}));
    CHECK(r->size() == 8);
    for (uint64_t i = 0; i < 8; ++i) {
      CHECK(r->try_pop(out));
      CHECK(out.seq == round * 8 + i);
    }
    CHECK(!r->try_pop(out));
  }
  CHECK(r->tail.load() < start); // actually wrapped
}

static void test_two_threads() {
  constexpr uint64_t N = 5'000'000;
  auto r = std::make_unique<common::SpscRing<Item, 1024>>();
  std::thread producer([&] {
    for (uint64_t i = 0; i < N; ++i) {
      Item *slot;
      while ((slot = r->try_claim()) == nullptr) {
      }
      slot->seq = i;
      slot->payload = ~i;
      r->publish();
    }
  });
  uint64_t bad = 0;
  Item out{};
  for (uint64_t i = 0; i < N;) {
    if (r->try_pop(out)) {
      if (out.seq != i || out.payload != ~i)
        ++bad;
      ++i;
    }
  }
  producer.join();
  CHECK(bad == 0);
  CHECK(!r->try_pop(out));
}

int main() {
  test_fill_and_drain();
  test_claim_publish();
  test_wraparound();
  test_two_threads();
  return check_summary();
}
