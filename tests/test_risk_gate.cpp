#include <cstring>
#include <memory>

#include "check.hpp"
#include "exec/risk.hpp"

using common::Side;
using exec::Money;
using exec::OrderCheck;
using exec::RejectReason;

static constexpr Money USD = 100'000'000;
static constexpr int64_t PX = 10'000; // ticks per dollar
static constexpr int64_t NOW = 3'600 * exec::SEC_NS;

struct Fixture {
  ref::Instrument inst{};
  std::unique_ptr<exec::Account> acct = std::make_unique<exec::Account>();
  std::unique_ptr<exec::OrderTable> orders = std::make_unique<exec::OrderTable>();
  std::unique_ptr<exec::RateModel> rate = std::make_unique<exec::RateModel>();
  exec::RiskGate gate;
  uint32_t control = 0;

  Fixture() {
    std::strcpy(inst.symbol, "NVDA");
    std::strcpy(inst.venue, "ITCH");
    inst.tick_mant = 1;
    inst.tick_exp = -4;
    inst.lot_mant = 1;
    acct->add_instrument(0, inst);
    acct->set_balance("USD", 10'000 * USD);
    acct->set_balance("NVDA", 50 * USD); // 50 shares
    gate.set_allowed_assets("NVDA,TQQQ");
    gate.register_instrument(0, inst);
    rate->configure(true, {.max = 20, .decay_per_s = 1, .max_open = 60, .margin = 0});
  }

  OrderCheck order(Side side, int64_t px, int64_t qty) const {
    return {0, side, oe::OrdType::Limit, oe::Tif::GTC, px, qty, 200 * PX, NOW};
  }
  RejectReason check(const OrderCheck &o) const {
    return gate.check_new(o, inst, *acct, *orders, *rate, control);
  }
  void rest(uint64_t id, Side side, int64_t px, int64_t qty) {
    exec::OrderRow r{};
    r.id = id;
    r.side = side;
    r.px = px;
    r.qty = r.leaves = qty;
    r.t_ack_ns = NOW;
    orders->insert(r);
  }
};

static void test_accepts_and_controls() {
  Fixture f;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::None);
  CHECK(f.check(f.order(Side::Sell, 100 * PX, 10)) == RejectReason::None);
  f.control = ipc::PAUSE;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::Pause);
  f.control = ipc::KILL | ipc::PAUSE;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::Kill);
}

static void test_instrument_checks() {
  Fixture f;
  auto o = f.order(Side::Buy, 100 * PX, 10);
  o.instrument = 5;
  CHECK(f.check(o) == RejectReason::NotTraded);

  f.gate.set_allowed_assets("BTC,ETH");
  f.gate.register_instrument(0, f.inst);
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::NotAllowed);
  CHECK(f.gate.asset_allowed("ETH") && !f.gate.asset_allowed("ET"));
  f.gate.set_allowed_assets("NVDA");
  f.gate.register_instrument(0, f.inst);

  f.inst.status = ref::HALTED;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::InstrumentStatus);
  f.inst.status = ref::POST_ONLY;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::InstrumentStatus);
  auto po = f.order(Side::Buy, 100 * PX, 10);
  po.tif = oe::Tif::PostOnly;
  CHECK(f.check(po) == RejectReason::None);
  f.inst.status = ref::LIMIT_ONLY;
  auto mkt = f.order(Side::Buy, 0, 10);
  mkt.ord_type = oe::OrdType::Market;
  CHECK(f.check(mkt) == RejectReason::InstrumentStatus);
  f.inst.status = ref::ONLINE;
  CHECK(f.check(mkt) == RejectReason::None); // priced at mid for the checks

  CHECK(f.check(f.order(Side::Buy, 100 * PX, 0)) == RejectReason::BadOrder);
  CHECK(f.check(f.order(Side::Buy, 0, 10)) == RejectReason::BadOrder);

  f.inst.min_qty_lots = 20;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::MinQty);
  f.inst.min_qty_lots = 0;
  f.inst.min_notional = 2'000 * USD;
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 10)) == RejectReason::MinNotional);
}

static void test_limits() {
  Fixture f;
  f.gate.set_limits({.max_order_qty = 5});
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 6)) == RejectReason::MaxOrderQty);
  f.gate.set_limits({.max_order_notional = 500 * USD});
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 6)) == RejectReason::MaxOrderNotional);
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 5)) == RejectReason::None);

  // Position limits count open orders on the same side as if they filled.
  f.gate.set_limits({.max_position = 30, .spot_only = false});
  f.rest(1, Side::Buy, 100 * PX, 25);
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 6)) == RejectReason::MaxPosition);
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 5)) == RejectReason::None);
  CHECK(f.check(f.order(Side::Sell, 100 * PX, 30)) == RejectReason::None);
  CHECK(f.check(f.order(Side::Sell, 100 * PX, 31)) == RejectReason::MaxPosition);
  f.gate.set_limits({.max_position_notional = 2'000 * USD, .spot_only = false});
  CHECK(f.check(f.order(Side::Buy, 50 * PX, 16)) == RejectReason::MaxPositionNotional);
  CHECK(f.check(f.order(Side::Buy, 50 * PX, 15)) == RejectReason::None);

  f.gate.set_limits({.max_open_orders = 1, .spot_only = false});
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 1)) == RejectReason::MaxOpenOrders);

  f.gate.set_limits({.spot_only = false});
  f.rate->configure(true, {.max = 20, .decay_per_s = 1, .max_open = 1, .margin = 0});
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 1)) == RejectReason::OpenOrderLimit);
}

static void test_band_and_spot() {
  Fixture f;
  f.gate.set_limits({.price_band_bp = 50, .spot_only = true});
  // The fixture's book is 100.00 mid (mid2 = bid + ask = 200.00).
  CHECK(f.check(f.order(Side::Buy, 101 * PX, 1)) == RejectReason::PriceBand); // +100bp
  CHECK(f.check(f.order(Side::Buy, 100 * PX + 49 * 100, 1)) == RejectReason::None);
  CHECK(f.check(f.order(Side::Sell, 99 * PX, 1)) == RejectReason::PriceBand);

  f.gate.set_limits({.spot_only = true});
  CHECK(f.check(f.order(Side::Buy, 200 * PX, 51)) == RejectReason::InsufficientQuote);
  CHECK(f.check(f.order(Side::Buy, 200 * PX, 50)) == RejectReason::None);
  CHECK(f.check(f.order(Side::Sell, 200 * PX, 51)) == RejectReason::InsufficientBase);
  f.acct->reserve(0, Side::Sell, 200 * PX, 45);
  CHECK(f.check(f.order(Side::Sell, 200 * PX, 6)) == RejectReason::InsufficientBase);
}

static void test_rate_limit() {
  Fixture f;
  for (int i = 0; i < 20; ++i)
    f.rate->charge(0, exec::RateOp::Add, 0, NOW);
  CHECK(f.check(f.order(Side::Buy, 100 * PX, 1)) == RejectReason::RateLimit);
  auto later = f.order(Side::Buy, 100 * PX, 1);
  later.now_ns = NOW + exec::SEC_NS;
  CHECK(f.check(later) == RejectReason::None);
}

static void test_replace() {
  Fixture f;
  f.gate.set_limits({.max_order_qty = 40, .spot_only = true});
  f.rest(7, Side::Buy, 100 * PX, 10);
  exec::OrderRow &row = *f.orders->find(7);
  row.cum = 4;
  f.orders->set_leaves(row, 6);
  f.acct->reserve(0, Side::Buy, 100 * PX, 6); // the 4 filled released theirs
  auto rep = [&](int64_t px, int64_t qty, int64_t now) {
    return f.gate.check_replace(row, px, qty, 200 * PX, now, f.inst, *f.acct,
                                *f.orders, *f.rate, f.control);
  };
  CHECK(rep(101 * PX, 10, NOW) == RejectReason::None);
  CHECK(rep(101 * PX, 4, NOW) == RejectReason::BadOrder); // not above cum
  CHECK(rep(101 * PX, 41, NOW) == RejectReason::MaxOrderQty);
  // 10'000 USD balance, 600 reserved by this order's 6 leaves.
  CHECK(rep(100 * PX, 104, NOW) == RejectReason::MaxOrderQty);
  f.gate.set_limits({.spot_only = true});
  CHECK(rep(100 * PX, 104, NOW) == RejectReason::None); // 100 leaves: 9'400 more
  CHECK(rep(100 * PX, 105, NOW) == RejectReason::InsufficientQuote);
  // A young amend costs 4: fits 16 + 4 = 20, not 17 + 4.
  for (int i = 0; i < 17; ++i)
    f.rate->charge(0, exec::RateOp::Add, 0, NOW);
  CHECK(rep(101 * PX, 10, NOW) == RejectReason::RateLimit);
  CHECK(rep(101 * PX, 10, NOW + exec::SEC_NS) == RejectReason::None);
  f.control = ipc::KILL;
  CHECK(rep(101 * PX, 10, NOW + exec::SEC_NS) == RejectReason::Kill);
}

static void test_names() {
  CHECK(!std::strcmp(exec::reject_name(RejectReason::RateLimit), "rate_limit"));
  CHECK(!std::strcmp(exec::reject_name(RejectReason::RingFull), "ring_full"));
}

int main() {
  test_accepts_and_controls();
  test_instrument_checks();
  test_limits();
  test_band_and_spot();
  test_rate_limit();
  test_replace();
  test_names();
  return check_summary();
}
