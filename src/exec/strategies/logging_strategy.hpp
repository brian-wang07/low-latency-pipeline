#pragma once

#include "exec/strategy.hpp"
#include <cstdint>
#include <cstdio>

namespace exec {

// Placeholder strategy: consumes the feed and periodically logs the book state to
// prove data flows end to end. Does not quote. Replace with a real Strategy.
class LoggingStrategy {
public:
  template <class Ctx> void on_tick(Ctx &ctx) noexcept {
    const MarketView &m = ctx.market();
    ++ticks_;
    if (m.last_was_trade())
      ++trades_;
    if (ticks_ == 1 || (ticks_ & HEARTBEAT_MASK) == 0)
      std::fprintf(stderr,
                   "[exec] seq=%llu ticks=%llu trades=%llu bid=%lld ask=%lld\n",
                   (unsigned long long)m.event_seq(),
                   (unsigned long long)ticks_, (unsigned long long)trades_,
                   (long long)m.best_bid(), (long long)m.best_ask());
  }

private:
  static constexpr uint64_t HEARTBEAT_MASK = (1ull << 12) - 1;

  uint64_t ticks_ = 0;
  uint64_t trades_ = 0;
};

} // namespace exec
