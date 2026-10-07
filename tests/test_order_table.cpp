#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "check.hpp"
#include "exec/order_table.hpp"

using common::Side;
using exec::OrderRow;
using exec::OrderTable;

static OrderRow make_row(uint64_t id, uint16_t inst, Side side, int64_t leaves,
                         int64_t t) {
  OrderRow r{};
  r.id = id;
  r.instrument = inst;
  r.side = side;
  r.qty = r.leaves = leaves;
  r.t_submit_ns = t;
  return r;
}

// Every live row must be findable with the oracle's contents, and the aggregates
// must equal the oracle's sums.
static bool consistent(OrderTable &t,
                       const std::unordered_map<uint64_t, OrderRow> &oracle) {
  if (t.size() != oracle.size())
    return false;
  uint32_t live[4] = {};
  int64_t buy[4] = {}, sell[4] = {};
  for (const auto &[id, r] : oracle) {
    const OrderRow *f = t.find(id);
    if (!f || f->leaves != r.leaves || f->instrument != r.instrument)
      return false;
    ++live[r.instrument];
    (r.side == Side::Buy ? buy : sell)[r.instrument] += r.leaves;
  }
  for (uint16_t i = 0; i < 4; ++i)
    if (t.live_count(i) != live[i] || t.open_buy_lots(i) != buy[i] ||
        t.open_sell_lots(i) != sell[i])
      return false;
  return true;
}

static void test_ids() {
  auto t = std::make_unique<OrderTable>();
  CHECK(t->find(exec::INVALID_ORDER) == nullptr); // empty slots have key 0
  t->insert(make_row(1, 0, Side::Buy, 1, 0));
  CHECK(t->find(exec::INVALID_ORDER) == nullptr);
  t->set_generation(5);
  const uint64_t a = t->next_id(), b = t->next_id();
  CHECK(a == ((5ull << 40) | 1));
  CHECK(b == a + 1);
}

static void test_random_against_oracle() {
  auto t = std::make_unique<OrderTable>();
  t->set_generation(3);
  std::unordered_map<uint64_t, OrderRow> oracle;
  std::vector<uint64_t> ids;
  std::mt19937_64 rng(7);
  bool ok = true;
  for (int step = 0; step < 200'000; ++step) {
    const uint64_t op = rng() % 10;
    if (op < 5 && oracle.size() < OrderTable::MAX_ROWS) {
      // Mix sequential ids (realistic) with random ones (hash spread).
      const uint64_t id = (rng() & 1) ? t->next_id() : (rng() | 1);
      if (oracle.count(id))
        continue;
      const OrderRow r = make_row(id, uint16_t(rng() % 4),
                                  (rng() & 1) ? Side::Buy : Side::Sell,
                                  int64_t(1 + rng() % 100), step);
      ok &= t->insert(r) != nullptr;
      oracle[id] = r;
      ids.push_back(id);
    } else if (op < 8 && !ids.empty()) {
      const size_t k = rng() % ids.size();
      const uint64_t id = ids[k];
      ids[k] = ids.back();
      ids.pop_back();
      t->erase(id);
      oracle.erase(id);
      ok &= t->find(id) == nullptr;
    } else if (!ids.empty()) {
      const uint64_t id = ids[rng() % ids.size()];
      const int64_t leaves = int64_t(rng() % 50);
      t->set_leaves(*t->find(id), leaves);
      oracle[id].leaves = leaves;
    }
    if (step % 997 == 0)
      ok &= consistent(*t, oracle);
  }
  CHECK(ok);
  CHECK(consistent(*t, oracle));
}

// Keys forced into one probe cluster, then erased out of order.
static void test_collisions() {
  auto t = std::make_unique<OrderTable>();
  std::vector<uint64_t> ids;
  for (uint64_t id = 1; ids.size() < 40; ++id) // collect ids hashing to one home slot
    if (((id * 0x9e3779b97f4a7c15ULL) >> 52) == 77)
      ids.push_back(id);
  for (uint64_t id : ids)
    CHECK(t->insert(make_row(id, 0, Side::Buy, 1, 0)) != nullptr);
  for (size_t i = 0; i < ids.size(); i += 2)
    t->erase(ids[i]);
  bool ok = true;
  for (size_t i = 0; i < ids.size(); ++i)
    ok &= (t->find(ids[i]) != nullptr) == (i % 2 == 1);
  CHECK(ok);
  CHECK(t->size() == ids.size() / 2);
}

static void test_full_and_duplicates() {
  auto t = std::make_unique<OrderTable>();
  for (uint64_t i = 1; i <= OrderTable::MAX_ROWS; ++i)
    CHECK(t->insert(make_row(i, 0, Side::Sell, 1, 0)) != nullptr);
  CHECK(t->full());
  CHECK(t->insert(make_row(999'999, 0, Side::Sell, 1, 0)) == nullptr);
  t->erase(10);
  CHECK(t->insert(make_row(5, 0, Side::Sell, 1, 0)) == nullptr); // duplicate
  CHECK(t->insert(make_row(exec::INVALID_ORDER, 0, Side::Sell, 1, 0)) == nullptr);
  CHECK(t->insert(make_row(999'999, 0, Side::Sell, 1, 0)) != nullptr);
}

static void test_snapshot_oldest_first() {
  auto t = std::make_unique<OrderTable>();
  for (uint64_t i = 1; i <= 100; ++i)
    t->insert(make_row(i, 1, Side::Buy, 2, int64_t(1000 - i))); // later id, older
  stats::OpenOrders oo{};
  t->snapshot(oo);
  CHECK(oo.count == 64);
  CHECK(oo.rows[0].cl_ord_id == 100);
  bool sorted = true;
  for (uint32_t i = 1; i < oo.count; ++i)
    sorted &= oo.rows[i].cl_ord_id < oo.rows[i - 1].cl_ord_id;
  CHECK(sorted);
}

int main() {
  test_ids();
  test_random_against_oracle();
  test_collisions();
  test_full_and_duplicates();
  test_snapshot_oldest_first();
  return check_summary();
}
