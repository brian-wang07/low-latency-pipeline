// Strategy host: drains the normalized market-data feed, runs the active strategy
// against the account, order table and risk gate, and routes its orders (NullRouter
// in ITCH mode). Pinned, busy-polling hot loop.
//
//   exec_main <shm_fd> [--mode=null|live] [--strategy=<spec>] [--config=<file>]
//
// <spec> is builtin:<name> (a strategy compiled into this binary) or a path to a
// strategy_<name>.so. Both resolve to the same module table and the same
// run_strategy<Type, Router> code; exec_main_<name> just has <name> built in.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>

#include "common/config.hpp"
#include "common/config_file.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
#include "exec/loader.hpp"
#include "exec/runner.hpp"
#include "exec/stats_log.hpp"

#ifdef EXEC_STRATEGY_TYPE
#include EXEC_STRATEGY_HEADER
LL_BUILTIN_STRATEGY(exec::EXEC_STRATEGY_TYPE, EXEC_STRATEGY_NAME)
static constexpr const char *DEFAULT_STRATEGY = "builtin:" EXEC_STRATEGY_NAME;
#else
static constexpr const char *DEFAULT_STRATEGY = nullptr;
#endif

static volatile std::sig_atomic_t shutdown_flag{0};
static void on_signal(int) { shutdown_flag = 1; }

static void prefault(void *p, size_t len) noexcept {
  volatile char *c = static_cast<volatile char *>(p);
  for (size_t off = 0; off < len; off += 4096)
    c[off] = c[off];
}

// Exit status asking the manager to respawn exec (Control.SWAP).
static constexpr int EXIT_SWAP = 75;

struct Args {
  const char *mode = "null";
  const char *strategy = DEFAULT_STRATEGY;
  const char *config = nullptr;
};

static bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 2; i < argc; ++i) {
    const char *s = argv[i];
    if (!std::strncmp(s, "--mode=", 7))
      a.mode = s + 7;
    else if (!std::strncmp(s, "--strategy=", 11))
      a.strategy = s + 11;
    else if (!std::strncmp(s, "--config=", 9))
      a.config = s + 9;
    else {
      std::fprintf(stderr, "exec_main: unknown argument '%s'\n", s);
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  Args args;
  if (argc < 2 || !parse_args(argc, argv, args))
    return 2;
  if (std::strcmp(args.mode, "null") != 0) {
    std::fprintf(stderr, "exec_main: --mode=%s needs a venue gateway (plan Phase 4)\n",
                 args.mode);
    return 2;
  }
  if (!args.strategy) {
    std::fprintf(stderr, "exec_main: --strategy=<builtin:name|path.so> is required\n");
    return 2;
  }

  static common::Config cfg;
  if (args.config && !cfg.load(args.config))
    return 2;

  exec::LoadedModule mod;
  char err[256];
  if (!exec::load_strategy(args.strategy, mod, err, sizeof(err))) {
    std::fprintf(stderr, "exec_main: %s\n", err);
    return 2;
  }

  ShmSegment shm;
  ipc::PipelineShm *p = ipc::attach_pipeline(argv[1], shm, "exec_main");

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(int(cfg.get_i64("cores.exec", config::EXEC_CORE))))
    std::perror("pin_to_core exec");

  static exec::ExecContext x;
  if (!x.setup(p, cfg, &shutdown_flag))
    return shutdown_flag ? 0 : 2;
  std::strncpy(x.st.strategy_name, mod.m->name, sizeof(x.st.strategy_name) - 1);
  x.st.mode = stats::EXEC_NULL;
  std::fprintf(stderr, "[exec] generation=%llu strategy=%s (%s, build %s) mode=%s\n",
               (unsigned long long)x.st.exec_generation, mod.m->name,
               mod.handle ? args.strategy : "builtin", mod.m->build_id, args.mode);

  static exec::NullRouter router;
  // Static state is zero-filled BSS: fault every page in now, not on the first
  // orders (the router's report queue alone is 32 pages).
  prefault(&x, sizeof(x));
  prefault(&router, sizeof(router));
  void *strat = mod.m->create(&x, cfg.get_str("strategy.params", ""));
  if (!strat) {
    std::fprintf(stderr, "exec_main: strategy '%s' failed to start\n", mod.m->name);
    return 2;
  }
  const exec::RunExit why = mod.m->run_null(strat, &x, &router);

  // Leave nothing resting: cancel everything and wait for the reports.
  exec::StrategyContext<exec::NullRouter> ctx(x, router);
  ctx.cancel_all();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (x.orders.size() > 0 && std::chrono::steady_clock::now() < deadline)
    router.poll([&](const oe::ExecReport &rep) { x.apply_report(rep); });
  x.publish();
  exec::write_stats_log(x, cfg.get_str("exec.stats_log", "strategy_stats.log"),
                        mod.m->name);
  mod.m->destroy(strat);
  if (why == exec::RunExit::Swap) {
    p->control.flags.fetch_and(~uint32_t(ipc::SWAP), std::memory_order_acq_rel);
    return EXIT_SWAP;
  }
  return 0;
}
