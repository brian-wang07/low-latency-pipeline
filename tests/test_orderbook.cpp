#include <cassert>
#include <cstdio>
#include <memory>

#include "common/event.hpp"
#include "core/core.hpp"

using namespace core;
using common::Side;

static int passed = 0;
static int failed = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (cond) {                                                                \
      ++passed;                                                                \
    } else {                                                                   \
      ++failed;                                                                \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                          \
  } while (0)

// Fixed-point prices (4 decimal places). BASE must be <= all prices used.
static constexpr Price BASE = 999900;  // 99.9900 - base, anchors the level array
static constexpr Price BID2 = 999900;  // 99.9900
static constexpr Price BID1 = 1000000; // 100.0000
static constexpr Price ASK1 = 1000100; // 100.0100
static constexpr Price ASK2 = 1000200; // 100.0200

static std::unique_ptr<OrderBook> make_book() {
  auto b = std::make_unique<OrderBook>();
  b->init(1, BASE);
  return b;
}

static void test_add_single_bid() {
  auto b = make_book();
  b->on_add(1, Side::Buy, BID1, 100);
  CHECK(b->best_bid() == BID1);
  CHECK(b->qty_at_bid(BID1) == 100);
}

static void test_add_single_ask() {
  auto b = make_book();
  b->on_add(1, Side::Sell, ASK1, 200);
  CHECK(b->best_ask() == ASK1);
  CHECK(b->qty_at_ask(ASK1) == 200);
}

static void test_best_bid_tracks_highest() {
  auto b = make_book();
  b->on_add(1, Side::Buy, BID2, 100);
  b->on_add(2, Side::Buy, BID1, 100);
  CHECK(b->best_bid() == BID1);
}

static void test_best_ask_tracks_lowest() {
  auto b = make_book();
  b->on_add(1, Side::Sell, ASK2, 100);
  b->on_add(2, Side::Sell, ASK1, 100);
  CHECK(b->best_ask() == ASK1);
}

static void test_cancel_partial() {
  auto b = make_book();
  b->on_add(1, Side::Buy, BID1, 100);
  b->on_cancel(1, 40);
  CHECK(b->qty_at_bid(BID1) == 60);
  CHECK(b->best_bid() == BID1);
}

static void test_delete_order() {
  auto b = make_book();
  b->on_add(1, Side::Buy, BID1, 100);
  b->on_add(2, Side::Buy, BID2, 50);
  b->on_delete(1);
  CHECK(b->qty_at_bid(BID1) == 0);
  CHECK(b->best_bid() == BID2);
}

static void test_execute_full() {
  auto b = make_book();
  b->on_add(1, Side::Sell, ASK1, 100);
  b->on_add(2, Side::Sell, ASK2, 50);
  b->on_execute(1, 100);
  CHECK(b->qty_at_ask(ASK1) == 0);
  CHECK(b->best_ask() == ASK2);
}

static void test_execute_partial() {
  auto b = make_book();
  b->on_add(1, Side::Sell, ASK1, 100);
  b->on_execute(1, 60);
  CHECK(b->qty_at_ask(ASK1) == 40);
  CHECK(b->best_ask() == ASK1);
}

static void test_replace_order() {
  auto b = make_book();
  b->on_add(1, Side::Buy, BID2, 100);
  b->on_replace(1, 2, BID1, 50);
  CHECK(b->qty_at_bid(BID2) == 0);
  CHECK(b->qty_at_bid(BID1) == 50);
  CHECK(b->best_bid() == BID1);
}

static void test_multiple_orders_same_level() {
  auto b = make_book();
  b->on_add(1, Side::Buy, BID1, 100);
  b->on_add(2, Side::Buy, BID1, 200);
  CHECK(b->qty_at_bid(BID1) == 300);
  b->on_delete(1);
  CHECK(b->qty_at_bid(BID1) == 200);
  CHECK(b->best_bid() == BID1);
}

static void test_execute_with_price() {
  auto b = make_book();
  b->on_add(1, Side::Sell, ASK1, 100);
  b->on_execute_with_price(1, 100, ASK1);
  CHECK(b->qty_at_ask(ASK1) == 0);
  CHECK(b->best_ask() > ASK1);
}

int main() {
  test_add_single_bid();
  test_add_single_ask();
  test_best_bid_tracks_highest();
  test_best_ask_tracks_lowest();
  test_cancel_partial();
  test_delete_order();
  test_execute_full();
  test_execute_partial();
  test_replace_order();
  test_multiple_orders_same_level();
  test_execute_with_price();

  printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
