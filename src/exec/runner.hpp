#pragma once

#include "common/ipc/shm.hpp"
#include "exec/fill_model.hpp"
#include "exec/market_view.hpp"
#include "exec/strategy.hpp"
#include <csignal>

namespace exec {

// Hot loop: drain the core feed, update the view, apply fills to resting shadow
// orders, then dispatch to the strategy (which sees the fills and may quote).
// Templated on the concrete strategy so on_tick inlines (no vtable on the hot path).
template <Strategy S>
void run_strategy(S &strat, ExecRing &ring, MarketView &view, FillModel &fills,
                  const volatile std::sig_atomic_t &shutdown) noexcept {
  StrategyContext ctx(view, fills);
  MarketUpdate<EXEC_DEPTH> u;
  while (!shutdown) {
    if (ring.try_pop(u)) {
      view.on_update(u);
      fills.on_update(u);
      strat.on_tick(ctx);
    }
  }
  while (ring.try_pop(u)) { // drain in-flight updates on shutdown
    view.on_update(u);
    fills.on_update(u);
    strat.on_tick(ctx);
  }
}

} // namespace exec
