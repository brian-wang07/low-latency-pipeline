#include <cstdio>
#include <cstring>
#include <memory>

#include "check.hpp"
#include "exec/rate_model.hpp"

using exec::RateOp;
using exec::SEC_NS;

static void test_vectors() {
  const char *path = LL_FIXTURES "/kraken_rate_vectors.csv";
  std::FILE *f = std::fopen(path, "r");
  CHECK(f != nullptr);
  if (!f)
    return;
  char line[128];
  int rows = 0;
  while (std::fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n')
      continue;
    char op[16];
    long long age_ms;
    int cost;
    if (std::sscanf(line, "%15[^,],%lld,%d", op, &age_ms, &cost) != 3) {
      CHECK(!"malformed vector line");
      continue;
    }
    const RateOp o = !std::strcmp(op, "add")     ? RateOp::Add
                     : !std::strcmp(op, "amend") ? RateOp::Amend
                                                 : RateOp::Cancel;
    if (exec::kraken_rate_cost(o, age_ms * 1'000'000) != cost)
      std::fprintf(stderr, "vector %s,%lld expected %d\n", op, age_ms, cost);
    CHECK(exec::kraken_rate_cost(o, age_ms * 1'000'000) == cost);
    ++rows;
  }
  std::fclose(f);
  CHECK(rows == 23);
}

static void test_counter_and_decay() {
  auto m = std::make_unique<exec::RateModel>();
  m->configure(true, {.max = 20, .decay_per_s = 2, .max_open = 60, .margin = 2});
  const int64_t t0 = 1'000 * SEC_NS;
  for (int i = 0; i < 18; ++i) {
    CHECK(m->would_allow(0, RateOp::Add, 0, t0));
    m->charge(0, RateOp::Add, 0, t0);
  }
  CHECK(m->count(0, t0) == 18);
  CHECK(m->budget(0, t0) == 0);
  CHECK(!m->would_allow(0, RateOp::Add, 0, t0)); // 19 > 20 - 2
  CHECK(m->would_allow(1, RateOp::Add, 0, t0));  // per instrument
  // 0.5 s decays 1.0 at 2/s.
  CHECK(m->would_allow(0, RateOp::Add, 0, t0 + SEC_NS / 2));
  CHECK(m->count(0, t0 + 3 * SEC_NS) == 12);
  // A young cancel costs 8: fits only once enough has decayed.
  CHECK(!m->would_allow(0, RateOp::Cancel, 0, t0 + 3 * SEC_NS)); // 12 + 8 > 18
  CHECK(m->would_allow(0, RateOp::Cancel, 0, t0 + 4 * SEC_NS));  // 10 + 8 = 18
  // Decay floors at zero and the clock never runs backwards.
  CHECK(m->count(0, t0 + 100 * SEC_NS) == 0);
  m->charge(0, RateOp::Add, 0, t0 + 100 * SEC_NS);
  CHECK(m->count(0, t0) == 1);
}

static void test_disabled() {
  auto m = std::make_unique<exec::RateModel>();
  m->configure(false, {});
  for (int i = 0; i < 1000; ++i)
    m->charge(0, RateOp::Cancel, 0, 0);
  CHECK(m->would_allow(0, RateOp::Cancel, 0, 0));
  CHECK(m->budget(0, 0) > 1e17);
}

int main() {
  test_vectors();
  test_counter_and_decay();
  test_disabled();
  return check_summary();
}
