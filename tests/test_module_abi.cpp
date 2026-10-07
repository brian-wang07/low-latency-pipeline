// Loads the real strategy_<name>.so modules from build/ and runs a
// dummy -> logging -> dummy swap on one ExecContext, as the in-process hot-swap
// will (plan Phase 6). Run from build/ (ctest does).
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <thread>

#include "check.hpp"
#include "exec/loader.hpp"

static exec::MarketUpdate<exec::EXEC_DEPTH> frame(uint64_t seq) {
  exec::MarketUpdate<exec::EXEC_DEPTH> f{};
  f.event_seq = seq;
  f.event_time = seq * exec::SEC_NS;
  f.best_bid = 1'000'000 + int64_t(seq % 7) * 100; // touch moves every frame
  f.best_ask = f.best_bid + 100;
  f.nb = f.na = 1;
  f.bids[0] = {f.best_bid, 500, 3, 0};
  f.asks[0] = {f.best_ask, 500, 3, 0};
  f.flags = exec::TWO_SIDED;
  return f;
}

struct Engine {
  std::unique_ptr<ipc::PipelineShm> shm = std::make_unique<ipc::PipelineShm>();
  common::Config cfg;
  exec::ExecContext x;
  exec::NullRouter router;
  volatile std::sig_atomic_t stop = 0;
  uint64_t seq = 0;

  Engine() {
    ref::Instrument row{};
    std::strcpy(row.symbol, "NVDA");
    std::strcpy(row.venue, "ITCH");
    row.tick_mant = 1;
    row.tick_exp = -4;
    row.lot_mant = 1;
    shm->instruments.add(row);
    cfg.parse("risk.allowed_assets=NVDA\nrate.model=none\nexec.timer_ms=1\n"
              "null.balance.USD=1000000\nnull.balance.NVDA=100000\n");
    CHECK(x.setup(shm.get(), cfg, &stop));
  }

  // Runs the module on its own thread over n frames, then asks it to swap out.
  exec::RunExit run(const ll_strategy_module_v1 *m, void *s, int n) {
    exec::RunExit why = exec::RunExit::Shutdown;
    std::thread t([&] { why = m->run_null(s, &x, &router); });
    for (int i = 0; i < n; ++i)
      while (!shm->feed_to_exec.try_push(frame(++seq))) {
      }
    while (shm->feed_to_exec.size() != 0)
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    shm->control.flags.fetch_or(ipc::SWAP);
    t.join();
    shm->control.flags.fetch_and(~uint32_t(ipc::SWAP));
    return why;
  }
};

static void test_load_and_checks() {
  char err[256];
  exec::LoadedModule dummy, logging, tls, missing;
  CHECK(exec::load_strategy("./strategy_dummy.so", dummy, err, sizeof(err)));
  CHECK(exec::load_strategy("./strategy_logging.so", logging, err, sizeof(err)));
  CHECK(dummy.m && !std::strcmp(dummy.m->name, "dummy"));
  CHECK(logging.m && !std::strcmp(logging.m->name, "logging"));
  CHECK(dummy.m && dummy.m->layout_hash == exec::exec_abi_hash());
  CHECK(dummy.m && dummy.m->run_live == nullptr); // until GatewayRouter, Phase 4

  CHECK(!exec::load_strategy("./strategy_tls_fixture.so", tls, err, sizeof(err)));
  CHECK(std::strstr(err, "PT_TLS") != nullptr);
  CHECK(!exec::load_strategy("builtin:dummy", missing, err, sizeof(err)));
  CHECK(!exec::load_strategy("./no_such_strategy.so", missing, err, sizeof(err)));
}

static void test_swap_preserves_engine_state() {
  char err[256];
  exec::LoadedModule dmod, lmod;
  CHECK(exec::load_strategy("./strategy_dummy.so", dmod, err, sizeof(err)));
  CHECK(exec::load_strategy("./strategy_logging.so", lmod, err, sizeof(err)));
  if (!dmod.m || !lmod.m)
    return;
  static Engine e;

  void *d1 = dmod.m->create(&e.x, "");
  CHECK(e.run(dmod.m, d1, 50) == exec::RunExit::Swap);
  const uint64_t orders_new = e.x.st.orders_new;
  const uint64_t replaces = e.x.st.orders_replace;
  const uint32_t open = e.x.orders.size();
  const exec::Money quote_reserved = e.x.account.quote_balance(0).reserved;
  CHECK(orders_new == 2);  // one bid, one ask
  CHECK(replaces > 0);     // then amends to follow the moving touch
  CHECK(open == 2);
  CHECK(quote_reserved > 0);

  unsigned char state[4096];
  const size_t len = dmod.m->export_state(d1, state, sizeof(state));
  CHECK(len > 0);
  dmod.m->destroy(d1);

  void *l = lmod.m->create(&e.x, "");
  CHECK(!lmod.m->import_state(l, state, len)); // another strategy's state
  CHECK(e.run(lmod.m, l, 20) == exec::RunExit::Swap);
  CHECK(e.x.orders.size() == open); // engine state untouched by the swap
  CHECK(e.x.account.quote_balance(0).reserved == quote_reserved);
  lmod.m->destroy(l);

  // The second dummy resumes the first's quotes: it amends the same two orders.
  void *d2 = dmod.m->create(&e.x, "");
  CHECK(dmod.m->import_state(d2, state, len));
  CHECK(e.run(dmod.m, d2, 50) == exec::RunExit::Swap);
  CHECK(e.x.st.orders_new == orders_new);
  CHECK(e.x.st.orders_replace > replaces);
  CHECK(e.x.orders.size() == open);
  CHECK(e.x.st.acks == e.x.st.orders_new);
  dmod.m->destroy(d2);
}

int main() {
  test_load_and_checks();
  test_swap_preserves_engine_state();
  return check_summary();
}
