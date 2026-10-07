#include <cstring>
#include <memory>

#include "check.hpp"
#include "exec/account.hpp"

using common::Side;
using exec::Money;

static constexpr Money USD = 100'000'000; // 1e-8 units per dollar
static constexpr int64_t PX = 10'000;     // ITCH ticks per dollar (tick 1e-4)

static ref::Instrument itch_row(const char *sym) {
  ref::Instrument r{};
  std::strcpy(r.symbol, sym);
  std::strcpy(r.venue, "ITCH");
  r.tick_mant = 1;
  r.tick_exp = -4;
  r.lot_mant = 1;
  r.lot_exp = 0;
  return r;
}

static std::unique_ptr<exec::Account> itch_account() {
  auto a = std::make_unique<exec::Account>();
  CHECK(a->add_instrument(0, itch_row("NVDA")));
  a->set_balance("USD", 100'000 * USD);
  a->set_balance("NVDA", 1'000 * USD); // 1000 shares, 1e8 units per share
  return a;
}

static void test_split_symbol() {
  char b[8], q[8];
  exec::split_symbol("BTC/USD", b, q);
  CHECK(!std::strcmp(b, "BTC") && !std::strcmp(q, "USD"));
  exec::split_symbol("ETH-CAD", b, q);
  CHECK(!std::strcmp(b, "ETH") && !std::strcmp(q, "CAD"));
  exec::split_symbol("NVDA", b, q);
  CHECK(!std::strcmp(b, "NVDA") && !std::strcmp(q, "USD"));
}

static void test_notional_units() {
  auto a = itch_account();
  CHECK(a->notional(0, 100 * PX, 10) == 1'000 * USD);
  CHECK(a->base_units(0, 10) == 10 * USD);

  // Kraken BTC/USD: price_increment 0.1, qty in 1e-8 BTC.
  ref::Instrument k{};
  std::strcpy(k.symbol, "BTC/USD");
  k.tick_mant = 1;
  k.tick_exp = -1;
  k.lot_mant = 1;
  k.lot_exp = -8;
  CHECK(a->add_instrument(1, k));
  CHECK(a->notional(1, 600'000, 100'000'000) == 60'000 * USD); // 1 BTC at 60000.0
  CHECK(a->notional(1, 600'000, 1'000) == 60'000 * USD / 100'000);
  CHECK(a->base_units(1, 1'000) == 1'000);
}

static void test_round_trip_and_average_cost() {
  auto a = itch_account();
  a->on_fill(0, Side::Buy, 100 * PX, 10, 100 * PX, 0, false);
  a->on_fill(0, Side::Buy, 102 * PX, 10, 102 * PX, 0, false);
  CHECK(a->position(0).qty == 20);
  CHECK(a->avg_px(0) == 101 * PX);
  a->on_fill(0, Side::Sell, 103 * PX, 5, 103 * PX, 0, false);
  CHECK(a->position(0).qty == 15);
  CHECK(a->realized(0) == 10 * USD); // (103 - 101) * 5
  CHECK(a->avg_px(0) == 101 * PX);
  a->on_fill(0, Side::Sell, 101 * PX, 15, 101 * PX, 0, false);
  CHECK(a->position(0).qty == 0);
  CHECK(a->realized(0) == 10 * USD);
  CHECK(a->quote_balance(0).total == (100'000 + 10) * USD);
  CHECK(a->base_balance(0).total == 1'000 * USD);
  CHECK(a->position(0).bought == 20 && a->position(0).sold == 20);
}

static void test_flip_through_zero() {
  auto a = itch_account();
  a->on_fill(0, Side::Buy, 100 * PX, 10, 100 * PX, 0, false);
  a->on_fill(0, Side::Sell, 99 * PX, 15, 99 * PX, 0, false);
  CHECK(a->position(0).qty == -5);
  CHECK(a->realized(0) == -10 * USD); // (99 - 100) * 10
  CHECK(a->avg_px(0) == 99 * PX);
  a->on_fill(0, Side::Buy, 98 * PX, 5, 98 * PX, 0, false);
  CHECK(a->position(0).qty == 0);
  CHECK(a->realized(0) == -5 * USD); // + (99 - 98) * 5
}

static void test_fees_and_marks() {
  auto a = itch_account();
  a->on_fill(0, Side::Buy, 100 * PX, 10, 100 * PX, USD / 2, false);
  CHECK(a->position(0).fees == USD / 2);
  CHECK(a->quote_balance(0).total == (100'000 - 1'000) * USD - USD / 2);

  a->mark(0, 101 * PX, 103 * PX); // mid 102
  CHECK(a->unrealized(0) == 20 * USD);
  CHECK(a->equity() == 20 * USD - USD / 2);
  CHECK(a->peak_equity() == 20 * USD - USD / 2);

  a->mark(0, 98 * PX, 98 * PX + 1); // mid 98.00005
  CHECK(a->unrealized(0) == -20 * USD + USD / 2000);
  CHECK(a->max_drawdown() == 40 * USD - USD / 2000);

  a->mark(0, exec::EMPTY_BID, 99 * PX); // one-sided: ignored
  CHECK(a->unrealized(0) == -20 * USD + USD / 2000);

  // Base-currency fee: 0.1 share at a 100.00 fill is $10.
  auto b = itch_account();
  b->on_fill(0, Side::Buy, 100 * PX, 10, 100 * PX, USD / 10, true);
  CHECK(b->base_balance(0).total == 1'010 * USD - USD / 10);
  CHECK(b->position(0).fees == 10 * USD);
}

static void test_reservations() {
  auto a = itch_account();
  a->reserve(0, Side::Buy, 100 * PX, 10);
  CHECK(a->quote_balance(0).reserved == 1'000 * USD);
  CHECK(a->quote_balance(0).available() == 99'000 * USD);
  a->reserve(0, Side::Sell, 101 * PX, 7);
  CHECK(a->base_balance(0).reserved == 7 * USD);

  // A fill below the limit releases the limit's reservation for the filled qty.
  a->on_fill(0, Side::Buy, 99 * PX, 4, 100 * PX, 0, false);
  CHECK(a->quote_balance(0).reserved == 600 * USD);
  a->release(0, Side::Buy, 100 * PX, 6);
  CHECK(a->quote_balance(0).reserved == 0);
  a->release(0, Side::Sell, 101 * PX, 7);
  CHECK(a->base_balance(0).reserved == 0);
}

static void test_snapshots() {
  auto a = itch_account();
  a->on_fill(0, Side::Buy, 100 * PX, 3, 100 * PX, 0, false);
  stats::PositionStats ps{};
  a->snapshot(ps);
  CHECK(ps.count == 1);
  CHECK(ps.rows[0].qty_lots == 3 && ps.rows[0].avg_px_ticks == 100 * PX);
  stats::BalanceStats bs{};
  a->snapshot(bs);
  CHECK(bs.count == 2);
  CHECK(!std::strcmp(bs.assets[0].name, "NVDA"));
  CHECK(bs.assets[0].total == 1'003 * USD);
}

int main() {
  test_split_symbol();
  test_notional_units();
  test_round_trip_and_average_cost();
  test_flip_through_zero();
  test_fees_and_marks();
  test_reservations();
  test_snapshots();
  return check_summary();
}
