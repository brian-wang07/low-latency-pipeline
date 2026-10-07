// Trading/execution runtime: drains the normalized market-data feed from core, fills
// the active strategy's shadow orders, and tracks inventory/PnL. Pinned, busy-polling
// hot loop. The strategy is selected per binary (see CMakeLists).
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/config.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
#include "common/platform/spin_pause.hpp"
#include "exec/fill_model.hpp"
#include "exec/runner.hpp"

// One exec binary per strategy: CMake defines EXEC_STRATEGY_TYPE / _HEADER per target
// (see CMakeLists). Default to the placeholder so the TU also builds standalone.
#ifndef EXEC_STRATEGY_TYPE
#define EXEC_STRATEGY_TYPE LoggingStrategy
#define EXEC_STRATEGY_HEADER "exec/strategies/logging_strategy.hpp"
#endif
#include EXEC_STRATEGY_HEADER

static volatile std::sig_atomic_t shutdown_flag{0};
static void on_signal(int) { shutdown_flag = 1; }

// Selected per binary at build time, so on_tick inlines into the hot loop.
using ActiveStrategy = exec::EXEC_STRATEGY_TYPE;

int main(int argc, char **argv) {
  if (argc != 2)
    std::abort();

  int shm_fd = std::stoi(argv[1]);

  ShmSegment shm;
  if (!shm.attach(shm_fd, ipc::SHM_SIZE))
    std::abort();
  auto *p = shm.as<ipc::PipelineShm>();
  while (p->header.magic.load(std::memory_order_acquire) == 0) {
    SPIN_PAUSE();
  }
  if (p->header.magic != ipc::MAGIC)
    std::abort();

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(config::EXEC_CORE))
    std::perror("pin_to_core exec");

  // Reaction+transport latency budget (event-time ns) for the fill model -- the sweep
  // knob for "what is a microsecond of queue position worth." Override via env.
  uint64_t latency_ns = 10'000;
  if (const char *e = std::getenv("EXEC_LATENCY_NS"))
    latency_ns = std::strtoull(e, nullptr, 10);

  exec::MarketView view;
  exec::FillModel fills(latency_ns);
  ActiveStrategy strat;
  exec::run_strategy(strat, p->core_to_exec, view, fills, shutdown_flag);
  return 0;
}
