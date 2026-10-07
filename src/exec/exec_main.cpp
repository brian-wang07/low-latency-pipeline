// Strategy host: drains the normalized market-data feed and runs the active strategy.
// Pinned, busy-polling hot loop. The strategy is selected per binary (see CMakeLists).
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "common/config.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
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

  ShmSegment shm;
  ipc::PipelineShm *p = ipc::attach_pipeline(argv[1], shm, "exec_main");

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(config::EXEC_CORE))
    std::perror("pin_to_core exec");

  exec::MarketView view;
  ActiveStrategy strat;
  exec::run_strategy(strat, p->feed_to_exec, view, p->exec_stats,
                     shutdown_flag);
  return 0;
}
