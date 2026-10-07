#pragma once

#include "common/ipc/shm.hpp"
#include "common/platform/tsc.hpp"
#include "exec/exec_context.hpp"
#include "exec/router.hpp"
#include "exec/strategy.hpp"
#include <cstdint>

namespace exec {

enum class RunExit : uint8_t { Shutdown, Swap };

// Optional lifecycle hooks, called only if the strategy defines them.
template <class S, class Ctx> inline void call_on_start(S &strat, Ctx &ctx) noexcept {
  if constexpr (requires { strat.on_start(ctx); })
    strat.on_start(ctx);
}
template <class S, class Ctx> inline void call_on_stop(S &strat, Ctx &ctx) noexcept {
  if constexpr (requires { strat.on_stop(ctx); })
    strat.on_stop(ctx);
}

// Reports first: the order table and account must reflect every ack, fill and
// cancel before the strategy sees the next frame.
template <class S, class R>
inline void poll_reports(S &strat, ExecContext &x, R &router,
                         StrategyContext<R> &ctx) noexcept {
  router.poll([&](const oe::ExecReport &rep) {
    x.apply_report(rep);
    if constexpr (requires { strat.on_exec(ctx, rep); })
      strat.on_exec(ctx, rep);
  });
}

template <class S, class R>
inline void handle_frame(S &strat, ExecContext &x, R &router,
                         StrategyContext<R> &ctx,
                         const MarketUpdate<EXEC_DEPTH> &u) noexcept {
  const uint64_t t0 = read_tsc();
  x.st.feed_transit.record(t0 - u.tsc_in);
  x.view.on_update(u);
  x.frame_tsc_in = u.tsc_in;
  x.event_ns = int64_t(u.event_time);
  x.account.mark(u.instrument, u.best_bid, u.best_ask);
  router.on_frame(u);
  x.in_tick = true;
  const uint64_t t1 = read_tsc();
  strat.on_tick(ctx);
  x.st.tick.record(read_tsc() - t1);
  x.in_tick = false;
  ++x.st.frames;
}

// Hot loop: control flags, reports, then one frame. Every 1024 iterations it checks
// the TSC; each timer period it runs on_timer (if the strategy has one) and
// publishes stats, so the heartbeat stays fresh while the feed is idle.
// Templated on strategy and router so on_tick and the whole order path inline.
template <class S, class R>
  requires Strategy<S, StrategyContext<R>>
RunExit run_strategy(S &strat, ExecContext &x, R &router) noexcept {
  StrategyContext<R> ctx(x, router);
  MarketUpdate<EXEC_DEPTH> u;
  uint64_t next_timer = read_tsc() + x.timer_cycles;
  for (uint64_t iter = 1;; ++iter) {
    x.control_flags = x.shm->control.flags.load(std::memory_order_relaxed);
    if (*x.shutdown) {
      // Finish what the producer already published, so a run that ends because
      // the feed did (max_events) handles every frame, deterministically.
      while (x.feed->try_pop(u)) {
        poll_reports(strat, x, router, ctx);
        handle_frame(strat, x, router, ctx, u);
      }
      poll_reports(strat, x, router, ctx);
      return RunExit::Shutdown;
    }
    if (x.control_flags & ipc::SWAP)
      return RunExit::Swap;

    poll_reports(strat, x, router, ctx);
    if (x.feed->try_pop(u))
      handle_frame(strat, x, router, ctx, u);

    if ((iter & 1023) == 0) {
      const uint64_t now = read_tsc();
      if (now >= next_timer) {
        next_timer = now + x.timer_cycles;
        if constexpr (requires { strat.on_timer(ctx, int64_t{}); })
          strat.on_timer(ctx, x.now_ns());
        x.st.iterations = iter;
        x.publish();
      }
    }
  }
}

} // namespace exec
