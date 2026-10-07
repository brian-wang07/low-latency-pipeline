#pragma once

#include "common/ipc/shm.hpp"
#include "common/platform/tsc.hpp"
#include "common/seqlock.hpp"
#include "exec/market_view.hpp"
#include "exec/strategy.hpp"
#include <csignal>

namespace exec {

// Hot loop: drain the feed, update the view, then dispatch to the strategy.
// Templated on the concrete strategy so on_tick inlines (no vtable on the hot path).
// Feed counters go to shm every STATS_MASK + 1 frames and on exit.
template <Strategy S>
void run_strategy(S &strat, FeedRing &ring, MarketView &view,
                  common::Seqlock<stats::ExecStats> &stats_out,
                  const volatile std::sig_atomic_t &shutdown) noexcept {
  constexpr uint64_t STATS_MASK = (1ull << 16) - 1;
  StrategyContext ctx(view);
  MarketUpdate<EXEC_DEPTH> u;
  stats::ExecStats st{};
  auto publish = [&] {
    st.frame_gaps = view.frame_gaps();
    st.heartbeat_tsc = read_tsc();
    stats_out.store(st);
  };

  while (!shutdown) {
    if (ring.try_pop(u)) {
      view.on_update(u);
      strat.on_tick(ctx);
      if ((++st.frames & STATS_MASK) == 0)
        publish();
    }
  }
  while (ring.try_pop(u)) { // drain in-flight updates on shutdown
    view.on_update(u);
    strat.on_tick(ctx);
    ++st.frames;
  }
  publish();
}

} // namespace exec
